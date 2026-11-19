/* forosdl.c - cliente del FORO de BASIC-OS para Linux, con SDL2 (porque se puede).
 *
 * Habla con el mismo hub que FORO.APP (hub/server.py): /foro/list?u=1, /foro/hilo/ID?u=1 y POST /foro/post. Aqui el texto
 * va en UTF-8 tal cual (acentos y ñ incluidos); la consola de BASIC-OS los ve sin acento.
 * La red la hace el programa curl (el de la terminal), en un hilo aparte para que la ventana no se congele mientras el hub
 * de Render despierta.
 *
 * Compilar:  make tools/forosdl        (necesita SDL2, SDL2_ttf y curl:  sudo pacman -S sdl2 sdl2_ttf curl ttf-dejavu)
 * Usar:      tools/forosdl [https://mi-hub.onrender.com] [clave]
 *   Sin argumentos usa https://basic-os.onrender.com. La clave (la de la comunidad) se busca en la variable BASICOS_HUB_KEY,
 *   en ~/.config/basicos-foro.cfg, en la 2a linea de HUB.CFG (carpeta actual) o en ~/BASIC-OS/hub.key; si no hay, se pide al escribir.
 *   La fuente: FOROSDL_FONT=/ruta/a/fuente.ttf (por defecto DejaVu Sans Mono).
 *
 * Teclas:  lista: flechas + Enter (o clic) = abrir, N = hilo nuevo, R/F5 = recargar, Esc = salir
 *          hilo:  flechas, RePag/AvPag, Inicio/Fin, rueda = desplazar, R = responder, Esc/Retroceso = volver
 *          escribir: Tab = siguiente campo, Ctrl+Enter = enviar, Ctrl+V = pegar, Esc = cancelar
 * Licencia: GPL-2.0-or-later, como el resto de BASIC-OS.
 */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* ---- colores (los del C64, como el sistema) ---- */
static const SDL_Color C_BG = { 0x35, 0x28, 0x79, 255 }, C_PANEL = { 0x6c, 0x5e, 0xb5, 255 }, C_WHITE = { 255, 255, 255, 255 },
                       C_GREEN = { 0x9a, 0xd2, 0x84, 255 }, C_YELLOW = { 0xb8, 0xc7, 0x6f, 255 }, C_CYAN = { 0x70, 0xa4, 0xb2, 255 },
                       C_GREY = { 0x95, 0x95, 0x95, 255 }, C_DARK = { 0x22, 0x1a, 0x52, 255 }, C_RED = { 0xd0, 0x70, 0x60, 255 };

static SDL_Window *Win;
static SDL_Renderer *R;
static TTF_Font *font;
static int WW = 960, WH = 640, cw = 8, chh = 16;     /* ventana y tamano de una letra */
static char hub[256] = "https://basic-os.onrender.com", key[128], nick[40];
static char status[300];
static int status_err;

/* ================= utilidades UTF-8 ================= */
static int u8len(const char *s)                       /* letras (puntos de codigo) */
{
    int n = 0;
    for (; *s; s++)
        if ((*s & 0xC0) != 0x80) n++;
    return n;
}
static const char *u8skip(const char *s, int n)       /* avanza n letras */
{
    while (*s && n > 0) {
        s++;
        while ((*s & 0xC0) == 0x80) s++;
        n--;
    }
    return s;
}
static void u8pop(char *s)                            /* borra la ultima letra */
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    s[n] = 0;
}
static void scopy(char *d, const char *s, size_t max) { snprintf(d, max, "%s", s); }

static void set_status(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_err = err;
}

/* ================= configuracion ================= */
static char cfg_path[512];

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

static void load_cfg(void)
{
    const char *home = getenv("HOME");
    char line[512], path[512];
    FILE *f;
    if (home) snprintf(cfg_path, sizeof cfg_path, "%s/.config/basicos-foro.cfg", home);
    if (cfg_path[0] && (f = fopen(cfg_path, "r"))) {
        while (fgets(line, sizeof line, f)) {
            trim(line);
            if (!strncmp(line, "hub=", 4)) scopy(hub, line + 4, sizeof hub);
            else if (!strncmp(line, "key=", 4)) scopy(key, line + 4, sizeof key);
            else if (!strncmp(line, "nick=", 5)) scopy(nick, line + 5, sizeof nick);
        }
        fclose(f);
    }
    if (!key[0] && getenv("BASICOS_HUB_KEY")) scopy(key, getenv("BASICOS_HUB_KEY"), sizeof key);
    if (!key[0] && (f = fopen("HUB.CFG", "r"))) {
        if (fgets(line, sizeof line, f) && fgets(line, sizeof line, f)) { trim(line); scopy(key, line, sizeof key); }
        fclose(f);
    }
    if (!key[0] && home) {
        snprintf(path, sizeof path, "%s/BASIC-OS/hub.key", home);
        if ((f = fopen(path, "r"))) {
            if (fgets(line, sizeof line, f)) { trim(line); scopy(key, line, sizeof key); }
            fclose(f);
        }
    }
}

