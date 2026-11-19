/* bcc_gen.c - generador de codigo x86-64 de BCC (parte de bcc.c): escribe los bytes de las instrucciones directo */

static u8 *code;
static int clen, cmax;
static int *lpos;                               /* posicion de cada etiqueta */
static int nlpos;

typedef struct Fix { struct Fix *next; int pos, kind, label; Obj *obj; i64 addend; } Fix;
enum { FX_LABEL, FX_CALL, FX_ABS, FX_THUNK, FX_STACKTOP, FX_EXIT, FX_SAVED };
static Fix *fixes;
static Obj *gen_fn;
static int ret_label, exit_pos;
static int new_label_g(void);

static void e1(int b)
{
    if (clen >= cmax) fatal("el programa es demasiado grande (codigo)");
    code[clen++] = (u8)b;
}
static void e4(unsigned v) { e1(v & 255); e1((v >> 8) & 255); e1((v >> 16) & 255); e1((v >> 24) & 255); }
static void e8(u64 v) { e4((unsigned)v); e4((unsigned)(v >> 32)); }
static void eb(const u8 *b, int n) { int i; for (i = 0; i < n; i++) e1(b[i]); }
#define E(...) do { static const u8 _b[] = { __VA_ARGS__ }; eb(_b, (int)sizeof _b); } while (0)

static void add_fix(int kind, int pos, Obj *o, i64 addend, int label)
{
    Fix *f = zalloc(sizeof *f);
    f->kind = kind; f->pos = pos; f->obj = o; f->addend = addend; f->label = label;
    f->next = fixes;
    fixes = f;
}

static void mov_rax_imm(i64 v)
{
    if (v >= -0x80000000LL && v <= 0x7FFFFFFFLL) {
        if (v >= 0) { E(0xB8); e4((unsigned)v); }               /* mov eax, imm32 (pone en cero lo de arriba) */
        else { E(0x48, 0xC7, 0xC0); e4((unsigned)v); }          /* mov rax, imm32 con signo */
    } else { E(0x48, 0xB8); e8((u64)v); }
}
static void mov_rax_addr(int kind, Obj *o, i64 addend)
{
    E(0x48, 0xB8);
    add_fix(kind, clen, o, addend, 0);
    e8(0);
}
static void jmp_label(int op, int label)                       /* op: 0 = jmp, 0x84 = je, 0x85 = jne */
{
    if (op) { E(0x0F); e1(op); } else E(0xE9);
    add_fix(FX_LABEL, clen, 0, 0, label);
    e4(0);
}
static void place_label(int label)
{
    if (label >= nlpos) fatal("etiqueta interna fuera de rango");
    lpos[label] = clen;
}
static void push_rax(void) { E(0x50); }
static void pop_rdi(void) { E(0x5F); }

static void load(Type *ty)
{
    switch (ty->kind) {
    case TY_ARRAY: case TY_STRUCT: case TY_UNION: case TY_FUNC: return;
    }
    switch (ty->size) {
    case 1: if (ty->is_unsigned) E(0x48, 0x0F, 0xB6, 0x00); else E(0x48, 0x0F, 0xBE, 0x00); return;
    case 2: if (ty->is_unsigned) E(0x48, 0x0F, 0xB7, 0x00); else E(0x48, 0x0F, 0xBF, 0x00); return;
    case 4: if (ty->is_unsigned) E(0x8B, 0x00); else E(0x48, 0x63, 0x00); return;
    default: E(0x48, 0x8B, 0x00); return;
    }
}

/* guarda rax en [rdi] */
static void store_rdi(Type *ty)
{
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        E(0x48, 0x89, 0xC6);                                    /* mov rsi, rax */
        E(0x48, 0x89, 0xF8);                                    /* mov rax, rdi */
        E(0x48, 0xC7, 0xC1); e4((unsigned)ty->size);            /* mov rcx, size */
        E(0xF3, 0xA4);                                          /* rep movsb */
        return;
    }
    switch (ty->size) {
    case 1: E(0x88, 0x07); return;
    case 2: E(0x66, 0x89, 0x07); return;
    case 4: E(0x89, 0x07); return;
    default: E(0x48, 0x89, 0x07); return;
    }
}

