/* b2b.c - compilador bas2bin (ver b2b.h). No usa libc: se compila igual para Linux (main.c) y dentro de PKG.APP.
 *
 * Modo nativo: cada linea BASIC se traduce a x86-64. Las expresiones se evaluan en EAX (numeros, enteros de 32 bits
 * como el interprete) o RAX (direccion de una cadena), con la pila para los operandos pendientes. Las variables
 * A-Z / A$-Z$ estan en el runtime y se tocan por su direccion absoluta (el modulo vive en 0x9000000, todo cabe en
 * 32 bits). Lo que no es aritmetica ni control de flujo lo hacen funciones del runtime por la tabla fns[].
 * Sentencias que el compilador no conoce (PLOT, SOUND, LOCATE, POKE...) y funciones como PEEK o XMAX: los
 * argumentos se calculan en nativo y el texto ya con numeros se le pasa al interprete (kapi basic_do / basic_eval).
 * GPL-3.0-or-later. */
#include "b2b.h"
#include "b2b_blobs.h"

typedef unsigned char b2u8;
typedef unsigned int b2u32;

static b2u32 rd32(const b2u8 *p) { return (b2u32)p[0] | (b2u32)p[1] << 8 | (b2u32)p[2] << 16 | (b2u32)p[3] << 24; }
static void wr32(b2u8 *p, b2u32 v) { p[0] = (b2u8)v; p[1] = (b2u8)(v >> 8); p[2] = (b2u8)(v >> 16); p[3] = (b2u8)(v >> 24); }
static int find_ext(const b2u8 *b, int n, b2u32 magic)
{
    int i;
    for (i = 16; i + 8 <= n && i < 4096; i += 4)
        if (rd32(b + i) == magic) return i;
    return -1;
}
static void put_msg(char *msg, int max, const char *a, int num, const char *b)
{
    int k = 0, i;
    char t[12];
    if (!msg || max <= 0) return;
    for (i = 0; a && a[i] && k < max - 1; i++) msg[k++] = a[i];
    if (num >= 0) {
        int u = num, m = 0;
        do { t[m++] = (char)('0' + u % 10); u /= 10; } while (u);
        while (m && k < max - 1) msg[k++] = t[--m];
    }
    for (i = 0; b && b[i] && k < max - 1; i++) msg[k++] = b[i];
    msg[k] = 0;
}
static int copy_runtime(const b2u8 *blob, int bn, unsigned char *out, int max, int *endoff)
{
    int i, end;
    if (bn < 16 || rd32(blob) != 0x4D424350u || rd32(blob + 12) != B2B_BASE) return -1;
    end = (int)(rd32(blob + 4) - B2B_BASE);     /* incluye el .bss: el programa va despues */
    end = (end + 15) & ~15;
    if (end < bn || end > max) return -1;
    for (i = 0; i < bn; i++) out[i] = blob[i];
    for (; i < end; i++) out[i] = 0;
    *endoff = end;
    return 0;
}


