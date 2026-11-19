/* bcc.c - BCC, el compilador de C de BASIC-OS (BASIC-OS COMPILER C)
 *
 * Compila un subconjunto grande de C a un programa .BIN de la tienda (cabecera PCBM, se carga en 0x9000000)
 * escribiendo el codigo x86-64 directamente, sin ensamblador ni enlazador. El mismo archivo se compila dos veces:
 *   - en Linux (tools/bcc/bcc):     bcc hola.c -o HOLA.BIN
 *   - en BASIC-OS (store/BCC.BIN):  tools/bcc/bccos.c lo incluye y le pone la interfaz de la consola
 *
 * Lo que entiende: char/short/int/long (con y sin signo), punteros, arreglos, struct/union, enum, typedef,
 * punteros a funcion, funciones variadicas (stdarg), static, inicializadores (tambien {.campo = ...}),
 * if/while/do/for/switch/goto, el preprocesador (#include, #define con y sin argumentos, # y ##, #if/#ifdef...).
 * Lo que NO entiende (todavia): float/double, pasar o devolver structs por valor, bitfields, VLA.
 *
 * Convencion de llamada propia: los argumentos van en la pila (el primero mas abajo, 8 bytes cada uno) y el
 * resultado en rax; asi las funciones variadicas son triviales. Para hablar con el kernel (struct kapi, convencion
 * SysV) esta la intrinseca __kcall(OFFSET, ...) y las funciones marcadas __callback reciben argumentos SysV.
 * La biblioteca (printf, malloc, string.h, ctype.h, archivos de solo lectura...) esta escrita en C y viaja dentro
 * del compilador (bcclib.h): se compila junto con cada programa.
 *
 * Licencia: GPL-2.0-or-later (como el resto de BASIC-OS).
 */

/* ======================= plataforma (la pone bcc_host.c o bccos.c) ======================= */
/* lee un archivo entero; devuelve NULL si no existe. *len = bytes. */
static char *plat_read(const char *path, int *len);
/* mensajes de error y avisos */
static void plat_msg(const char *s);

typedef unsigned char u8;
typedef unsigned long long u64;
typedef long long i64;

#include "kapi.h"
#include "bcclib.h"

#define BCC_VERSION "0.1"
#define LOAD_BASE  0x9000000ull
#define STACK_SIZE 0x100000                 /* pila propia del programa: 1 MB en su .bss */
#define HEAP_END   0xB000000ull             /* el monticulo va del final del programa hasta aqui */

/* ======================= utilidades ======================= */
static char *arena, *arena_end;
static void fatal(const char *msg);

static void *zalloc(long n)
{
    char *p;
    n = (n + 15) & ~15L;
    if (arena + n > arena_end)
        fatal("BCC se quedo sin memoria");
    p = arena;
    arena += n;
    for (long i = 0; i < n; i++)
        p[i] = 0;
    return p;
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void mcopy(void *d, const void *s, long n) { char *a = d; const char *b = s; while (n-- > 0) *a++ = *b++; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static int sneq(const char *a, int n, const char *b)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i] || !b[i]) return 0;
    return b[n] == 0;
}
static int memeq(const char *a, const char *b, int n) { while (n-- > 0) if (*a++ != *b++) return 0; return 1; }
static int pfx(const char *p, const char *s) { while (*s) if (*p++ != *s++) return 0; return 1; }
static char *sdup(const char *s, int n)
{
    char *p = zalloc(n + 1);
    mcopy(p, s, n);
    p[n] = 0;
    return p;
}
static char *sfmt_num(char *o, i64 v)
{
    char t[24];
    int n = 0, neg = v < 0;
    u64 u = neg ? (u64)-v : (u64)v;
    do t[n++] = (char)('0' + u % 10); while (u /= 10);
    if (neg) *o++ = '-';
    while (n) *o++ = t[--n];
    *o = 0;
    return o;
}
static char *scat(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }

/* errores: se vuelve a bcc_compile con __builtin_longjmp */
static void *jbuf[5];
static char errbuf[300];

static void fatal(const char *msg)
{
    char *o = scat(errbuf, "bcc: ");
    scat(o, msg);
    __builtin_longjmp(jbuf, 1);
}

