/* wtk.h - WTK, el kit de ventanas de BASIC-OS: botones, etiquetas, cajas de texto, casillas, listas, barras de progreso,
 * marcos, lienzos para dibujar y una barra de menus. El programa solo DECLARA los controles y recibe avisos; WTK los dibuja,
 * lleva el foco (Tab), el teclado, el raton y el cambio de tamano de la ventana.
 *
 * Sirve igual para las apps de /SYS (.APP) y para los programas de la tienda (.BIN): es una cabecera, se compila dentro.
 *
 *   #include "sys/wtk.h"
 *   static char nombre[32];
 *   static void saludar(int id) { wt_msg("Hola!"); }
 *   static void armar(void) {                       // se llama al abrir la ventana
 *       wt_label(8, 8, "Tu nombre:");
 *       wt_anchor(wt_input(96, 6, 160, nombre, sizeof nombre, saludar), WT_FILLW);
 *       wt_button(8, 40, 100, 24, "Saludar", saludar);
 *   }
 *   int mod_main(const struct kapi *k) { K = k; return wt_window("Hola", 320, 120, 1, armar); }
 *
 * Coordenadas: en pixeles, dentro de la ventana y por debajo de la barra de menus (si hay). Anclas (wt_anchor):
 *   WT_FILLW / WT_FILLH  el ancho/alto llega hasta el borde derecho/inferior dejando el mismo margen que w/h dicen
 *                        (w = margen derecho, h = margen inferior);  WT_RIGHT / WT_BOTTOM  x/y se miden desde la derecha/abajo.
 * Los avisos (callbacks) reciben el id del control. wt_ev dice que paso: WT_EV_CHANGE (casilla marcada o seleccion de una
 * lista cambiada) o WT_EV_ACTIVATE (boton pulsado, Enter en una caja o en una lista, doble clic en una lista). Una caja de
 * texto solo avisa con Enter: lo escrito ya esta en su buffer en todo momento.
 * Todo cambio (wt_text, valores...) se pinta solo en el siguiente cuadro.
 * La consola entrega MAYUSCULAS sin Shift: en las cajas de texto se invierte (sin Shift = minusculas), como en NET y COSMOS.
 */
#ifndef WTK_H
#define WTK_H
#include "sys/ui.h"

#define WT_FN static __attribute__((unused))
#define WT_MAX    64
#define WT_MENUS  8
#define WT_MENU_H 20

enum { WT_LABEL = 1, WT_BUTTON, WT_CHECK, WT_INPUT, WT_LIST, WT_PROGRESS, WT_BOX, WT_CANVAS };
#define WT_FILLW    1
#define WT_FILLH    2
#define WT_RIGHT    4
#define WT_BOTTOM   8
#define WT_HIDDEN   16
#define WT_DISABLED 32

#define WT_EV_CHANGE   1
#define WT_EV_ACTIVATE 2

struct wt_widget {
    int type, flags;
    int x, y, w, h;                        /* como los pidio el programa */
    int rx, ry, rw, rh;                    /* donde quedo de verdad (anclas, barra de menus) */
    char text[64];
    int fg, bg;
    char *buf; int max;                    /* WT_INPUT */
    int *val; int vmax;                    /* WT_CHECK (0/1), WT_PROGRESS (0..vmax), WT_LIST (seleccion, -1 = ninguna) */
    const char *const *items; int nitems, top;   /* WT_LIST */
    void (*cb)(int id);
    void (*draw)(struct surf *s, int w, int h);              /* WT_CANVAS: dibuja en su rectangulo (0,0 = esquina) */
    void (*click)(int id, int x, int y, int btn);            /* WT_CANVAS: clic y arrastre, coordenadas del lienzo */
};

struct wt_menu { char title[16]; const char *const *items; int n; void (*cb)(int item); int x, w; };