#ifndef B2B_NO_NATIVE
static unsigned long long rd64(const b2u8 *p)
{
    unsigned long long v = 0;
    int i;
    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
static void wr64(b2u8 *p, unsigned long long v) { int i; for (i = 0; i < 8; i++) p[i] = (b2u8)(v >> (8 * i)); }
#define B2_MAXL   304                     /* como el interprete */
#define B2_LLEN   128
#define B2_MAXFX  6000
#define B2_MAXLB  4000
#define B2_LITMAX 32768

static struct { int num; char text[B2_LLEN]; } b2_lines[B2_MAXL];
static int b2_nl;
static int b2_lineoff[B2_MAXL];

static b2u8 *O;
static int On, Omax, b2_code0;
static int b2_depth;
static const char *b2_err;
static int b2_curnum;
static const char *P;                     /* texto que se esta compilando */
static unsigned long long b2_vars, b2_svars, b2_saved, b2_fns;

static struct { int pos, kind, val; } b2_fx[B2_MAXFX];
static int b2_nfx;
enum { FX_LINE, FX_LREL, FX_LABS, FX_LIT };
static int b2_lbl[B2_MAXLB];
static int b2_nlbl;
static char b2_lit[B2_LITMAX];
static int b2_nlit;

/* ---------------- utilidades sin libc ---------------- */
static int b2_up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int b2_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int b2_digit(int c) { return c >= '0' && c <= '9'; }
static void b2_fail(const char *m) { if (!b2_err) b2_err = m; }
static void b2_ws(void) { while (*P == ' ' || *P == '\t') P++; }
static int b2_kwat(const char *p, const char *w)
{
    while (*w) { if (b2_up(*p) != *w) return 0; p++; w++; }
    return !b2_alpha(*p);
}
static int b2_kw(const char *w)
{
    const char *p = P;
    int n = 0;
    b2_ws();
    p = P;
    if (!b2_kwat(p, w)) return 0;
    while (w[n]) n++;
    P += n;
    return 1;
}

/* ---------------- emision de codigo ---------------- */
static void e1(int b) { if (On < Omax) O[On] = (b2u8)b; else b2_fail("EL PROGRAMA COMPILADO NO CABE"); On++; }
static void e4(b2u32 v) { e1((int)(v & 255)); e1((int)((v >> 8) & 255)); e1((int)((v >> 16) & 255)); e1((int)(v >> 24)); }
static void e2b(int a, int b) { e1(a); e1(b); }
static void e3b(int a, int b, int c) { e1(a); e1(b); e1(c); }
static void fixup(int kind, int val)
{
    if (b2_nfx >= B2_MAXFX) { b2_fail("PROGRAMA DEMASIADO GRANDE"); return; }
    b2_fx[b2_nfx].pos = On; b2_fx[b2_nfx].kind = kind; b2_fx[b2_nfx].val = val; b2_nfx++;
    e4(0);
}
static int newlbl(void)
{
    if (b2_nlbl >= B2_MAXLB) { b2_fail("PROGRAMA DEMASIADO GRANDE"); return 0; }
    b2_lbl[b2_nlbl] = -1;
    return b2_nlbl++;
}
static void putlbl(int l) { b2_lbl[l] = On; }
static b2u32 absaddr(unsigned long long a) { return (b2u32)a; }

static void push_rax(void) { e1(0x50); b2_depth++; }
static void pop_rax(void)  { e1(0x58); b2_depth--; }
static void pop_rdi(void)  { e1(0x5F); b2_depth--; }
static void pop_rsi(void)  { e1(0x5E); b2_depth--; }
static void mov_eax_imm(b2u32 v) { e1(0xB8); e4(v); }
static void mov_edi_imm(b2u32 v) { e1(0xBF); e4(v); }
static void mov_edi_eax(void) { e2b(0x89, 0xC7); }
static void mov_esi_eax(void) { e2b(0x89, 0xC6); }
static void mov_edx_eax(void) { e2b(0x89, 0xC2); }
static void mov_ecx_eax(void) { e2b(0x89, 0xC1); }
static void mov_rdi_rax(void) { e3b(0x48, 0x89, 0xC7); }
static void mov_rsi_rax(void) { e3b(0x48, 0x89, 0xC6); }
static void load_var(int i)  { e3b(0x8B, 0x04, 0x25); e4(absaddr(b2_vars) + 4u * (b2u32)i); }
static void store_var(int i) { e3b(0x89, 0x04, 0x25); e4(absaddr(b2_vars) + 4u * (b2u32)i); }
static void ecall(int f)
{
    int pad = b2_depth & 1;                    /* la pila debe ir alineada a 16 en cada call */
    if (pad) { e1(0x48); e1(0x83); e1(0xEC); e1(8); }
    e3b(0xFF, 0x14, 0x25);
    e4(absaddr(b2_fns) + 8u * (b2u32)f);
    if (pad) { e1(0x48); e1(0x83); e1(0xC4); e1(8); }
}
static void jmp_line(int n) { e1(0xE9); fixup(FX_LINE, n); }
static void jmp_lbl(int l) { e1(0xE9); fixup(FX_LREL, l); }
static void je_lbl(int l) { e2b(0x0F, 0x84); fixup(FX_LREL, l); }
static void lit_addr(const char *s, int n)  /* mov eax, direccion de una cadena literal */
{
    int i, at = b2_nlit;
    if (b2_nlit + n + 1 > B2_LITMAX) { b2_fail("DEMASIADAS CADENAS"); return; }
    for (i = 0; i < n; i++) b2_lit[b2_nlit++] = s[i];
    b2_lit[b2_nlit++] = 0;
    e1(0xB8); fixup(FX_LIT, at);
}

/* ---------------- expresiones ---------------- */
static int n_expr(void);
static void s_expr(void);

static int str_start(void)
{
    b2_ws();
    if (*P == '"') return 1;
    if (b2_alpha(*P) && P[1] == '$') return 1;
    return b2_kwat(P, "CHR$") || b2_kwat(P, "STR$") || b2_kwat(P, "LEFT$") || b2_kwat(P, "RIGHT$") ||
           b2_kwat(P, "MID$") || b2_kwat(P, "INKEY$");
}
static void expect(int c) { b2_ws(); if (*P == c) P++; else b2_fail("SYNTAX"); }

/* palabra de 2+ letras (con $ opcional): nombre de una sentencia o funcion */
static int word(char *w, int max)
{
    int n = 0;
    b2_ws();
    while (b2_alpha(*P) && n < max - 2) w[n++] = (char)b2_up(*P++);
    if (*P == '$' && n) w[n++] = *P++;
    w[n] = 0;
    return n;
}

static void s_primary(void)
{
    b2_ws();
    if (*P == '"') {
        const char *s = ++P;
        int n = 0;
        while (s[n] && s[n] != '"') n++;
        lit_addr(s, n);
        P = s + n;
        if (*P == '"') P++;
    } else if (b2_alpha(*P) && P[1] == '$') {
        mov_eax_imm(absaddr(b2_svars) + 128u * (b2u32)(b2_up(*P) - 'A'));
        P += 2;
    } else if (b2_kwat(P, "CHR$") || b2_kwat(P, "STR$")) {
        int chr = b2_kw("CHR$");
        if (!chr) b2_kw("STR$");
        expect('('); n_expr(); expect(')');
        mov_edi_eax(); ecall(chr ? RT_CHR : RT_STR);
    } else if (b2_kwat(P, "LEFT$") || b2_kwat(P, "RIGHT$")) {
        int left = b2_kw("LEFT$");
        if (!left) b2_kw("RIGHT$");
        expect('('); s_expr(); push_rax(); expect(','); n_expr(); expect(')');
        mov_esi_eax(); pop_rdi(); ecall(left ? RT_LEFT : RT_RIGHT);
    } else if (b2_kw("MID$")) {
        expect('('); s_expr(); push_rax(); expect(','); n_expr(); push_rax();
        b2_ws();
        if (*P == ',') { P++; n_expr(); } else mov_eax_imm(127);
        expect(')');
        mov_edx_eax(); pop_rsi(); pop_rdi(); ecall(RT_MID);
    } else if (b2_kw("INKEY$")) {
        ecall(RT_INKEY);
    } else {
        b2_fail("TYPE MISMATCH");
    }
}
static void s_expr(void)
{
    s_primary();
    for (;;) {
        b2_ws();
        if (*P != '+') break;
        P++;
        push_rax(); s_primary(); mov_rsi_rax(); pop_rdi(); ecall(RT_CAT);
    }
}

static const char *const zero_arg_fns[] = { "XMAX", "YMAX", "MOUSEX", "MOUSEY", "MOUSEB", "CDTRACK", "CDTIME", 0 };
static int is_zero_arg_fn(const char *w)
{
    int i, k;
    for (i = 0; zero_arg_fns[i]; i++) {
        for (k = 0; w[k] && w[k] == zero_arg_fns[i][k]; k++)
            ;
        if (!w[k] && !zero_arg_fns[i][k]) return 1;
    }
    return 0;
}

/* argumento de una sentencia/funcion que hace el interprete */
static void cb_arg(void)
{
    const char *save;
    char w[24];
    int n;
    b2_ws();
    if (str_start()) { s_expr(); mov_rdi_rax(); ecall(RT_CB_STR); return; }
    save = P;
    if (b2_alpha(P[0]) && b2_alpha(P[1])) {    /* palabra suelta (SINE, KICK, OFF...) se pasa tal cual */
        n = word(w, sizeof w);
        b2_ws();
        if (*P != '(' && !is_zero_arg_fn(w)) {
            lit_addr(w, n); mov_rdi_rax(); ecall(RT_CB_WORD);
            return;
        }
        P = save;                              /* PEEK(...), XMAX...: es una expresion */
    }
    n_expr(); mov_edi_eax(); ecall(RT_CB_NUM);
}

static int n_primary(void)
{
    b2_ws();
    if (b2_digit(*P)) {
        b2u32 v = 0;
        while (b2_digit(*P)) v = v * 10u + (b2u32)(*P++ - '0');
        mov_eax_imm(v);
        return 1;
    }
    if (*P == '&') {
        b2u32 v = 0;
        P++;
        if (b2_up(*P) == 'H') P++;
        for (;;) {
            int c = b2_up(*P), h = c >= '0' && c <= '9' ? c - '0' : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1);
            if (h < 0) break;
            v = (v << 4) | (b2u32)h;
            P++;
        }
        mov_eax_imm(v);
        return 1;
    }
    if (*P == '(') { P++; n_expr(); expect(')'); return 1; }
    if (b2_kw("ABS") || b2_kwat(P, "SGN") || b2_kwat(P, "SQR") || b2_kwat(P, "RND")) {
        int id = FN_ABS;
        if (b2_kw("SGN")) id = FN_SGN; else if (b2_kw("SQR")) id = FN_SQR; else if (b2_kw("RND")) id = FN_RND;
        expect('('); n_expr(); expect(')');
        mov_esi_eax(); mov_edi_imm((b2u32)id); ecall(RT_FN);
        return 1;
    }
    if (b2_kw("LEN") || b2_kwat(P, "ASC") || b2_kwat(P, "VAL")) {
        int f = RT_LEN;
        if (b2_kw("ASC")) f = RT_ASC; else if (b2_kw("VAL")) f = RT_VAL;
        expect('('); s_expr(); expect(')');
        mov_rdi_rax(); ecall(f);
        return 1;
    }
    if (*P == '"' || (b2_alpha(*P) && P[1] == '$')) { b2_fail("TYPE MISMATCH"); return 0; }
    if (b2_alpha(*P) && !b2_alpha(P[1])) {      /* variable A-Z */
        load_var(b2_up(*P) - 'A');
        P++;
        return 1;
    }
    if (b2_alpha(*P)) {                          /* PEEK(..), XMAX, MOUSEX, FRE(0)...: los calcula el interprete */
        char w[24];
        int n = word(w, sizeof w);
        lit_addr(w, n); mov_rdi_rax(); ecall(RT_EV_BEGIN);
        b2_ws();
        if (*P == '(') {
            P++;
            for (;;) {
                cb_arg();
                b2_ws();
                if (*P == ',') { P++; continue; }
                break;
            }
            expect(')');
        }
        ecall(RT_EV_END);
        return 1;
    }
    b2_fail("SYNTAX");
    return 0;
}
static void n_unary(void)
{
    b2_ws();
    if (*P == '-') { P++; n_unary(); e2b(0xF7, 0xD8); return; }   /* neg eax */
    if (*P == '+') { P++; n_unary(); return; }
    n_primary();
}
static void n_mul(void)
{
    n_unary();
    for (;;) {
        int op = 0;
        b2_ws();
        if (*P == '*') { op = 1; P++; }
        else if (*P == '/') { op = 2; P++; }
        else if (*P == '%') { op = 3; P++; }
        else if (b2_kw("MOD")) op = 3;
        if (!op) break;
        push_rax(); n_unary();
        if (op == 1) { mov_ecx_eax(); pop_rax(); e3b(0x0F, 0xAF, 0xC1); }      /* imul eax, ecx */
        else { mov_esi_eax(); pop_rdi(); ecall(op == 2 ? RT_DIV : RT_MOD); }
    }
}
static void n_add(void)
{
    n_mul();
    for (;;) {
        int sub;
        b2_ws();
        if (*P != '+' && *P != '-') break;
        sub = *P++ == '-';
        push_rax(); n_mul(); mov_ecx_eax(); pop_rax();
        e2b(sub ? 0x29 : 0x01, 0xC8);           /* sub/add eax, ecx */
    }
}
static int relop(void)                         /* devuelve el codigo setcc o 0 */
{
    b2_ws();
    if (P[0] == '<' && P[1] == '>') { P += 2; return 0x95; }
    if (P[0] == '<' && P[1] == '=') { P += 2; return 0x9E; }
    if (P[0] == '>' && P[1] == '=') { P += 2; return 0x9D; }
    if (P[0] == '<') { P++; return 0x9C; }
    if (P[0] == '>') { P++; return 0x9F; }
    if (P[0] == '=') { P++; return 0x94; }
    return 0;
}
static void setcc_bool(int cc) { e3b(0x0F, cc, 0xC0); e3b(0x0F, 0xB6, 0xC0); e2b(0xF7, 0xD8); }  /* -1 / 0 */
static void str_compare_rest(void)             /* RAX = cadena izquierda; lee op y cadena derecha */
{
    int cc = relop();
    if (!cc) { b2_fail("TYPE MISMATCH"); return; }
    push_rax(); s_expr(); mov_rsi_rax(); pop_rdi(); ecall(RT_SCMP);
    e2b(0x85, 0xC0);                           /* test eax, eax */
    setcc_bool(cc);
}
static void n_cmp(void)
{
    int cc;
    if (str_start()) { s_expr(); str_compare_rest(); return; }
    n_add();
    cc = relop();
    if (!cc) return;
    push_rax(); n_add(); mov_ecx_eax(); pop_rax();
    e2b(0x39, 0xC8);                           /* cmp eax, ecx */
    setcc_bool(cc);
}
static void n_not(void)
{
    if (b2_kw("NOT")) { n_not(); e2b(0xF7, 0xD0); return; }   /* not eax */
    n_cmp();
}
static void n_and(void)
{
    n_not();
    while (b2_kw("AND")) { push_rax(); n_not(); mov_ecx_eax(); pop_rax(); e2b(0x21, 0xC8); }
}
static int n_expr(void)
{
    n_and();
    for (;;) {
        int op;
        if (b2_kw("OR")) op = 0x09; else if (b2_kw("XOR")) op = 0x31; else break;
        push_rax(); n_and(); mov_ecx_eax(); pop_rax(); e2b(op, 0xC8);
    }
    return 1;
}