/* ======================= tokens ======================= */
enum { TK_IDENT, TK_PUNCT, TK_NUM, TK_STR, TK_EOF, TK_MARK };

typedef struct File { char *name; char *text; int len; } File;
typedef struct Macro Macro;
typedef struct Type Type;

typedef struct Token {
    int kind;
    struct Token *next;
    i64 val;
    Type *ty;                               /* numeros: int, long, unsigned... */
    char *loc; int len;
    char *str; int slen;                    /* cadenas: el contenido ya decodificado (con el 0 final) */
    File *file; int line;
    u8 bol, space;
    Macro *mark;                            /* TK_MARK: fin de la expansion de esta macro */
} Token;


static void error_at(char *loc, int line, File *f, const char *msg)
{
    char *o = errbuf;
    (void)loc;
    o = scat(o, f ? f->name : "?");
    *o++ = ':';
    o = sfmt_num(o, line);
    o = scat(o, ": error: ");
    o = scat(o, msg);
    __builtin_longjmp(jbuf, 1);
}
static void error_tok(Token *t, const char *msg) { error_at(t->loc, t->line, t->file, msg); }
static void error_tok2(Token *t, const char *msg, const char *what)
{
    char b[200], *o = scat(b, msg);
    o = scat(o, ": ");
    if (what) { int n = slen(what); if (n > 60) n = 60; mcopy(o, what, n); o[n] = 0; }
    error_tok(t, b);
}
static void warn_tok(Token *t, const char *msg)
{
    char b[260], *o = b;
    o = scat(o, t->file ? t->file->name : "?");
    *o++ = ':';
    o = sfmt_num(o, t->line);
    o = scat(o, ": aviso: ");
    o = scat(o, msg);
    scat(o, "\n");
    plat_msg(b);
}

