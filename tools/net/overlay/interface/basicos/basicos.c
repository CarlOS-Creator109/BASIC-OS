/*
 * iPXE platform glue for BASIC-OS (PC BASIC V1.2).  GPL-2.0-or-later.
 *
 * NET.APP (a tiny /SYS module) loads this binary at 0x700000 and calls mod_main(&kapi).
 * Provides: module header and entry point, console (kapi print/poll), PCI BAR mapping
 * (kernel map_mmio), and the user-memory allocator (RAM pool above 64 MB, identity-mapped).
 */

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ipxe/init.h>
#include <ipxe/console.h>
#include <ipxe/iomap.h>
#include <ipxe/umalloc.h>
#include <ipxe/timer.h>
#include <ipxe/process.h>
#include <ipxe/netdevice.h>
#include <ipxe/keys.h>
#include <ipxe/entropy.h>
#include <ipxe/command.h>
#include <ipxe/image.h>
#include <ipxe/io.h>
#include <rtc.h>

/* services handed over by NET.APP (sys/net.c fills this from the real kapi) */
struct bos_boot {
	int ( * poll ) ( void );			/* pending key or 0 */
	void ( * print ) ( const char *s );
	void * ( * map_mmio ) ( unsigned long long phys, unsigned size );
	unsigned ( * ms ) ( void );
	int ( * read ) ( const char *spec, char *buf, int max, unsigned *total );
	int ( * write ) ( const char *spec, const void *buf, unsigned n );
	const char *args;				/* command line from BASIC, or "" */
};
static const struct bos_boot *X;

extern int system ( const char *command );

/* ---- module header ('PCBM', end of bss, entry, load address) ---- */
__asm__ ( ".section .header,\"a\"\n"
	  ".long 0x4D424350\n"
	  ".long __mod_end\n"
	  ".long mod_main\n"
	  ".long 0x700000\n"
	  ".text\n" );

/* entry symbol the iPXE linker rules expect for this media type ("bos") */
__asm__ ( ".globl _bos_start\n.set _bos_start, mod_main\n" );

/* ---- timer: BASIC-OS ya calibra el TSC; no exigimos TSC invariante ---- */

static unsigned long long tsc_per_us;

static inline unsigned long long bos_rdtsc ( void ) {
	unsigned lo, hi;
	__asm__ __volatile__ ( "rdtsc" : "=a" ( lo ), "=d" ( hi ) );
	return ( ( (unsigned long long) hi << 32 ) | lo );
}

static unsigned long bos_currticks ( void ) {
	return ( ( (unsigned long) X->ms() * TICKS_PER_SEC ) / 1000 );
}

static void bos_udelay ( unsigned long usecs ) {
	unsigned long long start = bos_rdtsc();
	unsigned long long n = ( (unsigned long long) usecs * tsc_per_us );
	while ( ( bos_rdtsc() - start ) < n )
		__asm__ __volatile__ ( "pause" );
}

static int bos_timer_probe ( void ) {
	unsigned m0, m;
	unsigned long long t0;
	m0 = X->ms();
	while ( ( m = X->ms() ) == m0 )
		;
	m0 = m;
	t0 = bos_rdtsc();
	while ( ( X->ms() - m0 ) < 20 )
		;
	tsc_per_us = ( bos_rdtsc() - t0 ) / 20000;
	if ( ! tsc_per_us )
		tsc_per_us = 1000;
	return 0;
}

struct timer bos_timer __timer ( TIMER_PREFERRED ) = {
	.name = "basicos",
	.probe = bos_timer_probe,
	.currticks = bos_currticks,
	.udelay = bos_udelay,
};

/* ---- console ---- */

static int pending = -1;

static int bos_poll ( void ) {
	int k;
	if ( pending >= 0 )
		return pending;
	k = X->poll();
	if ( ! k )
		return -1;
	/* BASIC-OS entrega MAYUSCULAS por defecto: aqui se invierte (sin Shift = minusculas) */
	if ( k >= 'a' && k <= 'z' )
		k -= 32;
	else if ( k >= 'A' && k <= 'Z' )
		k += 32;
	if ( k == 27 )		/* ESC aborts like Ctrl-C */
		k = CTRL_C;
	pending = k;
	return pending;
}