/* ---------------- sentencias ---------------- */
static int const_line(int *n)                  /* GOTO 100: numero fijo? */
{
    const char *s;
    int v = 0;
    b2_ws();
    s = P;
    if (!b2_digit(*s)) return 0;
    while (b2_digit(*s)) v = v * 10 + (*s++ - '0');
    while (*s == ' ') s++;
    if (*s && *s != ':') return 0;             /* GOTO 100+X: calculado */
    P = s;
    *n = v;
    return 1;
}
static int line_exists(int n) { int i; for (i = 0; i < b2_nl; i++) if (b2_lines[i].num == n) return 1; return 0; }

static void st_print(void)
{
    int nl = 1;
    for (;;) {
        b2_ws();
        if (*P == 0 || *P == ':') break;
        if (str_start()) {
            s_expr();
            b2_ws();
            if (*P == '=' || *P == '<' || *P == '>') { str_compare_rest(); mov_edi_eax(); ecall(RT_PRN); }
            else { mov_rdi_rax(); ecall(RT_PRS); }
        } else {
            n_expr(); mov_edi_eax(); ecall(RT_PRN);
        }
        if (b2_err) return;
        nl = 1;
        b2_ws();
        if (*P == ';') { P++; nl = 0; }
        else if (*P == ',') { P++; nl = 0; ecall(RT_PRTAB); }
    }
    if (nl) ecall(RT_PRNL);
}
static void st_input(void)
{
    b2_ws();
    if (*P == '"') {
        s_primary(); mov_rdi_rax(); ecall(RT_PRS);
        b2_ws();
        if (*P == ';' || *P == ',') P++;
    }
    for (;;) {
        int v;
        b2_ws();
        if (!b2_alpha(*P)) { b2_fail("SYNTAX"); return; }
        v = b2_up(*P++) - 'A';
        if (*P == '$') { P++; mov_edi_imm((b2u32)v); ecall(RT_INS); }
        else { mov_edi_imm((b2u32)v); ecall(RT_INN); }
        b2_ws();
        if (*P != ',') break;
        P++;
    }
}
static void st_goto(int gosub)
{
    int n, ret = 0;
    if (gosub) ret = newlbl();
    if (const_line(&n)) {
        if (!line_exists(n)) { b2_fail("UNDEF'D STATEMENT"); return; }
        if (gosub) { e1(0xBF); fixup(FX_LABS, ret); ecall(RT_GPUSH); }
        jmp_line(n);
    } else {
        n_expr(); mov_edi_eax(); ecall(RT_GOTO);
        if (gosub) { push_rax(); e1(0xBF); fixup(FX_LABS, ret); ecall(RT_GPUSH); pop_rax(); }
        e2b(0xFF, 0xE0);                       /* jmp rax */
    }
    if (gosub) putlbl(ret);
}
static void st_for(void)
{
    int v, resume = newlbl();
    b2_ws();
    if (!b2_alpha(*P) || b2_alpha(P[1])) { b2_fail("SYNTAX"); return; }
    v = b2_up(*P++) - 'A';
    expect('=');
    n_expr(); store_var(v);
    if (!b2_kw("TO")) { b2_fail("SYNTAX"); return; }
    n_expr(); push_rax();
    if (b2_kw("STEP")) n_expr(); else mov_eax_imm(1);
    mov_edx_eax(); pop_rsi(); mov_edi_imm((b2u32)v);
    e1(0xB9); fixup(FX_LABS, resume);          /* mov ecx, direccion de regreso */
    ecall(RT_FOR);
    putlbl(resume);
}
static void st_next(void)
{
    int v = -1, after = newlbl();
    b2_ws();
    if (b2_alpha(*P) && !b2_alpha(P[1])) v = b2_up(*P++) - 'A';
    mov_edi_imm((b2u32)v); ecall(RT_NEXT);
    e3b(0x48, 0x85, 0xC0);                     /* test rax, rax */
    je_lbl(after);
    e2b(0xFF, 0xE0);
    putlbl(after);
}
static void st_generic(void)                   /* PLOT, SOUND, LOCATE, POKE, CLS...: los hace el interprete */
{
    char w[24];
    int n = word(w, sizeof w);
    if (!n) { b2_fail("SYNTAX"); return; }
    lit_addr(w, n); mov_rdi_rax(); ecall(RT_CB_BEGIN);
    b2_ws();
    if (*P && *P != ':') {
        for (;;) {
            cb_arg();
            if (b2_err) return;
            b2_ws();
            if (*P == ',') { P++; continue; }
            break;
        }
    }
    ecall(RT_CB_DO);
}