static struct {
    struct wt_widget w[WT_MAX];
    int n;
    struct wt_menu m[WT_MENUS];
    int nm, open, hover;                   /* menu abierto (-1 = ninguno) y renglon bajo el raton */
    struct wctx *c;
    int focus, pressed, bg, bgset, dirty;
    unsigned blink, last_click;
    int last_row, last_list;
    char msg[160];                         /* cuadro de aviso (wt_msg) */
    char title[32];
    int iw, ih, resizable;
    void (*build)(void);
    void (*tick)(void);
    int (*keyhook)(int k);
    void (*on_close)(void);
    void (*on_resize)(int w, int h);
    struct wapp app;
} WT;
WT_FN int wt_ev;

/* ---------- crear controles ---------- */
WT_FN int wt_add(int type, int x, int y, int w, int h, const char *t)
{
    struct wt_widget *g;
    if (WT.n >= WT_MAX)
        return -1;
    g = &WT.w[WT.n];
    memset(g, 0, sizeof *g);
    g->type = type; g->x = x; g->y = y; g->w = w; g->h = h;
    g->fg = C_BLACK; g->bg = -1;
    if (t) scopy(g->text, t, (int)sizeof g->text);
    WT.dirty = 1;
    return WT.n++;
}
WT_FN int wt_label(int x, int y, const char *t) { return wt_add(WT_LABEL, x, y, slen(t) * 8, 16, t); }
WT_FN int wt_button(int x, int y, int w, int h, const char *t, void (*cb)(int))
{
    int id = wt_add(WT_BUTTON, x, y, w, h, t);
    if (id >= 0) WT.w[id].cb = cb;
    return id;
}
WT_FN int wt_check(int x, int y, const char *t, int *v, void (*cb)(int))
{
    int id = wt_add(WT_CHECK, x, y, 22 + slen(t) * 8, 16, t);
    if (id >= 0) { WT.w[id].val = v; WT.w[id].cb = cb; }
    return id;
}
WT_FN int wt_input(int x, int y, int w, char *buf, int max, void (*cb)(int))
{
    int id = wt_add(WT_INPUT, x, y, w, 20, 0);
    if (id >= 0) { WT.w[id].buf = buf; WT.w[id].max = max; WT.w[id].cb = cb; }
    return id;
}
WT_FN int wt_list(int x, int y, int w, int h, const char *const *items, int n, int *sel, void (*cb)(int))
{
    int id = wt_add(WT_LIST, x, y, w, h, 0);
    if (id >= 0) { WT.w[id].items = items; WT.w[id].nitems = n; WT.w[id].val = sel; WT.w[id].cb = cb; }
    return id;
}
WT_FN int wt_progress(int x, int y, int w, int h, int *v, int max)
{
    int id = wt_add(WT_PROGRESS, x, y, w, h, 0);
    if (id >= 0) { WT.w[id].val = v; WT.w[id].vmax = max > 0 ? max : 1; }
    return id;
}
WT_FN int wt_box(int x, int y, int w, int h, const char *title) { return wt_add(WT_BOX, x, y, w, h, title); }
WT_FN int wt_canvas(int x, int y, int w, int h, void (*draw)(struct surf *, int, int), void (*click)(int, int, int, int))
{
    int id = wt_add(WT_CANVAS, x, y, w, h, 0);
    if (id >= 0) { WT.w[id].draw = draw; WT.w[id].click = click; }
    return id;
}
WT_FN void wt_menu(const char *title, const char *const *items, int n, void (*cb)(int item))
{
    struct wt_menu *m;
    if (WT.nm >= WT_MENUS)
        return;
    m = &WT.m[WT.nm++];
    scopy(m->title, title, (int)sizeof m->title);
    m->items = items; m->n = n; m->cb = cb;
    WT.dirty = 1;
}

