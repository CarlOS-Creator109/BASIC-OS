/* foro.c - FORO.APP: foro de texto de BASIC-OS. Lee y escribe en el hub (hub/server.py, rutas /foro/...) por HTTPS;
 * el hub guarda cada hilo como un archivo del repo de GitHub, asi que GitHub es el "servidor" del foro.
 *
 * Flujo:  FORO.APP -> NET.APP (iPXE: dhcp + imgfetch https://HUB/foro/list + bosstash) -> memoria fija -> lista de hilos
 *         -> elegir uno -> NET.APP otra vez (imgfetch https://HUB/foro/hilo/ID) -> se muestra con pausas
 *         -> R = responder: NET.APP con bospost (POST https://HUB/foro/post con "BOSFORO 1\nCLAVE\nAUTOR\nHILO\nTITULO\n" + mensaje).
 * Direccion del hub y clave: HUB.CFG en la particion FAT (1a linea = direccion, 2a = clave), igual que PKG. Sin clave no se escribe
 * (se pide al escribir). No hay direccion por defecto: FORO solo habla con el hub que tu pongas.
 * Uso:  FORO   (o EXEC "FORO.APP")     numero = leer ese hilo, N = hilo nuevo, R = recargar, U = direccion del hub,
 *       flechas (o + / -) = pagina siguiente / anterior, Q o ESC = salir.   Dentro de un hilo: Enter = seguir, R = responder, Q = volver.
 * Limites (los pone el hub): mensajes de hasta 1500 caracteres, hilos de hasta 60 mensajes, un mensaje cada 15 s por conexion.
 * El hub quita acentos y signos raros porque la consola solo pinta ASCII; la consola entrega MAYUSCULAS (sin Shift = minusculas aqui).
 */
#include "kapi.h"
#include "mod_head.h"
#include "sys/binwin.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define APP_BASE   0x9000000u           /* aqui se arma el cuerpo del POST (igual que PKG) */
#define LIST_ADDR  0xB000000u           /* aqui deja NET.BIN lo que baja (bosstash) */
#define LIST_MAX   0x100000u
#define MBOX_ADDR  0xB100000u
#define NEED_RAM   0xB200000u
#define MBOX_MAGIC 0x48545342u          /* 'BSTH' */

struct mbox { u32 magic, app_len, list_len, spare; };

struct bootinfo { u32 magic; u16 w, h, pitch; u8 bpp, gfx; u32 fb; u8 rs, rp, gs, gp, bs, bp; u16 e820n; u16 extkb; } __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static const struct kapi *K;

static void pr(const char *s) { K->print(s); }

static void prnum(unsigned v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    pr(b + i);
}

static u64 ram_end(void)
{
    u64 end = 0;
    int i;
    if (BI->e820n) {
        for (i = 0; i < BI->e820n; i++)
            if (E8[i].type == 1 && E8[i].base <= 0x4000000ull && E8[i].base + E8[i].len > 0x4000000ull)
                end = E8[i].base + E8[i].len;
    } else {
        end = ((u64)BI->extkb + 1024) * 1024;
    }
    return end;
}

/* ---- direccion del hub y clave (HUB.CFG) ---- */
static char hub[96];
static char hubkey[64];

static void load_cfg(void)
{
    static const char *const where[] = {
        "HD0.1:HUB.CFG", "HD1.1:HUB.CFG", "HD2.1:HUB.CFG", "HD0:HUB.CFG", "FD1:HUB.CFG", "USB0.1:HUB.CFG", 0
    };
    char buf[200];
    unsigned total = 0;
    int i, n;
    for (i = 0; where[i]; i++) {
        total = 0;
        n = K->read(where[i], buf, (int)sizeof buf - 1, &total);
        if (n > 8) {
            int j = 0, k = 0, q = 0;
            while (j < n && (buf[j] == ' ' || buf[j] == '\r' || buf[j] == '\n'))
                j++;
            while (j < n && buf[j] > ' ' && k < (int)sizeof hub - 1)
                hub[k++] = buf[j++];
            while (k > 0 && hub[k - 1] == '/')
                k--;
            hub[k] = 0;
            while (j < n && buf[j] != '\n')
                j++;
            while (j < n && (buf[j] == ' ' || buf[j] == '\r' || buf[j] == '\n'))
                j++;
            while (j < n && buf[j] > ' ' && q < (int)sizeof hubkey - 1)
                hubkey[q++] = buf[j++];
            hubkey[q] = 0;
            if (k > 8)
                return;
        }
    }
    hub[0] = 0;
}

