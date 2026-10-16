/* install.c - INSTALL.APP: instala BASIC-OS en otro disco (o en el mismo USB del que arrancaste).
 *
 * 1. Lee a RAM todo el sistema del disco de arranque: los primeros 4096 sectores (sector de arranque, stage2, kernel, /SYS,
 *    SAVE) y la particion FAT16 completa (archivos, subdirectorios y nombres largos). Por eso se puede instalar sobre el
 *    mismo USB: cuando empieza a escribir, el sistema ya esta entero en memoria.
 * 2. Crea una tabla de particiones y un FAT16 nuevo llamado "BASIC-OS FS", con el tamano de cluster que le toque al disco
 *    destino (maximo ~2 GB de particion), y recoloca los archivos en el.
 * 3. Escribe primero la particion, luego el kernel y por ultimo el sector 0, y comprueba cada bloque leyendolo de vuelta.
 *    Solo usa discos que ve la BIOS (INT 13h): un USB de arranque lo es. El USB nativo (EHCI) y los CD no se pueden escribir.
 * Uso:  INSTALL   (o EXEC "INSTALL.APP")
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

static const struct kapi *K;

/* ---- memoria: todo por encima de 192 MB (hace falta RAM hasta ~200 MB + el tamano del sistema) ---- */
#define IB       0xC000000UL
#define HEADMAX  4096u                         /* sectores antes de la particion: arranque, stage2, kernel, /SYS */
#define P_HEAD   ((u8 *)(IB))                  /* 2 MiB */
#define P_SFAT   ((u16 *)(IB + 0x200000UL))    /* FAT del origen (hasta 256 KB) */
#define P_TFAT   ((u16 *)(IB + 0x240000UL))    /* FAT del destino */
#define P_SROOT  ((u8 *)(IB + 0x280000UL))
#define P_TROOT  ((u8 *)(IB + 0x290000UL))
#define P_SCR    ((u8 *)(IB + 0x2A0000UL))     /* 64 KB para releer lo escrito */
#define P_IMG    ((u8 *)(IB + 0x400000UL))     /* area de datos del destino, ya recolocada */
#define RAM_TOP  0x40000000ULL                 /* la memoria identica llega a 1 GB */

struct bootinfo { u32 magic; u16 w, h, pitch; u8 bpp, gfx; u32 fb; u8 rs, rp, gs, gp, bs, bp; u16 e820n; u16 extkb; } __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static u64 ram_end(void)
{
    u64 end = 0;
    int i;
    if (BI->e820n) {
        for (i = 0; i < BI->e820n; i++)
            if (E8[i].type == 1 && E8[i].base <= 0x4000000ull && E8[i].base + E8[i].len > 0x4000000ull)
                end = E8[i].base + E8[i].len;
    } else {
        end = ((u64)BI->extkb + 1024) * 1024;
    }
    return end > RAM_TOP ? RAM_TOP : end;
}

/* ---- pantalla ---- */
static void pr(const char *s) { K->print(s); }
static void prnum(u32 v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    pr(b + i);
}
static void prmb(u32 sectors)
{
    u32 mb = sectors / 2048;
    if (mb >= 10240) { prnum(mb / 1024); pr(" GB"); }
    else { prnum(mb); pr(" MB"); }
}
static int fail(const char *m) { pr("\nINSTALL: "); pr(m); pr("\n"); return 0; }

static int readline(char *buf, int max)
{
    int len = 0;
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); continue; }
        if (c == 27) return -1;
        if (c == '\r' || c == '\n') { pr("\n"); buf[len] = 0; return len; }
        if ((c == 8 || c == 127 || c == 20) && len) { len--; pr("\b \b"); continue; }
        if (c >= 32 && c < 127 && len < max - 1) {
            char s[2];
            if (c >= 'a' && c <= 'z') c -= 32;
            buf[len++] = (char)c;
            s[0] = (char)c; s[1] = 0;
            pr(s);
        }
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int same(const u8 *a, const u8 *b, u32 n)
{
    while (n--) if (*a++ != *b++) return 0;
    return 1;
}

/* ---- unidades ---- */
struct drv { char name[8]; u32 sectors; int boot, wr; };
static struct drv drv[16];
static int ndrv;