static void cast_to(Type *to)
{
    if (to->kind == TY_VOID) return;
    if (to->kind == TY_BOOL) { E(0x48, 0x85, 0xC0, 0x0F, 0x95, 0xC0, 0x48, 0x0F, 0xB6, 0xC0); return; }
    if (!is_integer(to)) return;
    switch (to->size) {
    case 1: if (to->is_unsigned) E(0x48, 0x0F, 0xB6, 0xC0); else E(0x48, 0x0F, 0xBE, 0xC0); return;
    case 2: if (to->is_unsigned) E(0x48, 0x0F, 0xB7, 0xC0); else E(0x48, 0x0F, 0xBF, 0xC0); return;
    case 4: if (to->is_unsigned) E(0x89, 0xC0); else E(0x48, 0x63, 0xC0); return;
    }
}

static void gen_expr(Node *n);
static void gen_stmt(Node *n);

static void gen_addr(Node *n)
{
    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local) {
            E(0x48, 0x8D, 0x85); e4((unsigned)n->var->offset);  /* lea rax, [rbp+off] */
            return;
        }
        if (n->var->is_function && n->var->callback) { mov_rax_addr(FX_THUNK, n->var, 0); return; }
        mov_rax_addr(FX_ABS, n->var, 0);
        return;
    case ND_DEREF: gen_expr(n->lhs); return;
    case ND_COMMA: gen_expr(n->lhs); gen_addr(n->rhs); return;
    case ND_MEMBER:
        gen_addr(n->lhs);
        if (n->member->offset) { E(0x48, 0x05); e4((unsigned)n->member->offset); }
        return;
    case ND_ASSIGN: case ND_COND:
        if (n->ty->kind == TY_STRUCT || n->ty->kind == TY_UNION) { gen_expr(n); return; }
        break;
    }
    error_tok(n->tok, "eso no tiene direccion (no se puede asignar ni tomar &)");
}

static void gen_funcall(Node *n)
{
    Node *a, *args[256];
    int na = 0, i;
    for (a = n->args; a; a = a->next) {
        if (na >= 256) error_tok(n->tok, "demasiados argumentos");
        args[na++] = a;
    }
    for (i = na - 1; i >= 0; i--) {                             /* el primero queda mas abajo en la pila */
        gen_expr(args[i]);
        push_rax();
    }
    if (n->lhs->kind == ND_VAR && n->lhs->var->is_function) {
        E(0xE8);
        add_fix(FX_CALL, clen, n->lhs->var, 0, 0);
        e4(0);
    } else {
        gen_expr(n->lhs);
        E(0xFF, 0xD0);                                          /* call rax */
    }
    if (na) { E(0x48, 0x81, 0xC4); e4((unsigned)(na * 8)); }   /* add rsp, 8*n */
    if (n->ty->kind == TY_BOOL) cast_to(n->ty);
}

static void gen_kcall(Node *n)
{
    static const u8 pops[6][2] = { { 0x5F, 0 }, { 0x5E, 0 }, { 0x5A, 0 }, { 0x59, 0 }, { 0x41, 0x58 }, { 0x41, 0x59 } };
    Node *a, *args[6];
    int na = 0, i;
    Obj *k = find_func("__K");
    if (!k) fatal("falta la variable __K de la biblioteca");
    for (a = n->args; a; a = a->next) args[na++] = a;
    for (i = na - 1; i >= 0; i--) { gen_expr(args[i]); push_rax(); }
    for (i = 0; i < na; i++) { if (pops[i][0] == 0x41) E(0x41); e1(pops[i][pops[i][0] == 0x41 ? 1 : 0]); }
    E(0x48, 0xB8); add_fix(FX_ABS, clen, k, 0, 0); e8(0);       /* mov rax, &__K */
    E(0x48, 0x8B, 0x00);                                        /* mov rax, [rax] */
    E(0x48, 0x8B, 0x80); e4((unsigned)n->val);                  /* mov rax, [rax+off] */
    E(0x53, 0x48, 0x89, 0xE3, 0x48, 0x83, 0xE4, 0xF0);          /* push rbx; mov rbx, rsp; and rsp, -16 */
    E(0xFF, 0xD0, 0x48, 0x89, 0xDC, 0x5B);                      /* call rax; mov rsp, rbx; pop rbx */
}