static void save_cfg(void)
{
    FILE *f;
    char dir[512];
    if (!cfg_path[0]) return;
    snprintf(dir, sizeof dir, "%s", cfg_path);
    *strrchr(dir, '/') = 0;
    mkdir(dir, 0755);
    if ((f = fopen(cfg_path, "w"))) {
        fprintf(f, "hub=%s\nkey=%s\nnick=%s\n", hub, key, nick);
        fclose(f);
        chmod(cfg_path, 0600);
    }
}

/* ================= red: curl en un hilo ================= */
enum { J_NONE, J_LIST, J_THREAD, J_POST };
static SDL_atomic_t job_busy, job_done;
static int job_kind, job_code;
static char *job_out;
static size_t job_len;
static char job_url[600], job_file[256], job_arg[16];

/* corre curl sin shell (execvp): la salida va a *out, el codigo HTTP a *code (0 = no hubo respuesta) */
static int run_curl(const char *url, const char *postfile, char **out, size_t *len, int *code)
{
    int pfd[2];
    pid_t pid;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap), data[300];
    ssize_t r;
    if (!buf || pipe(pfd) < 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(pfd[1], 1);
        if (dn >= 0) dup2(dn, 2);
        close(pfd[0]);
        if (postfile) {
            snprintf(data, sizeof data, "@%s", postfile);
            execlp("curl", "curl", "-sS", "-m", "120", "-w", "\n%{http_code}", "-H", "Content-Type: application/octet-stream",
                   "--data-binary", data, url, (char *)0);
        } else
            execlp("curl", "curl", "-sS", "-L", "-m", "120", "-w", "\n%{http_code}", url, (char *)0);
        _exit(127);
    }
    close(pfd[1]);
    while ((r = read(pfd[0], buf + n, cap - n - 1)) > 0) {
        n += (size_t)r;
        if (n + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
    }
    close(pfd[0]);
    waitpid(pid, 0, 0);
    buf[n] = 0;
    {   /* la ultima linea es el codigo HTTP que pidio -w */
        char *nl = strrchr(buf, '\n');
        *code = nl ? atoi(nl + 1) : 0;
        if (nl) { *nl = 0; n = (size_t)(nl - buf); }
    }
    *out = buf;
    *len = n;
    return 0;
}

static int job_thread(void *u)
{
    (void)u;
    run_curl(job_url, job_kind == J_POST ? job_file : 0, &job_out, &job_len, &job_code);
    SDL_AtomicSet(&job_done, 1);
    return 0;
}

static int start_job(int kind, const char *path, const char *postfile, const char *arg)
{
    SDL_Thread *t;
    if (SDL_AtomicGet(&job_busy))
        return 0;
    job_kind = kind;
    snprintf(job_url, sizeof job_url, "%s%s", hub, path);
    scopy(job_file, postfile ? postfile : "", sizeof job_file);
    scopy(job_arg, arg ? arg : "", sizeof job_arg);
    free(job_out); job_out = 0;
    SDL_AtomicSet(&job_done, 0);
    SDL_AtomicSet(&job_busy, 1);
    t = SDL_CreateThread(job_thread, "curl", 0);
    if (!t) { SDL_AtomicSet(&job_busy, 0); return 0; }
    SDL_DetachThread(t);
    set_status(0, kind == J_POST ? "Enviando..." : "Cargando... (si el hub estaba dormido tarda ~1 minuto)");
    return 1;
}

/* ================= datos ================= */
struct th { char id[8]; int n; char date[12]; char nick[48]; char title[160]; };
#define MAXTH 200
static struct th ths[MAXTH];
static int nth, sel, list_scroll;