static int b2_endlbl, b2_lineend;

static void statement(void)
{
    b2_ws();
    if (*P == 0 || *P == ':') return;
    if (b2_kwat(P, "REM") || (b2_up(P[0]) == 'R' && b2_up(P[1]) == 'E' && b2_up(P[2]) == 'M') || *P == '\'') {
        while (*P) P++;
        return;
    }
    if (*P == '?') { P++; st_print(); return; }
    if (b2_kw("PRINT")) { st_print(); return; }
    if (b2_kw("IF")) {
        n_expr();
        e2b(0x85, 0xC0);                       /* falso: se salta el resto de la linea, como el interprete */
        je_lbl(b2_lineend);
        if (b2_kw("THEN")) {
            int n;
            if (const_line(&n)) {
                if (!line_exists(n)) { b2_fail("UNDEF'D STATEMENT"); return; }
                jmp_line(n);
            } else {
                statement();
            }
        } else if (b2_kw("GOTO")) {
            st_goto(0);
        } else {
            b2_fail("SYNTAX");
        }
        return;
    }
    if (b2_kw("GOTO")) { st_goto(0); return; }
    if (b2_kw("GOSUB")) { st_goto(1); return; }
    if (b2_kw("RETURN")) { ecall(RT_GPOP); e2b(0xFF, 0xE0); return; }
    if (b2_kw("FOR")) { st_for(); return; }
    if (b2_kw("NEXT")) { st_next(); return; }
    if (b2_kw("END")) { jmp_lbl(b2_endlbl); return; }
    if (b2_kw("STOP")) { ecall(RT_STOP); return; }
    if (b2_kw("INPUT")) { st_input(); return; }
    b2_kw("LET");
    b2_ws();
    if (b2_alpha(*P) && P[1] == '$') {         /* A$ = ... */
        int v = b2_up(*P) - 'A';
        P += 2;
        expect('=');
        s_expr(); mov_rsi_rax(); mov_edi_imm((b2u32)v); ecall(RT_SSET);
        return;
    }
    if (b2_alpha(*P) && !b2_alpha(P[1])) {     /* A = ... */
        int v = b2_up(*P) - 'A';
        P++;
        expect('=');
        n_expr(); store_var(v);
        return;
    }
    st_generic();
}

