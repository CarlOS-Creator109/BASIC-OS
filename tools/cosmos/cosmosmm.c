/* cosmosmm.c - COSMOS: navegador grafico minimo de BASIC-OS con el Minimal Motor (MM) de Cosmos Browser
 *
 * El motor (html.c = HTML + CSS, layout.c = maquetado) es el mismo de Cosmos Browser para Linux, sin cambios.
 * Aqui va lo que en Linux hacen gfx.c / net.c / main.c:
 *   - texto con la fuente 8x16 del sistema (CP437), escalada x2/x3 para titulos grandes; negrita = doble trazo
 *   - colores 0xRRGGBB -> los 16 del C64 (el mas cercano), con correccion de contraste para el texto
 *   - la red: carga NET.BIN (iPXE) por su cuenta y le pasa  dhcp;imgfetch;bosstash  (la URL va troceada en
 *     variables de iPXE, asi no hay limite de 160 letras); la salida de iPXE se guarda para la pagina de error
 *   - archivos locales:  HD0.1:PAGINA.HTM, USB0.1:... (con K->read)
 *   - ventana del escritorio: barra de direccion, atras/adelante, recargar, inicio, scroll y barra de estado
 * Sin JavaScript ni imagenes (por ahora). Se enlaza a 0x7000000 como COSMOS.BIN (va en la particion FAT,
 * no cabe en /SYS) y lo carga COSMOS.APP (sys/cosmosapp.c). GPL-3.0-or-later.
 */
#include "cosmos.h"
#include "kapi.h"
#include "mod_head.h"
#include "sys/binwin.h"
#include <string.h>
#include <stdio.h>

typedef unsigned char u8;

void mm_heap_reset(void);
unsigned long mm_heap_used(void);
extern int mm_oom;
void url_resolve(const char *base, const char *ref, char *out, size_t cap);

static const struct kapi *K;

/* ================= SSE: el motor usa float; el puente a la BIOS (discos) apaga CR4.OSFXSR ================= */
static inline __attribute__((always_inline)) void sse_on(void)
{
    unsigned long c;
    unsigned mx = 0x9F80;
    __asm__ volatile("mov %%cr0, %0" : "=r"(c));
    c &= ~(1ul << 2);
    c &= ~(1ul << 3);
    c |= (1ul << 1);
    __asm__ volatile("mov %0, %%cr0" : : "r"(c) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(c));
    c |= (1ul << 9) | (1ul << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(c) : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mx));
}
static int kread(const char *spec, char *buf, int max, unsigned *total)
{
    int r = K->read(spec, buf, max, total);
    sse_on();
    return r;
}

/* ================= lo que en Linux hace gfx.c ================= */
struct ABlk { ABlk *next; size_t used, cap; char data[]; };
#define ABLK_CAP (65536 - 16 - sizeof(ABlk))

void *arena_alloc(Arena *a, size_t n)
{
    void *p;
    n = (n + 7) & ~(size_t)7;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > ABLK_CAP ? n : ABLK_CAP;
        ABlk *b = malloc(sizeof(ABlk) + cap);
        if (!b)
            return 0;
        b->next = a->head; b->used = 0; b->cap = cap; a->head = b;
    }
    p = a->head->data + a->head->used;
    a->head->used += n;
    memset(p, 0, n);
    return p;
}
char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1);
    if (!p)
        return (char *)"";
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
void arena_free(Arena *a)
{
    ABlk *b = a->head;
    while (b) { ABlk *nx = b->next; free(b); b = nx; }
    a->head = NULL;
}

int utf8_next(const char **pp, const char *end)
{
    const unsigned char *p = (const unsigned char *)*pp;
    int c = *p, n = 0, i;
    if (c < 0x80) { *pp += 1; return c; }
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *pp += 1; return 0xFFFD; }
    if ((const char *)(p + n) >= end) { *pp = end; return 0xFFFD; }
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp += i; return 0xFFFD; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return c;
}
int utf8_put(char *o, int cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* la fuente es fija de 8x16: 1x hasta 25 px, 2x para titulos, 3x para letras enormes */
static int fscale(int px) { return px >= 44 ? 3 : px >= 26 ? 2 : 1; }
int text_w(int font, int px, const char *s, int len)
{
    const char *e = s + len;
    int n = 0;
    (void)font;
    while (s < e) { utf8_next(&s, e); n++; }
    return n * 8 * fscale(px);
}
int font_asc(int font, int px) { (void)font; return 12 * fscale(px); }
int font_desc(int font, int px) { (void)font; return 4 * fscale(px); }

/* sin imagenes ni JavaScript en esta version */
void image_free(Image *im) { (void)im; }
void doc_js_free(Doc *d) { (void)d; }

/* ================= Unicode -> CP437 (la fuente del sistema) ================= */
static int cp437(int c)
{
    static const unsigned short lat[96] = {     /* U+00A0..U+00FF */
        ' ', 0xAD, 0x9B, 0x9C, '$', 0x9D, '|', 0x15, '"', 'c', 0xA6, 0xAE, 0xAA, '-', 'r', '-',
        0xF8, 0xF1, 0xFD, '3', '\'', 0xE6, 0x14, 0xFA, ',', '1', 0xA7, 0xAF, 0xAC, 0xAB, '3', 0xA8,
        'A', 'A', 'A', 'A', 0x8E, 0x8F, 0x92, 0x80, 'E', 0x90, 'E', 'E', 'I', 'I', 'I', 'I',
        'D', 0xA5, 'O', 'O', 'O', 'O', 0x99, 'x', 'O', 'U', 'U', 'U', 0x9A, 'Y', 'P', 0xE1,
        0x85, 0xA0, 0x83, 'a', 0x84, 0x86, 0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
        'd', 0xA4, 0x95, 0xA2, 0x93, 'o', 0x94, 0xF6, 'o', 0x97, 0xA3, 0x96, 0x81, 'y', 'p', 0x98
    };
    if (c < 0x80)
        return c < 32 ? ' ' : c;
    if (c >= 0xA0 && c <= 0xFF)
        return lat[c - 0xA0];
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2022: case 0x25CF: case 0x2219: return 0x07;
    case 0x25E6: case 0x25CB: return 0x09;
    case 0x25AA: case 0x25A0: return 0xFE;
    case 0x2026: return 0xFA;
    case 0x2190: return 0x1B;
    case 0x2192: return 0x1A;
    case 0x2191: return 0x18;
    case 0x2193: return 0x19;
    case 0x25B6: case 0x25BA: return 0x10;
    case 0x25C0: case 0x25C4: return 0x11;
    case 0x25B2: case 0x25B4: return 0x1E;
    case 0x25BC: case 0x25BE: return 0x1F;
    case 0x2713: case 0x2714: case 0x221A: return 0xFB;
    case 0x00D7: case 0x2715: case 0x2716: return 'x';
    case 0x20AC: return 'E';
    case 0x2122: return 'T';
    case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: return ' ';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return ' ';
    case 0x2248: return 0xF7;
    case 0x2264: return 0xF3;
    case 0x2265: return 0xF2;
    case 0x221E: return 0xEC;
    case 0x03C0: return 0xE3;
    case 0x03B1: return 0xE0;
    case 0x03B2: return 0xE1;
    case 0x03A3: return 0xE4;
    case 0x03C3: return 0xE5;
    case 0x03BC: return 0xE6;
    case 0x2665: return 0x03;
    case 0x2666: return 0x04;
    case 0x2663: return 0x05;
    case 0x2660: return 0x06;
    case 0x263A: return 0x01;
    case 0x2302: case 0x2616: return 0x7F;
    case 0x2500: return 0xC4;
    case 0x2502: return 0xB3;
    case 0x2588: return 0xDB;
    case 0x2591: return 0xB0;
    case 0x2592: return 0xB1;
    case 0x2593: return 0xB2;
    }
    if (c >= 0x0100 && c <= 0x017F) {           /* Latin Extended-A: la letra sin acento */
        static const char ext[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
        int i = c - 0x100;
        return i < (int)sizeof ext - 1 ? ext[i] : '?';
    }
    return '?';
}

/* ================= colores: 0xRRGGBB -> paleta del C64 del sistema (vga.c) ================= */
static const u8 c64rgb[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0xFF, 0xFF, 0xFF }, { 0x68, 0x37, 0x2B }, { 0x70, 0xA4, 0xB2 },
    { 0x6F, 0x3D, 0x86 }, { 0x58, 0x8D, 0x43 }, { 0x35, 0x28, 0x79 }, { 0xB8, 0xC7, 0x6F },
    { 0x6F, 0x4F, 0x25 }, { 0x43, 0x39, 0x00 }, { 0x9A, 0x67, 0x59 }, { 0x44, 0x44, 0x44 },
    { 0x6C, 0x6C, 0x6C }, { 0x9A, 0xD2, 0x84 }, { 0x6C, 0x5E, 0xB5 }, { 0x95, 0x95, 0x95 }
};
enum { C_BLACK, C_WHITE, C_RED, C_CYAN, C_PURPLE, C_GREEN, C_BLUE, C_YELLOW, C_ORANGE, C_BROWN,
       C_LRED, C_DGREY, C_GREY, C_LGREEN, C_LBLUE, C_LGREY };

