/* rt_native.c - runtime del modo nativo de bas2bin. El codigo x86-64 que genera el compilador va pegado detras
 * de este modulo y llama a estas funciones por la tabla fns[] (ver b2b.h). Las variables A-Z / A$-Z$ viven aqui y
 * el codigo generado las toca directo por su direccion. Solo para PC BASIC de 64 bits. GPL-3.0-or-later. */
#include "kapi.h"
#include "mod_head.h"
#include "tools/bas2bin/b2b.h"

#define SMAX 127
#define POOL 16384
#define GMAX 64
#define FMAX 16
#define CBMAX 4

static const struct kapi *K;
int vars[26];
char svars[26][SMAX + 1];
unsigned long long saved_rsp;                   /* lo guarda el prologo del programa: para salir desde cualquier lado */
static int cur_line, outcol;
static unsigned rng;
static char pool[POOL];
static int pool_at;
static unsigned long long gstack[GMAX];
static int gsp;
static struct { int var, limit, step; unsigned long long resume; } fst[FMAX];
static int fsp;
static struct { char buf[128]; int n, args, paren; } cb[CBMAX];
static int cbsp;

extern void *rt_fns[];
struct b2b_next b2b_ext = { B2B_NMAGIC, 1, (unsigned long long)(unsigned long)vars,
                            (unsigned long long)(unsigned long)svars, (unsigned long long)(unsigned long)&saved_rsp,
                            (unsigned long long)(unsigned long)rt_fns, 0, 0, 0 };

/* ---- salida ---- */
static void out(const char *s)
{
    const char *p;
    K->print(s);
    for (p = s; *p; p++) {
        if (*p == '\n' || *p == '\r') outcol = 0;
        else if (*p == '\b') { if (outcol) outcol--; }
        else if ((unsigned char)*p >= 32 && !((unsigned char)*p >= 128 && (unsigned char)*p < 160)) outcol++;
    }
}
static void outc(char c) { char s[2]; s[0] = c; s[1] = 0; out(s); }
static int itoa_(char *b, int v)
{
    char t[12];
    int i = 0, n = 0;
    unsigned u = v < 0 ? 0u - (unsigned)v : (unsigned)v;
    if (v < 0) b[n++] = '-';
    do { t[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) b[n++] = t[--i];
    b[n] = 0;
    return n;
}

/* salir del programa desde cualquier profundidad: vuelve a mod_main (ver el prologo en b2b.c) */
static void rt_abort(void)
{
    __asm__ volatile("mov %0, %%rsp\n\tpop %%rbx\n\tret" :: "m"(saved_rsp));
    __builtin_unreachable();
}
static void err_(const char *m)
{
    char b[16];
    if (outcol) out("\n");
    out("?"); out(m); out(" ERROR IN "); itoa_(b, cur_line); out(b); out("\n");
    rt_abort();
}

static char *tmp(void)                          /* cadena temporal (cada linea empieza de nuevo) */
{
    char *p;
    if (pool_at + SMAX + 1 > POOL) pool_at = 0;
    p = pool + pool_at;
    pool_at += SMAX + 1;
    p[0] = 0;
    return p;
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scpy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max) { d[i] = s[i]; i++; } d[i] = 0; }

/* ---- funciones que llama el codigo generado ---- */
static void rt_line(int n)
{
    cur_line = n;
    pool_at = 0;
    if (K->esc()) {
        char b[16];
        if (outcol) out("\n");
        out("BREAK IN "); itoa_(b, n); out(b); out("\n");
        rt_abort();
    }
}
static void rt_prn(int v) { char b[16]; if (v >= 0) outc(' '); itoa_(b, v); out(b); outc(' '); }
static void rt_prs(const char *s) { out(s); }
static void rt_prtab(void) { do outc(' '); while (outcol % 8); }
static void rt_prnl(void) { out("\n"); }

