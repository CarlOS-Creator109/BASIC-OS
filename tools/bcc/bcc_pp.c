/* bcc_pp.c - preprocesador de BCC (parte de bcc.c) */

struct Macro {
    Macro *next;
    char *name;
    int fn, nparams, variadic, busy, special;   /* special: 1 = __LINE__, 2 = __FILE__ */
    char **params;
    Token *body;
};

#define MHASH 512
static Macro *mtab[MHASH];

static unsigned hash_name(const char *s, int n)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < n; i++) h = (h ^ (u8)s[i]) * 16777619u;
    return h & (MHASH - 1);
}

static Macro *find_macro(Token *t)
{
    Macro *m;
    if (t->kind != TK_IDENT)
        return 0;
    for (m = mtab[hash_name(t->loc, t->len)]; m; m = m->next)
        if (sneq(t->loc, t->len, m->name))
            return m;
    return 0;
}

static Macro *add_macro(char *name, int n)
{
    Macro *m = zalloc(sizeof *m), **pp;
    unsigned h = hash_name(name, n);
    for (pp = &mtab[h]; *pp; pp = &(*pp)->next)
        if (sneq(name, n, (*pp)->name)) { *pp = (*pp)->next; break; }
    m->name = sdup(name, n);
    m->next = mtab[h];
    mtab[h] = m;
    return m;
}

static void del_macro(Token *t)
{
    Macro **pp;
    for (pp = &mtab[hash_name(t->loc, t->len)]; *pp; pp = &(*pp)->next)
        if (sneq(t->loc, t->len, (*pp)->name)) { *pp = (*pp)->next; return; }
}

static Token *copy_tok(Token *t)
{
    Token *c = zalloc(sizeof *c);
    *c = *t;
    c->next = 0;
    return c;
}

static Token *eof_tok(Token *like)
{
    Token *t = copy_tok(like);
    t->kind = TK_EOF;
    t->len = 0;
    return t;
}

/* tokens hasta el final de la linea (no incluye el siguiente con bol) */
static Token *line_tokens(Token *tok, Token **rest)
{
    Token head = {0}, *cur = &head;
    while (!tok->bol) {
        cur = cur->next = copy_tok(tok);
        tok = tok->next;
    }
    cur->next = eof_tok(tok);
    *rest = tok;
    return head.next;
}

static Token *skip_line(Token *tok)
{
    while (!tok->bol) tok = tok->next;
    return tok;
}

/* ---- #include ---- */
static char *incl_names[256];
static int nincl_once;
static int incl_depth;

static int once_seen(const char *name)
{
    int i;
    for (i = 0; i < nincl_once; i++) if (seq(incl_names[i], name)) return 1;
    return 0;
}

static File *new_file(char *name, char *text, int len)
{
    File *f = zalloc(sizeof *f);
    f->name = name; f->text = text; f->len = len;
    return f;
}

static char *dir_of(const char *path)
{
    int i, last = -1, n = slen(path);
    for (i = 0; i < n; i++) if (path[i] == '/' || path[i] == '\\' || path[i] == ':') last = i;
    return sdup(path, last + 1);
}

static char *path_join(const char *dir, const char *name, int nlen)
{
    int a = slen(dir);
    char *p = zalloc(a + nlen + 1);
    mcopy(p, dir, a);
    mcopy(p + a, name, nlen);
    return p;
}

static Token *read_include(Token *dirtok, char *name, int nlen, int angle, Token *rest)
{
    char *full = 0, *text = 0;
    int len = 0;
    const char *bt;
    File *f;
    Token *t, *last;
    if (incl_depth > 40)
        error_tok(dirtok, "demasiados #include anidados");
    if (!angle) {
        full = path_join(dir_of(dirtok->file->name), name, nlen);
        text = plat_read(full, &len);
    }
    if (!text) {
        char *nm = sdup(name, nlen);
        bt = bcc_header(nm);
        if (bt) {
            full = path_join("<", nm, nlen);
            full = path_join(full, ">", 1);
            text = (char *)bt;
            len = slen(bt);
        }
    }
    if (!text && angle) {
        full = path_join(dir_of(dirtok->file->name), name, nlen);
        text = plat_read(full, &len);
    }
    if (!text)
        error_tok2(dirtok, "no encuentro el archivo de #include", sdup(name, nlen));
    if (once_seen(full))
        return rest;
    f = new_file(full, text, len);
    t = tokenize(f);
    if (t->kind == TK_EOF)
        return rest;
    for (last = t; last->next->kind != TK_EOF; last = last->next) ;
    last->next = rest;
    return t;
}