/* distancia de color: brillo con mas peso que el tinte, para que los grises vayan a grises y no a cafes */
static long cdist(int r, int g, int b, int r2, int g2, int b2)
{
    long y = (3 * r + 6 * g + b) / 10, y2 = (3 * r2 + 6 * g2 + b2) / 10;
    long dy = y - y2, dcr = (r - y) - (r2 - y2), dcb = (b - y) - (b2 - y2);
    return 4 * dy * dy + 2 * (dcr * dcr + dcb * dcb);
}
static signed char ccache[4096];
static int cmap(uint32_t rgb)
{
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4), i, best = 0;
    long bd = -1;
    if (ccache[key])
        return ccache[key] - 1;
    for (i = 0; i < 16; i++) {
        long d = cdist(r, g, b, c64rgb[i][0], c64rgb[i][1], c64rgb[i][2]);
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    ccache[key] = (signed char)(best + 1);
    return best;
}
static int lum(int c) { return (c64rgb[c][0] * 3 + c64rgb[c][1] * 6 + c64rgb[c][2]) / 10; }

/* fondos: si mezclar dos colores en tablero se parece mucho mas que uno solo (p.ej. amarillo palido), se tramea */
static unsigned char dcache[4096][2];
static void cmap2(uint32_t rgb, int *a, int *b2)
{
    int r = (int)(rgb >> 16) & 255, g = (int)(rgb >> 8) & 255, b = (int)rgb & 255;
    int key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4), i, j, ba, bb;
    long d1, bd;
    if (dcache[key][0]) { *a = dcache[key][0] - 1; *b2 = dcache[key][1] - 1; return; }
    ba = bb = cmap(rgb);
    d1 = bd = cdist(r, g, b, c64rgb[ba][0], c64rgb[ba][1], c64rgb[ba][2]);
    for (i = 0; i < 16; i++)
        for (j = i + 1; j < 16; j++) {
            long d;
            int li = lum(i), lj = lum(j);
            if (li - lj > 80 || lj - li > 80)    /* tablero de colores muy distintos = ruido */
                continue;
            d = cdist(r, g, b, (c64rgb[i][0] + c64rgb[j][0]) / 2, (c64rgb[i][1] + c64rgb[j][1]) / 2,
                      (c64rgb[i][2] + c64rgb[j][2]) / 2);
            if (d < bd) { bd = d; ba = i; bb = j; }
        }
    if (bd * 2 > d1)                             /* no mejora tanto: color solido */
        ba = bb = cmap(rgb);
    dcache[key][0] = (unsigned char)(ba + 1);
    dcache[key][1] = (unsigned char)(bb + 1);
    *a = ba; *b2 = bb;
}


/* ================= dibujo en la superficie de la ventana ================= */
static u8 *PIX;
static int STR, WW, WH, dphase;     /* dphase: el tramado se mueve con el scroll (no "nada" al desplazar) */
static int clx0, cly0, clx1, cly1;
static void clip(int x0, int y0, int x1, int y1) { clx0 = x0; cly0 = y0; clx1 = x1; cly1 = y1; }
static void noclip(void) { clip(0, 0, WW, WH); }
static void rect(int x, int y, int w, int h, int c)
{
    int x1 = x + w, y1 = y + h, j;
    if (x < clx0) x = clx0;
    if (y < cly0) y = cly0;
    if (x1 > clx1) x1 = clx1;
    if (y1 > cly1) y1 = cly1;
    if (x >= x1 || y >= y1)
        return;
    for (j = y; j < y1; j++)
        memset(PIX + (long)j * STR + x, c, (size_t)(x1 - x));
}
static void rect_rgb(int x, int y, int w, int h, uint32_t rgb)
{
    int a, b, x1 = x + w, y1 = y + h, i, j;
    cmap2(rgb, &a, &b);
    if (a == b) { rect(x, y, w, h, a); return; }
    if (x < clx0) x = clx0;
    if (y < cly0) y = cly0;
    if (x1 > clx1) x1 = clx1;
    if (y1 > cly1) y1 = cly1;
    for (j = y; j < y1; j++) {
        u8 *row = PIX + (long)j * STR;
        for (i = x; i < x1; i++)
            row[i] = (u8)(((i + j + dphase) & 1) ? b : a);
    }
}
static void frame(int x, int y, int w, int h, int c)
{
    rect(x, y, w, 1, c); rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c); rect(x + w - 1, y, 1, h, c);
}
static int getpx(int x, int y)
{
    if (x < 0 || y < 0 || x >= WW || y >= WH)
        return -1;
    return PIX[(long)y * STR + x];
}
static void glyph(int x, int y, int ch, int s, int fg, int bold)
{
    const u8 *g = (const u8 *)FONT_ADDR + (ch & 255) * 16;
    int i, j;
    if (x >= clx1 || y >= cly1 || x + 8 * s <= clx0 || y + 16 * s <= cly0)
        return;
    for (i = 0; i < 16; i++) {
        unsigned bits = g[i];
        if (bold)
            bits |= bits >> 1;
        if (!bits)
            continue;
        for (j = 0; j < 8; j++)
            if (bits & (0x80u >> j))
                rect(x + j * s, y + i * s, s, s, fg);
    }
}
/* texto UTF-8 con la fuente del sistema; devuelve el ancho */
static int text(int x, int y, const char *s, int len, int s_, int fg, int bold)
{
    const char *e = s + len;
    int x0 = x;
    while (s < e) {
        int cp = utf8_next(&s, e);
        glyph(x, y, cp437(cp), s_, fg, bold);
        x += 8 * s_;
    }
    return x - x0;
}
static void atext(int x, int y, const char *s, int fg) { text(x, y, s, (int)strlen(s), 1, fg, 0); }

/* ================= estado del navegador ================= */
#define TOPH  22                     /* barra de direccion */
#define STATH 18                     /* barra de estado */
#define SBW   10                     /* barra de desplazamiento */
#define URLMAX 1024
#define HMAX  32

static struct wctx *Wc;
static Doc *doc;
static Page pg;
static int scroll, closed, busy, pending, pend_push, sb_drag;
static char hist[HMAX][URLMAX];
static int hn, hpos = -1;
static char cur[URLMAX], want[URLMAX];
static char addr[URLMAX];
static int addr_focus = 1, addr_fresh;
static char status[200], hover[URLMAX];
static unsigned load_ms, net_ms, page_ms, draw_ms;

/* ---- formularios: el campo con el foco se edita aqui y se guarda en su atributo value ---- */
#define FMAX 1024
static Node *fnode;                          /* campo de texto con el foco (0 = ninguno) */
static char fbuf[FMAX];                      /* su texto en UTF-8 */
static int flen;
static int vh(void) { int h = WH - TOPH - STATH; return h < 1 ? 1 : h; }
static int vw(void) { int w = WW - SBW; return w < 40 ? 40 : w; }
static int maxscroll(void) { int m = pg.height - vh(); return m < 0 ? 0 : m; }
static void clamp_scroll(void) { if (scroll > maxscroll()) scroll = maxscroll(); if (scroll < 0) scroll = 0; }

static void scopy(char *d, const char *s, int max)
{
    int i;
    for (i = 0; s[i] && i < max - 1; i++)
        d[i] = s[i];
    d[i] = 0;
}

/* ---- pintar ---- */
static void button(int x, int y, int w, const char *lab, int enabled)
{
    rect(x, y, w, 18, C_DGREY);
    rect(x, y, w - 1, 17, C_WHITE);
    rect(x + 1, y + 1, w - 2, 16, C_LGREY);
    rect(x + 2, y + 2, w - 4, 14, C_GREY);
    text(x + (w - 8 * (int)strlen(lab)) / 2, y + 1, lab, (int)strlen(lab), 1, enabled ? C_WHITE : C_DGREY, 1);
}
#define AX 92                         /* donde empieza el campo de la direccion */

static void draw_bar(void)
{
    int fw = WW - AX - 4, maxc, n, off;
    noclip();
    rect(0, 0, WW, TOPH, C_BLUE);
    rect(0, TOPH - 1, WW, 1, C_LBLUE);
    button(2, 2, 20, "<", hpos > 0);
    button(24, 2, 20, ">", hpos < hn - 1);
    button(46, 2, 20, "R", 1);
    button(68, 2, 20, "\x7f", 1);
    if (fw < 24)
        return;
    rect(AX, 2, fw, 18, addr_focus ? C_WHITE : C_LGREY);
    frame(AX, 2, fw, 18, C_DGREY);
    maxc = (fw - 8) / 8;
    n = (int)strlen(addr);
    off = n > maxc - 1 ? n - (maxc - 1) : 0;
    clip(AX + 1, 3, AX + fw - 1, 19);
    {
        int x = AX + 4, i;
        for (i = off; i < n; i++, x += 8)
            glyph(x, 3, (u8)addr[i] < 128 ? addr[i] : '?', 1, addr_fresh && addr_focus ? C_BLUE : C_BLACK, 0);
        if (addr_focus && !busy)
            rect(x, 4, 2, 15, C_BLUE);
    }
    noclip();
}