static void list_drives(void)
{
    int i;
    ndrv = 0;
    for (i = 0; i < 16; i++) {
        char line[64];
        struct drv *d = &drv[ndrv];
        int k = 0, f;
        if (!K->drive_line(i, line))
            continue;
        while (line[k] && line[k] != ' ' && k < 7) { d->name[k] = line[k]; k++; }
        d->name[k] = 0;
        d->sectors = (u32)K->blk(d->name, 2, 0, 0, 0);
        f = K->blk(d->name, 3, 0, 0, 0);
        d->boot = f & 1;
        d->wr = (f & 2) != 0;
        ndrv++;
    }
}

/* ---- origen: el disco de arranque ---- */
static const char *sname;
static u32 head_n;                             /* sectores sueltos del principio que se copian */
static int have_fs;
static u32 spart;                              /* LBA de la particion en el origen */
static u32 s_spc, s_res, s_nfat, s_rootent, s_fatsz, s_root_lba, s_data_lba, s_nclus;

static int sread(u32 lba, u32 n, void *buf) { return K->blk(sname, 0, lba, n, buf); }

static void load_head(void)
{
    u32 lim = HEADMAX;
    static u8 s0[512];
    u32 pt_lba = 0, pt_len = 0;
    int i;
    have_fs = 0;
    if (sread(0, 1, s0) == 0 && s0[510] == 0x55 && s0[511] == 0xAA) {
        for (i = 0; i < 4; i++) {
            const u8 *e = s0 + 0x1BE + 16 * i;
            u32 st = *(const u32 *)(e + 8), ln = *(const u32 *)(e + 12);
            if ((e[4] == 4 || e[4] == 6 || e[4] == 0x0E) && st && ln) { pt_lba = st; pt_len = ln; break; }
        }
    }
    spart = pt_lba;
    if (pt_lba && pt_lba < lim)
        lim = pt_lba;
    if ((u32)K->blk(sname, 2, 0, 0, 0) < lim)
        lim = (u32)K->blk(sname, 2, 0, 0, 0);
    head_n = lim;
    (void)pt_len;
}

static u32 s_total;
static int open_source_fs(void)
{
    static u8 bs[512];
    u32 total, rootsec, nclus;
    if (!spart || sread(spart, 1, bs) || bs[510] != 0x55 || bs[511] != 0xAA)
        return 0;
    if (*(u16 *)(bs + 11) != 512 || !bs[13] || (bs[13] & (bs[13] - 1)) || !*(u16 *)(bs + 14) || !bs[16] || bs[16] > 2)
        return 0;
    s_spc = bs[13]; s_res = *(u16 *)(bs + 14); s_nfat = bs[16]; s_rootent = *(u16 *)(bs + 17);
    total = *(u16 *)(bs + 19) ? *(u16 *)(bs + 19) : *(u32 *)(bs + 32);
    s_fatsz = *(u16 *)(bs + 22);
    if (!s_fatsz || !total || (s_rootent & 15) || s_rootent > 512)
        return 0;
    rootsec = s_rootent / 16;
    if (total < s_res + s_nfat * s_fatsz + rootsec)
        return 0;
    nclus = (total - s_res - s_nfat * s_fatsz - rootsec) / s_spc;
    if (nclus < 4085 || nclus > 65524 || s_fatsz > 256)
        return 0;                              /* solo FAT16 */
    s_nclus = nclus; s_total = total;
    s_root_lba = spart + s_res + s_nfat * s_fatsz;
    s_data_lba = s_root_lba + rootsec;
    if (sread(spart + s_res, s_fatsz, P_SFAT))
        return 0;
    memset(P_SROOT, 0, 16384);
    if (rootsec && sread(s_root_lba, rootsec, P_SROOT))
        return 0;
    return 1;
}

/* ---- destino ---- */
#define PSTART 4096u
static u32 t_spc, t_fatsz, t_nclus, t_psect, t_csize, t_data;   /* t_data: LBA relativo a la particion */
static u32 nextc = 2;
static u8 *img_limit;

static int geometry(u32 disk_sectors)
{
    u32 R = 1, rootsec = 32, spc, fatsz, nclus, psect;
    if (disk_sectors < PSTART + 16384)
        return 0;
    psect = disk_sectors - PSTART;
    if (psect > 4160000u)
        psect = 4160000u;                      /* ~2 GB: el maximo de FAT16 con clusters de 32 KB */
    for (spc = 1; spc <= 64; spc <<= 1) {
        int it;
        fatsz = 1;
        for (it = 0; it < 8; it++) {
            nclus = (psect - R - rootsec - 2 * fatsz) / spc;
            fatsz = ((nclus + 2) * 2 + 511) / 512;
        }
        nclus = (psect - R - rootsec - 2 * fatsz) / spc;
        if (nclus <= 65000)
            break;
    }
    if (spc > 64 || nclus < 4085)
        return 0;
    t_spc = spc; t_fatsz = fatsz; t_nclus = nclus; t_psect = psect;
    t_csize = spc * 512;
    t_data = R + 2 * fatsz + rootsec;
    return 1;
}