/* ---------- cambiar controles ---------- */
WT_FN int wt_ok(int id) { return id >= 0 && id < WT.n; }
WT_FN void wt_anchor(int id, int flags) { if (wt_ok(id)) { WT.w[id].flags |= flags & 15; WT.dirty = 1; } }
WT_FN void wt_text(int id, const char *t) { if (wt_ok(id)) { scopy(WT.w[id].text, t, (int)sizeof WT.w[id].text); WT.dirty = 1; } }
WT_FN void wt_color(int id, int fg, int bg) { if (wt_ok(id)) { WT.w[id].fg = fg; WT.w[id].bg = bg; WT.dirty = 1; } }
WT_FN void wt_show(int id, int on) { if (wt_ok(id)) { if (on) WT.w[id].flags &= ~WT_HIDDEN; else WT.w[id].flags |= WT_HIDDEN; WT.dirty = 1; } }
WT_FN void wt_enable(int id, int on) { if (wt_ok(id)) { if (on) WT.w[id].flags &= ~WT_DISABLED; else WT.w[id].flags |= WT_DISABLED; WT.dirty = 1; } }
WT_FN void wt_items(int id, const char *const *items, int n) { if (wt_ok(id)) { WT.w[id].items = items; WT.w[id].nitems = n; WT.w[id].top = 0; WT.dirty = 1; } }
WT_FN void wt_focus(int id) { WT.focus = id; WT.dirty = 1; }
WT_FN void wt_redraw(void) { WT.dirty = 1; }
WT_FN void wt_background(int color) { WT.bg = color; WT.bgset = 1; WT.dirty = 1; }
WT_FN void wt_msg(const char *t) { scopy(WT.msg, t, (int)sizeof WT.msg); WT.dirty = 1; }
WT_FN void wt_title(const char *t) { scopy(WT.title, t, (int)sizeof WT.title); if (WT.c) WT.c->title(WT.c, WT.title); }
WT_FN void wt_close(void) { if (WT.c) WT.c->close(WT.c); }
WT_FN void wt_on_tick(void (*fn)(void)) { WT.tick = fn; }          /* ~60 veces por segundo */
WT_FN void wt_on_key(int (*fn)(int k)) { WT.keyhook = fn; }       /* devuelve 1 si se comio la tecla */
WT_FN void wt_on_close(void (*fn)(void)) { WT.on_close = fn; }
WT_FN void wt_on_resize(void (*fn)(int w, int h)) { WT.on_resize = fn; }
WT_FN int wt_width(void) { return WT.c ? WT.c->w : WT.iw; }
WT_FN int wt_height(void) { return WT.c ? WT.c->h - (WT.nm ? WT_MENU_H : 0) : WT.ih; }

/* ---------- geometria ---------- */
WT_FN int wt_top(void) { return WT.nm ? WT_MENU_H : 0; }

WT_FN void wt_layout(void)
{
    int i, W = WT.c->w, H = WT.c->h, top = wt_top();
    for (i = 0; i < WT.n; i++) {
        struct wt_widget *g = &WT.w[i];
        g->rx = (g->flags & WT_RIGHT) ? W - g->x - g->w : g->x;
        g->ry = top + ((g->flags & WT_BOTTOM) ? (H - top) - g->y - g->h : g->y);
        g->rw = (g->flags & WT_FILLW) ? W - g->rx - g->w : g->w;
        g->rh = (g->flags & WT_FILLH) ? H - g->ry - g->h : g->h;
        if (g->rw < 8) g->rw = 8;
        if (g->rh < 8) g->rh = 8;
    }
    if (WT.nm) {
        int x = 4;
        for (i = 0; i < WT.nm; i++) {
            WT.m[i].x = x;
            WT.m[i].w = slen(WT.m[i].title) * 8 + 16;
            x += WT.m[i].w;
        }
    }
}

WT_FN int wt_focusable(int i)
{
    const struct wt_widget *g = &WT.w[i];
    return (g->type == WT_BUTTON || g->type == WT_CHECK || g->type == WT_INPUT || g->type == WT_LIST) &&
           !(g->flags & (WT_HIDDEN | WT_DISABLED));
}

/* ---------- dibujo ---------- */
WT_FN void wt_text_clip(const struct surf *S, int x, int y, const char *t, int maxch, int fg, int bg)
{
    char b[96];
    int i;
    for (i = 0; t[i] && i < maxch && i < 95; i++) b[i] = t[i];
    b[i] = 0;
    s_text(S, x, y, b, fg, bg);
}

