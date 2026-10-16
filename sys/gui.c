/* gui.c - escritorio y gestor de ventanas de PC BASIC (modulo /SYS/GUI.SYS)
 *
 * Las ventanas se ponen ENCIMA de lo que ya hay en pantalla (la terminal del kernel sigue
 * debajo, en la capa FB0). Cada ventana tiene su propia superficie; el escritorio las mezcla en
 * el plano del escritorio y el kernel compone todas las capas:
 *
 *   FB0 terminal  <  FB1 dibujo de programas  <  plano del escritorio  <  FB2  <  FB3
 *
 * Funciones: ventanas que se mueven, se agrandan, se maximizan y se cierran; terminal BASIC
 * en ventana (con su propio plano de PLOT); apps de ventana (.APP con struct wapp); fondos
 * animados; barra de tareas con reloj. El kernel llama a wm_hook() ~60 veces por segundo desde
 * cualquier espera, asi las ventanas siguen vivas aunque un programa BASIC este corriendo.
 */
#include "ui.h"
#include "mod_head.h"

#define MAXW   10
#define BD     3                        /* borde de la ventana */
#define TH     20                       /* barra de titulo */
#define TBAR   24                       /* barra de tareas */
#define ICON_W 96
#define ICON_H 84
#define NBG    6

/* ---- estado ---- */
static int SW, SH;
static u8 *desk;
static u8 *pool;
static unsigned slot_sz;
static int nslots;
static u8 slot_used[2 * MAXW + 2];

struct win {
    int used;
    int x, y, w, h;                     /* rectangulo exterior */
    int minw, minh;                     /* minimo de la zona interior */
    int resizable, maxed, rx, ry, rw, rh;
    u8 *surf, *gfx;
    char title[32], appname[16];
    const struct wapp *app;
    struct wctx ctx;
    int term;                           /* 0 = app; 1 o 2 = consola del kernel */
    char line[128], last[128];
    int ln, busy, closing, cur_on;
    unsigned cur_t;
};
static struct win wins[MAXW];
static int zord[MAXW], nz;
static int focus_w = -1;

/* recorte de dibujo */
static int cl_x0, cl_y0, cl_x1, cl_y1;
/* zona sucia del escritorio */
static int dm_x0, dm_y0, dm_x1, dm_y1;

static int mx, my, mb, pmb;
static int mode, mwin, gx, gy, gw0, gh0, rs_part;
enum { M_NONE, M_MOVE, M_RESIZE, M_CAPTURE };
enum { P_NONE, P_CLIENT, P_TITLE, P_CLOSE, P_MAX, P_RR, P_RB, P_RC };

static int bgmode = 1;                  /* 0 = liso, 1 = azul... (siempre opacos) */
static unsigned bg_t, bg_last;
static int hover_icon = -1;
static int quit;
static char pend[128];
static int pend_set;
static unsigned last_service;
static int in_service;
static int last_sec = -1;

static const char *const bgname[NBG] = { "LISO", "AZUL", "PLASMA", "ESTRELLAS", "BARRAS", "FUEGO" };

struct item { const char *label, *icon, *app; char key; };
static const struct item items[] = {
    { "TERMINAL",    "TERM.ICB",  0,           'T' },
    { "PONG",        "PONG.ICB",  "PONG.APP",  'P' },
    { "CALCULADORA", "CALC.ICB",  "CALC.APP",  'C' },
    { "ARCHIVOS",    "FILES.ICB", "FILES.APP", 'F' },
    { "DOOM",        "DOOM.ICB",  "DOOM.APP",  'D' },
};
#define NITEMS ((int)(sizeof items / sizeof items[0]))
static struct icon icons[NITEMS];

/* ---- zona sucia ---- */

static void desk_dirty(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    if (w <= 0 || h <= 0)
        return;
    if (dm_x1 <= dm_x0) {
        dm_x0 = x; dm_y0 = y; dm_x1 = x + w; dm_y1 = y + h;
        return;
    }
    if (x < dm_x0) dm_x0 = x;
    if (y < dm_y0) dm_y0 = y;
    if (x + w > dm_x1) dm_x1 = x + w;
    if (y + h > dm_y1) dm_y1 = y + h;
}

/* ---- superficies (reparto de la memoria que cede el kernel) ---- */

static u8 *slot_alloc(void)
{
    int i;
    for (i = 0; i < nslots; i++)
        if (!slot_used[i]) {
            slot_used[i] = 1;
            return pool + (unsigned)i * slot_sz;
        }
    return 0;
}

static void slot_free(u8 *p)
{
    if (p)
        slot_used[(unsigned)(p - pool) / slot_sz] = 0;
}

/* ---- dibujo en el plano del escritorio, con recorte ---- */

static void d_rect(int x, int y, int w, int h, int c)
{
    int j, i, x1 = x + w, y1 = y + h;
    if (x < cl_x0) x = cl_x0;
    if (y < cl_y0) y = cl_y0;
    if (x1 > cl_x1) x1 = cl_x1;
    if (y1 > cl_y1) y1 = cl_y1;
    if (x1 <= x || y1 <= y)
        return;
    for (j = y; j < y1; j++) {
        u8 *d = desk + j * SW + x;
        for (i = 0; i < x1 - x; i++)
            d[i] = (u8)c;
    }
}

