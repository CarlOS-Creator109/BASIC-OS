/* basic.c - Interprete de BASIC para PC BASIC
 *
 * Enteros de 32 bits, variables A-Z, programas con numero de linea y modo
 * directo. Sin dependencias de libc: todo lo que toca hardware pasa por las
 * funciones plat_*() declaradas en basic.h.
 */
#include "basic.h"
#include "store.h"
#include "fs.h"

#define MAX_LINES 304
#define LINE_LEN  128
#define GOSUB_MAX 16
#define FOR_MAX   8

struct line   { int num; char text[LINE_LEN]; };
struct gframe { int pc; const char *tp; };
struct fframe { int var; int limit; int step; int pc; const char *tp; };

static struct line   prog[MAX_LINES];
static int           nlines;
static int           vars[26];
#define STR_MAX 127
static char          svars[26][STR_MAX + 1];
static struct gframe gstack[GOSUB_MAX];
static int           gsp;
static struct fframe fstack[FOR_MAX];
static int           fsp;
static char          imm[LINE_LEN];

static const char   *tp;        /* puntero de texto actual */
static int           pc;        /* indice de linea actual (-1 = modo directo) */
static int           running;
static int           jumped;    /* la sentencia cambio pc/tp */
static const char   *err;
static unsigned int  rng = 2463534242u;

/* ------------------------------------------------------------------ */
/* utilidades                                                          */
/* ------------------------------------------------------------------ */