/* ---- #define ---- */
static void read_define(Token *tok, Token **rest)
{
    Token *name = tok;
    Macro *m;
    if (name->kind != TK_IDENT)
        error_tok(name, "#define sin nombre");
    m = add_macro(name->loc, name->len);
    tok = tok->next;
    if (!tok->bol && tok_is(tok, "(") && !tok->space) {
        char *ps[64];
        int n = 0;
        m->fn = 1;
        tok = tok->next;
        while (!tok_is(tok, ")")) {
            if (n) {
                if (!tok_is(tok, ",")) error_tok(tok, "falta una coma en los parametros de la macro");
                tok = tok->next;
            }
            if (tok_is(tok, "...")) { m->variadic = 1; tok = tok->next; break; }
            if (tok->kind != TK_IDENT || tok->bol) error_tok(tok, "parametro de macro raro");
            if (n >= 63) error_tok(tok, "demasiados parametros");
            ps[n++] = sdup(tok->loc, tok->len);
            tok = tok->next;
            if (tok_is(tok, "...")) { m->variadic = 1; tok = tok->next; break; }   /* nombre... (GNU) */
        }
        if (!tok_is(tok, ")")) error_tok(tok, "falta ) en la macro");
        tok = tok->next;
        m->nparams = n;
        m->params = zalloc(sizeof(char *) * (n + 1));
        mcopy(m->params, ps, sizeof(char *) * n);
    }
    m->body = line_tokens(tok, rest);
}

/* ---- expansion ---- */
static int param_index(Macro *m, Token *t)
{
    int i;
    if (t->kind != TK_IDENT)
        return -1;
    for (i = 0; i < m->nparams; i++)
        if (sneq(t->loc, t->len, m->params[i])) return i;
    if (m->variadic && sneq(t->loc, t->len, "__VA_ARGS__"))
        return m->nparams;
    return -1;
}

static Token *pp_expand_line(Token *tok);

static Token *stringize(Token *arg, Token *like)
{
    int n = 0, cap = 2;
    Token *t;
    char *buf, *p;
    for (t = arg; t && t->kind != TK_EOF; t = t->next) cap += t->len * 2 + 1;
    buf = zalloc(cap);
    for (t = arg; t && t->kind != TK_EOF; t = t->next) {
        int i;
        if (t != arg && t->space) buf[n++] = ' ';
        for (i = 0; i < t->len; i++) {
            if ((t->kind == TK_STR || (t->kind == TK_NUM && t->loc[0] == '\'')) && (t->loc[i] == '"' || t->loc[i] == '\\'))
                buf[n++] = '\\';
            buf[n++] = t->loc[i];
        }
    }
    t = copy_tok(like);
    t->kind = TK_STR;
    p = zalloc(n + 3);
    p[0] = '"'; mcopy(p + 1, buf, n); p[n + 1] = '"';
    t->loc = p; t->len = n + 2;
    t->str = sdup(buf, n); t->slen = n + 1;
    /* las comillas y barras ya escapadas se decodifican otra vez */
    {
        char *q = t->str, *o = t->str, *e = t->str + n;
        int k = 0;
        while (q < e) {
            if (*q == '\\' && q + 1 < e) { q++; }
            o[k++] = *q++;
        }
        o[k] = 0;
        t->slen = k + 1;
    }
    return t;
}

static Token *paste(Token *a, Token *b)
{
    char *buf = zalloc(a->len + b->len + 1);
    File *f;
    Token *t;
    mcopy(buf, a->loc, a->len);
    mcopy(buf + a->len, b->loc, b->len);
    f = new_file(a->file ? a->file->name : "##", buf, a->len + b->len);
    t = tokenize(f);
    if (t->next->kind != TK_EOF)
        error_tok(a, "## no forma un solo token");
    t->line = a->line; t->file = a->file;
    t->bol = 0; t->space = a->space;
    t->next = 0;
    return t;
}

