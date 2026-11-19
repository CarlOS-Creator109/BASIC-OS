/* bcclib.c - la biblioteca de C de BCC. Se compila con BCC junto con cada programa (solo entra lo que se usa).
 * Habla con el kernel por __kcall(__KO_campo, ...), que llama a la tabla struct kapi con la convencion SysV. */
#include <bcc.h>

void *__K;                                   /* la tabla del kernel: el arranque la deja aqui */
extern char __bcc_end[];                     /* primer byte libre despues del programa (y de su pila) */
int errno;

/* ---------------- consola ---------------- */
/* UTF-8 -> CP437 (la fuente del sistema) para U+00A0..U+00FF */
static unsigned char __lat[96] = {
    32, 173, 155, 156, 36, 157, 124, 21, 34, 99, 166, 174, 170, 45, 114, 45,
    248, 241, 253, 51, 39, 230, 20, 250, 44, 49, 167, 175, 172, 171, 51, 168,
    65, 65, 65, 65, 142, 143, 146, 128, 69, 144, 69, 69, 73, 73, 73, 73,
    68, 165, 79, 79, 79, 79, 153, 120, 79, 85, 85, 85, 154, 89, 80, 225,
    133, 160, 131, 97, 132, 134, 145, 135, 138, 130, 136, 137, 141, 161, 140, 139,
    100, 164, 149, 162, 147, 111, 148, 246, 111, 151, 163, 150, 129, 121, 112, 152
};

static char __obuf[260];
static int __on;

static void __flush(void)
{
    if (__on) {
        __obuf[__on] = 0;
        __kcall(__KO_print, __obuf);
        __on = 0;
    }
}

static int __u8need, __u8cp;
static void __outc(int c)
{
    c &= 255;
    if (__u8need) {                             /* continuacion de un caracter UTF-8 */
        if ((c & 0xC0) == 0x80) {
            __u8cp = (__u8cp << 6) | (c & 63);
            if (--__u8need) return;
            c = __u8cp >= 0xA0 && __u8cp <= 0xFF ? __lat[__u8cp - 0xA0] :
                __u8cp == 0x2022 ? 7 : __u8cp == 0x2014 || __u8cp == 0x2013 ? 45 : __u8cp == 0x2026 ? 250 : 63;
        } else
            __u8need = 0;
    } else if (c >= 0xC2 && c <= 0xDF) { __u8need = 1; __u8cp = c & 31; return; }
    else if (c >= 0xE0 && c <= 0xEF) { __u8need = 2; __u8cp = c & 15; return; }
    if (c == 0) c = 32;
    __obuf[__on++] = (char)c;
    if (c == 10 || __on >= 256) __flush();
}

void bos_print(const char *s) { __flush(); __kcall(__KO_print, s); }
int bos_inkey(void) { __flush(); return (int)__kcall(__KO_poll); }
int bos_esc(void) { return (int)__kcall(__KO_esc); }
unsigned bos_ms(void) { return (unsigned)__kcall(__KO_ms); }
void bos_sleep(int ms) { __flush(); __kcall(__KO_sleep, ms); }
int bos_basic(const char *stmt) { __flush(); return (int)__kcall(__KO_basic_do, stmt); }
int bos_eval(const char *e, int *v) { return (int)__kcall(__KO_basic_eval, e, v); }
void bos_sound(int kind, int hz, int arg, int ms) { __kcall(__KO_sound, kind, hz, arg, ms); }
int bos_scr_w(void) { return (int)__kcall(__KO_scr_w); }
int bos_scr_h(void) { return (int)__kcall(__KO_scr_h); }
void bos_mouse(int *x, int *y, int *b) { __kcall(__KO_mouse, x, y, b); }
unsigned bos_seed(void) { return (unsigned)__kcall(__KO_seed); }
static int __kver(void) { return *(unsigned *)((char *)__K + __KO_ver); }
int bos_tarea(void (*fn)(void *), void *arg, const char *n) { if (__kver() < 8) return -1; return (int)__kcall(__KO_task, 0, fn, arg, n); }
void bos_ceder(void) { __flush(); if (__kver() >= 8) __kcall(__KO_task, 1, 0, 0, 0); else __kcall(__KO_poll); }
void bos_dormir(int ms) { __flush(); if (__kver() >= 8) __kcall(__KO_task, 2, 0, (long)ms, 0); else __kcall(__KO_sleep, ms); }
int bos_tarea_matar(int n) { return __kver() >= 8 ? (int)__kcall(__KO_task, 4, 0, (long)n, 0) : -1; }
int bos_tarea_actual(void) { return __kver() >= 8 ? (int)__kcall(__KO_task, 6, 0, 0, 0) : 0; }
/* al terminar el programa: sus tareas no pueden seguir (su codigo se va a borrar) */
void __bcc_fin(void) { __flush(); if (__K && __kver() >= 8) __kcall(__KO_task, 5, (void *)0x9000000, (void *)0xB000000, 0); }
void *bos_ram(unsigned n) { return (void *)__kcall(__KO_ram, 0, 0, n, "BCC"); }
int bos_ram_free(void *p) { return (int)__kcall(__KO_ram, 1, p, 0, 0); }
unsigned bos_ram_info(int w) { unsigned long v = (unsigned long)__kcall(__KO_ram, 3, 0, w == 3 ? 3 : w, 0); return w == 3 ? (unsigned)(v >> 20) : (unsigned)v; }
const char *bos_args(void) { char **p = (char **)((char *)__K + __KO_args); return *p ? *p : ""; }