static void bos_putchar ( int c ) {
	char s[2];
	if ( c == '\r' )
		return;
	s[0] = ( char ) c;
	s[1] = 0;
	X->print ( s );
}

static int bos_getchar ( void ) {
	int k;
	while ( ( k = bos_poll() ) < 0 )
		step();
	pending = -1;
	return k;
}

static int bos_iskey ( void ) {
	return bos_poll() >= 0;
}

struct console_driver bos_console __console_driver = {
	.putchar = bos_putchar,
	.getchar = bos_getchar,
	.iskey = bos_iskey,
	.usage = CONSOLE_USAGE_ALL,
};

/* ---- PCI BAR mapping ---- */

static void * bos_ioremap ( unsigned long bus_addr, size_t len ) {
	void *p = X->map_mmio ( bus_addr, ( unsigned ) len );
	return p;
}

static void bos_iounmap ( volatile const void *io_addr __unused ) {
}

PROVIDE_IOMAP_INLINE ( pages, io_to_bus );
PROVIDE_IOMAP ( pages, ioremap, bos_ioremap );
PROVIDE_IOMAP ( pages, iounmap, bos_iounmap );

/* ---- user memory: address-ordered blocks in [UPOOL_BASE, top of RAM) ---- */

#define UPOOL_BASE 0x4000000UL
#define UPOOL_TOP_MAX 0x3C000000UL
#define UALIGN 64UL

struct ublock {
	size_t size;		/* block size including this header */
	size_t used;		/* 1 = allocated */
};
#define UHDR ( ( size_t ) 64 )	/* keeps user data 64-byte aligned */

static int upool_ready;
static unsigned long upool_top;

/* top of usable RAM from the E820 table left by stage2 (count at 0x6020, 24-byte entries) */
static unsigned long find_ram_top ( void ) {
	const uint8_t *t = ( const uint8_t * ) 0x6020;
	unsigned int n = *( const uint32_t * ) t;
	const uint8_t *e = t + 4;
	unsigned long top = 0;
	unsigned int i;
	if ( n == 0 || n > 64 )
		return 0x20000000UL;
	for ( i = 0 ; i < n ; i++, e += 24 ) {
		uint64_t base = *( const uint64_t * ) e;
		uint64_t len = *( const uint64_t * ) ( e + 8 );
		uint32_t type = *( const uint32_t * ) ( e + 16 );
		if ( type == 1 && base <= UPOOL_BASE && base + len > UPOOL_BASE ) {
			top = ( unsigned long ) ( base + len );
			break;
		}
	}
	if ( top > UPOOL_TOP_MAX )
		top = UPOOL_TOP_MAX;
	return top;
}

static void upool_init ( void ) {
	struct ublock *b = ( struct ublock * ) UPOOL_BASE;
	upool_top = find_ram_top();
	if ( upool_top < UPOOL_BASE + 0x100000 )
		upool_top = UPOOL_BASE + 0x100000;
	b->size = upool_top - UPOOL_BASE;
	b->used = 0;
	upool_ready = 1;
}

static struct ublock * unext ( struct ublock *b ) {
	unsigned long n = ( unsigned long ) b + b->size;
	return ( n >= upool_top ) ? NULL : ( struct ublock * ) n;
}

