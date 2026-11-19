/* pkg.c - PKG.APP: la "tienda" de BASIC-OS. Baja programas de un hub (hub/server.py en Render) por HTTPS y los ejecuta.
 *
 * Flujo:  PKG.APP -> NET.APP (iPXE: dhcp + imgfetch https://HUB/list + bosstash) -> memoria fija -> menu en pantalla
 *         -> elegir uno -> NET.APP otra vez (imgfetch https://HUB/get/NOMBRE + bosstash) -> comprobar SHA-256 y cabecera
 *         PCBM -> saltar a su mod_main.
 * Los programas de la tienda son modulos PCBM enlazados a 0x9000000 (hasta 32 MB). Si el disco de arranque es FAT16 y lo ve la
 * BIOS (el que crea INSTALL), cada programa bajado se guarda en HDn.1:/APPS/ y la proxima vez se ejecuta desde el disco, sin red.
 * Direccion del hub: primera linea de HUB.CFG en la particion FAT (HD0.1:HUB.CFG ...) o la que escribas con U.
 * No hay direccion por defecto a proposito: PKG ejecuta lo que baja, asi que solo habla con el hub que tu pongas.
 * Uso:  PKG   (o EXEC "PKG.APP")     Teclas: numero + Enter = ejecutar (del disco si ya esta), +numero = bajar otra vez,
 *       -numero = borrar del disco, S = subir un paquete al hub, R = recargar, U = direccion del hub, Q/ESC = salir.
 *       Con muchos paquetes: F texto = buscar en nombre y descripcion (F solo = quitar el filtro), T = ver las etiquetas,
 *       T etiqueta = solo esa, flechas o N / A = pagina siguiente / anterior (12 por pagina). La etiqueta es el [texto] con el que empieza la
 *       descripcion. Los numeros no cambian al filtrar: son los de la lista completa.
 * Paquetes: NOMBRE.BIN (modulo PCBM, se ejecuta) o NOMBRE.BAS (programa BASIC en texto: se carga y se escribe RUN).
 * Subir (S): un programa guardado con SAVE (se sube como NOMBRE.BAS) o un archivo de la particion (HD0.1:RUTA/X.BIN).
 *   Viaja por HTTPS como POST a HUB/upload con NET.BIN (comando bospost). El hub pide una clave: la segunda linea de
 *   HUB.CFG, o la que escribas. Maximo 2 MB por paquete.
 * Compilar (C): convierte un programa BASIC (guardado con SAVE o un .BAS de la particion) en NOMBRE.BIN con bas2bin
 *   (tools/bas2bin): nativo (x86-64, rapido) o empaquetado (todo el BASIC). Se guarda en HDn.1:/APPS/.
 */
#include "kapi.h"
#include "mod_head.h"
#include "sys/binwin.h"
#include "tools/bas2bin/b2b.c"                 /* el compilador de .BAS a .BIN (mismo codigo que en Linux) */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
unsigned long strlen(const char *s);

#define APP_BASE   0x9000000u
#define APP_MAX    0x2000000u
#define LIST_ADDR  0xB000000u
#define LIST_MAX   0x100000u
#define MBOX_ADDR  0xB100000u
#define NEED_RAM   0xB300000u           /* hay que tener RAM hasta aqui: ~179 MB (usa -m 256 en QEMU) */
#define FW_FAT     0xB200000u           /* escritor FAT16: FAT (128 KB), raiz (16 KB), cluster/directorio (64 KB), relectura (64 KB) */
#define FW_ROOT    0xB220000u
#define FW_BUF     0xB230000u
#define FW_SCR     0xB240000u
#define MBOX_MAGIC 0x48545342u          /* 'BSTH' */

struct mbox { u32 magic, app_len, list_len, spare; };
struct hdr { u32 magic, end, entry, base; };

/* ---- bootinfo (stage2.asm): para saber cuanta RAM hay ---- */
struct bootinfo { u32 magic; u16 w, h, pitch; u8 bpp, gfx; u32 fb; u8 rs, rp, gs, gp, bs, bp; u16 e820n; u16 extkb; } __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static const struct kapi *K;

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
    return end;
}

