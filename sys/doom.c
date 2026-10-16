/* doom.c - DOOM.BIN: el motor PureDOOM (id Software, DOOM Source Code License) + la parte de BASIC-OS
 *
 * No cabe en /SYS (el motor ocupa ~0.4 MB), asi que vive en la particion FAT junto al WAD y lo carga
 * DOOM.APP (sys/doomapp.c), igual que NET.APP carga NET.BIN. Este modulo esta enlazado a 0x4000000
 * (64 MB) y usa memoria fija:
 *
 *   0x4000000  DOOM.BIN (codigo, datos, bss ~1 MB)
 *   0x4200000  heap del motor (17 MB: 12 MB de zona de Doom + sus mallocs)
 *   0x5300000  pagina de control del WAD en cache
 *   0x5400000  el WAD (hasta 16 MB). Se queda ahi: la segunda vez que abres DOOM no se vuelve a leer del disco
 *   0x6400000  copia de la pantalla (pitch x alto bytes): al salir se restaura tal cual (escritorio o consola)
 *
 * Pantalla completa: dibuja directo en el framebuffer VBE (el mismo que dejo stage2.asm), escalado a 4:3.
 * Teclado y raton en crudo (K->rawin). Sonido por K->stream (HDA/AC'97): efectos de PureDOOM + musica MUS con el sintetizador entero de mussyn.h.
 * Salir: F12 (siempre) o el menu de Doom (ESC -> Salir del juego -> Y).
 *
 * Teclas: flechas = mover/girar, Ctrl = disparar, ESPACIO = abrir/usar, Shift = correr, Alt = esquivar,
 *         1-7 = armas, TAB = mapa, ESC = menu, F11 = alternar WASD (W/S avanzar, A/D esquivar), F12 = salir.
 */
#include "kapi.h"
#include "mod_head.h"

#include "mussyn.h"          /* musica: sintetizador de DoomProject (chip.c) en enteros */
#define BASICOS_MUS_VOL(v)   mus_vol(v)
#define BASICOS_MUS_PLAY(l)  mus_play(l)
#define BASICOS_MUS_PAUSE(p) mus_pause(p)
#define BASICOS_MUS_STOP()   mus_stop()
#define BASICOS_MUS_REG(d)   mus_reg(d)
#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

static const struct kapi *K;

#define HEAP_BASE  0x4200000u
#define HEAP_SIZE  0x1100000u           /* 17 MB */
#define META_ADDR  0x5300000u
#define WAD_BASE   0x5400000u
#define WAD_MAX    0x1000000u           /* 16 MB (DOOM2.WAD pesa 14.6 MB) */
#define MEM_END    (WAD_BASE + WAD_MAX)
#define SAVE_BASE  MEM_END              /* copia de la pantalla de antes de Doom, para devolverla intacta al salir */
#define SAVE_MAX   0x2000000u           /* 32 MB: pantallas de hasta ~3840x2160 */

/* ---- bootinfo: lo deja stage2.asm en 0x6000 (igual que kernel.h) ---- */
struct bootinfo {
    u32 magic;
    u16 w, h, pitch;
    u8  bpp, gfx;
    u32 fb;
    u8  rs, rp, gs, gp, bs, bp;
    u16 e820n;
    u16 extkb;
} __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));
#define BI  ((const struct bootinfo *)0x6000)
#define E8  ((const struct e820 *)0x6020)