static int is_ident1(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static int is_ident2(int c) { return is_ident1(c) || (c >= '0' && c <= '9'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static int is_hex(int c) { return is_digit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }

static int tok_is(Token *t, const char *s) { return (t->kind == TK_PUNCT || t->kind == TK_IDENT) && sneq(t->loc, t->len, s); }

static Token *new_tok(int kind, char *loc, int len, int line, File *f)
{
    Token *t = zalloc(sizeof *t);
    t->kind = kind; t->loc = loc; t->len = len; t->line = line; t->file = f;
    return t;
}

static const char *puncts[] = {
    "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "&&", "||", "++", "--", "<<", ">>", "##", 0
};

static int read_escape(char **pp)
{
    char *p = *pp;
    int c = *p++;
    if (c >= '0' && c <= '7') {
        int v = c - '0', k;
        for (k = 0; k < 2 && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0');
        *pp = p;
        return v;
    }
    if (c == 'x') {
        int v = 0;
        while (is_hex(*p)) v = v * 16 + hexval(*p++);
        *pp = p;
        return v;
    }
    *pp = p;
    switch (c) {
    case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r'; case 'a': return 7;
    case 'b': return 8; case 'f': return 12; case 'v': return 11; case 'e': return 27;
    default: return c;
    }
}

static Type *ty_int, *ty_uint, *ty_long, *ty_ulong, *ty_char, *ty_uchar, *ty_short, *ty_ushort, *ty_void, *ty_bool;

static Token *tokenize(File *f)
{
    Token head = {0}, *cur = &head;
    char *p = f->text, *end = f->text + f->len;
    int line = 1, bol = 1, space = 0;
    while (p < end && *p) {
        if (p[0] == '\\' && p[1] == '\n') { p += 2; line++; continue; }
        if (p[0] == '\\' && p[1] == '\r' && p[2] == '\n') { p += 3; line++; continue; }
        if (*p == '\n') { p++; line++; bol = 1; space = 0; continue; }
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v') { p++; space = 1; continue; }
        if (p[0] == '/' && p[1] == '/') { while (p < end && *p != '\n') p++; space = 1; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p < end && !(p[0] == '*' && p[1] == '/')) { if (*p == '\n') line++; p++; }
            if (p >= end) error_at(p, line, f, "comentario sin cerrar");
            p += 2; space = 1;
            continue;
        }
        if (is_digit(*p) || (*p == '.' && is_digit(p[1]))) {
            char *q = p;
            u64 v = 0;
            int base = 10, uns = 0, lng = 0;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && is_hex(p[2])) {
                p += 2; base = 16;
                while (is_hex(*p)) v = v * 16 + (u64)hexval(*p++);
            } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
                p += 2; base = 2;
                while (*p == '0' || *p == '1') v = v * 2 + (u64)(*p++ - '0');
            } else if (p[0] == '0') {
                base = 8;
                while (*p >= '0' && *p <= '7') v = v * 8 + (u64)(*p++ - '0');
            } else
                while (is_digit(*p)) v = v * 10 + (u64)(*p++ - '0');
            if (*p == '.' || *p == 'e' || *p == 'E')
                error_at(p, line, f, "BCC todavia no tiene numeros con punto (float/double)");
            for (;;) {
                if (*p == 'u' || *p == 'U') { uns = 1; p++; }
                else if (*p == 'l' || *p == 'L') { lng = 1; p++; }
                else break;
            }
            if (is_ident2(*p)) error_at(p, line, f, "numero mal escrito");
            cur = cur->next = new_tok(TK_NUM, q, (int)(p - q), line, f);
            cur->val = (i64)v;
            if (lng || v > 0xFFFFFFFFull || (base == 10 && !uns && v > 0x7FFFFFFF))
                cur->ty = uns ? ty_ulong : ty_long;
            else if (uns || v > 0x7FFFFFFF)
                cur->ty = ty_uint;
            else
                cur->ty = ty_int;
            cur->bol = (u8)bol; cur->space = (u8)space; bol = space = 0;
            continue;
        }
        if (*p == '"' || (*p == 'L' && p[1] == '"')) {
            char *q = p, *buf;
            int n = 0;
            if (*p == 'L') p++;
            p++;
            buf = zalloc(end - p + 1);
            while (*p != '"') {
                if (p >= end || *p == '\n') error_at(q, line, f, "cadena sin cerrar");
                if (*p == '\\') { p++; buf[n++] = (char)read_escape(&p); }
                else buf[n++] = *p++;
            }
            p++;
            cur = cur->next = new_tok(TK_STR, q, (int)(p - q), line, f);
            cur->str = buf; cur->slen = n + 1;
            cur->bol = (u8)bol; cur->space = (u8)space; bol = space = 0;
            continue;
        }
        if (*p == '\'' || (*p == 'L' && p[1] == '\'')) {
            char *q = p;
            int c;
            if (*p == 'L') p++;
            p++;
            if (*p == '\\') { p++; c = read_escape(&p); } else c = (u8)*p++;
            if (*p != '\'') error_at(q, line, f, "caracter mal cerrado");
            p++;
            cur = cur->next = new_tok(TK_NUM, q, (int)(p - q), line, f);
            cur->val = (signed char)c;
            cur->ty = ty_int;
            cur->bol = (u8)bol; cur->space = (u8)space; bol = space = 0;
            continue;
        }
        if (is_ident1(*p)) {
            char *q = p;
            while (is_ident2(*p)) p++;
            cur = cur->next = new_tok(TK_IDENT, q, (int)(p - q), line, f);
            cur->bol = (u8)bol; cur->space = (u8)space; bol = space = 0;
            continue;
        }
        {
            int i, n = 1;
            for (i = 0; puncts[i]; i++) {
                int l = slen(puncts[i]);
                if (p + l <= end && pfx(p, puncts[i])) { n = l; break; }
            }
            if (*p < 33 || *p > 126) error_at(p, line, f, "caracter raro en el codigo");
            cur = cur->next = new_tok(TK_PUNCT, p, n, line, f);
            cur->bol = (u8)bol; cur->space = (u8)space; bol = space = 0;
            p += n;
        }
    }
    cur = cur->next = new_tok(TK_EOF, p, 0, line, f);
    cur->bol = 1;
    return head.next;
}

#include "bcc_pp.c"
#include "bcc_parse.c"
#include "bcc_gen.c"
