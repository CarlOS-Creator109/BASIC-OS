/* bcc_parse.c - tipos y analizador de C de BCC (parte de bcc.c). Esta hecho al estilo de chibicc. */

enum { TY_VOID, TY_BOOL, TY_CHAR, TY_SHORT, TY_INT, TY_LONG, TY_ENUM, TY_PTR, TY_FUNC, TY_ARRAY, TY_STRUCT, TY_UNION };

typedef struct Member Member;
struct Type {
    int kind, size, align, is_unsigned;
    Type *origin, *base;
    Token *name, *name_pos;
    int array_len;
    Member *members;
    int is_flexible;
    Type *return_ty, *params;
    int is_variadic, callback, has_proto;
    Type *next;
};
struct Member { Member *next; Type *ty; Token *name, *tok; int offset, idx; };

typedef struct Reloc { struct Reloc *next; int offset; struct Obj *to; i64 addend; } Reloc;

typedef struct Node Node;
typedef struct Obj {
    struct Obj *next;
    char *name;
    Type *ty;
    Token *tok;
    int is_local, offset, is_param;
    int is_function, is_definition, is_static, from_lib, used;
    char *init_data; Reloc *rel; int is_str;
    u64 addr;                                /* direccion final (globales) */
    struct Obj *params, *locals;
    Node *body;
    int stack_size, code_pos, thunk_pos, callback, special;
} Obj;

enum {
    ND_NULL, ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE, ND_ASSIGN, ND_COND, ND_COMMA, ND_MEMBER, ND_ADDR, ND_DEREF, ND_NOT, ND_BITNOT,
    ND_LOGAND, ND_LOGOR, ND_RETURN, ND_IF, ND_FOR, ND_DO, ND_SWITCH, ND_CASE, ND_BLOCK, ND_GOTO, ND_LABEL,
    ND_FUNCALL, ND_EXPR_STMT, ND_VAR, ND_NUM, ND_CAST, ND_MEMZERO, ND_NEG, ND_EXIT, ND_KCALL, ND_POSTINC,
    ND_BREAK
};

struct Node {
    int kind;
    Node *next;
    Type *ty;
    Token *tok;
    Node *lhs, *rhs, *cond, *then, *els, *init, *inc, *body;
    Member *member;
    Obj *var;
    i64 val;
    Node *args;
    Type *func_ty;
    int brk_label, cont_label, label_id;
    char *label;
    Node *case_next, *default_case;
    i64 case_val;
};

static Type *new_type(int kind, int size, int align)
{
    Type *t = zalloc(sizeof *t);
    t->kind = kind; t->size = size; t->align = align;
    return t;
}
static Type *copy_type(Type *t)
{
    Type *c = zalloc(sizeof *c);
    *c = *t;
    c->origin = t;
    return c;
}
static Type *pointer_to(Type *base)
{
    Type *t = new_type(TY_PTR, 8, 8);
    t->base = base;
    t->is_unsigned = 1;
    return t;
}
static Type *array_of(Type *base, int len)
{
    Type *t = new_type(TY_ARRAY, base->size * len, base->align);
    t->base = base;
    t->array_len = len;
    return t;
}
static Type *func_type(Type *ret)
{
    Type *t = new_type(TY_FUNC, 1, 1);
    t->return_ty = ret;
    return t;
}
static Type *struct_type(void) { return new_type(TY_STRUCT, 0, 1); }

static void init_types(void)
{
    ty_void = new_type(TY_VOID, 1, 1);
    ty_bool = new_type(TY_BOOL, 1, 1); ty_bool->is_unsigned = 1;
    ty_char = new_type(TY_CHAR, 1, 1);
    ty_uchar = new_type(TY_CHAR, 1, 1); ty_uchar->is_unsigned = 1;
    ty_short = new_type(TY_SHORT, 2, 2);
    ty_ushort = new_type(TY_SHORT, 2, 2); ty_ushort->is_unsigned = 1;
    ty_int = new_type(TY_INT, 4, 4);
    ty_uint = new_type(TY_INT, 4, 4); ty_uint->is_unsigned = 1;
    ty_long = new_type(TY_LONG, 8, 8);
    ty_ulong = new_type(TY_LONG, 8, 8); ty_ulong->is_unsigned = 1;
}

static int is_integer(Type *t)
{
    int k = t->kind;
    return k == TY_BOOL || k == TY_CHAR || k == TY_SHORT || k == TY_INT || k == TY_LONG || k == TY_ENUM;
}
static int is_scalar(Type *t) { return is_integer(t) || t->kind == TY_PTR; }

static Type *get_common_type(Type *a, Type *b)
{
    if (a->base)
        return pointer_to(a->base);
    if (a->kind == TY_FUNC) return pointer_to(a);
    if (b->kind == TY_FUNC) return pointer_to(b);
    if (a->size < 4) a = ty_int;
    if (b->size < 4) b = ty_int;
    if (a->size != b->size)
        return a->size < b->size ? b : a;
    if (b->is_unsigned)
        return b;
    return a;
}

static Node *new_cast(Node *expr, Type *ty);
static void add_type(Node *n);

static void usual_arith_conv(Node **lhs, Node **rhs)
{
    Type *t = get_common_type((*lhs)->ty, (*rhs)->ty);
    *lhs = new_cast(*lhs, t);
    *rhs = new_cast(*rhs, t);
}

static void add_type(Node *n)
{
    Node *x;
    if (!n || n->ty)
        return;
    add_type(n->lhs); add_type(n->rhs); add_type(n->cond); add_type(n->then); add_type(n->els);
    add_type(n->init); add_type(n->inc);
    for (x = n->body; x; x = x->next) add_type(x);
    for (x = n->args; x; x = x->next) add_type(x);
    switch (n->kind) {
    case ND_NUM: n->ty = ty_int; return;
    case ND_ADD: case ND_SUB: case ND_MUL: case ND_DIV: case ND_MOD: case ND_BITAND: case ND_BITOR: case ND_BITXOR:
        usual_arith_conv(&n->lhs, &n->rhs);
        n->ty = n->lhs->ty;
        return;
    case ND_NEG: {
        Type *t = get_common_type(ty_int, n->lhs->ty);
        n->lhs = new_cast(n->lhs, t);
        n->ty = t;
        return;
    }
    case ND_ASSIGN:
        if (n->lhs->ty->kind == TY_ARRAY) error_tok(n->tok, "no se puede asignar a un arreglo");
        if (n->lhs->ty->kind != TY_STRUCT && n->lhs->ty->kind != TY_UNION)
            n->rhs = new_cast(n->rhs, n->lhs->ty);
        n->ty = n->lhs->ty;
        return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE:
        usual_arith_conv(&n->lhs, &n->rhs);
        n->ty = ty_int;
        return;
    case ND_FUNCALL: case ND_KCALL:
        if (!n->ty) n->ty = ty_long;
        return;
    case ND_NOT: case ND_LOGOR: case ND_LOGAND: n->ty = ty_int; return;
    case ND_BITNOT: case ND_SHL: case ND_SHR: {
        Type *t = n->lhs->ty->size < 4 ? ty_int : n->lhs->ty;
        n->lhs = new_cast(n->lhs, t);
        n->ty = t;
        return;
    }
    case ND_VAR: n->ty = n->var->ty; return;
    case ND_COND:
        if (n->then->ty->kind == TY_VOID || n->els->ty->kind == TY_VOID) n->ty = ty_void;
        else if (is_integer(n->then->ty) && is_integer(n->els->ty)) { usual_arith_conv(&n->then, &n->els); n->ty = n->then->ty; }
        else if (n->then->ty->base) n->ty = pointer_to(n->then->ty->base);
        else n->ty = n->then->ty;
        return;
    case ND_COMMA: n->ty = n->rhs->ty; return;
    case ND_MEMBER: n->ty = n->member->ty; return;
    case ND_ADDR: {
        Type *t = n->lhs->ty;
        n->ty = t->kind == TY_ARRAY ? pointer_to(t->base) : pointer_to(t);
        return;
    }
    case ND_DEREF:
        if (!n->lhs->ty->base) {
            if (n->lhs->ty->kind == TY_FUNC) { n->ty = n->lhs->ty; return; }
            error_tok(n->tok, "eso no es un puntero");
        }
        if (n->lhs->ty->base->kind == TY_VOID) error_tok(n->tok, "no se puede usar *void");
        n->ty = n->lhs->ty->base;
        return;
    case ND_POSTINC: n->ty = n->lhs->ty; return;
    case ND_EXIT: n->ty = ty_void; return;
    }
}

/* ======================= estado del analizador ======================= */
typedef struct VarScope { struct VarScope *next; char *name; Obj *var; Type *type_def, *enum_ty; int enum_val; } VarScope;
typedef struct TagScope { struct TagScope *next; char *name; Type *ty; } TagScope;
typedef struct Scope { struct Scope *next; VarScope *vars; TagScope *tags; } Scope;

static Obj *locals, *globals, *cur_fn;
static Scope *scope;
static Node *gotos, *labels;
static int brk_label, cont_label, nlabels, parsing_lib;
static Node *cur_switch;
static int str_count, static_count;

static int new_label(void) { return ++nlabels; }

static void enter_scope(void) { Scope *s = zalloc(sizeof *s); s->next = scope; scope = s; }
static void leave_scope(void) { scope = scope->next; }

static int tok_eq_name(Token *t, const char *name) { return t->kind == TK_IDENT && sneq(t->loc, t->len, name); }

static VarScope *find_var(Token *tok)
{
    Scope *sc;
    VarScope *v;
    for (sc = scope; sc; sc = sc->next)
        for (v = sc->vars; v; v = v->next)
            if (sneq(tok->loc, tok->len, v->name)) return v;
    return 0;
}
static Type *find_tag(Token *tok)
{
    Scope *sc;
    TagScope *t;
    for (sc = scope; sc; sc = sc->next)
        for (t = sc->tags; t; t = t->next)
            if (sneq(tok->loc, tok->len, t->name)) return t->ty;
    return 0;
}
static VarScope *push_scope(char *name)
{
    VarScope *v = zalloc(sizeof *v);
    v->name = name;
    v->next = scope->vars;
    scope->vars = v;
    return v;
}
static void push_tag(Token *tok, Type *ty)
{
    TagScope *t = zalloc(sizeof *t);
    t->name = sdup(tok->loc, tok->len);
    t->ty = ty;
    t->next = scope->tags;
    scope->tags = t;
}

