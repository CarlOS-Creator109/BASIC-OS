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
void plat_clear(void)                     { vga_clear(); bmg_api(8, 0, 0, 0, 0); }
int  plat_bmg(int op, int a, int b, int c, const char *s) { return bmg_api(op, a, b, c, s); }
int  plat_keydown(int code)               { return kbd_held(code); }

/* CP: lee el archivo entero a bancos de RAM y lo escribe en el destino (BFS) */
int plat_copy(const char *from, const char *to)
{
    unsigned int total = 0, nb;
    char probe[4];
    int r = fs_read(from, probe, 0, &total);
    u64 buf;
    if (r < 0)
        return r;
    nb = (unsigned int)(((u64)total + 0x1FFFFFFull) / 0x2000000ull);
    if (!nb) nb = 1;
    buf = ram_api(RAM_ALLOC, 0, nb, "COPIA");
    if (!buf)
        return FS_ENOSPC;
    r = fs_read(from, (char *)(unsigned long)buf, (int)(total > 0x7FFFFFFFu ? 0x7FFFFFFFu : total), &total);
    if (r >= 0)
        r = fs_write(to, (const void *)(unsigned long)buf, (unsigned int)r);
    ram_api(RAM_FREE, buf, 0, 0);
    return r < 0 ? r : 0;
}
void plat_color(int fg, int bg)           { vga_setcolor(fg, bg); }
int  plat_readline(char *buf, int max)    { return kbd_readline(buf, max); }
int  plat_break(void)                     { return kbd_esc(); }
int  plat_inkey(void)                     { return kbd_inkey(); }
int  plat_mouse(int what)                 { return vga_mouse_get(what); }
int  plat_drive_line(int i, char *out)    { return blk_line(i, out); }
const char *plat_audio_info(void)         { return audio_info(); }
int  plat_pci_line(int i, char *out)      { return pci_line(i, out); }
int  plat_ram_line(int i, char *out)      { return ram_line(i, out); }
int  plat_task_line(int i, char *out)     { return task_line(i, out); }
int  plat_gfx_w(void)                     { return vga_gfx_w(); }
int  plat_gfx_h(void)                     { return vga_gfx_h(); }
void plat_plot(int x, int y, int c)       { vga_plot(x, y, c); }
void plat_hline(int x1, int x2, int y, int c) { vga_hline(x1, x2, y, c); }

/* ---- sonido y tiempo: altavoz del PC (PIT canal 2) y PIT canal 0 como reloj ---- */

#define PIT_HZ 1193182u