static void d_glyph(int x, int y, int ch, int fg, int bg)
{
    const u8 *g = FONT + (ch & 255) * 16;
    int i, j;
    if (x >= cl_x1 || x + 8 <= cl_x0 || y >= cl_y1 || y + 16 <= cl_y0)
        return;
    for (i = 0; i < 16; i++) {
        int yy = y + i;
        if (yy < cl_y0 || yy >= cl_y1)
            continue;
        for (j = 0; j < 8; j++) {
            int xx = x + j;
            if (xx < cl_x0 || xx >= cl_x1)
                continue;
            if (g[i] & (0x80 >> j))
                desk[yy * SW + xx] = (u8)fg;
            else if (bg >= 0)
                desk[yy * SW + xx] = (u8)bg;
        }
    }
}

static void d_text(int x, int y, const char *s, int fg, int bg)
{
    for (; *s; s++, x += 8)
        d_glyph(x, y, (u8)*s, fg, bg);
}

static void d_bevel(int x, int y, int w, int h, int bg, int down)
{
    d_rect(x, y, w, h, C_DGREY);
    d_rect(x, y, w - 1, h - 1, down ? C_GREY : C_WHITE);
    d_rect(x + 1, y + 1, w - 2, h - 2, down ? C_GREY : C_LGREY);
    d_rect(x + 2, y + 2, w - 4, h - 4, bg);
}

/* ---- fondos animados ---- */

static int isin(int a)                   /* seno de 256 pasos por vuelta, -127..127 */
{
    int s;
    a &= 255;
    if (a < 128) {
        s = a * (128 - a);
        return s * 127 / 4096;
    }
    a -= 128;
    s = a * (128 - a);
    return -(s * 127 / 4096);
}

static unsigned rng = 12345;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return (rng >> 16) & 0x7FFF;
}

static const u8 ramp[16] = { 6, 14, 3, 13, 7, 1, 7, 8, 10, 2, 4, 6, 14, 3, 13, 5 };

#define NSTAR 260
struct star { int x, y, z; };
static struct star stars[NSTAR];
#define FWMAX 400
#define FHMAX 300
static u8 fire[FHMAX][FWMAX];
static int fw, fh;
static const u8 heat[16] = { 0, 0, 9, 9, 2, 2, 10, 8, 8, 7, 7, 15, 1, 1, 1, 1 };

static void star_reset(struct star *s, int first)
{
    s->x = (int)(rnd() % 2001) - 1000;
    s->y = (int)(rnd() % 1201) - 600;
    s->z = first ? (int)(rnd() % 1000) + 1 : 1000;
}

static void bg_init(void)
{
    int i;
    for (i = 0; i < NSTAR; i++)
        star_reset(&stars[i], 1);
    fw = SW / 4 + 1;
    fh = SH / 4 + 1;
    if (fw > FWMAX) fw = FWMAX;
    if (fh > FHMAX) fh = FHMAX;
    for (i = 0; i < FHMAX * FWMAX; i++)
        ((u8 *)fire)[i] = 0;
}

static void bg_step(void)                /* avanza la animacion un cuadro */
{
    int i, x, y;
    if (bgmode == 3) {
        for (i = 0; i < NSTAR; i++) {
            stars[i].z -= 14;
            if (stars[i].z < 4)
                star_reset(&stars[i], 0);
        }
    } else if (bgmode == 5) {
        for (x = 0; x < fw; x++)
            fire[fh - 1][x] = (u8)((rnd() & 3) ? 255 : (rnd() & 127));
        for (y = fh - 2; y >= 0; y--)                    /* de abajo hacia arriba: la llama sube entera */
            for (x = 0; x < fw; x++) {
                int a = fire[y + 1][x], b = fire[y + 1][x > 0 ? x - 1 : x],
                    c = fire[y + 1][x + 1 < fw ? x + 1 : x], d = y + 2 < fh ? fire[y + 2][x] : a;
                int v = ((a + b + c + d) >> 2) - (int)(rnd() % 7);
                fire[y][x] = (u8)(v < 0 ? 0 : v);
            }
    }
}