WT_FN void wt_sunken(const struct surf *S, int x, int y, int w, int h, int fill)
{
    s_rect(S, x, y, w, h, C_DGREY);
    s_rect(S, x + 1, y + 1, w - 1, h - 1, C_WHITE);
    s_rect(S, x + 1, y + 1, w - 2, h - 2, fill);
}

WT_FN int wt_list_rows(const struct wt_widget *g) { return (g->rh - 4) / 16 > 0 ? (g->rh - 4) / 16 : 1; }

WT_FN void wt_draw_widget(const struct surf *S, int i)
{
    struct wt_widget *g = &WT.w[i];
    int x = g->rx, y = g->ry, w = g->rw, h = g->rh, foc = WT.focus == i && WT.c && WT.c->focus;
    int dis = (g->flags & WT_DISABLED) != 0;
    switch (g->type) {
    case WT_LABEL:
        s_text(S, x, y, g->text, dis ? C_GREY : g->fg, g->bg);
        break;
    case WT_BUTTON:
        s_button(S, x, y, w, h, g->text, g->bg >= 0 ? g->bg : C_LGREY, dis ? C_GREY : g->fg, WT.pressed == i);
        if (foc) s_frame(S, x + 3, y + 3, w - 6, h - 6, C_BLUE);
        break;
    case WT_CHECK:
        wt_sunken(S, x, y + 1, 14, 14, dis ? C_LGREY : C_WHITE);
        if (g->val && *g->val)
            s_glyph(S, x + 3, y, 251, C_BLACK, -1);           /* palomita (CP437) */
        s_text(S, x + 20, y, g->text, dis ? C_GREY : g->fg, g->bg);
        if (foc) s_frame(S, x + 18, y - 1, slen(g->text) * 8 + 4, 18, C_BLUE);
        break;
    case WT_INPUT: {
        int vis = (w - 8) / 8, len = g->buf ? slen(g->buf) : 0, from = len > vis - 1 ? len - (vis - 1) : 0;
        wt_sunken(S, x, y, w, h, dis ? C_LGREY : C_WHITE);
        if (foc) s_frame(S, x, y, w, h, C_BLUE);
        if (g->buf)
            wt_text_clip(S, x + 4, y + (h - 16) / 2, g->buf + from, vis, C_BLACK, -1);
        if (foc && (WT.blink / 500) % 2 == 0)
            s_rect(S, x + 4 + (len - from) * 8, y + (h - 16) / 2, 2, 16, C_BLACK);
        break; }
    case WT_LIST: {
        int rows = wt_list_rows(g), r, sel = g->val ? *g->val : -1, sbar = g->nitems > rows;
        int tw = w - (sbar ? 12 : 0);
        wt_sunken(S, x, y, w, h, C_WHITE);
        if (foc) s_frame(S, x, y, w, h, C_BLUE);
        if (g->top > g->nitems - rows) g->top = g->nitems - rows;
        if (g->top < 0) g->top = 0;
        for (r = 0; r < rows && g->top + r < g->nitems; r++) {
            int it = g->top + r, yy = y + 2 + r * 16, on = it == sel;
            if (on) s_rect(S, x + 2, yy, tw - 4, 16, foc ? C_BLUE : C_GREY);
            wt_text_clip(S, x + 4, yy, g->items[it] ? g->items[it] : "", (tw - 8) / 8, on ? C_WHITE : C_BLACK, -1);
        }
        if (sbar) {
            int bx = x + w - 12, th = (h - 4) * rows / g->nitems, ty;
            if (th < 10) th = 10;
            ty = y + 2 + (h - 4 - th) * g->top / (g->nitems - rows);
            s_rect(S, bx, y + 2, 10, h - 4, C_LGREY);
            s_button(S, bx, ty, 10, th, "", C_GREY, C_BLACK, 0);
        }
        break; }
    case WT_PROGRESS: {
        int v = g->val ? *g->val : 0, fw;
        char t[8];
        if (v < 0) v = 0;
        if (v > g->vmax) v = g->vmax;
        fw = (int)((long)(w - 4) * v / g->vmax);
        wt_sunken(S, x, y, w, h, C_WHITE);
        s_rect(S, x + 2, y + 2, fw, h - 4, g->bg >= 0 ? g->bg : C_GREEN);
        if (h >= 16) {
            char *o = utoa(t, (unsigned)((long)v * 100 / g->vmax));
            o[0] = '%'; o[1] = 0;
            s_text_center(S, x + w / 2, y + (h - 16) / 2, t, C_BLACK, -1);
        }
        break; }
    case WT_BOX:
        s_frame(S, x, y + 8, w, h - 8, C_DGREY);
        s_frame(S, x + 1, y + 9, w - 2, h - 10, C_WHITE);
        if (g->text[0]) s_text(S, x + 8, y, g->text, g->fg, WT.bg);
        break;
    case WT_CANVAS:
        if (g->draw) {
            struct surf sub;
            if (x < 0 || y < 0 || x + w > S->w || y + h > S->h)
                break;
            sub.p = S->p + y * S->stride + x; sub.stride = S->stride; sub.w = w; sub.h = h;
            g->draw(&sub, w, h);
        }
        break;
    }
}