static Token *copy_list(Token *t, Token ***tailp)
{
    Token head = {0}, *cur = &head;
    for (; t && t->kind != TK_EOF; t = t->next)
        if (t->kind != TK_MARK)
            cur = cur->next = copy_tok(t);
    *tailp = &cur->next;
    return head.next;
}

/* lee los argumentos de una macro con parentesis; tok apunta al '(' */
static Token **read_args(Macro *m, Token *tok, Token **rest, Token *call)
{
    int n = m->nparams + (m->variadic ? 1 : 0), i = 0, depth = 0;
    Token **args = zalloc(sizeof(Token *) * (n + 1));
    Token head = {0}, *cur = &head;
    tok = tok->next;
    for (;;) {
        if (tok->kind == TK_EOF) error_tok(call, "la llamada a la macro no se cierra");
        if (tok->kind == TK_MARK) { tok->mark->busy--; tok->kind = TK_PUNCT; tok->len = 0; tok->mark = 0; tok = tok->next; continue; }
        if (depth == 0 && tok_is(tok, ")")) break;
        if (depth == 0 && tok_is(tok, ",") && !(m->variadic && i == m->nparams)) {
            cur->next = eof_tok(tok);
            if (i < n) args[i] = head.next;
            i++;
            head.next = 0; cur = &head;
            tok = tok->next;
            continue;
        }
        if (tok_is(tok, "(")) depth++;
        if (tok_is(tok, ")")) depth--;
        if (tok->len)
            cur = cur->next = copy_tok(tok);
        tok = tok->next;
    }
    cur->next = eof_tok(tok);
    if (i < n) args[i] = head.next;
    i++;
    if (n == 0 && i == 1 && head.next->kind == TK_EOF) i = 0;
    if (i < m->nparams || (!m->variadic && i > n))
        error_tok2(call, "la macro recibe otro numero de argumentos", m->name);
    for (i = 0; i < n; i++)
        if (!args[i]) args[i] = eof_tok(tok);
    *rest = tok->next;
    return args;
}

static Token *subst(Macro *m, Token **args, Token *call)
{
    Token head = {0}, *cur = &head, *t, **tail;
    for (t = m->body; t->kind != TK_EOF; t = t->next) {
        int pi;
        if (tok_is(t, "#") && m->fn) {
            pi = param_index(m, t->next);
            if (pi < 0) error_tok(t, "# tiene que ir antes de un parametro");
            cur = cur->next = stringize(args[pi], t);
            t = t->next;
            continue;
        }
        if (tok_is(t, "##")) {
            Token *rhs = t->next, *rfirst;
            int rpi = param_index(m, rhs);
            if (cur == &head) error_tok(t, "## al principio de la macro");
            if (rhs->kind == TK_EOF) error_tok(t, "## al final de la macro");
            if (rpi >= 0) {
                rfirst = args[rpi];
                if (rfirst->kind != TK_EOF) {
                    Token *pasted = paste(cur, rfirst), *prev;
                    /* cur se reemplaza por pasted y luego va el resto del argumento */
                    for (prev = &head; prev->next != cur; prev = prev->next) ;
                    prev->next = pasted; cur = pasted;
                    for (rfirst = rfirst->next; rfirst->kind != TK_EOF; rfirst = rfirst->next)
                        cur = cur->next = copy_tok(rfirst);
                } else if (m->variadic && rpi == m->nparams && tok_is(cur, ",")) {
                    Token *prev;                 /* , ## __VA_ARGS__ vacio: se quita la coma (GNU) */
                    for (prev = &head; prev->next != cur; prev = prev->next) ;
                    prev->next = 0; cur = prev;
                }
            } else {
                Token *pasted = paste(cur, rhs), *prev;
                for (prev = &head; prev->next != cur; prev = prev->next) ;
                prev->next = pasted; cur = pasted;
            }
            t = rhs;
            continue;
        }
        pi = m->fn ? param_index(m, t) : -1;
        if (pi >= 0) {
            if (t->next->kind != TK_EOF && tok_is(t->next, "##") && args[pi]->kind == TK_EOF) {
                /* x ## y con x vacio: y va solo */
                t = t->next;            /* el ## */
                if (t->next->kind != TK_EOF) {
                    int r2 = param_index(m, t->next);
                    if (r2 >= 0) { Token *l = copy_list(args[r2], &tail); if (l) { cur->next = l; while (cur->next) cur = cur->next; } }
                    else cur = cur->next = copy_tok(t->next);
                    t = t->next;
                }
                continue;
            }
            {
                /* el argumento se expande antes de meterlo (salvo con # y ##) */
                int glued = t->next->kind != TK_EOF && tok_is(t->next, "##");
                Token *l = copy_list(glued ? args[pi] : pp_expand_line(args[pi]), &tail);
                if (l) {
                    l->space = t->space;
                    cur->next = l;
                    while (cur->next) cur = cur->next;
                }
            }
            continue;
        }
        cur = cur->next = copy_tok(t);
    }
    for (t = head.next; t; t = t->next) { t->line = call->line; t->file = call->file; t->bol = 0; }
    if (head.next) head.next->space = call->space;
    return head.next;
}

