/* pkg.c - PKG.APP: la "tienda" de BASIC-OS. Baja programas de un hub (hub/server.py en Render) por HTTPS y los ejecuta.
 *
 * Flujo:  PKG.APP -> NET.APP (iPXE: dhcp + imgfetch https://HUB/list + bosstash) -> memoria fija -> menu en pantalla
 *         -> elegir uno -> NET.APP otra vez (imgfetch https://HUB/get/NOMBRE + bosstash) -> comprobar SHA-256 y cabecera
 *         PCBM -> saltar a su mod_main.
 * Los programas de la tienda son modulos PCBM enlazados a 0x9000000 (hasta 32 MB). El SO todavia no escribe en disco,
 * asi que viven en RAM mientras corren; para tenerlos otra vez hay que volver a bajarlos.
 * Direccion del hub: primera linea de HUB.CFG en la particion FAT (HD0.1:HUB.CFG ...) o la que escribas con U.
 * No hay direccion por defecto a proposito: PKG ejecuta lo que baja, asi que solo habla con el hub que tu pongas.
 * Uso:  PKG   (o EXEC "PKG.APP")     Teclas: numero + Enter = ejecutar, R = recargar, U = direccion del hub, Q/ESC = salir.
 */
#include "kapi.h"
#include "mod_head.h"

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
#define NEED_RAM   0xB200000u           /* hay que tener RAM hasta aqui: ~178 MB (usa -m 256 en QEMU) */
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
static char hub[96];

static void load_cfg(void)
{
    static const char *const where[] = {
        "HD0.1:HUB.CFG", "HD1.1:HUB.CFG", "HD2.1:HUB.CFG", "HD0:HUB.CFG", "FD1:HUB.CFG", "USB0.1:HUB.CFG", 0
    };
    char buf[96];
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
            if (k > 8)
                return;
        }
    }
    hub[0] = 0;
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
#define MAXAPPS 60
static struct { char name[16]; u32 size; char sha[65]; char desc[56]; } apps[MAXAPPS];
static int napps;

static void parse_list(void)
{
    const char *p = (const char *)(unsigned long)LIST_ADDR;
    const struct mbox *m = (const struct mbox *)(unsigned long)MBOX_ADDR;
    const char *end = p + m->list_len;
    napps = 0;
    while (p < end && napps < MAXAPPS) {
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

static void show_list(void)
{
    int i;
    pr("\n=== BASIC-OS HUB: "); prnum((unsigned)napps); pr(" programas ===\n");
    for (i = 0; i < napps; i++) {
        prnum((unsigned)i + 1); pr(") ");
        pr(apps[i].name); pr("  "); prsize(apps[i].size);
        if (apps[i].desc[0]) { pr("  - "); pr(apps[i].desc); }
        pr("\n");
    }
    pr("\nNumero + Enter = descargar y ejecutar   R = recargar   Q = salir\n");
}

/* leer una linea corta con eco; devuelve la longitud, -1 si ESC */
static int readline(char *buf, int max, int invert)
{
    int len = 0;
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); continue; }
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

static int run_app(int i)
{
    char cmd[160], sum[65];
    const struct mbox *m = (const struct mbox *)(unsigned long)MBOX_ADDR;
    const struct hdr *h = (const struct hdr *)(unsigned long)APP_BASE;
    int (*entry)(const struct kapi *);
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
    if (n < sizeof *h || h->magic != 0x4D424350u || h->base != APP_BASE || h->end < APP_BASE + n ||
        h->end > APP_BASE + APP_MAX || h->entry < APP_BASE || h->entry >= APP_BASE + n) {
        pr("PKG: no es un modulo de BASIC-OS valido para la tienda (debe enlazarse a 0x9000000)\n");
        return 0;
    }
    memset((void *)(unsigned long)(APP_BASE + n), 0, h->end - (APP_BASE + n));
    pr("Ejecutando "); pr(apps[i].name); pr(" ...\n\n");
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    entry(K);
    pr("\n(PKG) el programa termino.\n");
    return 1;
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
    char line[16];
    int loaded = 0;
    K = k;
    pr("\nBASIC-OS PKG: tienda de programas por HTTPS\n");
    if (ram_end() < NEED_RAM) {
        pr("PKG necesita ~180 MB de RAM (la tienda usa memoria alta). En QEMU: -m 256\n");
        return 1;
    }
    load_cfg();
    if (!hub[0]) {
        pr("\nNo hay HUB.CFG en el disco: escribe la direccion de tu hub.\n");
        ask_hub();
    }
    if (hub[0])
        loaded = fetch_list();
    for (;;) {
        int n, i, v = 0;
        if (loaded)
            show_list();
        else
            pr("\nU = direccion del hub   R = reintentar   Q = salir\n");
        pr("> ");
        n = readline(line, sizeof line, 0);
        if (n < 0 || line[0] == 'q' || line[0] == 'Q' || line[0] == 0)
            break;
        if (line[0] == 'u' || line[0] == 'U') {
            ask_hub();
            loaded = hub[0] ? fetch_list() : 0;
            continue;
        }
        if (line[0] == 'r' || line[0] == 'R') {
            loaded = hub[0] ? fetch_list() : 0;
            continue;
        }
        if (!loaded)
            continue;
        for (i = 0; line[i] >= '0' && line[i] <= '9'; i++)
            v = v * 10 + (line[i] - '0');
        if (i == 0 || v < 1 || v > napps) {
            pr("Numero no valido\n");
            continue;
        }
        run_app(v - 1);
    }
    return 0;
}