/* ---- SHA-256 (para comprobar lo descargado contra la lista del hub) ---- */
static const u32 KK[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(u32 *h, const u8 *p)
{
    u32 w[64], a, b, c, d, e, f, g, hh, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (u32)p[i * 4] << 24 | (u32)p[i * 4 + 1] << 16 | (u32)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (i = 0; i < 64; i++) {
        t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + KK[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static void sha256_hex(const u8 *data, u32 n, char *out)         /* out: 65 bytes */
{
    u32 h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    u8 tail[128];
    u32 i, full = n / 64, rem = n % 64, tl;
    static const char hx[] = "0123456789abcdef";
    for (i = 0; i < full; i++)
        sha_block(h, data + i * 64);
    memset(tail, 0, sizeof tail);
    memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    tl = rem < 56 ? 64 : 128;
    {
        u64 bits = (u64)n * 8;
        for (i = 0; i < 8; i++)
            tail[tl - 1 - i] = (u8)(bits >> (8 * i));
    }
    sha_block(h, tail);
    if (tl == 128)
        sha_block(h, tail + 64);
    for (i = 0; i < 8; i++)
        for (u32 j = 0; j < 8; j++)
            out[i * 8 + j] = hx[(h[i] >> (28 - 4 * j)) & 15];
    out[64] = 0;
}

/* ---- utilidades ---- */
static void pr(const char *s) { K->print(s); }

static void prnum(unsigned v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    pr(b + i);
}

static void prsize(unsigned v)
{
    if (v >= 1024 * 1024) { prnum(v / (1024 * 1024)); pr("."); prnum(v / 1024 % 1024 * 10 / 1024); pr(" MB"); }
    else if (v >= 1024) { prnum(v / 1024); pr("."); prnum(v % 1024 * 10 / 1024); pr(" KB"); }
    else { prnum(v); pr(" B"); }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- direccion del hub ---- */
static int streq_n(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i] || !a[i])
            return 0;
    return 1;
}

static char hub[96];
static char hubkey[64];

static void load_cfg(void)
{
    static const char *const where[] = {
        "HD0.1:HUB.CFG", "HD1.1:HUB.CFG", "HD2.1:HUB.CFG", "HD0:HUB.CFG", "FD1:HUB.CFG", "USB0.1:HUB.CFG", 0
    };
    char buf[200];
    unsigned total = 0;
    int i, n;
    for (i = 0; where[i]; i++) {
        total = 0;
        n = K->read(where[i], buf, (int)sizeof buf - 1, &total);
        if (n > 8) {
            int j = 0, k = 0;
            while (j < n && (buf[j] == ' ' || buf[j] == '\r' || buf[j] == '\n'))
                j++;
            while (j < n && buf[j] > ' ' && k < (int)sizeof hub - 1)
                hub[k++] = buf[j++];
            while (k > 0 && hub[k - 1] == '/')
                k--;
            hub[k] = 0;
            {   int q = 0;                          /* segunda linea (opcional): clave para subir paquetes */
                while (j < n && buf[j] != '\n')
                    j++;
                while (j < n && (buf[j] == ' ' || buf[j] == '\r' || buf[j] == '\n'))
                    j++;
                while (j < n && buf[j] > ' ' && q < (int)sizeof hubkey - 1)
                    hubkey[q++] = buf[j++];
                hubkey[q] = 0; }
            if (k > 8)
                return;
        }
    }
    hub[0] = 0;
}


/* ---- escritor FAT16 minimo: guarda los programas en HDn.1:/APPS/ del disco de arranque (solo discos de la BIOS) ---- */
static struct {
    int ok; char dev[8]; u32 part, spc, res, nfat, rootent, fatsz, root_lba, data_lba, nclus, dlo, dhi, csize;
} fw;
static u16 *const FATP = (u16 *)(unsigned long)FW_FAT;
static u8 *const FROOT = (u8 *)(unsigned long)FW_ROOT;
static u8 *const FBUF = (u8 *)(unsigned long)FW_BUF;
static u8 *const FSCR = (u8 *)(unsigned long)FW_SCR;

static int fw_wr(u32 lba, u32 n, const u8 *src)           /* escribe y relee: 0 = bien */
{
    while (n) {
        u32 c = n > 128 ? 128 : n, i;
        if (K->blk(fw.dev, 1, lba, c, (void *)src) || K->blk(fw.dev, 0, lba, c, FSCR))
            return 1;
        for (i = 0; i < c * 512; i++)
            if (FSCR[i] != src[i])
                return 1;
        lba += c; src += c * 512; n -= c;
    }
    return 0;
}
static void fat_set(u32 c, u32 v)
{
    u32 sc = c / 256;
    FATP[c] = (u16)v;
    if (sc < fw.dlo) fw.dlo = sc;
    if (sc > fw.dhi) fw.dhi = sc;
}
static int fw_flush_fat(void)
{
    u32 n, k;
    if (fw.dlo > fw.dhi)
        return 0;
    n = fw.dhi - fw.dlo + 1;
    for (k = 0; k < (u32)fw.nfat; k++)
        if (fw_wr(fw.part + fw.res + k * fw.fatsz + fw.dlo, n, (const u8 *)FATP + fw.dlo * 512))
            return 1;
    fw.dlo = 0xFFFFFFFFu; fw.dhi = 0;
    return 0;
}
static u32 clus_lba(u32 c) { return fw.data_lba + (c - 2) * fw.spc; }

static void fw_init(void)
{
    static u8 s0[512], bs[512];
    int i, nd = 0;
    u32 total, rootsec, nclus, part = 0, ndev;
    char name[8];
    fw.ok = 0;
    for (ndev = 0; ndev < 16; ndev++) {
        char line[64];
        int k = 0;
        if (!K->drive_line((int)ndev, line))
            continue;
        while (line[k] && line[k] != ' ' && k < 7) { name[k] = line[k]; k++; }
        name[k] = 0;
        if ((K->blk(name, 3, 0, 0, 0) & 3) == 3) { nd = 1; break; }
    }
    if (!nd)
        return;
    for (i = 0; name[i]; i++) fw.dev[i] = name[i];
    fw.dev[i] = 0;
    if (K->blk(fw.dev, 0, 0, 1, s0) || s0[510] != 0x55 || s0[511] != 0xAA)
        return;
    for (i = 0; i < 4; i++) {
        const u8 *e = s0 + 0x1BE + 16 * i;
        if ((e[4] == 4 || e[4] == 6 || e[4] == 0x0E) && *(const u32 *)(e + 8)) { part = *(const u32 *)(e + 8); break; }
    }
    if (!part || K->blk(fw.dev, 0, part, 1, bs) || bs[510] != 0x55 || *(u16 *)(bs + 11) != 512)
        return;
    fw.part = part; fw.spc = bs[13]; fw.res = *(u16 *)(bs + 14); fw.nfat = bs[16]; fw.rootent = *(u16 *)(bs + 17);
    total = *(u16 *)(bs + 19) ? *(u16 *)(bs + 19) : *(u32 *)(bs + 32);
    fw.fatsz = *(u16 *)(bs + 22);
    if (!fw.spc || (fw.spc & (fw.spc - 1)) || !fw.res || !fw.nfat || fw.nfat > 2 || !fw.fatsz || fw.fatsz > 256 ||
        (fw.rootent & 15) || fw.rootent > 512 || fw.spc > 64)
        return;
    rootsec = fw.rootent / 16;
    if (total < fw.res + fw.nfat * fw.fatsz + rootsec)
        return;
    nclus = (total - fw.res - fw.nfat * fw.fatsz - rootsec) / fw.spc;
    if (nclus < 4085 || nclus > 65524)
        return;
    fw.nclus = nclus; fw.csize = fw.spc * 512;
    fw.root_lba = part + fw.res + fw.nfat * fw.fatsz;
    fw.data_lba = fw.root_lba + rootsec;
    memset(FROOT, 0, 16384);
    if (K->blk(fw.dev, 0, part + fw.res, fw.fatsz, FATP) || K->blk(fw.dev, 0, fw.root_lba, rootsec, FROOT))
        return;
    fw.dlo = 0xFFFFFFFFu; fw.dhi = 0;
    fw.ok = 1;
}

static void fw_spec(char *out, const char *file)      /* "HD0.1:/APPS/NOMBRE" */
{
    int k = 0, j;
    for (j = 0; fw.dev[j]; j++) out[k++] = fw.dev[j];
    out[k++] = '.'; out[k++] = '1'; out[k++] = ':'; out[k++] = '/';
    out[k++] = 'A'; out[k++] = 'P'; out[k++] = 'P'; out[k++] = 'S'; out[k++] = '/';
    for (j = 0; file && file[j] && k < 60; j++) out[k++] = file[j];
    out[k] = 0;
}

static int name11(const char *n, u8 *o)
{
    int b = 0, e = 0, dot = 0, i;
    memset(o, ' ', 11);
    for (i = 0; n[i]; i++) {
        char c = n[i];
        if (c == '.') { if (dot || !b) return 0; dot = 1; continue; }
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
        if (dot) { if (e >= 3) return 0; o[8 + e++] = (u8)c; }
        else { if (b >= 8) return 0; o[b++] = (u8)c; }
    }
    return b > 0;
}

/* n clusters libres encadenados; devuelve el primero (0 = no hay) */
static u32 fw_alloc(u32 n)
{
    u32 c, prev = 0, first = 0, got = 0;
    for (c = 2; c < fw.nclus + 2 && got < n; c++) {
        if (FATP[c])
            continue;
        if (prev) fat_set(prev, c); else first = c;
        prev = c;
        got++;
        FATP[c] = 0xFFFF;                       /* provisional (se marca sucio abajo) */
    }
    if (got < n) {                              /* no cabe: deshacer */
        for (c = first; c && c != 0xFFFF; ) {
            u32 nx = FATP[c];
            FATP[c] = 0;
            c = (nx >= 0xFFF8 || nx < 2) ? 0 : nx;
        }
        return 0;
    }
    fat_set(prev, 0xFFFF);
    return first;
}
static void fw_free_chain(u32 c)
{
    u32 guard = 0;
    while (c >= 2 && c < fw.nclus + 2 && guard++ <= fw.nclus) {
        u32 nx = FATP[c];
        fat_set(c, 0);
        if (nx >= 0xFFF8)
            break;
        c = nx;
    }
}

static int eq11(const u8 *a, const u8 *b)
{
    int i;
    for (i = 0; i < 11; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* Busca la entrada nm en el directorio d (0 = raiz). Devuelve 1 si existe. *ent apunta dentro de FROOT (raiz) o de FBUF
 * (el cluster que contiene la entrada) y wlba y wn dicen que sectores hay que reescribir. Con make = 1 y sin existir,
 * deja en *ent una entrada libre en blanco (si el directorio esta lleno, enlaza un cluster nuevo; la FAT queda sucia). */
static int fw_find(u32 d, const u8 *nm, int make, u8 **ent, u32 *wlba, u32 *wn)
{
    u32 c = d, i, prev = 0, guard = 0, cur = 0, fc = 0;
    int fi = -1;
    *ent = 0;
    if (!d) {
        for (i = 0; i < fw.rootent; i++) {
            u8 *e = FROOT + 32 * i;
            if (e[0] == 0) { if (fi < 0) fi = (int)i; break; }
            if (e[0] == 0xE5) { if (fi < 0) fi = (int)i; continue; }
            if ((e[11] & 0x3F) == 0x0F || (e[11] & 8)) continue;
            if (eq11(e, nm)) { *ent = e; *wlba = fw.root_lba + i / 16; *wn = 1; return 1; }
        }
        if (make && fi >= 0) { *ent = FROOT + 32 * fi; *wlba = fw.root_lba + (u32)fi / 16; *wn = 1; memset(*ent, 0, 32); }
        return 0;
    }
    while (c >= 2 && c < fw.nclus + 2 && guard++ <= fw.nclus) {
        if (K->blk(fw.dev, 0, clus_lba(c), fw.spc, FBUF))
            return 0;
        cur = c;
        for (i = 0; i < fw.csize / 32; i++) {
            u8 *e = FBUF + 32 * i;
            if (e[0] == 0) { if (!fc) { fc = c; fi = (int)i; } goto done; }
            if (e[0] == 0xE5) { if (!fc) { fc = c; fi = (int)i; } continue; }
            if ((e[11] & 0x3F) == 0x0F || (e[11] & 8)) continue;
            if (eq11(e, nm)) { *ent = e; *wlba = clus_lba(c); *wn = fw.spc; return 1; }
        }
        prev = c;
        c = FATP[c];
        if (c >= 0xFFF8)
            break;
    }
done:
    if (!make)
        return 0;
    if (fc) {                                   /* hay una libre: asegurarse de tener ese cluster en FBUF */
        if (cur != fc && K->blk(fw.dev, 0, clus_lba(fc), fw.spc, FBUF))
            return 0;
        *ent = FBUF + 32 * fi; *wlba = clus_lba(fc); *wn = fw.spc;
        memset(*ent, 0, 32);
        return 0;
    }
    {                                           /* lleno: un cluster mas al final */
        u32 nc = prev ? fw_alloc(1) : 0;
        if (!nc)
            return 0;
        fat_set(prev, nc);
        memset(FBUF, 0, fw.csize);
        *ent = FBUF; *wlba = clus_lba(nc); *wn = fw.spc;
    }
    return 0;
}

static void put_entry(u8 *e, const u8 *nm, int attr, u32 first, u32 size)
{
    memset(e, 0, 32);
    memcpy(e, nm, 11);
    e[11] = (u8)attr;
    *(u16 *)(e + 14) = 0; *(u16 *)(e + 16) = 0x0021; *(u16 *)(e + 18) = 0x0021;
    *(u16 *)(e + 22) = 0; *(u16 *)(e + 24) = 0x0021;
    *(u16 *)(e + 26) = (u16)first; *(u32 *)(e + 28) = size;
}

/* devuelve el primer cluster de APPS (lo crea si hace falta); 0 = error */
static u32 fw_apps_dir(void)
{
    static const u8 nm[11] = { 'A', 'P', 'P', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    u8 *e; u32 lba, n, c;
    if (fw_find(0, nm, 0, &e, &lba, &n) && (e[11] & 0x10))
        return *(u16 *)(e + 26);
    if (!fw_find(0, nm, 1, &e, &lba, &n) && !e)
        return 0;
    c = fw_alloc(1);
    if (!c)
        return 0;
    memset(FBUF, 0, fw.csize);
    put_entry(FBUF, (const u8 *)".          ", 0x10, c, 0);
    put_entry(FBUF + 32, (const u8 *)"..         ", 0x10, 0, 0);
    if (fw_wr(clus_lba(c), fw.spc, FBUF))
        return 0;
    put_entry(e, nm, 0x10, c, 0);
    if (fw_flush_fat() || fw_wr(lba, n, (const u8 *)FROOT + (lba - fw.root_lba) * 512))
        return 0;
    return c;
}

/* guarda data[size] como HDn.1:/APPS/nombre. 0 = bien */
static int fw_save(const char *name, const u8 *data, u32 size)
{
    u8 nm[11], *e; u32 d, lba, n, first = 0, old = 0, nc, c, off;
    int found;
    if (!fw.ok || !name11(name, nm))
        return 1;
    d = fw_apps_dir();
    if (!d)
        return 2;
    found = fw_find(d, nm, 0, &e, &lba, &n);
    if (found)
        old = *(u16 *)(e + 26);
    nc = (size + fw.csize - 1) / fw.csize;
    if (nc) {
        first = fw_alloc(nc);
        if (!first)
            return 4;                           /* disco lleno */
        c = first; off = 0;
        while (nc) {
            u32 run = 1;
            while (run < nc && FATP[c + run - 1] == c + run) run++;
            if (fw_wr(clus_lba(c), run * fw.spc, data + off) ) return 5;
            off += run * fw.csize; nc -= run;
            if (nc) c = FATP[c + run - 1];
        }
    }
    found = fw_find(d, nm, 1, &e, &lba, &n);        /* una sola vez con make: un cluster nuevo de directorio aun no esta en disco */
    if (!e)
        return 3;
    put_entry(e, nm, 0x20, first, size);
    if (old) fw_free_chain(old);
    if (fw_flush_fat())
        return 6;
    if (lba >= fw.root_lba && lba < fw.root_lba + 32)
        return fw_wr(lba, n, (const u8 *)FROOT + (lba - fw.root_lba) * 512) ? 7 : 0;
    return fw_wr(lba, n, FBUF) ? 7 : 0;
}

static int fw_delete(const char *name)
{
    u8 nm[11], *e; u32 d, lba, n, first;
    if (!fw.ok || !name11(name, nm))
        return 1;
    d = fw_apps_dir();
    if (!d || !fw_find(d, nm, 0, &e, &lba, &n))
        return 2;
    first = *(u16 *)(e + 26);
    e[0] = 0xE5;
    if (fw_wr(lba, n, FBUF))
        return 3;
    fw_free_chain(first);
    return fw_flush_fat() ? 4 : 0;
}

/* ---- ejecutar NET.APP con una linea de comandos de iPXE ---- */
static void clear_mbox(void)
{
    struct mbox *m = (struct mbox *)(unsigned long)MBOX_ADDR;
    m->magic = 0; m->app_len = 0; m->list_len = 0;
}

static int run_net(const char *s)
{
    char *a = (char *)K->args;                  /* es el buffer net_args del kernel (160 bytes) */
    int i;
    for (i = 0; s[i] && i < 159; i++)
        a[i] = s[i];
    a[i] = 0;
    return K->exec("NET.APP");
}

static int build_cmd(char *dst, int max, const char *a, const char *b, const char *c, const char *d, const char *e)
{
    const char *p[5];
    int i, k = 0, j;
    p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = e;
    for (i = 0; i < 5; i++)
        for (j = 0; p[i] && p[i][j]; j++) {
            if (k >= max - 1)
                return 0;
            dst[k++] = p[i][j];
        }
    dst[k] = 0;
    return 1;
}

/* ---- lista de programas (texto del hub: "#HUB 1 n" y lineas "NOMBRE.BIN bytes sha256 descripcion") ---- */
#define MAXAPPS 400
struct app { char name[16]; u32 size; char sha[65]; char desc[56]; u8 hub, inst; };
struct dfile { char name[16]; u32 size; };
#define APPS_ADDR  0xB260000u           /* tabla de programas: 400 x 144 B. Fuera del modulo (solo hay 64 KB entre PKG.APP e INSTALL.APP) */
#define DISK_ADDR  0xB270000u           /* archivos que ya estan en el disco: 400 x 20 B */
#define apps ((struct app *)(unsigned long)APPS_ADDR)
static int napps;
static int truncated;                   /* el hub tiene mas de MAXAPPS programas */

/* ---- lo que ya esta guardado en el disco ---- */
#define disk ((struct dfile *)(unsigned long)DISK_ADDR)
static int ndisk;
static void disk_cb(const char *name, unsigned size, int isdir)
{
    int i;
    if (isdir || ndisk >= MAXAPPS || name[0] == '.')
        return;
    for (i = 0; name[i] && i < 15; i++)
        disk[ndisk].name[i] = name[i];
    disk[ndisk].name[i] = 0;
    disk[ndisk].size = size;
    ndisk++;
}
static void merge_disk(void)
{
    char spec[40];
    int i, j;
    ndisk = 0;
    if (!fw.ok)
        return;
    fw_spec(spec, 0);
    K->dir(spec, disk_cb);
    for (i = 0; i < ndisk; i++) {
        for (j = 0; j < napps; j++)
            if (streq(apps[j].name, disk[i].name)) { apps[j].inst = 1; break; }
        if (j == napps && napps < MAXAPPS) {
            int k;
            for (k = 0; disk[i].name[k]; k++) apps[napps].name[k] = disk[i].name[k];
            apps[napps].name[k] = 0;
            apps[napps].size = disk[i].size; apps[napps].sha[0] = 0;
            { const char *t = "(solo en el disco)"; for (k = 0; t[k]; k++) apps[napps].desc[k] = t[k]; apps[napps].desc[k] = 0; }
            apps[napps].hub = 0; apps[napps].inst = 1;
            napps++;
        }
    }
}


static void parse_list(void)
{
    const char *p = (const char *)(unsigned long)LIST_ADDR;
    const struct mbox *m = (const struct mbox *)(unsigned long)MBOX_ADDR;
    const char *end = p + m->list_len;
    napps = 0;
    truncated = 0;
    while (p < end) {
        if (napps >= MAXAPPS) { truncated = 1; break; }
        const char *e = p;
        int i, k;
        u32 v = 0;
        while (e < end && *e != '\n')
            e++;
        if (*p != '#' && e > p) {
            for (k = 0; p < e && *p != ' ' && k < 15; )
                apps[napps].name[k++] = *p++;
            apps[napps].name[k] = 0;
            while (p < e && *p == ' ')
                p++;
            while (p < e && *p >= '0' && *p <= '9')
                v = v * 10 + (u32)(*p++ - '0');
            apps[napps].size = v;
            while (p < e && *p == ' ')
                p++;
            for (k = 0; p < e && *p != ' ' && k < 64; )
                apps[napps].sha[k++] = *p++;
            apps[napps].sha[k] = 0;
            while (p < e && *p == ' ')
                p++;
            for (i = 0; p < e && *p != '\r' && i < (int)sizeof apps[0].desc - 1; )
                apps[napps].desc[i++] = *p++;
            apps[napps].desc[i] = 0;
            apps[napps].hub = 1; apps[napps].inst = 0;
            if (k == 64 && apps[napps].name[0])
                napps++;
        }
        p = e + 1;
    }
}

static int fetch_list(void)
{
    char cmd[160];
    const struct mbox *m = (const struct mbox *)(unsigned long)MBOX_ADDR;
    pr("\nBajando la lista de "); pr(hub); pr(" ...\n");
    clear_mbox();
    if (!build_cmd(cmd, sizeof cmd, "dhcp;imgfetch -n hublist ", hub, "/list;bosstash hublist list", 0, 0)) {
        pr("PKG: la direccion del hub es demasiado larga\n");
        return 0;
    }
    run_net(cmd);
    if (m->magic != MBOX_MAGIC || m->list_len == 0) {
        pr("\nPKG: no pude bajar la lista. Revisa el cable de red, la direccion del hub (HUB.CFG)\n"
           "y que el hub este despierto (en Render gratis tarda ~1 minuto la primera vez).\n");
        return 0;
    }
    parse_list();
    return 1;
}

/* ---- buscar: filtro por texto (F), por etiqueta (T) y paginas (N/A) ----
 * La etiqueta es lo que va entre corchetes al principio de la descripcion: "[juego] Pong de dos jugadores". */
#define PER_PAGE 12
static char filt[24];                           /* filtro actual en minusculas ("" = sin filtro) */
static int filt_tag;                            /* 1 = filt es una etiqueta exacta, 0 = texto dentro del nombre o la descripcion */
static int page;
static int view[MAXAPPS], nview;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int has_ci(const char *hay, const char *needle)
{
    int i, j;
    if (!needle[0])
        return 1;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j] && hay[i + j] && lc(hay[i + j]) == needle[j]; j++)
            ;
        if (!needle[j])
            return 1;
    }
    return 0;
}

/* etiqueta de una descripcion en minusculas; devuelve 1 si tiene */
static int get_tag(const char *desc, char *out, int max)
{
    int k = 0;
    while (*desc == ' ')
        desc++;
    if (*desc != '[')
        return 0;
    desc++;
    while (*desc && *desc != ']' && k < max - 1)
        out[k++] = lc(*desc++);
    out[k] = 0;
    return *desc == ']' && k > 0;
}

static void build_view(void)
{
    int i, pages;
    char tag[20];
    nview = 0;
    for (i = 0; i < napps; i++) {
        int ok;
        if (!filt[0]) ok = 1;
        else if (filt_tag) ok = get_tag(apps[i].desc, tag, sizeof tag) && streq(tag, filt);
        else ok = has_ci(apps[i].name, filt) || has_ci(apps[i].desc, filt);
        if (ok)
            view[nview++] = i;
    }
    pages = (nview + PER_PAGE - 1) / PER_PAGE;
    if (page >= pages) page = pages - 1;
    if (page < 0) page = 0;
}

static void set_filter(const char *arg, int tag)
{
    int k = 0;
    while (*arg == ' ')
        arg++;
    while (arg[k] && k < (int)sizeof filt - 1) { filt[k] = lc(arg[k]); k++; }
    while (k > 0 && filt[k - 1] == ' ') k--;
    filt[k] = 0;
    filt_tag = tag && k > 0;
    page = 0;
}

static void show_tags(void)
{
    static char tg[32][20];
    static int cnt[32];
    int i, j, nt = 0, none = 0;
    char t[20];
    for (i = 0; i < napps; i++) {
        if (!get_tag(apps[i].desc, t, sizeof t)) { none++; continue; }
        for (j = 0; j < nt && !streq(tg[j], t); j++)
            ;
        if (j == nt) {
            if (nt >= 32) continue;
            { int k; for (k = 0; t[k]; k++) tg[nt][k] = t[k]; tg[nt][k] = 0; }
            cnt[nt++] = 0;
        }
        cnt[j]++;
    }
    if (!nt) {
        pr("\nNingun programa tiene etiqueta todavia. Al subir, empieza la descripcion con [etiqueta], por ejemplo: [juego] Mi juego.\n");
        return;
    }
    pr("\nEtiquetas:\n");
    for (j = 0; j < nt; j++) {
        pr("  "); pr(tg[j]); pr(" ("); prnum((unsigned)cnt[j]); pr(")");
        if (j % 4 == 3) pr("\n");
    }
    if (nt % 4) pr("\n");
    if (none) { pr("  sin etiqueta: "); prnum((unsigned)none); pr("\n"); }
    pr("T etiqueta = ver solo esa   (por ejemplo: T "); pr(tg[0]); pr(")\n");
}

static void show_list(void)
{
    int i, pages;
    build_view();
    pages = (nview + PER_PAGE - 1) / PER_PAGE;
    pr("\n=== BASIC-OS: ");
    if (filt[0]) { prnum((unsigned)nview); pr(" de "); }
    prnum((unsigned)napps); pr(" programas");
    if (filt[0]) { pr(filt_tag ? "  etiqueta: " : "  buscando: "); pr(filt); }
    if (pages > 1) { pr("  pagina "); prnum((unsigned)page + 1); pr("/"); prnum((unsigned)pages); }
    pr(" ===\n");
    if (!nview)
        pr("Nada coincide. F (solo) quita el filtro.\n");
    for (i = page * PER_PAGE; i < nview && i < (page + 1) * PER_PAGE; i++) {
        const struct app *a = &apps[view[i]];
        prnum((unsigned)view[i] + 1); pr(") ");
        pr(a->name); pr(a->inst ? " [en disco]  " : "  "); prsize(a->size);
        if (a->desc[0]) { pr("  - "); pr(a->desc); }
        pr("\n");
    }
    if (truncated) { pr("(el hub tiene mas de "); prnum(MAXAPPS); pr(" programas: solo se ven los primeros)\n"); }
    pr("\nNumero = ejecutar (del disco si ya esta, si no se baja y se guarda)   +N = bajar otra vez   -N = borrar del disco\n"
       "F texto = buscar   T = etiquetas   T etiqueta = solo esa   flechas (o N / A) = cambiar de pagina\n"
       "S = subir un paquete   C = compilar un .BAS a .BIN   R = recargar   U = hub   Q = salir\n");
}

/* leer una linea corta con eco; devuelve la longitud, -1 si ESC.
 * Con arrows != 0 (solo el menu principal) una flecha con la linea vacia devuelve -2 y deja en arrow_dir +1 (derecha/abajo) o -1 (izquierda/arriba). */
static int arrows, arrow_dir;
static int readline(char *buf, int max, int invert)
{
    int len = 0;
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); continue; }
        if (arrows && len == 0 && (c == KEY_RIGHT || c == KEY_DOWN || c == KEY_LEFT || c == KEY_UP)) {
            arrow_dir = (c == KEY_RIGHT || c == KEY_DOWN) ? 1 : -1;
            buf[0] = 0;
            return -2;
        }
        if (invert) {                           /* BASIC-OS entrega MAYUSCULAS por defecto: sin Shift = minusculas, como en NET */
            if (c >= 'a' && c <= 'z') c -= 32;
            else if (c >= 'A' && c <= 'Z') c += 32;
        }
        if (c == 27) return -1;
        if (c == '\r' || c == '\n') { pr("\n"); buf[len] = 0; return len; }
        if ((c == 8 || c == 127 || c == 20) && len) { len--; pr("\b \b"); continue; }
        if (c >= 32 && c < 127 && len < max - 1) {
            char s[2];
            buf[len++] = (char)c;
            s[0] = (char)c; s[1] = 0;
            pr(s);
        }
    }
}