WT_FN void wt_paint(void)
{
    struct surf S;
    int i;
    if (!WT.c)
        return;
    wt_layout();                                            /* por si se agregaron o movieron controles */
    S = surf_of(WT.c);
    s_rect(&S, 0, 0, S.w, S.h, WT.bg);
    for (i = 0; i < WT.n; i++)
        if (!(WT.w[i].flags & WT_HIDDEN))
            wt_draw_widget(&S, i);
    if (WT.nm) {
        s_rect(&S, 0, 0, S.w, WT_MENU_H, C_GREY);
        s_rect(&S, 0, WT_MENU_H - 1, S.w, 1, C_DGREY);
        for (i = 0; i < WT.nm; i++) {
            int on = WT.open == i;
            if (on) s_rect(&S, WT.m[i].x, 1, WT.m[i].w, WT_MENU_H - 2, C_BLUE);
            s_text(&S, WT.m[i].x + 8, 2, WT.m[i].title, on ? C_WHITE : C_BLACK, -1);
        }
        if (WT.open >= 0) {
            const struct wt_menu *m = &WT.m[WT.open];
            int k, mw = 0, x = m->x, y = WT_MENU_H - 1;
            for (k = 0; k < m->n; k++) if (slen(m->items[k]) > mw) mw = slen(m->items[k]);
            mw = mw * 8 + 16;
            s_rect(&S, x, y, mw + 2, m->n * 18 + 4, C_DGREY);
            s_rect(&S, x, y, mw + 1, m->n * 18 + 3, C_WHITE);
            s_rect(&S, x + 1, y + 1, mw, m->n * 18 + 2, C_LGREY);
            for (k = 0; k < m->n; k++) {
                int on = WT.hover == k;
                if (on) s_rect(&S, x + 2, y + 2 + k * 18, mw - 2, 18, C_BLUE);
                s_text(&S, x + 8, y + 3 + k * 18, m->items[k], on ? C_WHITE : C_BLACK, -1);
            }
        }
    }
    if (WT.msg[0]) {                                        /* cuadro de aviso encima de todo */
        int lines = 1, maxl = 0, cur = 0, bw, bh, bx, by, k, ly, fit = (S.w - 40) / 8, sp = -1;
        char wm[sizeof WT.msg];
        for (k = 0; WT.msg[k]; k++) {                       /* partir en renglones que quepan (en los espacios) */
            wm[k] = WT.msg[k];
            if (wm[k] == '\n') { cur = 0; sp = -1; continue; }
            if (wm[k] == ' ') sp = k;
            if (++cur > fit && sp >= 0) { wm[sp] = '\n'; cur = k - sp; sp = -1; }
        }
        wm[k] = 0;
        cur = 0;
        for (k = 0; wm[k]; k++) {
            if (wm[k] == '\n') { lines++; cur = 0; } else if (++cur > maxl) maxl = cur;
        }
        bw = maxl * 8 + 32; if (bw < 160) bw = 160; if (bw > S.w - 8) bw = S.w - 8;
        bh = lines * 16 + 56;
        bx = (S.w - bw) / 2; by = (S.h - bh) / 2;
        s_rect(&S, bx + 3, by + 3, bw, bh, C_DGREY);
        s_rect(&S, bx, by, bw, bh, C_WHITE);
        s_frame(&S, bx, by, bw, bh, C_BLACK);
        ly = by + 12;
        for (k = 0; wm[k]; ) {
            char line[96];
            int q = 0;
            while (wm[k] && wm[k] != '\n' && q < 95) line[q++] = wm[k++];
            line[q] = 0;
            if (wm[k] == '\n') k++;
            wt_text_clip(&S, bx + 16, ly, line, (bw - 24) / 8, C_BLACK, -1);
            ly += 16;
        }
        s_button(&S, bx + bw / 2 - 30, by + bh - 30, 60, 22, "OK", C_LGREY, C_BLACK, 0);
    }
    WT.c->damage(WT.c, 0, 0, S.w, S.h);
    WT.dirty = 0;
}