static void draw_status(void)
{
    const char *m = hover[0] ? hover : status;
    noclip();
    rect(0, WH - STATH, WW, STATH, C_DGREY);
    rect(0, WH - STATH, WW, 1, C_GREY);
    clip(4, WH - STATH, WW - 4, WH);
    atext(4, WH - STATH + 1, m, hover[0] ? C_LBLUE : C_LGREY);
    noclip();
}

static int is_area(Node *n) { return n && !strcmp(n->name, "textarea"); }

/* el rectangulo del control en la pagina (el primer IT_RECT con su nodo) */
static Item *ctrl_rect(Node *n)
{
    int i;
    for (i = 0; i < pg.n; i++)
        if (pg.it[i].kind == IT_RECT && pg.it[i].nd == n)
            return &pg.it[i];
    return 0;
}

/* pinta encima del campo lo que se va escribiendo, con el cursor */
static void draw_field(int top)
{
    Item *r = ctrl_rect(fnode);
    int sy, cw = 8, ch = 16, cols, rows, n = 0, i, x;
    const char *p, *e;
    int cps[FMAX];
    if (!r)
        return;
    sy = r->y - scroll + top;
    rect(r->x, sy, r->w, r->h, C_BLUE);              /* borde azul: aqui va lo que escribes */
    rect(r->x + 2, sy + 2, r->w - 4, r->h - 4, C_WHITE);
    cols = (r->w - 8) / cw; if (cols < 1) cols = 1;
    rows = is_area(fnode) ? (r->h - 4) / ch : 1; if (rows < 1) rows = 1;
    p = fbuf; e = fbuf + flen;
    while (p < e && n < FMAX)
        cps[n++] = utf8_next(&p, e);
    if (rows == 1) {                                 /* una linea: se ve el final */
        int from = n > cols - 1 ? n - (cols - 1) : 0;
        int ty = sy + (r->h - ch) / 2;
        for (i = from, x = r->x + 4; i < n; i++, x += cw)
            glyph(x, ty, cp437(cps[i]), 1, C_BLACK, 0);
        rect(x, ty, 2, ch, C_BLUE);
        return;
    }
    {   /* varias lineas (textarea): se parte por columnas y saltos, se ven las ultimas */
        int lines = 1, cx = 0, line = 0, first;
        for (i = 0; i < n; i++) {
            if (cps[i] == '\n' || cx >= cols) { lines++; cx = 0; if (cps[i] == '\n') continue; }
            cx++;
        }
        first = lines > rows ? lines - rows : 0;
        cx = 0;
        for (i = 0; i <= n; i++) {
            int c = i < n ? cps[i] : -1;
            if (c == '\n' || cx >= cols) { line++; cx = 0; if (c == '\n') continue; }
            if (line >= first) {
                int gx = r->x + 4 + cx * cw, gy = sy + 3 + (line - first) * ch;
                if (c < 0) rect(gx, gy, 2, ch, C_BLUE);
                else glyph(gx, gy, cp437(c), 1, C_BLACK, 0);
            }
            cx++;
        }
    }
}

static void draw_page(void)
{
    int top = TOPH, h = vh(), w = vw(), i;
    clip(0, top, w, top + h);
    dphase = scroll;
    rect_rgb(0, top, w, h, doc ? doc->canvas_bg : 0x352879);
    for (i = 0; doc && i < pg.n; i++) {
        Item *it = &pg.it[i];
        int sy = it->y - scroll + top;
        if (sy + it->h < top || sy > top + h)
            continue;
        switch (it->kind) {
        case IT_RECT:
            rect_rgb(it->x, sy, it->w, it->h, it->color);
            break;
        case IT_FRAME:
            frame(it->x, sy, it->w, it->h, cmap(it->color));
            break;
        case IT_IMG:
            frame(it->x, sy, it->w, it->h, C_GREY);
            break;
        case IT_TEXT: {
            int s = fscale(it->px), fg = cmap(it->color);
            int under = getpx(it->x + 1, sy + 2 * s);
            int bold = it->font == FONT_BOLD || it->font == FONT_MONOBOLD;
            if (under >= 0 && under < 16 && (fg == under || (lum(fg) - lum(under) < 50 && lum(under) - lum(fg) < 50)))
                fg = lum(under) > 110 ? C_BLACK : C_WHITE;       /* que se pueda leer con 16 colores */
            text(it->x, sy, it->s, it->len, s, fg, bold);
            if (it->ul)
                rect(it->x, sy + 13 * s, it->w, s, fg);
            if (it->st)
                rect(it->x, sy + 8 * s, it->w, s, fg);
            break;
        }
        }
    }
    if (fnode)
        draw_field(top);
    if (busy || pending) {                       /* aviso de carga encima de la pagina */
        int bw = 8 * 14;
        rect(w / 2 - bw / 2 - 6, top + h / 2 - 14, bw + 12, 28, C_BLUE);
        frame(w / 2 - bw / 2 - 6, top + h / 2 - 14, bw + 12, 28, C_WHITE);
        atext(w / 2 - bw / 2, top + h / 2 - 8, "Cargando...", C_WHITE);
    }
    noclip();
    /* barra de desplazamiento */
    rect(w, top, SBW, h, C_DGREY);
    if (pg.height > h) {
        int th = (int)((long)h * h / pg.height), ty;
        if (th < 16) th = 16;
        ty = top + (int)((long)(h - th) * scroll / maxscroll());
        rect(w + 1, ty, SBW - 2, th, C_LGREY);
    } else
        rect(w + 1, top, SBW - 2, h, C_GREY);
}

static void redraw(void)
{
    unsigned t0;
    if (!Wc || closed)
        return;
    PIX = Wc->pix; STR = Wc->stride; WW = Wc->w; WH = Wc->h;
    t0 = K->ms();
    draw_page();
    draw_ms = K->ms() - t0;
    draw_bar();
    draw_status();
    Wc->damage(Wc, 0, 0, WW, WH);
}
static void redraw_status(void)
{
    if (!Wc || closed)
        return;
    draw_status();
    Wc->damage(Wc, 0, WH - STATH, WW, STATH);
}

/* ================= red: NET.BIN (iPXE) cargado aqui mismo ================= */
#define NET_BASE   0x700000u
#define NET_MAX    0x800000u
#define NET_CACHE  0x8F00000u                    /* copia limpia de NET.BIN (cabe en 1 MB) */
#define NET_CMAX   0x100000u
#define STASH      0x9000000u                    /* bosstash app: aqui deja iPXE lo que bajo */
#define STASH_MAX  0x2000000u
#define MBOX       0xB100000u
#define MBOX_MAGIC 0x48545342u

struct bos_boot {
    int (*poll)(void);
    void (*print)(const char *s);
    void *(*map_mmio)(unsigned long long phys, unsigned size);
    unsigned (*ms)(void);
    int (*read)(const char *spec, char *buf, int max, unsigned *total);
    int (*write)(const char *spec, const void *buf, unsigned n);
    const char *args;
};
struct hdr { uint32_t magic, end, entry, base; };
struct mbox { uint32_t magic, app_len, list_len, spare; };

static unsigned net_len;                          /* 0 = todavia no se ha leido */
static char netlog[2048];
static int nlog;
static char netcmd[4096];

static void net_print(const char *s)
{
    for (; *s; s++) {
        char c = *s;
        if (c == '\r')
            continue;
        if (nlog >= (int)sizeof netlog - 1) {       /* se queda con lo ultimo */
            memmove(netlog, netlog + 1024, (size_t)nlog - 1024);
            nlog -= 1024;
        }
        netlog[nlog++] = c;
    }
    netlog[nlog] = 0;
}
static int poll_nokeys(void) { return K->poll(), 0; }   /* que iPXE no se coma las teclas */

static int net_load(void)
{
    static const char *const where[] = {
        "USB0.1:NET.BIN", "USB1.1:NET.BIN", "HD0.1:NET.BIN", "HD1.1:NET.BIN", "HD2.1:NET.BIN", "HD0:NET.BIN",
        "HD1:NET.BIN", "FD1:NET.BIN", "FD0:NET.BIN", 0
    };
    int i, n = -1;
    unsigned total;
    if (net_len)
        return 1;
    for (i = 0; where[i]; i++) {
        total = 0;
        n = kread(where[i], (char *)(unsigned long)NET_CACHE, (int)NET_CMAX, &total);
        if (n > 16 && (unsigned)n == total)
            break;
        n = -1;
    }
    if (n < 0)
        return 0;
    {
        const struct hdr *h = (const struct hdr *)(unsigned long)NET_CACHE;
        if (h->magic != 0x4D424350u || h->base != NET_BASE || h->end > NET_BASE + NET_MAX ||
            h->entry < NET_BASE || h->entry >= NET_BASE + (unsigned)n)
            return 0;
    }
    net_len = (unsigned)n;
    return 1;
}