int bos_key(void)
{
    int c;
    __flush();
    for (;;) {
        c = (int)__kcall(__KO_poll);
        if (c) return c;
        if (__kcall(__KO_esc)) return 27;
        __kcall(__KO_sleep, 5);
    }
}

static int __swapcase(int c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c == 164) return 165;
    if (c == 165) return 164;
    if (c == 135) return 128;
    if (c == 128) return 135;
    return c;
}

/* el sistema da MAYUSCULAS sin Shift (por BASIC): aqui se invierte para que se escriba normal */
int bos_readline(char *buf, int max)
{
    int n = 0, c;
    char e[4];
    __flush();
    __kcall(__KO_esc);
    for (;;) {
        c = (int)__kcall(__KO_poll);
        if (!c) {
            if (__kcall(__KO_esc)) { buf[0] = 0; __kcall(__KO_print, "\n"); return -1; }
            __kcall(__KO_sleep, 5);
            continue;
        }
        if (c == 13 || c == 10) { buf[n] = 0; __kcall(__KO_print, "\n"); return n; }
        if (c == 8 || c == 127 || c == 20) {
            if (n) { n--; __kcall(__KO_print, "\b \b"); }
            continue;
        }
        c = __swapcase(c);
        if ((c >= 32 && c < 127) || c >= 128) {
            if (c == KEY_UP || c == KEY_LEFT) continue;
            if (n < max - 1) {
                buf[n++] = (char)c;
                e[0] = (char)c; e[1] = 0;
                __kcall(__KO_print, e);
            }
        }
    }
}

int bos_read_file(const char *spec, char *buf, int max)
{
    unsigned total = 0;
    return (int)__kcall(__KO_read, spec, buf, max, &total);
}

/* ---------------- memoria ---------------- */
typedef struct __blk { unsigned long size; struct __blk *next; } __blk;   /* 16 bytes antes de cada bloque */
static char *__hp, *__hend;
static __blk *__free;

void *malloc(size_t n)
{
    __blk *b, **pp;
    if (!__hp) { __hp = __bcc_end; __hend = (char *)0xB000000; }
    n = (n + 15) & ~15UL;
    if (!n) n = 16;
    for (pp = &__free; *pp; pp = &(*pp)->next) {
        b = *pp;
        if (b->size >= n && b->size <= n * 2 + 64) {
            *pp = b->next;
            return (char *)b + 16;
        }
    }
    if (__hp + n + 16 > __hend) { errno = 12; return NULL; }
    b = (__blk *)__hp;
    b->size = n;
    __hp += n + 16;
    return (char *)b + 16;
}

void free(void *p)
{
    __blk *b;
    if (!p) return;
    b = (__blk *)((char *)p - 16);
    b->next = __free;
    __free = b;
}

void *calloc(size_t n, size_t size)
{
    void *p = malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    __blk *b;
    void *q;
    if (!p) return malloc(n);
    b = (__blk *)((char *)p - 16);
    if (b->size >= n) return p;
    q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

/* ---------------- cadenas ---------------- */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return p - s; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n)
{
    char *p = d + strlen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower(*a) == tolower(*b)) a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}
char *strrchr(const char *s, int c)
{
    const char *r = NULL;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}