/* ---------- eventos ---------- */
WT_FN void wt_fire(int i, int ev)
{
    wt_ev = ev;
    if (WT.w[i].cb)
        WT.w[i].cb(i);
    WT.dirty = 1;
}

WT_FN void wt_next_focus(int dir)
{
    int k, i = WT.focus;
    for (k = 0; k < WT.n; k++) {
        i = (i + dir + WT.n) % WT.n;
        if (i >= 0 && wt_focusable(i)) { WT.focus = i; WT.dirty = 1; return; }
    }
}

WT_FN void wt_list_move(int i, int d)
{
    struct wt_widget *g = &WT.w[i];
    int rows = wt_list_rows(g), s;
    if (!g->val || !g->nitems) return;
    s = *g->val + d;
    if (s < 0) s = 0;
    if (s >= g->nitems) s = g->nitems - 1;
    if (s == *g->val) return;
    *g->val = s;
    if (s < g->top) g->top = s;
    if (s >= g->top + rows) g->top = s - rows + 1;
    wt_fire(i, WT_EV_CHANGE);
}

WT_FN int wt_menu_pick(int k)
{
    const struct wt_menu *m = &WT.m[WT.open];
    WT.open = -1;
    WT.dirty = 1;
    wt_ev = WT_EV_ACTIVATE;
    if (k >= 0 && k < m->n && m->cb)
        m->cb(k);
    return 1;
}

WT_FN void wt_key_cb(struct wctx *c, int k)
{
    struct wt_widget *g;
    WT.c = c;
    if (WT.msg[0]) {                                        /* el aviso se cierra con Enter, Espacio o ESC */
        if (k == '\n' || k == '\r' || k == ' ' || k == 27) { WT.msg[0] = 0; WT.dirty = 1; }
        return;
    }
    if (WT.open >= 0) {
        const struct wt_menu *m = &WT.m[WT.open];
        if (k == KEY_DOWN) WT.hover = (WT.hover + 1) % m->n;
        else if (k == KEY_UP) WT.hover = (WT.hover + m->n - 1) % m->n;
        else if (k == KEY_RIGHT) { WT.open = (WT.open + 1) % WT.nm; WT.hover = 0; }
        else if (k == KEY_LEFT) { WT.open = (WT.open + WT.nm - 1) % WT.nm; WT.hover = 0; }
        else if (k == '\n' || k == '\r') { wt_menu_pick(WT.hover); return; }
        else if (k == 27 || k == 9) WT.open = -1;
        WT.dirty = 1;
        return;
    }
    if (WT.keyhook && WT.keyhook(k))
        return;
    if (k == 9) { wt_next_focus(1); return; }
    if (!wt_ok(WT.focus)) return;
    g = &WT.w[WT.focus];
    if (g->flags & (WT_HIDDEN | WT_DISABLED)) return;
    switch (g->type) {
    case WT_BUTTON:
        if (k == '\n' || k == '\r' || k == ' ') wt_fire(WT.focus, WT_EV_ACTIVATE);
        break;
    case WT_CHECK:
        if ((k == '\n' || k == '\r' || k == ' ') && g->val) { *g->val = !*g->val; wt_fire(WT.focus, WT_EV_CHANGE); }
        break;
    case WT_LIST:
        if (k == KEY_DOWN) wt_list_move(WT.focus, 1);
        else if (k == KEY_UP) wt_list_move(WT.focus, -1);
        else if (k == '\n' || k == '\r') wt_fire(WT.focus, WT_EV_ACTIVATE);
        break;
    case WT_INPUT: {
        int len = slen(g->buf);
        if (k == '\n' || k == '\r') { wt_fire(WT.focus, WT_EV_ACTIVATE); break; }
        if (k == 8 || k == 127 || k == 20) {
            if (len) { g->buf[len - 1] = 0; WT.dirty = 1; }
            break;
        }
        if (k >= 'A' && k <= 'Z') k += 32;                 /* sin Shift = minuscula */
        else if (k >= 'a' && k <= 'z') k -= 32;
        if (k >= 32 && k < 256 && k != 127 && len < g->max - 1) {
            g->buf[len] = (char)k; g->buf[len + 1] = 0;
            WT.dirty = 1;
        }
        break; }
    }
    WT.blink = 0;
}