/* leer una linea corta con eco; devuelve la longitud, -1 si ESC, -2 si fue una flecha (solo con arrows y la linea vacia).
 * invert: la consola entrega MAYUSCULAS; con invert = 1 sin Shift sale minuscula y con Shift mayuscula (como en NET). */
static int arrows, arrow_dir;
static int readline(char *buf, int max, int invert)
{
    int len = 0;
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); continue; }
        if (arrows && len == 0 && (c == KEY_RIGHT || c == KEY_DOWN || c == KEY_LEFT || c == KEY_UP)) {
            arrow_dir = (c == KEY_RIGHT || c == KEY_DOWN) ? 1 : -1;
            buf[0] = 0;
            return -2;
        }
        if (invert) {
            if (c >= 'a' && c <= 'z') c -= 32;
            else if (c >= 'A' && c <= 'Z') c += 32;
        }
        if (c == 27) return -1;
        if (c == '\r' || c == '\n') { pr("\n"); buf[len] = 0; return len; }
        if ((c == 8 || c == 127 || c == 20) && len) { len--; pr("\b \b"); continue; }
        if (c >= 32 && c < 127 && len < max - 1) {
            char s[2];
            buf[len++] = (char)c;
            s[0] = (char)c; s[1] = 0;
            pr(s);
        }
    }
}

static void ask_hub(void)
{
    char buf[96];
    int n, k = 0;
    pr("\nDireccion del hub (por ejemplo https://mi-hub.onrender.com; Enter = dejar la actual): ");
    n = readline(buf, sizeof buf, 1);
    if (n <= 0)
        return;
    while (k < n && k < (int)sizeof hub - 1) { hub[k] = buf[k]; k++; }
    while (k > 0 && hub[k - 1] == '/')
        k--;
    hub[k] = 0;
}

/* ---- NET.APP con una linea de iPXE ---- */
static void clear_mbox(void)
{
    struct mbox *m = (struct mbox *)(unsigned long)MBOX_ADDR;
    m->magic = 0; m->app_len = 0; m->list_len = 0; m->spare = 0;
}

static int run_net(const char *s)
{
    char *a = (char *)K->args;                  /* el buffer net_args del kernel (160 bytes) */
    int i;
    for (i = 0; s[i] && i < 159; i++)
        a[i] = s[i];
    a[i] = 0;
    return K->exec("NET.APP");
}

/* bajar por BNM (BN primero, iPXE de reserva); deja el resultado en el buzon 'BSTH' como bosstash */
static int run_bnm(const char *label, const char *a, const char *b)
{
    char *args = (char *)K->args;
    const char *parts[5];
    int i = 0, q, j;
    parts[0] = "@bnm fetch "; parts[1] = label; parts[2] = " "; parts[3] = a; parts[4] = b;
    for (q = 0; q < 5; q++)
        for (j = 0; parts[q] && parts[q][j] && i < 159; j++)
            args[i++] = parts[q][j];
    args[i] = 0;
    return K->exec("BN.APP");
}

static int build_cmd(char *dst, int max, const char *a, const char *b, const char *c, const char *d, const char *e)
{
    const char *p[5];
    int i, k = 0, j;
    p[0] = a; p[1] = b; p[2] = c; p[3] = d; p[4] = e;
    for (i = 0; i < 5; i++)
        for (j = 0; p[i] && p[i][j]; j++) {
            if (k >= max - 1)
                return 0;
            dst[k++] = p[i][j];
        }
    dst[k] = 0;
    return 1;
}

/* baja HUB + path a LIST_ADDR; devuelve los bytes (0 si fallo). El texto queda terminado en 0. */
static u32 fetch(const char *path)
{
    const struct mbox *m = (const struct mbox *)(unsigned long)MBOX_ADDR;
    clear_mbox();
    run_bnm("list", hub, path);
    if (m->magic != MBOX_MAGIC || m->list_len == 0 || m->list_len >= LIST_MAX)
        return 0;
    ((char *)(unsigned long)LIST_ADDR)[m->list_len] = 0;
    return m->list_len;
}