static char *tok_name(Token *t) { return sdup(t->loc, t->len); }

static Obj *new_var(char *name, Type *ty)
{
    Obj *v = zalloc(sizeof *v);
    v->name = name; v->ty = ty;
    push_scope(name)->var = v;
    return v;
}
static Obj *new_lvar(char *name, Type *ty)
{
    Obj *v = new_var(name, ty);
    v->is_local = 1;
    v->next = locals;
    locals = v;
    return v;
}
static Obj *new_gvar(char *name, Type *ty)
{
    Obj *v = new_var(name, ty);
    v->next = globals;
    v->from_lib = parsing_lib;
    globals = v;
    return v;
}
static char *unique_name(const char *pfx_)
{
    char b[40], *o = scat(b, pfx_);
    sfmt_num(o, ++static_count);
    return sdup(b, slen(b));
}
static Obj *new_anon_gvar(Type *ty)
{
    Obj *v = zalloc(sizeof *v);
    v->name = unique_name(".L.");
    v->ty = ty;
    v->next = globals;
    globals = v;
    return v;
}
static Obj *new_string_literal(char *p, int n)
{
    Type *ty = array_of(ty_char, n);
    Obj *v = new_anon_gvar(ty);
    v->init_data = p;
    v->is_str = 1;
    return v;
}

static Node *new_node(int kind, Token *tok) { Node *n = zalloc(sizeof *n); n->kind = kind; n->tok = tok; return n; }
static Node *new_binary(int kind, Node *l, Node *r, Token *tok) { Node *n = new_node(kind, tok); n->lhs = l; n->rhs = r; return n; }
static Node *new_unary(int kind, Node *e, Token *tok) { Node *n = new_node(kind, tok); n->lhs = e; return n; }
static Node *new_num(i64 v, Token *tok) { Node *n = new_node(ND_NUM, tok); n->val = v; return n; }
static Node *new_long(i64 v, Token *tok) { Node *n = new_num(v, tok); n->ty = ty_long; return n; }
static Node *new_var_node(Obj *v, Token *tok) { Node *n = new_node(ND_VAR, tok); n->var = v; v->used = 1; return n; }

static Node *new_cast(Node *expr, Type *ty)
{
    Node *n;
    add_type(expr);
    n = new_node(ND_CAST, expr->tok);
    n->lhs = expr;
    n->ty = copy_type(ty);
    return n;
}

static Token *skip(Token *tok, const char *s)
{
    if (!tok_is(tok, s)) {
        char b[60], *o = scat(b, s[0] == ';' ? "falta '" : "se esperaba '");
        o = scat(o, s);
        scat(o, "'");
        error_tok(tok, b);
    }
    return tok->next;
}
static int consume(Token **rest, Token *tok, const char *s)
{
    if (tok_is(tok, s)) { *rest = tok->next; return 1; }
    *rest = tok;
    return 0;
}

static i64 const_expr(Token **rest, Token *tok);
static i64 eval(Node *n);
static i64 eval2(Node *n, Obj **label);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);
static Node *conditional(Token **rest, Token *tok);
static Node *cast(Token **rest, Token *tok);
static Node *unary(Token **rest, Token *tok);
static Node *postfix(Token **rest, Token *tok);
static Node *compound_stmt(Token **rest, Token *tok);
static Node *stmt(Token **rest, Token *tok);
static Type *typename_(Token **rest, Token *tok);
static Type *declarator(Token **rest, Token *tok, Type *ty);
static Type *abstract_declarator(Token **rest, Token *tok, Type *ty);
static Node *declaration(Token **rest, Token *tok, Type *basety, int is_static);
static int is_typename(Token *tok);
static Type *struct_union_decl(Token **rest, Token *tok, int is_union);
static Type *enum_specifier(Token **rest, Token *tok);
static Node *new_add(Node *l, Node *r, Token *tok);
static Node *new_sub(Node *l, Node *r, Token *tok);

typedef struct { int is_typedef, is_static, is_extern, is_inline, callback; } VarAttr;

static Type *find_typedef(Token *tok)
{
    VarScope *v;
    if (tok->kind != TK_IDENT) return 0;
    v = find_var(tok);
    return v ? v->type_def : 0;
}

static void skip_attribute(Token **rest, Token *tok)
{
    /* __attribute__((...)) y __declspec(...): se saltan */
    int depth = 0;
    tok = tok->next;
    tok = skip(tok, "(");
    depth = 1;
    while (depth && tok->kind != TK_EOF) {
        if (tok_is(tok, "(")) depth++;
        else if (tok_is(tok, ")")) depth--;
        tok = tok->next;
    }
    *rest = tok;
}

static const char *kw_ignore[] = { "const", "volatile", "auto", "register", "restrict", "__restrict", "__restrict__",
                                   "_Noreturn", "__inline", "__inline__", "__const", "__volatile__", "__extension__", 0 };
static int is_ignored_kw(Token *t)
{
    int i;
    for (i = 0; kw_ignore[i]; i++) if (tok_eq_name(t, kw_ignore[i])) return 1;
    return 0;
}

/* declspec = combinaciones de void/char/short/int/long/signed/unsigned, struct, union, enum, typedef... */
static Type *declspec(Token **rest, Token *tok, VarAttr *attr)
{
    enum { VOID = 1 << 0, BOOL = 1 << 2, CHAR = 1 << 4, SHORT = 1 << 6, INT = 1 << 8, LONG = 1 << 10, OTHER = 1 << 12,
           SIGNED = 1 << 13, UNSIGNED = 1 << 14 };
    Type *ty = ty_int;
    int counter = 0;
    while (is_typename(tok)) {
        if (tok_eq_name(tok, "typedef") || tok_eq_name(tok, "static") || tok_eq_name(tok, "extern") ||
            tok_eq_name(tok, "inline") || tok_eq_name(tok, "__callback")) {
            if (!attr) error_tok(tok, "aqui no va eso");
            if (tok_eq_name(tok, "typedef")) attr->is_typedef = 1;
            else if (tok_eq_name(tok, "static")) attr->is_static = 1;
            else if (tok_eq_name(tok, "extern")) attr->is_extern = 1;
            else if (tok_eq_name(tok, "inline")) attr->is_inline = 1;
            else attr->callback = 1;
            tok = tok->next;
            continue;
        }
        if (is_ignored_kw(tok)) { tok = tok->next; continue; }
        if (tok_eq_name(tok, "__attribute__") || tok_eq_name(tok, "__declspec")) { skip_attribute(&tok, tok); continue; }
        if (tok_eq_name(tok, "_Alignas")) {
            tok = skip(tok->next, "(");
            if (is_typename(tok)) typename_(&tok, tok); else const_expr(&tok, tok);
            tok = skip(tok, ")");
            continue;
        }
        {
            Type *ty2 = find_typedef(tok);
            if (tok_eq_name(tok, "struct") || tok_eq_name(tok, "union") || tok_eq_name(tok, "enum") || ty2) {
                if (counter) break;
                if (tok_eq_name(tok, "struct")) ty = struct_union_decl(&tok, tok->next, 0);
                else if (tok_eq_name(tok, "union")) ty = struct_union_decl(&tok, tok->next, 1);
                else if (tok_eq_name(tok, "enum")) ty = enum_specifier(&tok, tok->next);
                else { ty = ty2; tok = tok->next; }
                counter += OTHER;
                continue;
            }
        }
        if (tok_eq_name(tok, "void")) counter += VOID;
        else if (tok_eq_name(tok, "_Bool") || tok_eq_name(tok, "bool")) counter += BOOL;
        else if (tok_eq_name(tok, "char")) counter += CHAR;
        else if (tok_eq_name(tok, "short")) counter += SHORT;
        else if (tok_eq_name(tok, "int")) counter += INT;
        else if (tok_eq_name(tok, "long")) counter += LONG;
        else if (tok_eq_name(tok, "signed") || tok_eq_name(tok, "__signed__")) counter |= SIGNED;
        else if (tok_eq_name(tok, "unsigned")) counter |= UNSIGNED;
        else if (tok_eq_name(tok, "float") || tok_eq_name(tok, "double"))
            error_tok(tok, "BCC todavia no tiene float/double (usa enteros o punto fijo)");
        else error_tok(tok, "tipo raro");
        switch (counter) {
        case VOID: ty = ty_void; break;
        case BOOL: ty = ty_bool; break;
        case CHAR: case SIGNED + CHAR: ty = ty_char; break;
        case UNSIGNED + CHAR: ty = ty_uchar; break;
        case SHORT: case SHORT + INT: case SIGNED + SHORT: case SIGNED + SHORT + INT: ty = ty_short; break;
        case UNSIGNED + SHORT: case UNSIGNED + SHORT + INT: ty = ty_ushort; break;
        case INT: case SIGNED: case SIGNED + INT: ty = ty_int; break;
        case UNSIGNED: case UNSIGNED + INT: ty = ty_uint; break;
        case LONG: case LONG + INT: case LONG + LONG: case LONG + LONG + INT: case SIGNED + LONG: case SIGNED + LONG + INT:
        case SIGNED + LONG + LONG: case SIGNED + LONG + LONG + INT: ty = ty_long; break;
        case UNSIGNED + LONG: case UNSIGNED + LONG + INT: case UNSIGNED + LONG + LONG: case UNSIGNED + LONG + LONG + INT:
            ty = ty_ulong; break;
        default: error_tok(tok, "combinacion de tipos que no entiendo");
        }
        tok = tok->next;
    }
    *rest = tok;
    return ty;
}