static int readline(char *buf, int max)
{
    int n = 0;
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); continue; }
        if (c == 27) { buf[n] = 0; out("\n"); out("BREAK\n"); rt_abort(); }
        if (c == '\r' || c == '\n') { out("\n"); buf[n] = 0; return n; }
        if ((c == 8 || c == 20 || c == 127) && n) { n--; out("\b \b"); continue; }
        if (c >= 32 && c < 127 && n < max - 1) { buf[n++] = (char)c; outc((char)c); }
    }
}
static int parse_int(const char *s, int *v)
{
    int i = 0, neg = 0, any = 0;
    unsigned u = 0;
    while (s[i] == ' ') i++;
    if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') i++;
    while (s[i] >= '0' && s[i] <= '9') { u = u * 10u + (unsigned)(s[i++] - '0'); any = 1; }
    while (s[i] == ' ') i++;
    if (!any || s[i]) return 0;
    *v = neg ? (int)(0u - u) : (int)u;
    return 1;
}
static void rt_inn(int var)
{
    char b[128];
    int v;
    for (;;) {
        out("? ");
        readline(b, sizeof b);
        if (parse_int(b, &v)) break;
        out("?REDO FROM START\n");
    }
    vars[var] = v;
}
static void rt_ins(int var) { out("? "); readline(svars[var], SMAX + 1); }
static int rt_div(int a, int b)
{
    if (!b) err_("DIVISION BY ZERO ");
    return b == -1 ? (int)(0u - (unsigned)a) : a / b;
}
static int rt_mod(int a, int b)
{
    if (!b) err_("DIVISION BY ZERO ");
    return b == -1 ? 0 : a % b;
}
static int isqrt(int n)
{
    unsigned x, r = 0, bit = 1u << 30;
    if (n <= 0) return 0;
    x = (unsigned)n;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}