static void bg_draw(void)
{
    int x, y, bx0 = cl_x0 & ~3, by0 = cl_y0 & ~3;
    int t = (int)bg_t;
    switch (bgmode) {
    case 0:
        d_rect(cl_x0, cl_y0, cl_x1 - cl_x0, cl_y1 - cl_y0, C_DGREY);  /* liso: la terminal de abajo nunca se ve */
        break;
    case 1:
        d_rect(cl_x0, cl_y0, cl_x1 - cl_x0, cl_y1 - cl_y0, C_BLUE);
        break;
    case 2:
        for (y = by0; y < cl_y1; y += 4)
            for (x = bx0; x < cl_x1; x += 4) {
                int bx = x >> 2, by = y >> 2;
                int v = isin(bx * 3 + t) + isin(by * 5 + t * 2) + isin((bx + by) * 2 + t * 3) +
                        isin(((bx * bx + by * by) >> 5) + t);
                d_rect(x, y, 4, 4, ramp[((v + 508) * 16 / 1017) & 15]);
            }
        break;
    case 3: {
        int i;
        d_rect(cl_x0, cl_y0, cl_x1 - cl_x0, cl_y1 - cl_y0, C_BLACK);
        for (i = 0; i < NSTAR; i++) {
            int sx = SW / 2 + stars[i].x * 160 / stars[i].z, sy = SH / 2 + stars[i].y * 160 / stars[i].z;
            int sz = stars[i].z < 250 ? 4 : (stars[i].z < 600 ? 3 : 2);
            int c = stars[i].z < 250 ? C_WHITE : (stars[i].z < 600 ? C_LGREY : C_GREY);
            d_rect(sx, sy, sz, sz, c);
        }
        break;
    }
    case 4:
        for (y = cl_y0; y < cl_y1; y++) {
            int p = (y + (isin(y * 2 + t * 3) >> 2) + isin(t) / 2) & 63;
            int c;
            if (p < 6)       c = C_BLUE;
            else if (p < 12) c = C_LBLUE;
            else if (p < 18) c = C_CYAN;
            else if (p < 24) c = C_WHITE;
            else if (p < 30) c = C_CYAN;
            else if (p < 36) c = C_LBLUE;
            else if (p < 42) c = C_BLUE;
            else             c = C_BLACK;
            d_rect(cl_x0, y, cl_x1 - cl_x0, 1, c);
        }
        break;
    default:
        for (y = by0; y < cl_y1; y += 4)
            for (x = bx0; x < cl_x1; x += 4) {
                int gx2 = x >> 2, gy2 = y >> 2;
                if (gx2 < fw && gy2 < fh)
                    d_rect(x, y, 4, 4, heat[fire[gy2][gx2] >> 4]);
                else
                    d_rect(x, y, 4, 4, C_BLACK);
            }
        break;
    }
}

/* ---- iconos y barra de tareas ---- */

static void icon_pos(int i, int *x, int *y)
{
    int rows = (SH - TBAR - 10) / (ICON_H + 6);      /* si no caben en una columna, pasan a la siguiente */
    if (rows < 1)
        rows = 1;
    *x = 10 + (i / rows) * (ICON_W + 6);
    *y = 10 + (i % rows) * (ICON_H + 6);
}

static void icons_draw(void)
{
    int i, x, y;
    for (i = 0; i < NITEMS; i++) {
        icon_pos(i, &x, &y);
        if (x >= cl_x1 || x + ICON_W <= cl_x0 || y >= cl_y1 || y + ICON_H <= cl_y0)
            continue;
        d_rect(x, y, ICON_W, ICON_H, i == hover_icon ? C_LBLUE : C_BLUE);
        d_rect(x, y, ICON_W, 1, C_LGREY);
        d_rect(x, y, 1, ICON_H, C_LGREY);
        {   /* el icono se dibuja con bloques de 3x3, recortado */
            const struct icon *ic = &icons[i];
            int bx = x + (ICON_W - 16 * 3) / 2, by = y + 6, a, b;
            if (!ic->ok)
                d_rect(bx, by, 48, 48, C_GREY);
            else
                for (b = 0; b < ic->h; b++)
                    for (a = 0; a < ic->w; a++) {
                        u8 v = ic->px[b * ICB_MAX + a];
                        if (v < 16)
                            d_rect(bx + a * 3, by + b * 3, 3, 3, v);
                    }
        }
        {
            int len = 0;
            while (items[i].label[len])
                len++;
            d_text(x + ICON_W / 2 - len * 4, y + 6 + 48 + 6, items[i].label, C_WHITE, hover_icon == i ? C_LBLUE : C_BLUE);
        }
    }
}

static int rtc_read(int reg)
{
    __asm__ volatile("outb %0, $0x70" : : "a"((u8)reg));
    u8 v;
    __asm__ volatile("inb $0x71, %0" : "=a"(v));
    return v;
}

static void clock_text(char *t)
{
    int b = rtc_read(0x0B), h = rtc_read(4), m = rtc_read(2);
    if (!(b & 4)) {
        h = (h & 15) + (h >> 4) * 10;
        m = (m & 15) + (m >> 4) * 10;
    }
    t[0] = (char)('0' + h / 10 % 10);
    t[1] = (char)('0' + h % 10);
    t[2] = ':';
    t[3] = (char)('0' + m / 10 % 10);
    t[4] = (char)('0' + m % 10);
    t[5] = 0;
}

/* geometria de la barra de tareas */
#define TB_BTN_W 112
static int tb_bg_x(void)   { return SW - 64 - 6 - 70 - 6 - 160; }
static int tb_clock_x(void) { return SW - 64 - 6 - 70; }
static int tb_exit_x(void) { return SW - 64 - 4; }