/* ---- salida limpia desde dentro del motor (exit hook) sin libc: setjmp/longjmp minimos ---- */
static unsigned long jb[8];
int  d_setjmp(unsigned long *b) __attribute__((returns_twice));
void d_longjmp(unsigned long *b, int v) __attribute__((noreturn));
__asm__(
    ".text\n"
    ".globl d_setjmp\n"
    "d_setjmp:\n"
    "    mov %rbx, 0(%rdi)\n"
    "    mov %rbp, 8(%rdi)\n"
    "    mov %r12, 16(%rdi)\n"
    "    mov %r13, 24(%rdi)\n"
    "    mov %r14, 32(%rdi)\n"
    "    mov %r15, 40(%rdi)\n"
    "    lea 8(%rsp), %rdx\n"
    "    mov %rdx, 48(%rdi)\n"
    "    mov (%rsp), %rdx\n"
    "    mov %rdx, 56(%rdi)\n"
    "    xor %eax, %eax\n"
    "    ret\n"
    ".globl d_longjmp\n"
    "d_longjmp:\n"
    "    mov %esi, %eax\n"
    "    mov 0(%rdi), %rbx\n"
    "    mov 8(%rdi), %rbp\n"
    "    mov 16(%rdi), %r12\n"
    "    mov 24(%rdi), %r13\n"
    "    mov 32(%rdi), %r14\n"
    "    mov 40(%rdi), %r15\n"
    "    mov 48(%rdi), %rsp\n"
    "    jmp *56(%rdi)\n");

/* ---- registro de mensajes del motor (se enseña si algo sale mal) ---- */
#define LOGSZ 2048
static char logbuf[LOGSZ];
static int  loglen;

static void d_print(const char *s)
{
    while (*s) {
        if (loglen == LOGSZ - 1) {                 /* se queda con lo ultimo */
            memcpy(logbuf, logbuf + LOGSZ / 2, LOGSZ / 2 - 1);
            loglen = LOGSZ / 2 - 1;
        }
        logbuf[loglen++] = *s++;
    }
    logbuf[loglen] = 0;
}

/* ---- memoria: un bump allocator sobre el heap fijo ---- */
static u32 heap_ptr;

static void *d_malloc(int size)
{
    u32 n = ((u32)size + 15u) & ~15u;
    void *p;
    if (size <= 0 || heap_ptr + n > HEAP_BASE + HEAP_SIZE) {
        d_print("DOOM: sin memoria en el heap\n");
        return 0;
    }
    p = (void *)(unsigned long)heap_ptr;
    heap_ptr += n;
    return p;
}

static void d_free(void *p) { (void)p; }

/* ---- el WAD: un solo archivo de solo lectura en RAM ---- */
struct meta {
    u32 magic;                      /* 'DWAD' */
    u32 size;
    u32 sum;                        /* suma de comprobacion de la cabeza y la cola */
    char name[16];                  /* "DOOM1.WAD", "DOOM.WAD"... */
};
#define META ((struct meta *)META_ADDR)
#define META_MAGIC 0x44415744u

static struct { const u8 *base; int size, pos; } wadf;
static const char *wadname;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void *d_open(const char *fn, const char *mode)
{
    const char *t = fn, *p, *n;
    for (p = fn; *p; p++)
        if (*p == '/' || *p == '\\')
            t = p + 1;
    for (p = mode; *p; p++)
        if (*p == 'w' || *p == 'a' || *p == '+')
            return 0;               /* no se guarda nada (config, partidas) */
    if (!wadname)
        return 0;
    for (n = wadname; *n && *t && lc(*n) == lc(*t); n++, t++)
        ;
    if (*n || *t)
        return 0;
    wadf.base = (const u8 *)(unsigned long)WAD_BASE;
    wadf.size = (int)META->size;
    wadf.pos = 0;
    return &wadf;
}

static void d_close(void *h) { (void)h; }

/* Mientras el motor trabaja sin pasar por game_loop (cargar un mapa, dibujar) nadie rellena el anillo de audio y la tarjeta
 * repetiria el ultimo trozo en bucle: estos avisos al kernel lo mantienen alimentado (con silencio si la cola esta vacia). */
static void snd_keep(void);

static int d_read(void *h, void *buf, int count)
{
    int n = wadf.size - wadf.pos;
    (void)h;
    snd_keep();
    if (count < n)
        n = count;
    if (n <= 0)
        return 0;
    memcpy(buf, wadf.base + wadf.pos, (unsigned long)n);
    wadf.pos += n;
    return n;
}