struct line { char *t; int kind; };                 /* kind: 0 texto, 1 titulo, 2 encabezado de mensaje, 3 vacia */
static struct line *lines;
static int nlines, cap_lines, thr_scroll;
static char *thr_text;
static char cur_id[8], cur_title[160];

enum { V_LIST, V_THREAD };
static int view = V_LIST;

static void parse_list(const char *s)
{
    nth = 0;
    while (*s && nth < MAXTH) {
        const char *e = strchr(s, '\n');
        char ln[400];
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l >= sizeof ln) l = sizeof ln - 1;
        memcpy(ln, s, l); ln[l] = 0;
        trim(ln);
        if (ln[0] && ln[0] != '#') {
            struct th *t = &ths[nth];
            char *p = ln, *q;
            memset(t, 0, sizeof *t);
            q = strchr(p, ' '); if (!q) goto next; *q = 0; scopy(t->id, p, sizeof t->id); p = q + 1;
            t->n = atoi(p); q = strchr(p, ' '); if (!q) goto next; p = q + 1;
            q = strchr(p, ' '); if (!q) goto next; *q = 0; scopy(t->date, p, sizeof t->date); p = q + 1;
            q = strchr(p, ' '); if (!q) goto next; *q = 0; scopy(t->nick, p, sizeof t->nick); p = q + 1;
            scopy(t->title, p, sizeof t->title);
            nth++;
        }
    next:
        if (!e) break;
        s = e + 1;
    }
    if (sel >= nth) sel = nth - 1;
    if (sel < 0) sel = 0;
}

static void add_line(const char *t, int len, int kind)
{
    if (nlines >= cap_lines) {
        cap_lines = cap_lines ? cap_lines * 2 : 256;
        lines = realloc(lines, sizeof *lines * (size_t)cap_lines);
    }
    lines[nlines].t = strndup(t, (size_t)len);
    lines[nlines].kind = kind;
    nlines++;
}

/* parte un renglon en pedazos de hasta maxc letras, por los espacios */
static void wrap_into(const char *s, int maxc, int kind)
{
    if (!*s) { add_line("", 0, kind == 0 ? 3 : kind); return; }
    while (*s) {
        const char *end = u8skip(s, maxc), *brk = end;
        if (*end) {
            const char *p;
            for (p = end; p > s && *p != ' '; p--)
                ;
            if (p > s) brk = p;
        }
        add_line(s, (int)(brk - s), kind);
        s = brk;
        while (*s == ' ') s++;
    }
}

static int text_cols(void) { int c = (WW - 48) / cw; return c < 20 ? 20 : c; }

static void build_lines(void)
{
    int i;
    const char *s = thr_text;
    for (i = 0; i < nlines; i++) free(lines[i].t);
    nlines = 0;
    if (!s) return;
    while (*s) {
        const char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        char *ln = strndup(s, l);
        trim(ln);
        if (!strncmp(ln, "# ", 2)) { scopy(cur_title, ln + 2, sizeof cur_title); }
        else if (!strncmp(ln, "== ", 3)) { if (nlines) add_line("", 0, 3); wrap_into(ln + 3, text_cols(), 2); }
        else wrap_into(ln, text_cols(), 0);
        free(ln);
        if (!e) break;
        s = e + 1;
    }
}

/* ================= dibujo ================= */
#define TOPH 44
#define BOTH 26

struct btn { SDL_Rect r; int id; };
static struct btn btns[16];
static int nbtn, mx, my;

