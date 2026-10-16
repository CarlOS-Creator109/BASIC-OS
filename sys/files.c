/* files.c - explorador de archivos en ventana (modulo /SYS/FILES.APP)
 *
 * Lista las unidades (DRIVES) y la carpeta SYS:, navega por FAT e ISO9660. Un clic abre:
 * .BAS se carga en BASIC y se ejecuta en una terminal, .ICB se ve como imagen, .APP de SYS: se abre
 * como ventana y el resto como texto.
 */
#include "ui.h"
#include "mod_head.h"

#define MAXE 200
#define NAMEL 24
#define ROW_H 20
#define TOP_Y 22

struct ent { char name[NAMEL]; unsigned size; u8 kind; };    /* kind: 0 archivo, 1 carpeta, 2 unidad */

enum { V_LIST, V_TEXT, V_ICB };

static struct ent ents[MAXE];
static int nents, top, status_err;
static char path[100];                      /* "" = raiz */
static struct icon ic_dir, ic_file, ic_bas, ic_drive, ic_cd, ic_view;
static int W, H, rows, view = V_LIST;
static char fbuf[40961];
static char vtitle[NAMEL];
static int lines[1500], nl, vt, vcols, vrows, vlen;
static const char *msg;
static struct wctx *cx;

static void add(const char *name, unsigned size, int kind)
{
    if (nents >= MAXE || (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))))
        return;
    scopy(ents[nents].name, name, NAMEL);
    ents[nents].size = size;
    ents[nents].kind = (u8)kind;
    nents++;
}

static void dircb(const char *name, unsigned size, int isdir)
{
    add(name, size, isdir ? 1 : 0);
}

static void sort_dirs_first(void)
{
    int i, j;
    static struct ent t;
    for (i = 1; i < nents; i++)
        for (j = i; j > 0 && ents[j].kind == 1 && ents[j - 1].kind == 0; j--) {
            t = ents[j]; ents[j] = ents[j - 1]; ents[j - 1] = t;
        }
}

static void list(void)
{
    nents = 0;
    top = 0;
    status_err = 0;
    msg = 0;
    if (!path[0]) {
        char ln[64];
        int i, k;
        for (i = 0; K->drive_line(i, ln); i++) {
            for (k = 0; ln[k] && ln[k] != ' '; k++)
                ;
            ln[k] = 0;
            add(ln, 0, 2);
        }
        add("SYS", 0, 2);
        return;
    }
    status_err = K->dir(path, dircb);
    sort_dirs_first();
}

static void go_up(void)
{
    int n = slen(path);
    if (!n)
        return;
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != ':')
        n--;
    if (n > 0 && path[n - 1] == ':')
        path[0] = 0;
    else if (n > 0)
        path[n - 1] = 0;
    else
        path[0] = 0;
    list();
}

static void full_path(char *out, const char *name)
{
    int n;
    scopy(out, path, 100);
    n = slen(out);
    if (n && out[n - 1] != ':' && n < 98)
        out[n++] = '/';
    scopy(out + n, name, 100 - n);
}

static const struct icon *icon_for(const struct ent *e)
{
    if (e->kind == 2)
        return (e->name[0] == 'C' && e->name[1] == 'D') ? &ic_cd : &ic_drive;
    if (e->kind == 1)
        return &ic_dir;
    return ends_with(e->name, ".BAS") ? &ic_bas : &ic_file;
}

static void layout(struct wctx *c)
{
    W = c->w;
    H = c->h;
    rows = (H - TOP_Y - 34) / ROW_H;
    if (rows < 1) rows = 1;
}

/* botones de la fila de abajo: SUBIR, ^, v (en la lista) o VOLVER (en los visores) */
static void bar_rects(int *by)
{
    *by = H - 28;
}