static void * bos_urealloc ( void *ptr, size_t new_size ) {
	struct ublock *b, *n;
	size_t need;
	if ( ! upool_ready )
		upool_init();
	if ( ptr == NULL ) {
		if ( ! new_size )
			return NULL;
		need = ( ( new_size + UHDR + UALIGN - 1 ) & ~( UALIGN - 1 ) );
		for ( b = ( struct ublock * ) UPOOL_BASE ; b ; b = unext ( b ) ) {
			if ( b->used || b->size < need )
				continue;
			if ( b->size >= need + 2 * UHDR ) {
				n = ( struct ublock * ) ( ( unsigned long ) b + need );
				n->size = b->size - need;
				n->used = 0;
				b->size = need;
			}
			b->used = 1;
			return ( void * ) ( ( unsigned long ) b + UHDR );
		}
		return NULL;
	}
	b = ( struct ublock * ) ( ( unsigned long ) ptr - UHDR );
	if ( ! new_size ) {		/* free, then merge with the free neighbours */
		struct ublock *p;
		b->used = 0;
		for ( p = ( struct ublock * ) UPOOL_BASE ; p ; p = unext ( p ) ) {
			while ( ! p->used && ( n = unext ( p ) ) && ! n->used )
				p->size += n->size;
		}
		return NULL;
	}
	need = ( ( new_size + UHDR + UALIGN - 1 ) & ~( UALIGN - 1 ) );
	if ( need <= b->size )
		return ptr;
	n = unext ( b );	/* grow in place when the next block is free and big enough */
	if ( n && ! n->used && b->size + n->size >= need ) {
		size_t total = b->size + n->size;
		if ( total >= need + 2 * UHDR ) {
			struct ublock *r = ( struct ublock * ) ( ( unsigned long ) b + need );
			r->size = total - need;
			r->used = 0;
			b->size = need;
		} else {
			b->size = total;
		}
		return ptr;
	}
	{
		size_t old = b->size - UHDR;
		void *np = bos_urealloc ( NULL, new_size );
		if ( ! np )
			return NULL;
		memcpy ( np, ptr, old < new_size ? old : new_size );
		bos_urealloc ( ptr, 0 );
		return np;
	}
}

PROVIDE_UMALLOC ( basicos, urealloc, bos_urealloc );

/* ---- the command line: a small line editor (backspace + enter) feeding iPXE's system() ---- */

static int bos_readline ( const char *prompt, char *buf, int max ) {
	int len = 0, c;
	printf ( "%s", prompt );
	for ( ;; ) {
		c = bos_getchar();
		if ( c == '\r' || c == '\n' ) {
			printf ( "\n" );
			buf[len] = 0;
			return len;
		}
		if ( c == CTRL_C ) {
			printf ( "\n" );
			buf[0] = 0;
			return 0;
		}
		if ( c == 8 || c == 127 ) {	/* backspace */
			if ( len ) {
				len--;
				bos_putchar ( 8 );
				bos_putchar ( ' ' );
				bos_putchar ( 8 );
			}
			continue;
		}
		if ( c >= 32 && c < 127 && len < max - 1 ) {
			buf[len++] = ( char ) c;
			bos_putchar ( c );
		}
	}
}

/** Entry point called by NET.APP: runs the iPXE shell until "exit". */
int mod_main ( const struct bos_boot *boot ) {
	char line[160];
	X = boot;
	initialise();
	startup();
	if ( X->args && X->args[0] ) {
		/* NET "comando; otro comando": se ejecutan en orden y se para en el primero que falle */
		char cmd[160];
		const char *p = X->args;
		while ( *p ) {
			size_t n = 0;
			while ( *p == ' ' || *p == ';' )
				p++;
			while ( p[n] && p[n] != ';' && n < sizeof ( cmd ) - 1 )
				n++;
			if ( ! n )
				break;
			memcpy ( cmd, p, n );
			cmd[n] = 0;
			p += n;
			if ( system ( cmd ) != 0 )
				break;
		}
	} else {
		printf ( "iPXE en BASIC-OS. Escribe comandos (help, dhcp, ping, imgfetch...). \"exit\" vuelve a BASIC.\n" );
		for ( ;; ) {
			if ( ! bos_readline ( "ipxe> ", line, sizeof ( line ) ) )
				continue;
			if ( ! strcmp ( line, "exit" ) || ! strcmp ( line, "quit" ) )
				break;
			system ( line );
		}
	}
	shutdown_exit();
	return 0;
}


/* ======================================================================== */
/* Reloj de tiempo real (CMOS): sin fecha correcta TLS rechaza todos los      */
/* certificados ("todavia no valido"). Es el rtc_time.c de iPXE (pcbios) sin  */
/* nada de BIOS: solo lee los puertos 0x70/0x71.                              */
/* ======================================================================== */
#include <time.h>
#include <ipxe/time.h>