char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, l)) return (char *)h;
    return NULL;
}
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strndup(const char *s, size_t n)
{
    size_t l = 0;
    char *p;
    while (l < n && s[l]) l++;
    p = malloc(l + 1);
    if (p) { memcpy(p, s, l); p[l] = 0; }
    return p;
}
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *r) { size_t n = 0; while (s[n] && !strchr(r, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { s += strcspn(s, a); return *s ? (char *)s : NULL; }
char *strtok(char *s, const char *d)
{
    static char *last;
    char *t;
    if (!s) s = last;
    if (!s) return NULL;
    s += strspn(s, d);
    if (!*s) { last = NULL; return NULL; }
    t = s;
    s += strcspn(s, d);
    if (*s) { *s = 0; last = s + 1; } else last = NULL;
    return t;
}
char *strerror(int e) { (void)e; return "error"; }

void *memcpy(void *d, const void *s, size_t n)
{
    char *a = d;
    const char *b = s;
    while (n >= 8) { *(long *)a = *(long *)b; a += 8; b += 8; n -= 8; }
    while (n--) *a++ = *b++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    char *a = d;
    const char *b = s;
    if (a == b || !n) return d;
    if (a < b || a >= b + n) return memcpy(d, s, n);
    while (n--) a[n] = b[n];
    return d;
}
void *memset(void *d, int c, size_t n)
{
    unsigned char *a = d;
    unsigned long v = (unsigned char)c;
    v |= v << 8; v |= v << 16; v |= v << 32;
    while (n >= 8) { *(unsigned long *)a = v; a += 8; n -= 8; }
    while (n--) *a++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p;
    return NULL;
}

/* ---------------- ctype ---------------- */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ---------------- numeros ---------------- */
int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;
    int neg = 0, any = 0;
    while (isspace(*s)) s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    for (;; s++) {
        int d;
        if (isdigit(*s)) d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        any = 1;
    }
    (void)any;
    if (end) *end = (char *)s;
    return neg ? -v : v;
}
long strtol(const char *s, char **end, int base) { return (long)strtoul(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

static unsigned long __rnd = 1;
int rand(void) { __rnd = __rnd * 1103515245 + 12345; return (int)((__rnd >> 16) & 32767); }
void srand(unsigned s) { __rnd = s; }

static void __swapb(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    size_t i, last;
    if (n < 2) return;
    __swapb(b, b + (n / 2) * size, size);
    last = 0;
    for (i = 1; i < n; i++)
        if (cmp(b + i * size, b) < 0) { last++; __swapb(b + last * size, b + i * size, size); }
    __swapb(b, b + last * size, size);
    qsort(b, last, size, cmp);
    qsort(b + (last + 1) * size, n - last - 1, size, cmp);
}
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    const char *b = base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = cmp(key, b + mid * size);
        if (!r) return (void *)(b + mid * size);
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
char *getenv(const char *name) { (void)name; return NULL; }

void exit(int code) { __bcc_fin(); __builtin_exit(code); }
void abort(void) { bos_print("\nabort()\n"); exit(1); }
void __bcc_assert(const char *what, const char *file, int line)
{
    printf("\nassert fallo: %s (%s:%d)\n", what, file, line);
    exit(1);
}

clock_t clock(void) { return (clock_t)bos_ms(); }
time_t time(time_t *t) { time_t v = bos_ms() / 1000; if (t) *t = v; return v; }

/* ---------------- printf ---------------- */
typedef struct { char *buf; long pos, max; int con; } __out;

static void __put(__out *o, int c)
{
    if (o->con) { __outc(c); o->pos++; return; }
    if (o->pos + 1 < o->max) o->buf[o->pos] = (char)c;
    o->pos++;
}

static void __fmt_num(__out *o, unsigned long v, int neg, int base, int upper, int width, int prec, int left, int zero, char sign, int alt)
{
    char t[72];
    int n = 0, len, pad, i;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0 && prec != 0) t[n++] = '0';
    while (v) { t[n++] = dig[v % base]; v /= base; }
    while (n < prec) t[n++] = '0';
    if (alt && base == 8 && (n == 0 || t[n - 1] != '0')) t[n++] = '0';
    len = n + (neg || sign ? 1 : 0) + (alt && base == 16 ? 2 : 0);
    pad = width > len ? width - len : 0;
    if (!left && !zero) for (i = 0; i < pad; i++) __put(o, ' ');
    if (neg) __put(o, '-'); else if (sign) __put(o, sign);
    if (alt && base == 16) { __put(o, '0'); __put(o, upper ? 'X' : 'x'); }
    if (!left && zero) for (i = 0; i < pad; i++) __put(o, '0');
    while (n) __put(o, t[--n]);
    if (left) for (i = 0; i < pad; i++) __put(o, ' ');
}

static int __vformat(__out *o, const char *f, va_list ap)
{
    for (; *f; f++) {
        int left = 0, zero = 0, alt = 0, width = 0, prec = -1, lng = 0;
        char sign = 0;
        if (*f != '%') { __put(o, *f); continue; }
        f++;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else if (*f == '+') sign = '+';
            else if (*f == ' ') { if (!sign) sign = ' '; }
            else if (*f == '#') alt = 1;
            else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (isdigit(*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (isdigit(*f)) prec = prec * 10 + (*f++ - '0');
        }
        for (;; f++) {
            if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'L' || *f == 'q') lng = 1;
            else if (*f == 'h') { if (lng <= 0) lng = lng == -1 ? -2 : -1; }
            else break;
        }
        if (prec >= 0) zero = 0;
        switch (*f) {
        case 'd': case 'i': {
            long v = va_arg(ap, long);
            if (lng == 0) v = (int)v; else if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            __fmt_num(o, v < 0 ? -(unsigned long)v : (unsigned long)v, v < 0, 10, 0, width, prec, left, zero, sign, 0);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            unsigned long v = va_arg(ap, unsigned long);
            if (lng == 0) v = (unsigned)v; else if (lng == -1) v = (unsigned short)v; else if (lng == -2) v = (unsigned char)v;
            __fmt_num(o, v, 0, *f == 'u' ? 10 : *f == 'o' ? 8 : *f == 'b' ? 2 : 16, *f == 'X', width, prec, left, zero, 0, alt);
            break;
        }
        case 'p': {
            void *p = va_arg(ap, void *);
            if (!p) { const char *z = "(nil)"; while (*z) __put(o, *z++); break; }
            __fmt_num(o, (unsigned long)p, 0, 16, 0, width, prec, left, zero, 0, 1);
            break;
        }
        case 'c': {
            int i;
            if (!left) for (i = 1; i < width; i++) __put(o, ' ');
            __put(o, va_arg(ap, int));
            if (left) for (i = 1; i < width; i++) __put(o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0, i;
            if (!s) s = "(null)";
            while (s[n] && (prec < 0 || n < prec)) n++;
            if (!left) for (i = n; i < width; i++) __put(o, ' ');
            for (i = 0; i < n; i++) __put(o, s[i]);
            if (left) for (i = n; i < width; i++) __put(o, ' ');
            break;
        }
        case 'f': case 'g': case 'e':
            va_arg(ap, long);
            __put(o, '?');
            break;
        case '%': __put(o, '%'); break;
        case 0: f--; break;
        default: __put(o, '%'); __put(o, *f);
        }
    }
    if (!o->con && o->max > 0) o->buf[o->pos < o->max ? o->pos : o->max - 1] = 0;
    return (int)o->pos;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { __out o; o.buf = buf; o.pos = 0; o.max = (long)n; o.con = 0; return __vformat(&o, fmt, ap); }
int vsprintf(char *buf, const char *fmt, va_list ap) { return vsnprintf(buf, 0x7fffffff, fmt, ap); }
int vprintf(const char *fmt, va_list ap) { __out o; o.buf = 0; o.pos = 0; o.max = 0; o.con = 1; return __vformat(&o, fmt, ap); }
int printf(const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vprintf(fmt, ap); va_end(ap); __flush(); return r; }
int sprintf(char *buf, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vsprintf(buf, fmt, ap); va_end(ap); return r; }
int snprintf(char *buf, size_t n, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vsnprintf(buf, n, fmt, ap); va_end(ap); return r; }
int putchar(int c) { __outc(c); return c; }
int puts(const char *s) { while (*s) __outc(*s++); __outc('\n'); return 0; }

/* ---------------- archivos (solo lectura: se leen enteros a la memoria) ---------------- */
struct __bcc_file { char *data; long len, pos; int con, eof, unget; };
static struct __bcc_file __stdin = { 0, 0, 0, 1, 0, -1 }, __stdout = { 0, 0, 0, 2, 0, -1 }, __stderr = { 0, 0, 0, 2, 0, -1 };
FILE *stdin = &__stdin, *stdout = &__stdout, *stderr = &__stderr;

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (f->con == 2) { int r = vprintf(fmt, ap); __flush(); return r; }
    return -1;
}
int fprintf(FILE *f, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }
int fputc(int c, FILE *f) { if (f->con == 2) { __outc(c); return c; } return EOF; }
int putc(int c, FILE *f) { return fputc(c, f); }
int fputs(const char *s, FILE *f) { if (f->con == 2) { while (*s) __outc(*s++); return 0; } return EOF; }
size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    const char *s = p;
    size_t i;
    if (f->con != 2) return 0;
    for (i = 0; i < size * n; i++) __outc(s[i]);
    return n;
}
int fflush(FILE *f) { (void)f; __flush(); return 0; }

static char __line[512];
static int __lpos = -1;
int getchar(void)
{
    int c;
    if (__stdin.unget >= 0) { c = __stdin.unget; __stdin.unget = -1; return c; }
    if (__lpos < 0) {
        int n = bos_readline(__line, 510);
        if (n < 0) return EOF;
        __line[n] = '\n';
        __line[n + 1] = 0;
        __lpos = 0;
    }
    c = (unsigned char)__line[__lpos++];
    if (c == '\n') __lpos = -1;
    return c;
}

FILE *fopen(const char *name, const char *mode)
{
    FILE *f;
    char *buf;
    unsigned total = 0;
    int r;
    if (mode[0] != 'r') { errno = 30; return NULL; }   /* todavia no se puede escribir en archivos */
    buf = malloc(65536);
    if (!buf) return NULL;
    r = (int)__kcall(__KO_read, name, buf, 65536, &total);
    if (r < 0) { free(buf); errno = 2; return NULL; }
    if (total > 65536) {
        free(buf);
        buf = malloc(total + 1);
        if (!buf) return NULL;
        r = (int)__kcall(__KO_read, name, buf, total, &total);
        if (r < 0) { free(buf); return NULL; }
    }
    f = calloc(1, sizeof *f);
    f->data = buf; f->len = r; f->pos = 0; f->unget = -1;
    return f;
}
int fclose(FILE *f) { if (f && !f->con) { free(f->data); free(f); } return 0; }
int fgetc(FILE *f)
{
    if (f->unget >= 0) { int c = f->unget; f->unget = -1; return c; }
    if (f->con == 1) return getchar();
    if (f->pos >= f->len) { f->eof = 1; return EOF; }
    return (unsigned char)f->data[f->pos++];
}
int getc(FILE *f) { return fgetc(f); }
int ungetc(int c, FILE *f) { f->unget = c; f->eof = 0; return c; }
char *fgets(char *buf, int n, FILE *f)
{
    int i = 0, c;
    if (n <= 0) return NULL;
    while (i < n - 1) {
        c = fgetc(f);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return i ? buf : NULL;
}
size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    char *d = p;
    size_t want = size * n, i;
    if (f->con) return 0;
    for (i = 0; i < want && f->pos < f->len; i++) d[i] = f->data[f->pos++];
    if (i < want) f->eof = 1;
    return size ? i / size : 0;
}
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { (void)f; return 0; }
int fseek(FILE *f, long off, int whence)
{
    long p = whence == SEEK_SET ? off : whence == SEEK_CUR ? f->pos + off : f->len + off;
    if (p < 0 || p > f->len) return -1;
    f->pos = p; f->eof = 0; f->unget = -1;
    return 0;
}
long ftell(FILE *f) { return f->pos; }
void rewind(FILE *f) { fseek(f, 0, SEEK_SET); }
void perror(const char *s) { printf("%s: error\n", s ? s : ""); }

/* scanf minimo: %d %i %u %x %s %c %ld */
static int __vscan(const char *s, const char *fmt, va_list ap)
{
    int n = 0;
    while (*fmt) {
        if (isspace(*fmt)) { while (isspace(*s)) s++; fmt++; continue; }
        if (*fmt != '%') { if (*s != *fmt) return n; s++; fmt++; continue; }
        fmt++;
        {
            int lng = 0;
            while (*fmt == 'l' || *fmt == 'h') { if (*fmt == 'l') lng = 1; fmt++; }
            if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x') {
                char *e;
                long v;
                while (isspace(*s)) s++;
                v = strtol(s, &e, *fmt == 'x' ? 16 : *fmt == 'i' ? 0 : 10);
                if (e == s) return n;
                s = e;
                if (lng) *va_arg(ap, long *) = v; else *va_arg(ap, int *) = (int)v;
                n++;
            } else if (*fmt == 's') {
                char *d = va_arg(ap, char *);
                while (isspace(*s)) s++;
                if (!*s) return n;
                while (*s && !isspace(*s)) *d++ = *s++;
                *d = 0;
                n++;
            } else if (*fmt == 'c') {
                if (!*s) return n;
                *va_arg(ap, char *) = *s++;
                n++;
            }
            fmt++;
        }
    }
    return n;
}
int sscanf(const char *s, const char *fmt, ...) { va_list ap; int r; va_start(ap, fmt); r = __vscan(s, fmt, ap); va_end(ap); return r; }
int scanf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int r, n;
    __flush();
    n = bos_readline(buf, 256);
    if (n < 0) return EOF;
    va_start(ap, fmt);
    r = __vscan(buf, fmt, ap);
    va_end(ap);
    return r;
}