/* si tok es una macro, la expande y devuelve la lista nueva (expansion + marca + resto); si no, NULL */
static Token *expand_macro(Token *tok)
{
    Macro *m = find_macro(tok);
    Token *rest, *body, *mark, *t;
    if (!m || m->busy)
        return 0;
    if (m->special) {
        Token *n = copy_tok(tok);
        if (m->special == 1) { n->kind = TK_NUM; n->val = tok->line; n->ty = ty_int; }
        else {
            const char *fn = tok->file ? tok->file->name : "?";
            n->kind = TK_STR; n->str = sdup(fn, slen(fn)); n->slen = slen(fn) + 1;
        }
        n->next = tok->next;
        return n;
    }
    if (m->fn) {
        Token **args;
        if (!tok_is(tok->next, "("))
            return 0;
        args = read_args(m, tok->next, &rest, tok);
        body = subst(m, args, tok);
    } else {
        rest = tok->next;
        body = subst(m, 0, tok);
    }
    m->busy++;
    mark = copy_tok(tok);
    mark->kind = TK_MARK; mark->mark = m; mark->len = 0;
    mark->next = rest;
    if (!body)
        return mark;
    for (t = body; t->next; t = t->next) ;
    t->next = mark;
    return body;
}

/* ---- #if ---- */
static Token *pp_expand_line(Token *tok);
static i64 pp_cond(Token **t);

static i64 pp_primary(Token **pt)
{
    Token *t = *pt;
    i64 v;
    if (tok_is(t, "(")) {
        *pt = t->next;
        v = pp_cond(pt);
        if (!tok_is(*pt, ")")) error_tok(*pt, "falta ) en #if");
        *pt = (*pt)->next;
        return v;
    }
    if (tok_is(t, "!")) { *pt = t->next; return !pp_primary(pt); }
    if (tok_is(t, "~")) { *pt = t->next; return ~pp_primary(pt); }
    if (tok_is(t, "-")) { *pt = t->next; return -pp_primary(pt); }
    if (tok_is(t, "+")) { *pt = t->next; return pp_primary(pt); }
    if (t->kind == TK_NUM) { *pt = t->next; return t->val; }
    if (t->kind == TK_IDENT) { *pt = t->next; return 0; }      /* identificador que no es macro = 0 */
    error_tok(t, "expresion rara en #if");
    return 0;
}
static int pp_prec(Token *t)
{
    static const char *ops[] = { "||", "&&", "|", "^", "&", "==", "!=", "<", ">", "<=", ">=", "<<", ">>", "+", "-", "*", "/", "%", 0 };
    static const int pr[] = { 1, 2, 3, 4, 5, 6, 6, 7, 7, 7, 7, 8, 8, 9, 9, 10, 10, 10 };
    int i;
    if (t->kind != TK_PUNCT) return 0;
    for (i = 0; ops[i]; i++) if (tok_is(t, ops[i])) return pr[i];
    return 0;
}
static i64 pp_bin(Token **pt, int minp)
{
    i64 l = pp_primary(pt);
    for (;;) {
        Token *op = *pt;
        int p = pp_prec(op);
        i64 r;
        if (!p || p < minp) return l;
        *pt = op->next;
        r = pp_bin(pt, p + 1);
        if (tok_is(op, "||")) l = l || r; else if (tok_is(op, "&&")) l = l && r;
        else if (tok_is(op, "|")) l |= r; else if (tok_is(op, "^")) l ^= r; else if (tok_is(op, "&")) l &= r;
        else if (tok_is(op, "==")) l = l == r; else if (tok_is(op, "!=")) l = l != r;
        else if (tok_is(op, "<")) l = l < r; else if (tok_is(op, ">")) l = l > r;
        else if (tok_is(op, "<=")) l = l <= r; else if (tok_is(op, ">=")) l = l >= r;
        else if (tok_is(op, "<<")) l <<= r; else if (tok_is(op, ">>")) l >>= r;
        else if (tok_is(op, "+")) l += r; else if (tok_is(op, "-")) l -= r; else if (tok_is(op, "*")) l *= r;
        else if (tok_is(op, "/")) { if (!r) error_tok(op, "division entre cero en #if"); l /= r; }
        else if (tok_is(op, "%")) { if (!r) error_tok(op, "division entre cero en #if"); l %= r; }
    }
}
static i64 pp_cond(Token **pt)
{
    i64 c = pp_bin(pt, 1);
    if (tok_is(*pt, "?")) {
        i64 a, b;
        *pt = (*pt)->next;
        a = pp_cond(pt);
        if (!tok_is(*pt, ":")) error_tok(*pt, "falta : en #if");
        *pt = (*pt)->next;
        b = pp_cond(pt);
        return c ? a : b;
    }
    return c;
}

