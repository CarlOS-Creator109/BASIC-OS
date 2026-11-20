/* kbd.c - teclado PS/2 por sondeo (scancode set 1)
 *
 * Tres distribuciones: ES (Espana, la de siempre), LA (latinoamericana) y US. Se elige al arrancar
 * (kbd_choose en kernel.c) o con el comando KEYB, y se guarda en el disco (LBA 514).
 * Los caracteres fuera de ASCII salen con los codigos de CP437, que es lo que dibuja la
 * fuente VGA:  ñ=A4 Ñ=A5  ç=87 Ç=80  ¡=AD  ¿=A8  º=A7  ª=A6  ·=FA  ¬=AA
 *
 * Las letras salen en MAYUSCULAS por defecto; Shift o Bloq Mayus las pasan a minusculas.
 * Las teclas muertas (acentos) no estan implementadas: ` ´ ¨ ^ salen directo.
 */
#include "kernel.h"

#define K_NTILDE 0xA4   /* ñ */
#define K_CCEDIL 0x87   /* ç */

/* ===== Espana (ES) ===== */
static const u8 es_n[0x57] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '\'', [0x0D] = 0xAD, [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '`', [0x1B] = '+', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = K_NTILDE,
    [0x28] = '\'', [0x29] = 0xA7, [0x2B] = K_CCEDIL,
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x37] = '*', [0x39] = ' ', [0x56] = '<'
};
static const u8 es_s[0x57] = {
    [0x02] = '!', [0x03] = '"', [0x04] = 0xFA, [0x05] = '$', [0x06] = '%',
    [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = 0xA8, [0x0E] = '\b', [0x0F] = '\t',
    [0x1A] = '^', [0x1B] = '*', [0x1C] = '\n',
    [0x28] = '"', [0x29] = 0xA6,
    [0x33] = ';', [0x34] = ':', [0x35] = '_',
    [0x37] = '*', [0x39] = ' ', [0x56] = '>'
};
static const u8 es_g[0x57] = {
    [0x02] = '|', [0x03] = '@', [0x04] = '#', [0x05] = '~', [0x07] = 0xAA,
    [0x1A] = '[', [0x1B] = ']', [0x28] = '{', [0x29] = '\\', [0x2B] = '}'
};

/* ===== Latinoamericano (LA) ===== */
static const u8 la_n[0x57] = {
    [0x29] = '|',
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '\'', [0x0D] = 0xA8, [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '\'', [0x1B] = '+', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = K_NTILDE,
    [0x28] = '{', [0x2B] = '}',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x37] = '*', [0x39] = ' ', [0x56] = '<'
};
static const u8 la_s[0x57] = {
    [0x29] = 0xF8,                                      /* ° */
    [0x02] = '!', [0x03] = '"', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = 0xAD, [0x0E] = '\b', [0x0F] = '\t',
    [0x1A] = '"', [0x1B] = '*', [0x1C] = '\n',
    [0x28] = '[', [0x2B] = ']',
    [0x33] = ';', [0x34] = ':', [0x35] = '_',
    [0x37] = '*', [0x39] = ' ', [0x56] = '>'
};
static const u8 la_g[0x57] = {
    [0x29] = 0xAA, [0x0C] = '\\', [0x10] = '@', [0x1B] = '~', [0x28] = '^', [0x2B] = '`'
};

/* ===== Estados Unidos (US) ===== */
static const u8 us_n[0x57] = {
    [0x29] = '`',
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = '-', [0x0D] = '=', [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x37] = '*', [0x39] = ' ', [0x56] = '\\'
};
static const u8 us_s[0x57] = {
    [0x29] = '~',
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')',
    [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b', [0x0F] = '\t',
    [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
    [0x27] = ':', [0x28] = '"', [0x2B] = '|',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x37] = '*', [0x39] = ' ', [0x56] = '|'
};
static const u8 us_g[0x57] = { [0x01] = 0 };

static const u8 *const maps[KB_COUNT][3] = {
    { es_n, es_s, es_g }, { la_n, la_s, la_g }, { us_n, us_s, us_g }
};
static const char *const kb_names[KB_COUNT] = { "ES", "LA", "US" };
static const char *const kb_long[KB_COUNT] = { "ESPANOL (ESPANA)", "LATINOAMERICANO", "INGLES (ESTADOS UNIDOS)" };
static int layout;
#define map_n (maps[layout][0])
#define map_s (maps[layout][1])
#define map_g (maps[layout][2])

void kbd_set_layout(int l)  { if (l >= 0 && l < KB_COUNT) layout = l; }
int  kbd_get_layout(void)   { return layout; }
const char *kbd_layout_name(int l, int longname)
{
    if (l < 0 || l >= KB_COUNT) return "";
    return longname ? kb_long[l] : kb_names[l];
}

#define QSIZE 1024
static u8 queue[QSIZE];
static int qhead, qtail;
static int shift, caps, altgr, ext, esc_flag;

/* ---- entrada cruda: scancodes tal cual (DOOM y otros juegos de pantalla completa) ---- */
#define RQSIZE 512
static u8 rawq[RQSIZE];
static int rawh, rawt, raw_on;

static void push(u8 c)
{
    int n = (qhead + 1) & (QSIZE - 1);
    if (n != qtail) {
        queue[qhead] = c;
        qhead = n;
    }
}

static int is_letter(u8 c)
{
    return (c >= 'a' && c <= 'z') || c == K_NTILDE || c == K_CCEDIL;
}

static u8 to_upper(u8 c)
{
    if (c >= 'a' && c <= 'z')
        return (u8)(c - 32);
    if (c == K_NTILDE)
        return 0xA5;                    /* Ñ */
    if (c == K_CCEDIL)
        return 0x80;                    /* Ç */
    return c;
}

void audio_service(void);
extern void (*g_gui_tick)(void);        /* kernel.c: gancho del escritorio */
extern u64 g_tsc_hz;

static void gui_tick(void)
{
    static u64 next;
    static int busy;
    u64 t;
    if (!g_gui_tick || busy)
        return;
    t = rdtsc();
    if (t < next)
        return;
    next = t + (g_tsc_hz ? g_tsc_hz : 2000000000ull) / 60;
    busy = 1;
    g_gui_tick();
    busy = 0;
}

/* teclas apretadas ahora mismo (para juegos: KEYDOWN de BASIC, BMG). Se lleva por scancode: el de 1 byte y el
 * extendido (E0 xx: flechas...) por separado */
static u8 held[128], held_e[128], held_pfx;

static void track_held(u8 sc)
{
    if (sc == 0xE0) { held_pfx = 1; return; }
    if (sc == 0xE1) return;                     /* Pausa: se ignora */
    if (held_pfx) held_e[sc & 0x7F] = !(sc & 0x80);
    else held[sc & 0x7F] = !(sc & 0x80);
    held_pfx = 0;
}

/* code: letra o simbolo (su ASCII; da igual mayuscula o minuscula), 13 Enter, 32 espacio, flechas con los codigos
 * PETSCII que da INKEY (145 arriba, 17 abajo, 157 izquierda, 29 derecha), 256+n = scancode n tal cual */
int kbd_held(int code)
{
    int sc;
    if (code >= 256 && code < 384) return held[code - 256];
    switch (code) {
    case 145: return held_e[0x48];
    case 17:  return held_e[0x50];
    case 157: return held_e[0x4B];
    case 29:  return held_e[0x4D];
    case 13:  return held[0x1C] || held_e[0x1C];
    case 32:  return held[0x39];
    case 27:  return held[0x01];
    case 9:   return held[0x0F];
    case 8:   return held[0x0E];
    }
    if (code >= 'a' && code <= 'z') code -= 32;
    for (sc = 1; sc < 0x57; sc++) {
        int c = map_n[sc];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c && c == code) return held[sc];
    }
    return 0;
}

void kbd_poll(void)
{
    audio_service();                    /* el sonido sigue mientras BASIC espera o calcula */
    gui_tick();                         /* el escritorio sigue vivo mientras BASIC corre */
    task_yield();                       /* y las tareas de fondo reciben su turno (task.c) */
    while (inb(0x64) & 1) {
        u8 st = inb(0x64);
        u8 sc = inb(0x60);

        if (st & 0x20) {                /* dato del raton */
            mouse_byte(sc);
            continue;
        }
        track_held(sc);
        if (raw_on) {                   /* modo crudo: el scancode va a su cola, sin interpretar */
            int n = (rawh + 1) & (RQSIZE - 1);
            if (n != rawt) {
                rawq[rawh] = sc;
                rawh = n;
            }
            continue;
        }
        if (sc == 0xE0) {               /* prefijo de tecla extendida */
            ext = 1;
            continue;
        }
        if (ext) {                      /* AltGr = Alt derecho (E0 38 / E0 B8); el resto se ignora */
            ext = 0;
            if (sc == 0x38)
                altgr = 1;
            else if (sc == 0xB8)
                altgr = 0;
            else if (sc == 0x48)
                push(145);              /* flechas: codigos PETSCII del cursor */
            else if (sc == 0x50)
                push(17);
            else if (sc == 0x4B)
                push(157);
            else if (sc == 0x4D)
                push(29);
            continue;
        }
        if (sc & 0x80) {                /* soltar tecla */
            u8 k = sc & 0x7F;
            if (k == 0x2A || k == 0x36)
                shift = 0;
            continue;
        }
        if (sc == 0x2A || sc == 0x36) {
            shift = 1;
            continue;
        }
        if (sc == 0x3A) {
            caps ^= 1;
            continue;
        }
        if (sc == 0x01) {
            esc_flag = 1;
            continue;
        }
        if (sc < 0x57) {
            u8 base = map_n[sc], c;
            if (altgr)
                c = map_g[sc];
            else if (is_letter(base))
                c = (caps ^ shift) ? base : to_upper(base);     /* MAYUSCULAS por defecto */
            else
                c = shift ? map_s[sc] : base;
            if (c)
                push(c);
        }
    }
}

int kbd_rawin(int op, int *a, int *b)
{
    int v;
    switch (op) {
    case 0:
        raw_on = 1;
        rawh = rawt = 0;
        mouse_raw_mode(1);
        return 0;
    case 1:
        raw_on = 0;
        rawh = rawt = 0;
        ext = shift = altgr = 0;
        esc_flag = 0;
        mouse_raw_mode(0);
        return 0;
    case 2:
        kbd_poll();
        if (rawh == rawt)
            return -1;
        v = rawq[rawt];
        rawt = (rawt + 1) & (RQSIZE - 1);
        return v;
    case 3:
        kbd_poll();
        return mouse_raw_get(a, b);
    }
    return -1;
}

/* Mete una tecla en la cola (lo usa el pegado con el raton) */
void kbd_inject(u8 c)
{
    push(c);
}

static u64 blink_cycles;

void kbd_set_blink(u64 cycles)
{
    blink_cycles = cycles;
}

int kbd_getc(void)
{
    int cursor = 0, on = 1;
    u64 next = 0;

    for (;;) {
        kbd_poll();
        if (qhead != qtail) {
            u8 c = queue[qtail];
            qtail = (qtail + 1) & (QSIZE - 1);
            if (cursor)
                vga_cursor_show(0);
            return c;
        }
        if (!cursor) {                          /* esperando: mostrar el cursor */
            cursor = 1;
            vga_cursor_show(1);
            if (blink_cycles)
                next = rdtsc() + blink_cycles;
        } else if (blink_cycles && rdtsc() >= next) {
            on ^= 1;
            vga_cursor_show(on);
            next = rdtsc() + blink_cycles;
        }
        cpu_idle();                             /* hlt hasta el siguiente milisegundo (o pause sin reloj) */
    }
}

/* Tecla pendiente sin esperar (0 = ninguna) */
int kbd_inkey(void)
{
    u8 c;
    kbd_poll();
    if (qhead == qtail)
        return 0;
    c = queue[qtail];
    qtail = (qtail + 1) & (QSIZE - 1);
    return c;
}

void kbd_esc_again(void)
{
    esc_flag = 1;                       /* devolver un ESC ya leido (lo usan las esperas) */
}

/* Devuelve 1 (y limpia la bandera) si se pulso ESC desde la ultima vez */
int kbd_esc(void)
{
    int r;
    kbd_poll();
    r = esc_flag;
    esc_flag = 0;
    return r;
}

int kbd_readline(char *buf, int max)
{
    int n = 0;
    esc_flag = 0;
    for (;;) {
        int c = kbd_getc();
        if (c == '\n') {
            vga_putc('\n');
            buf[n] = 0;
            return n;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                vga_putc('\b');
            }
            continue;
        }
        if (((c >= 32 && c < 127) || c >= 160) && n < max - 1) {
            buf[n++] = (char)c;
            vga_putc((char)c);
        }
    }
}