static unsigned int bos_rtc_readb ( int address ) {
	outb ( address, CMOS_ADDRESS );
	return inb ( CMOS_DATA );
}

static unsigned int bos_rtc_bcd ( int address ) {
	unsigned int bcd = bos_rtc_readb ( address );
	return ( bcd - ( 6 * ( bcd >> 4 ) ) );
}

static time_t bos_rtc_read ( void ) {
	unsigned int status_b, hour;
	int is_binary, is_24hour, is_pm, n = 0;
	unsigned int ( * rd ) ( int address );
	struct tm tm;

	/* esperar a que acabe una actualizacion en curso (con tope, por si el RTC no existe) */
	while ( ( bos_rtc_readb ( RTC_STATUS_A ) & RTC_STATUS_A_UPDATE_IN_PROGRESS ) && ++n < 100000 ) {}
	status_b = bos_rtc_readb ( RTC_STATUS_B );
	is_binary = ( status_b & RTC_STATUS_B_BINARY );
	is_24hour = ( status_b & RTC_STATUS_B_24_HOUR );
	rd = ( is_binary ? bos_rtc_readb : bos_rtc_bcd );
	tm.tm_sec = rd ( RTC_SEC );
	tm.tm_min = rd ( RTC_MIN );
	hour = rd ( RTC_HOUR );
	if ( ! is_24hour ) {
		is_pm = ( hour >= 80 );
		hour = ( ( ( ( hour & 0x7f ) % 80 ) % 12 ) + ( is_pm ? 12 : 0 ) );
	}
	tm.tm_hour = hour;
	tm.tm_mday = rd ( RTC_MDAY );
	tm.tm_mon = ( rd ( RTC_MON ) - 1 );
	tm.tm_year = ( rd ( RTC_YEAR ) + 100 );		/* siglo XXI */
	return mktime ( &tm );
}

static time_t bos_rtc_now ( void ) {
	time_t t = 0, last;
	int n = 0;
	do {			/* dos lecturas iguales seguidas (por si cae en mitad de una actualizacion) */
		last = t;
		t = bos_rtc_read();
	} while ( t != last && ++n < 8 );
	return t;
}
PROVIDE_TIME ( rtc, time_now, bos_rtc_now );

/* ======================================================================== */
/* Entropia: sin RDRAND (Turion II, QEMU qemu64...) TLS no arranca. Fuente de */
/* respaldo: fase del contador de ciclos (TSC) en cada tic periodico del RTC  */
/* (1024 Hz). El cristal del RTC y el reloj de la CPU son independientes, asi */
/* que el instante exacto de cada tic es ruido; se sondea el registro C (el   */
/* kernel va sin interrupciones). Se declara 1 bit por muestra (conservador): */
/* iPXE pasa las pruebas de repeticion/proporcion y mezcla con SHA-256.      */
/* ======================================================================== */
struct entropy_source bos_entropy __entropy_source ( ENTROPY_FALLBACK );

static uint8_t bos_saved_a, bos_saved_b;

static int bos_entropy_enable ( void ) {
	/* periodo de 1024 Hz y PIE puesto (la linea IRQ8 sigue enmascarada: el kernel no usa interrupciones) */
	outb ( ( RTC_STATUS_A | CMOS_DISABLE_NMI ), CMOS_ADDRESS );
	bos_saved_a = inb ( CMOS_DATA );
	outb ( ( RTC_STATUS_A | CMOS_DISABLE_NMI ), CMOS_ADDRESS );
	outb ( ( bos_saved_a & 0xF0 ) | 0x06, CMOS_DATA );
	outb ( ( RTC_STATUS_B | CMOS_DISABLE_NMI ), CMOS_ADDRESS );
	bos_saved_b = inb ( CMOS_DATA );
	outb ( ( RTC_STATUS_B | CMOS_DISABLE_NMI ), CMOS_ADDRESS );
	outb ( bos_saved_b | RTC_STATUS_B_PIE, CMOS_DATA );
	outb ( CMOS_DEFAULT_ADDRESS, CMOS_ADDRESS );
	( void ) inb ( CMOS_DATA );
	entropy_init ( &bos_entropy, MIN_ENTROPY ( 1.0 ) );
	return 0;
}