static void gen_cmp(Node *n, int cc_signed, int cc_unsigned)
{
    E(0x48, 0x39, 0xF8);                                        /* cmp rax, rdi */
    E(0x0F); e1(n->lhs->ty->is_unsigned ? cc_unsigned : cc_signed); E(0xC0);
    E(0x0F, 0xB6, 0xC0);                                        /* movzx eax, al */
}

static void gen_expr(Node *n)
{
    switch (n->kind) {
    case ND_NULL: return;
    case ND_NUM: mov_rax_imm(n->val); return;
    case ND_NEG: gen_expr(n->lhs); E(0x48, 0xF7, 0xD8); cast_to(n->ty); return;
    case ND_VAR: case ND_MEMBER: gen_addr(n); load(n->ty); return;
    case ND_DEREF: gen_expr(n->lhs); load(n->ty); return;
    case ND_ADDR: gen_addr(n->lhs); return;
    case ND_ASSIGN:
        gen_addr(n->lhs);
        push_rax();
        gen_expr(n->rhs);
        pop_rdi();
        store_rdi(n->ty);
        return;
    case ND_MEMZERO:
        E(0x48, 0x8D, 0xBD); e4((unsigned)n->var->offset);      /* lea rdi, [rbp+off] */
        E(0x48, 0xC7, 0xC1); e4((unsigned)n->var->ty->size);   /* mov rcx, size */
        E(0x31, 0xC0, 0xF3, 0xAA);                              /* xor eax, eax; rep stosb */
        return;
    case ND_COND: {
        int le = new_label_g(), lend = new_label_g();
        gen_expr(n->cond);
        E(0x48, 0x85, 0xC0);
        jmp_label(0x84, le);
        gen_expr(n->then);
        jmp_label(0, lend);
        place_label(le);
        gen_expr(n->els);
        place_label(lend);
        return;
    }
    case ND_NOT: gen_expr(n->lhs); E(0x48, 0x85, 0xC0, 0x0F, 0x94, 0xC0, 0x0F, 0xB6, 0xC0); return;
    case ND_BITNOT: gen_expr(n->lhs); E(0x48, 0xF7, 0xD0); cast_to(n->ty); return;
    case ND_LOGAND: {
        int lf = new_label_g(), lend = new_label_g();
        gen_expr(n->lhs);
        E(0x48, 0x85, 0xC0); jmp_label(0x84, lf);
        gen_expr(n->rhs);
        E(0x48, 0x85, 0xC0); jmp_label(0x84, lf);
        E(0xB8); e4(1); jmp_label(0, lend);
        place_label(lf);
        E(0x31, 0xC0);
        place_label(lend);
        return;
    }
    case ND_LOGOR: {
        int lt = new_label_g(), lend = new_label_g();
        gen_expr(n->lhs);
        E(0x48, 0x85, 0xC0); jmp_label(0x85, lt);
        gen_expr(n->rhs);
        E(0x48, 0x85, 0xC0); jmp_label(0x85, lt);
        E(0x31, 0xC0); jmp_label(0, lend);
        place_label(lt);
        E(0xB8); e4(1);
        place_label(lend);
        return;
    }
    case ND_FUNCALL: gen_funcall(n); return;
    case ND_KCALL: gen_kcall(n); return;
    case ND_EXIT:
        gen_expr(n->lhs);
        E(0x48, 0xB9); add_fix(FX_EXIT, clen, 0, 0, 0); e8(0);  /* mov rcx, salida */
        E(0xFF, 0xE1);                                          /* jmp rcx */
        return;
    case ND_POSTINC:
        gen_addr(n->lhs);
        E(0x48, 0x89, 0xC7);                                    /* mov rdi, rax */
        load(n->ty);
        E(0x48, 0x89, 0xC2);                                    /* mov rdx, rax */
        E(0x48, 0x05); e4((unsigned)n->val);                    /* add rax, delta */
        store_rdi(n->ty);
        E(0x48, 0x89, 0xD0);                                    /* mov rax, rdx */
        return;
    case ND_CAST: gen_expr(n->lhs); cast_to(n->ty); return;
    case ND_COMMA: gen_expr(n->lhs); gen_expr(n->rhs); return;
    }

    gen_expr(n->rhs);
    push_rax();
    gen_expr(n->lhs);
    pop_rdi();
    switch (n->kind) {
    case ND_ADD: E(0x48, 0x01, 0xF8); break;
    case ND_SUB: E(0x48, 0x29, 0xF8); break;
    case ND_MUL: E(0x48, 0x0F, 0xAF, 0xC7); break;
    case ND_DIV: case ND_MOD:
        if (n->ty->is_unsigned) E(0x31, 0xD2, 0x48, 0xF7, 0xF7);  /* xor edx, edx; div rdi */
        else E(0x48, 0x99, 0x48, 0xF7, 0xFF);                     /* cqo; idiv rdi */
        if (n->kind == ND_MOD) E(0x48, 0x89, 0xD0);
        break;
    case ND_BITAND: E(0x48, 0x21, 0xF8); break;
    case ND_BITOR: E(0x48, 0x09, 0xF8); break;
    case ND_BITXOR: E(0x48, 0x31, 0xF8); break;
    case ND_SHL: E(0x48, 0x89, 0xF9, 0x48, 0xD3, 0xE0); break;
    case ND_SHR:
        E(0x48, 0x89, 0xF9);
        if (n->lhs->ty->is_unsigned) E(0x48, 0xD3, 0xE8); else E(0x48, 0xD3, 0xF8);
        break;
    case ND_EQ: gen_cmp(n, 0x94, 0x94); return;
    case ND_NE: gen_cmp(n, 0x95, 0x95); return;
    case ND_LT: gen_cmp(n, 0x9C, 0x92); return;
    case ND_LE: gen_cmp(n, 0x9E, 0x96); return;
    default: error_tok(n->tok, "expresion que no se generar");
    }
    if (n->ty->size < 8) cast_to(n->ty);
}