static int is_bas(const char *name)
{
    int n = (int)strlen(name);
    return n > 4 && name[n - 4] == '.' && (name[n - 3] | 32) == 'b' && (name[n - 2] | 32) == 'a' && (name[n - 1] | 32) == 's';
}

static int bas_loaded;                          /* 1 = se cargo un .BAS: PKG sale para que el usuario escriba RUN */
static int win_opened;                          /* 1 = el programa abrio una ventana: PKG sale y el escritorio la muestra */

/* ventana de un programa de la tienda: la struct wapp se copia a memoria fija (binwin.h) y su stop() pasa por aqui para
 * apagar el aviso cuando se cierre. Original en spare[0..127], la que ve el escritorio en spare[128..255]. */
#define BW_ORIG ((struct wapp *)(unsigned long)(BINWIN_ADDR + 24))
#define BW_PUB  ((struct wapp *)(unsigned long)(BINWIN_ADDR + 24 + 128))
static void bw_stop(struct wctx *c)
{
    if (BW_ORIG->stop)
        BW_ORIG->stop(c);
    binwin_clear();
}
static void bw_open(const char *name, const struct wapp *a)
{
    int i;
    memcpy(BW_ORIG, a, sizeof *a);
    memcpy(BW_PUB, a, sizeof *a);
    BW_PUB->stop = bw_stop;
    for (i = 0; name[i] && i < 15; i++)
        BINWIN->name[i] = name[i];
    BINWIN->name[i] = 0;
    BINWIN->magic = BINWIN_MAGIC;
    BINWIN->check = ~BINWIN_MAGIC;
    K->wm_publish(BW_PUB);
    win_opened = 1;
}