static u32 alloc(u32 n)
{
    u32 f, i;
    if (!n)
        n = 1;
    if (nextc + n > t_nclus + 2 || P_IMG + (u64)(nextc - 2 + n) * t_csize > img_limit)
        return 0;
    f = nextc;
    for (i = 0; i < n; i++)
        P_TFAT[f + i] = (i + 1 < n) ? (u16)(f + i + 1) : 0xFFFF;
    nextc += n;
    return f;
}
static u8 *cptr(u32 c) { return P_IMG + (u64)(c - 2) * t_csize; }

/* ---- leer cadenas de clusters del origen ---- */
static u32 chain_len(u32 c)
{
    u32 n = 0;
    while (c >= 2 && c < s_nclus + 2 && n <= s_nclus) { n++; c = P_SFAT[c]; if (c >= 0xFFF8) break; }
    return n;
}

/* lee `need` clusters de origen a partir de c en dst */
static int read_chain(u32 c, u32 need, u8 *dst)
{
    u32 scs = s_spc * 512;
    while (need) {
        u32 run = 1;
        if (c < 2 || c >= s_nclus + 2)
            return 1;
        while (run < need && P_SFAT[c + run - 1] == c + run && c + run < s_nclus + 2)
            run++;
        if (sread(s_data_lba + (c - 2) * s_spc, run * s_spc, dst))
            return 1;
        dst += (u64)run * scs;
        need -= run;
        if (need) {
            c = P_SFAT[c + run - 1];
            if (c >= 0xFFF8)
                return 1;
        }
    }
    return 0;
}

static u32 files_n, dirs_n, bytes_total;
static const char *clone_err;

static void setfirst(u8 *e, u32 c) { *(u16 *)(e + 26) = (u16)c; *(u16 *)(e + 20) = 0; }

static int clone_dir(u8 *ents, u32 nent, u32 self, int depth)
{
    u32 i;
    if (depth > 8) { clone_err = "directorios demasiado anidados"; return 1; }
    for (i = 0; i < nent; i++) {
        u8 *e = ents + 32 * i;
        u8 at;
        u32 first, size;
        if (e[0] == 0)
            break;
        if (e[0] == 0xE5)
            continue;
        at = e[11];
        if ((at & 0x3F) == 0x0F)
            continue;                          /* nombre largo: se copia tal cual */
        if (at & 0x08) {                       /* etiqueta del volumen del origen: se reemplaza */
            e[0] = 0xE5;
            continue;
        }
        if (e[0] == '.')
            continue;                          /* "." y ".." se ajustan al crear el subdirectorio */
        first = *(u16 *)(e + 26);
        size = *(u32 *)(e + 28);
        if (at & 0x10) {
            u32 n = chain_len(first), nt, c;
            u8 *d;
            if (first < 2 || !n) { clone_err = "subdirectorio danado"; return 1; }
            nt = (n * s_spc * 512 + t_csize - 1) / t_csize;
            c = alloc(nt);
            if (!c) { clone_err = "no caben los archivos en el disco destino"; return 1; }
            d = cptr(c);
            memset(d, 0, (u64)nt * t_csize);
            if (read_chain(first, n, d)) { clone_err = "error leyendo el origen"; return 1; }
            setfirst(e, c);
            setfirst(d, c);                    /* "." */
            setfirst(d + 32, self);            /* ".." (0 si el padre es la raiz) */
            dirs_n++;
            if (clone_dir(d, n * s_spc * 16, c, depth + 1))
                return 1;
        } else if (size && first >= 2) {
            u32 nt = (size + t_csize - 1) / t_csize, c = alloc(nt), need = (size + s_spc * 512 - 1) / (s_spc * 512);
            u8 *d;
            if (!c) { clone_err = "no caben los archivos en el disco destino"; return 1; }
            d = cptr(c);
            if (read_chain(first, need, d)) { clone_err = "error leyendo el origen"; return 1; }
            memset(d + size, 0, (u64)nt * t_csize - size);
            setfirst(e, c);
            files_n++;
            bytes_total += size;
        } else {
            setfirst(e, 0);
        }
    }
    return 0;
}