static int d_write(void *h, const void *buf, int count) { (void)h; (void)buf; (void)count; return 0; }

static int d_seek(void *h, int off, doom_seek_t whence)
{
    int p;
    (void)h;
    snd_keep();
    p = whence == DOOM_SEEK_SET ? off : whence == DOOM_SEEK_CUR ? wadf.pos + off : wadf.size + off;
    if (p < 0 || p > wadf.size)
        return -1;
    wadf.pos = p;
    return 0;
}

static int d_tell(void *h) { (void)h; return wadf.pos; }
static int d_eof(void *h)  { (void)h; return wadf.pos >= wadf.size; }

static void d_gettime(int *sec, int *usec)
{
    snd_keep();
    unsigned ms = K->ms();
    *sec = (int)(ms / 1000u);
    *usec = (int)(ms % 1000u) * 1000;
}

static int exit_code;

static void d_exit(int code)
{
    exit_code = code;
    d_longjmp(jb, 1);
}

static char *d_getenv(const char *v)
{
    static char home[] = ".";
    if (v[0] == 'H' && v[1] == 'O' && v[2] == 'M' && v[3] == 'E' && !v[4])
        return home;
    return 0;                       /* DOOMWADDIR: el directorio actual */
}

/* ---- pantalla ---- */
static volatile u8 *fbp;
static u32 pitch, bytespp, scw, sch;
static u32 lutr[256], lutg[256], lutb[256];
static int dw, dh, ox, oy;                  /* rectangulo 4:3 donde va Doom */
static u32 save_size;
static int saved;

#define SRC_W 320
#define SRC_H 200
static u32 native[SRC_W * SRC_H];
static u16 xmap[4096];
static u8  rowbuf[4096 * 4];

static u32 pack(u32 r, u32 g, u32 b) { return lutr[r & 255] | lutg[g & 255] | lutb[b & 255]; }

static void gfx_init(void)
{
    const struct bootinfo *bi = BI;
    u32 rs = bi->rs, rp = bi->rp, gs = bi->gs, gp = bi->gp, bs = bi->bs, bp = bi->bp, i;
    if (!rs && !gs && !bs) {                /* BIOS sin mascaras: valores por defecto */
        if (bi->bpp == 16) { rs = 5; rp = 11; gs = 6; gp = 5; bs = 5; bp = 0; }
        else               { rs = 8; rp = 16; gs = 8; gp = 8; bs = 8; bp = 0; }
    }
    fbp = (volatile u8 *)(unsigned long)bi->fb;
    pitch = bi->pitch;
    bytespp = bi->bpp / 8u;
    scw = bi->w;
    sch = bi->h;
    for (i = 0; i < 256; i++) {
        lutr[i] = (i >> (8 - rs)) << rp;
        lutg[i] = (i >> (8 - gs)) << gp;
        lutb[i] = (i >> (8 - bs)) << bp;
    }
    /* rectangulo 4:3 centrado */
    dh = (int)sch;
    dw = dh * 4 / 3;
    if (dw > (int)scw) {
        dw = (int)scw;
        dh = dw * 3 / 4;
    }
    if (dw > 4096)
        dw = 4096;
    ox = ((int)scw - dw) / 2;
    oy = ((int)sch - dh) / 2;
    for (i = 0; i < (u32)dw; i++)
        xmap[i] = (u16)((i * SRC_W) / (u32)dw);
}

static void put_row(int y, const u8 *src, int bytes, int x0)
{
    memcpy((void *)(fbp + (u32)y * pitch + (u32)x0 * bytespp), src, (unsigned long)bytes);
}

static void fill_px(int x, int y, int w, int h, u32 v)
{
    int i, j;
    for (j = 0; j < h; j++) {
        volatile u8 *p = fbp + (u32)(y + j) * pitch + (u32)x * bytespp;
        for (i = 0; i < w; i++, p += bytespp) {
            if (bytespp == 4)
                *(volatile u32 *)p = v;
            else if (bytespp == 2)
                *(volatile u16 *)p = (u16)v;
            else {
                p[0] = (u8)v;
                p[1] = (u8)(v >> 8);
                p[2] = (u8)(v >> 16);
            }
        }
    }
}