static int launch(const char *name, u32 n)
{
    const struct hdr *h = (const struct hdr *)(unsigned long)APP_BASE;
    int (*entry)(const struct kapi *);
    if (is_bas(name)) {
        K->load_basic((const char *)(unsigned long)APP_BASE, (int)n);
        pr("Programa BASIC "); pr(name); pr(" cargado. Escribe RUN (o LIST para verlo; SAVE para guardarlo).\n");
        bas_loaded = 1;
        return 1;
    }
    if (n >= sizeof *h && h->magic == 0x4D424350u && h->base > APP_BASE && h->base < APP_BASE + APP_MAX) {
        pr("(PKG) "); pr(name); pr(" es un complemento (plugin) de otro programa: ya quedo guardado en el disco y ese programa lo usa solo.\n");
        return 1;
    }
    if (n < sizeof *h || h->magic != 0x4D424350u || h->base != APP_BASE || h->end < APP_BASE + n ||
        h->end > APP_BASE + APP_MAX || h->entry < APP_BASE || h->entry >= APP_BASE + n) {
        pr("PKG: no es un modulo de BASIC-OS valido para la tienda (debe enlazarse a 0x9000000)\n");
        return 0;
    }
    memset((void *)(unsigned long)(APP_BASE + n), 0, h->end - (APP_BASE + n));
    pr("Ejecutando "); pr(name); pr(" ...\n\n");
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    if (entry(K) == MOD_WINAPP) {
        const struct wapp *a = K->wm_take();
        if (a) {
            bw_open(name, a);
            pr("(PKG) "); pr(name); pr(" abre una ventana en el escritorio.\n");
            return 1;
        }
    }
    pr("\n(PKG) el programa termino.\n");
    return 1;
}

