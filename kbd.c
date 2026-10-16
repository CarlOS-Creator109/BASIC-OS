/* kbd.c - teclado PS/2 por sondeo (scancode set 1)
 *
 * Distribucion en espanol (ISO, Espana): Shift+2 = ", AltGr+2 = @, ñ, ç, ¿, ¡, tecla < >.
 * Los caracteres fuera de ASCII salen con los codigos de CP437, que es lo que dibuja la
 * fuente VGA:  ñ=A4 Ñ=A5  ç=87 Ç=80  ¡=AD  ¿=A8  º=A7  ª=A6  ·=FA  ¬=AA
 *
 * Las letras salen en MAYUSCULAS por defecto; Shift o Bloq Mayus las pasan a minusculas.
 * Las teclas muertas (acentos) no estan implementadas: ` ´ ¨ ^ salen directo.
 */
#include "kernel.h"

#define K_NTILDE 0xA4   /* ñ */
#define K_CCEDIL 0x87   /* ç */

/* sin modificador */
static const u8 map_n[0x57] = {
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

/* con Shift */
static const u8 map_s[0x57] = {
    [0x02] = '!', [0x03] = '"', [0x04] = 0xFA, [0x05] = '$', [0x06] = '%',
    [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')', [0x0B] = '=',
    [0x0C] = '?', [0x0D] = 0xA8, [0x0E] = '\b', [0x0F] = '\t',
    [0x1A] = '^', [0x1B] = '*', [0x1C] = '\n',
    [0x28] = '"', [0x29] = 0xA6,
    [0x33] = ';', [0x34] = ':', [0x35] = '_',
    [0x37] = '*', [0x39] = ' ', [0x56] = '>'
};

/* con AltGr */
static const u8 map_g[0x57] = {
    [0x02] = '|', [0x03] = '@', [0x04] = '#', [0x05] = '~', [0x07] = 0xAA,
    [0x1A] = '[', [0x1B] = ']', [0x28] = '{', [0x29] = '\\', [0x2B] = '}'
};

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

void kbd_poll(void)
{
    audio_service();                    /* el sonido sigue mientras BASIC espera o calcula */
    gui_tick();                         /* el escritorio sigue vivo mientras BASIC corre */
    while (inb(0x64) & 1) {
        u8 st = inb(0x64);
        u8 sc = inb(0x60);

        if (st & 0x20) {                /* dato del raton */
            mouse_byte(sc);
            continue;
        }
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
        __asm__ volatile("pause");
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