static void clear_screen(void)
{
    fill_px(0, 0, (int)scw, (int)sch, 0);
}

static void save_screen(void)
{
    save_size = pitch * sch;
    if (save_size > SAVE_MAX)
        return;
    memcpy((void *)(unsigned long)SAVE_BASE, (const void *)fbp, save_size);
    saved = 1;
}

static void restore_screen(void)
{
    if (saved)
        memcpy((void *)fbp, (const void *)(unsigned long)SAVE_BASE, save_size);
}

static void draw_text(int x, int y, const char *s, int sc, u32 color)
{
    const u8 *font = (const u8 *)FONT_ADDR;
    for (; *s; s++, x += 8 * sc) {
        const u8 *g = font + (u8)*s * 16;
        int r, c;
        for (r = 0; r < 16; r++)
            for (c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c))
                    fill_px(x + c * sc, y + r * sc, sc, sc, color);
    }
}

static int tlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void say(const char *l1, const char *l2)
{
    int sc = scw >= 1000 ? 2 : 1;
    int y = (int)sch / 2 - 16 * sc;
    clear_screen();
    draw_text(((int)scw - tlen(l1) * 8 * sc) / 2, y, l1, sc, pack(255, 255, 255));
    if (l2)
        draw_text(((int)scw - tlen(l2) * 8 * sc) / 2, y + 24 * sc, l2, sc, pack(255, 224, 0));
}

static void blit(void)
{
    const u32 *src = (const u32 *)doom_get_framebuffer(4);     /* RGBA */
    int i, y, last = -1, bytes = dw * (int)bytespp;
    u32 *nat = native;

    for (i = 0; i < SRC_W * SRC_H; i++) {
        u32 v = src[i];
        nat[i] = pack(v, v >> 8, v >> 16);
        if ((i & 8191) == 0)
            snd_keep();
    }
    for (y = 0; y < dh; y++) {
        int sy = y * SRC_H / dh, x;
        if (sy != last) {
            const u32 *row = nat + sy * SRC_W;
            last = sy;
            if (bytespp == 4) {
                u32 *d = (u32 *)rowbuf;
                for (x = 0; x < dw; x++)
                    d[x] = row[xmap[x]];
            } else if (bytespp == 2) {
                u16 *d = (u16 *)rowbuf;
                for (x = 0; x < dw; x++)
                    d[x] = (u16)row[xmap[x]];
            } else {
                u8 *d = rowbuf;
                for (x = 0; x < dw; x++, d += 3) {
                    u32 v = row[xmap[x]];
                    d[0] = (u8)v;
                    d[1] = (u8)(v >> 8);
                    d[2] = (u8)(v >> 16);
                }
            }
        }
        put_row(oy + y, rowbuf, bytes, ox);
        if ((y & 15) == 0)
            snd_keep();
    }
}

/* ---- teclado: scancodes set 1 -> teclas de Doom ---- */
static int wasd;                            /* F11 */
static u16 downkey[256 * 2];                /* que tecla de Doom mandamos al pulsar cada scancode */
static unsigned down_at[256 * 2];           /* cuadro (doom_update) en que se pulso */
static u8 pend_up[256 * 2];                 /* soltada demasiado pronto: se entrega al cuadro siguiente */
static unsigned cur_frame;                  /* cuadros de Doom ejecutados */