/* ---- la lista de hilos ("#FORO 1 n" y lineas "ID mensajes fecha autor titulo") ---- */
#define MAXTH 80
#define PER_PAGE 12
struct th { char id[8]; u16 n; char date[12]; char nick[18]; char title[54]; };
static struct th ths[MAXTH];
static int nth, page;

static void parse_list(void)
{
    const char *p = (const char *)(unsigned long)LIST_ADDR;
    nth = 0;
    while (*p && nth < MAXTH) {
        const char *e = p;
        while (*e && *e != '\n')
            e++;
        if (*p != '#' && e > p) {
            struct th *t = &ths[nth];
            const char *q = p;
            int k = 0;
            u32 v = 0;
            while (q < e && *q != ' ' && k < 7) t->id[k++] = *q++;
            t->id[k] = 0;
            while (q < e && *q == ' ') q++;
            while (q < e && *q >= '0' && *q <= '9') v = v * 10 + (u32)(*q++ - '0');
            t->n = (u16)v;
            while (q < e && *q == ' ') q++;
            for (k = 0; q < e && *q != ' ' && k < 11; ) t->date[k++] = *q++;
            t->date[k] = 0;
            while (q < e && *q == ' ') q++;
            for (k = 0; q < e && *q != ' ' && k < 17; ) t->nick[k++] = *q++;
            t->nick[k] = 0;
            while (q < e && *q == ' ') q++;
            for (k = 0; q < e && *q != '\r' && k < 53; ) t->title[k++] = *q++;
            t->title[k] = 0;
            if (t->id[0] && k)
                nth++;
        }
        p = *e ? e + 1 : e;
    }
}

static int load_list(void)
{
    pr("\nBajando la lista del foro de "); pr(hub); pr(" ...\n");
    if (!fetch("/foro/list")) {
        pr("\nFORO: no pude bajar la lista. Revisa el cable de red, la direccion del hub (HUB.CFG)\n"
           "y que el hub este despierto (en Render gratis tarda ~1 minuto la primera vez).\n");
        nth = 0;
        return 0;
    }
    parse_list();
    return 1;
}

static void show_list(void)
{
    int i, pages = (nth + PER_PAGE - 1) / PER_PAGE;
    if (page >= pages) page = pages - 1;
    if (page < 0) page = 0;
    pr("\n=== FORO de BASIC-OS: "); prnum((unsigned)nth); pr(nth == 1 ? " hilo" : " hilos");
    if (pages > 1) { pr("  pagina "); prnum((unsigned)page + 1); pr("/"); prnum((unsigned)pages); }
    pr(" ===\n");
    if (!nth)
        pr("Todavia no hay hilos. N = abrir el primero.\n");
    for (i = page * PER_PAGE; i < nth && i < (page + 1) * PER_PAGE; i++) {
        pr(i + 1 < 10 ? " " : ""); prnum((unsigned)i + 1); pr(") "); pr(ths[i].title);
        pr("  ["); prnum(ths[i].n); pr(ths[i].n == 1 ? " mensaje, " : " mensajes, "); pr(ths[i].date); pr(", "); pr(ths[i].nick); pr("]\n");
    }
    pr("\nNumero = leer   N = hilo nuevo   flechas = cambiar de pagina   R = recargar   U = hub   Q = salir\n");
}

/* ---- leer un hilo: texto del hub, con una pausa cada pantalla ---- */
#define ROWS 20

static int more(int *rows)             /* 1 = seguir, 0 = el usuario quiere salir de la lectura */
{
    if (++*rows < ROWS)
        return 1;
    *rows = 0;
    pr("-- Enter = seguir, Q = salir de la lectura --");
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); continue; }
        pr("\r                                                  \r");
        if (c == 'q' || c == 'Q' || c == 27)
            return 0;
        if (c == '\r' || c == '\n' || c == ' ')
            return 1;
    }
}