static void fill(int x, int y, int w, int h, SDL_Color c)
{
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(R, &r);
}
static void frame(int x, int y, int w, int h, SDL_Color c)
{
    SDL_Rect r = { x, y, w, h };
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
    SDL_RenderDrawRect(R, &r);
}
static int text(int x, int y, const char *s, SDL_Color c)          /* devuelve el ancho */
{
    SDL_Surface *sf;
    SDL_Texture *tx;
    SDL_Rect r;
    if (!s || !*s) return 0;
    sf = TTF_RenderUTF8_Blended(font, s, c);
    if (!sf) return 0;
    tx = SDL_CreateTextureFromSurface(R, sf);
    r.x = x; r.y = y; r.w = sf->w; r.h = sf->h;
    SDL_RenderCopy(R, tx, 0, &r);
    SDL_DestroyTexture(tx);
    SDL_FreeSurface(sf);
    return r.w;
}
static void text_clip(int x, int y, const char *s, int maxc, SDL_Color c)
{
    char b[1024];
    const char *e = u8skip(s, maxc);
    size_t l = (size_t)(e - s);
    if (l > sizeof b - 4) l = sizeof b - 4;
    memcpy(b, s, l); b[l] = 0;
    if (*e) strcat(b, "…");
    text(x, y, b, c);
}
static int button(int x, int y, const char *label, int id)
{
    int w = u8len(label) * cw + 20, h = chh + 10;
    int hov = mx >= x && mx < x + w && my >= y && my < y + h;
    fill(x, y, w, h, hov ? C_CYAN : C_PANEL);
    frame(x, y, w, h, C_WHITE);
    text(x + 10, y + 5, label, C_WHITE);
    if (nbtn < 16) { btns[nbtn].r.x = x; btns[nbtn].r.y = y; btns[nbtn].r.w = w; btns[nbtn].r.h = h; btns[nbtn].id = id; nbtn++; }
    return w;
}

enum { B_BACK = 1, B_RELOAD, B_NEW, B_REPLY, B_SEND, B_CANCEL };

/* ---- escribir (hilo nuevo o respuesta) ---- */
static int comp_open, comp_new, comp_focus;      /* foco: 0 nombre, 1 clave, 2 titulo, 3 mensaje */
static char c_title[256], c_msg[8192], c_nick[64], c_key[128];
#define MSG_MAX 1500

static int comp_field_ok(int f) { return f == 0 || f == 3 || (f == 1 && !key[0]) || (f == 2 && comp_new); }

static void open_composer(int is_new)
{
    comp_open = 1;
    comp_new = is_new;
    c_title[0] = c_msg[0] = 0;
    scopy(c_nick, nick, sizeof c_nick);
    scopy(c_key, key, sizeof c_key);
    comp_focus = !c_nick[0] ? 0 : !key[0] ? 1 : is_new ? 2 : 3;
    SDL_StartTextInput();
}

static void close_composer(void) { comp_open = 0; SDL_StopTextInput(); }

static SDL_Rect f_rect[4];

static void draw_field(int f, int x, int y, int w, int h, const char *label, const char *val, int secret)
{
    int foc = comp_focus == f;
    char shown[200];
    text(x, y, label, C_GREEN);
    y += chh + 4;
    fill(x, y, w, h, C_DARK);
    frame(x, y, w, h, foc ? C_YELLOW : C_GREY);
    f_rect[f].x = x; f_rect[f].y = y; f_rect[f].w = w; f_rect[f].h = h;
    if (secret) {
        int n = u8len(val), i;
        for (i = 0; i < n && i < 60; i++) shown[i] = '*';
        shown[i] = 0;
        val = shown;
    }
    if (h <= chh + 12) {                               /* una linea: se ve el final */
        int maxc = (w - 12) / cw - 1, n = u8len(val);
        const char *from = n > maxc ? u8skip(val, n - maxc) : val;
        int tw = text(x + 6, y + (h - chh) / 2, from, C_WHITE);
        if (foc && (SDL_GetTicks() / 500) % 2 == 0) fill(x + 6 + tw + 1, y + (h - chh) / 2, 2, chh, C_YELLOW);
    } else {                                           /* el mensaje: varias lineas, se ve el final */
        int maxc = (w - 12) / cw, rows = (h - 8) / chh, total = 0, k, cx = x + 6, cy = y + 4;
        const char *s = val;
        const char *starts[600];
        int lens[600];
        while (total < 600) {                          /* cortar en renglones */
            const char *e = strchr(s, '\n'), *lim = u8skip(s, maxc);
            const char *stop = e && e < lim ? e : lim;
            if (*lim && (!e || e >= lim)) {
                const char *p;
                for (p = stop; p > s && *p != ' '; p--)
                    ;
                if (p > s) stop = p;
            }
            starts[total] = s; lens[total] = (int)(stop - s); total++;
            if (!*stop) break;
            s = (*stop == '\n' || *stop == ' ') ? stop + 1 : stop;
            if (!*s && *stop == '\n') { starts[total] = s; lens[total] = 0; total++; break; }
        }
        for (k = total > rows ? total - rows : 0; k < total; k++) {
            char b[1024];
            int l = lens[k] < 1000 ? lens[k] : 1000;
            memcpy(b, starts[k], (size_t)l); b[l] = 0;
            {   int tw = text(cx, cy, b, C_WHITE);
                if (foc && k == total - 1 && (SDL_GetTicks() / 500) % 2 == 0) fill(cx + tw + 1, cy, 2, chh, C_YELLOW); }
            cy += chh;
        }
    }
}