static int build_fs(void)
{
    u8 *r = P_TROOT;
    int i, placed = 0;
    memset(P_TFAT, 0, 256 * 1024);
    P_TFAT[0] = 0xFFF8; P_TFAT[1] = 0xFFFF;
    memset(r, 0, 16384);
    nextc = 2; files_n = dirs_n = 0; bytes_total = 0;
    if (have_fs) {
        memcpy(r, P_SROOT, s_rootent * 32);
        if (clone_dir(r, s_rootent, 0, 0))
            return 0;
    }
    /* etiqueta del volumen: primera entrada libre o borrada */
    for (i = 0; i < 512 && !placed; i++) {
        u8 *e = r + 32 * i;
        if (e[0] == 0 || e[0] == 0xE5) {
            memset(e, 0, 32);
            memcpy(e, "BASIC-OS FS", 11);
            e[11] = 0x08;
            *(u16 *)(e + 24) = 0x0021;         /* 1980-01-01 */
            placed = 1;
        }
    }
    return 1;
}

/* ---- escribir con verificacion ---- */
static u32 written_mb;
static int wr(const char *name, u32 lba, u32 n, const u8 *src)
{
    while (n) {
        u32 c = n > 128 ? 128 : n;
        if (K->blk(name, 1, lba, c, (void *)src))
            return 1;
        if (K->blk(name, 0, lba, c, P_SCR))
            return 2;
        if (!same(P_SCR, src, c * 512))
            return 3;
        lba += c; src += c * 512; n -= c;
        written_mb += c;
        if (written_mb >= 2048) { written_mb -= 2048; pr("."); }
    }
    return 0;
}

static int wr_or_fail(const char *name, u32 lba, u32 n, const u8 *src, const char *what)
{
    int r = wr(name, lba, n, src);
    if (!r)
        return 1;
    pr("\nINSTALL: fallo escribiendo ");
    pr(what);
    pr(r == 1 ? " (la BIOS rechazo la escritura)\n" : r == 2 ? " (no pude releerlo)\n" : " (lo leido no coincide con lo escrito)\n");
    return 0;
}

static int do_write(const char *tn, u32 tsect)
{
    static u8 bs[512], mbr[512];
    u32 used = nextc - 2, i;
    u8 *e;
    (void)tsect;
    written_mb = 0;
    pr("Escribiendo particion ");
    /* la particion: arranque FAT, 2 FATs, raiz y solo los clusters usados */
    memset(bs, 0, sizeof bs);
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90; memcpy(bs + 3, "BASICOS ", 8);
    *(u16 *)(bs + 11) = 512; bs[13] = (u8)t_spc; *(u16 *)(bs + 14) = 1; bs[16] = 2;
    *(u16 *)(bs + 17) = 512; *(u16 *)(bs + 19) = t_psect < 65536 ? (u16)t_psect : 0; bs[21] = 0xF8;
    *(u16 *)(bs + 22) = (u16)t_fatsz; *(u16 *)(bs + 24) = 63; *(u16 *)(bs + 26) = 255;
    *(u32 *)(bs + 28) = PSTART; *(u32 *)(bs + 32) = t_psect < 65536 ? 0 : t_psect;
    bs[36] = 0x80; bs[38] = 0x29; *(u32 *)(bs + 39) = K->seed(); memcpy(bs + 43, "BASIC-OS FS", 11); memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    if (!wr_or_fail(tn, PSTART + 1, t_fatsz, (const u8 *)P_TFAT, "la FAT"))
        return 0;
    if (!wr_or_fail(tn, PSTART + 1 + t_fatsz, t_fatsz, (const u8 *)P_TFAT, "la copia de la FAT"))
        return 0;
    if (!wr_or_fail(tn, PSTART + 1 + 2 * t_fatsz, 32, P_TROOT, "el directorio raiz"))
        return 0;
    if (used && !wr_or_fail(tn, PSTART + t_data, used * t_spc, P_IMG, "los archivos"))
        return 0;
    if (!wr_or_fail(tn, PSTART, 1, bs, "el sector de la particion"))
        return 0;
    pr("\nEscribiendo el sistema (arranque, kernel y /SYS) ");
    if (head_n > 1 && !wr_or_fail(tn, 1, head_n - 1, P_HEAD + 512, "el kernel"))
        return 0;
    /* sector 0 al final: el disco solo arranca si todo lo anterior llego bien */
    memcpy(mbr, P_HEAD, 512);
    for (i = 0x1BE; i < 0x1FE; i++)
        mbr[i] = 0;
    e = mbr + 0x1BE;
    e[0] = 0x80; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[4] = 0x0E; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    *(u32 *)(e + 8) = PSTART; *(u32 *)(e + 12) = t_psect;
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (!wr_or_fail(tn, 0, 1, mbr, "el sector de arranque"))
        return 0;
    return 1;
}