static void show_thread(const char *id, int tail)
{
    char path[40];
    const char *p;
    int rows = 0, i, k = 0;
    path[0] = 0;
    for (i = 0; "/foro/hilo/"[i]; i++) path[k++] = "/foro/hilo/"[i];
    for (i = 0; id[i] && k < 38; i++) path[k++] = id[i];
    path[k] = 0;
    pr("\nBajando el hilo ...\n");
    if (!fetch(path)) {
        pr("FORO: no pude bajar el hilo (ya no existe, sin red o el hub esta dormido).\n");
        return;
    }
    p = (const char *)(unsigned long)LIST_ADDR;
    pr("\n");
    if (tail) {                                /* solo el final: el titulo y los ultimos mensajes que quepan en una pantalla */
        const char *q = p, *start = 0;
        int total = 0, seen = 0;
        for (q = p; *q; q++) if (*q == '\n') total++;
        for (q = p; *q; q++)
            if (*q == '\n' && ++seen >= total - (ROWS - 4)) { start = q + 1; break; }
        if (start && start > p) {
            const char *e = p;
            while (*e && *e != '\n') e++;
            if (p[0] == '#' && p[1] == ' ') {
                char t[60]; int n = 0;
                while (p + 2 + n < e && n < 59) { t[n] = p[2 + n]; n++; }
                t[n] = 0;
                pr("=== "); pr(t); pr(" ===\n");
            }
            pr("(... hay mensajes anteriores: vuelve a abrir el hilo para leerlo entero ...)\n");
            while (*start && !(start[0] == '=' && start[1] == '=' && start[2] == ' ')) {   /* empezar en un encabezado */
                while (*start && *start != '\n') start++;
                if (*start) start++;
            }
            p = start;
        }
    }
    while (*p) {
        const char *e = p;
        char line[96];
        int n = 0;
        while (*e && *e != '\n')
            e++;
        while (p + n < e && n < (int)sizeof line - 1) { line[n] = p[n]; n++; }
        line[n] = 0;
        if (line[0] == '#' && line[1] == ' ') {
            pr("=== "); pr(line + 2); pr(" ===\n");
        } else if (line[0] == '=' && line[1] == '=' && line[2] == ' ') {
            pr("\n-- "); pr(line + 3); pr(" --\n");
            if (!more(&rows)) return;
        } else {
            pr(line); pr("\n");
        }
        if (!more(&rows))
            return;
        p = *e ? e + 1 : e;
    }
}

/* ---- escribir ---- */
static char nick[20];
#define MSG_MAX 1400

static int ask_nick(void)
{
    char b[20];
    int n;
    if (nick[0])
        return 1;
    pr("Tu nombre en el foro (hasta 16 letras o numeros): ");
    n = readline(b, 17, 1);
    if (n <= 0)
        return 0;
    for (n = 0; b[n]; n++)
        nick[n] = b[n] == ' ' ? '_' : b[n];
    nick[n] = 0;
    return 1;
}

/* ---- editor del mensaje (pantalla completa) ----
 * Lo que pidio Nomura: el texto pasa solo al siguiente renglon (se corta por palabras), Enter hace un
 * renglon nuevo (se pueden dejar renglones en blanco) y abajo siempre se ve cuanto falta.
 * TAB = enviar, ESC dos veces = descartar. Las letras con acento del teclado (ñ, ¿, ¡...) se mandan en UTF-8. */
#define ED_W     76                     /* renglon de la pantalla (el hub parte a 78) */
#define ED_L     20                     /* renglones */
#define ED_TOP   3                      /* fila donde empieza el texto */
static char ed[ED_L][ED_W + 1];
static int edn[ED_L], edcur;
static const char *ed_head;

static void putc_(int c) { char t[2]; t[0] = (char)c; t[1] = 0; pr(t); }
static void ed_goto(int r, int c)
{
    int i;
    putc_(19);                          /* home */
    for (i = 0; i < r; i++) putc_(17);
    for (i = 0; i < c; i++) putc_(29);
}

static int ed_count(void)               /* letras que se van a mandar (los saltos cuentan 1) */
{
    int i, n = 0;
    for (i = 0; i <= edcur; i++) n += edn[i] + (i < edcur);
    return n;
}