WT_FN void wt_mouse_cb(struct wctx *c, int x, int y, int btn, int prev)
{
    int press = (btn & 1) && !(prev & 1), release = !(btn & 1) && (prev & 1), i;
    WT.c = c;
    if (WT.msg[0]) {
        if (release) { WT.msg[0] = 0; WT.dirty = 1; }
        return;
    }
    if (WT.nm && (WT.open >= 0 || (press && y < WT_MENU_H))) {
        int k;
        if (WT.open >= 0) {                                 /* menu abierto: el raton manda sobre el */
            const struct wt_menu *m = &WT.m[WT.open];
            int mw = 0, mx = m->x, my = WT_MENU_H + 1;
            for (k = 0; k < m->n; k++) if (slen(m->items[k]) > mw) mw = slen(m->items[k]);
            mw = mw * 8 + 16;
            if (inside(x, y, mx, my, mw, m->n * 18)) {
                int h = (y - my) / 18;
                if (h != WT.hover) { WT.hover = h; WT.dirty = 1; }
                if (release) wt_menu_pick(h);
                return;
            }
            if (!press) return;
        }
        for (k = 0; k < WT.nm; k++)
            if (press && y < WT_MENU_H && x >= WT.m[k].x && x < WT.m[k].x + WT.m[k].w) {
                WT.open = WT.open == k ? -1 : k;
                WT.hover = -1;
                WT.dirty = 1;
                return;
            }
        WT.open = -1;
        WT.dirty = 1;
        return;
    }
    if (WT.pressed >= 0 && WT.w[WT.pressed].type == WT_CANVAS && (btn & 3)) {      /* arrastre en un lienzo */
        struct wt_widget *g = &WT.w[WT.pressed];
        if (g->click) g->click(WT.pressed, x - g->rx, y - g->ry, btn);
        WT.dirty = 1;
        return;
    }
    if (release) {
        if (wt_ok(WT.pressed)) {
            struct wt_widget *g = &WT.w[WT.pressed];
            int in = inside(x, y, g->rx, g->ry, g->rw, g->rh);
            if (g->type == WT_BUTTON && in) wt_fire(WT.pressed, WT_EV_ACTIVATE);
            else if (g->type == WT_CHECK && in && g->val) { *g->val = !*g->val; wt_fire(WT.pressed, WT_EV_CHANGE); }
        }
        WT.pressed = -1;
        WT.dirty = 1;
        return;
    }
    if (!(press || ((btn & 2) && !(prev & 2))))
        return;
    for (i = WT.n - 1; i >= 0; i--) {
        struct wt_widget *g = &WT.w[i];
        if ((g->flags & (WT_HIDDEN | WT_DISABLED)) || g->type == WT_LABEL || g->type == WT_BOX || g->type == WT_PROGRESS)
            continue;
        if (!inside(x, y, g->rx, g->ry, g->rw, g->rh))
            continue;
        if (wt_focusable(i)) WT.focus = i;
        WT.pressed = i;
        if (g->type == WT_CANVAS && g->click)
            g->click(i, x - g->rx, y - g->ry, btn);
        else if (g->type == WT_LIST && g->val) {
            int rows = wt_list_rows(g);
            if (g->nitems > rows && x >= g->rx + g->rw - 12) {           /* barra: media pagina arriba o abajo */
                int ty = g->ry + 2 + (g->rh - 4) * g->top / g->nitems;
                g->top += y < ty ? -rows : rows;
            } else {
                int r = g->top + (y - g->ry - 2) / 16;
                unsigned now = K->ms();
                if (r >= 0 && r < g->nitems) {
                    int dbl = r == WT.last_row && i == WT.last_list && now - WT.last_click < 450;
                    if (r != *g->val) { *g->val = r; wt_fire(i, WT_EV_CHANGE); }
                    if (dbl) wt_fire(i, WT_EV_ACTIVATE);
                    WT.last_row = r; WT.last_list = i; WT.last_click = now;
                }
            }
        }
        WT.blink = 0;
        WT.dirty = 1;
        return;
    }
}