/* corre una linea de comandos de iPXE (separados por ;) */
static int net_run(const char *cmd)
{
    static struct bos_boot b;
    const struct hdr *h = (const struct hdr *)(unsigned long)NET_CACHE;
    int (*entry)(const struct bos_boot *);
    if (!net_load())
        return -1;
    memcpy((void *)(unsigned long)NET_BASE, (void *)(unsigned long)NET_CACHE, net_len);
    memset((void *)(unsigned long)(NET_BASE + net_len), 0, h->end - (NET_BASE + net_len));
    b.poll = poll_nokeys;
    b.print = net_print;
    b.map_mmio = K->map_mmio;
    b.ms = K->ms;
    b.read = K->read;
    b.write = 0;
    b.args = cmd;
    entry = (int (*)(const struct bos_boot *))(unsigned long)h->entry;
    entry(&b);
    sse_on();
    return 0;
}

static void cat(char *d, int *k, int max, const char *s)
{
    while (*s && *k < max - 1)
        d[(*k)++] = *s++;
    d[*k] = 0;
}

/* baja url a STASH; devuelve los bytes o -1 (el porque queda en netlog) */
/* lo que dio el DHCP, para no esperarlo en cada pagina: "ifopen net0;set net0/ip ...;..." */
static char netcfg[200];
static void remember_net(void)
{
    char *p = strstr(netlog, "BOSIP "), *f[4];
    int n = 0, k = 0;
    if (!p) return;
    p += 6;
    while (n < 4) {                               /* ip mascara puerta dns */
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\n') p++;
        if (*p == ' ') *p++ = 0; else if (*p == '\n') { *p = 0; break; }
    }
    if (n < 2 || !strchr(f[0], '.')) return;
    netcfg[0] = 0;
    cat(netcfg, &k, sizeof netcfg, "ifopen net0;set net0/ip ");
    cat(netcfg, &k, sizeof netcfg, f[0]);
    cat(netcfg, &k, sizeof netcfg, ";set net0/netmask ");
    cat(netcfg, &k, sizeof netcfg, f[1]);
    if (n > 2 && strchr(f[2], '.')) { cat(netcfg, &k, sizeof netcfg, ";set net0/gateway "); cat(netcfg, &k, sizeof netcfg, f[2]); }
    if (n > 3 && strchr(f[3], '.')) { cat(netcfg, &k, sizeof netcfg, ";set dns "); cat(netcfg, &k, sizeof netcfg, f[3]); }
}

static long net_fetch2(const char *url, int retry)
{
    struct mbox *m = (struct mbox *)(unsigned long)MBOX;
    char enc[URLMAX * 3], part[128];
    int k = 0, i, j, np = 0;
    const char *hx = "0123456789ABCDEF";
    /* sin #fragmento; lo que iPXE no se traga va como %XX */
    for (i = 0; url[i] && url[i] != '#' && k < (int)sizeof enc - 4; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c <= ' ' || c >= 127 || c == ';' || c == '$' || c == '"' || c == '\'' || c == '\\' || c == '{' || c == '}') {
            enc[k++] = '%'; enc[k++] = hx[c >> 4]; enc[k++] = hx[c & 15];
        } else
            enc[k++] = (char)c;
    }
    enc[k] = 0;
    k = 0;
    netcmd[0] = 0;
    /* la primera vez dhcp (y se anota lo que dio); las siguientes, esa misma configuracion sin esperar al DHCP */
    if (netcfg[0])
        cat(netcmd, &k, sizeof netcmd, netcfg);
    else
        cat(netcmd, &k, sizeof netcmd, "dhcp;echo BOSIP ${net0/ip} ${net0/netmask} ${net0/gateway} ${dns}");
    for (i = 0; enc[i]; ) {                   /* la URL en trozos de 100 letras: set u0 ...;set u1 ... */
        char nm[24];
        int pl = 0;
        for (j = 0; j < 100 && enc[i]; j++)
            part[pl++] = enc[i++];
        part[pl] = 0;
        snprintf(nm, sizeof nm, ";set u%d ", np++);
        cat(netcmd, &k, sizeof netcmd, nm);
        cat(netcmd, &k, sizeof netcmd, part);
    }
    cat(netcmd, &k, sizeof netcmd, ";imgfetch -n cosmospage ");
    for (i = 0; i < np; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "${u%d}", i);
        cat(netcmd, &k, sizeof netcmd, nm);
    }
    cat(netcmd, &k, sizeof netcmd, ";bosstash cosmospage app");
    m->magic = 0; m->app_len = 0;
    nlog = 0; netlog[0] = 0;
    if (net_run(netcmd) < 0) {
        net_print("No encuentro NET.BIN en la particion FAT (usa pc-basic-doom.img o super-basicos.img).\n");
        return -1;
    }
    if (m->magic != MBOX_MAGIC || m->app_len == 0) {
        if (netcfg[0] && !retry) {                /* la red cambio? otra vez con dhcp */
            netcfg[0] = 0;
            return net_fetch2(url, 1);
        }
        return -1;
    }
    if (!netcfg[0])
        remember_net();
    return (long)m->app_len;
}

static long net_fetch(const char *url) { return net_fetch2(url, 0); }

static int is_drive_path(const char *u)
{
    const char *c = strchr(u, ':');
    int n;
    if (!c || c[1] == '/' || c == u)
        return 0;
    n = (int)(c - u);
    if (n > 6)
        return 0;
    return !strncasecmp(u, "HD", 2) || !strncasecmp(u, "USB", 3) || !strncasecmp(u, "FD", 2) || !strncasecmp(u, "CD", 2);
}

/* ================= paginas internas ================= */
static const char home_html[] =
    "<html><head><title>Inicio</title></head>"
    "<body style=\"background:#352879;color:#ffffff;margin:16px\">"
    "<h1 style=\"color:#b8c76f\">COSMOS MM</h1>"
    "<p>Navegador m\xc3\xadnimo de BASIC-OS hecho con el <b>Minimal Motor (MM)</b> de Cosmos Browser (COS-Mods): "
    "el mismo motor de HTML y CSS, dibujado con la fuente y los 16 colores del sistema.</p>"
    "<p>Escribe una direcci\xc3\xb3n arriba y pulsa <b>Enter</b>. Si escribes palabras sueltas se buscan en DuckDuckGo.</p>"
    "<h2 style=\"color:#9ad284\">Para empezar</h2>"
    "<ul>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\" style=\"color:#70a4b2\">La primera p\xc3\xa1gina web (CERN, 1991)</a></li>"
    "<li><a href=\"https://example.com/\" style=\"color:#70a4b2\">example.com</a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\" style=\"color:#70a4b2\">DuckDuckGo Lite</a></li>"
    "<li><a href=\"http://frogfind.com/\" style=\"color:#70a4b2\">FrogFind: buscador para computadoras viejas</a></li>"
    "<li><a href=\"https://text.npr.org/\" style=\"color:#70a4b2\">NPR solo texto (noticias)</a></li>"
    "<li><a href=\"http://68k.news/\" style=\"color:#70a4b2\">68k.news (noticias para equipos retro)</a></li>"
    "<li><a href=\"https://basic-os.onrender.com/list\" style=\"color:#70a4b2\">Lista del hub de BASIC-OS</a></li>"
    "<li><a href=\"https://basic-os.onrender.com/videos\" style=\"color:#70a4b2\">Videos del hub de BASIC-OS (PLAY y MPLAY)</a></li>"
    "</ul>"
    "<h2 style=\"color:#9ad284\">Teclas</h2>"
    "<p><b>Enter</b> ir &middot; <b>Tab</b> salir de la barra &middot; empieza a escribir para una direcci\xc3\xb3n nueva<br>"
    "<b>\xe2\x86\x91 \xe2\x86\x93</b> desplazar &middot; <b>Espacio</b> p\xc3\xa1gina siguiente &middot; "
    "<b>\xe2\x86\x90</b> o <b>Retroceso</b> atr\xc3\xa1s &middot; <b>\xe2\x86\x92</b> adelante</p>"
    "<p>Tambi\xc3\xa9n abre archivos del disco: <b>HD0.1:PAGINA.HTM</b></p>"
    "<p style=\"color:#959595\">Sin JavaScript ni im\xc3\xa1genes en esta versi\xc3\xb3n. La red usa NET.BIN (iPXE): "
    "hace falta la imagen con partici\xc3\xb3n FAT (pc-basic-doom.img o super-basicos.img) y una tarjeta de red.</p>"
    "</body></html>";