static Type *func_params(Token **rest, Token *tok, Type *ty)
{
    Type head = {0}, *cur = &head, *fn;
    int variadic = 0;
    if (tok_eq_name(tok, "void") && tok_is(tok->next, ")")) {
        fn = func_type(ty);
        fn->has_proto = 1;
        *rest = tok->next->next;
        return fn;
    }
    while (!tok_is(tok, ")")) {
        Type *t;
        Token *name;
        if (cur != &head) tok = skip(tok, ",");
        if (tok_is(tok, "...")) { variadic = 1; tok = tok->next; break; }
        if (tok->kind == TK_IDENT && !is_typename(tok)) {   /* f(a, b) estilo K&R: se toman como int */
            t = copy_type(ty_int); t->name = tok; tok = tok->next;
        } else {
            t = declspec(&tok, tok, 0);
            t = declarator(&tok, tok, t);
        }
        name = t->name;
        if (t->kind == TY_ARRAY) { t = pointer_to(t->base); t->name = name; }
        else if (t->kind == TY_FUNC) { t = pointer_to(t); t->name = name; }
        cur = cur->next = copy_type(t);
    }
    fn = func_type(ty);
    fn->params = head.next;
    fn->is_variadic = variadic;
    fn->has_proto = head.next || variadic;
    *rest = tok->next;
    return fn;
}

static Type *array_dims(Token **rest, Token *tok, Type *ty);
static Type *type_suffix(Token **rest, Token *tok, Type *ty)
{
    if (tok_is(tok, "(")) return func_params(rest, tok->next, ty);
    if (tok_is(tok, "[")) return array_dims(rest, tok->next, ty);
    *rest = tok;
    return ty;
}
static Type *array_dims(Token **rest, Token *tok, Type *ty)
{
    int sz;
    while (is_ignored_kw(tok) || tok_eq_name(tok, "static")) tok = tok->next;
    if (tok_is(tok, "]")) {
        ty = type_suffix(rest, tok->next, ty);
        ty = array_of(ty, -1);
        return ty;
    }
    sz = (int)const_expr(&tok, tok);
    tok = skip(tok, "]");
    ty = type_suffix(rest, tok, ty);
    return array_of(ty, sz);
}

static Type *pointers(Token **rest, Token *tok, Type *ty)
{
    while (consume(&tok, tok, "*")) {
        ty = pointer_to(ty);
        while (is_ignored_kw(tok)) tok = tok->next;
    }
    *rest = tok;
    return ty;
}

static Type *declarator(Token **rest, Token *tok, Type *ty)
{
    Token *name = 0, *pos = tok;
    ty = pointers(&tok, tok, ty);
    while (tok_eq_name(tok, "__attribute__")) skip_attribute(&tok, tok);
    if (tok_is(tok, "(")) {
        Token *start = tok;
        Type dummy = {0};
        declarator(&tok, start->next, &dummy);
        tok = skip(tok, ")");
        ty = type_suffix(rest, tok, ty);
        return declarator(&tok, start->next, ty);
    }
    if (tok->kind == TK_IDENT) { name = tok; tok = tok->next; }
    ty = type_suffix(&tok, tok, ty);
    while (tok_eq_name(tok, "__attribute__") || tok_eq_name(tok, "__asm__") || tok_eq_name(tok, "asm")) skip_attribute(&tok, tok);
    *rest = tok;
    ty->name = name;
    ty->name_pos = pos;
    return ty;
}

static Type *abstract_declarator(Token **rest, Token *tok, Type *ty)
{
    ty = pointers(&tok, tok, ty);
    if (tok_is(tok, "(") && !is_typename(tok->next) && !tok_is(tok->next, ")")) {
        Token *start = tok;
        Type dummy = {0};
        abstract_declarator(&tok, start->next, &dummy);
        tok = skip(tok, ")");
        ty = type_suffix(rest, tok, ty);
        return abstract_declarator(&tok, start->next, ty);
    }
    return type_suffix(rest, tok, ty);
}

static Type *typename_(Token **rest, Token *tok)
{
    Type *ty = declspec(&tok, tok, 0);
    return abstract_declarator(rest, tok, ty);
}

static int is_typename(Token *tok)
{
    static const char *kw[] = {
        "void", "_Bool", "bool", "char", "short", "int", "long", "struct", "union", "typedef", "enum", "static", "extern",
        "_Alignas", "signed", "unsigned", "const", "volatile", "auto", "register", "restrict", "__restrict", "__restrict__",
        "_Noreturn", "float", "double", "inline", "__inline", "__inline__", "__attribute__", "__callback", "__signed__",
        "__const", "__volatile__", "__extension__", "__declspec", 0 };
    int i;
    if (tok->kind != TK_IDENT) return 0;
    for (i = 0; kw[i]; i++) if (sneq(tok->loc, tok->len, kw[i])) return 1;
    return find_typedef(tok) != 0;
}

static Type *enum_specifier(Token **rest, Token *tok)
{
    Type *ty = new_type(TY_ENUM, 4, 4);
    Token *tag = 0;
    int val = 0, i = 0;
    if (tok->kind == TK_IDENT) { tag = tok; tok = tok->next; }
    if (tag && !tok_is(tok, "{")) {
        Type *t = find_tag(tag);
        *rest = tok;
        return t ? t : ty;
    }
    tok = skip(tok, "{");
    while (!consume(&tok, tok, "}")) {
        VarScope *vs;
        char *name;
        if (i++ > 0) { tok = skip(tok, ","); if (consume(&tok, tok, "}")) break; }
        if (tok->kind != TK_IDENT) error_tok(tok, "se esperaba un nombre en el enum");
        name = tok_name(tok);
        tok = tok->next;
        if (tok_is(tok, "=")) val = (int)const_expr(&tok, tok->next);
        vs = push_scope(name);
        vs->enum_ty = ty;
        vs->enum_val = val++;
    }
    *rest = tok;
    if (tag) push_tag(tag, ty);
    return ty;
}

static Type *struct_union_decl(Token **rest, Token *tok, int is_union)
{
    Token *tag = 0;
    Type *ty;
    Member head = {0}, *cur = &head, *m;
    int offset = 0, idx = 0;
    while (tok_eq_name(tok, "__attribute__")) skip_attribute(&tok, tok);
    if (tok->kind == TK_IDENT) { tag = tok; tok = tok->next; }
    if (tag && !tok_is(tok, "{")) {
        Type *t = find_tag(tag);
        *rest = tok;
        if (t) return t;
        t = struct_type();
        t->kind = is_union ? TY_UNION : TY_STRUCT;
        t->size = -1;
        push_tag(tag, t);
        return t;
    }
    tok = skip(tok, "{");
    ty = struct_type();
    ty->kind = is_union ? TY_UNION : TY_STRUCT;
    while (!consume(&tok, tok, "}")) {
        VarAttr attr = {0};
        Type *basety = declspec(&tok, tok, &attr);
        int first = 1;
        if (consume(&tok, tok, ";")) {           /* struct/union anonimo dentro */
            m = zalloc(sizeof *m);
            m->ty = basety; m->idx = idx++; m->tok = tok;
            cur = cur->next = m;
            continue;
        }
        while (!consume(&tok, tok, ";")) {
            if (!first) tok = skip(tok, ",");
            first = 0;
            m = zalloc(sizeof *m);
            m->ty = declarator(&tok, tok, basety);
            m->name = m->ty->name;
            m->tok = m->ty->name_pos;
            m->idx = idx++;
            if (tok_is(tok, ":")) error_tok(tok, "BCC todavia no tiene campos de bits (bitfields)");
            cur = cur->next = m;
        }
    }
    while (tok_eq_name(tok, "__attribute__")) skip_attribute(&tok, tok);
    *rest = tok;
    if (cur != &head && cur->ty->kind == TY_ARRAY && cur->ty->array_len < 0) {
        cur->ty = array_of(cur->ty->base, 0);
        ty->is_flexible = 1;
    }
    ty->members = head.next;
    for (m = ty->members; m; m = m->next) {
        if (m->ty->size < 0) error_tok(m->tok ? m->tok : tok, "miembro de tipo incompleto");
        if (is_union) {
            m->offset = 0;
            if (offset < m->ty->size) offset = m->ty->size;
        } else {
            offset = (offset + m->ty->align - 1) / m->ty->align * m->ty->align;
            m->offset = offset;
            offset += m->ty->size;
        }
        if (ty->align < m->ty->align) ty->align = m->ty->align;
    }
    ty->size = (offset + ty->align - 1) / ty->align * ty->align;
    if (tag) {
        Type *t = 0;
        TagScope *ts;
        for (ts = scope->tags; ts; ts = ts->next)
            if (sneq(tag->loc, tag->len, ts->name)) { t = ts->ty; break; }
        if (t) { *t = *ty; return t; }        /* completa la declaracion adelantada */
        push_tag(tag, ty);
    }
    return ty;
}

static Member *get_member(Type *ty, Token *tok)
{
    Member *m;
    for (m = ty->members; m; m = m->next) {
        if (!m->name && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION)) {
            if (get_member(m->ty, tok)) return m;
            continue;
        }
        if (m->name && m->name->len == tok->len && memeq(m->name->loc, tok->loc, tok->len))
            return m;
    }
    return 0;
}

static Node *struct_ref(Node *node, Token *tok)
{
    Type *ty;
    add_type(node);
    ty = node->ty;
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION) error_tok(node->tok, "eso no es un struct ni un union");
    for (;;) {
        Member *m = get_member(ty, tok);
        Node *n;
        if (!m) error_tok2(tok, "ese struct no tiene ese campo", tok_name(tok));
        n = new_unary(ND_MEMBER, node, tok);
        n->member = m;
        if (m->name) return n;
        node = n;
        ty = m->ty;
    }
}

