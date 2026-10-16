/* kernel.h - tipos, E/S de puertos y prototipos del kernel */
#ifndef KERNEL_H
#define KERNEL_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed int     i32;
typedef short          s16;
typedef unsigned long  size_t;
typedef unsigned long  u64;

#define NULL ((void *)0)

static inline void outb(u16 port, u8 v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 r;
    __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

/* bootinfo: lo deja stage2.asm en 0x6000 */
struct bootinfo {
    u32 magic;                          /* 'PCBI' */
    u16 w, h, pitch;                    /* modo grafico VBE */
    u8  bpp, gfx;                       /* gfx = 1 si hay framebuffer */
    u32 fb;                             /* direccion fisica del framebuffer */
    u8  rs, rp, gs, gp, bs, bp;         /* tamano/posicion de los canales R, G, B */
    u16 e820n;                          /* entradas del mapa de memoria */
    u16 extkb;                          /* KB sobre 1 MB segun INT 15h/88h */
} __attribute__((packed));

struct e820 {
    u64 base, len;
    u32 type, attr;                     /* type 1 = RAM utilizable */
} __attribute__((packed));

#define BOOTINFO   ((const struct bootinfo *)0x6000)
#define E820_TABLE ((const struct e820 *)0x6020)

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

/* lib.c */
void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

/* vga.c */
void vga_init(void);
void vga_clear(void);
void vga_splash(int pct, const char *msg);
void vga_splash_end(void);
int vga_vid(int op, const unsigned char *chars, int w, int h);
void vga_putc(char c);
void vga_puts(const char *s);
void vga_setcolor(int fg, int bg);
void vga_set_fg(int c);
void vga_set_bg(int c);
void vga_set_border(int c);
int  vga_get_fg(void);
int  vga_get_bg(void);
int  vga_get_border(void);
int  vga_col(void);
int  vga_cols(void);
int  vga_rows(void);
int  vga_is_gfx(void);
int  vga_width(void);
int  vga_height(void);
int  vga_bpp(void);
void vga_cursor_show(int on);
void vga_mouse(int dx, int dy, int buttons);
int  vga_mouse_get(int what);          /* 0 = x, 1 = y (px logicos), 2 = botones */
int  vga_main_w(void);                 /* tamano de la consola del kernel (no el de la ventana actual) */
int  vga_main_h(void);
int  vga_gfx_w(void);
int  vga_gfx_h(void);
void vga_plot(int x, int y, int c);
void vga_hline(int x1, int x2, int y, int c);
int  vga_gui(int on);                   /* 0 = bien; <0 = sin memoria para las capas */
int  vga_gui_active(void);
void vga_locate(int col, int row);

/* geometria del framebuffer real, para el compositor (fb.c) */
struct vgeom {
    volatile u8 *fb;
    u32 pitch, bpp;
    int x0, y0, scale, w, h;            /* w x h = area logica (la de PLOT) */
    u32 pal[16];
};
void vga_geom(struct vgeom *g);

/* consolas adicionales (ventanas de terminal): cada una con su cuadricula de texto y su plano de PLOT */
typedef void (*con_dirty_fn)(void *user, int x, int y, int w, int h);
int  vga_con_attach(int id, u8 *txt, u8 *gfx, int stride, int cols, int rows, con_dirty_fn cb, void *user);
void vga_con_detach(int id);
void vga_con_select(int id);
void vga_con_resize(int id, int cols, int rows);
void vga_con_putc(int id, int ch);
void vga_con_cursor(int id, int on);

/* fb.c: capas y compositor */
int   fb_open(void);
void  fb_close(void);
u8   *fb_layer(int n);
void *fb_pool(unsigned int *bytes);
void  fb_dirty(int x, int y, int w, int h);
void  fb_flush(void);
void  fb_present(int x, int y, int w, int h);
void  fb_cursor_show(int show);
void  fb_cursor_move(int x, int y);

/* bios.c: INT 13h desde modo largo */
struct bregs { u16 ax, bx, cx, dx, si; };
#define BOUNCE_BUF 0x80000              /* buffer de rebote bajo 1 MB (64 KB) */
#define BOUNCE_SEG 0x8000
int bios_int13(struct bregs *r);

/* blk.c: dispositivos de bloques (discos y disqueteras de la BIOS, lectoras USB) */
struct blk;
typedef int (*blk_read_fn)(struct blk *b, u64 lba, int n, void *buf);
/* Comando SCSI/MMC (CDB de 6 a 12 bytes) con datos de entrada. 0 = bien */
typedef int (*blk_scsi_fn)(struct blk *b, const u8 *cdb, int cdblen, void *data, int dlen);
struct blk {
    char name[8];                       /* FD0, HD0, CD0... */
    int  bios;                          /* numero de unidad de la BIOS (o -1) */
    int  boot;                          /* 1 = unidad de la que arrancamos */
    int  edd;                           /* 1 = INT 13h extendido */
    u32  bps;                           /* bytes por sector (512, 2048) */
    u64  sectors;
    u32  cyls, heads, spt;              /* geometria CHS (disquetes / sin EDD) */
    blk_read_fn rd;                     /* lectura propia (USB/ATAPI); NULL = BIOS */
    blk_scsi_fn scsi;                   /* comandos SCSI (lectoras de CD); NULL = no */
    void *priv;
};
int  blk_count(void);
struct blk *blk_get(int i);
int  blk_find(const char *name, int len);
int  blk_read(struct blk *b, u64 lba, int n, void *buf);   /* 0 = bien */
int  blk_write(struct blk *b, u64 lba, int n, const void *buf);
int  blk_api(const char *name, int op, u64 lba, unsigned n, void *buf);
void blk_add(struct blk *b);            /* alta de un dispositivo (USB) */
int  blk_line(int i, char *out);

/* mouse.c */
void mouse_init(void);
void mouse_byte(u8 b);
void mouse_raw_mode(int on);
int  mouse_raw_get(int *dx, int *dy);

/* kbd.c */
void kbd_poll(void);
int  kbd_getc(void);
int  kbd_esc(void);
void kbd_esc_again(void);
int  kbd_inkey(void);
int  kbd_rawin(int op, int *a, int *b);     /* entrada cruda (ver kapi.h) */
void kbd_inject(u8 c);
int  kbd_readline(char *buf, int max);
void kbd_set_blink(u64 cycles);

/* pci.c */
u32 pci_read32(int bus, int dev, int fn, int off);
void pci_write32(int bus, int dev, int fn, int off, u32 v);
int pci_find(int class, int sub, int prog, int nth, int *bus, int *dev, int *fn);
void *map_mmio(u64 phys, u32 size);   /* mmio.c */
void delay_ms(int ms);                /* kernel.c */
void plat_sleep(int ms);
void usb_probe(void);
void atapi_probe(void);               /* atapi.c: lectoras ATAPI IDE */                 /* usb.c: da de alta las unidades USB con blk_add() */
int pci_line(int i, char *out);

/* sysinfo.c */
void sysinfo_print(void);

#endif