static int rt_fn(int id, int v)
{
    switch (id) {
    case FN_ABS: return v < 0 ? (int)(0u - (unsigned)v) : v;
    case FN_SGN: return v > 0 ? 1 : (v < 0 ? -1 : 0);
    case FN_SQR: return isqrt(v);
    default:
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return v > 0 ? (int)(rng % (unsigned)v) : 0;
    }
}
static int rt_len(const char *s) { return slen(s); }
static int rt_asc(const char *s) { return (unsigned char)s[0]; }
static int rt_val(const char *s)
{
    int i = 0, neg = 0;
    unsigned v = 0;
    while (s[i] == ' ') i++;
    if (s[i] == '-') { neg = 1; i++; } else if (s[i] == '+') i++;
    while (s[i] >= '0' && s[i] <= '9') v = v * 10u + (unsigned)(s[i++] - '0');
    return neg ? (int)(0u - v) : (int)v;
}
static char *rt_chr(int v) { char *t = tmp(); if (v > 0 && v < 256) { t[0] = (char)v; t[1] = 0; } return t; }
static char *rt_str(int v) { char *t = tmp(); if (v >= 0) { t[0] = ' '; itoa_(t + 1, v); } else itoa_(t, v); return t; }   /* como el C64: " 5" */
static char *rt_left(const char *s, int a)
{
    char *t = tmp();
    int n = slen(s), i;
    if (a < 0) a = 0;
    if (a > n) a = n;
    for (i = 0; i < a; i++) t[i] = s[i];
    t[i] = 0;
    return t;
}
static char *rt_right(const char *s, int a)
{
    char *t = tmp();
    int n = slen(s), i;
    if (a < 0) a = 0;
    if (a > n) a = n;
    for (i = 0; i < a; i++) t[i] = s[n - a + i];
    t[i] = 0;
    return t;
}
static char *rt_mid(const char *s, int a, int b)
{
    char *t = tmp();
    int n = slen(s), i;
    if (a < 1 || b < 0) err_("ILLEGAL QUANTITY ");
    for (i = 0; a - 1 + i < n && i < b && i < SMAX; i++) t[i] = s[a - 1 + i];
    t[i] = 0;
    return t;
}
static char *rt_inkey(void) { char *t = tmp(); int c = K->poll(); if (c) { t[0] = (char)c; t[1] = 0; } return t; }
static char *rt_cat(const char *a, const char *b)
{
    char *t = tmp();
    int n = 0, i;
    for (i = 0; a[i] && n < SMAX; i++) t[n++] = a[i];
    for (i = 0; b[i] && n < SMAX; i++) t[n++] = b[i];
    t[n] = 0;
    return t;
}
static void rt_sset(int var, const char *s)
{
    char t[SMAX + 1];
    scpy(t, s, SMAX);                           /* s puede ser la misma variable */
    scpy(svars[var], t, SMAX);
}
static int rt_scmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static void rt_for(int var, int limit, int step, unsigned long long resume)
{
    int i;
    for (i = 0; i < fsp; i++)
        if (fst[i].var == var) { fsp = i; break; }
    if (fsp >= FMAX) err_("OUT OF MEMORY ");
    fst[fsp].var = var; fst[fsp].limit = limit; fst[fsp].step = step; fst[fsp].resume = resume;
    fsp++;
}
static unsigned long long rt_next(int var)
{
    int i;
    if (!fsp) err_("NEXT WITHOUT FOR ");
    if (var >= 0) {
        for (i = fsp - 1; i >= 0; i--)
            if (fst[i].var == var) break;
        if (i < 0) err_("NEXT WITHOUT FOR ");
        fsp = i + 1;
    }
    i = fsp - 1;
    vars[fst[i].var] = (int)((unsigned)vars[fst[i].var] + (unsigned)fst[i].step);
    if (fst[i].step >= 0 ? vars[fst[i].var] <= fst[i].limit : vars[fst[i].var] >= fst[i].limit)
        return fst[i].resume;
    fsp--;
    return 0;
}
static unsigned long long rt_goto(int n)
{
    const struct b2b_line *l = (const struct b2b_line *)(unsigned long)b2b_ext.lines;
    unsigned i;
    for (i = 0; i < b2b_ext.nlines; i++)
        if (l[i].num == n) return l[i].addr;
    err_("UNDEF'D STATEMENT ");
    return 0;
}
static void rt_gpush(unsigned long long ret)
{
    if (gsp >= GMAX) err_("OUT OF MEMORY ");
    gstack[gsp++] = ret;
}
static unsigned long long rt_gpop(void)
{
    if (!gsp) err_("RETURN WITHOUT GOSUB ");
    return gstack[--gsp];
}
static void rt_stop(void)
{
    char b[16];
    if (outcol) out("\n");
    out("BREAK IN "); itoa_(b, cur_line); out(b); out("\n");
    rt_abort();
}
static void rt_fail(int code)
{
    static const char *const m[] = { "SYNTAX ", "TYPE MISMATCH ", "ILLEGAL QUANTITY " };
    err_(m[code >= 0 && code < 3 ? code : 0]);
}