static int map_key(int sc, int e0)
{
    if (e0) {
        switch (sc) {
        case 0x48: return DOOM_KEY_UP_ARROW;
        case 0x50: return DOOM_KEY_DOWN_ARROW;
        case 0x4B: return DOOM_KEY_LEFT_ARROW;
        case 0x4D: return DOOM_KEY_RIGHT_ARROW;
        case 0x1D: return DOOM_KEY_CTRL;
        case 0x38: return DOOM_KEY_ALT;
        case 0x1C: return DOOM_KEY_ENTER;
        case 0x35: return DOOM_KEY_SLASH;
        }
        return -1;
    }
    switch (sc) {
    case 0x01: return DOOM_KEY_ESCAPE;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x07: case 0x08: case 0x09: case 0x0A:
        return '1' + (sc - 0x02);
    case 0x0B: return '0';
    case 0x0C: return DOOM_KEY_MINUS;
    case 0x0D: return DOOM_KEY_EQUALS;
    case 0x0E: return DOOM_KEY_BACKSPACE;
    case 0x0F: return DOOM_KEY_TAB;
    case 0x10: return 'q';
    case 0x11: return wasd ? DOOM_KEY_UP_ARROW : 'w';
    case 0x12: return 'e';
    case 0x13: return 'r';
    case 0x14: return 't';
    case 0x15: return 'y';
    case 0x16: return 'u';
    case 0x17: return 'i';
    case 0x18: return 'o';
    case 0x19: return 'p';
    case 0x1A: return DOOM_KEY_LEFT_BRACKET;
    case 0x1B: return DOOM_KEY_RIGHT_BRACKET;
    case 0x1C: return DOOM_KEY_ENTER;
    case 0x1D: return DOOM_KEY_CTRL;
    case 0x1E: return wasd ? DOOM_KEY_COMMA : 'a';
    case 0x1F: return wasd ? DOOM_KEY_DOWN_ARROW : 's';
    case 0x20: return wasd ? DOOM_KEY_PERIOD : 'd';
    case 0x21: return 'f';
    case 0x22: return 'g';
    case 0x23: return 'h';
    case 0x24: return 'j';
    case 0x25: return 'k';
    case 0x26: return 'l';
    case 0x27: return DOOM_KEY_SEMICOLON;
    case 0x28: return DOOM_KEY_APOSTROPHE;
    case 0x2A: case 0x36: return DOOM_KEY_SHIFT;
    case 0x2C: return 'z';
    case 0x2D: return 'x';
    case 0x2E: return 'c';
    case 0x2F: return 'v';
    case 0x30: return 'b';
    case 0x31: return 'n';
    case 0x32: return 'm';
    case 0x33: return DOOM_KEY_COMMA;
    case 0x34: return DOOM_KEY_PERIOD;
    case 0x35: return DOOM_KEY_SLASH;
    case 0x37: return DOOM_KEY_MULTIPLY;
    case 0x38: return DOOM_KEY_ALT;
    case 0x39: return DOOM_KEY_SPACE;
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
        return DOOM_KEY_F1 + (sc - 0x3B);
    case 0x57: return DOOM_KEY_F11;
    }
    return -1;
}

/* ---- sonido: Doom mezcla efectos (512 cuadros estereo s16 a 11025 Hz por llamada) y la musica la sintetiza mussyn.h
 * (mono, MUS_SR = 22050 Hz). Aqui se juntan a 22050 Hz (los efectos se duplican con interpolacion lineal) y el kernel
 * lo mezcla en la tarjeta con K->stream. ---- */
static int snd_on;

static void snd_keep(void)
{
    if (snd_on)
        K->stream(3, 0, 0);
}

static void snd_pump(void)
{
    int n = 0;
    static short mix[1024 * 2];
    static int pl, pr;
    while (snd_on && n++ < 4 && K->stream(3, 0, 0) < 2560) {    /* mantener ~90 ms en cola */
        const short *b = doom_get_sound_buffer();
        int i;
        for (i = 0; i < 512; i++) {
            int l = b[i * 2], r = b[i * 2 + 1], m0 = mus_next(), m1 = mus_next();
            mix[i * 4]     = (short)clamp15((pl + l) / 2 + m0);
            mix[i * 4 + 1] = (short)clamp15((pr + r) / 2 + m0);
            mix[i * 4 + 2] = (short)clamp15(l + m1);
            mix[i * 4 + 3] = (short)clamp15(r + m1);
            pl = l; pr = r;
        }
        K->stream(1, mix, 1024);
    }
}

