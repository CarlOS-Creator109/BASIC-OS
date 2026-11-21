/* editor.c - EDITOR.APP: editor de texto de BASIC-OS, estilo nano.
 *
 * Funciona en los dos sitios:
 *   - En la TERMINAL: a pantalla completa, con los codigos de control de la consola
 *     (147 limpiar, 19 home, 17/145/29/157 mover el cursor, 18/146 video inverso).
 *   - En el ESCRITORIO: como ventana hecha con WTK (barra de menus + lienzo donde se pinta el texto).
 * Decide solo: si K->scr_w() > 0 el escritorio esta activo, asi que publica ventana.
 *
 * Teclas (como en nano):  ^O guardar   ^X salir   ^G ayuda   flechas, Inicio/Fin, Re/Av Pag
 *
 * Uso desde BASIC:   EDITOR "PROGRAMA.C"
 *
 * El texto vive en un banco de RAM que se le pide al asignador del sistema (K->ram), no en una
 * direccion fija; si el kernel es viejo y no lo tiene, cae a un buffer estatico mas chico.
 */
#include "sys/wtk.h"
#include "mod_head.h"

#define TAB_W      4
#define STATIC_MAX 60000                  /* respaldo si no hay K->ram */
#define BANK_TEXT  0x400000u              /* 4 MB de texto dentro del banco prestado */

static char  fallback[STATIC_MAX];
static char *buf;                         /* el texto */
static int   cap;                         /* cuanto cabe */
static int   len;                         /* bytes usados */
static int   cur;                         /* cursor: offset dentro de buf */
static int   top;                         /* offset del inicio de la primera linea visible */
static int   want_col = -1;               /* columna deseada al subir/bajar (-1 = la actual) */
static int   dirty;                       /* hay cambios sin guardar */
static int   quit;                        /* 1 = salir */
static unsigned long bank;                /* banco prestado (0 = usamos el estatico) */
static char  path[96];
static char  status[96];
static int   win_mode;                    /* 1 = ventana WTK, 0 = consola */

/* ---------------------------------------------------------------- utilidades */