/* ---- sentencias y funciones que hace el interprete: se arma el texto con los valores ya calculados ---- */
static void cb_add(const char *s)
{
    int i;
    for (i = 0; s[i] && cb[cbsp - 1].n < 126; i++) cb[cbsp - 1].buf[cb[cbsp - 1].n++] = s[i];
    cb[cbsp - 1].buf[cb[cbsp - 1].n] = 0;
}
static void cb_sep(void)
{
    if (cb[cbsp - 1].paren) cb_add(cb[cbsp - 1].args ? "," : "(");
    else cb_add(cb[cbsp - 1].args ? "," : " ");
    cb[cbsp - 1].args++;
}
static void rt_cb_begin(const char *name)
{
    if (cbsp >= CBMAX) err_("FORMULA TOO COMPLEX ");
    cbsp++;
    cb[cbsp - 1].n = 0; cb[cbsp - 1].args = 0; cb[cbsp - 1].paren = 0;
    cb_add(name);
}
static void rt_ev_begin(const char *name) { rt_cb_begin(name); cb[cbsp - 1].paren = 1; }
static void rt_cb_num(int v) { char b[16]; cb_sep(); itoa_(b, v); cb_add(b); }
static void rt_cb_str(const char *s) { cb_sep(); cb_add("\""); cb_add(s); cb_add("\""); }
static void rt_cb_word(const char *w) { cb_sep(); cb_add(w); }
static void rt_cb_do(void)
{
    char b[128];
    scpy(b, cb[cbsp - 1].buf, 127);
    cbsp--;
    outcol = 0;                                 /* el interprete escribe por su cuenta */
    if (K->basic_do(b) < 0) {
        char n[16];
        out("(EN LA LINEA "); itoa_(n, cur_line); out(n); out(")\n");
        rt_abort();
    }
    outcol = K->col();
}
static int rt_ev_end(void)
{
    char b[128];
    int v = 0;
    if (cb[cbsp - 1].args) cb_add(")");
    scpy(b, cb[cbsp - 1].buf, 127);
    cbsp--;
    if (K->basic_eval(b, &v) < 0) {
        char n[16];
        out("(EN LA LINEA "); itoa_(n, cur_line); out(n); out(")\n");
        rt_abort();
    }
    return v;
}

void *rt_fns[RT_NFUNCS] = {
    [RT_LINE] = (void *)rt_line, [RT_PRN] = (void *)rt_prn, [RT_PRS] = (void *)rt_prs, [RT_PRTAB] = (void *)rt_prtab,
    [RT_PRNL] = (void *)rt_prnl, [RT_INN] = (void *)rt_inn, [RT_INS] = (void *)rt_ins, [RT_DIV] = (void *)rt_div,
    [RT_MOD] = (void *)rt_mod, [RT_FN] = (void *)rt_fn, [RT_LEN] = (void *)rt_len, [RT_ASC] = (void *)rt_asc,
    [RT_VAL] = (void *)rt_val, [RT_CHR] = (void *)rt_chr, [RT_STR] = (void *)rt_str, [RT_LEFT] = (void *)rt_left,
    [RT_RIGHT] = (void *)rt_right, [RT_MID] = (void *)rt_mid, [RT_INKEY] = (void *)rt_inkey, [RT_CAT] = (void *)rt_cat,
    [RT_SSET] = (void *)rt_sset, [RT_SCMP] = (void *)rt_scmp, [RT_FOR] = (void *)rt_for, [RT_NEXT] = (void *)rt_next,
    [RT_GOTO] = (void *)rt_goto, [RT_GPUSH] = (void *)rt_gpush, [RT_GPOP] = (void *)rt_gpop, [RT_STOP] = (void *)rt_stop,
    [RT_FAIL] = (void *)rt_fail, [RT_CB_BEGIN] = (void *)rt_cb_begin, [RT_CB_NUM] = (void *)rt_cb_num,
    [RT_CB_STR] = (void *)rt_cb_str, [RT_CB_WORD] = (void *)rt_cb_word, [RT_CB_DO] = (void *)rt_cb_do,
    [RT_EV_BEGIN] = (void *)rt_ev_begin, [RT_EV_END] = (void *)rt_ev_end,
};

int mod_main(const struct kapi *k)
{
    int i;
    K = k;
    if (K->ver < 5) {
        K->print("Este programa compilado necesita un PC BASIC mas nuevo (kapi 5).\n");
        return 1;
    }
    for (i = 0; i < 26; i++) { vars[i] = 0; svars[i][0] = 0; }
    gsp = fsp = cbsp = pool_at = 0;
    rng = K->seed() | 1u;
    outcol = K->col();
    ((void (*)(void))(unsigned long)b2b_ext.code)();
    if (outcol) out("\n");
    return 0;
}