/* ---------------- programa ---------------- */
static int b2_parse(const char *src, int n)
{
    int i = 0;
    b2_nl = 0;
    while (i < n) {
        int num = 0, k = 0, inq = 0, j, digits = 0;
        char t[B2_LLEN];
        while (i < n && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) i++;
        while (i < n && b2_digit(src[i])) { num = num * 10 + (src[i++] - '0'); digits++; if (num > 65535) break; }
        if (i < n && src[i] == ' ') i++;
        while (i < n && src[i] != '\n') {
            char c = src[i++];
            if (c == '\r') continue;
            if (c == '"') inq = !inq;
            if (k < B2_LLEN - 1) t[k++] = inq ? c : (char)b2_up(c);
        }
        if (i < n) i++;
        t[k] = 0;
        if (!digits) {
            for (j = 0; t[j] == ' '; j++) ;
            if (t[j]) { b2_curnum = 0; return -1; }   /* linea sin numero */
            continue;
        }
        if (num < 1 || num > 65535) { b2_curnum = num; return -1; }
        for (j = 0; j < b2_nl && b2_lines[j].num < num; j++) ;
        if (j < b2_nl && b2_lines[j].num == num) {
            int q;
            if (!k) { for (q = j; q + 1 < b2_nl; q++) b2_lines[q] = b2_lines[q + 1]; b2_nl--; continue; }
        } else {
            int q;
            if (!k) continue;
            if (b2_nl >= B2_MAXL) { b2_curnum = num; return -2; }
            for (q = b2_nl; q > j; q--) b2_lines[q] = b2_lines[q - 1];
            b2_nl++;
        }
        b2_lines[j].num = num;
        for (k = 0; t[k]; k++) b2_lines[j].text[k] = t[k];
        b2_lines[j].text[k] = 0;
    }
    return 0;
}