static void sleep_ms(int ms, int use_esc)
{
    if (irq_active()) {                       /* con reloj: la CPU duerme entre milisegundos */
        u64 end = irq_ticks + (u64)(unsigned)ms;
        while ((long long)(irq_ticks - end) < 0) {
            audio_service();
            if (use_esc && kbd_esc()) {
                kbd_esc_again();
                break;
            }
            task_yield();
            cpu_idle();
        }
        return;
    }
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

static unsigned int tsc_ms(void);
/* milisegundos desde el arranque. Con interrupciones (64 bits) sale del reloj PIT (irq_ticks): el TSC no sirve de
 * reloj cuando la CPU hace hlt en algunas maquinas (AMD K10 / Turion II con KVM: el TSC se frena al dormir). */
static unsigned int api_ms(void)
{
    if (irq_active())
        return (unsigned int)irq_ticks;
    return tsc_ms();
}

static unsigned int tsc_ms(void)
{
    static u64 t0;
    u64 hz = g_tsc_hz ? g_tsc_hz : 2000000000ull, t = rdtsc();
    if (!t0)
        t0 = t;
    return (unsigned int)((t - t0) / (hz / 1000));
}
unsigned plat_ms_now(void)                { return api_ms(); }

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
static int api_fsop(int op, const char *spec, const void *data, unsigned long long size, const char *spec2)
{
    switch (op) {
    case FSOP_WRITE:  return size > 0xFFFFFFFFull ? FS_E2BIG : fs_write(spec, data, (unsigned int)size);
    case FSOP_MKDIR:  return fs_mkdir(spec);
    case FSOP_REMOVE: return fs_remove(spec);
    case FSOP_RENAME: return spec2 ? fs_rename(spec, spec2) : FS_ENOENT;
    case FSOP_SPACE: {
        unsigned long long *o = (unsigned long long *)data;
        return fs_space(spec, o, o + 1);
    }
    case FSOP_FORMAT: return fs_format(spec, data ? (const char *)data : "BASIC-OS");
    case FSOP_KIND: {
        int r = fs_space(spec, 0, 0), i;
        const char *k = fs_kind();
        if (r && r != FS_EROFS) return r;
        for (i = 0; k[i] && i < 7; i++) ((char *)data)[i] = k[i];
        ((char *)data)[i] = 0;
        return 0;
    }
    }
    return FS_ENOENT;
}
static int  api_read(const char *spec, char *buf, int max, unsigned int *total)
{
    return fs_read(spec, buf, max, total);
}
static int  api_exec(const char *name)    { return mod_exec(name, 0); }
static int  api_load_basic(const char *t, int n) { basic_load_text(t, n); return 0; }
static unsigned int api_seed(void)        { return plat_seed(); }
static int  api_saved(const char *name, char *buf, int max) { return store_load(name, buf, max); }
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
    (int (*)(const char *, int, unsigned long long, unsigned, void *))blk_api,
    api_saved,
    basic_do, basic_eval, vga_col,
    fb_overlay, ram_api, task_api, api_fsop
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
        if (rr != MOD_WINAPP)
            return rr;
        /* el modulo dejo una ventana publicada (p.ej. PKG con un programa de la tienda que abre ventana) */
        if (!pub_app)
            return MODERR_BADHDR;
        if (vga_gui_active())
            return 0;                           /* ya estamos en el escritorio: la terminal que lo corrio la recoge */
        if (!vga_is_gfx() || vga_gui(1) < 0) {
            pub_app = 0;
            return MODERR_BADHDR;
        }
        rr = mod_exec("GUI.SYS", 0);            /* el escritorio la recoge al arrancar (wm_take) */
        g_gui_tick = 0;
        vga_con_select(0);
        vga_gui(0);
        pub_app = 0;
        return rr == MOD_WINAPP ? 0 : rr;
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
    if (!ram_ok(addr))      return 0;
    return *(volatile u8 *)(unsigned long)addr;
}