static int slen2(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void sset(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* pega 'src' al final de 'd' sin pasarse de max */
static void scat(char *d, const char *src, int max)
{
    int i = slen2(d), j = 0;
    while (src[j] && i < max - 1) d[i++] = src[j++];
    d[i] = 0;
}

static char *num(char *o, int v)                  /* entero -> texto, devuelve el fin */
{
    char t[12];
    int n = 0;
    if (v < 0) { *o++ = '-'; v = -v; }
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}

/* ---------------------------------------------------------- lineas del texto */

static int line_start(int off)
{
    while (off > 0 && buf[off - 1] != '\n') off--;
    return off;
}

static int line_end(int off)
{
    while (off < len && buf[off] != '\n') off++;
    return off;
}

static int col_of(int off) { return off - line_start(off); }

/* numero de linea (1..n) del offset, para la barra de estado */
static int lineno_of(int off)
{
    int i, n = 1;
    for (i = 0; i < off; i++)
        if (buf[i] == '\n') n++;
    return n;
}

static int prev_line(int off)                     /* inicio de la linea anterior (o la misma si es la 1a) */
{
    int s = line_start(off);
    return s ? line_start(s - 1) : s;
}

static int next_line(int off)                     /* inicio de la linea siguiente (o la misma si es la ultima) */
{
    int e = line_end(off);
    return e < len ? e + 1 : line_start(off);
}

/* coloca el cursor en la linea que empieza en 'ls', en la columna 'c' (o al final si la linea es corta) */
static void goto_col(int ls, int c)
{
    int e = line_end(ls);
    cur = ls + c;
    if (cur > e) cur = e;
}

/* ------------------------------------------------------------- edicion */

static void ins_ch(int ch)
{
    int i;
    if (len + 1 >= cap) { sset(status, "Texto lleno", sizeof status); return; }
    for (i = len; i > cur; i--) buf[i] = buf[i - 1];
    buf[cur++] = (char)ch;
    len++;
    dirty = 1;
}

static void del_back(void)
{
    int i;
    if (!cur) return;
    for (i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1];
    cur--;
    len--;
    dirty = 1;
}

static void del_fwd(void)
{
    int i;
    if (cur >= len) return;
    for (i = cur; i < len - 1; i++) buf[i] = buf[i + 1];
    len--;
    dirty = 1;
}

static void do_save(void)
{
    int r;
    if (!path[0]) { sset(status, "Sin nombre: usa EDITOR \"ARCHIVO\"", sizeof status); return; }
    if (K->ver < 9 || !K->fsop) { sset(status, "Este kernel no puede escribir archivos", sizeof status); return; }
    r = K->fsop(FSOP_WRITE, path, buf, (unsigned long long)len, 0);
    if (r == 0) {
        dirty = 0;
        sset(status, "Guardado: ", sizeof status);
        scat(status, path, sizeof status);
    } else {
        sset(status, r == -7 ? "Disco de solo lectura" :
                     r == -8 ? "No hay espacio en el disco" :
                     r == -2 ? "No existe esa ruta" : "No se pudo guardar", sizeof status);
    }
}

static const char *HELP = "^O guardar  ^X salir  ^G ayuda  flechas mueven  Inicio/Fin  Re/Av Pag";

/* ------------------------------------------------------------- teclas */

/* La consola entrega MAYUSCULAS sin Shift; en un editor de codigo conviene al reves (igual que
 * hacen las cajas de WTK): sin Shift salen minusculas. */
static int swapcase(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

/* devuelve 0 si hay que salir del editor */
static int handle_key(int k, int rows)
{
    int ls, c, i;

    switch (k) {
    case 15:                                      /* ^O guardar */
        do_save();
        return 1;
    case 24:                                      /* ^X salir */
        if (dirty) {
            dirty = 0;                            /* una segunda pulsacion sale sin guardar */
            sset(status, "Hay cambios sin guardar: ^O guarda, ^X otra vez sale igual", sizeof status);
            return 1;
        }
        quit = 1;
        return 0;
    case 7:                                       /* ^G ayuda */
        sset(status, HELP, sizeof status);
        return 1;
    case KEY_LEFT:
        if (cur) cur--;
        want_col = -1;
        return 1;
    case KEY_RIGHT:
        if (cur < len) cur++;
        want_col = -1;
        return 1;
    case KEY_UP:
        c = want_col < 0 ? col_of(cur) : want_col;
        want_col = c;
        ls = line_start(cur);
        if (ls) goto_col(prev_line(cur), c);
        return 1;
    case KEY_DOWN:
        c = want_col < 0 ? col_of(cur) : want_col;
        want_col = c;
        if (line_end(cur) < len) goto_col(next_line(cur), c);
        return 1;
    case 8:                                       /* borrar hacia atras */
        del_back();
        want_col = -1;
        return 1;
    case 4:                                       /* ^D borrar hacia adelante */
        del_fwd();
        want_col = -1;
        return 1;
    case 1:                                       /* ^A inicio de linea */
        cur = line_start(cur);
        want_col = -1;
        return 1;
    case 5:                                       /* ^E fin de linea */
        cur = line_end(cur);
        want_col = -1;
        return 1;
    case 22:                                      /* ^V avanzar pagina */
        c = want_col < 0 ? col_of(cur) : want_col;
        for (i = 0; i < rows - 2; i++) if (line_end(cur) < len) cur = next_line(cur);
        goto_col(line_start(cur), c);
        want_col = c;
        return 1;
    case 25:                                      /* ^Y retroceder pagina */
        c = want_col < 0 ? col_of(cur) : want_col;
        for (i = 0; i < rows - 2; i++) if (line_start(cur)) cur = prev_line(cur);
        goto_col(line_start(cur), c);
        want_col = c;
        return 1;
    case '\r':
    case '\n':
        ins_ch('\n');
        want_col = -1;
        return 1;
    case '\t':
        for (i = 0; i < TAB_W; i++) ins_ch(' ');
        want_col = -1;
        return 1;
    default:
        break;
    }
    if (k >= 32 && k < 256 && k != 127) {
        ins_ch(swapcase(k));
        want_col = -1;
    }
    return 1;
}

/* mueve 'top' para que el cursor quede a la vista, en una pantalla de 'rows' filas de texto */
static void scroll_to_cursor(int rows)
{
    int ls = line_start(cur), t = top, i;
    if (t > len) t = 0;
    t = line_start(t);
    if (ls < t) { top = ls; return; }
    /* ¿el cursor cae dentro de las 'rows' lineas que empiezan en t? */
    for (i = 0; i < rows; i++) {
        if (t == ls) { top = line_start(top); return; }
        if (line_end(t) >= len) break;
        t = next_line(t);
    }
    /* quedo por debajo: dejamos la linea del cursor como la ultima visible */
    t = ls;
    for (i = 0; i < rows - 1; i++) {
        if (!line_start(t)) break;
        t = prev_line(t);
    }
    top = t;
}

/* arma el texto de la barra de estado: nombre, marca de cambios y linea:columna */
static void status_bar(char *o, int max)
{
    char t[96];
    char *p = t;
    int i = 0;
    while (path[i] && i < 40) { *p++ = path[i]; i++; }
    if (!i) { const char *s = "(sin nombre)"; while (*s) *p++ = *s++; }
    if (dirty) { const char *s = " *"; while (*s) *p++ = *s++; }
    { const char *s = "  L"; while (*s) *p++ = *s++; }
    p = num(p, lineno_of(cur));
    *p++ = ':';
    p = num(p, col_of(cur) + 1);
    *p = 0;
    sset(o, t, max);
}

/* ============================================================ MODO CONSOLA */

static void pc(int c) { char s[2]; s[0] = (char)c; s[1] = 0; K->print(s); }

static int con_cols(void)
{
    int n;
    if (K->ver < 6 || !K->col) return 80;
    pc(19);                                        /* home */
    for (n = 0; n < 250; n++) {
        pc(' ');
        if (K->col() == 0) return n + 1;           /* dio la vuelta: esas son las columnas */
    }
    return 80;
}

static void con_draw(int cols, int rows)
{
    int r, t = top, crow = 0, ccol = 0, ls = line_start(cur);
    char bar[96];

    pc(147);                                       /* limpiar */
    pc(19);                                        /* home */
    for (r = 0; r < rows; r++) {
        int e = line_end(t), i;
        if (t == ls) { crow = r; ccol = cur - t; }
        for (i = t; i < e && i - t < cols; i++)
            pc(buf[i] ? buf[i] : ' ');
        if (r < rows - 1) pc('\n');
        if (e >= len) { t = len; break; }
        t = e + 1;
    }
    /* barra de estado abajo, en video inverso */
    pc(19);
    for (r = 0; r < rows; r++) pc(17);             /* bajar hasta la ultima fila */
    status_bar(bar, sizeof bar);
    pc(18);                                        /* inverso */
    {   int i, n = slen2(bar);
        for (i = 0; i < cols - 1; i++) pc(i < n ? bar[i] : ' ');
    }
    pc(146);                                       /* fin del inverso */
    /* segunda linea: el mensaje o la ayuda */
    pc(19);
    for (r = 0; r < rows + 1; r++) pc(17);
    {   const char *m = status[0] ? status : HELP;
        int i, n = slen2(m);
        for (i = 0; i < cols - 1; i++) pc(i < n ? m[i] : ' ');
    }
    /* cursor donde toca */
    pc(19);
    for (r = 0; r < crow; r++) pc(17);
    for (r = 0; r < ccol && r < cols - 1; r++) pc(29);
}

static void console_editor(void)
{
    int cols = con_cols(), rows;
    rows = 23;                                     /* 25 filas: 23 de texto + barra + mensaje */
    pc(147);
    if (K->cursor) K->cursor(1);
    sset(status, HELP, sizeof status);
    while (!quit) {
        int k;
        scroll_to_cursor(rows);
        con_draw(cols, rows);
        do {
            k = K->poll();
            if (!k) K->sleep(5);
        } while (!k);
        status[0] = 0;
        handle_key(k, rows);
    }
    pc(147);
    pc(19);
}

/* ============================================================ MODO VENTANA (WTK) */

static int id_canvas;
static const char *const m_archivo[] = { "Guardar", "Guardar y salir", "Salir", "Ayuda" };

static void menu_archivo(int item)
{
    if (item == 0) do_save();
    else if (item == 1) { do_save(); if (!dirty) wt_close(); }
    else if (item == 2) wt_close();
    else wt_msg(HELP);
}

static void draw_canvas(struct surf *s, int w, int h)
{
    int cols = w / 8, rows = (h / 16) - 1, r, t, ls = line_start(cur);
    char bar[96];

    if (rows < 1) rows = 1;
    scroll_to_cursor(rows);
    t = top;
    s_rect(s, 0, 0, w, h, C_WHITE);
    for (r = 0; r < rows; r++) {
        int e = line_end(t), i, x = 0;
        for (i = t; i < e && i - t < cols; i++, x += 8)
            s_glyph(s, x, r * 16, (u8)buf[i], C_BLACK, C_WHITE);
        if (t == ls) {                                      /* el cursor: glifo en inverso */
            int cc = cur - t;
            if (cc <= cols - 1)
                s_glyph(s, cc * 8, r * 16, cur < len && buf[cur] != '\n' ? (u8)buf[cur] : ' ',
                        C_WHITE, C_BLACK);
        }
        if (e >= len) break;
        t = e + 1;
    }
    /* barra de estado abajo */
    s_rect(s, 0, h - 16, w, 16, C_BLUE);
    status_bar(bar, sizeof bar);
    s_text(s, 2, h - 16, bar, C_WHITE, -1);
}

static int key_hook(int k)
{
    handle_key(k, 20);
    if (quit) { wt_close(); return 1; }
    if (status[0]) { wt_msg(status); status[0] = 0; }
    wt_redraw();
    return 1;                                      /* el editor se come todas las teclas */
}

static void armar(void)
{
    wt_menu("Archivo", m_archivo, 4, menu_archivo);
    id_canvas = wt_canvas(0, 0, 400, 300, draw_canvas, 0);
    wt_anchor(id_canvas, WT_FILLW | WT_FILLH);
    wt_on_key(key_hook);
    wt_msg(HELP);
}

/* ============================================================ arranque */

static void load_file(void)
{
    unsigned total = 0;
    int n;
    const char *a = K->args;

    path[0] = 0;
    if (a) {
        int i = 0, j = 0;
        while (a[i] == ' ' || a[i] == '"') i++;             /* quita espacios y comillas */
        while (a[i] && a[i] != '"' && j < (int)sizeof path - 1) path[j++] = a[i++];
        while (j > 0 && path[j - 1] == ' ') j--;            /* y el espacio del final */
        path[j] = 0;
    }
    if (!path[0]) { sset(status, "Archivo nuevo (sin nombre)", sizeof status); return; }
    n = K->read(path, buf, cap - 1, &total);
    if (n < 0) {
        len = 0;
        sset(status, "Archivo nuevo: ", sizeof status);
        scat(status, path, sizeof status);
        return;
    }
    len = n;
    if (total > (unsigned)n) sset(status, "OJO: el archivo no cabe entero", sizeof status);
}

int mod_main(const struct kapi *k)
{
    int r;
    K = k;
    /* memoria del texto: un banco del asignador del sistema; si no hay, el estatico */
    bank = 0;
    if (K->ver >= 7 && K->ram) {
        bank = (unsigned long)K->ram(RAM_ALLOC, 0, 1, "EDITOR");
        if (bank) { buf = (char *)bank; cap = BANK_TEXT; }
    }
    if (!bank) { buf = fallback; cap = STATIC_MAX; }
    len = cur = top = 0;
    dirty = quit = 0;
    want_col = -1;
    status[0] = 0;
    load_file();

    if (K->scr_w && K->scr_w() > 0) {              /* el escritorio esta abierto: ventana */
        win_mode = 1;
        return wt_window("EDITOR", 560, 380, 1, armar);
    }
    win_mode = 0;
    console_editor();
    r = MOD_OK;
    if (bank && K->ram) K->ram(RAM_FREE, bank, 0, 0);
    return r;
}