#endif /* !B2B_NO_NATIVE */

static int pack(const char *src, int n, unsigned char *out, int max, char *msg, int msgmax)
{
    int end, ext, i, total;
    if (copy_runtime(b2b_rt_pack, (int)b2b_rt_pack_len, out, max, &end) < 0 ||
        (ext = find_ext(out, end, B2B_PMAGIC)) < 0) {
        put_msg(msg, msgmax, "runtime empaquetador danado", -1, 0);
        return -1;
    }
    if (end + n + 1 > max) { put_msg(msg, msgmax, "el programa no cabe", -1, 0); return -1; }
    for (i = 0; i < n; i++) out[end + i] = (unsigned char)src[i];
    out[end + n] = 0;
    total = (end + n + 1 + 15) & ~15;
    for (i = end + n + 1; i < total; i++) out[i] = 0;
    wr32(out + ext + 4, B2B_BASE + (b2u32)end);
    wr32(out + ext + 8, (b2u32)n);
    wr32(out + 4, B2B_BASE + (b2u32)total);
    return total;
}

#ifndef B2B_NO_NATIVE
static int native(const char *src, int n, unsigned char *out, int max, char *msg, int msgmax)
{
    int end, ext, i, total, lit0, tab0, r;
    (void)msgmax;
    b2_err = 0; b2_nfx = 0; b2_nlbl = 0; b2_nlit = 0; b2_depth = 0; b2_curnum = 0;
    r = b2_parse(src, n);
    if (r < 0) {
        put_msg(msg, msgmax, r == -2 ? "demasiadas lineas (maximo 304), linea " : "linea sin numero o fuera de rango: ",
                b2_curnum, 0);
        return -1;
    }
    if (copy_runtime(b2b_rt_native, (int)b2b_rt_native_len, out, max, &end) < 0 ||
        (ext = find_ext(out, end, B2B_NMAGIC)) < 0) {
        put_msg(msg, msgmax, "runtime nativo danado", -1, 0);
        return -1;
    }
    b2_vars = rd64(out + ext + 8); b2_svars = rd64(out + ext + 16);
    b2_saved = rd64(out + ext + 24); b2_fns = rd64(out + ext + 32);
    O = out; Omax = max; On = end; b2_code0 = end;

    e1(0x53);                                  /* push rbx: la pila queda alineada a 16 */
    e1(0x48); e3b(0x89, 0x24, 0x25); e4(absaddr(b2_saved));   /* mov [saved_rsp], rsp */
    b2_endlbl = newlbl();
    for (i = 0; i < b2_nl && !b2_err; i++) {
        b2_lineoff[i] = On;
        b2_curnum = b2_lines[i].num;
        b2_lineend = newlbl();
        mov_edi_imm((b2u32)b2_curnum); ecall(RT_LINE);
        P = b2_lines[i].text;
        for (;;) {
            b2_depth = 0;
            statement();
            if (b2_err) break;
            b2_ws();
            if (*P == ':') { P++; continue; }
            if (*P) b2_fail("SYNTAX");
            break;
        }
        putlbl(b2_lineend);
    }
    if (b2_err) {
        put_msg(msg, msgmax, "?", -1, 0);
        put_msg(msg + 1, msgmax - 1, b2_err, -1, 0);
        for (i = 0; msg[i]; i++) ;
        put_msg(msg + i, msgmax - i, " ERROR EN LA LINEA ", b2_curnum, 0);
        return -1;
    }
    putlbl(b2_endlbl);
    e1(0x48); e3b(0x8B, 0x24, 0x25); e4(absaddr(b2_saved));   /* mov rsp, [saved_rsp] */
    e1(0x5B); e1(0xC3);                        /* pop rbx; ret */

    lit0 = On;                                 /* cadenas literales */
    for (i = 0; i < b2_nlit; i++) e1(b2_lit[i]);
    while (On & 3) e1(0);
    tab0 = On;                                 /* tabla de lineas (GOTO calculado) */
    for (i = 0; i < b2_nl; i++) { e4((b2u32)b2_lines[i].num); e4(B2B_BASE + (b2u32)b2_lineoff[i]); }
    while (On & 15) e1(0);
    if (b2_err || On > max) { put_msg(msg, msgmax, "el programa compilado no cabe", -1, 0); return -1; }
    total = On;

    for (i = 0; i < b2_nfx; i++) {
        int pos = b2_fx[i].pos, v = b2_fx[i].val, tgt = -1, j;
        switch (b2_fx[i].kind) {
        case FX_LINE:
            for (j = 0; j < b2_nl; j++) if (b2_lines[j].num == v) tgt = b2_lineoff[j];
            wr32(out + pos, (b2u32)(tgt - (pos + 4)));
            break;
        case FX_LREL: wr32(out + pos, (b2u32)(b2_lbl[v] - (pos + 4))); break;
        case FX_LABS: wr32(out + pos, B2B_BASE + (b2u32)b2_lbl[v]); break;
        case FX_LIT:  wr32(out + pos, B2B_BASE + (b2u32)(lit0 + v)); break;
        }
    }
    wr64(out + ext + 40, B2B_BASE + (unsigned long long)b2_code0);
    wr64(out + ext + 48, B2B_BASE + (unsigned long long)tab0);
    wr64(out + ext + 56, (unsigned long long)b2_nl);
    wr32(out + 4, B2B_BASE + (b2u32)total);
    return total;
}
#endif

int b2b_compile(const char *src, int n, int nat, unsigned char *out, int max, char *msg, int msgmax)
{
    if (msg && msgmax) msg[0] = 0;
#ifdef B2B_NO_NATIVE
    if (nat) {
        put_msg(msg, msgmax, "el modo nativo solo existe en PC BASIC de 64 bits", -1, 0);
        return -1;
    }
#else
    if (nat) return native(src, n, out, max, msg, msgmax);
#endif
    return pack(src, n, out, max, msg, msgmax);
}