static void ed_status(const char *msg)
{
    char t[24];
    int n = ed_count(), i = 0, v, d;
    ed_goto(ED_TOP + ED_L + 1, 0);
    pr(n > MSG_MAX - 100 ? "\x1c" : "\x1e");             /* rojo cerca del limite, verde si no */
    t[i++] = '[';
    for (d = 1000; d; d /= 10) { v = n / d % 10; if (v || d == 1 || i > 1) t[i++] = (char)('0' + v); }
    t[i++] = '/';
    t[i++] = '1'; t[i++] = '4'; t[i++] = '0'; t[i++] = '0'; t[i++] = ']'; t[i] = 0;
    pr(t);
    pr("\x05");                                          /* blanco */
    pr(msg ? msg : (n > MSG_MAX - 100 ? "  casi lleno" : "            "));
    pr("                                        ");
    ed_goto(ED_TOP + edcur, edn[edcur]);
}

static void ed_redraw(void)
{
    int i, j;
    putc_(147);
    pr(ed_head);
    pr("\n\x9b" "Enter = renglon nuevo   TAB = enviar   ESC = descartar   (el texto pasa solo)\x05\n");
    for (i = 0; i < ED_W; i++) putc_('-');
    for (i = 0; i <= edcur; i++) {
        ed_goto(ED_TOP + i, 0);
        for (j = 0; j < edn[i]; j++) putc_((unsigned char)ed[i][j]);
    }
    ed_goto(ED_TOP + ED_L, 0);
    for (i = 0; i < ED_W; i++) putc_('-');
    ed_status(0);
}

/* la palabra que no cabe se va al renglon siguiente */
static int ed_wrap(int c)
{
    int i, sp = -1, k;
    if (edcur >= ED_L - 1)
        return 0;
    if (c != ' ')
        for (i = edn[edcur] - 1; i > 0; i--)
            if (ed[edcur][i] == ' ') { sp = i; break; }
    edcur++;
    edn[edcur] = 0;
    if (sp > 0) {
        for (k = sp + 1; k < edn[edcur - 1]; k++)
            ed[edcur][edn[edcur]++] = ed[edcur - 1][k];
        edn[edcur - 1] = sp;                             /* el espacio se vuelve el salto */
    }
    if (c != ' ')
        ed[edcur][edn[edcur]++] = (char)c;
    ed_redraw();
    return 1;
}

static int is_text_key(int c)
{
    return (c >= 32 && c < 127) || c == 0xA4 || c == 0xA5 || c == 0x87 || c == 0x80 || c == 0xAD ||
           c == 0xA8 || c == 0xA7 || c == 0xA6 || c == 0xFA || c == 0xAA || c == 0xF8;
}

/* CP437 del teclado -> UTF-8 */
static int to_utf8(int c, char *o)
{
    int u;
    switch (c) {
    case 0xA4: u = 0xF1; break; case 0xA5: u = 0xD1; break; case 0x87: u = 0xE7; break; case 0x80: u = 0xC7; break;
    case 0xAD: u = 0xA1; break; case 0xA8: u = 0xBF; break; case 0xA7: u = 0xBA; break; case 0xA6: u = 0xAA; break;
    case 0xFA: u = 0xB7; break; case 0xAA: u = 0xAC; break; case 0xF8: u = 0xB0; break;
    default: o[0] = (char)c; return 1;
    }
    o[0] = (char)(0xC0 | (u >> 6));
    o[1] = (char)(0x80 | (u & 63));
    return 2;
}