static void draw_composer(void)
{
    int pw = WW - 80 < 820 ? WW - 80 : 820, ph = WH - 80, px = (WW - pw) / 2, py = 40, y, fw = pw - 40;
    char cnt[64];
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, 0, 0, 0, 140);
    { SDL_Rect all = { 0, 0, WW, WH }; SDL_RenderFillRect(R, &all); }
    fill(px, py, pw, ph, C_PANEL);
    frame(px, py, pw, ph, C_WHITE);
    text(px + 20, py + 12, comp_new ? "Hilo nuevo" : "Responder", C_YELLOW);
    if (!comp_new) text_clip(px + 20 + 12 * cw, py + 12, cur_title, (pw - 40) / cw - 12, C_WHITE);
    y = py + 44;
    memset(f_rect, 0, sizeof f_rect);
    draw_field(0, px + 20, y, fw / 2 - 10, chh + 12, "Tu nombre (letras y numeros, hasta 16):", c_nick, 0);
    if (!key[0]) draw_field(1, px + 20 + fw / 2 + 10, y, fw / 2 - 10, chh + 12, "Clave del hub:", c_key, 1);
    y += 2 * chh + 26;
    if (comp_new) {
        draw_field(2, px + 20, y, fw, chh + 12, "Titulo (3 a 50 letras):", c_title, 0);
        y += 2 * chh + 26;
    }
    draw_field(3, px + 20, y, fw, py + ph - y - chh - 70, "Mensaje:", c_msg, 0);
    snprintf(cnt, sizeof cnt, "%d / %d", u8len(c_msg), MSG_MAX);
    text(px + pw - 20 - u8len(cnt) * cw, py + ph - 44, cnt, u8len(c_msg) > MSG_MAX ? C_RED : C_GREY);
    button(px + 20, py + ph - 48, "Enviar (Ctrl+Enter)", B_SEND);
    button(px + 20 + 22 * cw, py + ph - 48, "Cancelar (Esc)", B_CANCEL);
}