static void bos_entropy_disable ( void ) {
	outb ( ( RTC_STATUS_B | CMOS_DISABLE_NMI ), CMOS_ADDRESS );
	outb ( bos_saved_b, CMOS_DATA );
	outb ( ( RTC_STATUS_A | CMOS_DISABLE_NMI ), CMOS_ADDRESS );
	outb ( bos_saved_a, CMOS_DATA );
	outb ( CMOS_DEFAULT_ADDRESS, CMOS_ADDRESS );
	( void ) inb ( CMOS_DATA );
}

static int bos_entropy_noise ( noise_sample_t *noise ) {
	unsigned int t0 = X->ms();
	uint32_t lo, hi;
	for ( ;; ) {
		outb ( RTC_STATUS_C, CMOS_ADDRESS );	/* leer C: limpia y dice si hubo tic */
		if ( inb ( CMOS_DATA ) & 0x40 )
			break;
		if ( ( unsigned int ) ( X->ms() - t0 ) > 100 )
			return -ETIMEDOUT;
	}
	__asm__ __volatile__ ( "rdtsc" : "=a" ( lo ), "=d" ( hi ) );
	*noise = ( noise_sample_t ) ( lo & 0xff );
	return 0;
}

struct entropy_source bos_entropy __entropy_source ( ENTROPY_FALLBACK ) = {
	.name = "bos-rtc-tsc",
	.enable = bos_entropy_enable,
	.disable = bos_entropy_disable,
	.get_noise = bos_entropy_noise,
};

/* ======================================================================== */
/* bosstash <imagen> app|list : copia una imagen bajada con imgfetch a la     */
/* memoria fija que lee PKG.APP (el SO aun no escribe en disco). Programas   */
/* PKG: base 0x9000000 (hasta 32 MB); lista de programas: 0xB000000 (1 MB);  */
/* buzon con los tamanos: 0xB100000.                                         */
/* ======================================================================== */
#define BOS_STASH_APP	0x9000000UL
#define BOS_STASH_APPMAX 0x2000000UL
#define BOS_STASH_LIST	0xB000000UL
#define BOS_STASH_LISTMAX 0x100000UL
#define BOS_STASH_MBOX	0xB100000UL

struct bos_mbox { uint32_t magic, app_len, list_len, spare; };

static int bosstash_exec ( int argc, char **argv ) {
	struct image *image;
	struct bos_mbox *mb = ( struct bos_mbox * ) BOS_STASH_MBOX;
	if ( argc != 3 ) {
		printf ( "uso: bosstash <imagen> app|list\n" );
		return -EINVAL;
	}
	if ( find_ram_top() < BOS_STASH_MBOX + 0x1000UL ) {
		printf ( "bosstash: hace falta mas RAM (~180 MB; en QEMU usa -m 256)\n" );
		return -ENOMEM;
	}
	image = find_image ( argv[1] );
	if ( ! image ) {
		printf ( "bosstash: no hay imagen \"%s\"\n", argv[1] );
		return -ENOENT;
	}
	if ( mb->magic != 0x48545342u ) {		/* 'BSTH' */
		mb->magic = 0x48545342u;
		mb->app_len = mb->list_len = 0;
	}
	if ( ! strcmp ( argv[2], "app" ) ) {
		if ( image->len > BOS_STASH_APPMAX ) {
			printf ( "bosstash: programa demasiado grande (%zd bytes)\n", image->len );
			return -ENOSPC;
		}
		memcpy ( ( void * ) BOS_STASH_APP, image->data, image->len );
		mb->app_len = image->len;
	} else if ( ! strcmp ( argv[2], "list" ) ) {
		if ( image->len > BOS_STASH_LISTMAX ) {
			printf ( "bosstash: lista demasiado grande\n" );
			return -ENOSPC;
		}
		memcpy ( ( void * ) BOS_STASH_LIST, image->data, image->len );
		mb->list_len = image->len;
	} else {
		printf ( "uso: bosstash <imagen> app|list\n" );
		return -EINVAL;
	}
	return 0;
}
COMMAND ( bosstash, bosstash_exec );