static int quit_req;
static int e0_pending, e1_skip;

static void key_event(int sc)
{
    int make, k, idx, e0;
    if (e1_skip) {                          /* Pausa: E1 1D 45 E1 9D C5 */
        e1_skip--;
        return;
    }
    if (sc == 0xE1) {
        e1_skip = 5;
        return;
    }
    if (sc == 0xE0) {
        e0_pending = 1;
        return;
    }
    e0 = e0_pending;
    e0_pending = 0;
    make = !(sc & 0x80);
    sc &= 0x7F;
    idx = sc + (e0 ? 256 : 0);
    if (!e0 && sc == 0x58) {                /* F12: salir */
        if (make)
            quit_req = 1;
        return;
    }
    if (!e0 && sc == 0x57) {                /* F11: WASD */
        if (make)
            wasd = !wasd;
        return;
    }
    if (make) {
        if (downkey[idx])
            return;                         /* repeticion del teclado: Doom ya sabe que esta pulsada */
        k = map_key(sc, e0);
        if (k < 0)
            return;
        downkey[idx] = (u16)(k + 1);
        down_at[idx] = cur_frame;
        pend_up[idx] = 0;
        doom_key_down((doom_key_t)k);
    } else if (downkey[idx]) {
        if (down_at[idx] == cur_frame) {    /* un toque rapido: Doom aun no vio la tecla pulsada; que dure un cuadro */
            pend_up[idx] = 1;
            return;
        }
        k = downkey[idx] - 1;
        downkey[idx] = 0;
        doom_key_up((doom_key_t)k);
    }
}

static void flush_pending_ups(void)
{
    int i;
    for (i = 0; i < 512; i++)
        if (pend_up[i]) {
            pend_up[i] = 0;
            if (downkey[i]) {
                doom_key_up((doom_key_t)(downkey[i] - 1));
                downkey[i] = 0;
            }
        }
}

static int prev_btn;

static void mouse_event(void)
{
    int dx = 0, dy = 0, b = K->rawin(3, &dx, &dy), ch = b ^ prev_btn;
    if (dx)
        doom_mouse_move(dx * 2, 0);
    if (ch & 1) { if (b & 1) doom_button_down(DOOM_LEFT_BUTTON);   else doom_button_up(DOOM_LEFT_BUTTON); }
    if (ch & 2) { if (b & 2) doom_button_down(DOOM_RIGHT_BUTTON);  else doom_button_up(DOOM_RIGHT_BUTTON); }
    if (ch & 4) { if (b & 4) doom_button_down(DOOM_MIDDLE_BUTTON); else doom_button_up(DOOM_MIDDLE_BUTTON); }
    prev_btn = b;
}

static void release_all_keys(void)
{
    int i;
    for (i = 0; i < 512; i++)
        if (downkey[i]) {
            doom_key_up((doom_key_t)(downkey[i] - 1));
            downkey[i] = 0;
        }
}

/* ---- buscar y cargar el WAD ---- */

static const char *const where[] = {
    "HD0.1:", "HD1.1:", "HD2.1:", "HD0:", "HD1:", "FD1:", "FD0:", "USB0.1:", "USB0:", "CD0:", 0
};
static const char *const wadnames[] = { "DOOM2.WAD", "DOOM.WAD", "DOOMU.WAD", "DOOM1.WAD", 0 };

static u32 wad_sum(const u8 *w, u32 size)
{
    u32 s = size, i;
    for (i = 0; i < 256 && i < size; i++)
        s = s * 31u + w[i];
    for (i = 0; i < 256 && i < size; i++)
        s = s * 31u + w[size - 1 - i];
    return s;
}