static void draw(struct wctx *c)
{
    struct surf S = surf_of(c);
    int i, y, by;
    char t[40];
    layout(c);
    bar_rects(&by);
    s_rect(&S, 0, 0, W, H, C_LGREY);
    s_rect(&S, 4, 2, W - 8, 18, C_BLACK);
    s_text(&S, 8, 3, view == V_LIST ? (path[0] ? path : "UNIDADES") : vtitle, C_LGREEN, C_BLACK);

    if (view == V_LIST) {
        s_rect(&S, 4, TOP_Y, W - 8, rows * ROW_H, C_WHITE);
        for (i = 0; i < rows && top + i < nents; i++) {
            const struct ent *e = &ents[top + i];
            y = TOP_Y + i * ROW_H;
            icon_draw(&S, icon_for(e), 8, y + 2, 1);
            s_text(&S, 32, y + 2, e->name, C_BLACK, C_WHITE);
            if (e->kind == 0) {
                utoa(t, e->size);
                s_text(&S, W - 16 - slen(t) * 8, y + 2, t, C_DGREY, C_WHITE);
            } else if (e->kind == 1) {
                s_text(&S, W - 16 - 72, y + 2, "<CARPETA>", C_BLUE, C_WHITE);
            }
        }
        if (status_err)
            s_text(&S, 10, TOP_Y + 4, "NO SE PUDO LEER (SIN DISCO / FORMATO)", C_RED, C_WHITE);
        else if (!nents)
            s_text(&S, 10, TOP_Y + 4, "(VACIO)", C_DGREY, C_WHITE);
        s_button(&S, 4, by, 72, 24, "SUBIR", C_LGREY, C_BLACK, 0);
        s_button(&S, 82, by, 24, 24, "^", C_LGREY, C_BLACK, 0);
        s_button(&S, 110, by, 24, 24, "v", C_LGREY, C_BLACK, 0);
        {
            char *o = t;
            o = utoa(o, (unsigned)nents);
            scopy(o, " ELEMENTOS", 12);
        }
        s_text(&S, 144, by + 4, msg ? msg : t, msg ? C_RED : C_DGREY, C_LGREY);
    } else if (view == V_TEXT) {
        int r, j;
        s_rect(&S, 4, TOP_Y, W - 8, H - TOP_Y - 34, C_BLACK);
        for (r = 0; r < vrows && vt + r < nl; r++) {
            int a = lines[vt + r], e = lines[vt + r + 1];
            for (j = 0; a + j < e && j < vcols; j++) {
                unsigned char ch = (unsigned char)fbuf[a + j];
                if (ch == '\n' || ch == '\r') continue;
                if (ch < 32) ch = '.';
                s_glyph(&S, 8 + j * 8, TOP_Y + 2 + r * 16, ch, C_LGREEN, C_BLACK);
            }
        }
        s_button(&S, 4, by, 80, 24, "VOLVER", C_LGREY, C_BLACK, 0);
    } else {
        int blk = 12, avw = W - 16, avh = H - TOP_Y - 40;
        s_rect(&S, 4, TOP_Y, W - 8, H - TOP_Y - 34, C_BLACK);
        while (blk > 1 && (ic_view.w * blk > avw || ic_view.h * blk > avh))
            blk--;
        icon_draw(&S, &ic_view, (W - ic_view.w * blk) / 2, TOP_Y + (H - TOP_Y - 34 - ic_view.h * blk) / 2, blk);
        s_button(&S, 4, by, 80, 24, "VOLVER", C_LGREY, C_BLACK, 0);
    }
    c->damage(c, 0, 0, W, H);
}

static void text_layout(void)
{
    int i, col = 0;
    vcols = (W - 16) / 8;
    vrows = (H - TOP_Y - 38) / 16;
    if (vcols < 8) vcols = 8;
    if (vrows < 1) vrows = 1;
    nl = 0;
    lines[nl++] = 0;
    for (i = 0; i < vlen && nl < 1499; i++) {
        if (fbuf[i] == '\n' || ++col >= vcols) {
            col = fbuf[i] == '\n' ? 0 : 1;
            lines[nl++] = i + (fbuf[i] == '\n' ? 1 : 0);
        }
    }
    lines[nl] = vlen;
    if (vt + vrows > nl)
        vt = nl > vrows ? nl - vrows : 0;
}

