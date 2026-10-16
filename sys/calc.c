/* calc.c - calculadora en ventana (modulo /SYS/CALC.APP): enteros de 32 bits, + - * / con raton o teclado.
 * Las teclas se ajustan al tamano de la ventana. */
#include "ui.h"
#include "mod_head.h"

static const char *const keys[5][4] = {
    { "C", "+/-", "%", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", "0", ".", "=" },
};

static long acc, cur;
static int op, fresh = 1, error;
static int down_r = -1, down_c = -1;
static int W, H;

#define DISP_H 40
#define GAP 4

static void cell_rect(int r, int c, int *x, int *y, int *w, int *h)
{
    int bw = (W - GAP * 5) / 4, bh = (H - DISP_H - GAP * 7) / 5;
    *x = GAP + c * (bw + GAP);
    *y = DISP_H + GAP * 2 + r * (bh + GAP);
    *w = bw;
    *h = bh;
    if (r == 4 && c == 0)
        *w = bw * 2 + GAP;
}

static void draw(struct wctx *c)
{
    struct surf S = surf_of(c);
    char t[24], *o = t;
    int r, cc;
    long v = cur;
    W = c->w;
    H = c->h;
    s_rect(&S, 0, 0, W, H, C_LGREY);
    s_rect(&S, GAP, GAP, W - GAP * 2, DISP_H - GAP, C_BLACK);
    if (error) {
        scopy(t, "ERROR", 8);
    } else {
        if (v < 0) { *o++ = '-'; v = -v; }
        utoa(o, (unsigned)v);
    }
    s_text(&S, W - GAP - 8 - slen(t) * 8, GAP + (DISP_H - GAP - 16) / 2, t, C_LGREEN, C_BLACK);
    for (r = 0; r < 5; r++)
        for (cc = 0; cc < 4; cc++) {
            int x, y, w, h;
            if (r == 4 && cc == 1)
                continue;
            cell_rect(r, cc, &x, &y, &w, &h);
            s_button(&S, x, y, w, h, keys[r][cc], cc == 3 ? C_ORANGE : (r == 0 ? C_LRED : C_WHITE), C_BLACK,
                     r == down_r && cc == down_c);
        }
    c->damage(c, 0, 0, W, H);
}

static void apply(void)
{
    if (op == '+') acc += cur;
    else if (op == '-') acc -= cur;
    else if (op == '*') acc *= cur;
    else if (op == '/') {
        if (!cur) { error = 1; return; }
        acc /= cur;
    } else acc = cur;
    if (acc > 2147483647L || acc < -2147483647L) { error = 1; return; }
    cur = acc;
}

static void press(const char *k)
{
    char c = k[0];
    if (error && c != 'C')
        return;
    if (c >= '0' && c <= '9') {
        if (fresh) { cur = 0; fresh = 0; }
        if (cur < 214748364L)
            cur = cur * 10 + (c - '0');
    } else if (c == 'C') {
        acc = cur = 0; op = 0; fresh = 1; error = 0;
    } else if (c == '+' && k[1] == '/') {
        cur = -cur;
    } else if (c == '%') {
        cur = cur / 100;
        fresh = 1;
    } else if (c == '.') {
        /* solo enteros */
    } else if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (!fresh || op == 0) apply();
        op = c;
        fresh = 1;
    } else if (c == '=') {
        apply();
        op = 0;
        fresh = 1;
    }
}

static void start(struct wctx *c) { draw(c); }
static void resize(struct wctx *c) { draw(c); }

static void key(struct wctx *c, int k)
{
    static char kb[2];
    kb[1] = 0;
    if ((k >= '0' && k <= '9') || k == '+' || k == '-' || k == '*' || k == '/' || k == '=' || k == '%') {
        kb[0] = (char)k;
        press(kb);
    } else if (k == '\n') {
        press("=");
    } else if (k == 'c' || k == 'C') {
        press("C");
    } else if (k == '\b') {
        cur /= 10;
    } else {
        return;
    }
    draw(c);
}

static void mouse(struct wctx *c, int x, int y, int btn, int prev)
{
    int r, cc, hr = -1, hc = -1;
    for (r = 0; r < 5; r++)
        for (cc = 0; cc < 4; cc++) {
            int bx, by, bw, bh;
            if (r == 4 && cc == 1)
                continue;
            cell_rect(r, cc, &bx, &by, &bw, &bh);
            if (inside(x, y, bx, by, bw, bh)) { hr = r; hc = cc; }
        }
    if ((btn & 1) && !(prev & 1)) {
        down_r = hr; down_c = hc;
        draw(c);
    } else if (!(btn & 1) && (prev & 1)) {
        if (hr >= 0 && hr == down_r && hc == down_c)
            press(keys[hr][hc]);
        down_r = down_c = -1;
        draw(c);
    }
}

static const struct wapp app = {
    "CALCULADORA", 232, 300, 168, 220, 1, start, 0, key, mouse, resize, 0
};

int mod_main(const struct kapi *api)
{
    K = api;
    K->wm_publish(&app);
    return MOD_WINAPP;
}