static int run_disk(int i)
{
    char sp[40], sum[65];
    unsigned total = 0;
    int n;
    fw_spec(sp, apps[i].name);
    pr("\nCargando "); pr(sp); pr(" del disco ...\n");
    n = K->read(sp, (char *)(unsigned long)APP_BASE, (int)APP_MAX, &total);
    if (n <= 0 || (unsigned)n != total) {
        pr("PKG: no pude leerlo del disco.\n");
        return 0;
    }
    if (apps[i].hub && apps[i].sha[0]) {
        sha256_hex((const u8 *)(unsigned long)APP_BASE, (u32)n, sum);
        if (!streq(sum, apps[i].sha))
            pr("(aviso: es distinto al del hub; usa +numero para actualizarlo)\n");
    }
    return launch(apps[i].name, (u32)n);
}

static int run_app(int i)
{
    char cmd[160], sum[65];
    const struct mbox *m = (const struct mbox *)(unsigned long)MBOX_ADDR;
    u32 n;

    pr("\nBajando "); pr(apps[i].name); pr(" ...\n");
    clear_mbox();
    if (!build_cmd(cmd, sizeof cmd, "dhcp;imgfetch -n app ", hub, "/get/", apps[i].name, ";bosstash app app")) {
        pr("PKG: la linea de comandos es demasiado larga (acorta la direccion del hub)\n");
        return 0;
    }
    run_net(cmd);
    if (m->magic != MBOX_MAGIC || m->app_len == 0) {
        pr("\nPKG: la descarga fallo.\n");
        return 0;
    }
    n = m->app_len;
    if (n != apps[i].size) {
        pr("\nPKG: el tamano no coincide con la lista (descarga cortada). No se ejecuta.\n");
        return 0;
    }
    sha256_hex((const u8 *)(unsigned long)APP_BASE, n, sum);
    if (!streq(sum, apps[i].sha)) {
        pr("\nPKG: la suma SHA-256 no coincide con la del hub. No se ejecuta.\n");
        return 0;
    }
    pr("SHA-256 correcto: "); pr(sum); pr("\n");
    if (fw.ok) {
        int r = fw_save(apps[i].name, (const u8 *)(unsigned long)APP_BASE, n);
        if (r) { pr("PKG: no pude guardarlo en el disco (codigo "); prnum((unsigned)r); pr("); se ejecuta desde la RAM.\n"); }
        else { char sp[40]; fw_spec(sp, apps[i].name); pr("Guardado en "); pr(sp); pr("\n"); apps[i].inst = 1; }
    }
    return launch(apps[i].name, n);
}

