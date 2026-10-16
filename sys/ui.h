/* ui.h - utilidades graficas comunes a los modulos de /SYS (escritorio y apps)
 *
 * Todo se dibuja en superficies (struct surf): un buffer de indices de color del C64 (0-15,
 * 255 = transparente) con su ancho de fila. Las apps de ventana dibujan en la superficie de su
 * ventana; el escritorio mezcla las superficies y el kernel las pasa a la pantalla.
 */
#ifndef UI_H
#define UI_H
#include "kapi.h"

typedef unsigned char u8;
typedef unsigned int  u32;
typedef unsigned long size_t;
void *memset(void *dst, int c, size_t n);       /* lib.c */
void *memcpy(void *dst, const void *src, size_t n);

static const struct kapi *K;

#define C_BLACK 0
#define C_WHITE 1
#define C_RED   2
#define C_CYAN  3
#define C_PURPLE 4
#define C_GREEN 5
#define C_BLUE  6
#define C_YELLOW 7
#define C_ORANGE 8
#define C_BROWN 9
#define C_LRED  10
#define C_DGREY 11
#define C_GREY  12
#define C_LGREEN 13
#define C_LBLUE 14
#define C_LGREY 15
#define C_NONE  255

#define FONT ((const u8 *)FONT_ADDR)

struct surf { u8 *p; int stride, w, h; };

static inline struct surf surf_of(const struct wctx *c)
{
    struct surf s;
    s.p = c->pix;
    s.stride = c->stride;
    s.w = c->w;
    s.h = c->h;
    return s;
}

static inline int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static inline void scopy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static inline int ends_with(const char *s, const char *suf)
{
    int a = slen(s), b = slen(suf), i;
    if (b > a) return 0;
    for (i = 0; i < b; i++) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (x != y) return 0;
    }
    return 1;
}

static inline char *utoa(char *o, unsigned v)
{
    char t[12];
    int i = 0;
    if (!v) t[i++] = '0';
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    while (i) *o++ = t[--i];
    *o = 0;
    return o;
}

static inline int inside(int px, int py, int x, int y, int w, int h)
{
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* ---- primitivas sobre una superficie (todas recortan) ---- */

static inline void s_rect(const struct surf *s, int x, int y, int w, int h, int c)
{
    int j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (j = 0; j < h; j++) {
        u8 *d = s->p + (y + j) * s->stride + x;
        int i;
        for (i = 0; i < w; i++)
            d[i] = (u8)c;
    }
}

static inline void s_px(const struct surf *s, int x, int y, int c)
{
    if (x >= 0 && y >= 0 && x < s->w && y < s->h)
        s->p[y * s->stride + x] = (u8)c;
}

static inline void s_frame(const struct surf *s, int x, int y, int w, int h, int c)
{
    s_rect(s, x, y, w, 1, c);
    s_rect(s, x, y + h - 1, w, 1, c);
    s_rect(s, x, y, 1, h, c);
    s_rect(s, x + w - 1, y, 1, h, c);
}

/* un caracter de 8x16; bg < 0 = transparente */
static inline void s_glyph(const struct surf *s, int x, int y, int ch, int fg, int bg)
{
    const u8 *g = FONT + (ch & 255) * 16;
    int i, j;
    for (i = 0; i < 16; i++) {
        u8 bits = g[i];
        for (j = 0; j < 8; j++) {
            if (bits & (0x80 >> j))
                s_px(s, x + j, y + i, fg);
            else if (bg >= 0)
                s_px(s, x + j, y + i, bg);
        }
    }
}

static inline void s_text(const struct surf *s, int x, int y, const char *t, int fg, int bg)
{
    for (; *t; t++, x += 8)
        s_glyph(s, x, y, (u8)*t, fg, bg);
}

static inline void s_text_center(const struct surf *s, int cx, int y, const char *t, int fg, int bg)
{
    s_text(s, cx - slen(t) * 4, y, t, fg, bg);
}

/* boton con relieve */
static inline void s_button(const struct surf *s, int x, int y, int w, int h, const char *label,
                            int bg, int fg, int down)
{
    s_rect(s, x, y, w, h, C_DGREY);
    s_rect(s, x, y, w - 1, h - 1, down ? C_GREY : C_WHITE);
    s_rect(s, x + 1, y + 1, w - 2, h - 2, down ? C_GREY : C_LGREY);
    s_rect(s, x + 2, y + 2, w - 4, h - 4, bg);
    s_text_center(s, x + w / 2 + (down ? 1 : 0), y + (h - 16) / 2 + (down ? 1 : 0), label, fg, bg);
}

/* ---- iconos .icb (ICono de BASIC-OS): texto, una fila por linea, un digito hex por bloque ---- *
 *   ICB             <- primera linea (firma)
 *   ....3333....    <- 0-F = color del C64, '.' = transparente
 */
#define ICB_MAX 24
struct icon {
    int w, h;
    u8 px[ICB_MAX * ICB_MAX];           /* 0-15 color, 255 transparente */
    int ok;
};

static inline int icb_parse(struct icon *ic, const char *buf, int n)
{
    int i = 0, x = 0, y = 0;
    ic->ok = 0;
    ic->w = ic->h = 0;
    if (n < 4 || buf[0] != 'I' || buf[1] != 'C' || buf[2] != 'B')
        return 0;
    while (i < n && buf[i] != '\n')
        i++;
    i++;
    for (; i < n && y < ICB_MAX; i++) {
        char c = buf[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            if (x > ic->w) ic->w = x;
            if (x) y++;
            x = 0;
            continue;
        }
        if (x < ICB_MAX) {
            u8 v = 255;
            if (c >= '0' && c <= '9') v = (u8)(c - '0');
            else if (c >= 'A' && c <= 'F') v = (u8)(c - 'A' + 10);
            else if (c >= 'a' && c <= 'f') v = (u8)(c - 'a' + 10);
            ic->px[y * ICB_MAX + x] = v;
            x++;
        }
    }
    if (x) {
        if (x > ic->w) ic->w = x;
        y++;
    }
    ic->h = y;
    ic->ok = ic->w > 0 && ic->h > 0;
    return ic->ok;
}

static inline int icon_load(struct icon *ic, const char *name)
{
    static char buf[1200];
    char path[40] = "SYS:";
    unsigned total = 0;
    int n;
    scopy(path + 4, name, 30);
    n = K->read(path, buf, (int)sizeof buf, &total);
    if (n <= 0) {
        ic->ok = 0;
        return 0;
    }
    return icb_parse(ic, buf, n);
}

static inline void icon_draw(const struct surf *s, const struct icon *ic, int x, int y, int blk)
{
    int i, j;
    if (!ic->ok) {                      /* sin icono: un cuadro gris */
        s_rect(s, x, y, 16 * blk, 16 * blk, C_GREY);
        return;
    }
    for (j = 0; j < ic->h; j++)
        for (i = 0; i < ic->w; i++) {
            u8 v = ic->px[j * ICB_MAX + i];
            if (v < 16)
                s_rect(s, x + i * blk, y + j * blk, blk, blk, v);
        }
}

#endif