/* escapa texto para meterlo en una pagina */
static void put_esc(char *o, int *k, int max, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        const char *r = 0;
        char ch[2] = { s[i], 0 };
        if (s[i] == '<') r = "&lt;";
        else if (s[i] == '>') r = "&gt;";
        else if (s[i] == '&') r = "&amp;";
        else r = ch;
        cat(o, k, max, r);
    }
}

/* ================= cargar una pagina ================= */
static int utf8_ok(const unsigned char *s, long n)
{
    long i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0)
            return 0;
        if (i + k >= n)
            return 1;
        for (i++; k; k--, i++)
            if ((s[i] & 0xC0) != 0x80)
                return 0;
    }
    return 1;
}

static void set_page(const char *src, long n, const char *url)
{
    const char *p = src;
    long i;
    int html;
    char *buf;
    long bn = 0;
    /* todo lo de la pagina anterior se tira de un golpe */
    mm_heap_reset();
    memset(&pg, 0, sizeof pg);
    doc = 0;
    fnode = 0;
    scroll = 0;
    while (p < src + n && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
        p++;
    if (n >= 3 && (u8)p[0] == 0xEF && (u8)p[1] == 0xBB && (u8)p[2] == 0xBF)
        p += 3;
    html = p < src + n && *p == '<';
    if (!html)                                   /* texto plano: dentro de <pre> */
        for (i = 0; i < n && i < 4096; i++)
            if (!strncasecmp(src + i, "<html", 5) || !strncasecmp(src + i, "<body", 5)) { html = 1; break; }
    if (n > 0x200000)                            /* mas de 2 MB no cabe bien en el monticulo: se corta */
        n = 0x200000;
    buf = malloc((size_t)n * 5 + 64);
    if (!buf)
        return;
    if (!html) {
        static const char pre[] = "<html><body><pre>";
        memcpy(buf, pre, sizeof pre - 1);
        bn = sizeof pre - 1;
    }
    if (utf8_ok((const unsigned char *)src, n)) {
        for (i = 0; i < n; i++) {
            if (!html && (src[i] == '<' || src[i] == '&')) {
                const char *r = src[i] == '<' ? "&lt;" : "&amp;";
                while (*r) buf[bn++] = *r++;
            } else
                buf[bn++] = src[i];
        }
    } else {                                     /* latin-1 / windows-1252 -> UTF-8 */
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)src[i];
            if (!html && (c == '<' || c == '&')) {
                const char *r = c == '<' ? "&lt;" : "&amp;";
                while (*r) buf[bn++] = *r++;
            } else
                bn += utf8_put(buf + bn, c);
        }
    }
    if (!html) {
        static const char post[] = "</pre></body></html>";
        memcpy(buf + bn, post, sizeof post - 1);
        bn += sizeof post - 1;
    }
    buf[bn] = 0;
    css_viewport_w = vw();
    css_viewport_h = vh();
    doc = html_parse(buf, (size_t)bn, url);
    if (!doc)
        return;
    doc_style(doc);
    layout_doc(doc, &pg, vw(), 1.0f, 0);
}

static void error_page(const char *url, const char *why)
{
    static char e[8192];
    int k = 0, from;
    cat(e, &k, sizeof e, "<html><head><title>Error</title></head><body style=\"background:#68372b;color:#ffffff;margin:16px\">"
                         "<h1>No pude abrir la p\xc3\xa1gina</h1><p><b>");
    put_esc(e, &k, sizeof e, url, (int)strlen(url));
    cat(e, &k, sizeof e, "</b></p><p>");
    cat(e, &k, sizeof e, why);
    cat(e, &k, sizeof e, "</p>");
    if (nlog) {
        from = nlog > 1500 ? nlog - 1500 : 0;
        cat(e, &k, sizeof e, "<h2>Lo que dijo iPXE</h2><pre style=\"color:#b8c76f\">");
        put_esc(e, &k, sizeof e, netlog + from, nlog - from);
        cat(e, &k, sizeof e, "</pre>");
    }
    cat(e, &k, sizeof e, "<p><a href=\"cosmos:home\" style=\"color:#b8c76f\">Volver al inicio</a></p></body></html>");
    set_page(e, k, url);
}

static void title_from_doc(void)
{
    char t[64];
    int k = 0;
    cat(t, &k, 44, "COSMOS");
    if (doc && doc->title && doc->title[0]) {
        const char *s = doc->title, *e = s + strlen(s);
        cat(t, &k, 44, " - ");
        while (s < e && k < 42) {                /* el titulo de la ventana usa CP437 */
            int cp = utf8_next(&s, e);
            t[k++] = (char)cp437(cp);
        }
        t[k] = 0;
    }
    Wc->title(Wc, t);
}

/* la URL apunta a un video .BVC (sin contar ?consulta ni #ancla) */
static int is_bvc(const char *u)
{
    size_t n = strcspn(u, "?#");
    return n > 4 && !strncasecmp(u + n - 4, ".bvc", 4);
}

/* los videos no se abren como pagina: se bajan a 0x9000000 y PLAY los reproduce a pantalla completa (modo MEM:) */
static void play_video(const char *u)
{
    long n;
    const unsigned char *v = (const unsigned char *)(unsigned long)STASH;
    if (is_drive_path(u)) {
        unsigned total = 0;
        int r = kread(u, (char *)(unsigned long)STASH, (int)STASH_MAX, &total);
        n = (r > 0 && (unsigned)r == total) ? r : -1;
    } else {
        scopy(status, "Bajando el video...", sizeof status);
        redraw_status();
        n = net_fetch(u);
    }
    if (n < 32 || v[0] != 'B' || v[1] != 'V' || v[2] != 'C' || v[3] != '1') {
        scopy(status, n < 0 ? "Error: no se pudo bajar el video" : "Error: no es un video BVC1", sizeof status);
        redraw_status();
        return;
    }
    snprintf((char *)K->args, 160, "MEM:%ld", n);
    snprintf(status, sizeof status, "Video: %ld KB (Espacio = pausa, ESC = salir)", n / 1024);
    redraw_status();
    Wc->launch("PLAY.APP");                     /* el escritorio lo corre a pantalla completa y luego repinta todo */
    sse_on();
    snprintf(status, sizeof status, "Video terminado (%ld KB)", n / 1024);
    redraw_status();
}

/* la URL apunta a un video/audio para MPLAY (.MP4 .MKV .WEBM .MP3...): se baja y se guarda en el disco */
static int is_media(const char *u)
{
    static const char *const ext[] = { ".mp4", ".m4v", ".mkv", ".webm", ".avi", ".mov", ".mp3", ".ogg", ".wav", 0 };
    size_t n = strcspn(u, "?#"), k;
    int i;
    for (i = 0; ext[i]; i++) {
        k = strlen(ext[i]);
        if (n > k && !strncasecmp(u + n - k, ext[i], k))
            return 1;
    }
    return 0;
}

int cm_save(const struct kapi *k, const char *dir11, const char *name, const void *data, unsigned size, char *where);

/* baja el archivo y lo deja en HDn.1:/VIDEOS/ (ahi lo busca MPLAY). El nombre es el de la URL (tiene que ser 8.3) */
static void save_media(const char *u)
{
    char name[16], where[48];
    size_t n = strcspn(u, "?#"), a = n;
    long got;
    int r, k = 0;
    while (a > 0 && u[a - 1] != '/' && u[a - 1] != ':')
        a--;
    while (a < n && k < 15)
        name[k++] = u[a++];
    name[k] = 0;
    scopy(status, "Bajando el video para MPLAY...", sizeof status);
    redraw_status();
    got = net_fetch(u);
    if (got <= 0) {
        scopy(status, "Error: no se pudo bajar el video (maximo 32 MB)", sizeof status);
        redraw_status();
        return;
    }
    scopy(status, "Guardando en el disco...", sizeof status);
    redraw_status();
    r = cm_save(K, "VIDEOS     ", name, (const void *)(unsigned long)STASH, (unsigned)got, where);
    sse_on();
    if (r == 100)
        scopy(status, "Error: no hay disco FAT de la BIOS donde guardarlo", sizeof status);
    else if (r == 1)
        snprintf(status, sizeof status, "Error: %s no es un nombre corto (8.3)", name);
    else if (r == 4)
        scopy(status, "Error: el disco esta lleno", sizeof status);
    else if (r)
        snprintf(status, sizeof status, "Error %d al guardar %s", r, name);
    else
        snprintf(status, sizeof status, "Guardado %s (%ld KB): abrelo con MPLAY", where, got / 1024);
    redraw_status();
}