static void draw(void)
{
    int x, y, i, rowh = chh + 14;
    nbtn = 0;
    SDL_SetRenderDrawColor(R, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(R);
    /* barra de arriba */
    fill(0, 0, WW, TOPH, C_PANEL);
    x = 12;
    if (view == V_THREAD) x += button(x, 8, "< Volver", B_BACK) + 8;
    x += button(x, 8, "Recargar", B_RELOAD) + 8;
    if (view == V_LIST) x += button(x, 8, "Hilo nuevo", B_NEW) + 8;
    else x += button(x, 8, "Responder", B_REPLY) + 8;
    text(x + 10, 14, view == V_LIST ? "FORO de BASIC-OS" : "", C_YELLOW);
    if (view == V_THREAD) text_clip(x + 10, 14, cur_title, (WW - x - 20) / cw, C_YELLOW);

    if (view == V_LIST) {
        int vis = (WH - TOPH - BOTH - 12) / rowh;
        if (sel < list_scroll) list_scroll = sel;
        if (sel >= list_scroll + vis) list_scroll = sel - vis + 1;
        if (list_scroll < 0) list_scroll = 0;
        y = TOPH + 8;
        if (!nth && !SDL_AtomicGet(&job_busy))
            text(20, y + 10, "Todavia no hay hilos (o no se pudo cargar). N = abrir el primero.", C_GREY);
        for (i = list_scroll; i < nth && i < list_scroll + vis; i++, y += rowh) {
            char info[160];
            int hov = mx >= 0 && my >= y && my < y + rowh && my > TOPH && my < WH - BOTH, infow;
            if (i == sel) fill(8, y, WW - 16, rowh - 2, C_PANEL);
            else if (hov) fill(8, y, WW - 16, rowh - 2, C_DARK);
            snprintf(info, sizeof info, "%d %s · %s · %s", ths[i].n, ths[i].n == 1 ? "mensaje" : "mensajes", ths[i].date, ths[i].nick);
            infow = u8len(info);
            text(WW - 20 - infow * cw, y + 7, info, C_GREY);
            text_clip(20, y + 7, ths[i].title, (WW - 60) / cw - infow - 2 > 8 ? (WW - 60) / cw - infow - 2 : 8, C_WHITE);
        }
    } else {
        int vis = (WH - TOPH - BOTH - 16) / chh;
        if (thr_scroll > nlines - vis) thr_scroll = nlines - vis;
        if (thr_scroll < 0) thr_scroll = 0;
        y = TOPH + 8;
        for (i = thr_scroll; i < nlines && i < thr_scroll + vis; i++, y += chh) {
            if (lines[i].kind == 2) {
                fill(16, y, WW - 32, chh, C_DARK);
                text(24, y, lines[i].t, C_GREEN);
            } else
                text(24, y, lines[i].t, C_WHITE);
        }
        if (nlines > vis) {                            /* barra de desplazamiento */
            int bh = WH - TOPH - BOTH - 16, th = bh * vis / nlines, ty;
            if (th < 20) th = 20;
            ty = TOPH + 8 + (bh - th) * thr_scroll / (nlines - vis);
            fill(WW - 10, TOPH + 8, 6, bh, C_DARK);
            fill(WW - 10, ty, 6, th, C_GREY);
        }
    }
    /* barra de abajo */
    fill(0, WH - BOTH, WW, BOTH, C_DARK);
    {
        char right[300];
        snprintf(right, sizeof right, "%s%s%s", hub, nick[0] ? " · " : "", nick);
        text(WW - 12 - u8len(right) * cw, WH - BOTH + 5, right, C_GREY);
        text_clip(12, WH - BOTH + 5, status, (WW - 40) / cw - u8len(right) - 2, status_err ? C_RED : C_WHITE);
    }
    if (comp_open)
        draw_composer();
    SDL_RenderPresent(R);
}

/* ================= acciones ================= */
static void load_list(void) { start_job(J_LIST, "/foro/list?u=1", 0, 0); }

static void open_thread(const char *id)
{
    char path[64];
    snprintf(path, sizeof path, "/foro/hilo/%s?u=1", id);
    start_job(J_THREAD, path, 0, id);
}

static int clean_nick(char *n)                        /* lo mismo que hara el hub: letras, numeros, _ . - */
{
    int i, k = 0;
    for (i = 0; n[i] && k < 16; i++) {
        char c = n[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-') n[k++] = c;
        else if (c == ' ') n[k++] = '_';
    }
    n[k] = 0;
    return k;
}

static void send_post(void)
{
    char path[] = "/tmp/forosdl-XXXXXX";
    int fd, tl = u8len(c_title), ml = u8len(c_msg);
    FILE *f;
    if (!clean_nick(c_nick)) { set_status(1, "Pon tu nombre (letras y numeros)."); comp_focus = 0; return; }
    if (!c_key[0]) { set_status(1, "Falta la clave del hub."); comp_focus = 1; return; }
    if (comp_new && (tl < 3 || tl > 50)) { set_status(1, "El titulo debe tener de 3 a 50 letras."); comp_focus = 2; return; }
    if (!ml) { set_status(1, "El mensaje esta vacio."); comp_focus = 3; return; }
    if (ml > MSG_MAX) { set_status(1, "El mensaje pasa de %d letras.", MSG_MAX); comp_focus = 3; return; }
    fd = mkstemp(path);
    if (fd < 0 || !(f = fdopen(fd, "wb"))) { set_status(1, "No pude crear el archivo temporal."); return; }
    fprintf(f, "BOSFORO 1\n%s\n%s\n%s\n%s\n%s", c_key, c_nick, comp_new ? "NEW" : cur_id, comp_new ? c_title : "", c_msg);
    fclose(f);
    scopy(nick, c_nick, sizeof nick);
    if (!start_job(J_POST, "/foro/post", path, 0))
        unlink(path);
}

static void job_finished(void)
{
    int code = job_code;
    char *out = job_out ? job_out : (char *)"";
    if (job_kind == J_POST) unlink(job_file);
    if (code == 0) {
        set_status(1, "Sin respuesta del hub (¿hay internet? ¿la direccion esta bien? ¿esta curl instalado?)");
        return;
    }
    switch (job_kind) {
    case J_LIST:
        if (code != 200 || strncmp(out, "#FORO", 5)) { set_status(1, "El hub respondio %d: %.120s", code, out); return; }
        parse_list(out);
        set_status(0, "%d %s", nth, nth == 1 ? "hilo" : "hilos");
        break;
    case J_THREAD:
        if (code != 200) { set_status(1, "No pude abrir el hilo (%d): %.100s", code, out); return; }
        free(thr_text);
        thr_text = strdup(out);
        if (strcmp(cur_id, job_arg)) thr_scroll = 0; else thr_scroll = 1 << 30;   /* el mismo hilo (tras responder): al final */
        scopy(cur_id, job_arg, sizeof cur_id);
        cur_title[0] = 0;
        build_lines();
        view = V_THREAD;
        set_status(0, "Hilo %s", cur_id);
        break;
    case J_POST:
        if (code == 200 && !strncmp(out, "OK ", 3)) {
            char id[8];
            scopy(id, out + 3, sizeof id);
            trim(id);
            scopy(key, c_key, sizeof key);
            save_cfg();
            close_composer();
            set_status(0, "Publicado en el hilo %s", id);
            if (comp_new) cur_id[0] = 0;
            open_thread(id);
            if (!comp_new) scopy(cur_id, id, sizeof cur_id);
        } else
            set_status(1, "El hub no lo acepto (%d): %.150s", code, out);
        break;
    }
}

/* ================= eventos ================= */
static void comp_text(const char *t)
{
    char *dst; size_t max; int lim;
    switch (comp_focus) {
    case 0: dst = c_nick; max = sizeof c_nick; lim = 16; break;
    case 1: dst = c_key; max = sizeof c_key; lim = 120; break;
    case 2: dst = c_title; max = sizeof c_title; lim = 50; break;
    default: dst = c_msg; max = sizeof c_msg; lim = MSG_MAX + 200; break;
    }
    for (; *t; ) {                                     /* letra por letra (UTF-8) */
        const char *n = u8skip(t, 1);
        size_t l = strlen(dst), cl = (size_t)(n - t);
        if (comp_focus != 3 && (*t == '\n' || *t == '\r')) { t = n; continue; }
        if (*t == '\r') { t = n; continue; }
        if (u8len(dst) >= lim || l + cl + 1 >= max) break;
        memcpy(dst + l, t, cl); dst[l + cl] = 0;
        t = n;
    }
}

static void comp_next(int dir)
{
    int k;
    for (k = 0; k < 4; k++) {
        comp_focus = (comp_focus + dir + 4) % 4;
        if (comp_field_ok(comp_focus)) return;
    }
}

static void click(int x, int y)
{
    int i;
    for (i = 0; i < nbtn; i++) {
        SDL_Rect *r = &btns[i].r;
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            switch (btns[i].id) {
            case B_BACK: view = V_LIST; load_list(); break;
            case B_RELOAD: if (view == V_LIST) load_list(); else open_thread(cur_id); break;
            case B_NEW: open_composer(1); break;
            case B_REPLY: open_composer(0); break;
            case B_SEND: send_post(); break;
            case B_CANCEL: close_composer(); break;
            }
            return;
        }
    }
    if (comp_open) {
        for (i = 0; i < 4; i++)
            if (f_rect[i].w && x >= f_rect[i].x && x < f_rect[i].x + f_rect[i].w && y >= f_rect[i].y && y < f_rect[i].y + f_rect[i].h)
                comp_focus = i;
        return;
    }
    if (view == V_LIST && y > TOPH + 8 && y < WH - BOTH) {
        int rowh = chh + 14, i2 = list_scroll + (y - TOPH - 8) / rowh;
        if (i2 >= 0 && i2 < nth) { sel = i2; open_thread(ths[i2].id); }
    }
}

static void key_down(SDL_Keysym k)
{
    int ctrl = (k.mod & KMOD_CTRL) != 0, page = (WH - TOPH - BOTH) / chh - 2;
    if (comp_open) {
        if (k.sym == SDLK_ESCAPE) close_composer();
        else if (k.sym == SDLK_TAB) comp_next((k.mod & KMOD_SHIFT) ? -1 : 1);
        else if ((k.sym == SDLK_RETURN || k.sym == SDLK_KP_ENTER) && ctrl) send_post();
        else if (k.sym == SDLK_RETURN || k.sym == SDLK_KP_ENTER) { if (comp_focus == 3) comp_text("\n"); else comp_next(1); }
        else if (k.sym == SDLK_BACKSPACE) u8pop(comp_focus == 0 ? c_nick : comp_focus == 1 ? c_key : comp_focus == 2 ? c_title : c_msg);
        else if (k.sym == SDLK_v && ctrl) { char *t = SDL_GetClipboardText(); if (t) { comp_text(t); SDL_free(t); } }
        return;
    }
    if (view == V_LIST) {
        switch (k.sym) {
        case SDLK_ESCAPE: { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); } break;
        case SDLK_UP: if (sel > 0) sel--; break;
        case SDLK_DOWN: if (sel < nth - 1) sel++; break;
        case SDLK_PAGEUP: sel -= 10; if (sel < 0) sel = 0; break;
        case SDLK_PAGEDOWN: sel += 10; if (sel >= nth) sel = nth - 1; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: if (nth) open_thread(ths[sel].id); break;
        case SDLK_n: open_composer(1); break;
        case SDLK_r: case SDLK_F5: load_list(); break;
        }
    } else {
        switch (k.sym) {
        case SDLK_ESCAPE: case SDLK_BACKSPACE: view = V_LIST; load_list(); break;
        case SDLK_UP: thr_scroll -= 1; break;
        case SDLK_DOWN: thr_scroll += 1; break;
        case SDLK_PAGEUP: thr_scroll -= page; break;
        case SDLK_PAGEDOWN: case SDLK_SPACE: thr_scroll += page; break;
        case SDLK_HOME: thr_scroll = 0; break;
        case SDLK_END: thr_scroll = 1 << 30; break;
        case SDLK_r: open_composer(0); break;
        case SDLK_F5: open_thread(cur_id); break;
        }
    }
}