/* ---- subir un paquete al hub ---- */
#define POST_MAX 0x200000u                      /* lo que acepta bospost (el heap de iPXE es de 4 MB) */
#define POST_DATA (APP_BASE + 4096u)            /* el archivo se lee aqui y luego se pega detras de la cabecera */

static int name_ok(const char *n)              /* NOMBRE.BIN / NOMBRE.BAS, 8.3 */
{
    int i = 0;
    while (n[i] && n[i] != '.') {
        char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~') || i >= 8)
            return 0;
        i++;
    }
    if (i == 0 || n[i] != '.')
        return 0;
    return streq(n + i, ".BIN") || streq(n + i, ".BAS");
}

static void up_name(char *dst, const char *src, const char *ext)   /* nombre 8.3 en mayusculas desde cualquier texto */
{
    int k = 0, i;
    for (i = 0; src[i] && src[i] != '.' && k < 8; i++) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~')) c = '_';
        dst[k++] = c;
    }
    if (!k) dst[k++] = 'X';
    while (*ext) dst[k++] = *ext++;
    dst[k] = 0;
}

static void upload(void)
{
    char what[64], name[16], desc[64], key[64], cmd[160];
    struct mbox *m = (struct mbox *)(unsigned long)MBOX_ADDR;
    u8 *out = (u8 *)(unsigned long)APP_BASE, *src = (u8 *)(unsigned long)POST_DATA;
    unsigned total = 0;
    int n, i, hl, colon = 0, last = 0;
    char sum[65];

    if (!hub[0]) { pr("Primero pon la direccion del hub (U).\n"); return; }
    pr("\nSUBIR AL HUB. Que quieres subir?\n"
       "  NOMBRE              un programa guardado con SAVE (se sube como NOMBRE.BAS)\n"
       "  HD0.1:RUTA/X.BIN    un archivo de la particion (.BIN o .BAS)\n> ");
    n = readline(what, sizeof what, 0);
    if (n <= 0)
        return;
    for (i = 0; what[i]; i++)
        if (what[i] == ':') colon = 1;
    if (colon) {
        const char *b, *dot = 0;
        int j;
        n = K->read(what, (char *)src, (int)(POST_MAX - 4096u + 1), &total);
        if (n < 0) { pr("PKG: no pude leer ese archivo.\n"); return; }
        if (total > POST_MAX - 4096u) { pr("PKG: el archivo pasa de 2 MB.\n"); return; }
        for (i = 0; what[i]; i++)
            if (what[i] == ':' || what[i] == '/' || what[i] == '\\') last = i + 1;
        b = what + last;
        for (j = 0; b[j]; j++)
            if (b[j] == '.') dot = b + j;
        up_name(name, b, (dot && (dot[1] | 32) == 'b' && (dot[2] | 32) == 'a' && (dot[3] | 32) == 's') ? ".BAS" : ".BIN");
    } else {
        n = K->saved(what, (char *)src, (int)(POST_MAX - 4096u));
        if (n < 0) { pr("PKG: no hay un programa guardado con ese nombre (mira DIR).\n"); return; }
        up_name(name, what, ".BAS");
    }
    if (n == 0) { pr("PKG: esta vacio.\n"); return; }
    if (!is_bas(name)) {
        const struct hdr *h = (const struct hdr *)src;
        if ((u32)n < sizeof *h || h->magic != 0x4D424350u || h->base != APP_BASE) {
            pr("PKG: un .BIN de la tienda debe ser un modulo PCBM enlazado a 0x9000000 (mira apps/hello.c).\n");
            return;
        }
    }
    pr("Nombre en el hub ["); pr(name); pr("]: ");
    i = readline(what, 13, 0);
    if (i < 0) return;
    if (i > 0) {
        for (i = 0; what[i]; i++) if (what[i] >= 'a' && what[i] <= 'z') what[i] -= 32;
        if (!name_ok(what)) { pr("PKG: el nombre debe ser 8.3 terminado en .BIN o .BAS\n"); return; }
        for (i = 0; what[i]; i++) name[i] = what[i];
        name[i] = 0;
    }
    pr("Descripcion (una linea, mejor con [etiqueta] al inicio: [juego] Mi juego; Enter = ninguna): ");
    if (readline(desc, 56, 0) < 0) return;
    if (hubkey[0]) {
        for (i = 0; hubkey[i]; i++) key[i] = hubkey[i];
        key[i] = 0;
    } else {
        pr("Clave del hub (HUB_KEY; ponla en la 2a linea de HUB.CFG para no escribirla): ");
        if (readline(key, sizeof key, 1) <= 0) return;
    }
    /* cuerpo: "BOSUP 1\nNOMBRE\nCLAVE\nDESCRIPCION\n" + datos */
    hl = 0;
    {   const char *parts[4];
        int j, q;
        parts[0] = "BOSUP 1"; parts[1] = name; parts[2] = key; parts[3] = desc;
        for (j = 0; j < 4; j++) {
            for (q = 0; parts[j][q]; q++) out[hl++] = (u8)parts[j][q];
            out[hl++] = '\n';
        } }
    for (i = 0; i < n; i++)
        out[hl + i] = src[i];
    sha256_hex(out + hl, (u32)n, sum);
    pr("Subiendo "); pr(name); pr(" ("); prsize((unsigned)n); pr(") a "); pr(hub); pr(" ...\n");
    m->magic = MBOX_MAGIC; m->app_len = 0; m->list_len = 0; m->spare = (u32)(hl + n);
    if (!build_cmd(cmd, sizeof cmd, "dhcp;bospost ", hub, "/upload", 0, 0)) {
        pr("PKG: la direccion del hub es demasiado larga\n");
        return;
    }
    run_net(cmd);
    if (m->magic != MBOX_MAGIC || m->list_len == 0) {
        pr("\nPKG: la subida fallo (clave equivocada, hub dormido o sin red).\n");
        return;
    }
    {   /* respuesta del hub: "OK NOMBRE bytes sha256" o un mensaje de error */
        const char *r = (const char *)(unsigned long)LIST_ADDR;
        char line[120];
        u32 j;
        int ok = 0;
        for (j = 0; j < m->list_len && j < sizeof line - 1 && r[j] != '\n'; j++) line[j] = r[j];
        line[j] = 0;
        pr("Hub: "); pr(line); pr("\n");
        if (line[0] == 'O' && line[1] == 'K') {
            for (j = 0; line[j]; j++)
                if (streq_n(line + j, sum, 64)) ok = 1;
            pr(ok ? "Subido y comprobado (SHA-256 igual).\n" : "Subido, pero el hub reporta otra suma: revisa.\n");
        }
    }
}