static void taskbar_draw(void)
{
    int y = SH - TBAR, i, x = 6;
    char t[8], lab[32];
    if (y >= cl_y1)
        return;
    d_rect(0, y, SW, TBAR, C_LGREY);
    d_rect(0, y, SW, 1, C_WHITE);
    for (i = 0; i < nz; i++) {
        struct win *w = &wins[zord[i]];
        int n = 0;
        while (w->title[n] && n < 12) { lab[n] = w->title[n]; n++; }
        lab[n] = 0;
        if (x + TB_BTN_W > tb_bg_x() - 4)
            break;
        d_bevel(x, y + 2, TB_BTN_W, TBAR - 4, zord[i] == focus_w ? C_GREY : C_LGREY, zord[i] == focus_w);
        d_text(x + 6, y + 4, lab, C_BLACK, -1);
        x += TB_BTN_W + 4;
    }
    {
        char s[32] = "FONDO:";
        int n = 6, k = 0;
        while (bgname[bgmode][k] && n < 30)
            s[n++] = bgname[bgmode][k++];
        s[n] = 0;
        d_bevel(tb_bg_x(), y + 2, 160, TBAR - 4, C_LGREY, 0);
        d_text(tb_bg_x() + 8, y + 4, s, C_BLACK, -1);
    }
    clock_text(t);
    d_text(tb_clock_x() + 14, y + 4, t, C_BLACK, -1);
    d_bevel(tb_exit_x(), y + 2, 60, TBAR - 4, C_LRED, 0);
    d_text(tb_exit_x() + 6, y + 4, "SALIR", C_BLACK, -1);
}

/* ---- ventanas ---- */

static int win_at(int px, int py)
{
    int i;
    for (i = nz - 1; i >= 0; i--) {
        struct win *w = &wins[zord[i]];
        if (inside(px, py, w->x, w->y, w->w, w->h))
            return zord[i];
    }
    return -1;
}

static void win_damage_all(const struct win *w)
{
    desk_dirty(w->x, w->y, w->w, w->h);
}

static void set_focus(int i);

static void win_draw(struct win *w, int foc)
{
    int x = w->x, y = w->y, ww = w->w, hh = w->h, ox = x + BD, oy = y + BD + TH;
    int j, i, n, x0, x1, y0, y1;
    if (x >= cl_x1 || x + ww <= cl_x0 || y >= cl_y1 || y + hh <= cl_y0)
        return;
    d_rect(x, y, ww, hh, C_BLACK);
    d_rect(x + 1, y + 1, ww - 2, hh - 2, C_LGREY);
    d_rect(x + BD, y + BD, ww - 2 * BD, TH, foc ? C_LBLUE : C_GREY);
    {   /* titulo recortado para que no pise los botones */
        int maxc = (ww - 2 * BD - 12 - (w->resizable ? 40 : 22)) / 8, k;
        char t[32];
        for (k = 0; k < maxc && k < 31 && w->title[k]; k++)
            t[k] = w->title[k];
        t[k] = 0;
        d_text(x + BD + 6, y + BD + 2, t, foc ? C_WHITE : C_LGREY, -1);
    }
    d_bevel(x + ww - BD - 18, y + BD + 2, 16, 16, C_LRED, 0);
    d_text(x + ww - BD - 18 + 4, y + BD + 2, "X", C_WHITE, -1);
    if (w->resizable) {
        d_bevel(x + ww - BD - 36, y + BD + 2, 16, 16, C_LGREY, 0);
        d_rect(x + ww - BD - 36 + 4, y + BD + 2 + 4, 8, 8, C_BLACK);
        d_rect(x + ww - BD - 36 + 5, y + BD + 2 + 6, 6, 5, C_LGREY);
    }
    /* zona interior */
    x0 = ox > cl_x0 ? ox : cl_x0;
    x1 = ox + w->ctx.w < cl_x1 ? ox + w->ctx.w : cl_x1;
    y0 = oy > cl_y0 ? oy : cl_y0;
    y1 = oy + w->ctx.h < cl_y1 ? oy + w->ctx.h : cl_y1;
    if (x1 > x0)
        for (j = y0; j < y1; j++) {
            u8 *d = desk + j * SW + x0;
            const u8 *s = w->surf + (j - oy) * SW + (x0 - ox);
            n = x1 - x0;
            if (w->gfx) {
                const u8 *g = w->gfx + (j - oy) * SW + (x0 - ox);
                for (i = 0; i < n; i++)
                    d[i] = g[i] < 16 ? g[i] : s[i];
            } else {
                for (i = 0; i < n; i++)
                    d[i] = s[i];
            }
        }
    if (w->resizable) {                  /* esquina para agrandar */
        int cx = x + ww - 4, cy = y + hh - 4;
        for (i = 0; i < 3; i++)
            for (j = 0; j <= i * 3 + 2; j++) {
                d_rect(cx - 3 - i * 3 + j, cy - j, 1, 1, C_DGREY);
            }
    }
}

static void compose(int rx, int ry, int rw, int rh)
{
    int i;
    cl_x0 = rx < 0 ? 0 : rx;
    cl_y0 = ry < 0 ? 0 : ry;
    cl_x1 = rx + rw > SW ? SW : rx + rw;
    cl_y1 = ry + rh > SH ? SH : ry + rh;
    if (cl_x1 <= cl_x0 || cl_y1 <= cl_y0)
        return;
    bg_draw();
    icons_draw();
    for (i = 0; i < nz; i++)
        win_draw(&wins[zord[i]], zord[i] == focus_w);
    taskbar_draw();
    K->dirty(cl_x0, cl_y0, cl_x1 - cl_x0, cl_y1 - cl_y0);
}

/* ---- servicios para las apps ---- */

static void c_damage(struct wctx *c, int x, int y, int w, int h)
{
    struct win *wn = (struct win *)c->wm;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w > 0 && h > 0)
        desk_dirty(wn->x + BD + x, wn->y + BD + TH + y, w, h);
}

