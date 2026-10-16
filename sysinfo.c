/* sysinfo.c - datos reales de la maquina para el banner: CPU, MHz, RAM y video */
#include "kernel.h"
extern u64 g_tsc_hz;

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void putu(u64 v)
{
    char b[21];
    int i = 0;
    if (!v)
        b[i++] = '0';
    while (v) {
        b[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i)
        vga_putc(b[--i]);
}

/* Nombre de la CPU en mayusculas, sin espacios repetidos */
static void cpu_name(char *out)
{
    u32 a, b, c, d;
    u32 raw[13];
    char *tmp = (char *)raw;
    const char *s;
    int n = 0, sp = 0;

    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        int i;
        for (i = 0; i < 3; i++)
            cpuid(0x80000002 + (u32)i, &raw[i * 4], &raw[i * 4 + 1], &raw[i * 4 + 2], &raw[i * 4 + 3]);
        tmp[48] = 0;
    } else {
        cpuid(0, &a, &raw[0], &raw[2], &raw[1]);        /* vendor: EBX, EDX, ECX */
        tmp[12] = 0;
    }

    s = tmp;
    while (*s == ' ')
        s++;
    for (; *s; s++) {
        char ch = *s;
        if (ch == ' ') {
            if (sp)
                continue;
            sp = 1;
        } else {
            sp = 0;
        }
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
        out[n++] = ch;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = 0;
}

/* Frecuencia del TSC en Hz, midiendo 10 ms con el canal 2 del PIT (0 si falla) */
static u64 tsc_hz(void)
{
    u8 p61 = inb(0x61);
    u64 t0, t1;
    unsigned guard = 0;

    outb(0x61, (u8)((p61 & ~0x02) | 0x01));     /* compuerta del canal 2 encendida, bocina apagada */
    outb(0x43, 0xB0);                           /* canal 2, lobyte/hibyte, modo 0 */
    outb(0x42, 0x9C);                           /* cuenta 11932 = ~10 ms */
    outb(0x42, 0x2E);
    t0 = rdtsc();
    while (!(inb(0x61) & 0x20) && ++guard < 20000000u)
        ;
    t1 = rdtsc();
    outb(0x61, p61);
    if (guard >= 20000000u)
        return 0;
    return (t1 - t0) * 100;
}

static u64 ram_bytes(void)
{
    const struct bootinfo *bi = BOOTINFO;
    u64 tot = 0;
    int i;
    if (bi->e820n) {
        for (i = 0; i < bi->e820n; i++)
            if (E820_TABLE[i].type == 1)
                tot += E820_TABLE[i].len;
        return tot;
    }
    return ((u64)bi->extkb + 1024) * 1024;      /* BIOS antigua: AH=88h */
}

void sysinfo_print(void)
{
    char name[49];
    u64 hz = tsc_hz();

    g_tsc_hz = hz;
    if (hz)
        kbd_set_blink(hz / 2);                  /* el cursor parpadea cada 0.5 s */

    cpu_name(name);
    vga_puts("CPU:   ");
    vga_puts(name);
    if (hz) {
        vga_puts(" @ ");
        putu((hz + 500000) / 1000000);
        vga_puts(" MHZ");
    }
    vga_putc('\n');

    vga_puts("RAM:   ");
    putu((ram_bytes() + (1u << 19)) >> 20);
    vga_puts(" MB\n");

    vga_puts("VIDEO: ");
    if (vga_is_gfx()) {
        putu((u64)vga_width());
        vga_putc('X');
        putu((u64)vga_height());
        vga_puts(", ");
        putu((u64)vga_bpp());
        vga_puts("-BIT COLOR\n");
    } else {
        vga_puts("VGA TEXT 80X25\n");
    }
    vga_putc('\n');
}