static i64 eval_if(Token *tok, Token **rest)
{
    Token *line = line_tokens(tok, rest), head = {0}, *cur = &head, *t;
    i64 v;
    /* defined X / defined(X) antes de expandir */
    for (t = line; t->kind != TK_EOF; t = t->next) {
        if (tok_is(t, "defined") && t->kind == TK_IDENT) {
            Token *n = t->next;
            int par = 0;
            if (tok_is(n, "(")) { par = 1; n = n->next; }
            if (n->kind != TK_IDENT) error_tok(n, "defined sin nombre");
            cur = cur->next = copy_tok(t);
            cur->kind = TK_NUM; cur->val = find_macro(n) != 0; cur->ty = ty_int;
            t = n;
            if (par) { if (!tok_is(t->next, ")")) error_tok(t, "falta ) en defined"); t = t->next; }
            continue;
        }
        cur = cur->next = copy_tok(t);
    }
    cur->next = eof_tok(*rest);
    line = pp_expand_line(head.next);
    t = line;
    v = pp_cond(&t);
    if (t->kind != TK_EOF) error_tok(t, "sobra algo en #if");
    return v;
}

/* salta un bloque falso: devuelve el token '#' de la directiva que lo termina (#elif, #else o #endif) */
static Token *skip_cond(Token *tok)
{
    int depth = 0;
    while (tok->kind != TK_EOF) {
        if (tok->bol && tok_is(tok, "#")) {
            Token *d = tok->next;
            if (!d->bol && (tok_is(d, "if") || tok_is(d, "ifdef") || tok_is(d, "ifndef"))) { depth++; tok = skip_line(d->next); continue; }
            if (!d->bol && tok_is(d, "endif")) { if (!depth) return tok; depth--; tok = skip_line(d->next); continue; }
            if (!d->bol && !depth && (tok_is(d, "elif") || tok_is(d, "else"))) return tok;
        }
        tok = tok->next;
    }
    return tok;
}

typedef struct Cond { struct Cond *up; int taken; Token *tok; } Cond;
static Cond *conds;

/* expansion completa de una lista (sin directivas): para #if y para el resultado final */
static Token *pp_expand_line(Token *tok)
{
    Token head = {0}, *cur = &head;
    while (tok->kind != TK_EOF) {
        Token *e;
        if (tok->kind == TK_MARK) { tok->mark->busy--; tok = tok->next; continue; }
        e = expand_macro(tok);
        if (e) { tok = e; continue; }
        cur = cur->next = copy_tok(tok);
        tok = tok->next;
    }
    cur->next = tok;
    return head.next;
}

