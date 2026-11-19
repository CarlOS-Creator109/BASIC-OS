/* ram.c - bancos de RAM ("salto de RAM"): la memoria alta se reparte en bancos de 32 MB
 *
 * Desde 0x9000000 hasta el final de la RAM, en pedazos de 32 MB. Un programa pide N bancos seguidos con
 * K->ram(RAM_ALLOC, 0, n, "NOMBRE") y el kernel le da los primeros que esten libres; si ya se usaron, salta
 * a los siguientes, hasta que se acabe la RAM. Al terminar los devuelve con K->ram(RAM_FREE, direccion, 0, 0).
 *
 *   banco 0  0x09000000  TIENDA   el programa de la tienda que corre (PKG, escritorio, BCC...): fijo, no se presta
 *   banco 1  0x0B000000  SISTEMA  buzon de NET, lista de PKG, escritor FAT, tablas de PLAY...: fijo, no se presta
 *   banco 2+ 0x0D000000  libres para quien los pida
 *
 * En 64 bits entry.asm solo mapea el primer GB; ram_init() mapea con paginas de 2 MB toda la RAM que la BIOS
 * reporta (E820), asi los bancos llegan hasta el final de verdad. Las tablas de pagina extra van en 0xB300000
 * (dentro del banco SISTEMA; 256 tablas = hasta 256 GB).
 */
#include "kernel.h"
#include "kapi.h"

#define BANK_BASE  0x9000000ull
#define BANK_SIZE  0x2000000ull
#define MAXBANKS   4096                     /* 128 GB */
#define B_FREE     0
#define B_USED     1                        /* primer banco de un prestamo */
#define B_CONT     2                        /* continuacion del prestamo */
#define B_SYS      3                        /* fijo del sistema */
#define B_HOLE     4                        /* ahi no hay RAM (hueco PCI, ROM...) */

static u8 state[MAXBANKS];
static u16 count[MAXBANKS];                 /* bancos del prestamo (en el primero) */
static char owner[MAXBANKS][12];
static int nbanks;
static u64 ram_top;                         /* final de la RAM mas alta */

static int usable(u64 a, u64 b)             /* [a, b) entero dentro de RAM utilizable? */
{
    const struct bootinfo *bi = BOOTINFO;
    int i;
    if (!bi->e820n)
        return b <= ((u64)bi->extkb + 1024) * 1024;
    for (i = 0; i < bi->e820n; i++) {
        const struct e820 *e = &E820_TABLE[i];
        if (e->type == 1 && e->base <= a && e->base + e->len >= b)
            return 1;
    }
    return 0;
}

static void set_owner(int i, const char *who)
{
    int k = 0;
    if (who)
        for (; who[k] && k < 11; k++) owner[i][k] = who[k];
    owner[i][k] = 0;
}

#ifdef __x86_64__
#define PDPT     ((volatile u64 *)0x71000)
#define PD_POOL  0xB300000ull               /* tablas de pagina para la RAM sobre 1 GB */
#define PD_MAX   256

/* mapea con identidad (paginas de 2 MB, con cache) la RAM utilizable que esta sobre 1 GB */
static void map_high_ram(void)
{
    const struct bootinfo *bi = BOOTINFO;
    int i, used = 0;
    for (i = 0; i < bi->e820n; i++) {
        const struct e820 *e = &E820_TABLE[i];
        u64 a, b;
        if (e->type != 1) continue;
        a = (e->base + 0x1FFFFF) & ~0x1FFFFFull;
        b = (e->base + e->len) & ~0x1FFFFFull;
        if (a < 0x40000000ull) a = 0x40000000ull;
        for (; a < b; a += 0x200000) {
            u64 gb = a >> 30, idx = (a >> 21) & 511;
            volatile u64 *pd;
            if (gb >= 512) break;
            if (!(PDPT[gb] & 1)) {
                if (used >= PD_MAX) goto done;
                pd = (volatile u64 *)(unsigned long)(PD_POOL + (u64)used++ * 4096);
                { int k; for (k = 0; k < 512; k++) pd[k] = 0; }
                PDPT[gb] = (u64)(unsigned long)pd | 3;
            } else
                pd = (volatile u64 *)(unsigned long)(PDPT[gb] & ~0xFFFull);
            if (!(pd[idx] & 1))
                pd[idx] = a | 0x83;         /* P | RW | PS (normal, con cache) */
        }
    }
done:
    {
        u64 cr3;
        __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    }
}
#define ADDR_LIMIT 0x8000000000ull          /* 512 GB */
#else
#define ADDR_LIMIT 0xF0000000ull            /* 32 bits sin paginacion: lo que cabe debajo de 4 GB */
#endif

void ram_init(void)
{
    const struct bootinfo *bi = BOOTINFO;
    int i;
    ram_top = 0;
    if (bi->e820n) {
        for (i = 0; i < bi->e820n; i++)
            if (E820_TABLE[i].type == 1 && E820_TABLE[i].base + E820_TABLE[i].len > ram_top)
                ram_top = E820_TABLE[i].base + E820_TABLE[i].len;
    } else
        ram_top = ((u64)bi->extkb + 1024) * 1024;
    if (ram_top > ADDR_LIMIT) ram_top = ADDR_LIMIT;
#ifdef __x86_64__
    if (bi->e820n && ram_top > 0x40000000ull)
        map_high_ram();
#endif
    nbanks = 0;
    for (i = 0; i < MAXBANKS; i++) {
        u64 a = BANK_BASE + (u64)i * BANK_SIZE;
        if (a + BANK_SIZE > ram_top) break;
        state[i] = usable(a, a + BANK_SIZE) ? B_FREE : B_HOLE;
        count[i] = 0;
        owner[i][0] = 0;
        nbanks = i + 1;
    }
    if (nbanks > 0 && state[0] == B_FREE) { state[0] = B_SYS; set_owner(0, "TIENDA"); }
    if (nbanks > 1 && state[1] == B_FREE) { state[1] = B_SYS; set_owner(1, "SISTEMA"); }
}