WT_FN void wt_start_cb(struct wctx *c)
{
    WT.c = c;
    WT.n = 0; WT.nm = 0; WT.open = -1; WT.hover = -1;
    WT.focus = -1; WT.pressed = -1; WT.msg[0] = 0;
    WT.last_list = -1;
    if (WT.build)
        WT.build();
    wt_layout();
    if (!wt_ok(WT.focus) || !wt_focusable(WT.focus)) { WT.focus = -1; wt_next_focus(1); }
    if (WT.title[0]) c->title(c, WT.title);
    wt_paint();
}

WT_FN void wt_frame_cb(struct wctx *c)
{
    static unsigned last;
    unsigned now;
    WT.c = c;
    now = K->ms();
    if (last && now - last < 1000) {
        unsigned before = WT.blink;
        WT.blink += now - last;
        if (wt_ok(WT.focus) && WT.w[WT.focus].type == WT_INPUT && before / 500 != WT.blink / 500)
            WT.dirty = 1;
    }
    last = now;
    if (WT.tick)
        WT.tick();
    if (WT.dirty)
        wt_paint();
}

WT_FN void wt_resize_cb(struct wctx *c)
{
    WT.c = c;
    wt_layout();
    if (WT.on_resize)
        WT.on_resize(c->w, wt_height());
    wt_layout();
    wt_paint();
}

WT_FN void wt_stop_cb(struct wctx *c)
{
    (void)c;
    if (WT.on_close)
        WT.on_close();
    WT.c = 0;
}

/* entrega la ventana al escritorio. build() crea los controles (se llama al abrirse). Devuelve MOD_WINAPP: hay que
 * devolverlo desde mod_main. Desde la consola, PKG y EXEC abren el escritorio solos para mostrarla. */
WT_FN int wt_window(const char *title, int w, int h, int resizable, void (*build)(void))
{
    scopy(WT.title, title, (int)sizeof WT.title);
    WT.iw = w; WT.ih = h; WT.resizable = resizable;
    WT.build = build;
    if (!WT.bgset) WT.bg = C_LGREY;
    memset(&WT.app, 0, sizeof WT.app);
    WT.app.title = WT.title;
    WT.app.w = w; WT.app.h = h;
    WT.app.minw = w < 160 ? w : 160; WT.app.minh = h < 80 ? h : 80;
    WT.app.resizable = resizable;
    WT.app.start = wt_start_cb;
    WT.app.frame = wt_frame_cb;
    WT.app.key = wt_key_cb;
    WT.app.mouse = wt_mouse_cb;
    WT.app.resize = wt_resize_cb;
    WT.app.stop = wt_stop_cb;
    K->wm_publish(&WT.app);
    return MOD_WINAPP;
}

#endif
