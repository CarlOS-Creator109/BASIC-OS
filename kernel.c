/* kernel.c - kmain() y la capa de plataforma que usa el interprete */
#include "kernel.h"
#include "basic.h"
#include "store.h"
#include "fs.h"
#include "audio.h"
#include "cd.h"
#include "kapi.h"
#include "module.h"
#include "sys.h"

/* ---- plataforma para basic.c ---- */

void plat_putc(char c)                    { vga_putc(c); }
int  plat_col(void)                       { return vga_col(); }
void plat_clear(void)                     { vga_clear(); }
void plat_color(int fg, int bg)           { vga_setcolor(fg, bg); }
int  plat_readline(char *buf, int max)    { return kbd_readline(buf, max); }
int  plat_break(void)                     { return kbd_esc(); }
int  plat_inkey(void)                     { return kbd_inkey(); }
int  plat_mouse(int what)                 { return vga_mouse_get(what); }
int  plat_drive_line(int i, char *out)    { return blk_line(i, out); }
const char *plat_audio_info(void)         { return audio_info(); }
int  plat_pci_line(int i, char *out)      { return pci_line(i, out); }
int  plat_gfx_w(void)                     { return vga_gfx_w(); }
int  plat_gfx_h(void)                     { return vga_gfx_h(); }
void plat_plot(int x, int y, int c)       { vga_plot(x, y, c); }
void plat_hline(int x1, int x2, int y, int c) { vga_hline(x1, x2, y, c); }

/* ---- sonido y tiempo: altavoz del PC (PIT canal 2) y PIT canal 0 como reloj ---- */

#define PIT_HZ 1193182u

static void sleep_ms(int ms, int use_esc)
{
    u64 need = (u64)(unsigned int)ms * PIT_HZ / 1000, got = 0;
    u32 prev, cur;

    outb(0x43, 0x34);                         /* canal 0, modo 2, cuenta 65536 */
    outb(0x40, 0);
    outb(0x40, 0);
    outb(0x43, 0x00);
    prev = inb(0x40);
    prev |= (u32)inb(0x40) << 8;
    while (got < need) {
        outb(0x43, 0x00);                     /* latch */
        cur = inb(0x40);
        cur |= (u32)inb(0x40) << 8;
        got += (prev - cur) & 0xFFFF;
        prev = cur;
        audio_service();
        if (use_esc && kbd_esc()) {
            kbd_esc_again();                  /* que BASIC tambien vea el ESC y detenga el programa */
            break;
        }
    }
}

void plat_sleep(int ms) { sleep_ms(ms, 1); }
void delay_ms(int ms)   { sleep_ms(ms, 0); }

void plat_sndwait(void)
{
    while (audio_busy()) {
        if (kbd_esc()) {
            audio_stop();
            break;
        }
        delay_ms(5);
        audio_service();
    }
}

int plat_sound(int kind, int hz, int arg, int ms)
{
    return audio_sound(kind, hz, arg, ms);
}

void plat_beep(int ms, int hz)
{
    if (hz > 0 && audio_open()) {               /* tarjeta de sonido (HDA o AC'97) */
        audio_sound(SND_SINE, hz, 0, ms);
        plat_sndwait();
        return;
    }
    if (hz > 0) {
        u32 div = PIT_HZ / (u32)hz;
        if (div < 1) div = 1;
        if (div > 0xFFFF) div = 0xFFFF;
        outb(0x43, 0xB6);                     /* canal 2, onda cuadrada */
        outb(0x42, (u8)div);
        outb(0x42, (u8)(div >> 8));
        outb(0x61, inb(0x61) | 3);
    }
    plat_sleep(ms);
    outb(0x61, inb(0x61) & ~3);
}

int  plat_cd_toc(int i, char *out)        { return cd_toc(i, out); }
int  plat_cd_play(int track)              { return cd_play(track); }
void plat_cd_stop(void)                   { cd_stop(); }
int  plat_cd_eject(void)                  { return cd_eject(); }
int  plat_cd_status(int *t, int *s)       { return cd_status(t, s); }
void plat_locate(int col, int row)        { vga_locate(col, row); }

/* ---- modulos de /SYS ---- */

void (*g_gui_tick)(void);                 /* la llama kbd_poll() ~60 veces por segundo (ver kbd.c) */
static const struct wapp *pub_app;
static int basic_running;

static unsigned int api_ms(void)
{
    static u64 t0;
    u64 hz = g_tsc_hz ? g_tsc_hz : 2000000000ull, t = rdtsc();
    if (!t0)
        t0 = t;
    return (unsigned int)((t - t0) / (hz / 1000));
}

static int api_poll(void)                 { return kbd_inkey(); }
static void api_mouse(int *x, int *y, int *b)
{
    kbd_poll();
    *x = vga_mouse_get(0);
    *y = vga_mouse_get(1);
    *b = vga_mouse_get(2);
}
static void api_sleep(int ms)             { delay_ms(ms); }
static void api_sound(int k, int hz, int arg, int ms) { audio_sound(k, hz, arg, ms); }
static int  api_dir(const char *spec, kapi_dircb cb)  { return fs_dir(spec, cb); }
static int  api_read(const char *spec, char *buf, int max, unsigned int *total)
{
    return fs_read(spec, buf, max, total);
}
static int  api_exec(const char *name)    { return mod_exec(name, 0); }
static int  api_load_basic(const char *t, int n) { basic_load_text(t, n); return 0; }
static unsigned int api_seed(void)        { return plat_seed(); }
static int  api_scr_w(void)               { return vga_gui_active() ? vga_main_w() : 0; }
static int  api_scr_h(void)               { return vga_gui_active() ? vga_main_h() : 0; }
/* numeracion de kapi.h: 0 FB0, 1 FB1, 2 FB2, 3 FB3, 4 plano del escritorio (internas 0, 1, 3, 4, 2) */
static u8  *api_layer(int n)
{
    static const int map[5] = { 0, 1, 3, 4, 2 };
    return (n >= 0 && n < 5) ? fb_layer(map[n]) : NULL;
}
static void api_cursor(int on)            { fb_cursor_show(on); }

