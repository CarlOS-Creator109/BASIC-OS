/* host.c - arnes de pruebas: corre basic.c en el host leyendo lineas de stdin */
#include <stdio.h>
#include <string.h>
#include "../basic.h"
#include "../store.h"
#include "../fs.h"

static int column;

void plat_putc(char c)
{
    putchar(c);
    if (c == '\n') column = 0; else column++;
}
int  plat_col(void)                       { return column; }
void plat_clear(void)                     { puts("[CLS]"); column = 0; }
void plat_color(int fg, int bg)           { printf("[COLOR %d,%d]", fg, bg); }
int  plat_break(void)                     { return 0; }
void plat_reboot(void)                    { puts("[REBOOT]"); }
int  plat_peek(unsigned int a)            { return (int)(a & 0xFF); }
void plat_poke(unsigned int a, int v)     { printf("[POKE %u,%d]", a, v); }
unsigned int plat_seed(void)              { return 0; }
int  plat_inkey(void)                     { return 0; }
const char *plat_audio_info(void)         { return "AUDIO: HOST"; }
int  plat_mouse(int w)                    { return w + 7; }
int  plat_drive_line(int i, char *o)      { (void)i; (void)o; return 0; }
int  fs_dir(const char *s, fs_cb cb)      { (void)s; (void)cb; return FS_ENODEV; }
int  fs_read(const char *s, char *b, int m, unsigned int *t) { (void)s; (void)b; (void)m; (void)t; return FS_ENODEV; }
int  plat_pci_line(int i, char *o)        { (void)i; (void)o; return 0; }
void plat_beep(int ms, int hz)            { printf("[BEEP %d,%d]", ms, hz); }
int  plat_sound(int k, int h, int a, int m)  { printf("[SOUND %d,%d,%d,%d]", k, h, a, m); return 1; }
void plat_sndwait(void)                   { printf("[SNDWAIT]"); }
int  plat_cd_toc(int i, char *o)          { (void)i; (void)o; return -1; }
int  plat_cd_play(int t)                  { (void)t; return -1; }
void plat_cd_stop(void)                   { printf("[CDSTOP]"); }
int  plat_cd_eject(void)                  { return -1; }
int  plat_cd_status(int *t, int *s)       { *t = 0; *s = 0; return 0; }
int  plat_exec(const char *n)             { (void)n; return -1; }
void plat_locate(int c, int r)            { printf("[LOCATE %d,%d]", c, r); }
void plat_sleep(int ms)                   { printf("[SLEEP %d]", ms); }
int  plat_gfx_w(void)                     { return 64; }
int  plat_gfx_h(void)                     { return 32; }
void plat_plot(int x, int y, int c)
{
    if (x >= 0 && x < 64 && y >= 0 && y < 32) printf("[P %d,%d,%d]", x, y, c);
}
void plat_hline(int x1, int x2, int y, int c) { printf("[H %d,%d,%d,%d]", x1, x2, y, c); }

static unsigned char fake_disk[4096 * 512];
int plat_disk_read(unsigned int lba, void *b)  { memcpy(b, fake_disk + lba * 512, 512); return 0; }
int plat_disk_write(unsigned int lba, const void *b) { memcpy(fake_disk + lba * 512, b, 512); return 0; }

static char *in_line(char *buf, int max)
{
    return fgets(buf, max, stdin);
}

int plat_readline(char *buf, int max)
{
    if (!in_line(buf, max)) { buf[0] = 0; return 0; }
    buf[strcspn(buf, "\r\n")] = 0;
    column = 0;
    return (int)strlen(buf);
}

int main(void)
{
    char line[128];
    basic_init();
    while (in_line(line, sizeof line)) {
        line[strcspn(line, "\r\n")] = 0;
        basic_exec_line(line);
    }
    return 0;
}