static void gen_stmt(Node *n)
{
    Node *x;
    switch (n->kind) {
    case ND_IF: {
        int le = new_label_g(), lend = new_label_g();
        gen_expr(n->cond);
        E(0x48, 0x85, 0xC0);
        jmp_label(0x84, le);
        gen_stmt(n->then);
        jmp_label(0, lend);
        place_label(le);
        if (n->els) gen_stmt(n->els);
        place_label(lend);
        return;
    }
    case ND_FOR: {
        int lbeg = new_label_g();
        if (n->init) gen_stmt(n->init);
        place_label(lbeg);
        if (n->cond) {
            gen_expr(n->cond);
            E(0x48, 0x85, 0xC0);
            jmp_label(0x84, n->brk_label);
        }
        gen_stmt(n->then);
        place_label(n->cont_label);
        if (n->inc) gen_expr(n->inc);
        jmp_label(0, lbeg);
        place_label(n->brk_label);
        return;
    }
    case ND_DO: {
        int lbeg = new_label_g();
        place_label(lbeg);
        gen_stmt(n->then);
        place_label(n->cont_label);
        gen_expr(n->cond);
        E(0x48, 0x85, 0xC0);
        jmp_label(0x85, lbeg);
        place_label(n->brk_label);
        return;
    }
    case ND_SWITCH:
        gen_expr(n->cond);
        for (x = n->case_next; x; x = x->case_next) {
            E(0x48, 0xBF); e8((u64)x->case_val);                /* mov rdi, valor */
            E(0x48, 0x39, 0xF8);                                /* cmp rax, rdi */
            jmp_label(0x84, x->label_id);
        }
        jmp_label(0, n->default_case ? n->default_case->label_id : n->brk_label);
        gen_stmt(n->then);
        place_label(n->brk_label);
        return;
    case ND_CASE: place_label(n->label_id); gen_stmt(n->lhs); return;
    case ND_BLOCK: for (x = n->body; x; x = x->next) gen_stmt(x); return;
    case ND_GOTO: case ND_BREAK: jmp_label(0, n->label_id); return;
    case ND_LABEL: place_label(n->label_id); gen_stmt(n->lhs); return;
    case ND_RETURN:
        if (n->lhs) gen_expr(n->lhs);
        jmp_label(0, ret_label);
        return;
    case ND_EXPR_STMT: gen_expr(n->lhs); return;
    }
    error_tok(n->tok, "sentencia que no se generar");
}