static void open_entry(struct wctx *c, int i)
{
    const struct ent *e = &ents[i];
    char full[100];
    unsigned total = 0;
    int n;

    if (e->kind == 2) {
        scopy(path, e->name, 90);
        n = slen(path);
        path[n] = ':';
        path[n + 1] = 0;
        list();
        return;
    }
    if (e->kind == 1) {
        full_path(full, e->name);
        scopy(path, full, 100);
        list();
        return;
    }
    full_path(full, e->name);
    if (ends_with(e->name, ".BAS")) {
        if (K->basic_busy()) {
            msg = "BASIC OCUPADO";
            return;
        }
        n = K->read(full, fbuf, 40960, &total);
        if (n < 0 || total > 40960) {
            msg = "NO SE PUDO CARGAR";
            return;
        }
        K->load_basic(fbuf, n);
        c->run_basic("RUN");
        msg = "CARGADO Y EJECUTANDO EN LA TERMINAL";
        return;
    }
    if (ends_with(e->name, ".APP") && full[0] == 'S' && full[1] == 'Y' && full[2] == 'S') {
        c->launch(e->name);
        return;
    }
    scopy(vtitle, e->name, NAMEL);
    if (ends_with(e->name, ".ICB")) {
        n = K->read(full, fbuf, 1200, 0);
        if (n <= 0 || !icb_parse(&ic_view, fbuf, n)) {
            msg = "ICONO NO VALIDO";
            return;
        }
        view = V_ICB;
        return;
    }
    n = K->read(full, fbuf, 16384, &total);
    if (n < 0) {
        msg = "NO SE PUDO LEER";
        return;
    }
    vlen = n;
    vt = 0;
    view = V_TEXT;
    text_layout();
}

static void start(struct wctx *c)
{
    cx = c;
    icon_load(&ic_dir, "DIR.ICB");
    icon_load(&ic_file, "FILE.ICB");
    icon_load(&ic_bas, "BAS.ICB");
    icon_load(&ic_drive, "DRIVE.ICB");
    icon_load(&ic_cd, "CD.ICB");
    path[0] = 0;
    view = V_LIST;
    list();
    layout(c);
    draw(c);
}

static void resize(struct wctx *c)
{
    layout(c);
    if (view == V_TEXT)
        text_layout();
    if (top + rows > nents)
        top = nents > rows ? nents - rows : 0;
    draw(c);
}

static void key(struct wctx *c, int k)
{
    if (view == V_LIST) {
        if (k == KEY_UP || k == 'w' || k == 'W') { if (top > 0) top--; }
        else if (k == KEY_DOWN || k == 's' || k == 'S') { if (top + rows < nents) top++; }
        else if (k == 8) go_up();
        else return;
    } else if (view == V_TEXT) {
        if (k == KEY_UP || k == 'w' || k == 'W') { if (vt > 0) vt--; }
        else if (k == KEY_DOWN || k == 's' || k == 'S') { if (vt + vrows < nl) vt++; }
        else if (k == 8) view = V_LIST;
        else return;
    } else {
        if (k == 8) view = V_LIST;
        else return;
    }
    draw(c);
}

static void mouse(struct wctx *c, int x, int y, int btn, int prev)
{
    int by;
    if (!((prev & 1) && !(btn & 1)))        /* actuar al soltar el boton */
        return;
    bar_rects(&by);
    if (view != V_LIST) {
        if (inside(x, y, 4, by, 80, 24)) {
            view = V_LIST;
            draw(c);
        }
        return;
    }
    if (inside(x, y, 4, by, 72, 24)) {
        go_up();
    } else if (inside(x, y, 82, by, 24, 24)) {
        top -= rows / 2;
        if (top < 0) top = 0;
    } else if (inside(x, y, 110, by, 24, 24)) {
        top += rows / 2;
        if (top + rows > nents) top = nents > rows ? nents - rows : 0;
    } else if (inside(x, y, 4, TOP_Y, W - 8, rows * ROW_H)) {
        int i = top + (y - TOP_Y) / ROW_H;
        if (i < nents)
            open_entry(c, i);
    } else {
        return;
    }
    draw(c);
}

static const struct wapp app = {
    "ARCHIVOS", 520, 380, 260, 150, 1, start, 0, key, mouse, resize, 0
};

int mod_main(const struct kapi *api)
{
    K = api;
    K->wm_publish(&app);
    return MOD_WINAPP;
}