static int wad_cached(void)
{
    const u8 *w = (const u8 *)(unsigned long)WAD_BASE;
    struct meta *m = META;
    if (m->magic != META_MAGIC || !m->size || m->size > WAD_MAX || m->name[0] == 0)
        return 0;
    if (w[1] != 'W' || w[2] != 'A' || w[3] != 'D' || (w[0] != 'I' && w[0] != 'P'))
        return 0;
    return m->sum == wad_sum(w, m->size);
}

static int load_wad(void)
{
    char spec[48];
    int i, j, k, n;
    unsigned total;
    struct meta *m = META;
    const u8 *w = (const u8 *)(unsigned long)WAD_BASE;

    if (wad_cached()) {
        wadname = m->name;
        return 1;
    }
    m->magic = 0;
    for (n = 0; wadnames[n]; n++) {
        for (i = 0; where[i]; i++) {
            const char *p = where[i];
            for (j = 0; p[j]; j++)
                spec[j] = p[j];
            for (k = 0; wadnames[n][k]; k++)
                spec[j + k] = wadnames[n][k];
            spec[j + k] = 0;
            total = 0;
            k = K->read(spec, (char *)(unsigned long)WAD_BASE, (int)WAD_MAX, &total);
            if (k > 0 && (unsigned)k == total && total <= WAD_MAX && w[1] == 'W' && w[2] == 'A' && w[3] == 'D') {
                for (j = 0; wadnames[n][j] && j < 15; j++)
                    m->name[j] = wadnames[n][j];
                m->name[j] = 0;
                m->size = total;
                m->sum = wad_sum(w, total);
                m->magic = META_MAGIC;
                wadname = m->name;
                return 1;
            }
        }
    }
    return 0;
}

static u64 ram_end(void)
{
    const struct bootinfo *bi = BI;
    int i;
    u64 end = 0;
    if (bi->e820n) {
        for (i = 0; i < bi->e820n; i++) {
            const struct e820 *e = &E8[i];
            if (e->type == 1 && e->base <= 0x4000000ull && e->base + e->len > 0x4000000ull)
                end = e->base + e->len;
        }
    } else {
        end = ((u64)bi->extkb + 1024) * 1024;
    }
    return end;
}

static int ram_ok(void)
{
    return ram_end() >= (u64)MEM_END;
}

static void delay_note(void) { K->sleep(1200); }     /* que alcance a leerse el aviso de sonido */

static void wait_key(unsigned ms)
{
    unsigned t0 = K->ms();
    K->rawin(0, 0, 0);
    while (K->ms() - t0 < ms) {
        int sc = K->rawin(2, 0, 0);
        if (sc >= 0 && !(sc & 0x80) && sc != 0xE0 && sc != 0xE1)
            break;
        K->sleep(5);
    }
    K->rawin(1, 0, 0);
}

static void show_log(void)
{
    /* ultimas lineas del motor, para saber por que se cerro */
    int sc = scw >= 1000 ? 2 : 1, y = 8, i, start = loglen, lines = 0;
    char line[100];
    while (start > 0 && lines < 14) {
        start--;
        if (logbuf[start] == '\n' && start != loglen - 1)
            lines++;
    }
    if (logbuf[start] == '\n')
        start++;
    clear_screen();
    draw_text(8, y, "DOOM SE CERRO:", sc, pack(255, 224, 0));
    y += 20 * sc;
    for (i = start; i < loglen;) {
        int n = 0;
        while (i < loglen && logbuf[i] != '\n' && n < 98)
            line[n++] = logbuf[i++];
        if (i < loglen && logbuf[i] == '\n')
            i++;
        line[n] = 0;
        draw_text(8, y, line, sc, pack(255, 255, 255));
        y += 16 * sc;
        if (y > (int)sch - 40)
            break;
    }
    draw_text(8, (int)sch - 24 * sc, "(pulsa una tecla)", sc, pack(255, 224, 0));
}

