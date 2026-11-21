/* libc.h - la mini biblioteca estandar de BASIC-OS: el puente entre el Kapi y un programa "de Unix".
 *
 * Es el segundo cimiento (el primero son los descriptores de archivo, ver fd.c y kapi.h v11): con esto
 * un programa ya se escribe con malloc, strlen, printf, open/read/write/close... en vez de llamar a mano
 * a K->algo. Es lo que necesitan BCC (tu compilador de C), los drivers de la comunidad y, el dia que le
 * entremos, un port de verdad de algo como nano.
 *
 * Es una cabecera: se compila dentro del programa, no hay que enlazar nada.
 *
 *   #include "sys/ui.h"      // declara K (o declara tu  static const struct kapi *K;)
 *   #include "sys/libc.h"
 *   #include "mod_head.h"
 *   int mod_main(const struct kapi *k) {
 *       K = k;
 *       lc_init();                                  // una vez, antes de usar malloc
 *       int fd = c_open("HD0.1:/NOTAS.TXT", O_RDWR | O_NEW);
 *       c_write(fd, "hola\n", 5);
 *       c_close(fd);
 *       c_printf("escritos %d bytes en %s\n", 5, "NOTAS.TXT");
 *       lc_done();                                  // devuelve la memoria
 *       return MOD_OK;
 *   }
 *
 * Nota: los nombres llevan c_ / lc_ a proposito, para no chocar con los de lib.c del kernel
 * (memcpy, memset) ni con nada que ya tengas.
 */
#ifndef BOS_LIBC_H
#define BOS_LIBC_H
#include "kapi.h"

/* igual que WT_FN en wtk.h: es una biblioteca de cabecera, no todas las funciones se usan siempre */
#define LC_FN static __attribute__((unused))

/* ================================================================ cadenas y memoria */

LC_FN int c_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

LC_FN int c_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

LC_FN int c_strncmp(const char *a, const char *b, int n)
{
    int i = 0;
    for (; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) break;
    }
    return 0;
}

LC_FN char *c_strcpy(char *d, const char *s)
{
    int i = 0;
    while ((d[i] = s[i]) != 0) i++;
    return d;
}

/* copia como mucho max-1 y siempre cierra con 0 (la version segura, no la de C) */
LC_FN char *c_strlcpy(char *d, const char *s, int max)
{
    int i = 0;
    if (max <= 0) return d;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
    return d;
}

LC_FN char *c_strcat(char *d, const char *s)
{
    int i = c_strlen(d), j = 0;
    while ((d[i + j] = s[j]) != 0) j++;
    return d;
}

LC_FN char *c_strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? 0 : (char *)s;
}

LC_FN void *c_memcpy(void *d, const void *s, int n)
{
    unsigned char *a = (unsigned char *)d; const unsigned char *b = (const unsigned char *)s;
    int i; for (i = 0; i < n; i++) a[i] = b[i];
    return d;
}

LC_FN void *c_memset(void *d, int v, int n)
{
    unsigned char *a = (unsigned char *)d;
    int i; for (i = 0; i < n; i++) a[i] = (unsigned char)v;
    return d;
}