int main(int argc, char **argv)
{
    const char *fontpath = getenv("FOROSDL_FONT");
    static const char *const fonts[] = {
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", "/usr/share/fonts/noto/NotoSansMono-Regular.ttf", 0
    };
    int i, running = 1;
    load_cfg();
    if (argc > 1) scopy(hub, argv[1], sizeof hub);
    if (argc > 2) scopy(key, argv[2], sizeof key);
    { int n = (int)strlen(hub); while (n && hub[n - 1] == '/') hub[--n] = 0; }
    if (SDL_Init(SDL_INIT_VIDEO) < 0 || TTF_Init() < 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    if (fontpath) font = TTF_OpenFont(fontpath, 16);
    for (i = 0; !font && fonts[i]; i++) font = TTF_OpenFont(fonts[i], 16);
    if (!font) {
        fprintf(stderr, "No encuentro una fuente monoespaciada. Instala ttf-dejavu o usa FOROSDL_FONT=/ruta/fuente.ttf\n");
        return 1;
    }
    TTF_SizeUTF8(font, "M", &cw, &chh);
    Win = SDL_CreateWindow("FORO de BASIC-OS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WW, WH,
                           SDL_WINDOW_RESIZABLE);
    R = SDL_CreateRenderer(Win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) R = SDL_CreateRenderer(Win, -1, SDL_RENDERER_SOFTWARE);
    if (!Win || !R) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(Win, 560, 360);
    SDL_StopTextInput();
    load_list();
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) {
            do {
                switch (e.type) {
                case SDL_QUIT: running = 0; break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        int keep = thr_scroll >= nlines - (WH - TOPH - BOTH - 16) / chh;
                        SDL_GetWindowSize(Win, &WW, &WH);
                        build_lines();
                        if (keep) thr_scroll = 1 << 30;
                    }
                    break;
                case SDL_MOUSEMOTION: mx = e.motion.x; my = e.motion.y; break;
                case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) click(e.button.x, e.button.y); break;
                case SDL_MOUSEWHEEL:
                    if (comp_open) break;
                    if (view == V_THREAD) thr_scroll -= e.wheel.y * 3;
                    else { list_scroll -= e.wheel.y * 2; if (list_scroll < 0) list_scroll = 0; if (sel < list_scroll) sel = list_scroll; }
                    break;
                case SDL_KEYDOWN: key_down(e.key.keysym); break;
                case SDL_TEXTINPUT: if (comp_open) comp_text(e.text.text); break;
                }
            } while (SDL_PollEvent(&e));
        }
        if (SDL_AtomicGet(&job_done) && SDL_AtomicGet(&job_busy)) {
            SDL_AtomicSet(&job_busy, 0);
            job_finished();
        }
        draw();
    }
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