int mod_main(const struct kapi *k)
{
    char line[16];
    int i, ti, n, ndst = 0, dst[16];
    const char *tn;
    u32 tsect;
    K = k;
    pr("\nBASIC-OS INSTALL: instalar el sistema en un disco\n");
    if (ram_end() < IB + 0x800000ULL + 0x1000000ULL) {
        pr("Hace falta mas RAM (en QEMU usa -m 256)\n");
        return 1;
    }
    list_drives();
    sname = 0;
    for (i = 0; i < ndrv; i++)
        if (drv[i].boot)
            sname = drv[i].name;
    if (!sname) { fail("no encuentro el disco de arranque (arranca desde disco o USB visto por la BIOS)"); return 1; }
    pr("Origen (disco de arranque): "); pr(sname); pr("\n\nDiscos donde se puede instalar:\n");
    for (i = 0; i < ndrv; i++) {
        if (!drv[i].wr || drv[i].sectors < PSTART + 16384)
            continue;
        dst[ndst++] = i;
        prnum((u32)ndst); pr(") "); pr(drv[i].name); pr("  "); prmb(drv[i].sectors);
        if (drv[i].boot) pr("   <- este mismo disco (se vuelve a escribir desde la RAM)");
        pr("\n");
    }
    if (!ndst) { fail("no hay ningun disco escribible de al menos 10 MB"); return 1; }
    pr("\nNumero del disco destino (ESC = cancelar): ");
    n = readline(line, sizeof line);
    if (n <= 0) { pr("Cancelado.\n"); return 0; }
    ti = 0;
    for (i = 0; line[i] >= '0' && line[i] <= '9'; i++)
        ti = ti * 10 + (line[i] - '0');
    if (ti < 1 || ti > ndst) { fail("numero invalido"); return 1; }
    tn = drv[dst[ti - 1]].name;
    tsect = drv[dst[ti - 1]].sectors;
    if (!geometry(tsect)) { fail("el disco destino es muy chico (minimo ~10 MB)"); return 1; }

    img_limit = (u8 *)(unsigned long)ram_end();
    pr("\nCargando el sistema en RAM ...\n");
    load_head();
    if (head_n < 160 || sread(0, head_n, P_HEAD)) { fail("no pude leer el sistema del origen"); return 1; }
    have_fs = open_source_fs();
    if (!have_fs)
        pr("(el origen no tiene particion FAT16: se creara una vacia)\n");
    if (!build_fs()) { fail(clone_err ? clone_err : "no pude armar el sistema de archivos"); return 1; }

    pr("Listo en RAM: "); prnum(head_n / 2); pr(" KB de sistema, ");
    prnum(files_n); pr(" archivos y "); prnum(dirs_n); pr(" carpetas ("); prnum(bytes_total / 1024); pr(" KB).\n");
    pr("Se creara BASIC-OS FS (FAT16, cluster de "); prnum(t_csize / 1024 ? t_csize / 1024 : 1); pr(t_csize >= 1024 ? " KB) de " : " B) de ");
    prmb(t_psect); pr(" en "); pr(tn); pr(".\n");
    pr("\n!!! SE BORRARA TODO EL CONTENIDO DE "); pr(tn); pr(" !!!\nEscribe el nombre del disco para confirmar: ");
    n = readline(line, sizeof line);
    if (n <= 0 || !streq(line, tn)) { pr("Cancelado: no se escribio nada.\n"); return 0; }

    pr("\nNo apagues ni saques el disco.\n");
    if (!do_write(tn, tsect)) {
        pr("\nLa instalacion NO termino. El disco puede haber quedado inservible: reinstala.\n");
        return 1;
    }
    pr("\n\nInstalacion completa y verificada en "); pr(tn); pr(".\nReinicia y arranca desde ese disco.\n");
    return 0;
}