static void assign_offsets(Obj *fn)
{
    Obj *v;
    int off = 0, i = 0;
    for (v = fn->params; v; v = v->next) v->offset = 16 + 8 * i++;
    for (v = fn->locals; v; v = v->next) {
        int al = v->ty->align < 1 ? 1 : v->ty->align, sz = v->ty->size < 1 ? 1 : v->ty->size;
        if (v->is_param) continue;
        if (al < 8 && sz >= 8) al = 8;
        off += sz;
        off = (off + al - 1) / al * al;
        v->offset = -off;
    }
    fn->stack_size = (off + 15) / 16 * 16;
}

static void gen_function(Obj *fn)
{
    gen_fn = fn;
    assign_offsets(fn);
    if (fn->callback) {
        /* trampolin: argumentos SysV (rdi, rsi, rdx, rcx, r8, r9) -> pila */
        fn->thunk_pos = clen;
        E(0x41, 0x51, 0x41, 0x50, 0x51, 0x52, 0x56, 0x57);      /* push r9 r8 rcx rdx rsi rdi */
        E(0xE8); e4(5);                                         /* call +5 (la funcion va justo despues del add y ret) */
        E(0x48, 0x83, 0xC4, 0x30, 0xC3);                        /* add rsp, 48; ret */
    }
    fn->code_pos = clen;
    ret_label = new_label_g();
    E(0x55, 0x48, 0x89, 0xE5);                                  /* push rbp; mov rbp, rsp */
    if (fn->stack_size) { E(0x48, 0x81, 0xEC); e4((unsigned)fn->stack_size); }
    gen_stmt(fn->body);
    E(0x31, 0xC0);                                              /* sin return: 0 */
    place_label(ret_label);
    E(0x48, 0x89, 0xEC, 0x5D, 0xC3);                            /* mov rsp, rbp; pop rbp; ret */
}

/* ---- alcance: solo se generan las funciones y variables que se usan desde main ---- */
static void reach_obj(Obj *o);
static void reach_node(Node *n)
{
    Node *x;
    for (; n; n = 0) {
        if (n->kind == ND_VAR && !n->var->is_local) reach_obj(n->var);
        if (n->lhs) reach_node(n->lhs);
        if (n->rhs) reach_node(n->rhs);
        if (n->cond) reach_node(n->cond);
        if (n->then) reach_node(n->then);
        if (n->els) reach_node(n->els);
        if (n->init) reach_node(n->init);
        if (n->inc) reach_node(n->inc);
        for (x = n->body; x; x = x->next) reach_node(x);
        for (x = n->args; x; x = x->next) reach_node(x);
    }
}
static Obj *reached_fns[8192];
static int nreached;
static void reach_obj(Obj *o)
{
    Reloc *r;
    if (o->used == 2) return;
    o->used = 2;
    if (o->is_function) {
        if (nreached >= 8192) fatal("demasiadas funciones");
        reached_fns[nreached++] = o;
        return;                                     /* el cuerpo se recorre despues (sin recursion profunda) */
    }
    for (r = o->rel; r; r = r->next) reach_obj(r->to);
}

/* ---- salida ---- */
static u8 *outbuf;
static int outlen, outmax;

static u64 align_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