/* ---- compilar un .BAS a .BIN con bas2bin ---- */
#define B2B_SRC  0xA000000u                     /* texto del programa (64 KB) */
#define B2B_OUT  0xA100000u                     /* el .BIN compilado (hasta 14 MB) */
#define B2B_OMAX 0xE00000u

static void compile_bas(void)
{
    char what[64], name[16], msg[120];
#ifndef B2B_NO_NATIVE
    char mode[8];
#endif
    char *src = (char *)(unsigned long)B2B_SRC;
    unsigned char *outb = (unsigned char *)(unsigned long)B2B_OUT;
    unsigned total = 0;
    int n, i, colon = 0, last = 0, nat = 1, r;

    pr("\nCOMPILAR A .BIN. Que programa?\n"
       "  NOMBRE              uno guardado con SAVE\n"
       "  HD0.1:RUTA/X.BAS    un archivo de la particion\n> ");
    if (readline(what, sizeof what, 0) <= 0)
        return;
    for (i = 0; what[i]; i++)
        if (what[i] == ':') colon = 1;
    if (colon) {
        n = K->read(what, src, 65535, &total);
        if (n < 0) { pr("PKG: no pude leer ese archivo.\n"); return; }
        if (total > 65535) { pr("PKG: el programa es demasiado grande.\n"); return; }
        for (i = 0; what[i]; i++)
            if (what[i] == ':' || what[i] == '/' || what[i] == '\\') last = i + 1;
        up_name(name, what + last, ".BIN");
    } else {
        n = K->saved(what, src, 65535);
        if (n < 0) { pr("PKG: no hay un programa guardado con ese nombre (mira DIR).\n"); return; }
        up_name(name, what, ".BIN");
    }
#ifdef B2B_NO_NATIVE
    pr("(PC BASIC de 32 bits: se empaqueta, el modo nativo es solo de 64 bits)\n");
    nat = 0;
#else
    pr("Modo:  N = nativo (codigo x86-64, mucho mas rapido)   P = empaquetado (todo el BASIC)  [N]: ");
    if (readline(mode, sizeof mode, 0) < 0) return;
    nat = !(mode[0] == 'P' || mode[0] == 'p');
#endif
    pr("Compilando "); pr(what); pr(nat ? " (nativo) ...\n" : " (empaquetado) ...\n");
    r = b2b_compile(src, n, nat, outb, (int)B2B_OMAX, msg, (int)sizeof msg);
    if (r < 0) {
        pr("bas2bin: "); pr(msg); pr("\n");
        if (nat) pr("(si usa algo que el compilador no entiende, prueba el modo P)\n");
        return;
    }
    pr("Listo: "); pr(name); pr(" ("); prsize((unsigned)r); pr(")\n");
    if (fw.ok) {
        char sp[40];
        int e = fw_save(name, outb, (u32)r);
        if (e) { pr("PKG: no pude guardarlo en el disco (codigo "); prnum((unsigned)e); pr(")\n"); return; }
        fw_spec(sp, name);
        pr("Guardado en "); pr(sp); pr(". Corre con su numero en la lista; subelo con S y esa ruta.\n");
    } else {
        pr("(no hay disco FAT16 de la BIOS para guardarlo: se ejecuta desde la RAM)\n");
        memcpy((void *)(unsigned long)APP_BASE, outb, (unsigned long)r);
        launch(name, (u32)r);
    }
}