LC_FN int c_memcmp(const void *x, const void *y, int n)
{
    const unsigned char *a = (const unsigned char *)x, *b = (const unsigned char *)y;
    int i; for (i = 0; i < n; i++) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

LC_FN int c_atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

/* entero -> texto en base 10 o 16. Devuelve el final de lo escrito. */
LC_FN char *c_itoa(char *o, long v, int base)
{
    char t[24]; int n = 0; unsigned long u;
    if (v < 0 && base == 10) { *o++ = '-'; u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { int d = (int)(u % (unsigned)base); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (unsigned)base; }
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}

/* ================================================================ memoria dinamica
 * malloc de verdad (primer hueco que sirva, con fusion al liberar) sobre un banco que le
 * pedimos al asignador del sistema. lc_init() una vez al arrancar, lc_done() al terminar. */

#define LC_BANK 0x2000000u                  /* 32 MB: el tamano de un banco */

struct lc_blk { unsigned size; unsigned free; struct lc_blk *next; };
static unsigned long long lc_bank;
static struct lc_blk *lc_head;

LC_FN int lc_init(void)
{
    if (lc_bank) return 1;
    if (K->ver < 7 || !K->ram) return 0;
    lc_bank = K->ram(RAM_ALLOC, 0, 1, "LIBC");
    if (!lc_bank) return 0;
    lc_head = (struct lc_blk *)(unsigned long)lc_bank;
    lc_head->size = LC_BANK - (unsigned)sizeof(struct lc_blk);
    lc_head->free = 1;
    lc_head->next = 0;
    return 1;
}

LC_FN void lc_done(void)
{
    if (lc_bank && K->ram) K->ram(RAM_FREE, lc_bank, 0, 0);
    lc_bank = 0;
    lc_head = 0;
}

LC_FN void *c_malloc(int n)
{
    struct lc_blk *b;
    if (n <= 0 || !lc_head) return 0;
    n = (n + 15) & ~15;                                  /* alineado a 16 */
    for (b = lc_head; b; b = b->next) {
        if (!b->free || (int)b->size < n) continue;
        if ((int)b->size >= n + (int)sizeof(struct lc_blk) + 16) {   /* partir el hueco */
            struct lc_blk *r = (struct lc_blk *)((unsigned char *)(b + 1) + n);
            r->size = b->size - (unsigned)n - (unsigned)sizeof(struct lc_blk);
            r->free = 1;
            r->next = b->next;
            b->size = (unsigned)n;
            b->next = r;
        }
        b->free = 0;
        return (void *)(b + 1);
    }
    return 0;
}

LC_FN void c_free(void *p)
{
    struct lc_blk *b, *q;
    if (!p || !lc_head) return;
    b = (struct lc_blk *)p - 1;
    b->free = 1;
    for (q = lc_head; q; q = q->next)                    /* fusionar con el de al lado */
        while (q->free && q->next && q->next->free) {
            q->size += q->next->size + (unsigned)sizeof(struct lc_blk);
            q->next = q->next->next;
        }
}

LC_FN void *c_calloc(int n, int sz)
{
    void *p = c_malloc(n * sz);
    if (p) c_memset(p, 0, n * sz);
    return p;
}

/* ================================================================ archivos (descriptores, v11) */

LC_FN int c_open(const char *spec, int mode)
{
    if (K->ver < 11 || !K->f_open) return FDERR_BADF;
    return K->f_open(spec, mode);
}
LC_FN int c_read(int fd, void *buf, int n)        { return K->f_read(fd, buf, n); }
LC_FN int c_write(int fd, const void *buf, int n) { return K->f_write(fd, buf, n); }
LC_FN int c_seek(int fd, int off, int whence)     { return K->f_seek(fd, off, whence); }
LC_FN int c_close(int fd)                         { return K->f_close(fd); }

/* tamano del archivo abierto, sin perder la posicion */
LC_FN int c_fsize(int fd)
{
    int here = c_seek(fd, 0, SEEK_CUR), end;
    if (here < 0) return here;
    end = c_seek(fd, 0, SEEK_END);
    c_seek(fd, here, SEEK_SET);
    return end;
}

/* ================================================================ salida por consola */

LC_FN void c_puts(const char *s) { K->print(s); }
LC_FN void c_putchar(int c) { char t[2]; t[0] = (char)c; t[1] = 0; K->print(t); }

/* printf chiquito: %d %u %x %s %c %% . Suficiente para programas de verdad. */
LC_FN void c_printf(const char *f, ...)
{
    __builtin_va_list ap;
    char out[512];
    int o = 0;
    __builtin_va_start(ap, f);
    for (; *f && o < (int)sizeof out - 32; f++) {
        if (*f != '%') { out[o++] = *f; continue; }
        f++;
        if (*f == 'd' || *f == 'i') { char t[24]; c_itoa(t, __builtin_va_arg(ap, int), 10); { int i; for (i = 0; t[i]; i++) out[o++] = t[i]; } }
        else if (*f == 'u')         { char t[24]; c_itoa(t, (long)__builtin_va_arg(ap, unsigned), 10); { int i; for (i = 0; t[i]; i++) out[o++] = t[i]; } }
        else if (*f == 'x')         { char t[24]; c_itoa(t, (long)__builtin_va_arg(ap, unsigned), 16); { int i; for (i = 0; t[i]; i++) out[o++] = t[i]; } }
        else if (*f == 's')         { const char *s = __builtin_va_arg(ap, const char *); if (!s) s = "(null)";
                                      while (*s && o < (int)sizeof out - 2) out[o++] = *s++; }
        else if (*f == 'c')         { out[o++] = (char)__builtin_va_arg(ap, int); }
        else if (*f == '%')         { out[o++] = '%'; }
        else                        { out[o++] = '%'; out[o++] = *f; }
    }
    __builtin_va_end(ap);
    out[o] = 0;
    K->print(out);
}

#endif