static void link_program(void)
{
    Obj *v, *mainf = find_func("main"), *kvar = find_func("__K");
    Fix *f;
    int i;
    u64 code_base = LOAD_BASE + 16, data_base, bss_base, dsz = 0, bsz = 0, end, saved_rsp, stack_top;
    if (!mainf || !mainf->is_definition) fatal("el programa no tiene main()");
    if (!kvar) fatal("falta __K en la biblioteca");
    nlpos = nlabels + 200000;
    lpos = zalloc(sizeof(int) * nlpos);
    /* que se usa */
    nreached = 0;
    reach_obj(mainf);
    reach_obj(kvar);
    for (v = globals; v; v = v->next) if (v->callback && v->used) reach_obj(v);
    for (i = 0; i < nreached; i++) {
        Obj *fn = reached_fns[i];
        if (!fn->is_definition) {
            char b[120], *o = scat(b, "falta la funcion ");
            o = scat(o, fn->name);
            scat(o, "() (se usa pero no esta escrita)");
            if (fn->tok) error_tok(fn->tok, b);
            fatal(b);
        }
        reach_node(fn->body);
    }
    /* codigo: primero el arranque */
    clen = 0;
    E(0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57);   /* push rbx rbp r12 r13 r14 r15 */
    E(0x48, 0xB8); add_fix(FX_ABS, clen, kvar, 0, 0); e8(0);
    E(0x48, 0x89, 0x38);                                         /* mov [rax], rdi  (__K = api) */
    E(0x48, 0xB8); add_fix(FX_SAVED, clen, 0, 0, 0); e8(0);
    E(0x48, 0x89, 0x20);                                         /* mov [rax], rsp */
    E(0x48, 0xB8); add_fix(FX_STACKTOP, clen, 0, 0, 0); e8(0);
    E(0x48, 0x89, 0xC4);                                         /* mov rsp, rax */
    E(0x31, 0xC0, 0x50, 0x50);                                   /* argv = argc = 0 */
    E(0xE8); add_fix(FX_CALL, clen, mainf, 0, 0); e4(0);
    exit_pos = clen;
    E(0x48, 0xB8); add_fix(FX_SAVED, clen, 0, 0, 0); e8(0);
    E(0x48, 0x8B, 0x20);                                         /* mov rsp, [rax] */
    E(0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B);
    E(0x31, 0xC0, 0xC3);
    for (i = 0; i < nreached; i++)
        gen_function(reached_fns[i]);
    while (clen % 16) e1(0xCC);
    /* datos y bss */
    data_base = code_base + (u64)clen;
    for (v = globals; v; v = v->next) {
        if (v->is_function || v->used != 2 || !v->init_data) continue;
        dsz = align_up(dsz, (u64)(v->ty->align < 1 ? 1 : v->ty->align));
        v->addr = data_base + dsz;
        dsz += (u64)(v->ty->size > 0 ? v->ty->size : 0);
    }
    dsz = align_up(dsz, 16);
    bss_base = data_base + dsz;
    for (v = globals; v; v = v->next) {
        if (v->is_function || v->used != 2 || v->init_data) continue;
        if (seq(v->name, "__bcc_end")) continue;
        if (v->ty->size < 0) {
            char b[120], *o = scat(b, "la variable ");
            o = scat(o, v->name);
            scat(o, " no tiene tamano (extern sin definir?)");
            fatal(b);
        }
        bsz = align_up(bsz, (u64)(v->ty->align < 1 ? 1 : v->ty->align));
        v->addr = bss_base + bsz;
        bsz += (u64)v->ty->size;
    }
    bsz = align_up(bsz, 16);
    saved_rsp = bss_base + bsz;
    bsz += 16;
    bsz += STACK_SIZE;
    stack_top = bss_base + bsz;
    end = stack_top + 16;
    if (end > LOAD_BASE + 0x2000000) fatal("el programa no cabe en 32 MB");
    for (v = globals; v; v = v->next)
        if (v->is_function && v->used == 2) v->addr = code_base + (u64)v->code_pos;
    {   Obj *e = find_func("__bcc_end");
        if (e) e->addr = align_up(end, 4096); }
    /* parches */
    for (f = fixes; f; f = f->next) {
        u64 val = 0;
        switch (f->kind) {
        case FX_LABEL: {
            int t = lpos[f->label];
            unsigned rel = (unsigned)(t - (f->pos + 4));
            code[f->pos] = (u8)rel; code[f->pos + 1] = (u8)(rel >> 8); code[f->pos + 2] = (u8)(rel >> 16); code[f->pos + 3] = (u8)(rel >> 24);
            continue;
        }
        case FX_CALL: {
            unsigned rel = (unsigned)(f->obj->code_pos - (f->pos + 4));
            if (f->obj->used != 2) fatal("llamada a una funcion que no se genero (error interno)");
            code[f->pos] = (u8)rel; code[f->pos + 1] = (u8)(rel >> 8); code[f->pos + 2] = (u8)(rel >> 16); code[f->pos + 3] = (u8)(rel >> 24);
            continue;
        }
        case FX_ABS: val = f->obj->addr + (u64)f->addend; if (f->obj->used != 2 && !seq(f->obj->name, "__bcc_end")) { char b[120]; scat(scat(b, "direccion de algo que no se genero (error interno): "), f->obj->name); fatal(b); } break;
        case FX_THUNK: val = code_base + (u64)f->obj->thunk_pos; break;
        case FX_STACKTOP: val = stack_top; break;
        case FX_SAVED: val = saved_rsp; break;
        case FX_EXIT: val = code_base + (u64)exit_pos; break;
        }
        for (i = 0; i < 8; i++) code[f->pos + i] = (u8)(val >> (8 * i));
    }
    /* el archivo: cabecera + codigo + datos */
    outlen = 16 + clen + (int)dsz;
    if (outlen > outmax) fatal("el programa es demasiado grande");
    for (i = 0; i < outlen; i++) outbuf[i] = 0;
    {
        unsigned hdr[4];
        hdr[0] = 0x4D424350u;                       /* PCBM */
        hdr[1] = (unsigned)end;
        hdr[2] = (unsigned)code_base;
        hdr[3] = (unsigned)LOAD_BASE;
        mcopy(outbuf, hdr, 16);
    }
    mcopy(outbuf + 16, code, clen);
    for (v = globals; v; v = v->next) {
        Reloc *r;
        u8 *d;
        if (v->is_function || v->used != 2 || !v->init_data) continue;
        d = outbuf + (v->addr - LOAD_BASE);
        mcopy(d, v->init_data, v->ty->size);
        for (r = v->rel; r; r = r->next) {
            u64 a = (r->to->is_function && r->to->callback) ? code_base + (u64)r->to->thunk_pos : r->to->addr;
            a += (u64)r->addend;
            for (i = 0; i < 8; i++) d[r->offset + i] = (u8)(a >> (8 * i));
        }
    }
}