static void ask_hub(void)
{
    char buf[96];
    int n, k;
    pr("\nDireccion del hub (por ejemplo https://mi-hub.onrender.com), Enter = no cambiar:\n> ");
    n = readline(buf, sizeof buf, 1);
    if (n <= 0)
        return;
    for (k = 0; k < n && k < (int)sizeof hub - 1; k++)
        hub[k] = buf[k];
    while (k > 0 && hub[k - 1] == '/')
        k--;
    hub[k] = 0;
}

int mod_main(const struct kapi *k)
{
    char line[40];
    int loaded = 0;
    K = k;
    pr("\nBASIC-OS PKG: tienda de programas por HTTPS\n");
    if (ram_end() < NEED_RAM) {
        pr("PKG necesita ~180 MB de RAM (la tienda usa memoria alta). En QEMU: -m 256\n");
        return 1;
    }
    {   const char *b = binwin_busy(ram_end());
        if (b) {
            pr("\nPKG: "); pr(b); pr(" tiene una ventana abierta en el escritorio y usa la memoria de la tienda.\n"
               "Cierrala primero (o sal del escritorio) y vuelve a abrir PKG.\n");
            return 1;
        } }
    load_cfg();
    if (!hub[0]) {
        pr("\nNo hay HUB.CFG en el disco: escribe la direccion de tu hub.\n");
        ask_hub();
    }
    fw_init();
    if (hub[0])
        loaded = fetch_list();
    if (!loaded)
        napps = 0;
    merge_disk();
    if (fw.ok) { pr("Disco de programas: "); pr(fw.dev); pr(".1:/APPS/\n"); }
    else pr("(el disco de arranque no es FAT16 de la BIOS: los programas viven solo en RAM; usa INSTALL)\n");
    for (;;) {
        int n, i, v = 0, force = 0, del = 0;
        char *p;
        if (napps)
            show_list();
        else
            pr("\nU = direccion del hub   S = subir un paquete   C = compilar un .BAS   R = reintentar   Q = salir\n");
        pr("> ");
        arrows = 1;
        n = readline(line, sizeof line, 0);
        arrows = 0;
        if (n == -2) { page += arrow_dir; if (page < 0) page = 0; pr("\n"); continue; }
        if (n < 0 || line[0] == 'q' || line[0] == 'Q' || line[0] == 0)
            break;
        if ((line[0] == 'f' || line[0] == 'F' || line[0] == '/') && (line[1] == 0 || line[1] == ' ' || line[0] == '/')) {
            set_filter(line + 1, 0);
            continue;
        }
        if ((line[0] == 't' || line[0] == 'T') && (line[1] == 0 || line[1] == ' ')) {
            if (line[1] == ' ' && line[2]) set_filter(line + 1, 1);
            else { show_tags(); }
            continue;
        }
        if ((line[0] == 'n' || line[0] == 'N') && line[1] == 0) { page++; continue; }
        if ((line[0] == 'a' || line[0] == 'A') && line[1] == 0) { if (page > 0) page--; continue; }
        if (line[0] == 'u' || line[0] == 'U') {
            ask_hub();
            loaded = hub[0] ? fetch_list() : 0;
            if (!loaded) napps = 0;
            merge_disk();
            continue;
        }
        if (line[0] == 'c' || line[0] == 'C') {
            compile_bas();
            if (bas_loaded)
                break;
            merge_disk();
            continue;
        }
        if (line[0] == 's' || line[0] == 'S') {
            upload();
            loaded = hub[0] ? fetch_list() : 0;
            if (!loaded) napps = 0;
            merge_disk();
            continue;
        }
        if (line[0] == 'r' || line[0] == 'R') {
            loaded = hub[0] ? fetch_list() : 0;
            if (!loaded) napps = 0;
            merge_disk();
            continue;
        }
        p = line;
        if (*p == '+') { force = 1; p++; }
        else if (*p == '-') { del = 1; p++; }
        for (i = 0; p[i] >= '0' && p[i] <= '9'; i++)
            v = v * 10 + (p[i] - '0');
        if (i == 0 || v < 1 || v > napps) {
            pr("Numero no valido\n");
            continue;
        }
        v--;
        if (del) {
            if (!apps[v].inst) { pr("No esta en el disco.\n"); continue; }
            if (fw_delete(apps[v].name)) pr("PKG: no pude borrarlo.\n");
            else { pr("Borrado de "); pr(apps[v].name); pr("\n"); apps[v].inst = 0;
                   if (!apps[v].hub) { int j; for (j = v; j + 1 < napps; j++) apps[j] = apps[j + 1]; napps--; } }
            continue;
        }
        if (apps[v].inst && !force)
            run_disk(v);
        else if (apps[v].hub && loaded)
            run_app(v);
        else
            pr("Ese programa solo existe en el disco, o no hay conexion con el hub.\n");
        if (bas_loaded)
            break;
        if (win_opened)
            return MOD_WINAPP;
    }
    return 0;
}