void plat_poke(unsigned int addr, int v)
{
    if (addr == C64_BORDER) { vga_set_border(v); return; }
    if (addr == C64_BG)     { vga_set_bg(v); return; }
    if (addr == C64_FG)     { vga_set_fg(v); return; }
    if (!ram_ok(addr))      return;
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

/* ---- distribucion del teclado: menu con flechas al arrancar y comando KEYB ----
 * Se guarda en el LBA 514 de la imagen de arranque (hueco libre entre el directorio de SAVE, 512-513,
 * y sus datos, 520+). Si el disco no se puede escribir (CD), solo dura hasta apagar. */
#define KEYB_LBA 514
static const char keyb_magic[8] = "PCBKEY1";

static int keyb_load(void)
{
    u8 sec[512];
    int i;
    if (plat_disk_read(KEYB_LBA, sec))
        return -1;
    for (i = 0; i < 8; i++)
        if (sec[i] != (u8)keyb_magic[i])
            return -1;
    return sec[8] < KB_COUNT ? sec[8] : -1;
}

static int keyb_save(int l)
{
    u8 sec[512];
    int i;
    for (i = 0; i < 512; i++)
        sec[i] = 0;
    for (i = 0; i < 8; i++)
        sec[i] = (u8)keyb_magic[i];
    sec[8] = (u8)l;
    return plat_disk_write(KEYB_LBA, sec);
}

static void keyb_line(int l, int sel)
{
    const char *n = kbd_layout_name(l, 1);
    int k = 0;
    vga_puts(l == sel ? "  > " : "    ");
    if (l == sel)
        vga_putc((char)18);                   /* video inverso */
    vga_putc(' ');
    vga_puts(kbd_layout_name(l, 0));
    vga_puts("  ");
    vga_puts(n);
    while (n[k]) k++;
    for (; k < 24; k++)
        vga_putc(' ');
    if (l == sel)
        vga_putc((char)146);
    vga_puts("   ");
}

static void keyb_count(int secs)
{
    vga_putc('\r');
    if (secs > 0) {
        vga_puts("  ARRANCA SOLO EN ");
        vga_putc((char)('0' + secs / 10 % 10));
        vga_putc((char)('0' + secs % 10));
        vga_puts(" S (CUALQUIER FLECHA LO DETIENE)   ");
    } else
        vga_puts("  ENTER = USAR ESTE   ESC = DEJAR EL QUE ESTABA              ");
}

/* timeout en segundos (0 = esperar). Devuelve la distribucion elegida o -1 si ESC. */
static int keyb_menu(int sel, int timeout)
{
    int l, ticks = timeout * 50, r = -1;
    vga_puts("\nTECLADO / KEYBOARD   (FLECHAS ARRIBA/ABAJO + ENTER)\n\n");
    for (l = 0; l < KB_COUNT; l++) {
        keyb_line(l, sel);
        vga_putc('\n');
    }
    vga_putc('\n');
    keyb_count(timeout);
    kbd_esc();
    for (;;) {
        int c = kbd_inkey();
        if (kbd_esc()) { r = -1; break; }
        if (c == '\n' || c == '\r') { r = sel; break; }
        if (c == 145 || c == 17 || c == 157 || c == 29) {     /* flechas */
            if (c == 145 || c == 157) sel = (sel + KB_COUNT - 1) % KB_COUNT;
            else sel = (sel + 1) % KB_COUNT;
            ticks = 0;
            vga_putc('\r');
            for (l = 0; l < KB_COUNT + 1; l++)
                vga_putc((char)145);
            for (l = 0; l < KB_COUNT; l++) {
                vga_putc('\r');
                keyb_line(l, sel);
                vga_putc((char)17);
            }
            vga_putc((char)17);
            keyb_count(0);
            continue;
        }
        if (c)
            continue;
        if (timeout && ticks > 0) {
            delay_ms(20);
            if (--ticks == 0) { r = sel; break; }
            if (ticks % 50 == 0)
                keyb_count(ticks / 50);
        } else
            delay_ms(10);
    }
    vga_putc('\n');
    return r;
}

static void keyb_apply(int l, int save)
{
    kbd_set_layout(l);
    vga_puts("TECLADO: ");
    vga_puts(kbd_layout_name(l, 1));
    if (save && keyb_save(l))
        vga_puts("  (NO SE PUDO GUARDAR EN EL DISCO)");
    vga_putc('\n');
}

/* al arrancar: el guardado preseleccionado, 8 segundos para cambiarlo */
static void keyb_boot(void)
{
    int saved = keyb_load(), r;
    if (saved >= 0)
        kbd_set_layout(saved);
    r = keyb_menu(kbd_get_layout(), 8);
    if (r < 0)
        r = kbd_get_layout();
    keyb_apply(r, r != saved);
    vga_puts("(PARA CAMBIARLO DESPUES: KEYB, KEYB ES, KEYB LA O KEYB US)\n\n");
}

/* comando KEYB de BASIC: sin argumento abre el menu */
int plat_keyb(const char *arg)
{
    int l, r;
    while (*arg == ' ') arg++;
    if (!*arg) {
        r = keyb_menu(kbd_get_layout(), 0);
        if (r >= 0)
            keyb_apply(r, 1);
        return 0;
    }
    for (l = 0; l < KB_COUNT; l++) {
        const char *n = kbd_layout_name(l, 0);
        if ((arg[0] & ~32) == n[0] && (arg[1] & ~32) == n[1] && (arg[2] == 0 || arg[2] == ' ')) {
            keyb_apply(l, 1);
            return 0;
        }
    }
    return -1;
}

/* ---- kernel ---- */

void kmain(void)
{
    char line[128];

    int i;

    ram_init();
    irq_init();
    vga_init();
    vga_splash(0, "INICIANDO...");
    mouse_init();
    vga_splash(15, "DETECTANDO HARDWARE");
    vga_puts("\nPC BASIC V1.2 (" BITS_STR ")\n\n");
    sysinfo_print();
    vga_splash(45, "CARGANDO BASIC");
    basic_init();
    for (i = 50; i <= 100; i += 2) {
        vga_splash(i, i < 80 ? "PREPARANDO SISTEMA" : "LISTO");
        delay_ms(25);
    }
    delay_ms(300);
    vga_splash_end();
    keyb_boot();
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