static void c_close(struct wctx *c)
{
    ((struct win *)c->wm)->closing = 1;
}

static void c_title(struct wctx *c, const char *t)
{
    struct win *w = (struct win *)c->wm;
    scopy(w->title, t, (int)sizeof w->title);
    desk_dirty(w->x, w->y, w->w, BD + TH);
}

static void launch(const char *name);
static void c_launch(const char *app) { launch(app); }
static void c_run_basic(const char *line)
{
    if (K->basic_busy() || pend_set)
        return;
    scopy(pend, line, (int)sizeof pend);
    pend_set = 1;
}

static void win_dirty_cb(void *user, int x, int y, int w, int h)
{
    struct win *wn = (struct win *)user;
    desk_dirty(wn->x + BD + x, wn->y + BD + TH + y, w, h);
}

static void zorder_remove(int i)
{
    int k, j = -1;
    for (k = 0; k < nz; k++)
        if (zord[k] == i)
            j = k;
    if (j < 0)
        return;
    for (k = j; k < nz - 1; k++)
        zord[k] = zord[k + 1];
    nz--;
}

static void set_focus(int i)
{
    if (i == focus_w)
        return;
    if (focus_w >= 0) {
        wins[focus_w].ctx.focus = 0;
        win_damage_all(&wins[focus_w]);
    }
    focus_w = i;
    if (i >= 0) {
        struct win *w = &wins[i];
        w->ctx.focus = 1;
        zorder_remove(i);
        zord[nz++] = i;
        win_damage_all(w);
    }
    desk_dirty(0, SH - TBAR, SW, TBAR);
}

static void term_fill(struct win *w)     /* fondo del terminal: todo el interior azul */
{
    int j;
    for (j = 0; j < w->ctx.h; j++)
        memset(w->surf + j * SW, C_BLUE, (size_t)w->ctx.w);
    if (w->gfx)
        for (j = 0; j < w->ctx.h; j++)
            memset(w->gfx + j * SW, 255, (size_t)w->ctx.w);
}

static void win_apply_size(struct win *w)
{
    int cw = w->w - 2 * BD, ch = w->h - 2 * BD - TH;
    w->ctx.w = cw;
    w->ctx.h = ch;
    w->ctx.sx = w->x + BD;
    w->ctx.sy = w->y + BD + TH;
    if (w->term) {
        term_fill(w);
        K->con_resize(w->term, cw / 8, ch / 16);
    } else if (w->app && w->app->resize) {
        w->app->resize(&w->ctx);
    }
}

static void win_close(int i)
{
    struct win *w = &wins[i];
    if (!w->used)
        return;
    if (w->busy)
        return;                          /* corriendo BASIC: se cierra cuando termine */
    desk_dirty(w->x, w->y, w->w, w->h);
    if (w->term) {
        K->con_detach(w->term);
        K->con_select(0);
    } else if (w->app && w->app->stop) {
        w->app->stop(&w->ctx);
    }
    slot_free(w->surf);
    slot_free(w->gfx);
    w->used = 0;
    zorder_remove(i);
    if (focus_w == i) {
        focus_w = -1;
        if (nz > 0)
            set_focus(zord[nz - 1]);
    }
    desk_dirty(0, SH - TBAR, SW, TBAR);
}

static int win_new(int term, const struct wapp *app, const char *appname)
{
    int i, cw, ch;
    struct win *w;
    for (i = 0; i < MAXW && wins[i].used; i++)
        ;
    if (i >= MAXW)
        return -1;
    w = &wins[i];
    memset(w, 0, sizeof *w);
    w->surf = slot_alloc();
    if (!w->surf)
        return -1;
    if (term) {
        w->gfx = slot_alloc();
        if (!w->gfx) {
            slot_free(w->surf);
            return -1;
        }
        cw = 80 * 8;
        ch = 24 * 16;
        w->minw = 160; w->minh = 64;
        w->resizable = 1;
    } else {
        cw = app->w; ch = app->h;
        w->minw = app->minw; w->minh = app->minh;
        w->resizable = app->resizable;
    }
    if (cw > SW - 2 * BD - 8) cw = SW - 2 * BD - 8;
    if (ch > SH - TBAR - 2 * BD - TH - 8) ch = SH - TBAR - 2 * BD - TH - 8;
    w->used = 1;
    w->w = cw + 2 * BD;
    w->h = ch + 2 * BD + TH;
    w->x = 120 + (nz % 6) * 30;
    w->y = 30 + (nz % 6) * 26;
    if (w->x + w->w > SW) w->x = SW - w->w;
    if (w->y + w->h > SH - TBAR) w->y = SH - TBAR - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    w->app = app;
    scopy(w->appname, appname, (int)sizeof w->appname);
    w->ctx.pix = w->surf;
    w->ctx.stride = SW;
    w->ctx.w = cw;
    w->ctx.h = ch;
    w->ctx.sx = w->x + BD;
    w->ctx.sy = w->y + BD + TH;
    w->ctx.wm = w;
    w->ctx.damage = c_damage;
    w->ctx.close = c_close;
    w->ctx.title = c_title;
    w->ctx.launch = c_launch;
    w->ctx.run_basic = c_run_basic;
    zord[nz++] = i;
    if (term) {
        int id = 1, k;
        for (k = 0; k < MAXW; k++)
            if (wins[k].used && wins[k].term == 1)
                id = 2;
        w->term = id;
        scopy(w->title, id == 1 ? "TERMINAL 1" : "TERMINAL 2", (int)sizeof w->title);
        term_fill(w);
        if (K->con_attach(id, w->surf, w->gfx, SW, cw / 8, ch / 16, win_dirty_cb, w) < 0) {
            win_close(i);
            return -1;
        }
        K->con_putc(id, 147);
        {
            const char *b = "PC BASIC V1.2  -  TERMINAL EN VENTANA\n\nREADY.\n";
            while (*b)
                K->con_putc(id, *b++);
        }
    } else {
        scopy(w->title, app->title, (int)sizeof w->title);
        {
            int j;
            for (j = 0; j < ch; j++)
                memset(w->surf + j * SW, C_LGREY, (size_t)cw);
        }
        if (app->start)
            app->start(&w->ctx);
    }
    set_focus(i);
    win_damage_all(w);
    return i;
}