/* pide el mensaje. Devuelve la longitud en bytes (UTF-8), 0 si no hay mensaje o se cancelo. */
static int ask_message(char *msg, const char *head)
{
    int len = 0, i, j, esc = 0, last;
    edcur = 0; edn[0] = 0; ed_head = head;
    K->esc();
    ed_redraw();
    for (;;) {
        int c = K->poll();
        if (K->esc() || c == 27) {
            if (esc) { putc_(147); return 0; }
            esc = 1;
            ed_status("  ESC otra vez = descartar el mensaje");
            continue;
        }
        if (!c) { K->sleep(5); continue; }
        if (esc) { esc = 0; ed_status(0); }
        if (c >= 'a' && c <= 'z') c -= 32;               /* la consola da MAYUSCULAS sin Shift: aqui al reves */
        else if (c >= 'A' && c <= 'Z') c += 32;
        else if (c == 0xA4) c = 0xA5; else if (c == 0xA5) c = 0xA4;
        else if (c == 0x87) c = 0x80; else if (c == 0x80) c = 0x87;
        if (c == '\t') {
            if (ed_count() > 0) break;
            ed_status("  todavia no escribiste nada");
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (edcur >= ED_L - 1 || ed_count() >= MSG_MAX) { ed_status("  ya no caben mas renglones"); continue; }
            edcur++;
            edn[edcur] = 0;
            ed_status(0);
            continue;
        }
        if (c == 8 || c == 127 || c == 20) {
            if (edn[edcur]) {
                edn[edcur]--;
                pr("\b \b");
            } else if (edcur) {
                edcur--;                                 /* se junta con el renglon de arriba */
            }
            ed_status(0);
            continue;
        }
        if (!is_text_key(c))
            continue;
        if (ed_count() >= MSG_MAX) { ed_status("  lleno: maximo 1400 letras"); continue; }
        if (edn[edcur] >= ED_W) {
            if (!ed_wrap(c)) ed_status("  ya no caben mas renglones");
            continue;
        }
        ed[edcur][edn[edcur]++] = (char)c;
        putc_(c);
        ed_status(0);
    }
    /* se quitan los renglones vacios del final */
    last = edcur;
    while (last > 0 && edn[last] == 0) last--;
    for (i = 0; i <= last; i++) {
        for (j = 0; j < edn[i]; j++) len += to_utf8((unsigned char)ed[i][j], msg + len);
        if (i < last) msg[len++] = '\n';
    }
    msg[len] = 0;
    putc_(147);
    return len;
}

/* hid = "NEW" para un hilo nuevo (con title) o el numero de un hilo. Devuelve 1 si el hub dijo OK. */
static int send_post(const char *hid, const char *title, const char *msg, int mlen, char *newid)
{
    char key[64], cmd[160];
    struct mbox *m = (struct mbox *)(unsigned long)MBOX_ADDR;
    u8 *out = (u8 *)(unsigned long)APP_BASE;
    int hl = 0, i;
    {   const char *b = binwin_busy(ram_end());
        if (b) {
            pr("FORO: cierra primero la ventana de "); pr(b); pr(" (usa la memoria donde se arma el envio).\n");
            return 0;
        } }
    if (hubkey[0]) {
        for (i = 0; hubkey[i]; i++) key[i] = hubkey[i];
        key[i] = 0;
    } else {
        pr("Clave del hub (la 2a linea de HUB.CFG; sin ella no se puede escribir): ");
        if (readline(key, sizeof key, 1) <= 0)
            return 0;
    }
    {   const char *parts[5];
        int j, q;
        parts[0] = "BOSFORO 1"; parts[1] = key; parts[2] = nick; parts[3] = hid; parts[4] = title;
        for (j = 0; j < 5; j++) {
            for (q = 0; parts[j][q]; q++) out[hl++] = (u8)parts[j][q];
            out[hl++] = '\n';
        } }
    for (i = 0; i < mlen; i++)
        out[hl + i] = (u8)msg[i];
    pr("Enviando ...\n");
    m->magic = MBOX_MAGIC; m->app_len = 0; m->list_len = 0; m->spare = (u32)(hl + mlen);
    if (!build_cmd(cmd, sizeof cmd, "dhcp;bospost ", hub, "/foro/post", 0, 0)) {
        pr("FORO: la direccion del hub es demasiado larga\n");
        return 0;
    }
    run_net(cmd);
    if (m->magic != MBOX_MAGIC || m->list_len == 0) {
        pr("\nFORO: no se pudo enviar (clave equivocada, mensajes muy seguidos, hub dormido o sin red).\n");
        return 0;
    }
    {   const char *r = (const char *)(unsigned long)LIST_ADDR;
        char line[100];
        u32 j;
        for (j = 0; j < m->list_len && j < sizeof line - 1 && r[j] != '\n' && r[j] != '\r'; j++) line[j] = r[j];
        line[j] = 0;
        if (line[0] == 'O' && line[1] == 'K') {
            int q = 0;
            for (j = 3; line[j] && q < 7; j++) newid[q++] = line[j];
            newid[q] = 0;
            pr("Listo, publicado.\n");
            return 1;
        }
        pr("Hub: "); pr(line); pr("\n");
    }
    return 0;
}

