/* bccrun.c - corre un .BIN de la tienda (de BCC) en Linux, sin BASIC-OS, para probar rapido.
 * uso: bccrun PROGRAMA.BIN       (las rutas HD0.1:/X.TXT se leen como ./X.TXT)
 * Solo imita lo que usa la biblioteca de BCC: consola, teclado (de la entrada estandar), tiempo y archivos. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "kapi.h"

static struct kapi K;

static const char *cp437_utf8(unsigned char c)
{
    static char b[8];
    static const unsigned short hi[128] = {
        0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
        0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x20A7, 0x192,
        0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0x2310, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB };
    unsigned cp = c < 128 ? c : (c - 128 < 48 ? hi[c - 128] : c == 0xF8 ? 0xB0 : c == 0xFA ? 0xB7 : '?');
    if (cp < 128) { b[0] = (char)cp; b[1] = 0; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); b[2] = 0; }
    else { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); b[3] = 0; }
    return b;
}
static void r_print(const char *s) { for (; *s; s++) fputs(cp437_utf8((unsigned char)*s), stdout); fflush(stdout); }
static int r_poll(void)
{
    int c = getchar();
    if (c == EOF) return 0;
    if (c >= 'a' && c <= 'z') return c - 32;        /* el teclado de BASIC-OS da MAYUSCULAS sin Shift */
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c == '\n' ? 13 : c;
}
static int r_esc(void) { return feof(stdin) ? 1 : 0; }
static unsigned r_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
static void r_sleep(int ms) { usleep((useconds_t)ms * 1000); }
static unsigned r_seed(void) { return (unsigned)time(0); }
static int r_scr(void) { return 0; }
static int r_basic_do(const char *s) { printf("[BASIC] %s\n", s); return 0; }
static int r_basic_eval(const char *e, int *v) { (void)e; *v = 0; return 0; }
static void r_sound(int k, int hz, int a, int ms) { printf("[SOUND %d %d %d %d]\n", k, hz, a, ms); }
static void r_mouse(int *x, int *y, int *b) { *x = *y = *b = 0; }
static int r_read(const char *spec, char *buf, int max, unsigned *total)
{
    const char *p = strchr(spec, ':');
    FILE *f;
    long n;
    p = p ? p + 1 : spec;
    while (*p == '/') p++;
    f = fopen(p, "rb");
    if (!f) return -2;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    *total = (unsigned)n;
    if (n > max) n = max;
    n = (long)fread(buf, 1, (size_t)n, f);
    fclose(f);
    return (int)n;
}

int main(int argc, char **argv)
{
    unsigned char *mem = (unsigned char *)0x9000000;
    unsigned *h = (unsigned *)mem;
    FILE *f;
    long n;
    int (*entry)(const struct kapi *);
    if (argc < 2) { fprintf(stderr, "uso: bccrun PROGRAMA.BIN\n"); return 2; }
    if (mmap(mem, 0x2000000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != mem) {
        perror("mmap"); return 1;
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    n = (long)fread(mem, 1, 0x2000000, f);
    fclose(f);
    if (n < 16 || h[0] != 0x4D424350u || h[3] != 0x9000000u) { fprintf(stderr, "no es un .BIN de la tienda\n"); return 1; }
    if (mmap((void *)0xB000000, 0x400000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == MAP_FAILED) { perror("mmap2"); }
    K.ver = KAPI_VER;
    K.print = r_print; K.poll = r_poll; K.esc = r_esc; K.ms = r_ms; K.sleep = r_sleep; K.seed = r_seed;
    K.scr_w = r_scr; K.scr_h = r_scr; K.basic_do = r_basic_do; K.basic_eval = r_basic_eval; K.sound = r_sound;
    K.mouse = r_mouse; K.read = r_read; K.args = "";
    entry = (int (*)(const struct kapi *))(unsigned long)h[2];
    return entry(&K);
}