static int find_app(const char *name)
{
    int i, k;
    for (i = 0; i < MAXW; i++) {
        if (!wins[i].used || wins[i].term)
            continue;
        for (k = 0; wins[i].appname[k] == name[k]; k++)
            if (!name[k])
                return i;
    }
    return -1;
}

static void wm_hook(void);

static void launch(const char *name)
{
    int r, i;
    const struct wapp *a;
    if (name[0] == 'T' && name[1] == 'E' && name[2] == 'R') {      /* TERMINAL */
        for (i = 0; i < MAXW; i++)
            if (wins[i].used && wins[i].term && !wins[i].busy) {
                if (i != focus_w) {
                    set_focus(i);
                    return;
                }
            }
        win_new(1, 0, "TERMINAL");
        return;
    }
    i = find_app(name);
    if (i >= 0) {
        set_focus(i);
        return;
    }
    r = K->exec(name);
    if (r != MOD_WINAPP) {
        /* modulo de pantalla completa (DOOM...): pinto sobre el framebuffer y apago el gancho; al volver
         * se reactiva y se repinta todo el escritorio */
        K->set_tick(wm_hook);
        desk_dirty(0, 0, SW, SH);
        K->esc();
        return;
    }
    a = K->wm_take();
    if (a)
        win_new(0, a, name);
}

/* ---- terminal en ventana ---- */

static struct win *term_win(void)
{
    int i;
    if (focus_w >= 0 && wins[focus_w].used && wins[focus_w].term)
        return &wins[focus_w];
    for (i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].term)
            return &wins[i];
    i = win_new(1, 0, "TERMINAL");
    return i >= 0 ? &wins[i] : 0;
}

static void term_hide_cursor(struct win *w)
{
    if (w->cur_on) {
        K->con_cursor(w->term, 0);
        w->cur_on = 0;
    }
}

static void term_run(struct win *w, const char *line)
{
    char buf[128];
    scopy(buf, line, (int)sizeof buf);
    term_hide_cursor(w);
    w->busy = 1;
    K->con_select(w->term);
    K->basic_run(buf);
    K->con_select(0);
    w->busy = 0;
    if (w->closing)
        win_close((int)(w - wins));
}

static void term_key(struct win *w, int k)
{
    int id = w->term;
    if (k == '\n' || k == '\r') {
        term_hide_cursor(w);
        K->con_putc(id, '\n');
        w->line[w->ln] = 0;
        if (w->ln)
            scopy(w->last, w->line, (int)sizeof w->last);
        w->ln = 0;
        {
            char buf[128];
            scopy(buf, w->line, (int)sizeof buf);
            term_run(w, buf);
        }
        return;
    }
    term_hide_cursor(w);
    if (k == '\b') {
        if (w->ln > 0) {
            w->ln--;
            K->con_putc(id, '\b');
        }
    } else if (k == KEY_UP) {            /* recuperar la ultima linea */
        int i;
        while (w->ln > 0) {
            w->ln--;
            K->con_putc(id, '\b');
        }
        for (i = 0; w->last[i] && w->ln < 127; i++) {
            w->line[w->ln++] = w->last[i];
            K->con_putc(id, w->last[i]);
        }
    } else if (((k >= 32 && k < 127) || k >= 160) && w->ln < 127) {
        w->line[w->ln++] = (char)k;
        K->con_putc(id, k);
    }
}

/* ---- raton ---- */

static void taskbar_click(int px)
{
    int i, x = 6;
    if (px >= tb_exit_x()) {
        quit = 1;
        return;
    }
    if (px >= tb_bg_x() && px < tb_bg_x() + 160) {
        bgmode = (bgmode + 1) % NBG;
        desk_dirty(0, 0, SW, SH);
        return;
    }
    for (i = 0; i < nz; i++) {
        if (x + TB_BTN_W > tb_bg_x() - 4)
            break;
        if (px >= x && px < x + TB_BTN_W) {
            set_focus(zord[i]);
            return;
        }
        x += TB_BTN_W + 4;
    }
}

