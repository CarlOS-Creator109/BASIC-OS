/* bmg.c - BMG (BASIC-OS Motor Game): sprites y cuadros para juegos en BASIC
 *
 * Desde BASIC:
 *   SPRITE DEF n, ancho, alto [, z]  define el sprite n (0-31), hasta 32x32 pixeles; queda vacio. z = zoom (1-4):
 *                                    cada pixel del dibujo se ve de z x z (un 16x16 con z=2 ocupa 32x32)
 *   SPRITE ROW n, fila, "0011..."   una fila del dibujo: un digito hex por pixel = color del C64; '.' o ' ' = nada
 *   SPRITE COPY destino, origen      copia el dibujo (para tener varios enemigos iguales)
 *   SPRITE n, x, y                   lo pone en x,y (se ve en el siguiente FRAME)
 *   SPRITE OFF n   /   SPRITE OFF    lo quita (o todos)
 *   FRAME  [fps]                     dibuja los sprites donde quedaron y espera al siguiente cuadro (30 por segundo,
 *                                    o los que digas). Lo que habia debajo de cada sprite se guarda y se repone solo:
 *                                    el fondo hecho con PLOT/LINE/BOX no se borra
 *   HIT(a, b)                        -1 si los sprites a y b se tocan (pixel a pixel), 0 si no
 *   KEYDOWN(k)                       -1 si la tecla esta apretada ahora (k = ASC de la letra, 32 espacio, 13 Enter,
 *                                    145/17/157/29 flechas arriba/abajo/izquierda/derecha) -- ver kbd_held()
 *
 * Funciona a pantalla completa (sobre el framebuffer) y en una terminal del escritorio (sobre el plano de PLOT de la
 * ventana). RUN quita los sprites de la corrida anterior; CLS olvida lo que habia debajo (la pantalla ya es otra).
 */
#include "kernel.h"
#include "kapi.h"

#define NSPR    32
#define SMAX    32
#define ZMAX    4
#define EMAX    (SMAX * ZMAX)           /* lado del dibujo ya ampliado */
#define SAVE_SZ 0x40000u                /* lo que hay debajo: 128x128 con escala de pantalla 2 y 4 bytes por pixel */
#define EPIX_OFF (NSPR * SAVE_SZ)       /* en el banco, detras de lo guardado: los dibujos ampliados */

#define BMG_RESET 0
#define BMG_DEF   1
#define BMG_ROW   2
#define BMG_POS   3
#define BMG_OFF   4
#define BMG_COPY  5
#define BMG_FRAME 6
#define BMG_HIT   7
#define BMG_CLS   8

void plat_sleep(int ms);                 /* kernel.c: espera con ESC */

struct spr {
    int def, on, drawn;
    int w, h, z, x, y;                   /* w, h: del dibujo; en pantalla mide w*z x h*z */
    int dx, dy, dw, dh;                  /* lo que se dibujo (ya recortado), para reponer el fondo */
    u8 pix[SMAX * SMAX];                 /* 255 = transparente */
};

static struct spr S[NSPR];
static u8 *save_base;
static unsigned fps = 30, next_ms;

static u8 *save_of(int i) { return save_base + (unsigned long)i * SAVE_SZ; }
static u8 *epix_of(int i) { return save_base + EPIX_OFF + (unsigned long)i * EMAX * EMAX; }

/* el dibujo ampliado (lo que se pinta) */
static void expand(int i)
{
    struct spr *s = &S[i];
    u8 *e = epix_of(i);
    int x, y, W = s->w * s->z, H = s->h * s->z;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            e[y * EMAX + x] = s->pix[(y / s->z) * SMAX + x / s->z];
}