static int bank_of(u64 addr)
{
    if (addr < BANK_BASE || (addr - BANK_BASE) % BANK_SIZE) return -1;
    addr = (addr - BANK_BASE) / BANK_SIZE;
    return addr < (u64)nbanks ? (int)addr : -1;
}

/* K->ram():
 *   RAM_ALLOC  n bancos seguidos -> direccion del primero (0 = no hay)
 *   RAM_FREE   a = direccion que dio RAM_ALLOC -> 0 bien, -1 no era un prestamo
 *   RAM_FREEALL who = nombre: devuelve todo lo de ese dueno -> bancos devueltos
 *   RAM_INFO   n = 0 total de bancos, 1 libres, 2 tamano de banco, 3 final de la RAM, 4 primer banco */
u64 ram_api(int op, u64 a, unsigned n, const char *who)
{
    int i, k;
    switch (op) {
    case RAM_ALLOC:
        if (!n || n > MAXBANKS) return 0;
        for (i = 0; i + (int)n <= nbanks; i++) {
            for (k = 0; k < (int)n && state[i + k] == B_FREE; k++) ;
            if (k < (int)n) { i += k; continue; }
            for (k = 0; k < (int)n; k++) { state[i + k] = k ? B_CONT : B_USED; set_owner(i + k, who ? who : "?"); }
            count[i] = (u16)n;
            return BANK_BASE + (u64)i * BANK_SIZE;
        }
        return 0;
    case RAM_FREE:
        i = bank_of(a);
        if (i < 0 || state[i] != B_USED) return (u64)-1;
        for (k = 0; k < count[i]; k++) { state[i + k] = B_FREE; owner[i + k][0] = 0; }
        count[i] = 0;
        return 0;
    case RAM_FREEALL: {
        u64 r = 0;
        if (!who) return 0;
        for (i = 0; i < nbanks; i++) {
            if (state[i] != B_USED) continue;
            for (k = 0; who[k] && who[k] == owner[i][k]; k++) ;
            if (who[k] || owner[i][k]) continue;
            r += count[i];
            ram_api(RAM_FREE, BANK_BASE + (u64)i * BANK_SIZE, 0, 0);
        }
        return r;
    }
    case RAM_INFO:
        if (n == 0) return (u64)nbanks;
        if (n == 1) { u64 f = 0; for (i = 0; i < nbanks; i++) f += state[i] == B_FREE; return f; }
        if (n == 2) return BANK_SIZE;
        if (n == 3) return ram_top;
        if (n == 4) return BANK_BASE;
        return 0;
    }
    return 0;
}

/* 1 si la direccion es RAM mapeada (para PEEK/POKE) */
int ram_ok(u64 addr)
{
    if (addr < 0x40000000ull) return 1;
    if (addr >= ram_top) return 0;
    return usable(addr & ~0x1FFFFFull, (addr & ~0x1FFFFFull) + 0x200000);
}

/* linea i del comando RAM de BASIC (0 = no hay mas). Agrupa los bancos seguidos del mismo estado. */
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *putn(char *o, u64 v)
{
    char t[24];
    int n = 0;
    do t[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}
static char *puthex(char *o, u64 v)
{
    int s, started = 0;
    o = put(o, "0X");
    for (s = 60; s >= 0; s -= 4) {
        int d = (int)((v >> s) & 15);
        if (d || started || s <= 24) { *o++ = "0123456789ABCDEF"[d]; started = 1; }
    }
    *o = 0;
    return o;
}

int ram_line(int i, char *out)
{
    int b = 0, line = 0;
    char *o = out;
    if (i == 0) {
        u64 f = ram_api(RAM_INFO, 0, 1, 0);
        o = put(o, "RAM: ");
        o = putn(o, ram_top >> 20);
        o = put(o, " MB   BANCOS DE 32 MB: ");
        o = putn(o, (u64)nbanks);
        o = put(o, " (");
        o = putn(o, f);
        o = put(o, " LIBRES = ");
        o = putn(o, f * 32);
        put(o, " MB)");
        return 1;
    }
    while (b < nbanks) {
        int s = state[b], e = b + 1;
        if (s == B_USED || s == B_SYS) {
            e = b + (s == B_USED ? count[b] : 1);
        } else {
            while (e < nbanks && state[e] == s) e++;
        }
        if (++line == i) {
            o = putn(o, (u64)b);
            if (e - b > 1) { o = put(o, "-"); o = putn(o, (u64)(e - 1)); }
            o = put(o, "  ");
            o = puthex(o, BANK_BASE + (u64)b * BANK_SIZE);
            o = put(o, "  ");
            o = putn(o, (u64)(e - b) * 32);
            o = put(o, " MB  ");
            o = put(o, s == B_FREE ? "LIBRE" : s == B_HOLE ? "(SIN RAM)" : owner[b]);
            return 1;
        }
        b = e;
    }
    return 0;
}