static int hit_part(const struct win *w, int px, int py)
{
    int rx = px - w->x, ry = py - w->y;
    if (rx < 0 || ry < 0 || rx >= w->w || ry >= w->h)
        return P_NONE;
    if (inside(rx, ry, w->w - BD - 18, BD + 2, 16, 16))
        return P_CLOSE;
    if (w->resizable && inside(rx, ry, w->w - BD - 36, BD + 2, 16, 16))
        return P_MAX;
    if (w->resizable && !w->maxed) {
        if (rx >= w->w - 10 && ry >= w->h - 10) return P_RC;
        if (rx >= w->w - 4 && ry >= BD + TH) return P_RR;
        if (ry >= w->h - 4) return P_RB;
    }
    if (ry < BD + TH)
        return P_TITLE;
    return P_CLIENT;
}

static void toggle_max(int i)
{
    struct win *w = &wins[i];
    desk_dirty(w->x, w->y, w->w, w->h);
    if (!w->maxed) {
        w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
        w->x = 0; w->y = 0; w->w = SW; w->h = SH - TBAR;
        w->maxed = 1;
    } else {
        w->x = w->rx; w->y = w->ry; w->w = w->rw; w->h = w->rh;
        w->maxed = 0;
    }
    win_apply_size(w);
    win_damage_all(w);
}

static void client_event(int i, int px, int py, int b, int pb)
{
    struct win *w = &wins[i];
    if (w->app && w->app->mouse)
        w->app->mouse(&w->ctx, px - (w->x + BD), py - (w->y + BD + TH), b, pb);
}

static void wm_mouse(void)
{
    int nx, ny, nb, pressed, released, moved, prevb;
    K->mouse(&nx, &ny, &nb);
    pressed = (nb & 1) && !(mb & 1);
    released = !(nb & 1) && (mb & 1);
    moved = nx != mx || ny != my;
    prevb = mb;
    mx = nx; my = ny; mb = nb;

    if (pressed) {
        int i = win_at(mx, my);
        if (i >= 0) {
            int part = hit_part(&wins[i], mx, my);
            set_focus(i);
            if (part == P_CLOSE) {
                if (wins[i].busy)
                    wins[i].closing = 1;
                else
                    win_close(i);
                mode = M_NONE;
                return;
            } else if (part == P_MAX) {
                toggle_max(i);
                mode = M_NONE;
                return;
            } else if (part == P_TITLE) {
                mode = M_MOVE; mwin = i;
                gx = mx - wins[i].x; gy = my - wins[i].y;
            } else if (part >= P_RR) {
                mode = M_RESIZE; mwin = i; rs_part = part;
                gx = mx; gy = my; gw0 = wins[i].w; gh0 = wins[i].h;
            } else {
                mode = M_CAPTURE; mwin = i;
            }
        } else if (my >= SH - TBAR) {
            taskbar_click(mx);
        } else {
            int k, hit = -1, x, y;
            for (k = 0; k < NITEMS; k++) {
                icon_pos(k, &x, &y);
                if (inside(mx, my, x, y, ICON_W, ICON_H))
                    hit = k;
            }
            if (hit >= 0)
                launch(items[hit].app ? items[hit].app : "TERMINAL");
            else
                set_focus(-1);
        }
    }

    if (mode == M_MOVE && (nb & 1) && moved) {
        struct win *w = &wins[mwin];
        int nx2 = mx - gx, ny2 = my - gy;
        if (w->maxed) {
            w->maxed = 0;                /* arrastrar una ventana maximizada la restaura */
            w->w = w->rw; w->h = w->rh;
            gx = w->w / 2;
            nx2 = mx - gx;
            ny2 = my - gy;
            win_apply_size(w);
            desk_dirty(0, 0, SW, SH);
        }
        if (nx2 < -(w->w - 60)) nx2 = -(w->w - 60);
        if (nx2 > SW - 60) nx2 = SW - 60;
        if (ny2 < 0) ny2 = 0;
        if (ny2 > SH - TBAR - TH) ny2 = SH - TBAR - TH;
        win_damage_all(w);
        w->x = nx2; w->y = ny2;
        w->ctx.sx = w->x + BD;
        w->ctx.sy = w->y + BD + TH;
        win_damage_all(w);
    } else if (mode == M_RESIZE && (nb & 1) && moved) {
        struct win *w = &wins[mwin];
        int nw = gw0, nh = gh0;
        if (rs_part == P_RR || rs_part == P_RC) nw = gw0 + (mx - gx);
        if (rs_part == P_RB || rs_part == P_RC) nh = gh0 + (my - gy);
        if (nw < w->minw + 2 * BD) nw = w->minw + 2 * BD;
        if (nh < w->minh + 2 * BD + TH) nh = w->minh + 2 * BD + TH;
        if (w->x + nw > SW) nw = SW - w->x;
        if (w->y + nh > SH - TBAR) nh = SH - TBAR - w->y;
        if (nw != w->w || nh != w->h) {
            win_damage_all(w);
            w->w = nw; w->h = nh;
            win_apply_size(w);
            win_damage_all(w);
        }
    }

    /* eventos de raton para las apps: la ventana capturada o la que esta bajo el puntero */
    {
        int target = -1;
        if (mode == M_CAPTURE)
            target = mwin;
        else if (mode == M_NONE) {
            int i = win_at(mx, my);
            if (i >= 0 && hit_part(&wins[i], mx, my) == P_CLIENT)
                target = i;
        }
        if (target >= 0 && wins[target].used && (moved || nb != prevb))
            client_event(target, mx, my, nb, prevb);
    }
    if (released) {
        mode = M_NONE;
    }

    {   /* resaltar el icono bajo el puntero */
        int k, hit = -1, x, y;
        if (win_at(mx, my) < 0 && my < SH - TBAR)
            for (k = 0; k < NITEMS; k++) {
                icon_pos(k, &x, &y);
                if (inside(mx, my, x, y, ICON_W, ICON_H))
                    hit = k;
            }
        if (hit != hover_icon) {
            int old = hover_icon;
            hover_icon = hit;
            if (old >= 0) { icon_pos(old, &x, &y); desk_dirty(x, y, ICON_W, ICON_H); }
            if (hit >= 0) { icon_pos(hit, &x, &y); desk_dirty(x, y, ICON_W, ICON_H); }
        }
    }
    pmb = nb;
}