/* Ejecuta una linea de BASIC como si se hubiera escrito en la consola actual */
static void api_basic_run(const char *line)
{
    char buf[128];
    int i;
    for (i = 0; line[i] && i < 127; i++)
        buf[i] = line[i];
    buf[i] = 0;
    basic_running++;
    if (basic_exec_line(buf)) {
        if (vga_col())
            vga_putc('\n');
        vga_puts("READY.\n");
    }
    basic_running--;
}
static int  api_basic_busy(void)          { return basic_running; }
static void api_set_tick(void (*fn)(void)) { g_gui_tick = fn; }
static void api_wm_publish(const struct wapp *a) { pub_app = a; }
static const struct wapp *api_wm_take(void)      { const struct wapp *a = pub_app; pub_app = 0; return a; }

extern char net_args[];                   /* basic.c: argumento de NET */

const struct kapi kernel_api = {
    KAPI_VER, api_scr_w, api_scr_h, api_poll, api_mouse, api_ms, api_sleep, api_sound, blk_line,
    api_dir, api_read, api_exec, api_load_basic, kbd_esc, api_seed,
    api_layer, fb_dirty, fb_flush, fb_pool, api_cursor,
    vga_con_attach, vga_con_detach, vga_con_resize, vga_con_putc, vga_con_select, vga_con_cursor,
    api_basic_run, api_basic_busy, api_set_tick, api_wm_publish, api_wm_take,
    vga_vid, audio_pcm_play, audio_pcm_stop, audio_pcm_ms, vga_puts,
    (void *(*)(unsigned long long, unsigned))map_mmio, net_args,
    kbd_rawin,
    audio_stream_ctl,
    (int (*)(const char *, int, unsigned long long, unsigned, void *))blk_api
};

static int is_gui(const char *n)
{
    const char *g = "GUI.SYS";
    while (*g && ((*n >= 'a' && *n <= 'z') ? *n - 32 : *n) == *g) { n++; g++; }
    return !*g && !*n;
}

int plat_exec(const char *name)
{
    int r;
    if (!is_gui(name))                          /* modulos de pantalla completa (YAAI...): sin escritorio */
    {
        int rr = mod_exec(name, 0);
        return rr == MOD_WINAPP ? MODERR_BADHDR : rr;
    }
    if (!vga_is_gfx())
        return MODERR_BADHDR;                   /* el escritorio necesita el modo grafico */
    if (vga_gui_active())
        return MODERR_BUSY;                     /* ya estamos dentro del escritorio */
    if (vga_gui(1) < 0)
        return MODERR_NOMEM;
    r = mod_exec(name, 0);
    g_gui_tick = 0;
    vga_con_select(0);
    vga_gui(0);                                 /* volver a la consola con su texto intacto */
    if (r == MOD_WINAPP)
        r = MODERR_BADHDR;                      /* una app de ventana necesita el escritorio abierto */
    return r;
}

/* Direcciones "magicas" del C64: borde, fondo y color de texto */
#define C64_BORDER 53280
#define C64_BG     53281
#define C64_FG     646

/* La memoria mapeada con identidad llega a 1 GB */
#define MEM_LIMIT 0x40000000u

int plat_peek(unsigned int addr)
{
    if (addr == C64_BORDER) return vga_get_border();
    if (addr == C64_BG)     return vga_get_bg();
    if (addr == C64_FG)     return vga_get_fg();
    if (addr >= MEM_LIMIT)  return 0;
    return *(volatile u8 *)(unsigned long)addr;
}

void plat_poke(unsigned int addr, int v)
{
    if (addr == C64_BORDER) { vga_set_border(v); return; }
    if (addr == C64_BG)     { vga_set_bg(v); return; }
    if (addr == C64_FG)     { vga_set_fg(v); return; }
    if (addr >= MEM_LIMIT)  return;
    *(volatile u8 *)(unsigned long)addr = (u8)v;
}

unsigned int plat_seed(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo;
}

static struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) null_idt = { 0, 0 };

void plat_reboot(void)
{
    int i;
    for (i = 0; i < 0x10000 && (inb(0x64) & 2); i++)
        ;
    outb(0x64, 0xFE);                         /* pulso de reset del 8042 */
    __asm__ volatile("lidt %0; int3" : : "m"(null_idt));   /* triple fault */
    for (;;)
        __asm__ volatile("hlt");
}

/* ---- kernel ---- */

void kmain(void)
{
    char line[128];

    int i;

    vga_init();
    vga_splash(0, "INICIANDO...");
    mouse_init();
    vga_splash(15, "DETECTANDO HARDWARE");
    vga_puts("\nPC BASIC V1.2 (64-BIT)\n\n");
    sysinfo_print();
    vga_splash(45, "CARGANDO BASIC");
    basic_init();
    for (i = 50; i <= 100; i += 2) {
        vga_splash(i, i < 80 ? "PREPARANDO SISTEMA" : "LISTO");
        delay_ms(25);
    }
    delay_ms(300);
    vga_splash_end();
    basic_banner();

    for (;;) {
        kbd_readline(line, sizeof line);
        if (basic_exec_line(line)) {
            if (vga_col())
                vga_putc('\n');
            vga_puts("READY.\n");
        }
    }
}