static int new_label_g(void)
{
    if (nlabels + 1 >= nlpos) fatal("demasiadas etiquetas");
    return ++nlabels;
}

/* ======================= entrada ======================= */
static void define_kapi_offsets(void)
{
    static char buf[4096];
    char *o = buf;
#define KO(f) o = scat(o, "#define __KO_" #f " "); o = sfmt_num(o, (i64)__builtin_offsetof(struct kapi, f)); o = scat(o, "\n");
    KO(scr_w) KO(scr_h) KO(poll) KO(mouse) KO(ms) KO(sleep) KO(sound) KO(drive_line) KO(dir) KO(read) KO(esc) KO(seed)
    KO(print) KO(basic_do) KO(basic_eval) KO(col) KO(args) KO(saved) KO(layer) KO(dirty) KO(flush) KO(cursor) KO(pcm_play)
    KO(pcm_stop) KO(pcm_ms) KO(blk) KO(wm_publish)
#undef KO
    define_text(buf);
}

/* compila path; devuelve 0 y deja el .BIN en out (olen bytes), o -1 con el mensaje en errbuf */
static int bcc_compile(const char *path, u8 *out, int omax, int *olen, char *mem, long memsize)
{
    static Macro m_line, m_file;
    char *text;
    int len;
    Token *tok;
    File *f;
    arena = mem; arena_end = mem + memsize;
    errbuf[0] = 0;
    if (__builtin_setjmp(jbuf))
        return -1;
    { int i; for (i = 0; i < MHASH; i++) mtab[i] = 0; }
    nincl_once = 0; incl_depth = 0; conds = 0;
    locals = globals = cur_fn = 0; scope = 0; gotos = labels = 0;
    brk_label = cont_label = nlabels = 0; cur_switch = 0; str_count = static_count = 0;
    fixes = 0; nreached = 0;
    init_types();
    enter_scope();
    (void)m_line; (void)m_file;
    add_macro("__LINE__", 8)->special = 1;
    add_macro("__FILE__", 8)->special = 2;
    define_text("#define __BCC__ 1\n#define __BASICOS__ 1\n#define __x86_64__ 1\n#define __STDC__ 1\n"
                "#define __STDC_VERSION__ 199901L\n#define __LP64__ 1\n#define __CHAR_BIT__ 8\n");
    define_kapi_offsets();
    /* la biblioteca primero */
    parsing_lib = 1;
    f = new_file("<bcclib>", (char *)bcc_lib_src, slen(bcc_lib_src));
    tok = preprocess(tokenize(f));
    parse(tok);
    parsing_lib = 0;
    text = plat_read(path, &len);
    if (!text) {
        char *o = scat(errbuf, "bcc: no puedo leer ");
        scat(o, path);
        return -1;
    }
    f = new_file((char *)path, text, len);
    tok = preprocess(tokenize(f));
    parse(tok);
    code = zalloc(cmax = 4 << 20);
    outbuf = out; outmax = omax;
    link_program();
    *olen = outlen;
    return 0;
}