/* ======================= expresiones ======================= */
static Node *to_assign(Node *binary)
{
    /* a op= b  ->  tmp = &a, *tmp = *tmp op b */
    Token *tok = binary->tok;
    Obj *var;
    Node *e1, *e2, *e3, *e4;
    add_type(binary->lhs);
    add_type(binary->rhs);
    if (binary->lhs->kind == ND_VAR) {             /* caso comun: sin variable temporal */
        Node *l = new_var_node(binary->lhs->var, tok);
        Node *op = new_binary(binary->kind, new_var_node(binary->lhs->var, tok), binary->rhs, tok);
        if (binary->kind == ND_ADD) op = new_add(op->lhs, op->rhs, tok);
        else if (binary->kind == ND_SUB) op = new_sub(op->lhs, op->rhs, tok);
        return new_binary(ND_ASSIGN, l, op, tok);
    }
    var = new_lvar("", pointer_to(binary->lhs->ty));
    e1 = new_binary(ND_ASSIGN, new_var_node(var, tok), new_unary(ND_ADDR, binary->lhs, tok), tok);
    e3 = new_unary(ND_DEREF, new_var_node(var, tok), tok);
    if (binary->kind == ND_ADD) e4 = new_add(new_unary(ND_DEREF, new_var_node(var, tok), tok), binary->rhs, tok);
    else if (binary->kind == ND_SUB) e4 = new_sub(new_unary(ND_DEREF, new_var_node(var, tok), tok), binary->rhs, tok);
    else e4 = new_binary(binary->kind, new_unary(ND_DEREF, new_var_node(var, tok), tok), binary->rhs, tok);
    e2 = new_binary(ND_ASSIGN, e3, e4, tok);
    return new_binary(ND_COMMA, e1, e2, tok);
}

static Node *new_add(Node *lhs, Node *rhs, Token *tok)
{
    add_type(lhs);
    add_type(rhs);
    if (is_integer(lhs->ty) && is_integer(rhs->ty))
        return new_binary(ND_ADD, lhs, rhs, tok);
    if (lhs->ty->kind == TY_FUNC) lhs = new_cast(lhs, pointer_to(lhs->ty));
    if (lhs->ty->base && rhs->ty->base) error_tok(tok, "no se pueden sumar dos punteros");
    if (!lhs->ty->base && rhs->ty->base) { Node *t = lhs; lhs = rhs; rhs = t; }
    if (!lhs->ty->base) error_tok(tok, "suma con tipos raros");
    rhs = new_binary(ND_MUL, new_cast(rhs, ty_long), new_long(lhs->ty->base->kind == TY_VOID ? 1 : lhs->ty->base->size, tok), tok);
    {
        Node *n = new_binary(ND_ADD, lhs, rhs, tok);
        n->ty = lhs->ty->kind == TY_ARRAY ? pointer_to(lhs->ty->base) : lhs->ty;
        add_type(n->rhs);
        n->lhs = new_cast(lhs, n->ty);
        return n;
    }
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok)
{
    add_type(lhs);
    add_type(rhs);
    if (is_integer(lhs->ty) && is_integer(rhs->ty))
        return new_binary(ND_SUB, lhs, rhs, tok);
    if (lhs->ty->base && is_integer(rhs->ty)) {
        Node *n;
        int sz = lhs->ty->base->kind == TY_VOID ? 1 : lhs->ty->base->size;
        rhs = new_binary(ND_MUL, new_cast(rhs, ty_long), new_long(sz, tok), tok);
        add_type(rhs);
        n = new_binary(ND_SUB, lhs, rhs, tok);
        n->ty = lhs->ty->kind == TY_ARRAY ? pointer_to(lhs->ty->base) : lhs->ty;
        n->lhs = new_cast(lhs, n->ty);
        return n;
    }
    if (lhs->ty->base && rhs->ty->base) {
        int sz = lhs->ty->base->kind == TY_VOID ? 1 : lhs->ty->base->size;
        Node *n = new_binary(ND_SUB, new_cast(lhs, ty_long), new_cast(rhs, ty_long), tok);
        n->ty = ty_long;
        return new_binary(ND_DIV, n, new_long(sz, tok), tok);
    }
    error_tok(tok, "resta con tipos raros");
    return 0;
}

static Node *funcall(Token **rest, Token *tok, Node *fn);

static Node *primary(Token **rest, Token *tok)
{
    Token *start = tok;
    if (tok_is(tok, "(") && is_typename(tok->next)) {
        error_tok(tok, "BCC todavia no tiene literales compuestos ((tipo){...})");
    }
    if (tok_is(tok, "(")) {
        Node *n = expr(&tok, tok->next);
        *rest = skip(tok, ")");
        return n;
    }
    if (tok_eq_name(tok, "sizeof")) {
        Node *n;
        if (tok_is(tok->next, "(") && is_typename(tok->next->next)) {
            Type *ty = typename_(&tok, tok->next->next);
            *rest = skip(tok, ")");
            if (ty->size < 0) error_tok(start, "sizeof de un tipo incompleto");
            return new_long(ty->size, start);
        }
        n = unary(rest, tok->next);
        add_type(n);
        return new_long(n->ty->size, tok);
    }
    if (tok_eq_name(tok, "_Alignof") || tok_eq_name(tok, "__alignof__")) {
        Type *ty;
        tok = skip(tok->next, "(");
        ty = typename_(&tok, tok);
        *rest = skip(tok, ")");
        return new_long(ty->align, tok);
    }
    if (tok_eq_name(tok, "__builtin_va_start")) {
        /* ap = (char *)&last + 8 */
        Node *ap, *last, *n;
        tok = skip(tok->next, "(");
        ap = assign(&tok, tok);
        tok = skip(tok, ",");
        last = assign(&tok, tok);
        *rest = skip(tok, ")");
        n = new_cast(new_unary(ND_ADDR, last, start), pointer_to(ty_char));
        n = new_add(n, new_num(8, start), start);
        return new_binary(ND_ASSIGN, ap, n, start);
    }
    if (tok_eq_name(tok, "__builtin_va_arg")) {
        /* *(T *)((ap += 8) - 8) */
        Node *ap, *n;
        Type *ty;
        tok = skip(tok->next, "(");
        ap = assign(&tok, tok);
        tok = skip(tok, ",");
        ty = typename_(&tok, tok);
        *rest = skip(tok, ")");
        n = to_assign(new_binary(ND_ADD, ap, new_num(8, start), start));
        n = new_sub(n, new_num(8, start), start);
        return new_unary(ND_DEREF, new_cast(n, pointer_to(ty)), start);
    }
    if (tok_eq_name(tok, "__builtin_exit")) {
        Node *n = new_node(ND_EXIT, tok);
        tok = skip(tok->next, "(");
        n->lhs = new_cast(assign(&tok, tok), ty_long);
        *rest = skip(tok, ")");
        return n;
    }
    if (tok_eq_name(tok, "__kcall")) {
        Node head = {0}, *cur = &head, *n = new_node(ND_KCALL, tok);
        int nargs = 0;
        tok = skip(tok->next, "(");
        n->val = const_expr(&tok, tok);
        while (consume(&tok, tok, ",")) {
            Node *a = assign(&tok, tok);
            add_type(a);
            if (a->ty->kind == TY_ARRAY || a->ty->kind == TY_FUNC) a = new_cast(a, pointer_to(a->ty->kind == TY_ARRAY ? a->ty->base : a->ty));
            else if (is_integer(a->ty) && a->ty->size < 8) a = new_cast(a, a->ty->is_unsigned ? ty_ulong : ty_long);
            cur = cur->next = a;
            if (++nargs > 6) error_tok(tok, "__kcall: maximo 6 argumentos");
        }
        *rest = skip(tok, ")");
        n->args = head.next;
        n->ty = ty_long;
        return n;
    }
    if (tok->kind == TK_IDENT) {
        VarScope *vs = find_var(tok);
        *rest = tok->next;
        if (vs && vs->var) return new_var_node(vs->var, tok);
        if (vs && vs->enum_ty) return new_num(vs->enum_val, tok);
        if (tok_is(tok->next, "(")) {          /* funcion sin declarar: se supone int f() */
            Obj *f;
            Type *ty = func_type(ty_int);
            Scope *sc;
            warn_tok(tok, "funcion sin declarar (se supone que devuelve int)");
            sc = scope; while (sc->next) sc = sc->next;
            {   Scope *save = scope; scope = sc; f = new_gvar(tok_name(tok), ty); scope = save; }
            f->is_function = 1;
            return new_var_node(f, tok);
        }
        error_tok2(tok, "variable sin declarar", tok_name(tok));
    }
    if (tok->kind == TK_STR) {
        Obj *v = new_string_literal(tok->str, tok->slen);
        *rest = tok->next;
        return new_var_node(v, tok);
    }
    if (tok->kind == TK_NUM) {
        Node *n = new_num(tok->val, tok);
        n->ty = tok->ty;
        *rest = tok->next;
        return n;
    }
    error_tok(tok, "se esperaba una expresion");
    return 0;
}

static Node *funcall(Token **rest, Token *tok, Node *fn)
{
    Type *ty, *param_ty;
    Node head = {0}, *cur = &head, *n;
    add_type(fn);
    if (fn->ty->kind != TY_FUNC && !(fn->ty->kind == TY_PTR && fn->ty->base->kind == TY_FUNC))
        error_tok(fn->tok, "eso no es una funcion");
    ty = fn->ty->kind == TY_FUNC ? fn->ty : fn->ty->base;
    param_ty = ty->params;
    while (!tok_is(tok, ")")) {
        Node *arg;
        if (cur != &head) tok = skip(tok, ",");
        arg = assign(&tok, tok);
        add_type(arg);
        if (param_ty) {
            if (param_ty->kind == TY_STRUCT || param_ty->kind == TY_UNION)
                error_tok(arg->tok, "BCC todavia no pasa structs por valor (usa un puntero)");
            arg = new_cast(arg, param_ty);
            param_ty = param_ty->next;
        } else {
            if (arg->ty->kind == TY_STRUCT || arg->ty->kind == TY_UNION)
                error_tok(arg->tok, "BCC todavia no pasa structs por valor (usa un puntero)");
            if (arg->ty->kind == TY_ARRAY) arg = new_cast(arg, pointer_to(arg->ty->base));
            else if (arg->ty->kind == TY_FUNC) arg = new_cast(arg, pointer_to(arg->ty));
            else if (is_integer(arg->ty) && arg->ty->size < 4) arg = new_cast(arg, ty_int);
        }
        cur = cur->next = arg;
    }
    if (param_ty && ty->has_proto) error_tok(tok, "faltan argumentos en la llamada");
    *rest = skip(tok, ")");
    n = new_unary(ND_FUNCALL, fn, tok);
    n->func_ty = ty;
    n->ty = ty->return_ty;
    n->args = head.next;
    if (n->ty->kind == TY_STRUCT || n->ty->kind == TY_UNION)
        error_tok(tok, "BCC todavia no devuelve structs por valor (usa un puntero)");
    return n;
}