static void do_load(void)
{
    long n = -1;
    int failed = 0;
    const char *u = want;
    unsigned t0 = K->ms();
    const char *bw = binwin_busy(0x40000000ull);
    if (bw && strncmp(u, "cosmos:", 7)) {        /* un programa de la tienda con ventana ocupa 0x9000000 */
        snprintf(status, sizeof status, "Cierra primero la ventana de %s (usa la misma memoria)", bw);
        redraw_status();
        return;
    }
    if (is_bvc(u) || (is_media(u) && !is_drive_path(u))) {
        busy = 1;
        if (is_bvc(u))
            play_video(u);
        else
            save_media(u);
        busy = 0;
        scopy(addr, strncmp(cur, "cosmos:", 7) ? cur : "", URLMAX);   /* la pagina sigue siendo la de antes */
        addr_fresh = 1;
        if (!closed)
            redraw();
        return;
    }
    busy = 1;
    hover[0] = 0;
    if (!strncmp(u, "cosmos:", 7)) {
        nlog = 0;
        set_page(home_html, (long)sizeof home_html - 1, "cosmos:home");
    } else if (is_drive_path(u)) {
        unsigned total = 0;
        int r = kread(u, (char *)(unsigned long)STASH, (int)STASH_MAX, &total);
        nlog = 0;
        if (r < 0)
            error_page(u, "No existe ese archivo (o la unidad)."), failed = 1;
        else
            set_page((const char *)(unsigned long)STASH, r, u), n = r;
    } else {
        unsigned tn = K->ms();
        n = net_fetch(u);
        net_ms = K->ms() - tn;
        if (n < 0)
            error_page(u, "No se pudo bajar. Revisa la direcci\xc3\xb3n, el cable de red y que la p\xc3\xa1gina exista "
                          "(iPXE no muestra p\xc3\xa1ginas de error 404/500)."), failed = 1;
        else {
            unsigned tp = K->ms();
            set_page((const char *)(unsigned long)STASH, n, u);
            page_ms = K->ms() - tp;
        }
    }
    load_ms = K->ms() - t0;
    scopy(cur, u, URLMAX);
    if (pend_push) {                             /* historial: lo de adelante se tira */
        if (hpos < hn - 1)
            hn = hpos + 1;
        if (hn == HMAX) {
            memmove(hist[0], hist[1], (size_t)(HMAX - 1) * URLMAX);
            hn--; hpos--;
        }
        scopy(hist[hn++], u, URLMAX);
        hpos = hn - 1;
    }
    scopy(addr, strncmp(u, "cosmos:", 7) ? u : "", URLMAX);
    addr_fresh = 1;
    addr_focus = !strncmp(u, "cosmos:", 7);
    if (failed)
        scopy(status, "Error: no se pudo abrir", sizeof status);
    else if (mm_oom)
        snprintf(status, sizeof status, "La p\xc3\xa1gina es demasiado grande: se ve solo una parte");
    else
        snprintf(status, sizeof status, n >= 10240 ? "Listo: %d KB en %d ms (red %d, pagina %d)" : "Listo: %d bytes en %d ms (red %d, pagina %d)",
                 (int)(n >= 10240 ? n / 1024 : n > 0 ? n : 0), (int)load_ms, (int)net_ms, (int)page_ms);
    {   /* el estado se dibuja con CP437 */
        static char st2[200];
        const char *s = status, *e = s + strlen(s);
        int k = 0;
        while (s < e && k < 199) st2[k++] = (char)cp437(utf8_next(&s, e));
        st2[k] = 0;
        memcpy(status, st2, (size_t)k + 1);
    }
    busy = 0;
    if (!closed) {
        title_from_doc();
        redraw();
    }
}

/* pide cargar una URL: se pinta "Cargando..." y la carga se hace dentro de frame() dos cuadros despues */
static void navigate(const char *u, int push)
{
    if (busy)
        return;
    scopy(want, u, URLMAX);
    pend_push = push;
    pending = 2;
    scopy(status, "Cargando...", sizeof status);
    redraw();
}

/* lo que se escribio en la barra -> URL */
static void omnibox(const char *in)
{
    char s[URLMAX], out[URLMAX * 3];
    int n, i, k = 0, sp = 0, dot = 0;
    while (*in == ' ') in++;
    scopy(s, in, URLMAX);
    n = (int)strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
    if (!n) { navigate("cosmos:home", 1); return; }
    if (strstr(s, "://") || !strncmp(s, "cosmos:", 7) || is_drive_path(s)) { navigate(s, 1); return; }
    for (i = 0; i < n; i++) { if (s[i] == ' ') sp = 1; if (s[i] == '.') dot = 1; }
    if (!sp && (dot || !strncasecmp(s, "localhost", 9))) {
        snprintf(out, sizeof out, "%s://%s", strncasecmp(s, "localhost", 9) && strncmp(s, "10.", 3) && strncmp(s, "192.168.", 8) ? "https" : "http", s);
        navigate(out, 1);
        return;
    }
    cat(out, &k, sizeof out, "https://lite.duckduckgo.com/lite/?q=");
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ')
            cat(out, &k, sizeof out, "+");
        else if (isalnum(c) || c == '-' || c == '_' || c == '.')
            { char t[2] = { (char)c, 0 }; cat(out, &k, sizeof out, t); }
        else {
            char t[4];
            int cp = c;
            if (c >= 128) {                      /* CP437 del teclado -> UTF-8 */
                static const unsigned short back[] = { 0xC7,0xFC,0xE9,0xE2,0xE4,0xE0,0xE5,0xE7,0xEA,0xEB,0xE8,0xEF,0xEE,0xEC,0xC4,0xC5,
                                                      0xC9,0xE6,0xC6,0xF4,0xF6,0xF2,0xFB,0xF9,0xFF,0xD6,0xDC,0xA2,0xA3,0xA5,'P','f',
                                                      0xE1,0xED,0xF3,0xFA,0xF1,0xD1,0xAA,0xBA,0xBF,'-',0xAC,0xBD,0xBC,0xA1,0xAB,0xBB };
                char u8b[4];
                int l, q;
                cp = c - 128 < (int)(sizeof back / sizeof back[0]) ? back[c - 128] : '?';
                l = utf8_put(u8b, cp);
                for (q = 0; q < l; q++) {
                    snprintf(t, sizeof t, "%%%c%c", "0123456789ABCDEF"[(u8)u8b[q] >> 4], "0123456789ABCDEF"[(u8)u8b[q] & 15]);
                    cat(out, &k, sizeof out, t);
                }
                continue;
            }
            snprintf(t, sizeof t, "%%%c%c", "0123456789ABCDEF"[cp >> 4], "0123456789ABCDEF"[cp & 15]);
            cat(out, &k, sizeof out, t);
        }
    }
    navigate(out, 1);
}

static void follow(const char *href)
{
    char abs[URLMAX * 2];
    const char *base = doc && doc->base ? doc->base : cur;
    if (!strncasecmp(href, "javascript:", 11) || !strncasecmp(href, "mailto:", 7) || !strncasecmp(href, "tel:", 4)) {
        scopy(status, "Ese enlace necesita JavaScript o otra aplicacion", sizeof status);
        redraw_status();
        return;
    }
    if (href[0] == '#') {                        /* ancla en la misma pagina */
        int y = page_anchor(&pg, href + 1);
        if (y >= 0) { scroll = y; clamp_scroll(); redraw(); }
        return;
    }
    url_resolve(base, href, abs, sizeof abs);
    {   /* misma pagina con otra #ancla: no se vuelve a bajar */
        size_t a = strcspn(abs, "#"), b = strcspn(cur, "#");
        if (abs[a] == '#' && a == b && !strncmp(abs, cur, a)) {
            int y = page_anchor(&pg, abs + a + 1);
            if (y >= 0) { scroll = y; clamp_scroll(); redraw(); }
            return;
        }
    }
    navigate(abs, 1);
}

static void do_key_scroll(int k)
{
    scroll += k == KEY_DOWN ? 48 : -48;
    clamp_scroll();
    redraw();
}

/* ================= formularios ================= */
static const char *ntype(Node *n)
{
    const char *t;
    if (!strcmp(n->name, "input")) { t = node_attr(n, "type"); return t ? t : "text"; }
    if (!strcmp(n->name, "button")) { t = node_attr(n, "type"); return t ? t : "submit"; }
    return n->name;                               /* textarea, select */
}
static int is_textish(Node *n)
{
    const char *t;
    if (!n || n->type) return 0;
    if (is_area(n)) return 1;
    if (strcmp(n->name, "input")) return 0;
    t = ntype(n);
    return !strcasecmp(t, "text") || !strcasecmp(t, "search") || !strcasecmp(t, "email") || !strcasecmp(t, "url") ||
           !strcasecmp(t, "password") || !strcasecmp(t, "number") || !strcasecmp(t, "tel");
}
static int is_ctrl(Node *n)
{
    return n && !n->type && (!strcmp(n->name, "input") || !strcmp(n->name, "button") ||
                             !strcmp(n->name, "textarea") || !strcmp(n->name, "select"));
}