static int hexd(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void restore_all(void)
{
    int i;
    for (i = NSPR - 1; i >= 0; i--) {
        struct spr *s = &S[i];
        if (!s->drawn) continue;
        if (vga_rect_bytes(s->dw, s->dh) <= (int)SAVE_SZ)
            vga_rect_set(s->dx, s->dy, s->dw, s->dh, save_of(i));
        s->drawn = 0;
    }
}

static void draw_all(void)
{
    int i, W = vga_gfx_w(), H = vga_gfx_h();
    for (i = 0; i < NSPR; i++) {
        struct spr *s = &S[i];
        int x = s->x, y = s->y, w = s->w * s->z, h = s->h * s->z, ox = 0, oy = 0;
        if (!s->def || !s->on) continue;
        if (x < 0) { ox = -x; w += x; x = 0; }
        if (y < 0) { oy = -y; h += y; y = 0; }
        if (x + w > W) w = W - x;
        if (y + h > H) h = H - y;
        if (w <= 0 || h <= 0) continue;
        if (vga_rect_bytes(w, h) > (int)SAVE_SZ)
            continue;
        vga_rect_get(x, y, w, h, save_of(i));
        vga_sprite(x, y, w, h, epix_of(i) + oy * EMAX + ox, EMAX);
        s->dx = x; s->dy = y; s->dw = w; s->dh = h;
        s->drawn = 1;
    }
}

static int hit(int a, int b)
{
    struct spr *p = &S[a], *q = &S[b];
    int x0, y0, x1, y1, x, y;
    if (a == b || !p->def || !q->def || !p->on || !q->on) return 0;
    int pw = p->w * p->z, ph = p->h * p->z, qw = q->w * q->z, qh = q->h * q->z;
    x0 = p->x > q->x ? p->x : q->x;
    y0 = p->y > q->y ? p->y : q->y;
    x1 = p->x + pw < q->x + qw ? p->x + pw : q->x + qw;
    y1 = p->y + ph < q->y + qh ? p->y + ph : q->y + qh;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            if (p->pix[((y - p->y) / p->z) * SMAX + (x - p->x) / p->z] != 255 &&
                q->pix[((y - q->y) / q->z) * SMAX + (x - q->x) / q->z] != 255)
                return 1;
    return 0;
}

/* <0 = error: -1 numero fuera de rango, -2 sin memoria, -3 sin modo grafico */
int bmg_api(int op, int a, int b, int c, const char *str)
{
    int i;
    switch (op) {
    case BMG_RESET:                         /* RUN: se borran los sprites que dejo la corrida anterior */
        if (vga_gfx_w() > 0) restore_all();
        for (i = 0; i < NSPR; i++) S[i].def = S[i].on = S[i].drawn = 0;
        fps = 30;
        next_ms = 0;
        return 0;
    case BMG_CLS:
        for (i = 0; i < NSPR; i++) S[i].drawn = 0;
        return 0;
    case BMG_DEF: {
        int z = str ? str[0] : 1;           /* el zoom llega como primer byte de str (basic.c) */
        if (a < 0 || a >= NSPR || b < 1 || b > SMAX || c < 1 || c > SMAX || z < 1 || z > ZMAX) return -1;
        if (!save_base) {                   /* fondo guardado y dibujos ampliados: en un banco de RAM (la primera vez) */
            u64 p = ram_api(RAM_ALLOC, 0, 1, "BMG");
            if (!p) return -2;
            save_base = (u8 *)(unsigned long)p;
        }
        if (S[a].drawn) restore_all();
        S[a].def = 1; S[a].on = 0; S[a].w = b; S[a].h = c; S[a].z = z;
        for (i = 0; i < SMAX * SMAX; i++) S[a].pix[i] = 255;
        expand(a);
        return 0;
    }
    case BMG_ROW:
        if (a < 0 || a >= NSPR || !S[a].def || b < 0 || b >= S[a].h) return -1;
        for (i = 0; str && str[i] && i < S[a].w; i++) {
            int v = hexd((unsigned char)str[i]);
            S[a].pix[b * SMAX + i] = v < 0 ? 255 : (u8)v;
        }
        expand(a);
        return 0;
    case BMG_COPY:
        if (a < 0 || a >= NSPR || b < 0 || b >= NSPR || !S[b].def) return -1;
        if (S[a].drawn) restore_all();
        S[a].def = 1; S[a].on = 0; S[a].w = S[b].w; S[a].h = S[b].h; S[a].z = S[b].z;
        for (i = 0; i < SMAX * SMAX; i++) S[a].pix[i] = S[b].pix[i];
        expand(a);
        return 0;
    case BMG_POS:
        if (a < 0 || a >= NSPR || !S[a].def) return -1;
        S[a].x = b; S[a].y = c; S[a].on = 1;
        return 0;
    case BMG_OFF:
        if (a < -1 || a >= NSPR) return -1;
        for (i = 0; i < NSPR; i++)
            if (a < 0 || i == a) S[i].on = 0;
        return 0;
    case BMG_HIT:
        if (a < 0 || a >= NSPR || b < 0 || b >= NSPR) return -1;
        return hit(a, b);
    case BMG_FRAME: {
        unsigned now, step;
        if (vga_gfx_w() <= 0) return -3;
        if (a < 0 || a > 1000) return -1;
        if (a) fps = (unsigned)a;
        restore_all();
        draw_all();
        fb_flush();
        step = 1000 / fps;
        now = plat_ms_now();
        if (!next_ms || (int)(now - next_ms) > 250)
            next_ms = now;                  /* primera vez o ibamos muy atrasados: no correr para alcanzar */
        next_ms += step;
        if ((int)(next_ms - now) > 0)
            plat_sleep((int)(next_ms - now));
        return 0;
    }
    }
    return -1;
}