static int game_loop(void)
{
    unsigned t0 = K->ms(), frame = 0;
    int e;

    while (!quit_req) {
        int sc;
        unsigned now = K->ms(), due;
        while ((sc = K->rawin(2, 0, 0)) >= 0)
            key_event(sc);
        mouse_event();
        snd_pump();
        due = t0 + frame * 1000u / 35u;
        if (now >= due) {
            doom_update();
            snd_pump();
            cur_frame++;
            flush_pending_ups();
            blit();
            frame++;
            if (now - due > 250u) {                 /* nos atrasamos mucho: no intentar alcanzar */
                t0 = now;
                frame = 1;
            }
        } else {
            K->sleep(1);
        }
    }
    e = 0;
    return e;
}

int mod_main(const struct kapi *api)
{
    static char *argv[] = { "doom", 0 };
    int ok = 1;

    K = api;
    if (!BI->gfx || !BI->fb || (BI->bpp != 16 && BI->bpp != 24 && BI->bpp != 32)) {
        K->print("DOOM NECESITA UN MODO GRAFICO (VESA)\n");
        return 1;
    }
    gfx_init();
    K->set_tick(0);                         /* el escritorio no debe pintar mientras Doom corre */
    if (ram_end() >= (u64)SAVE_BASE + (u64)pitch * sch)
        save_screen();
    if (!ram_ok()) {
        say("DOOM NECESITA ~100 MB DE RAM", "(en QEMU: -m 128)");
        wait_key(4000);
        restore_screen();
        return 1;
    }
    say("CARGANDO DOOM...", "BUSCANDO EL WAD EN LOS DISCOS");
    if (!load_wad()) {
        say("NO ENCUENTRO EL WAD (DOOM.WAD / DOOM1.WAD)", "COPIALO A LA PARTICION FAT, JUNTO A DOOM.BIN");
        wait_key(8000);
        restore_screen();
        return 1;
    }
    /* el audio se arranca despues de cargar el WAD: durante lecturas largas del disco nadie rellena el anillo
     * y la tarjeta repetiria en bucle lo ultimo que se mezclo */
    snd_on = K->stream(0, 0, 0);            /* 0 = sin tarjeta HDA/AC'97: Doom corre mudo */
    if (snd_on)
        K->stream(4, 0, MUS_SR);            /* flujo a 22050 Hz: efectos + musica */
    if (snd_on)
        K->sound(SND_SINE, 880, 0, 250);    /* pitido de prueba: si lo oyes, la tarjeta funciona */
    say(wadname, snd_on ? "SONIDO: TARJETA HDA/AC97 ACTIVA (PITIDO)" : "SONIDO: SIN TARJETA HDA/AC97 (MUDO)");
    delay_note();                           /* K->sleep mantiene vivo el audio: suena el pitido */

    heap_ptr = HEAP_BASE;
    memset((void *)(unsigned long)HEAP_BASE, 0, 4096);
    doom_set_print(d_print);
    doom_set_malloc(d_malloc, d_free);
    doom_set_file_io(d_open, d_close, d_read, d_write, d_seek, d_tell, d_eof);
    doom_set_gettime(d_gettime);
    doom_set_exit(d_exit);
    doom_set_getenv(d_getenv);

    if (!d_setjmp(jb)) {
        doom_init(1, argv, snd_on ? 0 : (DOOM_FLAG_HIDE_SOUND_OPTIONS | DOOM_FLAG_HIDE_MUSIC_OPTIONS));
        clear_screen();
        K->rawin(0, 0, 0);
        game_loop();
    } else if (exit_code != 0) {
        ok = 0;
    }
    release_all_keys();
    K->rawin(1, 0, 0);
    K->stream(2, 0, 0);
    snd_on = 0;
    if (!ok) {
        show_log();
        wait_key(15000);
    }
    if (saved) {
        restore_screen();                   /* la pantalla de antes (escritorio o consola), pixel por pixel */
    } else {
        clear_screen();
        if (K->vid(0, 0, 0, 0) >= 0)        /* sin copia: al menos repintar la consola de texto */
            K->vid(2, 0, 0, 0);
    }
    return ok ? 0 : 1;
}