static void set_attr(Node *n, const char *name, const char *val)
{
    int i;
    Attr *na;
    for (i = 0; i < n->nattrs; i++)
        if (!strcmp(n->attrs[i].name, name)) { n->attrs[i].val = val ? strdup(val) : 0; if (!val) n->attrs[i].name = "-"; return; }
    if (!val)
        return;
    na = malloc((size_t)(n->nattrs + 1) * sizeof(Attr));
    if (!na)
        return;
    for (i = 0; i < n->nattrs; i++) na[i] = n->attrs[i];
    na[i].name = strdup(name);
    na[i].val = strdup(val);
    n->attrs = na;
    n->nattrs++;
}

/* control bajo el raton (coordenadas de pagina) */
static Node *ctrl_at(int x, int y)
{
    int i;
    for (i = pg.n - 1; i >= 0; i--) {
        Item *t = &pg.it[i];
        if (t->kind == IT_RECT && is_ctrl(t->nd) && x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h)
            return t->nd;
    }
    return 0;
}

static void relayout(void)
{
    layout_doc(doc, &pg, vw(), 1.0f, 0);
    clamp_scroll();
}

/* lo escrito pasa al atributo value del nodo (y se vuelve a acomodar la pagina) */
static void commit_field(int lay)
{
    if (!fnode)
        return;
    fbuf[flen] = 0;
    set_attr(fnode, "value", fbuf);
    fnode = 0;
    if (lay)
        relayout();
}

static void focus_field(Node *n)
{
    const char *v = node_attr(n, "value");
    commit_field(1);
    fnode = n;
    flen = 0;
    if (v) { while (v[flen] && flen < FMAX - 8) { fbuf[flen] = v[flen]; flen++; } }
    fbuf[flen] = 0;
    addr_focus = 0;
    scopy(status, "Escribe; Enter = enviar el formulario, TAB = siguiente campo", sizeof status);
}

static Node *form_of(Node *n)
{
    for (; n; n = n->parent)
        if (!n->type && n->name && !strcmp(n->name, "form"))
            return n;
    return 0;
}

static void q_add(char *o, int *k, int max, const char *s, int n)
{
    int i;
    for (i = 0; i < n && *k < max - 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ') o[(*k)++] = '+';
        else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '*') o[(*k)++] = (char)c;
        else { o[(*k)++] = '%'; o[(*k)++] = "0123456789ABCDEF"[c >> 4]; o[(*k)++] = "0123456789ABCDEF"[c & 15]; }
    }
    o[*k] = 0;
}
static void q_pair(char *o, int *k, int max, const char *name, const char *val)
{
    if (*k > 0 && o[*k - 1] != '?') { if (*k < max - 2) o[(*k)++] = '&'; }
    q_add(o, k, max, name, (int)strlen(name));
    if (*k < max - 2) o[(*k)++] = '=';
    q_add(o, k, max, val, (int)strlen(val));
}

/* recorre el formulario y arma nombre=valor (como lo haria un navegador con GET) */
static void q_walk(Node *n, Node *btn, char *o, int *k, int max)
{
    Node *ch;
    for (ch = n->first; ch; ch = ch->next) {
        const char *nm, *t;
        if (ch->type) continue;
        nm = node_attr(ch, "name");
        if (is_ctrl(ch) && nm && *nm && !node_attr(ch, "disabled")) {
            t = ntype(ch);
            if (!strcmp(ch->name, "select")) {
                Node *op, *sel = 0, *first = 0;
                for (op = ch->first; op; op = op->next)
                    if (!op->type && !strcmp(op->name, "option")) { if (!first) first = op; if (node_attr(op, "selected")) sel = op; }
                if (!sel) sel = first;
                if (sel) {
                    const char *v = node_attr(sel, "value");
                    char buf[200]; int len = 0;
                    if (!v) { Node *tx; for (tx = sel->first; tx; tx = tx->next) if (tx->type == 1 && len + tx->tlen < 199) { memcpy(buf + len, tx->text, (size_t)tx->tlen); len += tx->tlen; } buf[len] = 0; v = buf; }
                    q_pair(o, k, max, nm, v);
                }
            } else if (!strcasecmp(t, "checkbox") || !strcasecmp(t, "radio")) {
                if (node_attr(ch, "checked")) { const char *v = node_attr(ch, "value"); q_pair(o, k, max, nm, v ? v : "on"); }
            } else if (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) {
                if (ch == btn) { const char *v = node_attr(ch, "value"); q_pair(o, k, max, nm, v ? v : ""); }
            } else if (strcasecmp(t, "button") && strcasecmp(t, "reset") && strcasecmp(t, "file")) {
                const char *v = node_attr(ch, "value");
                q_pair(o, k, max, nm, v ? v : "");
            }
        }
        if (strcmp(ch->name, "select"))
            q_walk(ch, btn, o, k, max);
    }
}

static void submit(Node *form, Node *btn)
{
    static char url[URLMAX * 2], act[URLMAX * 2];
    const char *a, *m, *base = doc && doc->base ? doc->base : cur;
    int k;
    commit_field(0);
    if (!form)
        return;
    a = btn && node_attr(btn, "formaction") ? node_attr(btn, "formaction") : node_attr(form, "action");
    m = node_attr(form, "method");
    if (a && *a) url_resolve(base, a, act, sizeof act);
    else scopy(act, cur, sizeof act);
    act[strcspn(act, "?#")] = 0;                  /* con GET la consulta se reemplaza */
    k = (int)strlen(act);
    memcpy(url, act, (size_t)k);
    url[k++] = '?';
    url[k] = 0;
    q_walk(form, btn, url, &k, URLMAX - 8);
    if (k > 0 && url[k - 1] == '?') url[--k] = 0;
    if (m && !strcasecmp(m, "post"))
        scopy(status, "(este formulario pide POST; COSMOS lo manda como GET)", sizeof status);
    navigate(url, 1);
}

/* TAB: siguiente campo de texto de la pagina */
static void next_field(void)
{
    int i, start = 0;
    Node *cur_f = fnode;
    if (cur_f)
        for (i = 0; i < pg.n; i++) if (pg.it[i].kind == IT_RECT && pg.it[i].nd == cur_f) { start = i + 1; break; }
    for (i = 0; i < pg.n; i++) {
        Item *t = &pg.it[(start + i) % pg.n];
        if (t->kind == IT_RECT && is_textish(t->nd) && t->nd != cur_f) {
            focus_field(t->nd);
            t = ctrl_rect(fnode);
            if (t && (t->y < scroll || t->y + t->h > scroll + vh())) { scroll = t->y - vh() / 3; clamp_scroll(); }
            redraw();
            return;
        }
    }
    if (cur_f) { commit_field(1); redraw(); }
}

/* teclas mientras un campo tiene el foco */
static void field_key(int k)
{
    if (k == 13 || k == 10) {
        if (is_area(fnode)) { if (flen < FMAX - 2) fbuf[flen++] = '\n'; }
        else { submit(form_of(fnode), 0); return; }
    } else if (k == 9) {
        next_field();
        return;
    } else if (k == 8 || k == 127) {
        while (flen > 0 && ((unsigned char)fbuf[flen - 1] & 0xC0) == 0x80) flen--;   /* caracter UTF-8 completo */
        if (flen > 0) flen--;
    } else if (k == KEY_UP || k == KEY_DOWN) {
        commit_field(1);
        do_key_scroll(k);
        return;
    } else if (k >= 32 && k != 127 && !(k >= 128 && k < 160 && k != 0x80 && k != 0x87)) {
        static const unsigned short back[] = { 0xC7,0xFC,0xE9,0xE2,0xE4,0xE0,0xE5,0xE7,0xEA,0xEB,0xE8,0xEF,0xEE,0xEC,0xC4,0xC5,
                                              0xC9,0xE6,0xC6,0xF4,0xF6,0xF2,0xFB,0xF9,0xFF,0xD6,0xDC,0xA2,0xA3,0xA5,'P','f',
                                              0xE1,0xED,0xF3,0xFA,0xF1,0xD1,0xAA,0xBA,0xBF,'-',0xAC,0xBD,0xBC,0xA1,0xAB,0xBB };
        int cp = k;
        if (k >= 'A' && k <= 'Z') cp = k + 32;    /* el sistema da MAYUSCULAS sin Shift: aqui al reves */
        else if (k >= 'a' && k <= 'z') cp = k - 32;
        else if (k == 0xA4) cp = 0xD1; else if (k == 0xA5) cp = 0xF1;     /* ñ/Ñ tambien al reves */
        else if (k == 0x87) cp = 0xC7; else if (k == 0x80) cp = 0xE7;
        else if (k >= 128) cp = k - 128 < (int)(sizeof back / sizeof back[0]) ? back[k - 128] : k == 0xF8 ? 0xB0 : k == 0xFA ? 0xB7 : '?';
        if (flen < FMAX - 6) flen += utf8_put(fbuf + flen, cp);
    } else
        return;
    redraw();
}