static Node *postfix(Token **rest, Token *tok)
{
    Node *node = primary(&tok, tok);
    for (;;) {
        if (tok_is(tok, "(")) { node = funcall(&tok, tok->next, node); continue; }
        if (tok_is(tok, "[")) {
            Token *start = tok;
            Node *idx = expr(&tok, tok->next);
            tok = skip(tok, "]");
            node = new_unary(ND_DEREF, new_add(node, idx, start), start);
            continue;
        }
        if (tok_is(tok, ".")) { node = struct_ref(node, tok->next); tok = tok->next->next; continue; }
        if (tok_is(tok, "->")) {
            node = new_unary(ND_DEREF, node, tok);
            node = struct_ref(node, tok->next);
            tok = tok->next->next;
            continue;
        }
        if (tok_is(tok, "++") || tok_is(tok, "--")) {
            Node *n = new_unary(ND_POSTINC, node, tok);
            add_type(node);
            if (!is_scalar(node->ty)) error_tok(tok, "++/-- solo con numeros y punteros");
            n->val = tok_is(tok, "++") ? 1 : -1;
            if (node->ty->base) n->val *= node->ty->base->kind == TY_VOID ? 1 : node->ty->base->size;
            node = n;
            tok = tok->next;
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *unary(Token **rest, Token *tok)
{
    if (tok_is(tok, "+")) return cast(rest, tok->next);
    if (tok_is(tok, "-")) return new_unary(ND_NEG, cast(rest, tok->next), tok);
    if (tok_is(tok, "&")) {
        Node *l = cast(rest, tok->next);
        add_type(l);
        return new_unary(ND_ADDR, l, tok);
    }
    if (tok_is(tok, "*")) {
        Node *l = cast(rest, tok->next);
        add_type(l);
        if (l->ty->kind == TY_FUNC) return l;
        return new_unary(ND_DEREF, l, tok);
    }
    if (tok_is(tok, "!")) return new_unary(ND_NOT, cast(rest, tok->next), tok);
    if (tok_is(tok, "~")) return new_unary(ND_BITNOT, cast(rest, tok->next), tok);
    if (tok_is(tok, "++")) return to_assign(new_binary(ND_ADD, unary(rest, tok->next), new_num(1, tok), tok));
    if (tok_is(tok, "--")) return to_assign(new_binary(ND_SUB, unary(rest, tok->next), new_num(1, tok), tok));
    return postfix(rest, tok);
}

static Node *cast(Token **rest, Token *tok)
{
    if (tok_is(tok, "(") && is_typename(tok->next)) {
        Token *start = tok;
        Type *ty = typename_(&tok, tok->next);
        Node *n;
        tok = skip(tok, ")");
        if (tok_is(tok, "{")) error_tok(start, "BCC todavia no tiene literales compuestos ((tipo){...})");
        n = new_cast(cast(rest, tok), ty);
        n->tok = start;
        return n;
    }
    return unary(rest, tok);
}

static Node *mul(Token **rest, Token *tok)
{
    Node *n = cast(&tok, tok);
    for (;;) {
        Token *s = tok;
        if (tok_is(tok, "*")) { n = new_binary(ND_MUL, n, cast(&tok, tok->next), s); continue; }
        if (tok_is(tok, "/")) { n = new_binary(ND_DIV, n, cast(&tok, tok->next), s); continue; }
        if (tok_is(tok, "%")) { n = new_binary(ND_MOD, n, cast(&tok, tok->next), s); continue; }
        *rest = tok;
        return n;
    }
}
static Node *add(Token **rest, Token *tok)
{
    Node *n = mul(&tok, tok);
    for (;;) {
        Token *s = tok;
        if (tok_is(tok, "+")) { n = new_add(n, mul(&tok, tok->next), s); continue; }
        if (tok_is(tok, "-")) { n = new_sub(n, mul(&tok, tok->next), s); continue; }
        *rest = tok;
        return n;
    }
}
static Node *shift(Token **rest, Token *tok)
{
    Node *n = add(&tok, tok);
    for (;;) {
        Token *s = tok;
        if (tok_is(tok, "<<")) { n = new_binary(ND_SHL, n, add(&tok, tok->next), s); continue; }
        if (tok_is(tok, ">>")) { n = new_binary(ND_SHR, n, add(&tok, tok->next), s); continue; }
        *rest = tok;
        return n;
    }
}
static Node *relational(Token **rest, Token *tok)
{
    Node *n = shift(&tok, tok);
    for (;;) {
        Token *s = tok;
        if (tok_is(tok, "<")) { n = new_binary(ND_LT, n, shift(&tok, tok->next), s); continue; }
        if (tok_is(tok, "<=")) { n = new_binary(ND_LE, n, shift(&tok, tok->next), s); continue; }
        if (tok_is(tok, ">")) { n = new_binary(ND_LT, shift(&tok, tok->next), n, s); continue; }
        if (tok_is(tok, ">=")) { n = new_binary(ND_LE, shift(&tok, tok->next), n, s); continue; }
        *rest = tok;
        return n;
    }
}
static Node *equality(Token **rest, Token *tok)
{
    Node *n = relational(&tok, tok);
    for (;;) {
        Token *s = tok;
        if (tok_is(tok, "==")) { n = new_binary(ND_EQ, n, relational(&tok, tok->next), s); continue; }
        if (tok_is(tok, "!=")) { n = new_binary(ND_NE, n, relational(&tok, tok->next), s); continue; }
        *rest = tok;
        return n;
    }
}
static Node *bitand(Token **rest, Token *tok)
{
    Node *n = equality(&tok, tok);
    while (tok_is(tok, "&")) { Token *s = tok; n = new_binary(ND_BITAND, n, equality(&tok, tok->next), s); }
    *rest = tok;
    return n;
}
static Node *bitxor(Token **rest, Token *tok)
{
    Node *n = bitand(&tok, tok);
    while (tok_is(tok, "^")) { Token *s = tok; n = new_binary(ND_BITXOR, n, bitand(&tok, tok->next), s); }
    *rest = tok;
    return n;
}
static Node *bitor(Token **rest, Token *tok)
{
    Node *n = bitxor(&tok, tok);
    while (tok_is(tok, "|")) { Token *s = tok; n = new_binary(ND_BITOR, n, bitxor(&tok, tok->next), s); }
    *rest = tok;
    return n;
}
static Node *logand(Token **rest, Token *tok)
{
    Node *n = bitor(&tok, tok);
    while (tok_is(tok, "&&")) { Token *s = tok; n = new_binary(ND_LOGAND, n, bitor(&tok, tok->next), s); }
    *rest = tok;
    return n;
}
static Node *logor(Token **rest, Token *tok)
{
    Node *n = logand(&tok, tok);
    while (tok_is(tok, "||")) { Token *s = tok; n = new_binary(ND_LOGOR, n, logand(&tok, tok->next), s); }
    *rest = tok;
    return n;
}
static Node *conditional(Token **rest, Token *tok)
{
    Node *c = logor(&tok, tok), *n;
    if (!tok_is(tok, "?")) { *rest = tok; return c; }
    n = new_node(ND_COND, tok);
    n->cond = c;
    n->then = expr(&tok, tok->next);
    tok = skip(tok, ":");
    n->els = conditional(rest, tok);
    return n;
}
static Node *assign(Token **rest, Token *tok)
{
    Node *n = conditional(&tok, tok);
    static const char *ops[] = { "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", 0 };
    static const int kinds[] = { ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_BITAND, ND_BITOR, ND_BITXOR, ND_SHL, ND_SHR };
    int i;
    if (tok_is(tok, "=")) return new_binary(ND_ASSIGN, n, assign(rest, tok->next), tok);
    for (i = 0; ops[i]; i++)
        if (tok_is(tok, ops[i])) {
            Token *s = tok;
            Node *r = assign(rest, tok->next);
            return to_assign(new_binary(kinds[i], n, r, s));
        }
    *rest = tok;
    return n;
}
static Node *expr(Token **rest, Token *tok)
{
    Node *n = assign(&tok, tok);
    if (tok_is(tok, ",")) return new_binary(ND_COMMA, n, expr(rest, tok->next), tok);
    *rest = tok;
    return n;
}

/* ======================= evaluacion de constantes ======================= */
static i64 eval_rval(Node *n, Obj **label);
static i64 eval2(Node *n, Obj **label)
{
    add_type(n);
    switch (n->kind) {
    case ND_ADD: return eval2(n->lhs, label) + eval(n->rhs);
    case ND_SUB: return eval2(n->lhs, label) - eval(n->rhs);
    case ND_MUL: return eval(n->lhs) * eval(n->rhs);
    case ND_DIV: {
        i64 b = eval(n->rhs);
        if (!b) error_tok(n->tok, "division entre cero");
        if (n->ty->is_unsigned) return (i64)((u64)eval(n->lhs) / (u64)b);
        return eval(n->lhs) / b;
    }
    case ND_MOD: {
        i64 b = eval(n->rhs);
        if (!b) error_tok(n->tok, "division entre cero");
        if (n->ty->is_unsigned) return (i64)((u64)eval(n->lhs) % (u64)b);
        return eval(n->lhs) % b;
    }
    case ND_NEG: return -eval(n->lhs);
    case ND_BITAND: return eval(n->lhs) & eval(n->rhs);
    case ND_BITOR: return eval(n->lhs) | eval(n->rhs);
    case ND_BITXOR: return eval(n->lhs) ^ eval(n->rhs);
    case ND_SHL: return eval(n->lhs) << eval(n->rhs);
    case ND_SHR:
        if (n->ty->is_unsigned && n->ty->size == 8) return (i64)((u64)eval(n->lhs) >> eval(n->rhs));
        if (n->ty->is_unsigned) return (i64)((unsigned)eval(n->lhs) >> eval(n->rhs));
        return eval(n->lhs) >> eval(n->rhs);
    case ND_EQ: return eval(n->lhs) == eval(n->rhs);
    case ND_NE: return eval(n->lhs) != eval(n->rhs);
    case ND_LT: return n->lhs->ty->is_unsigned ? (u64)eval(n->lhs) < (u64)eval(n->rhs) : eval(n->lhs) < eval(n->rhs);
    case ND_LE: return n->lhs->ty->is_unsigned ? (u64)eval(n->lhs) <= (u64)eval(n->rhs) : eval(n->lhs) <= eval(n->rhs);
    case ND_COND: return eval(n->cond) ? eval2(n->then, label) : eval2(n->els, label);
    case ND_COMMA: return eval2(n->rhs, label);
    case ND_NOT: return !eval(n->lhs);
    case ND_BITNOT: return ~eval(n->lhs);
    case ND_LOGAND: return eval(n->lhs) && eval(n->rhs);
    case ND_LOGOR: return eval(n->lhs) || eval(n->rhs);
    case ND_CAST: {
        i64 v = eval2(n->lhs, label);
        if (is_integer(n->ty)) {
            switch (n->ty->size) {
            case 1: return n->ty->kind == TY_BOOL ? v != 0 : n->ty->is_unsigned ? (u8)v : (signed char)v;
            case 2: return n->ty->is_unsigned ? (unsigned short)v : (short)v;
            case 4: return n->ty->is_unsigned ? (unsigned)v : (int)v;
            }
        }
        return v;
    }
    case ND_ADDR: return eval_rval(n->lhs, label);
    case ND_MEMBER:
        if (!label) error_tok(n->tok, "no es una constante");
        if (n->ty->kind != TY_ARRAY) error_tok(n->tok, "direccion no valida en un valor inicial");
        return eval_rval(n->lhs, label) + n->member->offset;
    case ND_VAR:
        if (!label) error_tok(n->tok, "no es una constante");
        if (n->var->ty->kind != TY_ARRAY && n->var->ty->kind != TY_FUNC) error_tok(n->tok, "valor inicial que no es constante");
        if (n->var->is_local) error_tok(n->tok, "valor inicial que no es constante");
        *label = n->var;
        return 0;
    case ND_NUM: return n->val;
    }
    error_tok(n->tok, "no es una constante");
    return 0;
}
static i64 eval_rval(Node *n, Obj **label)
{
    switch (n->kind) {
    case ND_VAR:
        if (n->var->is_local) error_tok(n->tok, "valor inicial que no es constante");
        *label = n->var;
        return 0;
    case ND_DEREF: return eval2(n->lhs, label);
    case ND_MEMBER: return eval_rval(n->lhs, label) + n->member->offset;
    }
    error_tok(n->tok, "valor inicial que no es constante");
    return 0;
}
static i64 eval(Node *n) { return eval2(n, 0); }
static i64 const_expr(Token **rest, Token *tok) { Node *n = conditional(rest, tok); return eval(n); }

/* ======================= valores iniciales ======================= */
typedef struct Init {
    struct Init *next;
    Type *ty;
    Token *tok;
    int is_flexible;
    Node *expr;
    struct Init **children;
    Member *val_member;
} Init;

static Init *new_init(Type *ty, int is_flexible)
{
    Init *in = zalloc(sizeof *in);
    int i;
    in->ty = ty;
    if (ty->kind == TY_ARRAY) {
        if (is_flexible && ty->size < 0) { in->is_flexible = 1; return in; }
        in->children = zalloc(sizeof(Init *) * (ty->array_len > 0 ? ty->array_len : 1));
        for (i = 0; i < ty->array_len; i++) in->children[i] = new_init(ty->base, 0);
        return in;
    }
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        Member *m;
        int len = 0;
        for (m = ty->members; m; m = m->next) len++;
        in->children = zalloc(sizeof(Init *) * (len + 1));
        for (m = ty->members; m; m = m->next) {
            if (is_flexible && ty->is_flexible && !m->next) {
                Init *c = zalloc(sizeof *c);
                c->ty = m->ty; c->is_flexible = 1;
                in->children[m->idx] = c;
            } else
                in->children[m->idx] = new_init(m->ty, 0);
        }
        return in;
    }
    return in;
}

static Token *skip_excess(Token *tok)
{
    if (tok_is(tok, "{")) {
        tok = skip_excess(tok->next);
        return skip(tok, "}");
    }
    assign(&tok, tok);
    return tok;
}

static void initializer2(Token **rest, Token *tok, Init *in);

static void string_initializer(Token **rest, Token *tok, Init *in)
{
    int len, i;
    if (in->is_flexible) {
        Init *n = new_init(array_of(in->ty->base, tok->slen), 0);
        *in = *n;
    }
    len = in->ty->array_len < tok->slen ? in->ty->array_len : tok->slen;
    for (i = 0; i < len; i++) in->children[i]->expr = new_num((signed char)tok->str[i], tok);
    *rest = tok->next;
}

static int count_array_init(Token *tok, Type *ty)
{
    Init *dummy = new_init(ty->base, 0);
    int i = 0, max = 0;
    while (!tok_is(tok, "}")) {
        if (i > 0 || max > 0) tok = skip(tok, ",");
        if (tok_is(tok, "}")) break;
        if (tok_is(tok, "[")) {
            i = (int)const_expr(&tok, tok->next);
            tok = skip(tok, "]");
            tok = skip(tok, "=");
        }
        initializer2(&tok, tok, dummy);
        i++;
        if (i > max) max = i;
    }
    return max;
}

static void array_initializer1(Token **rest, Token *tok, Init *in)
{
    int i = 0, first = 1;
    tok = skip(tok, "{");
    if (in->is_flexible) {
        int len = count_array_init(tok, in->ty);
        *in = *new_init(array_of(in->ty->base, len), 0);
    }
    while (!consume(rest, tok, "}")) {
        if (!first) { tok = skip(tok, ","); if (consume(rest, tok, "}")) return; }
        first = 0;
        if (tok_is(tok, "[")) {
            i = (int)const_expr(&tok, tok->next);
            tok = skip(tok, "]");
            tok = skip(tok, "=");
        }
        if (i < in->ty->array_len) initializer2(&tok, tok, in->children[i]);
        else tok = skip_excess(tok);
        i++;
    }
}

static void array_initializer2(Token **rest, Token *tok, Init *in)
{
    int i;
    if (in->is_flexible) {
        int len = count_array_init(tok, in->ty);
        *in = *new_init(array_of(in->ty->base, len), 0);
    }
    for (i = 0; i < in->ty->array_len && !tok_is(tok, "}"); i++) {
        if (i > 0) tok = skip(tok, ",");
        initializer2(&tok, tok, in->children[i]);
    }
    *rest = tok;
}

static void struct_initializer1(Token **rest, Token *tok, Init *in)
{
    Member *m = in->ty->members;
    int first = 1;
    tok = skip(tok, "{");
    while (!consume(rest, tok, "}")) {
        if (!first) { tok = skip(tok, ","); if (consume(rest, tok, "}")) return; }
        first = 0;
        if (tok_is(tok, ".")) {
            Member *mm = get_member(in->ty, tok->next);
            if (!mm) error_tok2(tok->next, "ese struct no tiene ese campo", tok_name(tok->next));
            if (!mm->name) error_tok(tok, "BCC no inicializa campos de structs anonimos por nombre");
            m = mm;
            tok = skip(tok->next->next, "=");
        }
        if (m) { initializer2(&tok, tok, in->children[m->idx]); m = m->next; }
        else tok = skip_excess(tok);
    }
}

static void struct_initializer2(Token **rest, Token *tok, Init *in)
{
    Member *m;
    int first = 1;
    for (m = in->ty->members; m && !tok_is(tok, "}"); m = m->next) {
        if (!first) tok = skip(tok, ",");
        first = 0;
        initializer2(&tok, tok, in->children[m->idx]);
    }
    *rest = tok;
}

static void union_initializer(Token **rest, Token *tok, Init *in)
{
    if (tok_is(tok, "{")) {
        Member *m = in->ty->members;
        tok = tok->next;
        if (tok_is(tok, ".")) {
            m = get_member(in->ty, tok->next);
            if (!m) error_tok(tok->next, "ese union no tiene ese campo");
            tok = skip(tok->next->next, "=");
        }
        in->expr = 0;
        initializer2(&tok, tok, in->children[m->idx]);
        in->val_member = m;
        consume(&tok, tok, ",");
        *rest = skip(tok, "}");
    } else {
        initializer2(rest, tok, in->children[in->ty->members->idx]);
        in->val_member = in->ty->members;
    }
}

static void initializer2(Token **rest, Token *tok, Init *in)
{
    if (in->ty->kind == TY_ARRAY && tok->kind == TK_STR) { string_initializer(rest, tok, in); return; }
    if (in->ty->kind == TY_ARRAY) {
        if (tok_is(tok, "{")) array_initializer1(rest, tok, in);
        else array_initializer2(rest, tok, in);
        return;
    }
    if (in->ty->kind == TY_STRUCT) {
        if (tok_is(tok, "{")) { struct_initializer1(rest, tok, in); return; }
        {
            Node *e = assign(rest, tok);
            add_type(e);
            if (e->ty->kind == TY_STRUCT) { in->expr = e; return; }
        }
        struct_initializer2(rest, tok, in);
        return;
    }
    if (in->ty->kind == TY_UNION) { union_initializer(rest, tok, in); return; }
    if (tok_is(tok, "{")) {           /* int x = {3}; */
        initializer2(&tok, tok->next, in);
        consume(&tok, tok, ",");
        *rest = skip(tok, "}");
        return;
    }
    in->expr = assign(rest, tok);
}

static Init *initializer(Token **rest, Token *tok, Type *ty, Type **new_ty)
{
    Init *in = new_init(ty, 1);
    initializer2(rest, tok, in);
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->is_flexible) {
        Type *t = copy_type(ty);
        Member head = {0}, *cur = &head, *m;
        for (m = ty->members; m; m = m->next) {
            Member *c = zalloc(sizeof *c);
            *c = *m;
            cur = cur->next = c;
        }
        cur->ty = in->children[cur->idx]->ty;
        t->size += cur->ty->size;
        t->members = head.next;
        *new_ty = t;
        return in;
    }
    *new_ty = in->ty;
    return in;
}

/* valores iniciales de variables locales: se convierten en asignaciones */
typedef struct InitDesg { struct InitDesg *next; int idx; Member *member; Obj *var; } InitDesg;

static Node *init_desg_expr(InitDesg *d, Token *tok)
{
    if (d->var) return new_var_node(d->var, tok);
    if (d->member) { Node *n = new_unary(ND_MEMBER, init_desg_expr(d->next, tok), tok); n->member = d->member; return n; }
    {
        Node *l = init_desg_expr(d->next, tok);
        Node *r = new_num(d->idx, tok);
        return new_unary(ND_DEREF, new_add(l, r, tok), tok);
    }
}

static Node *create_lvar_init(Init *in, Type *ty, InitDesg *d, Token *tok)
{
    if (ty->kind == TY_ARRAY) {
        Node *n = new_node(ND_NULL, tok);
        int i;
        for (i = 0; i < ty->array_len; i++) {
            InitDesg d2 = { d, i, 0, 0 };
            Node *r = create_lvar_init(in->children[i], ty->base, &d2, tok);
            n = new_binary(ND_COMMA, n, r, tok);
        }
        return n;
    }
    if (ty->kind == TY_STRUCT && !in->expr) {
        Node *n = new_node(ND_NULL, tok);
        Member *m;
        for (m = ty->members; m; m = m->next) {
            InitDesg d2 = { d, 0, m, 0 };
            Node *r = create_lvar_init(in->children[m->idx], m->ty, &d2, tok);
            n = new_binary(ND_COMMA, n, r, tok);
        }
        return n;
    }
    if (ty->kind == TY_UNION) {
        Member *m = in->val_member ? in->val_member : ty->members;
        InitDesg d2 = { d, 0, m, 0 };
        if (!m) return new_node(ND_NULL, tok);
        return create_lvar_init(in->children[m->idx], m->ty, &d2, tok);
    }
    if (!in->expr) return new_node(ND_NULL, tok);
    {
        Node *l = init_desg_expr(d, tok);
        return new_binary(ND_ASSIGN, l, in->expr, tok);
    }
}

static Node *lvar_initializer(Token **rest, Token *tok, Obj *var)
{
    Init *in = initializer(rest, tok, var->ty, &var->ty);
    InitDesg d = { 0, 0, 0, var };
    Node *lhs = new_node(ND_MEMZERO, tok), *rhs;
    lhs->var = var;
    rhs = create_lvar_init(in, var->ty, &d, tok);
    return new_binary(ND_COMMA, lhs, rhs, tok);
}

/* valores iniciales de globales: bytes + relocaciones */
static void write_buf(char *buf, u64 val, int sz)
{
    int i;
    for (i = 0; i < sz; i++) buf[i] = (char)(val >> (8 * i));
}

static Reloc *write_gvar_data(Reloc *cur, Init *in, Type *ty, char *buf, int offset)
{
    if (ty->kind == TY_ARRAY) {
        int i, sz = ty->base->size;
        for (i = 0; i < ty->array_len; i++)
            cur = write_gvar_data(cur, in->children[i], ty->base, buf, offset + sz * i);
        return cur;
    }
    if (ty->kind == TY_STRUCT && !in->expr) {
        Member *m;
        for (m = ty->members; m; m = m->next)
            cur = write_gvar_data(cur, in->children[m->idx], m->ty, buf, offset + m->offset);
        return cur;
    }
    if (ty->kind == TY_UNION) {
        Member *m = in->val_member;
        if (!m) return cur;
        return write_gvar_data(cur, in->children[m->idx], m->ty, buf, offset + m->offset);
    }
    if (!in->expr) return cur;
    if (ty->kind == TY_STRUCT) error_tok(in->expr->tok, "valor inicial de struct no constante");
    {
        Obj *label = 0;
        i64 val = eval2(in->expr, &label);
        if (!label) { write_buf(buf + offset, (u64)val, ty->size); return cur; }
        if (ty->size != 8) error_tok(in->expr->tok, "una direccion solo cabe en un puntero o long");
        {
            Reloc *r = zalloc(sizeof *r);
            r->offset = offset; r->to = label; r->addend = val;
            label->used = 1;
            cur->next = r;
            return r;
        }
    }
}

static void gvar_initializer(Token **rest, Token *tok, Obj *var)
{
    Init *in = initializer(rest, tok, var->ty, &var->ty);
    Reloc head = {0};
    char *buf = zalloc(var->ty->size > 0 ? var->ty->size : 1);
    write_gvar_data(&head, in, var->ty, buf, 0);
    var->init_data = buf;
    var->rel = head.next;
}

/* ======================= sentencias ======================= */
static Node *declaration(Token **rest, Token *tok, Type *basety, int is_static)
{
    Node head = {0}, *cur = &head, *blk;
    int i = 0;
    while (!tok_is(tok, ";")) {
        Type *ty;
        Obj *var;
        if (i++ > 0) {
            if (!tok_is(tok, ",")) error_tok(tok, "falta un ';' antes de esto");
            tok = tok->next;
        }
        ty = declarator(&tok, tok, basety);
        if (ty->kind == TY_VOID) error_tok(tok, "variable de tipo void");
        if (!ty->name) error_tok(ty->name_pos, "falta el nombre de la variable");
        if (ty->kind == TY_FUNC) {            /* declaracion de funcion dentro de otra */
            Scope *sc = scope;
            VarScope *vs;
            while (sc->next) sc = sc->next;
            vs = find_var(ty->name);
            if (!vs || !vs->var) {
                Scope *save = scope; Obj *f;
                scope = sc;
                f = new_gvar(tok_name(ty->name), ty);
                f->is_function = 1;
                scope = save;
                push_scope(f->name)->var = f;
            }
            continue;
        }
        if (is_static) {
            var = new_anon_gvar(ty);
            push_scope(tok_name(ty->name))->var = var;
            if (consume(&tok, tok, "=")) gvar_initializer(&tok, tok, var);
            continue;
        }
        var = new_lvar(tok_name(ty->name), ty);
        var->tok = ty->name;
        if (consume(&tok, tok, "=")) {
            Node *e = lvar_initializer(&tok, tok, var);
            cur = cur->next = new_unary(ND_EXPR_STMT, e, tok);
        }
        if (var->ty->size < 0) error_tok(ty->name, "variable de tipo incompleto");
        if (var->ty->kind == TY_VOID) error_tok(ty->name, "variable void");
    }
    blk = new_node(ND_BLOCK, tok);
    blk->body = head.next;
    *rest = tok->next;
    return blk;
}

static int is_end(Token *tok) { return tok_is(tok, "}") || (tok_is(tok, ",") && tok_is(tok->next, "}")); }

static void parse_typedef(Token **rest, Token *tok, Type *basety)
{
    int first = 1;
    while (!consume(&tok, tok, ";")) {
        Type *ty;
        if (!first) tok = skip(tok, ",");
        first = 0;
        ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(ty->name_pos, "typedef sin nombre");
        push_scope(tok_name(ty->name))->type_def = ty;
    }
    *rest = tok;
}

static Node *stmt(Token **rest, Token *tok)
{
    if (tok_eq_name(tok, "return")) {
        Node *n = new_node(ND_RETURN, tok);
        if (consume(rest, tok->next, ";")) return n;
        n->lhs = expr(&tok, tok->next);
        *rest = skip(tok, ";");
        add_type(n->lhs);
        if (cur_fn->ty->return_ty->kind != TY_VOID && cur_fn->ty->return_ty->kind != TY_STRUCT && cur_fn->ty->return_ty->kind != TY_UNION)
            n->lhs = new_cast(n->lhs, cur_fn->ty->return_ty);
        return n;
    }
    if (tok_eq_name(tok, "if")) {
        Node *n = new_node(ND_IF, tok);
        tok = skip(tok->next, "(");
        n->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        n->then = stmt(&tok, tok);
        if (tok_eq_name(tok, "else")) n->els = stmt(&tok, tok->next);
        *rest = tok;
        return n;
    }
    if (tok_eq_name(tok, "switch")) {
        Node *n = new_node(ND_SWITCH, tok), *sw = cur_switch;
        int brk = brk_label;
        tok = skip(tok->next, "(");
        n->cond = expr(&tok, tok);
        add_type(n->cond);
        if (n->cond->ty->size < 8) n->cond = new_cast(n->cond, n->cond->ty->is_unsigned ? ty_ulong : ty_long);
        tok = skip(tok, ")");
        cur_switch = n;
        brk_label = n->brk_label = new_label();
        n->then = stmt(rest, tok);
        cur_switch = sw;
        brk_label = brk;
        return n;
    }
    if (tok_eq_name(tok, "case")) {
        Node *n = new_node(ND_CASE, tok);
        i64 v;
        if (!cur_switch) error_tok(tok, "case fuera de un switch");
        v = const_expr(&tok, tok->next);
        if (tok_is(tok, "...")) { error_tok(tok, "case a ... b no esta soportado"); }
        tok = skip(tok, ":");
        n->label_id = new_label();
        n->case_val = v;
        n->lhs = is_end(tok) ? new_node(ND_BLOCK, tok) : stmt(rest, tok);
        if (is_end(tok)) *rest = tok;
        n->case_next = cur_switch->case_next;
        cur_switch->case_next = n;
        return n;
    }
    if (tok_eq_name(tok, "default")) {
        Node *n = new_node(ND_CASE, tok);
        if (!cur_switch) error_tok(tok, "default fuera de un switch");
        tok = skip(tok->next, ":");
        n->label_id = new_label();
        n->lhs = is_end(tok) ? new_node(ND_BLOCK, tok) : stmt(rest, tok);
        if (is_end(tok)) *rest = tok;
        cur_switch->default_case = n;
        return n;
    }
    if (tok_eq_name(tok, "for")) {
        Node *n = new_node(ND_FOR, tok);
        int brk = brk_label, cont = cont_label;
        tok = skip(tok->next, "(");
        enter_scope();
        brk_label = n->brk_label = new_label();
        cont_label = n->cont_label = new_label();
        if (is_typename(tok)) {
            Type *basety = declspec(&tok, tok, 0);
            n->init = declaration(&tok, tok, basety, 0);
        } else if (consume(&tok, tok, ";")) {
        } else {
            n->init = new_unary(ND_EXPR_STMT, expr(&tok, tok), tok);
            tok = skip(tok, ";");
        }
        if (!tok_is(tok, ";")) n->cond = expr(&tok, tok);
        tok = skip(tok, ";");
        if (!tok_is(tok, ")")) n->inc = expr(&tok, tok);
        tok = skip(tok, ")");
        n->then = stmt(rest, tok);
        leave_scope();
        brk_label = brk; cont_label = cont;
        return n;
    }
    if (tok_eq_name(tok, "while")) {
        Node *n = new_node(ND_FOR, tok);
        int brk = brk_label, cont = cont_label;
        tok = skip(tok->next, "(");
        n->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        brk_label = n->brk_label = new_label();
        cont_label = n->cont_label = new_label();
        n->then = stmt(rest, tok);
        brk_label = brk; cont_label = cont;
        return n;
    }
    if (tok_eq_name(tok, "do")) {
        Node *n = new_node(ND_DO, tok);
        int brk = brk_label, cont = cont_label;
        brk_label = n->brk_label = new_label();
        cont_label = n->cont_label = new_label();
        n->then = stmt(&tok, tok->next);
        brk_label = brk; cont_label = cont;
        if (!tok_eq_name(tok, "while")) error_tok(tok, "falta while despues de do");
        tok = skip(tok->next, "(");
        n->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        *rest = skip(tok, ";");
        return n;
    }
    if (tok_eq_name(tok, "goto")) {
        Node *n = new_node(ND_GOTO, tok);
        n->label = tok_name(tok->next);
        n->case_next = gotos;
        gotos = n;
        *rest = skip(tok->next->next, ";");
        return n;
    }
    if (tok_eq_name(tok, "break")) {
        Node *n = new_node(ND_BREAK, tok);
        if (!brk_label) error_tok(tok, "break fuera de un ciclo o switch");
        n->label_id = brk_label;
        *rest = skip(tok->next, ";");
        return n;
    }
    if (tok_eq_name(tok, "continue")) {
        Node *n = new_node(ND_BREAK, tok);
        if (!cont_label) error_tok(tok, "continue fuera de un ciclo");
        n->label_id = cont_label;
        *rest = skip(tok->next, ";");
        return n;
    }
    if (tok->kind == TK_IDENT && tok_is(tok->next, ":") && !find_typedef(tok)) {
        Node *n = new_node(ND_LABEL, tok);
        n->label = tok_name(tok);
        n->label_id = new_label();
        n->case_next = labels;
        labels = n;
        tok = tok->next->next;
        n->lhs = is_end(tok) ? new_node(ND_BLOCK, tok) : stmt(rest, tok);
        if (is_end(tok)) *rest = tok;
        return n;
    }
    if (tok_is(tok, "{")) return compound_stmt(rest, tok->next);
    if (tok_is(tok, ";")) { *rest = tok->next; return new_node(ND_BLOCK, tok); }
    {
        Node *n = new_node(ND_EXPR_STMT, tok);
        n->lhs = expr(&tok, tok);
        *rest = skip(tok, ";");
        return n;
    }
}

static Node *compound_stmt(Token **rest, Token *tok)
{
    Node *n = new_node(ND_BLOCK, tok), head = {0}, *cur = &head;
    enter_scope();
    while (!tok_is(tok, "}")) {
        if (tok->kind == TK_EOF) error_tok(tok, "falta } al final");
        if (is_typename(tok) && !tok_is(tok->next, ":")) {
            VarAttr attr = {0};
            Type *basety = declspec(&tok, tok, &attr);
            if (attr.is_typedef) { parse_typedef(&tok, tok, basety); continue; }
            if (attr.is_extern) {
                /* extern int x; dentro de una funcion: se busca la global */
                while (!consume(&tok, tok, ";")) tok = tok->next;
                continue;
            }
            cur = cur->next = declaration(&tok, tok, basety, attr.is_static);
        } else
            cur = cur->next = stmt(&tok, tok);
        add_type(cur);
    }
    leave_scope();
    n->body = head.next;
    *rest = tok->next;
    return n;
}

/* ======================= nivel superior ======================= */
static void create_param_lvars(Type *param)
{
    if (param) {
        create_param_lvars(param->next);
        if (!param->name) error_tok(param->name_pos, "parametro sin nombre");
        new_lvar(tok_name(param->name), param);
    }
}

static void resolve_goto_labels(void)
{
    Node *x, *y;
    for (x = gotos; x; x = x->case_next) {
        for (y = labels; y; y = y->case_next)
            if (seq(x->label, y->label)) { x->label_id = y->label_id; break; }
        if (!y) error_tok2(x->tok, "goto a una etiqueta que no existe", x->label);
    }
    gotos = labels = 0;
}

static Obj *find_func(char *name)
{
    Scope *sc = scope;
    VarScope *v;
    while (sc->next) sc = sc->next;
    for (v = sc->vars; v; v = v->next)
        if (seq(v->name, name) && v->var) return v->var;
    return 0;
}

static Token *function(Token *tok, Type *basety, VarAttr *attr)
{
    Type *ty = declarator(&tok, tok, basety);
    char *name;
    Obj *fn;
    Obj *p;
    if (!ty->name) error_tok(ty->name_pos, "funcion sin nombre");
    name = tok_name(ty->name);
    fn = find_func(name);
    if (fn) {
        if (!fn->is_function) error_tok(ty->name, "ese nombre ya es una variable");
        if (fn->is_definition && tok_is(tok, "{")) {
            if (fn->from_lib && !parsing_lib) {
                fn->is_definition = 0;          /* el programa reemplaza a la de la biblioteca */
                fn->body = 0;
            } else
                error_tok2(ty->name, "la funcion ya estaba definida", name);
        }
        if (tok_is(tok, "{") || !fn->ty->has_proto) fn->ty = ty;
    } else {
        fn = new_gvar(name, ty);
        fn->is_function = 1;
    }
    fn->is_static = attr->is_static || (attr->is_inline && !attr->is_extern);
    if (attr->callback) fn->callback = 1;
    fn->tok = ty->name;
    if (consume(&tok, tok, ";")) return tok;
    fn->is_definition = 1;
    fn->from_lib = parsing_lib;
    cur_fn = fn;
    locals = 0;
    enter_scope();
    create_param_lvars(ty->params);
    fn->params = locals;
    for (p = fn->params; p; p = p->next) p->is_param = 1;
    tok = skip(tok, "{");
    {
        /* __func__ */
        Obj *v = new_string_literal(name, slen(name) + 1);
        push_scope("__func__")->var = v;
        push_scope("__FUNCTION__")->var = v;
    }
    fn->body = compound_stmt(&tok, tok);
    fn->locals = locals;
    leave_scope();
    resolve_goto_labels();
    return tok;
}

static Token *global_variable(Token *tok, Type *basety, VarAttr *attr)
{
    int first = 1;
    while (!consume(&tok, tok, ";")) {
        Type *ty;
        Obj *var;
        VarScope *old;
        if (!first) {
            if (!tok_is(tok, ",")) error_tok(tok, "falta un ';' antes de esto");
            tok = tok->next;
        }
        first = 0;
        ty = declarator(&tok, tok, basety);
        if (!ty->name) error_tok(ty->name_pos, "variable sin nombre");
        if (ty->kind == TY_FUNC) {             /* int f(void), g(int); */
            char *nm = tok_name(ty->name);
            Obj *f = find_func(nm);
            if (!f) { f = new_gvar(nm, ty); f->is_function = 1; }
            continue;
        }
        old = find_var(ty->name);
        if (old && old->var && !old->var->is_local && !old->var->is_function) {
            var = old->var;                    /* extern int x; ... int x = 3; */
            if (ty->size > 0 || var->ty->size < 0) var->ty = ty;
        } else
            var = new_gvar(tok_name(ty->name), ty);
        var->is_static = attr->is_static;
        var->is_definition = !attr->is_extern;
        if (consume(&tok, tok, "=")) { gvar_initializer(&tok, tok, var); var->is_definition = 1; }
    }
    return tok;
}

static int is_function(Token *tok)
{
    Type dummy = {0}, *ty;
    if (tok_is(tok, ";")) return 0;
    ty = declarator(&tok, tok, &dummy);
    return ty->kind == TY_FUNC;
}

static void parse(Token *tok)
{
    while (tok->kind != TK_EOF) {
        VarAttr attr = {0};
        Type *basety;
        if (tok_is(tok, ";")) { tok = tok->next; continue; }
        if (tok_eq_name(tok, "_Static_assert") || tok_eq_name(tok, "static_assert")) {
            while (!tok_is(tok, ";")) tok = tok->next;
            tok = tok->next;
            continue;
        }
        basety = declspec(&tok, tok, &attr);
        if (attr.is_typedef) { parse_typedef(&tok, tok, basety); continue; }
        if (is_function(tok)) { tok = function(tok, basety, &attr); continue; }
        tok = global_variable(tok, basety, &attr);
    }
}
