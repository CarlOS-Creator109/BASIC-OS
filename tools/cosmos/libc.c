/* libc.c - la libc minima que necesita el motor de Cosmos Browser (html.c, layout.c) dentro de PC BASIC.
 * Memoria: un monticulo en [MM_HEAP, MM_HEAP_END) con listas libres por tamano (potencias de 2): free() devuelve el
 * bloque a su lista y malloc() lo reutiliza, asi re-maquetar al cambiar el tamano de la ventana no se come la memoria.
 * mm_heap_reset() lo vacia todo de un golpe al cargar otra pagina. GPL-3.0-or-later. */
#include "cstub.h"
#include <stdarg.h>

#define MM_HEAP     0x7400000UL
#define MM_HEAP_END 0x8F00000UL
#define NCLASS      26                          /* 2^4 = 16 B ... 2^25 = 32 MB */

static unsigned long heap_at = MM_HEAP;
static void *freel[NCLASS];
int mm_oom;

void mm_heap_reset(void)
{
    int i;
    heap_at = MM_HEAP;
    mm_oom = 0;
    for (i = 0; i < NCLASS; i++)
        freel[i] = 0;
}
unsigned long mm_heap_used(void) { return heap_at - MM_HEAP; }

static int size_class(size_t n)
{
    int c = 4;
    while (c < NCLASS && ((size_t)1 << c) < n)
        c++;
    return c;
}

void *malloc(size_t n)
{
    int c = size_class(n ? n : 1);
    unsigned long p;
    if (c >= NCLASS) { mm_oom = 1; return 0; }
    if (freel[c]) {
        void *r = freel[c];
        freel[c] = *(void **)r;
        return r;
    }
    if (heap_at + 16 + ((size_t)1 << c) > MM_HEAP_END) { mm_oom = 1; return 0; }
    p = heap_at + 16;
    *(size_t *)(p - 16) = (size_t)c;            /* la clase, para free y realloc */
    heap_at = p + ((size_t)1 << c);
    return (void *)p;
}
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); if (p) memset(p, 0, a * b); return p; }
void free(void *p)
{
    int c;
    if (!p || (unsigned long)p < MM_HEAP || (unsigned long)p >= MM_HEAP_END)
        return;
    c = (int)*(size_t *)((char *)p - 16);
    if (c < 4 || c >= NCLASS)
        return;
    *(void **)p = freel[c];
    freel[c] = p;
}
void *realloc(void *p, size_t n)
{
    void *q;
    size_t cap;
    if (!p) return malloc(n);
    cap = (size_t)1 << *(size_t *)((char *)p - 16);
    if (n <= cap) return p;
    q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, cap);
    free(p);
    return q;
}
char *strdup(const char *s) { size_t n = strlen(s); char *p = malloc(n + 1); if (p) memcpy(p, s, n + 1); return p; }

void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    char *a = d; const char *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return 0;
}
char *strcasestr(const char *h, const char *n)
{
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
    return 0;
}
size_t strcspn(const char *s, const char *rej)
{
    size_t n = 0;
    while (s[n] && !strchr(rej, s[n])) n++;
    return n;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strtok_r(char *s, const char *delim, char **save)
{
    char *e;
    if (!s) s = *save;
    if (!s) return 0;
    while (*s && strchr(delim, *s)) s++;
    if (!*s) { *save = 0; return 0; }
    e = s;
    while (*e && !strchr(delim, *e)) e++;
    if (*e) { *e = 0; *save = e + 1; } else *save = 0;
    return s;
}
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int ispunct(int c) { return c > 32 && c < 127 && !isalnum(c); }
int abs(int v) { return v < 0 ? -v : v; }

long strtol(const char *s, char **end, int base)
{
    long v = 0;
    int neg = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    if (!base) base = 10;
    for (;; s++) {
        int d = isdigit((unsigned char)*s) ? *s - '0' : isalpha((unsigned char)*s) ? tolower((unsigned char)*s) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
double strtod(const char *s, char **end)
{
    double v = 0, f = 0.1;
    int neg = 0, any = 0;
    const char *st = s;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (isdigit((unsigned char)*s)) { v = v * 10 + (*s++ - '0'); any = 1; }
    if (*s == '.') { s++; while (isdigit((unsigned char)*s)) { v += (*s++ - '0') * f; f *= 0.1; any = 1; } }
    if (any && (*s == 'e' || *s == 'E') && (isdigit((unsigned char)s[1]) || ((s[1] == '-' || s[1] == '+') && isdigit((unsigned char)s[2])))) {
        int e = 0, en = 0;
        s++;
        if (*s == '-') { en = 1; s++; } else if (*s == '+') s++;
        while (isdigit((unsigned char)*s)) e = e * 10 + (*s++ - '0');
        while (e--) v = en ? v / 10 : v * 10;
    }
    if (end) *end = (char *)(any ? s : st);
    return neg ? -v : v;
}
float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, 0); }
double fabs(double x) { return x < 0 ? -x : x; }
float fabsf(float x) { return x < 0 ? -x : x; }
double floor(double x) { long long i = (long long)x; return (double)(i > x ? i - 1 : i); }
double ceil(double x) { long long i = (long long)x; return (double)(i < x ? i + 1 : i); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float roundf(float x) { return (float)floor(x + 0.5); }
double sqrt(double x) { return __builtin_sqrt(x); }

void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    char *b = base, t;
    size_t i, j, k;
    for (i = 1; i < n; i++)                     /* insercion: las listas del motor son cortas */
        for (j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--)
            for (k = 0; k < sz; k++) { t = b[(j - 1) * sz + k]; b[(j - 1) * sz + k] = b[j * sz + k]; b[j * sz + k] = t; }
}

/* snprintf: %s %d %i %u %x %c %% y %.Ns (lo que usa el motor) */
int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    size_t n = 0;
    va_start(ap, fmt);
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (; *fmt; fmt++) {
        int prec = -1;
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else { prec = 0; while (isdigit((unsigned char)*fmt)) prec = prec * 10 + (*fmt++ - '0'); }
        }
        while (*fmt == 'l' || *fmt == 'z') fmt++;
        if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            for (; *s && prec != 0; s++, prec--) PUT(*s);
        } else if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x') {
            char t[24];
            int k = 0;
            unsigned long long u;
            if (*fmt == 'd' || *fmt == 'i') {
                long long v = va_arg(ap, int);
                if (v < 0) { PUT('-'); v = -v; }
                u = (unsigned long long)v;
            } else u = va_arg(ap, unsigned);
            do { int d = (int)(u % (*fmt == 'x' ? 16 : 10)); t[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); u /= (*fmt == 'x' ? 16 : 10); } while (u);
            while (k) PUT(t[--k]);
        } else if (*fmt == 'c') {
            PUT((char)va_arg(ap, int));
        } else if (*fmt == '%') {
            PUT('%');
        } else if (!*fmt) break;
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    va_end(ap);
    return (int)n;
#undef PUT
}