/* clic en un control de la pagina */
static void ctrl_click(Node *n)
{
    const char *t = ntype(n);
    if (is_textish(n)) { focus_field(n); redraw(); return; }
    if (!strcmp(n->name, "select")) {             /* sin menu desplegable: cada clic pasa a la siguiente opcion */
        Node *op, *sel = 0, *first = 0, *next = 0;
        int after = 0;
        for (op = n->first; op; op = op->next) {
            if (op->type || strcmp(op->name, "option")) continue;
            if (!first) first = op;
            if (after && !next) next = op;
            if (node_attr(op, "selected")) { sel = op; after = 1; }
        }
        if (!sel) { sel = first; next = first ? first->next : 0; while (next && (next->type || strcmp(next->name, "option"))) next = next->next; }
        if (!next) next = first;
        if (sel) set_attr(sel, "selected", 0);
        if (next) set_attr(next, "selected", "");
        commit_field(0);
        relayout();
        redraw();
        return;
    }
    if (!strcasecmp(t, "checkbox")) {
        set_attr(n, "checked", node_attr(n, "checked") ? 0 : "");
        commit_field(0); relayout(); redraw();
        return;
    }
    if (!strcasecmp(t, "radio")) {
        Node *f = form_of(n), *stack[64];
        const char *nm = node_attr(n, "name");
        int sp = 0;
        if (f && nm) {                            /* se apagan los demas del mismo grupo */
            stack[sp++] = f;
            while (sp) {
                Node *x = stack[--sp], *c2;
                for (c2 = x->first; c2; c2 = c2->next) {
                    if (c2->type) continue;
                    if (!strcmp(c2->name, "input") && node_attr(c2, "name") && !strcmp(node_attr(c2, "name"), nm))
                        set_attr(c2, "checked", 0);
                    if (sp < 64) stack[sp++] = c2;
                }
            }
        }
        set_attr(n, "checked", "");
        commit_field(0); relayout(); redraw();
        return;
    }
    if (!strcasecmp(t, "submit") || !strcasecmp(t, "image")) {
        submit(form_of(n), n);
        return;
    }
    if (!strcasecmp(t, "reset")) { scopy(status, "(Restablecer no hace nada aun)", sizeof status); redraw_status(); }
}

/* ================= callbacks de la ventana ================= */
static void back(void) { if (hpos > 0) { hpos--; navigate(hist[hpos], 0); } }
static void fwd(void) { if (hpos < hn - 1) { hpos++; navigate(hist[hpos], 0); } }

static __attribute__((noinline)) void do_start(struct wctx *c)
{
    Wc = c;
    closed = 0;
    PIX = c->pix; STR = c->stride; WW = c->w; WH = c->h;
    noclip();
    if (hpos < 0)
        navigate("cosmos:home", 1);
    else
        redraw();
}

static __attribute__((noinline)) void do_frame(struct wctx *c)
{
    Wc = c;
    if (busy || !pending)
        return;
    if (--pending == 0)
        do_load();
}

static __attribute__((noinline)) void do_key(struct wctx *c, int k)
{
    int n;
    Wc = c;
    if (busy || pending)
        return;
    if (fnode && !addr_focus) {
        field_key(k);
        return;
    }
    if (addr_focus) {
        n = (int)strlen(addr);
        if (k == 13 || k == 10) {
            addr_focus = 0;
            omnibox(addr);
            return;
        }
        if (k == 9) { addr_focus = 0; next_field(); return; }
        if (k == 8) {
            if (addr_fresh) addr[0] = 0, addr_fresh = 0;
            else if (n) addr[n - 1] = 0;
        } else if (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT) {
            addr_fresh = 0;
            if (k == KEY_UP || k == KEY_DOWN) { addr_focus = 0; }
            else return;
        } else if (k >= 32 && k != 127) {
            if (addr_fresh) { addr[0] = 0; n = 0; addr_fresh = 0; }
            if (k >= 'A' && k <= 'Z') k += 32;   /* el sistema da MAYUSCULAS sin Shift: aqui al reves */
            else if (k >= 'a' && k <= 'z') k -= 32;
            if (n < URLMAX - 1) { addr[n] = (char)k; addr[n + 1] = 0; }
        } else
            return;
        draw_bar();
        c->damage(c, 0, 0, WW, TOPH);
        return;
    }
    if (k == KEY_DOWN) scroll += 48;
    else if (k == KEY_UP) scroll -= 48;
    else if (k == ' ') scroll += vh() - 32;
    else if (k == KEY_LEFT || k == 8) { back(); return; }
    else if (k == KEY_RIGHT) { fwd(); return; }
    else if (k == 9) { next_field(); return; }    /* TAB: al primer campo de la pagina (buscadores) */
    else if (k == 13 || k == 10) { addr_focus = 1; addr_fresh = 1; draw_bar(); c->damage(c, 0, 0, WW, TOPH); return; }
    else if (k > 32 && k != 127) {               /* empezar a escribir = direccion nueva */
        addr_focus = 1;
        addr_fresh = 1;
        do_key(c, k);
        return;
    } else
        return;
    clamp_scroll();
    redraw();
}

static __attribute__((noinline)) void do_mouse(struct wctx *c, int x, int y, int btn, int prev)
{
    int press = (btn & 1) && !(prev & 1), release = !(btn & 1) && (prev & 1);
    Wc = c;
    if (busy || pending)
        return;
    if (sb_drag || (press && x >= vw() && y >= TOPH && y < WH - STATH)) {   /* barra de desplazamiento */
        if (press) sb_drag = 1;
        if (!(btn & 1)) { sb_drag = 0; return; }
        if (maxscroll() > 0) {
            scroll = (int)((long)(y - TOPH) * maxscroll() / vh());
            clamp_scroll();
            redraw();
        }
        return;
    }
    if (y < TOPH) {
        if (!release)
            return;
        if (x < 22) back();
        else if (x < 44) fwd();
        else if (x < 66) { if (cur[0]) navigate(cur, 0); }
        else if (x < 88) navigate("cosmos:home", 1);
        else if (x >= AX) { if (fnode) commit_field(1); addr_focus = 1; addr_fresh = 1; redraw(); }
        return;
    }
    if (y >= WH - STATH)
        return;
    {
        Item *it = doc ? page_hit(&pg, x, y - TOPH + scroll) : 0;
        Node *cn = doc && release ? ctrl_at(x, y - TOPH + scroll) : 0;
        if (cn) {
            addr_focus = 0;
            ctrl_click(cn);
            return;
        }
        if (release && fnode) {                  /* clic fuera del campo: se suelta el foco */
            commit_field(1);
            redraw();
        }
        if (release && it && it->href) {
            if (addr_focus) addr_focus = 0;
            follow(it->href);
            return;
        }
        if (release && addr_focus) {
            addr_focus = 0;
            draw_bar();
            c->damage(c, 0, 0, WW, TOPH);
        }
        /* mover: el enlace bajo el raton va a la barra de estado */
        {
            const char *h = it && it->href ? it->href : "";
            char abs[URLMAX];
            if (*h) {
                url_resolve(doc && doc->base ? doc->base : cur, h, abs, sizeof abs);
                h = abs;
            }
            if (strncmp(h, hover, URLMAX - 1)) {
                scopy(hover, h, URLMAX);
                redraw_status();
            }
        }
    }
}

static __attribute__((noinline)) void do_resize(struct wctx *c)
{
    Wc = c;
    PIX = c->pix; STR = c->stride; WW = c->w; WH = c->h;
    noclip();
    if (doc && !busy) {
        long old = pg.height;
        commit_field(0);
        old = pg.height ? (long)scroll * 1000 / (pg.height > 1 ? pg.height : 1) : 0;
        css_viewport_w = vw();
        css_viewport_h = vh();
        doc_style(doc);                          /* @media y vw/vh dependen del ancho */
        layout_doc(doc, &pg, vw(), 1.0f, 0);
        scroll = (int)(old * pg.height / 1000);
        clamp_scroll();
    }
    redraw();
}

static void do_stop(struct wctx *c) { (void)c; closed = 1; Wc = 0; }

/* envoltorios: SSE encendido antes de entrar al motor (sin instrucciones SSE aqui) */
static void w_start(struct wctx *c) { sse_on(); do_start(c); }
static void w_frame(struct wctx *c) { sse_on(); do_frame(c); }
static void w_key(struct wctx *c, int k) { sse_on(); do_key(c, k); }
static void w_mouse(struct wctx *c, int x, int y, int b, int p) { sse_on(); do_mouse(c, x, y, b, p); }
static void w_resize(struct wctx *c) { sse_on(); do_resize(c); }

static const struct wapp app = {
    "COSMOS", 760, 520, 280, 160, 1, w_start, w_frame, w_key, w_mouse, w_resize, do_stop
};

int mod_main(const struct kapi *api)
{
    K = api;
    K->wm_publish(&app);
    return MOD_WINAPP;
}