/* ---- servicio periodico (60 Hz desde el kernel y desde el bucle principal) ---- */

static void service(void)
{
    unsigned now;
    int i;
    if (in_service)
        return;
    now = K->ms();
    if (now - last_service < 14u)
        return;
    last_service = now;
    in_service = 1;

    wm_mouse();

    for (i = 0; i < MAXW; i++) {
        struct win *w = &wins[i];
        if (!w->used)
            continue;
        if (w->app && w->app->frame && !w->term)
            w->app->frame(&w->ctx);
        if (w->term && !w->busy) {       /* cursor parpadeante en la terminal inactiva */
            int want = (i == focus_w) && ((now / 500) & 1);
            if (want != w->cur_on) {
                K->con_cursor(w->term, want);
                w->cur_on = want;
            }
        }
    }
    for (i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].closing && !wins[i].busy)
            win_close(i);

    if (bgmode >= 2 && now - bg_last >= 38u) {       /* fondo animado: ~25 cuadros por segundo */
        bg_last = now;
        bg_t += 3;
        bg_step();
        desk_dirty(0, 0, SW, SH);
    }
    if ((int)(now / 1000) != last_sec) {          /* reloj de la barra */
        last_sec = (int)(now / 1000);
        desk_dirty(tb_clock_x(), SH - TBAR, 70, TBAR);
    }
    if (dm_x1 > dm_x0) {
        int x = dm_x0, y = dm_y0, w = dm_x1 - dm_x0, h = dm_y1 - dm_y0;
        dm_x0 = dm_y0 = dm_x1 = dm_y1 = 0;
        compose(x, y, w, h);
    }
    K->flush();
    in_service = 0;
}

static void wm_hook(void)
{
    service();
}

/* ---- teclado y bucle principal ---- */

static void dispatch_key(int k)
{
    if (focus_w >= 0 && wins[focus_w].used) {
        struct win *w = &wins[focus_w];
        if (w->term)
            term_key(w, k);
        else if (w->app && w->app->key)
            w->app->key(&w->ctx, k);
        return;
    }
    {
        int i;
        char u = (char)((k >= 'a' && k <= 'z') ? k - 32 : k);
        if (u == 'B') {
            bgmode = (bgmode + 1) % NBG;
            desk_dirty(0, 0, SW, SH);
            return;
        }
        for (i = 0; i < NITEMS; i++)
            if (u == items[i].key)
                launch(items[i].app ? items[i].app : "TERMINAL");
    }
}

int mod_main(const struct kapi *api)
{
    unsigned bytes = 0;
    int i, k;

    K = api;
    SW = K->scr_w();
    SH = K->scr_h();
    if (SW < 400 || SH < 300)
        return MOD_OK;
    desk = K->layer(4);
    pool = (u8 *)K->pool(&bytes);
    if (!desk || !pool)
        return MOD_OK;
    slot_sz = (unsigned)(SW * SH);
    nslots = (int)(bytes / slot_sz);
    if (nslots > (int)sizeof slot_used)
        nslots = (int)sizeof slot_used;
    memset(desk, 255, slot_sz);
    for (i = 0; i < NITEMS; i++)
        icon_load(&icons[i], items[i].icon);
    bg_init();
    K->mouse(&mx, &my, &mb);
    pmb = mb;
    K->esc();
    desk_dirty(0, 0, SW, SH);
    last_service = K->ms() - 100;
    service();
    K->set_tick(wm_hook);

    while (!quit) {
        k = K->poll();
        service();
        if (K->esc())
            break;
        if (k)
            dispatch_key(k);
        if (pend_set) {
            char line[128];
            struct win *w = term_win();
            scopy(line, pend, (int)sizeof line);
            pend_set = 0;
            if (w && !K->basic_busy()) {
                set_focus((int)(w - wins));
                term_hide_cursor(w);
                for (k = 0; line[k]; k++)
                    K->con_putc(w->term, line[k]);
                K->con_putc(w->term, '\n');
                term_run(w, line);
            }
        }
        K->sleep(1);
    }
    K->set_tick(0);
    for (i = 0; i < MAXW; i++) {
        wins[i].busy = 0;
        win_close(i);
    }
    return MOD_OK;
}