static int reply(const char *id)
{
    static char msg[MSG_MAX * 2 + 8];
    char nid[8];
    int n;
    if (!ask_nick())
        return 0;
    {   static char head[60];
        int k = 0;
        const char *p = "Respuesta al hilo ", *q = id;
        while (*p) head[k++] = *p++;
        while (*q && k < 40) head[k++] = *q++;
        head[k] = 0;
        n = ask_message(msg, head); }
    if (n <= 0) { pr("(cancelado)\n"); return 0; }
    return send_post(id, "", msg, n, nid);
}

static int new_thread(void)
{
    static char msg[MSG_MAX * 2 + 8];
    char title[52], nid[8];
    int n;
    if (!ask_nick())
        return 0;
    pr("Titulo del hilo (3 a 50 letras): ");
    n = readline(title, sizeof title, 1);
    if (n < 3) { pr("(cancelado)\n"); return 0; }
    n = ask_message(msg, "Hilo nuevo");
    if (n <= 0) { pr("(cancelado)\n"); return 0; }
    return send_post("NEW", title, msg, n, nid);
}

/* un hilo abierto: leerlo y, si quiere, responder (y volver a leerlo) */
static void thread_loop(const char *id)
{
    int shown = 0;
    for (;;) {
        char line[8];
        int n;
        show_thread(id, shown);
        shown = 0;
        pr("\nR = responder   Enter = volver a la lista > ");
        n = readline(line, sizeof line, 0);
        if (n > 0 && (line[0] == 'r' || line[0] == 'R')) {
            if (reply(id)) {
                shown = 1;                    /* se publico: se vuelve a bajar y se ve el final con el mensaje nuevo */
                continue;
            }                     /* se publico: se vuelve a leer para ver el mensaje */
            pr("\nEnter para seguir ");
            readline(line, sizeof line, 0);
        }
        return;
    }
}

int mod_main(const struct kapi *k)
{
    char line[40];
    int loaded = 0;
    K = k;
    pr("\nBASIC-OS FORO: foro de texto por HTTPS (el hub lo guarda en GitHub)\n");
    if (ram_end() < NEED_RAM) {
        pr("FORO necesita ~180 MB de RAM (usa memoria alta como PKG). En QEMU: -m 256\n");
        return 1;
    }
    load_cfg();
    if (!hub[0]) {
        pr("\nNo hay HUB.CFG en el disco: escribe la direccion de tu hub.\n");
        ask_hub();
    }
    if (hub[0])
        loaded = load_list();
    for (;;) {
        int n, i, v = 0;
        if (loaded)
            show_list();
        else
            pr("\nU = direccion del hub   R = reintentar   Q = salir\n");
        pr("> ");
        arrows = 1;
        n = readline(line, sizeof line, 0);
        arrows = 0;
        if (n == -2) { page += arrow_dir; if (page < 0) page = 0; pr("\n"); continue; }
        if (n < 0 || line[0] == 'q' || line[0] == 'Q' || line[0] == 0)
            break;
        if (line[1] == 0 && line[0] == '+') { page++; continue; }
        if (line[1] == 0 && line[0] == '-') { if (page > 0) page--; continue; }
        if (line[0] == 'u' || line[0] == 'U') {
            ask_hub();
            loaded = hub[0] ? load_list() : 0;
            continue;
        }
        if (line[0] == 'r' || line[0] == 'R') {
            loaded = hub[0] ? load_list() : 0;
            continue;
        }
        if (line[0] == 'n' || line[0] == 'N') {
            if (!hub[0]) { pr("Primero pon la direccion del hub (U).\n"); continue; }
            if (new_thread())
                loaded = load_list();
            continue;
        }
        for (i = 0; line[i] >= '0' && line[i] <= '9'; i++)
            v = v * 10 + (line[i] - '0');
        if (i == 0 || v < 1 || v > nth) {
            pr("Numero no valido\n");
            continue;
        }
        {   char id[8];
            for (i = 0; ths[v - 1].id[i]; i++) id[i] = ths[v - 1].id[i];
            id[i] = 0;
            thread_loop(id); }
        loaded = load_list();               /* al volver, la lista ya trae los mensajes nuevos */
    }
    return 0;
}
