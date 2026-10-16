/* pong.c - Pong en ventana (modulo /SYS/PONG.APP)
 * Tu paleta: raton (arriba/abajo dentro de la ventana), W/S o flechas. ESPACIO pausa. Gana el primero en 7.
 * La cancha se ajusta al tamano de la ventana.
 */
#include "ui.h"
#include "mod_head.h"

#define PW 8
#define PH 56
#define BS 8
#define WIN 7

static int W, H, fx, fy, fw, fh;
static int lp, rp;                          /* y de las paletas */
static int bx, by, vx, vy;                  /* pelota y velocidad por tick */
static int ls, rs, serve_dir = 1, paused, over;
static int last, wait_until, mouse_y = -1, dirty;

static void layout(struct wctx *c)
{
    W = c->w;
    H = c->h;
    fx = 6;
    fy = 22;
    fw = W - 12;
    fh = H - fy - 22;
    if (fw < 100) fw = 100;
    if (fh < 60) fh = 60;
}

static void serve(void)
{
    bx = fx + fw / 2 - BS / 2;
    by = fy + fh / 2 - BS / 2;
    vx = 4 * serve_dir;
    vy = (int)(K->seed() % 5) - 2;
    if (!vy)
        vy = 1;
}

static void clamp_paddle(int *p)
{
    if (*p < fy) *p = fy;
    if (*p + PH > fy + fh) *p = fy + fh - PH;
}

static void hit_paddle(int py)
{
    int off = (by + BS / 2) - (py + PH / 2);
    vx = -vx;
    if (vx > 0 && vx < 12) vx++;
    else if (vx < 0 && vx > -12) vx--;
    vy += off / 9;
    if (vy > 8) vy = 8;
    if (vy < -8) vy = -8;
    if (!vy) vy = 1;
    K->sound(SND_SQUARE, 880, 0, 50);
}

static void draw(struct wctx *c)
{
    struct surf S = surf_of(c);
    char t[8];
    int y;
    s_rect(&S, 0, 0, W, H, C_BLUE);
    s_rect(&S, fx - 2, fy - 2, fw + 4, fh + 4, C_WHITE);
    s_rect(&S, fx, fy, fw, fh, C_BLACK);
    for (y = fy + 6; y < fy + fh - 6; y += 20)
        s_rect(&S, fx + fw / 2 - 1, y, 3, 10, C_DGREY);
    s_rect(&S, fx + 12, lp, PW, PH, C_WHITE);
    s_rect(&S, fx + fw - 12 - PW, rp, PW, PH, C_WHITE);
    s_rect(&S, bx, by, BS, BS, C_YELLOW);
    utoa(t, (unsigned)ls);
    s_text(&S, fx + fw / 2 - 48, 3, t, C_WHITE, -1);
    utoa(t, (unsigned)rs);
    s_text(&S, fx + fw / 2 + 40, 3, t, C_WHITE, -1);
    s_text(&S, fx, fy + fh + 5, "RATON/W/S/FLECHAS  ESPACIO=PAUSA", C_LGREY, -1);
    if (paused)
        s_text_center(&S, fx + fw / 2, fy + fh / 2 - 8, " PAUSA ", C_YELLOW, C_BLACK);
    if (over)
        s_text_center(&S, fx + fw / 2, fy + fh / 2 - 8,
                      ls >= WIN ? " GANASTE! (ESPACIO=OTRA) " : " PERDISTE (ESPACIO=OTRA) ", C_YELLOW, C_BLACK);
    c->damage(c, 0, 0, W, H);
}

static void start(struct wctx *c)
{
    layout(c);
    ls = rs = 0;
    paused = over = 0;
    lp = rp = fy + fh / 2 - PH / 2;
    serve();
    last = (int)K->ms();
    wait_until = 0;
    dirty = 1;
}

static void resize(struct wctx *c)
{
    layout(c);
    clamp_paddle(&lp);
    clamp_paddle(&rp);
    serve();
    dirty = 1;
}

static void key(struct wctx *c, int k)
{
    (void)c;
    if (k >= 'a' && k <= 'z')
        k -= 32;
    if (k == ' ') {
        if (over) {
            ls = rs = 0;
            over = 0;
            serve();
        } else {
            paused = !paused;
        }
        last = (int)K->ms();
        dirty = 1;
    }
    if (k == 'W' || k == KEY_UP)   { lp -= 14; dirty = 1; }
    if (k == 'S' || k == KEY_DOWN) { lp += 14; dirty = 1; }
}

static void mouse(struct wctx *c, int x, int y, int btn, int prev)
{
    (void)c; (void)x; (void)btn; (void)prev;
    mouse_y = y;
}

static void frame(struct wctx *c)
{
    int now = (int)K->ms(), ticked = 0;

    if (mouse_y >= 0) {
        int ny = mouse_y - PH / 2;
        if (ny != lp) { lp = ny; dirty = 1; }
        mouse_y = -1;
    }
    clamp_paddle(&lp);
    if (wait_until && now < wait_until) {
        last = now;
    } else {
        wait_until = 0;
        while (!paused && !over && now - last >= 16) {
            last += 16;
            ticked = 1;
            bx += vx;
            by += vy;
            if (by <= fy) { by = fy; vy = -vy; K->sound(SND_TRIANGLE, 330, 0, 40); }
            if (by + BS >= fy + fh) { by = fy + fh - BS; vy = -vy; K->sound(SND_TRIANGLE, 330, 0, 40); }
            if (vx < 0 && bx <= fx + 12 + PW && bx + BS >= fx + 12 && by + BS >= lp && by <= lp + PH) {
                bx = fx + 12 + PW;
                hit_paddle(lp);
            }
            if (vx > 0 && bx + BS >= fx + fw - 12 - PW && bx <= fx + fw - 12 && by + BS >= rp && by <= rp + PH) {
                bx = fx + fw - 12 - PW - BS;
                hit_paddle(rp);
            }
            {                                   /* la computadora sigue la pelota */
                int cc = rp + PH / 2, spd = 3 + (vx > 8 || vx < -8);
                if (vx > 0 || bx > fx + fw / 2) {
                    if (by + BS / 2 > cc + 6) rp += spd;
                    else if (by + BS / 2 < cc - 6) rp -= spd;
                }
                clamp_paddle(&rp);
            }
            if (bx + BS < fx) {
                rs++;
                serve_dir = -1;
                K->sound(SND_KICK, 55, 0, 300);
                serve();
                wait_until = now + 400;
                break;
            } else if (bx > fx + fw) {
                ls++;
                serve_dir = 1;
                K->sound(SND_SNARE, 200, 0, 200);
                serve();
                wait_until = now + 400;
                break;
            }
            if (ls >= WIN || rs >= WIN) {
                over = 1;
                K->sound(SND_DRUMKIT, 60, 0, 1500);
            }
        }
    }
    if (ticked || dirty) {
        draw(c);
        dirty = 0;
    }
}

static const struct wapp app = {
    "PONG", 480, 320, 220, 160, 1, start, frame, key, mouse, resize, 0
};

int mod_main(const struct kapi *api)
{
    K = api;
    K->wm_publish(&app);
    return MOD_WINAPP;
}