static Token *preprocess(Token *tok)
{
    Token head = {0}, *cur = &head;
    while (tok->kind != TK_EOF) {
        Token *e;
        if (tok->kind == TK_MARK) { tok->mark->busy--; tok = tok->next; continue; }
        if (tok->bol && tok_is(tok, "#") && tok->kind == TK_PUNCT) {
            Token *d = tok->next, *start = tok;
            if (d->bol) { tok = d; continue; }                   /* '#' solo: directiva vacia */
            if (tok_is(d, "include")) {
                Token *n = d->next;
                Token *rest;
                if (n->kind == TK_STR && !n->bol) {
                    rest = skip_line(n->next);
                    incl_depth++;
                    tok = read_include(n, n->str, n->slen - 1, 0, rest);
                    continue;
                }
                if (tok_is(n, "<") && !n->bol) {
                    char buf[200];
                    int k = 0;
                    Token *t = n->next;
                    while (!t->bol && !tok_is(t, ">")) {
                        if (k + t->len >= 199) error_tok(t, "nombre de #include muy largo");
                        mcopy(buf + k, t->loc, t->len); k += t->len;
                        t = t->next;
                    }
                    if (!tok_is(t, ">")) error_tok(n, "falta > en #include");
                    rest = skip_line(t->next);
                    incl_depth++;
                    tok = read_include(n, sdup(buf, k), k, 1, rest);
                    continue;
                }
                {   /* #include MACRO */
                    Token *line = pp_expand_line(line_tokens(n, &rest));
                    if (line->kind == TK_STR) { tok = read_include(line, line->str, line->slen - 1, 0, rest); continue; }
                    error_tok(n, "#include espera \"archivo\" o <archivo>");
                }
            }
            if (tok_is(d, "define")) { read_define(d->next, &tok); continue; }
            if (tok_is(d, "undef")) { del_macro(d->next); tok = skip_line(d->next); continue; }
            if (tok_is(d, "ifdef") || tok_is(d, "ifndef")) {
                Cond *c = zalloc(sizeof *c);
                int v = find_macro(d->next) != 0;
                if (tok_is(d, "ifndef")) v = !v;
                c->up = conds; c->taken = v; c->tok = start; conds = c;
                tok = skip_line(d->next);
                if (!v) tok = skip_cond(tok);
                continue;
            }
            if (tok_is(d, "if")) {
                Cond *c = zalloc(sizeof *c);
                i64 v = eval_if(d->next, &tok);
                c->up = conds; c->taken = v != 0; c->tok = start; conds = c;
                if (!v) tok = skip_cond(tok);
                continue;
            }
            if (tok_is(d, "elif")) {
                if (!conds) error_tok(d, "#elif sin #if");
                if (conds->taken) { tok = skip_cond(skip_line(d->next)); continue; }
                if (eval_if(d->next, &tok)) conds->taken = 1;
                else tok = skip_cond(tok);
                continue;
            }
            if (tok_is(d, "else")) {
                if (!conds) error_tok(d, "#else sin #if");
                tok = skip_line(d->next);
                if (conds->taken) tok = skip_cond(tok);
                conds->taken = 1;
                continue;
            }
            if (tok_is(d, "endif")) {
                if (!conds) error_tok(d, "#endif sin #if");
                conds = conds->up;
                tok = skip_line(d->next);
                continue;
            }
            if (tok_is(d, "error")) error_tok(d, "#error");
            if (tok_is(d, "pragma")) {
                if (tok_is(d->next, "once") && !d->next->bol && nincl_once < 256)
                    incl_names[nincl_once++] = d->file->name;
                tok = skip_line(d->next);
                continue;
            }
            if (tok_is(d, "warning") || tok_is(d, "line") || tok_is(d, "ident") || d->kind == TK_NUM) {
                tok = skip_line(d->next);
                continue;
            }
            error_tok(d, "directiva del preprocesador que no conozco");
        }
        e = expand_macro(tok);
        if (e) { tok = e; continue; }
        cur = cur->next = tok;
        tok = tok->next;
    }
    if (conds)
        error_tok(conds->tok, "falta #endif");
    cur->next = tok;
    /* cadenas pegadas: "a" "b" -> "ab" */
    for (cur = head.next; cur && cur->kind != TK_EOF; cur = cur->next) {
        while (cur->kind == TK_STR && cur->next->kind == TK_STR) {
            Token *b = cur->next;
            char *s = zalloc(cur->slen + b->slen);
            mcopy(s, cur->str, cur->slen - 1);
            mcopy(s + cur->slen - 1, b->str, b->slen);
            cur->str = s;
            cur->slen = cur->slen + b->slen - 1;
            cur->next = b->next;
        }
    }
    return head.next;
}

static void define_text(const char *src)
{
    File *f = new_file("<bcc>", (char *)src, slen(src));
    preprocess(tokenize(f));
}