static int  is_digit(char c) { return c >= '0' && c <= '9'; }
static int  is_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static char up(char c)       { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void puts_(const char *s)
{
    while (*s)
        plat_putc(*s++);
}

static void print_int(int v)
{
    char b[12];
    int i = 0;
    unsigned int u;

    if (v < 0) {
        plat_putc('-');
        u = 0u - (unsigned int)v;
    } else {
        u = (unsigned int)v;
    }
    if (u == 0)
        b[i++] = '0';
    while (u) {
        b[i++] = (char)('0' + u % 10);
        u /= 10;
    }
    while (i)
        plat_putc(b[--i]);
}

static void fail(const char *msg)
{
    if (!err)
        err = msg;
}

static void skip_ws(void)
{
    while (*tp == ' ' || *tp == '\t')
        tp++;
}

/* Si el texto actual empieza con la palabra clave w (y no sigue una letra),
 * la consume y devuelve 1. */
static int kw(const char *w)
{
    const char *p = tp;
    while (*w) {
        if (up(*p) != *w)
            return 0;
        p++;
        w++;
    }
    if (is_alpha(*p))
        return 0;
    tp = p;
    return 1;
}

static int isqrt(int n)
{
    unsigned int x, r = 0, bit = 1u << 30;
    if (n <= 0)
        return 0;
    x = (unsigned int)n;
    while (bit > x)
        bit >>= 2;
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (int)r;
}

static unsigned int rnd32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* ------------------------------------------------------------------ */
/* expresiones                                                         */
/* ------------------------------------------------------------------ */

static int expr(void);

static int paren_arg(void)
{
    int v;
    skip_ws();
    if (*tp != '(') {
        fail("SYNTAX  ERROR");
        return 0;
    }
    tp++;
    v = expr();
    skip_ws();
    if (*tp == ')')
        tp++;
    else
        fail("SYNTAX  ERROR");
    return v;
}

static int hexval(char c)
{
    c = up(c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}


/* ------------------------------------------------------------------ */
/* cadenas: A$..Z$, "literales", +, CHR$ STR$ LEFT$ RIGHT$ MID$ INKEY$ */
/* ------------------------------------------------------------------ */

static void sexpr(char *out);

static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void sappend(char *dst, const char *src)
{
    int n = slen(dst);
    while (*src && n < STR_MAX)
        dst[n++] = *src++;
    dst[n] = 0;
}

static void expect(char c)
{
    skip_ws();
    if (*tp == c)
        tp++;
    else
        fail("SYNTAX  ERROR");
}

/* Empieza aqui una expresion de cadena? (no consume nada) */
static int str_start(void)
{
    const char *s;
    int r;

    skip_ws();
    if (*tp == '"')
        return 1;
    if (is_alpha(*tp) && tp[1] == '$')
        return 1;
    s = tp;
    r = kw("CHR$") || kw("STR$") || kw("LEFT$") || kw("RIGHT$") || kw("MID$") || kw("INKEY$");
    tp = s;
    return r;
}

static void num_to_str(char *out, int v)
{
    char b[12];
    int i = 0, n = 0;
    unsigned int u;

    if (v < 0) {
        out[n++] = '-';
        u = 0u - (unsigned int)v;
    } else {
        out[n++] = ' ';                         /* como el C64: espacio en vez de signo */
        u = (unsigned int)v;
    }
    if (u == 0)
        b[i++] = '0';
    while (u) {
        b[i++] = (char)('0' + u % 10);
        u /= 10;
    }
    while (i)
        out[n++] = b[--i];
    out[n] = 0;
}

static void sprim(char *out)
{
    char t[STR_MAX + 1];
    int a, b, n, i;

    out[0] = 0;
    skip_ws();
    if (*tp == '"') {
        n = 0;
        tp++;
        while (*tp && *tp != '"') {
            if (n < STR_MAX)
                out[n++] = *tp;
            tp++;
        }
        out[n] = 0;
        if (*tp == '"')
            tp++;
    } else if (is_alpha(*tp) && tp[1] == '$') {
        sappend(out, svars[up(*tp) - 'A']);
        tp += 2;
    } else if (kw("CHR$")) {
        a = paren_arg();
        if (a > 0 && a < 256) {
            out[0] = (char)a;
            out[1] = 0;
        }
    } else if (kw("STR$")) {
        a = paren_arg();
        num_to_str(out, a);
    } else if (kw("LEFT$")) {
        expect('(');
        sexpr(t);
        expect(',');
        a = expr();
        expect(')');
        n = slen(t);
        if (a < 0)
            a = 0;
        if (a > n)
            a = n;
        for (i = 0; i < a; i++)
            out[i] = t[i];
        out[i] = 0;
    } else if (kw("RIGHT$")) {
        expect('(');
        sexpr(t);
        expect(',');
        a = expr();
        expect(')');
        n = slen(t);
        if (a < 0)
            a = 0;
        if (a > n)
            a = n;
        for (i = 0; i < a; i++)
            out[i] = t[n - a + i];
        out[i] = 0;
    } else if (kw("MID$")) {
        expect('(');
        sexpr(t);
        expect(',');
        a = expr();
        b = STR_MAX;
        skip_ws();
        if (*tp == ',') {
            tp++;
            b = expr();
        }
        expect(')');
        if (a < 1 || b < 0) {
            fail("ILLEGAL QUANTITY  ERROR");
            return;
        }
        n = slen(t);
        for (i = 0; a - 1 + i < n && i < b; i++)
            out[i] = t[a - 1 + i];
        out[i] = 0;
    } else if (kw("INKEY$")) {
        a = plat_inkey();
        if (a) {
            out[0] = (char)a;
            out[1] = 0;
        }
    } else {
        fail("TYPE MISMATCH  ERROR");
    }
}

static void sexpr(char *out)
{
    char t[STR_MAX + 1];

    sprim(out);
    for (;;) {
        skip_ws();
        if (err || *tp != '+')
            break;
        tp++;
        sprim(t);
        sappend(out, t);
    }
}

/* a <op> b con cadenas; el operador esta en el texto. Devuelve -1 / 0 */
static int str_rel(const char *a)
{
    char b[STR_MAX + 1];
    int op, c, i = 0;

    skip_ws();
    if (tp[0] == '<' && tp[1] == '>')      { tp += 2; op = 5; }
    else if (tp[0] == '<' && tp[1] == '=') { tp += 2; op = 3; }
    else if (tp[0] == '>' && tp[1] == '=') { tp += 2; op = 6; }
    else if (tp[0] == '<')                 { tp += 1; op = 1; }
    else if (tp[0] == '>')                 { tp += 1; op = 4; }
    else if (tp[0] == '=')                 { tp += 1; op = 2; }
    else {
        fail("TYPE MISMATCH  ERROR");
        return 0;
    }
    sexpr(b);
    while (a[i] && a[i] == b[i])
        i++;
    c = (unsigned char)a[i] < (unsigned char)b[i] ? 1 : (a[i] == b[i] ? 2 : 4);
    return (op & c) ? -1 : 0;
}

static int __attribute__((noinline)) str_cmp_expr(void)
{
    char a[STR_MAX + 1];
    sexpr(a);
    if (err)
        return 0;
    return str_rel(a);
}

/* LEN ASC VAL: reciben una cadena y devuelven un numero */
static int __attribute__((noinline)) str_numfn(int which)
{
    char t[STR_MAX + 1];
    int v = 0, neg = 0, i = 0;

    expect('(');
    sexpr(t);
    expect(')');
    if (err)
        return 0;
    if (which == 0)
        return slen(t);
    if (which == 1)
        return (unsigned char)t[0];
    while (t[i] == ' ')
        i++;
    if (t[i] == '-') { neg = 1; i++; }
    else if (t[i] == '+') i++;
    while (is_digit(t[i])) {
        v = (int)((unsigned int)v * 10u + (unsigned int)(t[i] - '0'));
        i++;
    }
    return neg ? (int)(0u - (unsigned int)v) : v;
}


static int primary(void)
{
    int v;
    char c;

    skip_ws();
    c = *tp;

    if (is_digit(c)) {
        v = 0;
        while (is_digit(*tp)) {
            v = (int)((unsigned int)v * 10u + (unsigned int)(*tp - '0'));
            tp++;
        }
        return v;
    }
    if (c == '&') {                         /* hexadecimal: &H1F o &1F */
        tp++;
        if (up(*tp) == 'H')
            tp++;
        v = 0;
        while (hexval(*tp) >= 0) {
            v = (int)(((unsigned int)v << 4) | (unsigned int)hexval(*tp));
            tp++;
        }
        return v;
    }
    if (c == '(') {
        tp++;
        v = expr();
        skip_ws();
        if (*tp == ')')
            tp++;
        else
            fail("SYNTAX  ERROR");
        return v;
    }
    if (kw("ABS")) { v = paren_arg(); return v < 0 ? (int)(0u - (unsigned int)v) : v; }
    if (kw("SGN")) { v = paren_arg(); return v > 0 ? 1 : (v < 0 ? -1 : 0); }
    if (kw("SQR")) { v = paren_arg(); return isqrt(v); }
    if (kw("RND")) { v = paren_arg(); return v > 0 ? (int)(rnd32() % (unsigned int)v) : 0; }
    if (kw("FRE")) { (void)paren_arg(); return basic_bytes_free(); }
    if (kw("PEEK")) { v = paren_arg(); return plat_peek((unsigned int)v); }
    if (kw("LEN"))  return str_numfn(0);
    if (kw("ASC"))  return str_numfn(1);
    if (kw("VAL"))  return str_numfn(2);
    if (kw("MOUSEX")) return plat_mouse(0);
    if (kw("MOUSEY")) return plat_mouse(1);
    if (kw("MOUSEB")) return plat_mouse(2);
    if (kw("KEYDOWN")) { v = paren_arg(); return plat_keydown(v) ? -1 : 0; }
    if (kw("HIT")) {                        /* HIT(a, b): los sprites a y b se tocan (BMG) */
        int a, b;
        skip_ws();
        if (*tp != '(') { fail("SYNTAX  ERROR"); return 0; }
        tp++;
        a = expr();
        skip_ws();
        if (*tp != ',') { fail("SYNTAX  ERROR"); return 0; }
        tp++;
        b = expr();
        skip_ws();
        if (*tp != ')') { fail("SYNTAX  ERROR"); return 0; }
        tp++;
        v = plat_bmg(7, a, b, 0, 0);
        if (v < 0) { fail("ILLEGAL QUANTITY  ERROR"); return 0; }
        return v ? -1 : 0;
    }
    if (kw("CDTRACK") || kw("CDTIME")) {
        int t = 0, sec = 0;
        int isTrack = tp[-1] == 'K' || tp[-1] == 'k';
        plat_cd_status(&t, &sec);
        return isTrack ? t : sec;
    }
    if (kw("XMAX")) return plat_gfx_w() - 1;
    if (kw("YMAX")) return plat_gfx_h() - 1;

    if (c == '"' || (is_alpha(c) && tp[1] == '$')) {
        fail("TYPE MISMATCH  ERROR");
        return 0;
    }

    if (is_alpha(c)) {
        int i = up(c) - 'A';
        tp++;
        if (is_alpha(*tp)) {
            fail("SYNTAX  ERROR");
            return 0;
        }
        return vars[i];
    }
    fail("SYNTAX  ERROR");
    return 0;
}

static int unary(void)
{
    skip_ws();
    if (*tp == '-') {
        tp++;
        return (int)(0u - (unsigned int)unary());
    }
    if (*tp == '+') {
        tp++;
        return unary();
    }
    return primary();
}

static int mul_expr(void)
{
    int v = unary();
    for (;;) {
        int b;
        skip_ws();
        if (*tp == '*') {
            tp++;
            b = unary();
            v = (int)((unsigned int)v * (unsigned int)b);
        } else if (*tp == '/' || *tp == '%' || kw("MOD")) {
            int is_div = (*tp == '/');
            if (*tp == '/' || *tp == '%')
                tp++;
            b = unary();
            if (b == 0) {
                fail("DIVISION BY ZERO  ERROR");
                return 0;
            }
            if (b == -1)
                v = is_div ? (int)(0u - (unsigned int)v) : 0;
            else
                v = is_div ? v / b : v % b;
        } else {
            break;
        }
    }
    return v;
}

static int add_expr(void)
{
    int v = mul_expr();
    for (;;) {
        skip_ws();
        if (*tp == '+') {
            tp++;
            v = (int)((unsigned int)v + (unsigned int)mul_expr());
        } else if (*tp == '-') {
            tp++;
            v = (int)((unsigned int)v - (unsigned int)mul_expr());
        } else {
            break;
        }
    }
    return v;
}

static int cmp_expr(void)
{
    int a;
    char c, d;
    if (str_start())
        return str_cmp_expr();
    a = add_expr();
    skip_ws();
    c = tp[0];
    d = c ? tp[1] : 0;
    if (c == '<' && d == '>') { tp += 2; return a != add_expr() ? -1 : 0; }
    if (c == '<' && d == '=') { tp += 2; return a <= add_expr() ? -1 : 0; }
    if (c == '>' && d == '=') { tp += 2; return a >= add_expr() ? -1 : 0; }
    if (c == '<')             { tp += 1; return a <  add_expr() ? -1 : 0; }
    if (c == '>')             { tp += 1; return a >  add_expr() ? -1 : 0; }
    if (c == '=')             { tp += 1; return a == add_expr() ? -1 : 0; }
    return a;
}

static int not_expr(void)
{
    skip_ws();
    if (kw("NOT"))
        return ~not_expr();
    return cmp_expr();
}

static int and_expr(void)
{
    int v = not_expr();
    for (;;) {
        skip_ws();
        if (kw("AND"))
            v &= not_expr();
        else
            break;
    }
    return v;
}

static int expr(void)
{
    int v = and_expr();
    for (;;) {
        skip_ws();
        if (kw("OR"))
            v |= and_expr();
        else if (kw("XOR"))
            v ^= and_expr();
        else
            break;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* programa almacenado                                                 */
/* ------------------------------------------------------------------ */

static int find_line(int n)
{
    int i;
    for (i = 0; i < nlines; i++)
        if (prog[i].num == n)
            return i;
    return -1;
}

static void store_line(int n, const char *text)
{
    int i, j, len = 0;

    while (text[len])
        len++;

    for (i = 0; i < nlines && prog[i].num < n; i++)
        ;
    if (i < nlines && prog[i].num == n) {
        if (len == 0) {                         /* borrar */
            for (j = i; j < nlines - 1; j++)
                prog[j] = prog[j + 1];
            nlines--;
            return;
        }
    } else {
        if (len == 0)
            return;
        if (nlines >= MAX_LINES) {
            puts_("?OUT OF MEMORY  ERROR\n");
            return;
        }
        for (j = nlines; j > i; j--)
            prog[j] = prog[j - 1];
        nlines++;
        prog[i].num = n;
    }
    for (j = 0; j < len && j < LINE_LEN - 1; j++)
        prog[i].text[j] = text[j];
    prog[i].text[j] = 0;
}

/* Copia src a dst en mayusculas (salvo lo que este entre comillas) */
static void normalize(char *dst, const char *src)
{
    int inq = 0;
    int n = 0;
    while (*src && n < LINE_LEN - 1) {
        char c = *src++;
        if (c == '"')
            inq = !inq;
        dst[n++] = inq ? c : up(c);
    }
    dst[n] = 0;
}

/* ------------------------------------------------------------------ */
/* sentencias                                                          */
/* ------------------------------------------------------------------ */

static void exec_statement(void);

static void skip_rest(void)
{
    while (*tp)
        tp++;
}

static void do_goto(int n)
{
    int i = find_line(n);
    if (i < 0) {
        fail("UNDEF'D STATEMENT  ERROR");
        return;
    }
    pc = i;
    tp = prog[i].text;
    jumped = 1;
}

static void st_print(void)
{
    int nl = 1;
    for (;;) {
        skip_ws();
        if (*tp == 0 || *tp == ':')
            break;
        if (str_start()) {
            char t[STR_MAX + 1];
            sexpr(t);
            if (err)
                return;
            skip_ws();
            if (*tp == '=' || *tp == '<' || *tp == '>') {   /* PRINT A$="X" */
                int v = str_rel(t);
                if (err)
                    return;
                if (v >= 0)
                    plat_putc(' ');
                print_int(v);
                plat_putc(' ');
            } else {
                puts_(t);
            }
        } else {
            int v = expr();
            if (err)
                return;
            if (v >= 0)                     /* como QBasic: espacio inicial y final */
                plat_putc(' ');
            print_int(v);
            plat_putc(' ');
        }
        nl = 1;
        skip_ws();
        if (*tp == ';') {
            tp++;
            nl = 0;
        } else if (*tp == ',') {
            tp++;
            nl = 0;
            do {
                plat_putc(' ');
            } while (plat_col() % 8);
        } else {
            break;
        }
    }
    if (nl)
        plat_putc('\n');
}

static void st_let(void)
{
    int i, v;
    skip_ws();
    if (!is_alpha(*tp)) {
        fail("SYNTAX  ERROR");
        return;
    }
    i = up(*tp) - 'A';
    tp++;
    if (*tp == '$') {                           /* A$ = expresion de cadena */
        char t[STR_MAX + 1];
        tp++;
        skip_ws();
        if (*tp != '=') {
            fail("SYNTAX  ERROR");
            return;
        }
        tp++;
        sexpr(t);
        if (!err) {
            int k;
            for (k = 0; t[k]; k++)
                svars[i][k] = t[k];
            svars[i][k] = 0;
        }
        return;
    }
    skip_ws();
    if (*tp != '=') {
        fail("SYNTAX  ERROR");
        return;
    }
    tp++;
    v = expr();
    if (!err)
        vars[i] = v;
}

static void st_if(void)
{
    int v = expr();
    if (err)
        return;
    skip_ws();
    if (kw("THEN")) {
        skip_ws();
        if (!v) {
            skip_rest();
            return;
        }
        if (is_digit(*tp))
            do_goto(expr());
        else
            exec_statement();
    } else if (kw("GOTO")) {
        if (!v) {
            skip_rest();
            return;
        }
        do_goto(expr());
    } else {
        fail("SYNTAX  ERROR");
    }
}

static void st_gosub(void)
{
    int n = expr();
    if (err)
        return;
    if (gsp >= GOSUB_MAX) {
        fail("OUT OF MEMORY  ERROR");
        return;
    }
    gstack[gsp].pc = pc;
    gstack[gsp].tp = tp;
    gsp++;
    do_goto(n);
}

static void st_return(void)
{
    if (gsp == 0) {
        fail("RETURN WITHOUT GOSUB  ERROR");
        return;
    }
    gsp--;
    pc = gstack[gsp].pc;
    tp = gstack[gsp].tp;
    jumped = 1;
}

static void st_for(void)
{
    int var, start, limit, step = 1, i;

    skip_ws();
    if (!is_alpha(*tp)) {
        fail("SYNTAX  ERROR");
        return;
    }
    var = up(*tp) - 'A';
    tp++;
    skip_ws();
    if (*tp != '=') {
        fail("SYNTAX  ERROR");
        return;
    }
    tp++;
    start = expr();
    skip_ws();
    if (!kw("TO")) {
        fail("SYNTAX  ERROR");
        return;
    }
    limit = expr();
    skip_ws();
    if (kw("STEP"))
        step = expr();
    if (err)
        return;

    vars[var] = start;
    for (i = 0; i < fsp; i++)               /* reemplaza un FOR previo de la misma variable */
        if (fstack[i].var == var) {
            fsp = i;
            break;
        }
    if (fsp >= FOR_MAX) {
        fail("OUT OF MEMORY  ERROR");
        return;
    }
    fstack[fsp].var = var;
    fstack[fsp].limit = limit;
    fstack[fsp].step = step;
    fstack[fsp].pc = pc;
    fstack[fsp].tp = tp;
    fsp++;
}

static void st_next(void)
{
    int v = -1, i;
    struct fframe *f;

    skip_ws();
    if (is_alpha(*tp)) {
        v = up(*tp) - 'A';
        tp++;
    }
    if (fsp == 0) {
        fail("NEXT WITHOUT FOR  ERROR");
        return;
    }
    if (v >= 0) {
        for (i = fsp - 1; i >= 0; i--)
            if (fstack[i].var == v)
                break;
        if (i < 0) {
            fail("NEXT WITHOUT FOR  ERROR");
            return;
        }
        fsp = i + 1;
    }
    f = &fstack[fsp - 1];
    vars[f->var] = (int)((unsigned int)vars[f->var] + (unsigned int)f->step);
    if (f->step >= 0 ? vars[f->var] <= f->limit : vars[f->var] >= f->limit) {
        pc = f->pc;
        tp = f->tp;
        jumped = 1;
    } else {
        fsp--;
    }
}

static int parse_int(const char *s, int *out)
{
    int neg = 0, v = 0, digits = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (is_digit(*s)) {
        v = (int)((unsigned int)v * 10u + (unsigned int)(*s - '0'));
        s++;
        digits++;
    }
    while (*s == ' ')
        s++;
    if (!digits || *s)
        return 0;
    *out = neg ? (int)(0u - (unsigned int)v) : v;
    return 1;
}

static void st_input(void)
{
    char buf[LINE_LEN];
    int i, v;

    skip_ws();
    if (*tp == '"') {
        tp++;
        while (*tp && *tp != '"')
            plat_putc(*tp++);
        if (*tp == '"')
            tp++;
        skip_ws();
        if (*tp == ';' || *tp == ',')
            tp++;
    }
    for (;;) {
        skip_ws();
        if (!is_alpha(*tp)) {
            fail("SYNTAX  ERROR");
            return;
        }
        i = up(*tp) - 'A';
        tp++;
        if (*tp == '$') {                       /* INPUT A$ */
            tp++;
            puts_("? ");
            plat_readline(svars[i], STR_MAX + 1);
            skip_ws();
            if (*tp == ',') {
                tp++;
                continue;
            }
            break;
        }
        for (;;) {
            puts_("? ");
            plat_readline(buf, LINE_LEN);
            if (parse_int(buf, &v))
                break;
            puts_("?REDO FROM START\n");
        }
        vars[i] = v;
        skip_ws();
        if (*tp == ',')
            tp++;
        else
            break;
    }
}

static void st_poke(void)
{
    int a, v;
    a = expr();
    skip_ws();
    if (*tp != ',') {
        fail("SYNTAX  ERROR");
        return;
    }
    tp++;
    v = expr();
    if (!err)
        plat_poke((unsigned int)a, v);
}


/* ------------------------------------------------------------------ */
/* graficos y sonido                                                   */
/* ------------------------------------------------------------------ */

static int get_args(int *v, int min, int max)
{
    int n = 0;
    for (;;) {
        v[n++] = expr();
        if (err)
            return 0;
        skip_ws();
        if (*tp == ',' && n < max)
            tp++;
        else
            break;
    }
    if (n < min) {
        fail("SYNTAX  ERROR");
        return 0;
    }
    return n;
}

static int clamp16(int v)
{
    return v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
}

static int need_gfx(void)
{
    if (plat_gfx_w() > 0)
        return 1;
    fail("NO GRAPHICS MODE  ERROR");
    return 0;
}

static void gline(int x0, int y0, int x1, int y1, int c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int e = dx + dy, e2;

    for (;;) {
        plat_plot(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

static void st_plot(void)
{
    int v[3];
    int n = get_args(v, 2, 3);
    if (n && need_gfx())
        plat_plot(clamp16(v[0]), clamp16(v[1]), n > 2 ? v[2] : -1);
}

static void st_line(void)
{
    int v[5];
    int n = get_args(v, 4, 5);
    if (n && need_gfx())
        gline(clamp16(v[0]), clamp16(v[1]), clamp16(v[2]), clamp16(v[3]), n > 4 ? v[4] : -1);
}

static void st_box(void)
{
    int v[6], c, n = get_args(v, 4, 6);
    int x0, y0, x1, y1, t, y;

    if (!n || !need_gfx())
        return;
    c = n > 4 ? v[4] : -1;
    x0 = clamp16(v[0]); y0 = clamp16(v[1]);
    x1 = clamp16(v[2]); y1 = clamp16(v[3]);
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { t = y0; y0 = y1; y1 = t; }
    if (n > 5 && v[5]) {                        /* relleno */
        for (y = y0; y <= y1; y++)
            plat_hline(x0, x1, y, c);
        return;
    }
    plat_hline(x0, x1, y0, c);
    plat_hline(x0, x1, y1, c);
    gline(x0, y0, x0, y1, c);
    gline(x1, y0, x1, y1, c);
}

static void st_circle(void)
{
    int v[4], c, n = get_args(v, 3, 4);
    int cx, cy, r, x, y, d;

    if (!n || !need_gfx())
        return;
    if (v[2] < 0) {
        fail("ILLEGAL QUANTITY  ERROR");
        return;
    }
    c = n > 3 ? v[3] : -1;
    cx = clamp16(v[0]); cy = clamp16(v[1]);
    r = v[2] > 32767 ? 32767 : v[2];
    x = r; y = 0; d = 1 - r;
    while (x >= y) {
        plat_plot(cx + x, cy + y, c); plat_plot(cx - x, cy + y, c);
        plat_plot(cx + x, cy - y, c); plat_plot(cx - x, cy - y, c);
        plat_plot(cx + y, cy + x, c); plat_plot(cx - y, cy + x, c);
        plat_plot(cx + y, cy - x, c); plat_plot(cx - y, cy - x, c);
        y++;
        if (d < 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

/* BEEP [milisegundos [, hz]]   (hz = 0: silencio) */
static void st_beep(void)
{
    int ms = 200, hz = 880;

    skip_ws();
    if (*tp && *tp != ':') {
        ms = expr();
        skip_ws();
        if (*tp == ',') {
            tp++;
            hz = expr();
        }
    }
    if (err)
        return;
    if (ms < 0 || hz < 0 || hz > 20000) {
        fail("ILLEGAL QUANTITY  ERROR");
        return;
    }
    plat_beep(ms, hz);
}

static void st_sleep(void)
{
    int ms = expr();
    if (err)
        return;
    if (ms < 0) {
        fail("ILLEGAL QUANTITY  ERROR");
        return;
    }
    plat_sleep(ms);
}


/* ------------------------------------------------------------------ */
/* SOUND, CD de audio, escritorio y modulos de /SYS                     */
/* ------------------------------------------------------------------ */

#define SND_SINE     0
#define SND_SQUARE   1
#define SND_TRIANGLE 2
#define SND_SAW      3
#define SND_KICK     4
#define SND_HIHAT    5
#define SND_SNARE    6
#define SND_DRUMKIT  7
#define SND_OFF      8

/* SOUND onda,hz,ms | KICK,hz,ms | SNARE,hz,ms | HIHAT,ms | DRUMKIT,hz,tipo,ms | OFF */
static void st_sound(void)
{
    int kind = -1, a[3], n = 0, hz = 0, arg = 0, ms = 0;

    skip_ws();
    if (kw("SINE") || kw("SENO") || kw("SENOIDAL") || kw("SIN"))      kind = SND_SINE;
    else if (kw("SQUARE") || kw("CUADRADA"))                          kind = SND_SQUARE;
    else if (kw("TRIANGLE") || kw("TRIANGULE") || kw("TRIANGULO"))    kind = SND_TRIANGLE;
    else if (kw("SAW") || kw("SIERRA"))                               kind = SND_SAW;
    else if (kw("KICK") || kw("BOMBO"))                               kind = SND_KICK;
    else if (kw("HIHAT") || kw("CHARLES"))                            kind = SND_HIHAT;
    else if (kw("SNARE") || kw("CAJA"))                               kind = SND_SNARE;
    else if (kw("DRUMKIT") || kw("BATERIA"))                          kind = SND_DRUMKIT;
    else if (kw("OFF"))                                               kind = SND_OFF;
    if (kind < 0) {
        fail("SYNTAX  ERROR");
        return;
    }
    skip_ws();
    while (kind != SND_OFF && *tp == ',' && n < 3) {
        tp++;
        a[n++] = expr();
        if (err)
            return;
        skip_ws();
    }
    switch (kind) {
    case SND_HIHAT:    if (n == 1) ms = a[0]; else if (n >= 2) { hz = a[0]; ms = a[1]; } break;
    case SND_DRUMKIT:  if (n > 0) hz = a[0]; if (n > 1) arg = a[1]; if (n > 2) ms = a[2]; break;
    case SND_OFF:      break;
    default:           if (n > 0) hz = a[0]; if (n > 1) ms = a[1]; break;
    }
    if (hz < 0 || hz > 20000 || ms < 0 || arg < 0) {
        fail("ILLEGAL QUANTITY  ERROR");
        return;
    }
    if (!plat_sound(kind, hz, arg, ms) && kind <= SND_SAW && hz > 0)
        plat_beep(ms ? ms : 200, hz);           /* sin tarjeta de sonido: altavoz del PC */
}

static void cd_fail(int r)
{
    if (r == -1)       fail("NO CD DRIVE  ERROR");
    else if (r == -2)  fail("NO DISC  ERROR");
    else if (r == -3)  fail("NO SUCH TRACK  ERROR");
    else if (r == -4)  fail("DATA TRACK  ERROR");
    else if (r == -5)  fail("NO SOUND CARD  ERROR");
    else               fail("DISC READ  ERROR");
}

static void st_cdtoc(void)
{
    char ln[48];
    int i, r;

    for (i = 0;; i++) {
        r = plat_cd_toc(i, ln);
        if (r < 0) {
            cd_fail(r);
            return;
        }
        if (!r)
            break;
        puts_(ln);
        plat_putc('\n');
    }
    if (i == 0)
        puts_("EMPTY DISC\n");
}

static void st_cdplay(void)
{
    int track = 0, r;
    skip_ws();
    if (*tp && *tp != ':') {
        track = expr();
        if (err)
            return;
    }
    r = plat_cd_play(track);
    if (r)
        cd_fail(r);
}

static void st_cdstat(void)
{
    int t, s;
    if (!plat_cd_status(&t, &s)) {
        puts_("STOPPED\n");
        return;
    }
    puts_("PLAYING TRACK ");
    print_int(t);
    puts_("  ");
    print_int(s / 60);
    plat_putc(':');
    if (s % 60 < 10)
        plat_putc('0');
    print_int(s % 60);
    plat_putc('\n');
}

static void exec_fail(int r)
{
    if (r == -1)       fail("FILE NOT FOUND  ERROR");
    else if (r == -3)  fail("FILE TOO LARGE  ERROR");
    else if (r == -4)  fail("DESKTOP ALREADY OPEN  ERROR");
    else if (r == -5)  fail("OUT OF MEMORY  ERROR");
    else               fail("BAD MODULE  ERROR");
}

static int get_spec(void);
static char spec[STR_MAX + 1];

char net_args[160];                       /* argumento de NET "..." (lo lee el modulo) */

static void st_exec(void)
{
    int r;
    if (!get_spec())
        return;
    r = plat_exec(spec);
    if (r < 0)
        exec_fail(r);
}

/* ---- BMG (BASIC-OS Motor Game, bmg.c): SPRITE, FRAME, HIT(), KEYDOWN() ---- */
static void bmg_fail(int r)
{
    if (r == -3) fail("NO GRAPHICS MODE  ERROR");
    else if (r < 0) fail("ILLEGAL QUANTITY  ERROR");
}

static int stmt_end(void)
{
    skip_ws();
    return *tp == 0 || *tp == ':';
}

static void st_sprite(void)
{
    int v[3];
    skip_ws();
    if (kw("DEF")) {
        int w[4], n = get_args(w, 3, 4);
        char z[2];
        z[0] = (char)(n > 3 ? (w[3] < 1 || w[3] > 4 ? 0 : w[3]) : 1);
        z[1] = 0;
        if (n) bmg_fail(plat_bmg(1, w[0], w[1], w[2], z));
    } else if (kw("ROW")) {
        char t[STR_MAX + 1];
        if (!get_args(v, 2, 2)) return;
        skip_ws();
        if (*tp != ',') { fail("SYNTAX  ERROR"); return; }
        tp++;
        sexpr(t);
        if (!err) bmg_fail(plat_bmg(2, v[0], v[1], 0, t));
    } else if (kw("COPY")) {
        if (get_args(v, 2, 2)) bmg_fail(plat_bmg(5, v[0], v[1], 0, 0));
    } else if (kw("OFF")) {
        if (stmt_end()) plat_bmg(4, -1, 0, 0, 0);
        else if (get_args(v, 1, 1)) bmg_fail(plat_bmg(4, v[0], 0, 0, 0));
    } else if (get_args(v, 3, 3)) {
        bmg_fail(plat_bmg(3, v[0], clamp16(v[1]), clamp16(v[2]), 0));
    }
}

static void st_frame(void)
{
    int fps = 0;
    if (!stmt_end()) fps = expr();
    if (!err) bmg_fail(plat_bmg(6, fps, 0, 0, 0));
}

static void st_locate(void)
{
    int c = expr(), r = 0;
    skip_ws();
    if (*tp == ',') {
        tp++;
        r = expr();
    }
    if (!err)
        plat_locate(c, r);
}


/* ------------------------------------------------------------------ */
/* SAVE / LOAD / DIR / SCRATCH (almacen en la imagen de arranque)       */
/* ------------------------------------------------------------------ */

static char blob[STORE_SLOT_BYTES];
static void clear_state(void);

static void store_fail(int r)
{
    if (r == STORE_ENOENT)      fail("FILE NOT FOUND  ERROR");
    else if (r == STORE_EFULL)  fail("DIRECTORY FULL  ERROR");
    else if (r == STORE_E2BIG)  fail("FILE TOO LARGE  ERROR");
    else                        fail("DEVICE NOT PRESENT  ERROR");
}

/* Lee el nombre (cadena) que sigue al comando */

static int put_num(char *b, int n, int v)
{
    char d[12];
    int i = 0;
    if (v == 0)
        d[i++] = '0';
    while (v > 0) {
        d[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i)
        b[n++] = d[--i];
    return n;
}

static void fs_fail(int r);
static int has_colon(const char *p);
static int get_spec(void);
static void st_save(void)
{
    char name[STORE_NAME_MAX + 1];
    int i, n = 0, r, disk;

    if (!get_spec())
        return;
    disk = has_colon(spec);                     /* SAVE "HD0.1:/JUEGOS/MI JUEGO.BAS": a un disco BFS */
    if (!disk) {
        for (i = 0; i < STORE_NAME_MAX && spec[i]; i++)
            name[i] = spec[i];
        name[i] = 0;
        if (!name[0]) {
            fail("MISSING FILE NAME  ERROR");
            return;
        }
    }
    for (i = 0; i < nlines; i++) {
        const char *p = prog[i].text;
        if (n + 8 + LINE_LEN > STORE_SLOT_BYTES) {
            fail("FILE TOO LARGE  ERROR");
            return;
        }
        n = put_num(blob, n, prog[i].num);
        blob[n++] = ' ';
        while (*p)
            blob[n++] = *p++;
        blob[n++] = '\n';
    }
    puts_("SAVING ");
    puts_(disk ? spec : name);
    plat_putc('\n');
    if (disk) {
        r = fs_write(spec, blob, (unsigned int)n);
        if (r)
            fs_fail(r);
        return;
    }
    r = store_save(name, blob, n);
    if (r)
        store_fail(r);
}

static void fs_fail(int r)
{
    if (r == FS_ENOENT)       fail("FILE NOT FOUND  ERROR");
    else if (r == FS_EFMT)    fail("UNKNOWN FILE SYSTEM  ERROR");
    else if (r == FS_E2BIG)   fail("FILE TOO LARGE  ERROR");
    else if (r == FS_EIO)     fail("DISK READ  ERROR");
    else if (r == FS_EISDIR)  fail("FILE IS A DIRECTORY  ERROR");
    else if (r == FS_EROFS)   fail("READ ONLY DISK (SOLO BFS SE ESCRIBE)  ERROR");
    else if (r == FS_ENOSPC)  fail("DISK FULL  ERROR");
    else if (r == FS_EEXIST)  fail("FILE EXISTS  ERROR");
    else if (r == FS_ENOTEMPTY) fail("DIRECTORY NOT EMPTY  ERROR");
    else if (r == FS_ENAME)   fail("BAD FILE NAME  ERROR");
    else                      fail("DEVICE NOT PRESENT  ERROR");
}

static char spec[STR_MAX + 1];

static int has_colon(const char *p)
{
    for (; *p; p++)
        if (*p == ':')
            return 1;
    return 0;
}

/* Lee la cadena que sigue al comando en spec. 0 = error */
static int get_spec(void)
{
    skip_ws();
    if (*tp == 0 || *tp == ':') {
        fail("MISSING FILE NAME  ERROR");
        return 0;
    }
    sexpr(spec);
    return !err;
}

static void load_blob(int n)
{
    char ln[LINE_LEN + 8];
    int i, k = 0;

    nlines = 0;
    clear_state();
    for (i = 0; i <= n; i++) {
        if (i == n || blob[i] == '\n') {
            ln[k] = 0;
            if (k)
                basic_exec_line(ln);
            k = 0;
        } else if (blob[i] != '\r' && k < LINE_LEN + 7) {
            ln[k++] = blob[i];
        }
    }
    running = 0;                                /* el programa cargado reemplaza al actual */
}

void basic_load_text(const char *text, int n)
{
    int i;
    if (n > (int)sizeof blob)
        n = (int)sizeof blob;
    for (i = 0; i < n; i++)
        blob[i] = text[i];
    load_blob(n);
}

static void st_load(void)
{
    char name[STORE_NAME_MAX + 1];
    int n, i;
    unsigned int total;

    if (!get_spec())
        return;
    if (!spec[0]) {                             /* LOAD "": el Pong de texto de /SYS */
        const char *d = "SYS:PONG.BAS";
        for (i = 0; d[i]; i++)
            spec[i] = d[i];
        spec[i] = 0;
    }
    if (has_colon(spec)) {                      /* LOAD "HD0:PROG.BAS" */
        n = fs_read(spec, blob, STORE_SLOT_BYTES, &total);
        if (n < 0) {
            fs_fail(n);
            return;
        }
        if (total > STORE_SLOT_BYTES) {
            fail("FILE TOO LARGE  ERROR");
            return;
        }
        puts_("LOADING ");
        puts_(spec);
        plat_putc('\n');
        load_blob(n);
        return;
    }
    for (i = 0; i < STORE_NAME_MAX && spec[i]; i++)
        name[i] = spec[i];
    name[i] = 0;
    n = store_load(name, blob, STORE_SLOT_BYTES);
    if (n < 0) {
        store_fail(n);
        return;
    }
    puts_("LOADING ");
    puts_(name);
    plat_putc('\n');
    load_blob(n);
}

static void st_type(void)
{
    unsigned int total;
    int n, i;

    if (!get_spec())
        return;
    n = fs_read(spec, blob, STORE_SLOT_BYTES, &total);
    if (n < 0) {
        fs_fail(n);
        return;
    }
    for (i = 0; i < n; i++) {
        if (blob[i] != '\r')
            plat_putc(blob[i]);
        if ((i & 255) == 255 && plat_break())
            break;
    }
    if (n && blob[n - 1] != '\n')
        plat_putc('\n');
    if ((unsigned int)n < total)
        puts_("[TRUNCATED]\n");
}

static void dir_cb(const char *name, unsigned int size, int isdir)
{
    if (isdir)
        puts_("  DIR ");
    else {
        print_int((int)size);
        plat_putc(' ');
    }
    plat_putc('"');
    puts_(name);
    puts_("\"\n");
}

static void st_drives(void)
{
    char ln[64];
    int i;
    for (i = 0; plat_drive_line(i, ln); i++) {
        puts_(ln);
        plat_putc('\n');
    }
    if (i == 0)
        puts_("NO DRIVES\n");
}

static void st_dir(void)
{
    char name[17];
    int i, len, count = 0;

    skip_ws();
    if (*tp && *tp != ':') {                    /* DIR "HD0:" : listado de una unidad */
        int r;
        if (!get_spec())
            return;
        r = fs_dir(spec, dir_cb);
        if (r)
            fs_fail(r);
        else {
            unsigned long long fr, tot;
            if (fs_space(spec, &fr, &tot) == 0) {   /* BFS o FAT: espacio libre */
                print_int((int)(fr >> 20));
                puts_(" MB LIBRES DE ");
                print_int((int)(tot >> 20));
                puts_(" (");
                puts_(fs_kind());
                puts_(")\n");
            }
        }
        return;
    }

    if (store_open()) {
        fail("DEVICE NOT PRESENT  ERROR");
        return;
    }
    for (i = 0; i < STORE_SLOTS; i++)
        if (store_entry(i, name, &len)) {
            print_int(len);
            puts_(" \"");
            puts_(name);
            puts_("\"\n");
            count++;
        }
    print_int(count);
    puts_(" FILES, ");
    print_int(STORE_SLOTS - count);
    puts_(" FREE\n");
}

static void st_scratch(void)
{
    char name[STORE_NAME_MAX + 1];
    int r, i;
    if (!get_spec())
        return;
    if (has_colon(spec)) {                      /* SCRATCH "HD0.1:/ARCHIVO" (o una carpeta vacia) */
        r = fs_remove(spec);
        if (r)
            fs_fail(r);
        return;
    }
    for (i = 0; i < STORE_NAME_MAX && spec[i]; i++)
        name[i] = spec[i];
    name[i] = 0;
    r = store_delete(name);
    if (r)
        store_fail(r);
}

/* MKDIR "HD0.1:/CARPETA" */
static void st_mkdir(void)
{
    int r;
    if (!get_spec())
        return;
    if ((r = fs_mkdir(spec)))
        fs_fail(r);
}

/* MV "HD0.1:/A", "HD0.1:/B"  (renombrar o mover dentro del mismo disco)
 * CP "HD0.1:/A", "HD0.1:/B"  (copiar; el origen puede ser de cualquier disco, el destino BFS) */
static void st_mv_cp(int copy)
{
    char from[STR_MAX + 1];
    int r, i;
    if (!get_spec())
        return;
    for (i = 0; spec[i]; i++) from[i] = spec[i];
    from[i] = 0;
    skip_ws();
    if (*tp != ',') {
        fail("SYNTAX  ERROR");
        return;
    }
    tp++;
    if (!get_spec())
        return;
    r = copy ? plat_copy(from, spec) : fs_rename(from, spec);
    if (r)
        fs_fail(r);
}

static void st_list(void)
{
    int i;
    for (i = 0; i < nlines; i++) {
        print_int(prog[i].num);
        plat_putc(' ');
        puts_(prog[i].text);
        plat_putc('\n');
        if (plat_break())
            break;
    }
}

static void clear_state(void)
{
    int i;
    for (i = 0; i < 26; i++) {
        vars[i] = 0;
        svars[i][0] = 0;
    }
    gsp = 0;
    fsp = 0;
}

static void st_run(void)
{
    int n = -1;
    skip_ws();
    if (is_digit(*tp))
        n = expr();
    clear_state();
    plat_bmg(0, 0, 0, 0, 0);                /* RUN: sin sprites de la corrida anterior */
    if (n >= 0) {
        do_goto(n);
        return;
    }
    if (nlines == 0) {
        running = 0;
        return;
    }
    pc = 0;
    tp = prog[0].text;
    jumped = 1;
}

static void st_help(void)
{
    puts_("COMMANDS: RUN LIST NEW CLS REBOOT\n");
    puts_("PRINT LET IF/THEN GOTO GOSUB RETURN\n");
    puts_("FOR/TO/STEP NEXT INPUT REM END STOP\n");
    puts_("COLOR FG,BG  POKE ADDR,VAL  SLEEP MS\n");
    puts_("PLOT X,Y[,C]  LINE X1,Y1,X2,Y2[,C]\n");
    puts_("BOX X1,Y1,X2,Y2[,C[,FILL]]  CIRCLE X,Y,R[,C]\n");
    puts_("BEEP [MS[,HZ]]  AUDIO  XMAX YMAX (PIXELS)\n");
    puts_("BMG: SPRITE DEF N,W,H[,ZOOM]  SPRITE ROW N,Y,\"0F.\"  SPRITE N,X,Y\n");
    puts_("SPRITE OFF [N]  SPRITE COPY A,B  FRAME [FPS]  HIT(A,B)  KEYDOWN(K)\n");
    puts_("SAVE \"N\"  LOAD \"N\"  DIR  SCRATCH \"N\"\n");
    puts_("DRIVES  DIR \"HD0:\"  TYPE \"HD0:F.TXT\"  LOAD \"HD0:F.BAS\"\n");
    puts_("BFS: SAVE \"HD0.1:/X.BAS\"  MKDIR  CP \"A\",\"B\"  MV  SCRATCH \"HD0.1:/X\"\n");
    puts_("SOUND SINE|SQUARE|TRIANGLE|SAW,HZ,MS\n");
    puts_("SOUND KICK|SNARE,HZ,MS  HIHAT,MS  OFF\n");
    puts_("SOUND DRUMKIT,HZ,KIT(0-3),MS  SNDWAIT\n");
    puts_("CDTOC CDPLAY [N] CDSTOP CDEJECT CDSTAT\n");
    puts_("DESKTOP  YAAI  BASHTERM  TINYCORE  NET  BN  BNM  PKG  FORO  KEYB  RAM  TAREAS  INSTALL  PLAY \"X.BVC\"  SAY \"TEXTO\"  EXEC \"X.APP\"  LOAD \"\" = PONG\n");
    puts_("LOCATE COL,ROW  (TEXT CURSOR)\n");
    puts_("UNIX = LS CAT RM CLEAR MAN... (ATAJOS PARA LINUX)\n");
    puts_("PCI = LIST PCI DEVICES\n");
    puts_("MOUSE: DRAG=COPY  RIGHT/MIDDLE=PASTE\n");
    puts_("MOUSEX MOUSEY MOUSEB (1=L 2=R 4=M)\n");
    puts_("ABS SGN SQR RND PEEK FRE LEN ASC VAL\n");
    puts_("A$-Z$ CHR$ STR$ LEFT$ RIGHT$ MID$ INKEY$\n");
    puts_("AND OR XOR NOT MOD   ESC = BREAK\n");
    puts_("POKE 53280,C BORDER  53281,C BACKGROUND\n");
    puts_("POKE 646,C TEXT COLOR (C64 COLORS 0-15)\n");
}

/* ---- atajos para quien viene de UNIX/Linux: LS, CAT, RM, CLEAR, MAN y pistas para el resto ----
 * Aceptan el argumento sin comillas (LS HD0.1:  CAT HD0.1:LEEME.TXT): se le ponen las comillas y se llama al
 * comando de BASIC de verdad. Sin comillas el argumento llega hasta el final de la linea (o hasta " :"). */
static char alias_buf[LINE_LEN + 2];

static const char *alias_quote(void)             /* devuelve donde sigue la linea despues del argumento */
{
    const char *rest;
    int n = 0;
    skip_ws();
    if (*tp == '"' || *tp == 0 || *tp == ':')
        return 0;                                /* ya trae comillas, o no hay argumento */
    alias_buf[n++] = '"';
    while (*tp && n < LINE_LEN - 1) {            /* el ':' es parte de HD0.1:...; separa solo " :" */
        if (tp[0] == ' ' && tp[1] == ':')
            break;
        alias_buf[n++] = *tp++;
    }
    while (n > 1 && alias_buf[n - 1] == ' ')
        n--;
    alias_buf[n++] = '"';
    alias_buf[n] = 0;
    rest = tp;
    tp = alias_buf;
    return rest;
}

static void unix_hint(const char *cmd, const char *what)
{
    puts_("?");
    puts_(cmd);
    puts_(": ");
    puts_(what);
    plat_putc('\n');
}

static const char *unix_last;
static int unix_word(void)                       /* comandos de UNIX que no existen aqui */
{
    static const char *const w[] = { "TOUCH", "CD", "PWD", "NANO", "VI", "VIM",
                                     "SUDO", "EXIT", "CHMOD", "GREP", "ECHO", 0 };
    int i;
    for (i = 0; w[i]; i++)
        if (kw(w[i])) { unix_last = w[i]; return 1; }
    return 0;
}

static void st_unix_table(void)
{
    puts_("UNIX/LINUX        PC BASIC\n");
    puts_("LS [DISCO:]       DIR [\"HD0.1:\"]\n");
    puts_("CAT ARCHIVO       TYPE \"HD0.1:ARCHIVO\"\n");
    puts_("RM NOMBRE         SCRATCH \"NOMBRE\" (SAVE) O \"HD0.1:/ARCHIVO\" (BFS)\n");
    puts_("CP A B            CP \"HD0.1:/A\",\"HD0.1:/B\"\n");
    puts_("MV A B            MV \"HD0.1:/A\",\"HD0.1:/B\"\n");
    puts_("MKDIR X           MKDIR \"HD0.1:/X\"   (RMDIR PARA BORRARLA)\n");
    puts_("CLEAR             CLS\n");
    puts_("MAN               HELP\n");
    puts_("./PROG.BAS        LOAD \"HD0.1:PROG.BAS\" Y LUEGO RUN\n");
    puts_("LSBLK / DF        DRIVES\n");
    puts_("LSPCI             PCI\n");
    puts_("CP MV MKDIR CD    TODAVIA NO: USA BASHTERM (BASH DE VERDAD)\n");
}

static void exec_statement(void)
{
    skip_ws();
    if (*tp == 0 || *tp == ':')
        return;

    if (up(tp[0]) == 'R' && up(tp[1]) == 'E' && up(tp[2]) == 'M') {
        skip_rest();                        /* REM (tambien REMARK, etc.) */
    } else if (*tp == '\'') {
        skip_rest();
    } else if (kw("PRINT")) {
        st_print();
    } else if (*tp == '?') {
        tp++;
        st_print();
    } else if (kw("LET")) {
        st_let();
    } else if (kw("IF")) {
        st_if();
    } else if (kw("GOTO")) {
        int n = expr();
        if (!err)
            do_goto(n);
    } else if (kw("GOSUB")) {
        st_gosub();
    } else if (kw("RETURN")) {
        st_return();
    } else if (kw("FOR")) {
        st_for();
    } else if (kw("NEXT")) {
        st_next();
    } else if (kw("END")) {
        running = 0;
    } else if (kw("STOP")) {
        puts_("BREAK");
        if (pc >= 0) {
            puts_(" IN ");
            print_int(prog[pc].num);
        }
        plat_putc('\n');
        running = 0;
    } else if (kw("INPUT")) {
        st_input();
    } else if (kw("CLS")) {
        plat_clear();
    } else if (kw("COLOR")) {
        int fg = expr(), bg = 0;
        skip_ws();
        if (*tp == ',') {
            tp++;
            bg = expr();
        }
        if (!err)
            plat_color(fg, bg);
    } else if (kw("SAVE")) {
        st_save();
    } else if (kw("LOAD")) {
        st_load();
    } else if (kw("DIR") || kw("DIRECTORY") || kw("CATALOG")) {
        st_dir();
    } else if (kw("TYPE")) {
        st_type();
    } else if (kw("LS")) {
        const char *rest = alias_quote();
        st_dir();
        if (rest && !err) tp = rest;
    } else if (kw("CAT") || kw("MORE") || kw("LESS")) {
        const char *rest = alias_quote();
        st_type();
        if (rest && !err) tp = rest;
    } else if (kw("RM")) {
        const char *rest = alias_quote();
        st_scratch();
        if (rest && !err) tp = rest;
    } else if (kw("CLEAR")) {
        plat_clear();
    } else if (kw("MAN")) {
        st_help();
    } else if (kw("UNIX")) {
        st_unix_table();
    } else if (kw("LSBLK") || kw("DF")) {
        st_drives();
    } else if (unix_word()) {
        unix_hint(unix_last, "NO EXISTE EN PC BASIC. ESCRIBE UNIX PARA VER LOS EQUIVALENTES (O BASHTERM PARA BASH)");
        skip_rest();
    } else if (kw("AUDIO")) {
        puts_(plat_audio_info());
        plat_putc('\n');
    } else if (kw("DRIVES")) {
        st_drives();
    } else if (kw("SCRATCH") || kw("DELETE")) {
        st_scratch();
    } else if (kw("MKDIR") || kw("MD")) {
        const char *rest = alias_quote();
        st_mkdir();
        if (rest && !err) tp = rest;
    } else if (kw("RMDIR")) {
        const char *rest = alias_quote();
        st_scratch();
        if (rest && !err) tp = rest;
    } else if (kw("MV") || kw("RENAME")) {
        st_mv_cp(0);
    } else if (kw("CP") || kw("COPY")) {
        st_mv_cp(1);
    } else if (kw("TAREAS") || kw("PS")) {
        char ln[96];
        int i;
        for (i = 0; plat_task_line(i, ln); i++) {
            puts_(ln);
            plat_putc('\n');
        }
    } else if (kw("RAM") || kw("BANCOS")) {
        char ln[96];
        int i;
        for (i = 0; plat_ram_line(i, ln); i++) {
            puts_(ln);
            plat_putc('\n');
            if (plat_break())
                break;
        }
    } else if (kw("PCI") || kw("LSPCI")) {
        char ln[64];
        int i;
        for (i = 0; plat_pci_line(i, ln); i++) {
            puts_(ln);
            plat_putc('\n');
            if (plat_break())
                break;
        }
    } else if (kw("PLOT")) {
        st_plot();
    } else if (kw("LINE")) {
        st_line();
    } else if (kw("BOX")) {
        st_box();
    } else if (kw("CIRCLE")) {
        st_circle();
    } else if (kw("BEEP")) {
        st_beep();
    } else if (kw("SOUND")) {
        st_sound();
    } else if (kw("SNDWAIT")) {
        plat_sndwait();
    } else if (kw("CDTOC")) {
        st_cdtoc();
    } else if (kw("CDPLAY")) {
        st_cdplay();
    } else if (kw("CDSTOP")) {
        plat_cd_stop();
    } else if (kw("CDEJECT")) {
        int r = plat_cd_eject();
        if (r)
            cd_fail(r);
    } else if (kw("CDSTAT")) {
        st_cdstat();
    } else if (kw("DESKTOP")) {
        int r = plat_exec("GUI.SYS");
        if (r < 0)
            exec_fail(r);
    } else if (kw("NET")) {
        int r, i;
        net_args[0] = 0;
        skip_ws();
        if (*tp == '"') {
            if (!get_spec())
                return;
            for (i = 0; spec[i] && i < (int)sizeof net_args - 1; i++)
                net_args[i] = spec[i];
            net_args[i] = 0;
        }
        r = plat_exec("NET.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("BN")) {
        int r, i;
        net_args[0] = 0;
        skip_ws();
        if (*tp == '"') {
            if (!get_spec())
                return;
            for (i = 0; spec[i] && i < (int)sizeof net_args - 1; i++)
                net_args[i] = spec[i];
            net_args[i] = 0;
        }
        r = plat_exec("BN.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("BNM")) {
        int r, i, j = 0;
        const char *m = "@bnm ";
        skip_ws();
        while (m[j]) net_args[j] = m[j], j++;
        if (*tp == '"') {
            if (!get_spec())
                return;
            for (i = 0; spec[i] && j < (int)sizeof net_args - 1; i++)
                net_args[j++] = spec[i];
        }
        net_args[j] = 0;
        r = plat_exec("BN.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("PLAY")) {
        int r, i;
        net_args[0] = 0;
        skip_ws();
        if (*tp == '"') {
            if (!get_spec())
                return;
            for (i = 0; spec[i] && i < (int)sizeof net_args - 1; i++)
                net_args[i] = spec[i];
            net_args[i] = 0;
        }
        r = plat_exec("PLAY.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("SAY")) {
        int r, i;
        net_args[0] = 0;
        skip_ws();
        if (*tp && *tp != ':') {
            if (!get_spec())
                return;
            for (i = 0; spec[i] && i < (int)sizeof net_args - 1; i++)
                net_args[i] = spec[i];
            net_args[i] = 0;
        }
        r = plat_exec("SAY.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("PKG")) {
        int r = plat_exec("PKG.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("FORO")) {
        int r = plat_exec("FORO.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("INSTALL")) {
        int r = plat_exec("INSTALL.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("KEYB")) {
        char a[8];
        int n = 0;
        skip_ws();
        while (*tp && *tp != ':' && n < 7)
            a[n++] = *tp++;
        while (*tp && *tp != ':')
            tp++;
        a[n] = 0;
        if (plat_keyb(a) < 0)
            fail("USE KEYB, KEYB ES, KEYB LA O KEYB US");
    } else if (kw("TINYCORE")) {
        int r = plat_exec("TINYCORE.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("BASHTERM")) {
        int r = plat_exec("BASHTERM.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("YAAI")) {
        int r = plat_exec("YAAI.APP");
        if (r < 0)
            exec_fail(r);
    } else if (kw("EXEC")) {
        st_exec();
    } else if (kw("LOCATE")) {
        st_locate();
    } else if (kw("SLEEP")) {
        st_sleep();
    } else if (kw("SPRITE")) {
        st_sprite();
    } else if (kw("FRAME")) {
        st_frame();
    } else if (kw("POKE")) {
        st_poke();
    } else if (kw("LIST")) {
        st_list();
    } else if (kw("RUN")) {
        st_run();
    } else if (kw("NEW")) {
        nlines = 0;
        clear_state();
        running = 0;
    } else if (kw("HELP")) {
        st_help();
    } else if (kw("REBOOT")) {
        plat_reboot();
    } else if (is_alpha(*tp)) {
        st_let();
    } else {
        fail("SYNTAX  ERROR");
    }
}

/* ------------------------------------------------------------------ */
/* bucle de ejecucion                                                  */
/* ------------------------------------------------------------------ */

static void report_error(void)
{
    plat_putc('?');
    puts_(err);
    if (pc >= 0 && pc < nlines) {
        puts_(" IN ");
        print_int(prog[pc].num);
    }
    plat_putc('\n');
}

static void run_loop(void)
{
    while (running) {
        if (plat_break()) {
            puts_("BREAK");
            if (pc >= 0 && pc < nlines) {
                puts_(" IN ");
                print_int(prog[pc].num);
            }
            plat_putc('\n');
            running = 0;
            break;
        }

        exec_statement();

        if (!err && running && !jumped) {
            skip_ws();
            if (*tp == ':') {
                tp++;
            } else if (*tp == 0) {
                if (pc < 0) {
                    running = 0;
                } else if (++pc >= nlines) {
                    running = 0;
                } else {
                    tp = prog[pc].text;
                }
            } else {
                err = "SYNTAX  ERROR";
            }
        }
        jumped = 0;

        if (err) {
            report_error();
            running = 0;
        }
    }
}

/* ---- para los programas compilados (kapi 5): una sentencia o expresion suelta, guardando el estado ---- */
struct qstate { const char *tp; int pc, running, jumped; const char *err; };
static void q_save(struct qstate *s) { s->tp = tp; s->pc = pc; s->running = running; s->jumped = jumped; s->err = err; }
static void q_load(const struct qstate *s) { tp = s->tp; pc = s->pc; running = s->running; jumped = s->jumped; err = s->err; }

int basic_do(const char *stmt)
{
    static char qbuf[4][LINE_LEN];             /* una sentencia puede llamar a otra (EXEC dentro de EXEC...) */
    static int qdepth;
    struct qstate s;
    int bad;
    if (qdepth >= 4)
        return -1;
    q_save(&s);
    normalize(qbuf[qdepth], stmt);
    tp = qbuf[qdepth++];
    pc = -1;
    err = 0;
    running = 1;
    jumped = 0;
    run_loop();
    bad = err != 0;
    qdepth--;
    q_load(&s);
    return bad ? -1 : 0;
}

int basic_eval(const char *e, int *val)
{
    static char ebuf[LINE_LEN];
    struct qstate s;
    int v, bad;
    q_save(&s);
    normalize(ebuf, e);
    tp = ebuf;
    err = 0;
    v = expr();
    skip_ws();
    if (!err && *tp)
        err = "SYNTAX  ERROR";
    if (err)
        report_error();
    bad = err != 0;
    q_load(&s);
    if (!bad && val)
        *val = v;
    return bad ? -1 : 0;
}

int basic_bytes_free(void)
{
    return (MAX_LINES - nlines) * LINE_LEN - 1;
}

void basic_banner(void)
{
    print_int(basic_bytes_free());
    puts_(" BASIC BYTES FREE\n\nREADY.\n");
}

void basic_init(void)
{
    nlines = 0;
    clear_state();
    rng ^= plat_seed();
    if (!rng)
        rng = 2463534242u;
}

int basic_exec_line(char *line)
{
    char *p = line;
    char norm[LINE_LEN];

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == 0)
        return 0;

    if (is_digit(*p)) {                         /* linea de programa */
        int n = 0;
        while (is_digit(*p)) {
            n = n * 10 + (*p - '0');
            p++;
            if (n > 65535)
                break;
        }
        if (n < 1 || n > 65535) {
            puts_("?ILLEGAL QUANTITY  ERROR\n");
            return 0;
        }
        while (*p == ' ' || *p == '\t')
            p++;
        normalize(norm, p);
        store_line(n, norm);
        return 0;
    }

    normalize(imm, p);                          /* modo directo */
    pc = -1;
    tp = imm;
    err = 0;
    running = 1;
    jumped = 0;
    run_loop();
    return 1;
}
