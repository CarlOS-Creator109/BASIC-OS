/* Cosmos Browser - parser HTML tolerante, CSS básico y cálculo de estilos */
#include "cosmos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int in_list(const char *s, const char *const *l) {
    for (; *l; l++) if (!strcmp(s, *l)) return 1;
    return 0;
}

const char *node_attr(Node *n, const char *name) {
    for (int i = 0; i < n->nattrs; i++) if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].val;
    return NULL;
}

/* ================= entidades ================= */
static const struct { const char *n; int cp; } ents[] = {
    {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",160},{"copy",169},{"reg",174},
    {"trade",8482},{"hellip",8230},{"mdash",8212},{"ndash",8211},{"lsquo",8216},{"rsquo",8217},
    {"ldquo",8220},{"rdquo",8221},{"sbquo",8218},{"bdquo",8222},{"bull",8226},{"middot",183},
    {"laquo",171},{"raquo",187},{"euro",8364},{"pound",163},{"yen",165},{"cent",162},{"deg",176},
    {"times",215},{"divide",247},{"plusmn",177},{"para",182},{"sect",167},{"larr",8592},{"rarr",8594},
    {"uarr",8593},{"darr",8595},{"harr",8596},{"hearts",9829},{"iexcl",161},{"iquest",191},
    {"aacute",225},{"eacute",233},{"iacute",237},{"oacute",243},{"uacute",250},
    {"Aacute",193},{"Eacute",201},{"Iacute",205},{"Oacute",211},{"Uacute",218},
    {"ntilde",241},{"Ntilde",209},{"uuml",252},{"Uuml",220},{"auml",228},{"ouml",246},{"Auml",196},
    {"Ouml",214},{"szlig",223},{"agrave",224},{"egrave",232},{"igrave",236},{"ograve",242},{"ugrave",249},
    {"ccedil",231},{"acirc",226},{"ecirc",234},{"icirc",238},{"ocirc",244},{"ucirc",251},
    {"shy",173},{"ensp",8194},{"emsp",8195},{"thinsp",8201},{"infin",8734},{"ne",8800},{"le",8804},
    {"ge",8805},{"frac12",189},{"frac14",188},{"frac34",190},{"sup2",178},{"sup3",179},{"micro",181},
    {"zwj",8205},{"zwnj",8204},{"lrm",8206},{"rlm",8207},{"check",10003},{"star",9734},{"dagger",8224},
    {"prime",8242},{"minus",8722},{"asymp",8776},{"sum",8721},{"radic",8730},{"alpha",945},{"beta",946},
    {"pi",960},{"mu",956},{"omega",969},{"Omega",937},{"lambda",955},{"sigma",963},{"delta",948},
    {0,0}
};

static char *decode_entities(Arena *a, const char *s, size_t n, int *outlen) {
    char *o = arena_alloc(a, n + 8), *w = o;
    size_t i = 0;
    while (i < n) {
        if (s[i] != '&') { *w++ = s[i++]; continue; }
        size_t j = i + 1;
        int cp = -1;
        if (j < n && s[j] == '#') {
            j++;
            int hex = 0;
            if (j < n && (s[j] == 'x' || s[j] == 'X')) { hex = 1; j++; }
            long v = 0; size_t st = j;
            while (j < n && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j])) && j - st < 8) {
                int dgt = isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower(s[j]) - 'a' + 10);
                v = v * (hex ? 16 : 10) + dgt;
                j++;
            }
            if (j > st) {
                if (j < n && s[j] == ';') j++;
                cp = (v > 0 && v < 0x110000) ? (int)v : 0xFFFD;
                if (cp >= 0x80 && cp < 0xA0) cp = 0xFFFD;
            }
        } else {
            size_t st = j;
            while (j < n && isalnum((unsigned char)s[j]) && j - st < 10) j++;
            if (j > st) {
                char nm[16]; memcpy(nm, s + st, j - st); nm[j - st] = 0;
                for (int k = 0; ents[k].n; k++) if (!strcmp(ents[k].n, nm)) { cp = ents[k].cp; break; }
                if (cp >= 0) { if (j < n && s[j] == ';') j++; }
            }
        }
        if (cp < 0) { *w++ = s[i++]; continue; }
        w += utf8_put(w, cp);
        i = j;
    }
    *w = 0;
    if (outlen) *outlen = (int)(w - o);
    return o;
}

/* ================= colores ================= */
static const struct { const char *n; uint32_t c; } cnames[] = {
    {"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},{"blue",0x0000ff},
    {"yellow",0xffff00},{"orange",0xffa500},{"purple",0x800080},{"gray",0x808080},{"grey",0x808080},
    {"silver",0xc0c0c0},{"maroon",0x800000},{"navy",0x000080},{"teal",0x008080},{"olive",0x808000},
    {"lime",0x00ff00},{"aqua",0x00ffff},{"cyan",0x00ffff},{"fuchsia",0xff00ff},{"magenta",0xff00ff},
    {"pink",0xffc0cb},{"brown",0xa52a2a},{"gold",0xffd700},{"crimson",0xdc143c},{"darkred",0x8b0000},
    {"darkgreen",0x006400},{"darkblue",0x00008b},{"lightgray",0xd3d3d3},{"lightgrey",0xd3d3d3},
    {"darkgray",0xa9a9a9},{"darkgrey",0xa9a9a9},{"whitesmoke",0xf5f5f5},{"lightblue",0xadd8e6},
    {"lightgreen",0x90ee90},{"indigo",0x4b0082},{"violet",0xee82ee},{"coral",0xff7f50},{"salmon",0xfa8072},
    {"tan",0xd2b48c},{"beige",0xf5f5dc},{"ivory",0xfffff0},{"khaki",0xf0e68c},{"orchid",0xda70d6},
    {"turquoise",0x40e0d0},{"skyblue",0x87ceeb},{"steelblue",0x4682b4},{"royalblue",0x4169e1},
    {"slategray",0x708090},{"slategrey",0x708090},{"dimgray",0x696969},{"dimgrey",0x696969},
    {"midnightblue",0x191970},{"firebrick",0xb22222},{"tomato",0xff6347},{"chocolate",0xd2691e},
    {"darkorange",0xff8c00},{"forestgreen",0x228b22},{"seagreen",0x2e8b57},{"dodgerblue",0x1e90ff},
    {"gainsboro",0xdcdcdc},{"aliceblue",0xf0f8ff},{"lavender",0xe6e6fa},{"snow",0xfffafa},
    {"linen",0xfaf0e6},{"mintcream",0xf5fffa},{"honeydew",0xf0fff0},{"azure",0xf0ffff},
    {0,0}
};

/* devuelve 1 = color, 2 = transparente, 0 = no entendido */
static int parse_color(const char *v, uint32_t *out) {
    while (isspace((unsigned char)*v)) v++;
    if (*v == '#') {
        int n = 0; uint32_t x = 0;
        const char *p = v + 1;
        while (isxdigit((unsigned char)*p)) { x = x * 16 + (isdigit((unsigned char)*p) ? *p - '0' : tolower(*p) - 'a' + 10); p++; n++; }
        if (n == 3 || n == 4) {
            uint32_t r = (x >> (n == 3 ? 8 : 12)) & 15, g = (x >> (n == 3 ? 4 : 8)) & 15, b = (x >> (n == 3 ? 0 : 4)) & 15;
            *out = (r * 17) << 16 | (g * 17) << 8 | (b * 17);
            return 1;
        }
        if (n == 6) { *out = x; return 1; }
        if (n == 8) { *out = x >> 8; return 1; }
        return 0;
    }
    if (!strncasecmp(v, "rgb", 3)) {
        const char *p = strchr(v, '(');
        if (!p) return 0;
        p++;
        float c[3]; int k = 0;
        while (k < 3) {
            while (isspace((unsigned char)*p) || *p == ',') p++;
            char *e; float f = strtof(p, &e);
            if (e == p) return 0;
            p = e;
            if (*p == '%') { f = f * 255 / 100; p++; }
            c[k++] = f < 0 ? 0 : (f > 255 ? 255 : f);
        }
        *out = ((int)(c[0] + 0.5f)) << 16 | ((int)(c[1] + 0.5f)) << 8 | (int)(c[2] + 0.5f);
        return 1;
    }
    char nm[24]; int k = 0;
    while (isalpha((unsigned char)v[k]) && k < 23) { nm[k] = tolower((unsigned char)v[k]); k++; }
    nm[k] = 0;
    if (!k) return 0;
    if (eq(nm, "transparent")) return 2;
    for (int i = 0; cnames[i].n; i++) if (!strcmp(cnames[i].n, nm)) { *out = cnames[i].c; return 1; }
    return 0;
}

/* ================= CSS ================= */
int css_viewport_w = 1100, css_viewport_h = 700;
static __thread Doc *cur_doc;

typedef struct { char name[20]; char val[32]; char op; } AttrSel;
typedef struct {
    char tag[20]; char id[40]; char cls[3][32]; int ncls;
    AttrSel at[2]; int nat;
    uint8_t first, last, only, root, link, empty, has_nth, has_not;
    int nth_a, nth_b;
    char not_tag[20], not_cls[32], not_id[40];
    char comb;                      /* combinador con el compuesto anterior: ' ' '>' '+' '~' */
} Sel;
#define MAXSEL 5
typedef struct { Sel sel[MAXSEL]; int nsel; int spec, order; const char *decls; } Rule;

static int is_ident(char c) { return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80; }

static int read_ident(const char *s, size_t n, size_t *i, char *out, int cap) {
    int k = 0;
    while (*i < n && (is_ident(s[*i]) || s[*i] == '\\')) {
        if (s[*i] == '\\' && *i + 1 < n) (*i)++;
        if (k < cap - 1) out[k++] = s[*i];
        (*i)++;
    }
    out[k] = 0;
    return k;
}

/* parsea un selector compuesto s[0..n); 0 si no se soporta */
static int parse_compound(const char *s, size_t n, Sel *o, int *spec) {
    size_t i = 0;
    if (i < n && s[i] == '*') i++;
    else { char t[20]; if (read_ident(s, n, &i, t, sizeof t)) { for (char *c = t; *c; c++) *c = tolower((unsigned char)*c); strcpy(o->tag, t); *spec += 1; } }
    while (i < n) {
        char kind = s[i++];
        if (kind == '#') {
            if (!read_ident(s, n, &i, o->id, sizeof o->id)) return 0;
            *spec += 100;
        } else if (kind == '.') {
            if (o->ncls >= 3) return 0;
            if (!read_ident(s, n, &i, o->cls[o->ncls], 32)) return 0;
            o->ncls++; *spec += 10;
        } else if (kind == '[') {
            if (o->nat >= 2) return 0;
            AttrSel *a = &o->at[o->nat];
            while (i < n && isspace((unsigned char)s[i])) i++;
            if (!read_ident(s, n, &i, a->name, sizeof a->name)) return 0;
            for (char *c = a->name; *c; c++) *c = tolower((unsigned char)*c);
            while (i < n && isspace((unsigned char)s[i])) i++;
            if (i < n && s[i] == ']') { a->op = 0; i++; }
            else {
                char op = s[i];
                if (strchr("~|^$*", op)) { a->op = op; i++; if (i >= n || s[i] != '=') return 0; i++; }
                else if (op == '=') { a->op = '='; i++; }
                else return 0;
                while (i < n && isspace((unsigned char)s[i])) i++;
                int k = 0;
                if (i < n && (s[i] == '"' || s[i] == '\'')) {
                    char q = s[i++];
                    while (i < n && s[i] != q) { if (k < 31) a->val[k++] = s[i]; i++; }
                    if (i < n) i++;
                } else {
                    while (i < n && s[i] != ']' && !isspace((unsigned char)s[i])) { if (k < 31) a->val[k++] = s[i]; i++; }
                }
                a->val[k] = 0;
                while (i < n && s[i] != ']') i++;      /* flag "i" */
                if (i >= n) return 0;
                i++;
            }
            o->nat++; *spec += 10;
        } else if (kind == ':') {
            if (i < n && s[i] == ':') return 0;            /* pseudo-elementos: no */
            char nm[24];
            read_ident(s, n, &i, nm, sizeof nm);
            for (char *c = nm; *c; c++) *c = tolower((unsigned char)*c);
            char arg[48] = "";
            if (i < n && s[i] == '(') {
                int depth = 1, k = 0; i++;
                while (i < n && depth) {
                    if (s[i] == '(') depth++; else if (s[i] == ')') { depth--; if (!depth) break; }
                    if (k < 47) arg[k++] = s[i];
                    i++;
                }
                arg[k] = 0;
                if (i < n) i++;
            }
            if (eq(nm, "first-child")) o->first = 1;
            else if (eq(nm, "last-child")) o->last = 1;
            else if (eq(nm, "only-child")) o->only = 1;
            else if (eq(nm, "root")) o->root = 1;
            else if (eq(nm, "link") || eq(nm, "any-link")) o->link = 1;
            else if (eq(nm, "empty")) o->empty = 1;
            else if (eq(nm, "nth-child")) {
                char t[48]; int k = 0;
                for (char *c = arg; *c; c++) if (!isspace((unsigned char)*c)) t[k++] = tolower((unsigned char)*c);
                t[k] = 0;
                o->has_nth = 1;
                if (eq(t, "odd")) { o->nth_a = 2; o->nth_b = 1; }
                else if (eq(t, "even")) { o->nth_a = 2; o->nth_b = 0; }
                else if (strchr(t, 'n')) {
                    char *np = strchr(t, 'n');
                    *np = 0;
                    o->nth_a = !t[0] || eq(t, "+") ? 1 : (eq(t, "-") ? -1 : atoi(t));
                    o->nth_b = np[1] ? atoi(np + 1) : 0;
                } else { o->nth_a = 0; o->nth_b = atoi(t); }
            }
            else if (eq(nm, "not")) {
                Sel tmp; memset(&tmp, 0, sizeof tmp); int sp = 0;
                if (!parse_compound(arg, strlen(arg), &tmp, &sp) || tmp.ncls > 1 || tmp.nat || tmp.has_nth || tmp.first || tmp.last || tmp.only) return 0;
                o->has_not = 1;
                strcpy(o->not_tag, tmp.tag); strcpy(o->not_id, tmp.id);
                if (tmp.ncls) strcpy(o->not_cls, tmp.cls[0]);
            }
            else return 0;                                   /* :hover, :focus, :is, :has... se descartan */
            *spec += 10;
        } else return 0;
    }
    return 1;
}

static int parse_selector(const char *s, Rule *r) {
    r->nsel = 0; r->spec = 0;
    const char *p = s;
    char comb = ' ';
    while (*p) {
        int sawsp = 0;
        while (isspace((unsigned char)*p)) { p++; sawsp = 1; }
        if (*p == '>' || *p == '+' || *p == '~') {
            comb = *p++;
            while (isspace((unsigned char)*p)) p++;
        } else if (sawsp && r->nsel) comb = ' ';
        if (!*p) break;
        const char *st = p;
        int depth = 0, br = 0;
        while (*p && (depth || br || (!isspace((unsigned char)*p) && *p != '>' && *p != '+' && *p != '~'))) {
            if (*p == '(') depth++; else if (*p == ')') depth--;
            else if (*p == '[') br = 1; else if (*p == ']') br = 0;
            p++;
        }
        if (r->nsel >= MAXSEL) return 0;
        Sel *c = &r->sel[r->nsel];
        memset(c, 0, sizeof *c);
        if (!parse_compound(st, p - st, c, &r->spec)) return 0;
        c->comb = r->nsel ? comb : 0;
        r->nsel++;
        comb = ' ';
    }
    return r->nsel > 0;
}

static int has_class(const char *cl, const char *want) {
    size_t wl = strlen(want);
    while (*cl) {
        while (isspace((unsigned char)*cl)) cl++;
        const char *st = cl;
        while (*cl && !isspace((unsigned char)*cl)) cl++;
        if ((size_t)(cl - st) == wl && !strncmp(st, want, wl)) return 1;
    }
    return 0;
}

static int attr_match(Node *n, const AttrSel *a) {
    const char *v = node_attr(n, a->name);
    if (!v) return 0;
    size_t vl = strlen(v), al = strlen(a->val);
    switch (a->op) {
    case 0: return 1;
    case '=': return !strcmp(v, a->val);
    case '~': return has_class(v, a->val);
    case '|': return !strncmp(v, a->val, al) && (v[al] == 0 || v[al] == '-');
    case '^': return al && !strncmp(v, a->val, al);
    case '$': return al && vl >= al && !strcmp(v + vl - al, a->val);
    case '*': return al && strstr(v, a->val) != NULL;
    }
    return 0;
}

static Node *prev_elem(Node *n) {
    if (!n->parent) return NULL;
    Node *p = NULL;
    for (Node *c = n->parent->first; c && c != n; c = c->next) if (c->type == 0) p = c;
    return p;
}

static int match_compound(Node *n, const Sel *s) {
    if (n->type != 0) return 0;
    if (s->tag[0] && strcmp(n->name, s->tag)) return 0;
    if (s->id[0]) { const char *v = node_attr(n, "id"); if (!v || strcmp(v, s->id)) return 0; }
    if (s->ncls) {
        const char *cl = node_attr(n, "class");
        if (!cl) return 0;
        for (int i = 0; i < s->ncls; i++) if (!has_class(cl, s->cls[i])) return 0;
    }
    for (int i = 0; i < s->nat; i++) if (!attr_match(n, &s->at[i])) return 0;
    if (s->root && !(n->parent && n->parent->type == 0 && n->parent->name && !n->parent->name[0]) && !eq(n->name, "html")) return 0;
    if (s->link && !(eq(n->name, "a") && node_attr(n, "href"))) return 0;
    if (s->first || s->last || s->only || s->has_nth) {
        int idx = 0, total = 0;
        if (n->parent) for (Node *c = n->parent->first; c; c = c->next) if (c->type == 0) { total++; if (c == n) idx = total; }
        if (s->first && idx != 1) return 0;
        if (s->last && idx != total) return 0;
        if (s->only && total != 1) return 0;
        if (s->has_nth) {
            if (s->nth_a == 0) { if (idx != s->nth_b) return 0; }
            else { int d = idx - s->nth_b; if (d % s->nth_a != 0 || d / s->nth_a < 0) return 0; }
        }
    }
    if (s->empty) {
        for (Node *c = n->first; c; c = c->next) {
            if (c->type == 0) return 0;
            for (int i = 0; i < c->tlen; i++) if (!isspace((unsigned char)c->text[i])) return 0;
        }
    }
    if (s->has_not) {
        int m = 1;
        if (s->not_tag[0] && strcmp(n->name, s->not_tag)) m = 0;
        if (m && s->not_id[0]) { const char *v = node_attr(n, "id"); if (!v || strcmp(v, s->not_id)) m = 0; }
        if (m && s->not_cls[0]) { const char *cl = node_attr(n, "class"); if (!cl || !has_class(cl, s->not_cls)) m = 0; }
        if (m) return 0;
    }
    return 1;
}

static int match_chain(Node *n, const Rule *r, int i) {
    if (!match_compound(n, &r->sel[i])) return 0;
    if (i == 0) return 1;
    switch (r->sel[i].comb) {
    case '>': { Node *p = n->parent; return p && p->type == 0 && match_chain(p, r, i - 1); }
    case '+': { Node *p = prev_elem(n); return p && match_chain(p, r, i - 1); }
    case '~': {
        if (!n->parent) return 0;
        for (Node *c = n->parent->first; c && c != n; c = c->next) if (c->type == 0 && match_chain(c, r, i - 1)) return 1;
        return 0;
    }
    default:
        for (Node *a = n->parent; a; a = a->parent) if (a->type == 0 && match_chain(a, r, i - 1)) return 1;
        return 0;
    }
}
static int match_rule(Node *n, const Rule *r) { return match_chain(n, r, r->nsel - 1); }

/* ---- variables CSS (--x) : tabla por documento ---- */
typedef struct { char name[40]; char *val; } CssVar;
static void collect_vars(Doc *d, const char *decls, int strong) {
    const char *p = decls;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';')) p++;
        if (p[0] == '-' && p[1] == '-') {
            const char *ns = p; while (*p && *p != ':' && *p != ';') p++;
            if (*p != ':') continue;
            char name[40]; int k = (int)(p - ns); if (k > 39) k = 39;
            memcpy(name, ns, k); name[k] = 0;
            while (k && isspace((unsigned char)name[k - 1])) name[--k] = 0;
            p++;
            const char *vs = p; int depth = 0;
            while (*p && (depth || *p != ';')) { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
            while (isspace((unsigned char)*vs)) vs++;
            size_t vl = p - vs;
            while (vl && isspace((unsigned char)vs[vl - 1])) vl--;
            CssVar *vars = d->vars; int found = -1;
            for (int i = 0; i < d->nvars; i++) if (!strcmp(vars[i].name, name)) { found = i; break; }
            if (found >= 0) { if (strong) vars[found].val = arena_strndup(&d->arena, vs, vl); }
            else if (d->nvars < 400) {
                if (d->nvars == d->cvars) { d->cvars = d->cvars ? d->cvars * 2 : 32; d->vars = realloc(d->vars, d->cvars * sizeof(CssVar)); vars = d->vars; }
                strcpy(vars[d->nvars].name, name);
                vars[d->nvars].val = arena_strndup(&d->arena, vs, vl);
                d->nvars++;
            }
        } else {
            int depth = 0;
            while (*p && (depth || *p != ';')) { if (*p == '(') depth++; else if (*p == ')') depth--; p++; }
        }
    }
}

/* sustituye var(--x, fallback) y env(...) en v; devuelve out */
static const char *subst_vars(const char *v, char *out, size_t cap, int depth) {
    size_t w = 0;
    for (const char *p = v; *p && w + 1 < cap;) {
        int isvar = !strncmp(p, "var(", 4), isenv = !strncmp(p, "env(", 4);
        if (isvar || isenv) {
            const char *q = p + 4; int d2 = 1;
            const char *argend = q;
            while (*argend && d2) { if (*argend == '(') d2++; else if (*argend == ')') d2--; if (d2) argend++; }
            char arg[300]; size_t al = argend - q; if (al > 299) al = 299;
            memcpy(arg, q, al); arg[al] = 0;
            char *comma = NULL; int dd = 0;
            for (char *c = arg; *c; c++) { if (*c == '(') dd++; else if (*c == ')') dd--; else if (*c == ',' && !dd) { comma = c; break; } }
            const char *rep = NULL;
            char nm[40]; int k = 0;
            const char *a0 = arg; while (isspace((unsigned char)*a0)) a0++;
            while (a0 != comma && *a0 && !isspace((unsigned char)*a0) && k < 39) nm[k++] = *a0++;
            nm[k] = 0;
            if (isvar && cur_doc) { CssVar *vars = cur_doc->vars; for (int i = 0; i < cur_doc->nvars; i++) if (!strcmp(vars[i].name, nm)) { rep = vars[i].val; break; } }
            if (!rep && comma) { rep = comma + 1; while (isspace((unsigned char)*rep)) rep++; }
            if (!rep) rep = isenv ? "0px" : "";
            char sub[600];
            if (depth < 6 && (strstr(rep, "var(") || strstr(rep, "env("))) { subst_vars(rep, sub, sizeof sub, depth + 1); rep = sub; }
            for (const char *r = rep; *r && w + 1 < cap; r++) out[w++] = *r;
            p = *argend ? argend + 1 : argend;
        } else out[w++] = *p++;
    }
    out[w] = 0;
    return out;
}

/* ---- @media ---- */
static int parse_len(const char *v, float embase, float *out, int *kind);
static int media_feature(const char *f) {
    char name[32], val[48] = "";
    const char *c = strchr(f, ':');
    int nl = c ? (int)(c - f) : (int)strlen(f);
    if (nl > 31) nl = 31;
    int k = 0;
    for (int i = 0; i < nl; i++) if (!isspace((unsigned char)f[i])) name[k++] = tolower((unsigned char)f[i]);
    name[k] = 0;
    if (c) { c++; while (isspace((unsigned char)*c)) c++; snprintf(val, sizeof val, "%s", c); size_t l = strlen(val); while (l && isspace((unsigned char)val[l - 1])) val[--l] = 0; }
    float x; int kd;
    if (eq(name, "min-width") || eq(name, "max-width") || eq(name, "min-height") || eq(name, "max-height")) {
        if (!parse_len(val, 16, &x, &kd) || kd != 1) return 1;
        int vw = name[4] == 'w' ? css_viewport_w : css_viewport_h;
        return name[1] == 'i' ? vw >= x : vw <= x;
    }
    if (eq(name, "prefers-color-scheme")) return !strcasecmp(val, "light");
    if (eq(name, "prefers-reduced-motion")) return !strcasecmp(val, "no-preference");
    if (eq(name, "orientation")) return !strcasecmp(val, css_viewport_w >= css_viewport_h ? "landscape" : "portrait");
    if (eq(name, "hover") || eq(name, "any-hover")) return !strcasecmp(val, "hover");
    if (eq(name, "pointer") || eq(name, "any-pointer")) return !strcasecmp(val, "fine");
    if (eq(name, "prefers-contrast")) return !strcasecmp(val, "no-preference");
    if (eq(name, "forced-colors")) return !strcasecmp(val, "none");
    if (eq(name, "print") || eq(name, "speech")) return 0;
    return 1;
}
static int media_query(const char *q) {
    char buf[400]; snprintf(buf, sizeof buf, "%s", q);
    int neg = 0, result = 1;
    char *p = buf;
    while (isspace((unsigned char)*p)) p++;
    if (!strncasecmp(p, "not ", 4)) { neg = 1; p += 4; }
    else if (!strncasecmp(p, "only ", 5)) p += 5;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '(') {
            int depth = 1; char *st = ++p;
            while (*p && depth) { if (*p == '(') depth++; else if (*p == ')') depth--; if (depth) p++; }
            char save = *p; *p = 0;
            if (!media_feature(st)) result = 0;
            *p = save; if (*p) p++;
        } else {
            char *st = p;
            while (*p && !isspace((unsigned char)*p) && *p != '(') p++;
            char save = *p; *p = 0;
            if (!strcasecmp(st, "print") || !strcasecmp(st, "speech")) result = 0;
            *p = save;
        }
    }
    return neg ? !result : result;
}
static int media_matches(const char *prelude) {
    char buf[400]; snprintf(buf, sizeof buf, "%s", prelude);
    char *save = NULL;
    for (char *q = strtok_r(buf, ",", &save); q; q = strtok_r(NULL, ",", &save)) if (media_query(q)) return 1;
    return 0;
}

static void css_block(Doc *d, const char *buf, size_t n, Rule **rulesp, int *nrp, int *capp, int depthlim) {
    size_t i = 0;
    Rule *rules = *rulesp; int nr = *nrp, cap = *capp;
    while (i < n) {
        while (i < n && (isspace((unsigned char)buf[i]) || buf[i] == '}')) i++;
        if (i >= n) break;
        if (buf[i] == '@') {
            size_t as = i;
            while (i < n && buf[i] != '{' && buf[i] != ';') i++;
            if (i >= n) break;
            if (buf[i] == ';') { i++; continue; }
            size_t pe = i;
            size_t bs = ++i; int depth = 1;
            while (i < n && depth) { if (buf[i] == '{') depth++; else if (buf[i] == '}') depth--; i++; }
            size_t be = depth ? n : i - 1;
            char pre[400]; size_t pl = pe - as; if (pl > 399) pl = 399;
            memcpy(pre, buf + as, pl); pre[pl] = 0;
            int ok = 0;
            if (!strncasecmp(pre, "@media", 6)) ok = media_matches(pre + 6);
            else if (!strncasecmp(pre, "@supports", 9)) ok = !strstr(pre, "not");
            else if (!strncasecmp(pre, "@layer", 6)) ok = 1;
            if (ok && depthlim < 4) {
                *rulesp = rules; *nrp = nr; *capp = cap;
                css_block(d, buf + bs, be - bs, rulesp, nrp, capp, depthlim + 1);
                rules = *rulesp; nr = *nrp; cap = *capp;
            }
            continue;
        }
        size_t ss = i;
        while (i < n && buf[i] != '{') i++;
        if (i >= n) break;
        size_t se = i;
        i++;
        size_t ds = i;
        int depth = 1;
        while (i < n && depth) { if (buf[i] == '{') depth++; else if (buf[i] == '}') depth--; i++; }
        size_t de = depth ? n : i - 1;
        char *sel = arena_strndup(&d->arena, buf + ss, se - ss);
        const char *decls = arena_strndup(&d->arena, buf + ds, de - ds);
        char *save = NULL, *selcopy = strdup(sel);
        for (char *part = strtok_r(sel, ",", &save); part; part = strtok_r(NULL, ",", &save)) {
            if (nr >= 5000) break;
            if (nr + 1 >= cap) { cap *= 2; rules = realloc(rules, cap * sizeof(Rule)); }
            Rule *r = &rules[nr];
            if (!parse_selector(part, r)) {                /* aun así, :root.clase puede traer variables */
                continue;
            }
            r->decls = decls;
            r->order = nr;
            {
                Sel *last = &r->sel[r->nsel - 1];
                Node *html = NULL;
                for (Node *c = d->root->first; c; c = c->next) if (c->type == 0 && !strcmp(c->name, "html")) { html = c; break; }
                int rootish = last->root || !strcmp(last->tag, "html") || !strcmp(last->tag, "body") || (!last->tag[0] && !last->id[0] && !last->ncls && !last->nat && !last->first && !last->has_nth);
                if (rootish) { if (html && (match_rule(html, r) || (r->nsel == 1 && !strcmp(last->tag, "body")))) collect_vars(d, decls, 1); }
                else collect_vars(d, decls, 0);
            }
            nr++;
        }
        free(selcopy);
    }
    *rulesp = rules; *nrp = nr; *capp = cap;
}

static void css_parse(Doc *d, const char *css, size_t len) {
    char *buf = malloc(len + 1), *w = buf;
    for (size_t i = 0; i < len;) {                       /* quitar comentarios */
        if (css[i] == '/' && i + 1 < len && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) i++;
            i += 2;
        } else *w++ = css[i++];
    }
    *w = 0;
    Rule *rules = d->rules; int nr = d->nrules, cap = nr + 64;
    rules = realloc(rules, cap * sizeof(Rule));
    css_block(d, buf, w - buf, &rules, &nr, &cap, 0);
    d->rules = rules; d->nrules = nr;
    free(buf);
}

/* --------- longitudes --------- */
/* kind: 0 número, 1 px, 2 %, 3 auto */
typedef struct { const char *p; float em; int err; } Mx;
typedef struct { float v; int k; } Mv;
static Mv mx_expr(Mx *m);
static void mx_ws(Mx *m) { while (isspace((unsigned char)*m->p)) m->p++; }
static Mv mx_fail(Mx *m) { m->err = 1; Mv r = {0, 1}; return r; }
static Mv mx_atom(Mx *m) {
    mx_ws(m);
    const char *p = m->p;
    if (*p == '(') { m->p++; Mv r = mx_expr(m); mx_ws(m); if (*m->p == ')') m->p++; else m->err = 1; return r; }
    int isfn = !strncasecmp(p, "calc(", 5) || !strncasecmp(p, "min(", 4) || !strncasecmp(p, "max(", 4) || !strncasecmp(p, "clamp(", 6);
    if (isfn) {
        char fn = tolower((unsigned char)p[1]);           /* a=calc, i=min, a=max... */
        int kind = !strncasecmp(p, "calc(", 5) ? 0 : !strncasecmp(p, "min(", 4) ? 1 : !strncasecmp(p, "max(", 4) ? 2 : 3;
        (void)fn;
        m->p = strchr(p, '(') + 1;
        Mv args[3]; int na = 0;
        for (;;) {
            Mv a = mx_expr(m);
            if (na < 3) args[na++] = a;
            mx_ws(m);
            if (*m->p == ',') { m->p++; continue; }
            if (*m->p == ')') { m->p++; break; }
            m->err = 1; return a;
        }
        if (m->err) return args[0];
        if (kind == 0) return args[0];
        for (int i = 1; i < na; i++) if (args[i].k != args[0].k) return mx_fail(m);
        Mv r = args[0];
        if (kind == 1) { for (int i = 1; i < na; i++) if (args[i].v < r.v) r.v = args[i].v; }
        else if (kind == 2) { for (int i = 1; i < na; i++) if (args[i].v > r.v) r.v = args[i].v; }
        else if (na == 3) { r.v = args[1].v < args[0].v ? args[0].v : args[1].v; if (r.v > args[2].v) r.v = args[2].v; r.v = args[1].v < args[0].v ? args[0].v : (args[1].v > args[2].v ? args[2].v : args[1].v); }
        return r;
    }
    char *e;
    float f = strtof(p, &e);
    if (e == p) return mx_fail(m);
    Mv r = {f, 0};
    if (!strncasecmp(e, "px", 2)) { r.k = 1; e += 2; }
    else if (!strncasecmp(e, "rem", 3)) { r.v = f * 16; r.k = 1; e += 3; }
    else if (!strncasecmp(e, "em", 2)) { r.v = f * m->em; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "ex", 2) || !strncasecmp(e, "ch", 2)) { r.v = f * m->em * 0.5f; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "pt", 2)) { r.v = f * 4.0f / 3.0f; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "pc", 2)) { r.v = f * 16; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "cm", 2)) { r.v = f * 37.8f; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "mm", 2)) { r.v = f * 3.78f; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "in", 2)) { r.v = f * 96; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "vmin", 4)) { r.v = f * (css_viewport_w < css_viewport_h ? css_viewport_w : css_viewport_h) / 100; r.k = 1; e += 4; }
    else if (!strncasecmp(e, "vmax", 4)) { r.v = f * (css_viewport_w > css_viewport_h ? css_viewport_w : css_viewport_h) / 100; r.k = 1; e += 4; }
    else if (!strncasecmp(e, "vw", 2)) { r.v = f * css_viewport_w / 100; r.k = 1; e += 2; }
    else if (!strncasecmp(e, "vh", 2)) { r.v = f * css_viewport_h / 100; r.k = 1; e += 2; }
    else if (*e == '%') { r.k = 2; e++; }
    else if (isalpha((unsigned char)*e)) return mx_fail(m);
    m->p = e;
    return r;
}
static Mv mx_term(Mx *m) {
    Mv a = mx_atom(m);
    for (;;) {
        mx_ws(m);
        if (*m->p == '*' || *m->p == '/') {
            char op = *m->p++;
            Mv b = mx_atom(m);
            if (op == '*') { if (a.k && b.k) return mx_fail(m); a.v *= b.v; if (!a.k) a.k = b.k; }
            else { if (b.k || b.v == 0) return mx_fail(m); a.v /= b.v; }
        } else return a;
    }
}
static Mv mx_expr(Mx *m) {
    Mv a = mx_term(m);
    for (;;) {
        mx_ws(m);
        if ((*m->p == '+' || *m->p == '-') && isspace((unsigned char)m->p[1])) {
            char op = *m->p++;
            Mv b = mx_term(m);
            if (a.k != b.k) return mx_fail(m);
            a.v += op == '+' ? b.v : -b.v;
        } else return a;
    }
}

static int parse_len(const char *v, float embase, float *out, int *kind) {
    while (isspace((unsigned char)*v)) v++;
    if (!strncasecmp(v, "auto", 4)) { *kind = 3; *out = 0; return 1; }
    Mx m = { v, embase, 0 };
    Mv r = mx_expr(&m);
    if (m.err) return 0;
    mx_ws(&m);
    if (*m.p && *m.p != '!') { /* texto sobrante (p. ej. otra palabra): solo vale si no es parte de la longitud */ if (!isspace((unsigned char)m.p[-1]) && *m.p != ' ') return 0; }
    *out = r.v; *kind = r.k == 0 ? 1 : r.k;
    return 1;
}

static int split_tokens(const char *v, char tok[][48], int max) {
    int n = 0;
    while (*v && n < max) {
        while (isspace((unsigned char)*v)) v++;
        if (!*v) break;
        int k = 0, depth = 0;
        while (*v && (depth || !isspace((unsigned char)*v))) {
            if (*v == '(') depth++;
            if (*v == ')') depth--;
            if (k < 47) tok[n][k++] = *v;
            v++;
        }
        tok[n][k] = 0;
        n++;
    }
    return n;
}

static void set_box4(float *t, float *r, float *b, float *l, const char *v, float em, uint8_t *lauto, uint8_t *rauto, int allow_auto) {
    char tok[4][48];
    int n = split_tokens(v, tok, 4);
    if (!n) return;
    float val[4]; int kind[4], ok = 1;
    for (int i = 0; i < n; i++) {
        if (!parse_len(tok[i], em, &val[i], &kind[i]) || kind[i] == 2 || (kind[i] == 3 && !allow_auto)) ok = 0;
    }
    if (!ok) return;
    int ti, ri, bi, li;
    if (n == 1) ti = ri = bi = li = 0;
    else if (n == 2) { ti = bi = 0; ri = li = 1; }
    else if (n == 3) { ti = 0; ri = li = 1; bi = 2; }
    else { ti = 0; ri = 1; bi = 2; li = 3; }
    *t = val[ti]; *r = val[ri]; *b = val[bi]; *l = val[li];
    if (lauto) *lauto = (kind[li] == 3);
    if (rauto) *rauto = (kind[ri] == 3);
}

static const char *trim_val(char *v) {
    while (isspace((unsigned char)*v)) v++;
    size_t l = strlen(v);
    while (l && isspace((unsigned char)v[l - 1])) v[--l] = 0;
    char *imp = strstr(v, "!important");
    if (imp) { *imp = 0; l = strlen(v); while (l && isspace((unsigned char)v[l - 1])) v[--l] = 0; }
    return v;
}


static int ci_eq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

/* un lado de borde: "1px solid #ccc" */
static void parse_border(const char *val, float *w, uint32_t *c, int *cset, float em) {
    char tok[6][48];
    int n = split_tokens(val, tok, 6);
    float bw = -1; int none = 0; uint32_t col; *cset = 0;
    for (int i = 0; i < n; i++) {
        float f; int kind; uint32_t cc;
        if (ci_eq(tok[i], "none") || ci_eq(tok[i], "hidden")) none = 1;
        else if (ci_eq(tok[i], "thin")) bw = 1;
        else if (ci_eq(tok[i], "medium")) bw = 3;
        else if (ci_eq(tok[i], "thick")) bw = 5;
        else if (strchr("sdoigrx", tolower((unsigned char)tok[i][0])) && isalpha((unsigned char)tok[i][1]) && !strstr(tok[i], "(") && parse_color(tok[i], &cc) == 0) { /* solid/dashed/dotted/double... */ }
        else if (parse_len(tok[i], em, &f, &kind) && kind == 1) bw = f;
        else if (parse_color(tok[i], &cc) == 1) { col = cc; *c = cc; *cset = 1; }
        else if (parse_color(tok[i], &cc) == 2) { *cset = 0; none = 0; }
    }
    if (none) *w = 0;
    else if (bw >= 0) *w = bw < 1 && bw > 0 ? 1 : bw;
    else *w = 1;
    (void)col;
}

static int parse_side_name(const char *p) {     /* "border-top-width" -> 0.. */
    if (!strncmp(p, "top", 3)) return 0;
    if (!strncmp(p, "right", 5)) return 1;
    if (!strncmp(p, "bottom", 6)) return 2;
    if (!strncmp(p, "left", 4)) return 3;
    return -1;
}

static void to_lower_str(char *s2) { for (; *s2; s2++) *s2 = tolower((unsigned char)*s2); }

static void apply_decl(Style *s, const char *prop, const char *val, float psize);
static void apply_extra(Style *s, const char *prop, const char *val, float psize) {
    float f; int kind; uint32_t col;
    if (eq(prop, "line-height")) {
        if (ci_eq(val, "normal")) s->lh = -1;
        else if (parse_len(val, s->size, &f, &kind)) {
            char *e; strtof(val, &e);
            if (!*e || isspace((unsigned char)*e)) s->lh = f;                 /* sin unidad: factor */
            else if (kind == 1) s->lh = f / s->size;
            else if (kind == 2) s->lh = f / 100;
        }
    }
    else if (eq(prop, "text-transform")) {
        s->ttrans = ci_eq(val, "uppercase") ? 1 : ci_eq(val, "lowercase") ? 2 : ci_eq(val, "capitalize") ? 3 : 0;
    }
    else if (eq(prop, "min-height")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->minh = f; }
    else if (eq(prop, "max-width") && ci_eq(val, "none")) { s->maxw = s->maxwpct = -1; }
    else if (eq(prop, "box-sizing")) s->bsizing = ci_eq(val, "border-box");
    else if (eq(prop, "opacity")) { if (atof(val) == 0 && (val[0] == '0')) s->display = D_NONE; }
    else if (eq(prop, "border-radius")) {
        char tok[4][48]; int n = split_tokens(val, tok, 4);
        if (n) { if (strchr(tok[0], '%')) s->radius = 9999; else if (parse_len(tok[0], s->size, &f, &kind) && kind == 1) s->radius = f; }
    }
    else if (eq(prop, "border")) {
        for (int i = 0; i < 4; i++) { int cs; uint32_t c2 = s->bc[i]; parse_border(val, &s->bw[i], &c2, &cs, s->size); if (cs) { s->bc[i] = c2; s->bcmask |= 1 << i; } else s->bcmask &= ~(1 << i); }
    }
    else if (!strncmp(prop, "border-", 7) && parse_side_name(prop + 7) >= 0 && (prop[7 + (parse_side_name(prop + 7) == 0 ? 3 : parse_side_name(prop + 7) == 1 ? 5 : parse_side_name(prop + 7) == 2 ? 6 : 4)] == 0)) {
        int i = parse_side_name(prop + 7), cs; uint32_t c2 = s->bc[i];
        parse_border(val, &s->bw[i], &c2, &cs, s->size);
        if (cs) { s->bc[i] = c2; s->bcmask |= 1 << i; }
    }
    else if (!strncmp(prop, "border-", 7) && strstr(prop, "-width") && !strstr(prop, "radius")) {
        int i = parse_side_name(prop + 7);
        if (i >= 0) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->bw[i] = f; }
        else { char tok[4][48]; int n = split_tokens(val, tok, 4);
            if (n) { float w4[4]; int ok = 1; for (int k = 0; k < n; k++) if (!parse_len(tok[k], s->size, &w4[k], &kind) || kind != 1) ok = 0;
                if (ok) { int map[4][4] = {{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}}; for (int k = 0; k < 4; k++) s->bw[k] = w4[map[n - 1][k]]; } } }
    }
    else if (!strncmp(prop, "border-", 7) && strstr(prop, "-color") && !strstr(prop, "radius")) {
        int i = parse_side_name(prop + 7);
        if (i >= 0) { if (parse_color(val, &col) == 1) { s->bc[i] = col; s->bcmask |= 1 << i; } }
        else { char tok[4][48]; int n = split_tokens(val, tok, 4);
            if (n) { uint32_t c4[4]; int ok = 1; for (int k = 0; k < n; k++) if (parse_color(tok[k], &c4[k]) != 1) ok = 0;
                if (ok) { int map[4][4] = {{0,0,0,0},{0,1,0,1},{0,1,2,1},{0,1,2,3}}; for (int k = 0; k < 4; k++) { s->bc[k] = c4[map[n - 1][k]]; s->bcmask |= 1 << k; } } } }
    }
    else if (!strncmp(prop, "border-", 7) && strstr(prop, "-style") && !strstr(prop, "radius")) {
        if (ci_eq(val, "none") || ci_eq(val, "hidden")) { int i = parse_side_name(prop + 7); if (i >= 0) s->bw[i] = 0; else for (int k = 0; k < 4; k++) s->bw[k] = 0; }
    }
    else if (eq(prop, "gap") || eq(prop, "grid-gap")) {
        char tok[2][48]; int n = split_tokens(val, tok, 2);
        if (n && parse_len(tok[0], s->size, &f, &kind) && kind == 1) { s->rgap = f; s->gap = n > 1 && parse_len(tok[1], s->size, &f, &kind) && kind == 1 ? f : s->rgap; }
    }
    else if (eq(prop, "column-gap") || eq(prop, "grid-column-gap")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->gap = f; }
    else if (eq(prop, "row-gap") || eq(prop, "grid-row-gap")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->rgap = f; }
    else if (eq(prop, "flex-direction")) s->fdir = (ci_eq(val, "column") || ci_eq(val, "column-reverse"));
    else if (eq(prop, "flex-flow")) { s->fdir = strcasestr(val, "column") != NULL; s->fwrap = strcasestr(val, "wrap") && !strcasestr(val, "nowrap"); }
    else if (eq(prop, "flex-wrap")) s->fwrap = (ci_eq(val, "wrap") || ci_eq(val, "wrap-reverse"));
    else if (eq(prop, "justify-content")) {
        s->just = ci_eq(val, "center") ? 1 : (ci_eq(val, "flex-end") || ci_eq(val, "end") || ci_eq(val, "right")) ? 2 : ci_eq(val, "space-between") ? 3 :
                  (ci_eq(val, "space-around") || ci_eq(val, "space-evenly")) ? 4 : 0;
    }
    else if (eq(prop, "align-items")) {
        s->alignit = ci_eq(val, "center") ? 1 : (ci_eq(val, "flex-end") || ci_eq(val, "end")) ? 2 : (ci_eq(val, "flex-start") || ci_eq(val, "start")) ? 3 : 0;
    }
    else if (eq(prop, "flex-grow")) s->grow = (float)atof(val);
    else if (eq(prop, "flex-basis")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->basis = f; }
    else if (eq(prop, "flex")) {
        if (ci_eq(val, "none")) s->grow = 0;
        else if (ci_eq(val, "auto")) s->grow = 1;
        else { char tok[3][48]; int n = split_tokens(val, tok, 3);
            if (n >= 1 && (isdigit((unsigned char)tok[0][0]) || tok[0][0] == '.')) {
                s->grow = (float)atof(tok[0]);
                s->basis = 0;
                if (n >= 3) { if (ci_eq(tok[2], "auto")) s->basis = -1; else if (parse_len(tok[2], s->size, &f, &kind) && kind == 1) s->basis = f; }
            } }
    }
    else if (eq(prop, "mask-image") || eq(prop, "-webkit-mask-image") || eq(prop, "mask")) { if (strstr(val, "url(") || strstr(val, "gradient")) s->masked = 1; }
    else if (eq(prop, "clip")) { if (strcasestr(val, "rect(0") || strcasestr(val, "rect(1px, 1px")) s->display = D_NONE; }
    else if (eq(prop, "clip-path")) { if (strcasestr(val, "inset(50%")) s->display = D_NONE; }
    else if (eq(prop, "float")) s->flt = ci_eq(val, "left") ? 1 : ci_eq(val, "right") ? 2 : 0;
    else if (eq(prop, "clear")) s->clearf = (ci_eq(val, "both") || ci_eq(val, "left") || ci_eq(val, "right"));
    else if (eq(prop, "grid-template-columns")) {
        int cols = 0; float mn = 0;
        const char *r = strcasestr(val, "repeat(");
        if (r) {
            r += 7;
            if (!strncasecmp(r, "auto-fit", 8) || !strncasecmp(r, "auto-fill", 9)) {
                const char *m = strcasestr(r, "minmax(");
                float v2; int k2;
                if (m && parse_len(m + 7, s->size, &v2, &k2) && k2 == 1) mn = v2;
                cols = 0; s->gridmin = mn > 0 ? mn : 200;
            } else cols = atoi(r);
        } else {
            char tok[16][48]; int n = split_tokens(val, tok, 16);
            cols = n;
        }
        s->gridcols = cols > 12 ? 12 : (uint8_t)cols;
    }
    else if (eq(prop, "font")) {
        char tok[8][48]; int n = split_tokens(val, tok, 8);
        for (int i = 0; i < n; i++) {
            char *sl = strchr(tok[i], '/'); if (sl) *sl = 0;
            if (ci_eq(tok[i], "italic") || ci_eq(tok[i], "oblique")) s->italic = 1;
            else if (ci_eq(tok[i], "bold") || atoi(tok[i]) >= 600) { if (isdigit((unsigned char)tok[i][0]) && strchr(tok[i], 'p')) { apply_decl(s, "font-size", tok[i], s->size); break; } s->bold = 1; }
            else if (isdigit((unsigned char)tok[i][0])) { apply_decl(s, "font-size", tok[i], s->size); if (sl) { char lhv[48]; snprintf(lhv, sizeof lhv, "%s", sl + 1); apply_extra(s, "line-height", lhv, s->size); } break; }
        }
        if (strcasestr(val, "monospace") || strcasestr(val, "courier")) s->mono = 1;
    }
}

static void apply_decl(Style *s, const char *prop, const char *val, float psize) {
    float f; int kind;
    uint32_t col;
    apply_extra(s, prop, val, psize);
    if (prop[0] == '-' && prop[1] == '-') return;
    if (eq(prop, "color")) { if (parse_color(val, &col) == 1) s->color = col; }
    else if (eq(prop, "background-color") || eq(prop, "background")) {
        char tok[8][48];
        int n = split_tokens(val, tok, 8);
        for (int i = 0; i < n; i++) {
            if (!strncasecmp(tok[i], "url(", 4) || strstr(tok[i], "gradient")) continue;
            int r = parse_color(tok[i], &col);
            if (r == 1) { s->bg = col; s->has_bg = 1; break; }
            if (r == 2) { s->has_bg = 0; break; }
        }
    }
    else if (eq(prop, "font-size")) {
        static const struct { const char *n; float v; } kw[] = {
            {"xx-small",9},{"x-small",10},{"small",13},{"medium",16},{"large",18},{"x-large",24},{"xx-large",32},{0,0}};
        int done = 0;
        for (int i = 0; kw[i].n; i++) if (!strcasecmp(val, kw[i].n)) { s->size = kw[i].v; done = 1; }
        if (!done && !strcasecmp(val, "smaller")) { s->size = psize * 0.83f; done = 1; }
        if (!done && !strcasecmp(val, "larger")) { s->size = psize * 1.2f; done = 1; }
        if (!done && parse_len(val, psize, &f, &kind)) {
            if (kind == 1) s->size = f;
            else if (kind == 2) s->size = psize * f / 100;
        }
        if (s->size < 1) s->size = 1;
    }
    else if (eq(prop, "font-weight")) {
        if (!strcasecmp(val, "bold") || !strcasecmp(val, "bolder") || atoi(val) >= 600) s->bold = 1;
        else if (!strcasecmp(val, "normal") || !strcasecmp(val, "lighter") || (atoi(val) > 0 && atoi(val) < 600)) s->bold = 0;
    }
    else if (eq(prop, "font-style")) { s->italic = (!strncasecmp(val, "italic", 6) || !strncasecmp(val, "oblique", 7)); }
    else if (eq(prop, "font-family")) {
        if (strcasestr(val, "monospace") || strcasestr(val, "courier") || strcasestr(val, "consolas") || strcasestr(val, "menlo")) s->mono = 1;
        else s->mono = 0;
    }
    else if (eq(prop, "text-align")) {
        if (!strcasecmp(val, "center")) s->align = 1;
        else if (!strcasecmp(val, "right") || !strcasecmp(val, "end")) s->align = 2;
        else if (!strcasecmp(val, "left") || !strcasecmp(val, "start")) s->align = 0;
    }
    else if (eq(prop, "text-decoration") || eq(prop, "text-decoration-line")) {
        s->ul = strcasestr(val, "underline") != NULL;
        s->strike = strcasestr(val, "line-through") != NULL;
    }
    else if (eq(prop, "display")) {
        if (!strcasecmp(val, "none")) s->display = D_NONE;
        else if (!strcasecmp(val, "inline-flex") || !strcasecmp(val, "inline-grid")) { if (s->display != D_TABLE) { s->display = D_BLOCK; s->isflex = (val[7] == 'f'); s->isgrid = (val[7] == 'g'); } }
        else if (!strcasecmp(val, "inline") || !strcasecmp(val, "inline-block")) { if (s->display != D_TABLE) s->display = D_INLINE; s->isflex = s->isgrid = 0; }
        else if (!strcasecmp(val, "list-item")) s->display = D_LISTITEM;
        else if (!strcasecmp(val, "flex") || !strcasecmp(val, "grid")) { if (s->display != D_TABLE) { s->display = D_BLOCK; s->isflex = (val[0] == 'f'); s->isgrid = (val[0] == 'g'); } }
        else if (!strcasecmp(val, "block") || !strcasecmp(val, "table-cell") || !strcasecmp(val, "table-row") || !strcasecmp(val, "flow-root")) { if (s->display != D_TABLE) { s->display = D_BLOCK; s->isflex = s->isgrid = 0; } }
    }
    else if (eq(prop, "visibility")) { if (!strcasecmp(val, "hidden") || !strcasecmp(val, "collapse")) s->display = D_NONE; }
    else if (eq(prop, "white-space")) {
        s->pre = (!strcasecmp(val, "pre") || !strcasecmp(val, "pre-wrap") || !strcasecmp(val, "pre-line") || !strcasecmp(val, "break-spaces"));
        s->nowrap = (!strcasecmp(val, "nowrap") || !strcasecmp(val, "pre"));
    }
    else if (eq(prop, "list-style") || eq(prop, "list-style-type")) { s->list_none = (strcasestr(val, "none") != NULL); }
    else if (eq(prop, "margin")) set_box4(&s->mt, &s->mr, &s->mb, &s->ml, val, s->size, &s->ml_auto, &s->mr_auto, 1);
    else if (eq(prop, "padding")) set_box4(&s->pt, &s->pr, &s->pb, &s->pl, val, s->size, NULL, NULL, 0);
    else if (eq(prop, "margin-top")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->mt = f; }
    else if (eq(prop, "margin-bottom")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->mb = f; }
    else if (eq(prop, "margin-left")) { if (parse_len(val, s->size, &f, &kind)) { if (kind == 3) { s->ml_auto = 1; s->ml = 0; } else if (kind == 1) { s->ml = f; s->ml_auto = 0; } } }
    else if (eq(prop, "margin-right")) { if (parse_len(val, s->size, &f, &kind)) { if (kind == 3) { s->mr_auto = 1; s->mr = 0; } else if (kind == 1) { s->mr = f; s->mr_auto = 0; } } }
    else if (eq(prop, "padding-top")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->pt = f; }
    else if (eq(prop, "padding-bottom")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->pb = f; }
    else if (eq(prop, "padding-left")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->pl = f; }
    else if (eq(prop, "padding-right")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->pr = f; }
    else if (eq(prop, "width")) {
        if (parse_len(val, s->size, &f, &kind)) { if (kind == 1) { s->width = f; s->wpct = -1; } else if (kind == 2) { s->wpct = f; s->width = -1; } else { s->width = s->wpct = -1; } }
    }
    else if (eq(prop, "max-width")) {
        if (parse_len(val, s->size, &f, &kind)) { if (kind == 1) { s->maxw = f; s->maxwpct = -1; } else if (kind == 2) { s->maxwpct = f; s->maxw = -1; } }
    }
    else if (eq(prop, "height")) { if (parse_len(val, s->size, &f, &kind) && kind == 1) s->height = f; }
}

static void apply_decls(Style *s, const char *decls, float psize, int imode) {
    size_t n = strlen(decls);
    char *buf = malloc(n + 1);
    memcpy(buf, decls, n + 1);
    char *p = buf;
    while (*p) {
        char *start = p; int depth = 0; char q = 0;
        while (*p && !(*p == ';' && !depth && !q)) {
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        char *next = *p ? p + 1 : p;
        *p = 0;
        char *colon = strchr(start, ':');
        if (colon) {
            *colon = 0;
            char *prop = start;
            while (isspace((unsigned char)*prop)) prop++;
            size_t pl = strlen(prop);
            while (pl && isspace((unsigned char)prop[pl - 1])) prop[--pl] = 0;
            for (char *c = prop; *c; c++) *c = tolower((unsigned char)*c);
            int imp = strstr(colon + 1, "!important") != NULL;
            const char *val = trim_val(colon + 1);
            char rb[900];
            if (strstr(val, "var(") || strstr(val, "env(")) val = subst_vars(val, rb, sizeof rb, 0);
            if (imp == imode) apply_decl(s, prop, val, psize);
        }
        p = next;
    }
    free(buf);
}

/* ================= estilos por etiqueta ================= */
static const char *const NONE_TAGS[] = {"head","script","style","title","meta","link","base","template","svg","iframe","canvas",
    "object","embed","audio","video","source","track","datalist","noframes","area","map","param","option","optgroup","picture", 0};
static const char *const BLOCK_TAGS[] = {"html","body","div","p","section","article","header","footer","nav","aside","main","figure",
    "figcaption","address","center","form","fieldset","legend","details","summary","dl","dt","dd","ul","ol","blockquote","pre",
    "h1","h2","h3","h4","h5","h6","hr","caption","thead","tbody","tfoot","tr","td","th","menu","dir","noscript","hgroup","search","xmp", 0};

static float attr_size(const char *v) {
    static const float t[] = {10, 13, 16, 18, 24, 32, 48};
    int k = atoi(v);
    if (*v == '+') k = 3 + k; else if (*v == '-') k = 3 + k;
    if (k < 1) k = 1;
    if (k > 7) k = 7;
    return t[k - 1];
}

static void legacy_attrs(Node *n, Style *s) {
    const char *v;
    const char *nm = n->name;
    if ((v = node_attr(n, "align")) && (eq(nm, "img") || eq(nm, "table"))) {
        if (!strcasecmp(v, "left")) { s->flt = 1; v = NULL; }
        else if (!strcasecmp(v, "right")) { s->flt = 2; v = NULL; }
    }
    if ((v = node_attr(n, "align")) && !(s->flt && (eq(nm, "img") || eq(nm, "table")))) {
        if (!strcasecmp(v, "center") || !strcasecmp(v, "middle")) s->align = 1;
        else if (!strcasecmp(v, "right")) s->align = 2;
        else if (!strcasecmp(v, "left")) s->align = 0;
    }
    uint32_t c;
    if ((v = node_attr(n, "bgcolor")) && parse_color(v, &c) == 1) { s->bg = c; s->has_bg = 1; }
    if (eq(nm, "body")) {
        if ((v = node_attr(n, "text")) && parse_color(v, &c) == 1) s->color = c;
    }
    if (eq(nm, "font")) {
        if ((v = node_attr(n, "color")) && parse_color(v, &c) == 1) s->color = c;
        if ((v = node_attr(n, "size"))) s->size = attr_size(v);
        if ((v = node_attr(n, "face")) && (strcasestr(v, "mono") || strcasestr(v, "courier"))) s->mono = 1;
    }
    if (eq(nm, "table") || eq(nm, "td") || eq(nm, "th") || eq(nm, "img") || eq(nm, "col")) {
        if ((v = node_attr(n, "width"))) {
            float f; int kind;
            if (parse_len(v, 16, &f, &kind)) { if (kind == 1) { s->width = f; s->wpct = -1; } else if (kind == 2) { s->wpct = f; s->width = -1; } }
        }
        if (eq(nm, "img") && (v = node_attr(n, "height"))) {
            float f; int kind;
            if (parse_len(v, 16, &f, &kind) && kind == 1) s->height = f;
        }
    }
}

static void style_defaults(Node *n, Style *s, Style *par) {
    const char *t = n->name;
    float em = s->size;
    if (in_list(t, NONE_TAGS)) { s->display = D_NONE; return; }
    if (in_list(t, BLOCK_TAGS)) s->display = D_BLOCK;
    if (eq(t, "body")) { s->mt = s->mb = s->ml = s->mr = 8; }
    else if (eq(t, "p") || eq(t, "dl") || eq(t, "blockquote") || eq(t, "fieldset") || eq(t, "figure") || eq(t, "menu") || eq(t, "dir")) {
        s->mt = s->mb = em;
        if (eq(t, "blockquote")) { s->ml = 40; s->mr = 40; }
        if (eq(t, "figure")) { s->ml = 40; s->mr = 40; }
        if (eq(t, "menu") || eq(t, "dir")) s->pl = 40;
    }
    else if (eq(t, "h1")) { s->size = em * 2.0f;  s->bold = 1; s->mt = s->mb = s->size * 0.67f; }
    else if (eq(t, "h2")) { s->size = em * 1.5f;  s->bold = 1; s->mt = s->mb = s->size * 0.83f; }
    else if (eq(t, "h3")) { s->size = em * 1.17f; s->bold = 1; s->mt = s->mb = s->size; }
    else if (eq(t, "h4")) { s->size = em;         s->bold = 1; s->mt = s->mb = s->size * 1.33f; }
    else if (eq(t, "h5")) { s->size = em * 0.83f; s->bold = 1; s->mt = s->mb = s->size * 1.67f; }
    else if (eq(t, "h6")) { s->size = em * 0.67f; s->bold = 1; s->mt = s->mb = s->size * 2.33f; }
    else if (eq(t, "ul") || eq(t, "ol")) {
        s->pl = 40; s->mt = s->mb = em;
        for (Node *a = n->parent; a; a = a->parent) if (a->type == 0 && (eq(a->name, "li") || eq(a->name, "ul") || eq(a->name, "ol"))) { s->mt = s->mb = 0; break; }
    }
    else if (eq(t, "li")) s->display = D_LISTITEM;
    else if (eq(t, "dd")) s->ml = 40;
    else if (eq(t, "pre") || eq(t, "xmp")) { s->mono = 1; s->pre = 1; s->nowrap = 1; s->size = 13; s->mt = s->mb = 13; }
    else if (eq(t, "hr")) { s->mt = s->mb = 8; }
    else if (eq(t, "table")) s->display = D_TABLE;
    else if (eq(t, "td") || eq(t, "th")) { s->pt = s->pb = s->pl = s->pr = 1; if (s->ctag) { s->align = 0; s->ctag = 0; } if (eq(t, "th")) { s->bold = 1; s->align = 1; } }
    else if (eq(t, "center")) { s->align = 1; s->ctag = 1; }
    else if (eq(t, "address")) s->italic = 1;
    else if (eq(t, "b") || eq(t, "strong")) s->bold = 1;
    else if (eq(t, "i") || eq(t, "em") || eq(t, "cite") || eq(t, "var") || eq(t, "dfn")) s->italic = 1;
    else if (eq(t, "u") || eq(t, "ins")) s->ul = 1;
    else if (eq(t, "s") || eq(t, "strike") || eq(t, "del")) s->strike = 1;
    else if (eq(t, "code") || eq(t, "kbd") || eq(t, "samp") || eq(t, "tt")) { s->mono = 1; s->size = em * 0.88f; }
    else if (eq(t, "small")) s->size = em * 0.83f;
    else if (eq(t, "big")) s->size = em * 1.2f;
    else if (eq(t, "sub") || eq(t, "sup")) s->size = em * 0.75f;
    else if (eq(t, "a")) {
        const char *h = node_attr(n, "href");
        if (h) { s->href = h; s->color = 0x0000EE; s->ul = 1; }
    }
    else if (eq(t, "mark")) { s->bg = 0xFFFF00; }
    else if (eq(t, "summary")) s->bold = 1;
    (void)par;
}

/* ================= pasada de estilos ================= */
static void style_node(Doc *d, Node *n, Style *par) {
    if (n->type == 1) { n->st = *par; n->st.display = D_INLINE; return; }
    Style *s = &n->st;
    *s = *par;
    s->nd = n;
    /* no heredados */
    s->bg = 0; s->has_bg = 0;
    s->mt = s->mb = s->ml = s->mr = s->pt = s->pb = s->pl = s->pr = 0;
    s->width = s->maxw = s->wpct = s->maxwpct = s->height = -1;
    s->ml_auto = s->mr_auto = 0;
    s->minh = -1; s->radius = 0; s->basis = -1; s->grow = 0; s->gap = s->rgap = 0; s->gridmin = 0;
    memset(s->bw, 0, sizeof s->bw); s->bcmask = 0;
    s->bsizing = s->fdir = s->fwrap = s->just = s->alignit = s->flt = s->gridcols = s->isflex = s->isgrid = s->clearf = s->masked = 0;
    s->display = D_INLINE;
    float psize = par->size;
    style_defaults(n, s, par);
    if (s->display == D_NONE) return;
    legacy_attrs(n, s);
    /* tablas: cellpadding */
    if (eq(n->name, "td") || eq(n->name, "th")) {
        for (Node *a = n->parent; a; a = a->parent) {
            if (a->type == 0 && eq(a->name, "table")) {
                const char *cp = node_attr(a, "cellpadding");
                if (cp) s->pt = s->pb = s->pl = s->pr = (float)atoi(cp);
                break;
            }
        }
    }
    /* reglas */
    Rule *rules = d->rules;
    Rule *hit[64]; int nh = 0;
    for (int i = 0; i < d->nrules && nh < 64; i++) if (match_rule(n, &rules[i])) hit[nh++] = &rules[i];
    for (int i = 1; i < nh; i++) {                        /* orden por especificidad, luego fuente */
        Rule *x = hit[i]; int j = i - 1;
        while (j >= 0 && (hit[j]->spec > x->spec || (hit[j]->spec == x->spec && hit[j]->order > x->order))) { hit[j + 1] = hit[j]; j--; }
        hit[j + 1] = x;
    }
    const char *sty = node_attr(n, "style");
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nh; i++) if (pass == 0 || strstr(hit[i]->decls, "!important")) apply_decls(s, hit[i]->decls, psize, pass);
        if (sty && (pass == 0 || strstr(sty, "!important"))) apply_decls(s, sty, psize, pass);
    }
    if (s->masked) { s->has_bg = 0; s->ibg = 0; }
    if (s->width == 1 && s->height == 1) s->display = D_NONE;
    if (s->display == D_NONE) return;
    if (node_attr(n, "hidden")) { s->display = D_NONE; return; }
    if (eq(n->name, "a") && !node_attr(n, "href")) { /* sin href: no hereda enlace nuevo */ }
    if (s->display == D_INLINE && s->has_bg) { s->ibg = 1; s->ibgc = s->bg; }
    else if (s->display != D_INLINE) s->ibg = 0;
    for (Node *c = n->first; c; c = c->next) style_node(d, c, s);
    if (s->display == D_INLINE) {
        for (Node *c = n->first; c; c = c->next)
            if (c->type == 0 && (c->st.display == D_BLOCK || c->st.display == D_LISTITEM || c->st.display == D_TABLE)) { s->display = D_BLOCK; break; }
    }
}

void doc_prepend_css(Doc *d, const char *css, size_t n) {
    char *nc = malloc(n + d->csslen + 2);
    memcpy(nc, css, n);
    nc[n] = '\n';
    if (d->csslen) memcpy(nc + n + 1, d->css, d->csslen);
    free(d->css);
    d->css = nc; d->csslen = n + 1 + d->csslen;
}

void doc_append_css(Doc *d, const char *css, size_t n) {
    d->css = realloc(d->css, d->csslen + n + 2);
    memcpy(d->css + d->csslen, css, n);
    d->csslen += n; d->css[d->csslen++] = '\n';
}

void doc_style(Doc *d) {
    cur_doc = d;
    d->nrules = 0; d->nvars = 0;
    if (d->css && d->csslen) css_parse(d, d->css, d->csslen);
    Style base;
    memset(&base, 0, sizeof base);
    base.size = 16; base.color = 0x000000; base.display = D_BLOCK;
    base.width = base.maxw = base.wpct = base.maxwpct = base.height = -1;
    base.lh = -1; base.minh = -1; base.basis = -1;
    d->root->st = base;
    for (Node *c = d->root->first; c; c = c->next) style_node(d, c, &base);
    /* fondo del lienzo: html o body */
    d->canvas_bg = 0xFFFFFF;
    Node *html = NULL, *body = NULL;
    for (Node *c = d->root->first; c; c = c->next) if (c->type == 0 && eq(c->name, "html")) { html = c; break; }
    Node *scan = html ? html : d->root;
    for (Node *c = scan->first; c; c = c->next) if (c->type == 0 && eq(c->name, "body")) { body = c; break; }
    if (html && html->st.has_bg) d->canvas_bg = html->st.bg;
    else if (body && body->st.has_bg) d->canvas_bg = body->st.bg;
}

/* ================= imágenes ================= */
static void collect_imgs(Doc *d, Node *n, int *cap) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != 0 || c->st.display == D_NONE) continue;
        if (eq(c->name, "img") && !c->img_state) {
            const char *src = node_attr(c, "src");
            const char *ds = node_attr(c, "data-src");
            if (ds && (!src || !*src || !strncmp(src, "data:image/gif", 14) || !strncmp(src, "data:image/svg", 14))) src = ds;
            if (src && *src && d->nimgs < 60) {
                char abs[2048];
                url_resolve(d->base, src, abs, sizeof abs);
                c->img_url = arena_strndup(&d->arena, abs, strlen(abs));
                if (d->nimgs == *cap) { *cap = *cap ? *cap * 2 : 16; d->imgs = realloc(d->imgs, *cap * sizeof(Node *)); }
                d->imgs[d->nimgs++] = c;
                c->img_state = 1;
            }
        }
        collect_imgs(d, c, cap);
    }
}
void doc_collect_images(Doc *d) {
    int cap = 0;
    collect_imgs(d, d->root, &cap);
}

/* ================= modo lectura: busca el contenido principal ================= */
static void text_stats(Node *n, int *tot, int *lnk, int inlink) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == 1) {
            int k = 0;
            for (int i = 0; i < c->tlen; i++) if (!isspace((unsigned char)c->text[i])) k++;
            *tot += k;
            if (inlink) *lnk += k;
        } else if (c->st.display != D_NONE) text_stats(c, tot, lnk, inlink || eq(c->name, "a"));
    }
}
static int is_blockn(Node *n) { return n->type == 0 && (n->st.display == D_BLOCK || n->st.display == D_LISTITEM || n->st.display == D_TABLE); }
static int sum_scores(Node *n) {
    int hasblock = 0;
    for (Node *c = n->first; c; c = c->next) if (is_blockn(c)) { hasblock = 1; break; }
    int s = 0;
    if (!hasblock && is_blockn(n)) {
        int tot = 0, lnk = 0;
        text_stats(n, &tot, &lnk, 0);
        if (tot >= 25) { s = tot - lnk - lnk / 2; if (s < 0) s = 0; }
    } else {
        for (Node *c = n->first; c; c = c->next)
            if (c->type == 0 && c->st.display != D_NONE) s += sum_scores(c);
    }
    n->rscore = s;
    return s;
}
static Node *find_tag(Node *n, const char *tag) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != 0 || c->st.display == D_NONE) continue;
        if (eq(c->name, tag)) return c;
        Node *r = find_tag(c, tag);
        if (r) return r;
    }
    return NULL;
}
Node *doc_find_main(Doc *d) {
    if (d->main_done) return d->main_node;
    d->main_done = 1;
    sum_scores(d->root);
    Node *node = d->root;
    for (;;) {
        Node *best = NULL;
        for (Node *c = node->first; c; c = c->next)
            if (c->type == 0 && c->st.display != D_NONE && (!best || c->rscore > best->rscore)) best = c;
        if (!best || best->rscore * 100 < node->rscore * 60) break;
        node = best;
    }
    if (node == d->root || eq(node->name, "html") || eq(node->name, "body") || node->rscore < 150) { d->main_node = NULL; return NULL; }
    d->main_node = node;
    Node *h1 = find_tag(d->root, "h1");
    for (Node *a = h1; a; a = a->parent) if (a == node) { h1 = NULL; break; }
    d->h1_node = h1;
    return node;
}

void doc_free(Doc *d) {
    doc_js_free(d);
    for (int i = 0; i < d->nimgs; i++) image_free(d->imgs[i]->img);
    free(d->imgs);
    free(d->css);
    free(d->rules);
    free(d->vars);
    arena_free(&d->arena);
    free(d);
}

/* ================= parser ================= */
typedef struct { Doc *d; Node *stk[512]; int sp; int have_html, have_body; } P;

static Node *new_node(Doc *d, int type) { Node *n = arena_alloc(&d->arena, sizeof(Node)); n->type = type; return n; }
static void append(Node *p, Node *c) {
    c->parent = p;
    if (p->last) p->last->next = c; else p->first = c;
    p->last = c;
}

static const char *const VOID_TAGS[] = {"area","base","br","col","embed","hr","img","input","link","meta","param","source","track","wbr","keygen", 0};
static const char *const PCLOSE[] = {"address","article","aside","blockquote","center","details","dir","div","dl","fieldset","figcaption",
    "figure","footer","form","h1","h2","h3","h4","h5","h6","header","hgroup","hr","main","menu","nav","ol","p","pre","section","table","ul","li","dd","dt", 0};
static const char *const PSTOP[] = {"table","td","th","caption","button","object","html","body", 0};

static int find_open(P *p, const char *a, const char *b, const char *const *stops) {
    for (int k = p->sp - 1; k >= 1; k--) {
        const char *nm = p->stk[k]->name;
        if (!strcmp(nm, a) || (b && !strcmp(nm, b))) return k;
        if (stops && in_list(nm, stops)) return -1;
    }
    return -1;
}

static void start_tag(P *p, const char *name, Attr *at, int nat, int selfclose) {
    Doc *d = p->d;
    if (eq(name, "html")) { if (p->have_html) return; p->have_html = 1; }
    if (eq(name, "body")) { if (p->have_body) return; p->have_body = 1; }
    int k;
    if (in_list(name, PCLOSE)) { if ((k = find_open(p, "p", NULL, PSTOP)) > 0) p->sp = k; }
    if (eq(name, "li")) { static const char *const st[] = {"ul","ol","menu","dir","table","html", 0}; if ((k = find_open(p, "li", NULL, st)) > 0) p->sp = k; }
    else if (eq(name, "dt") || eq(name, "dd")) { static const char *const st[] = {"dl","table","html", 0}; if ((k = find_open(p, "dt", "dd", st)) > 0) p->sp = k; }
    else if (eq(name, "tr")) { static const char *const st[] = {"table","html", 0}; if ((k = find_open(p, "tr", NULL, st)) > 0) p->sp = k; }
    else if (eq(name, "td") || eq(name, "th")) { static const char *const st[] = {"tr","table","html", 0}; if ((k = find_open(p, "td", "th", st)) > 0) p->sp = k; }
    else if (eq(name, "thead") || eq(name, "tbody") || eq(name, "tfoot")) {
        int low = -1;
        for (int j = p->sp - 1; j >= 1; j--) {
            const char *nm = p->stk[j]->name;
            if (eq(nm, "table")) break;
            if (eq(nm, "tr") || eq(nm, "td") || eq(nm, "th") || eq(nm, "thead") || eq(nm, "tbody") || eq(nm, "tfoot") || eq(nm, "caption")) low = j;
        }
        if (low > 0) p->sp = low;
    }
    else if (eq(name, "option")) { static const char *const st[] = {"select","datalist","html", 0}; if ((k = find_open(p, "option", NULL, st)) > 0) p->sp = k; }
    else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        const char *top = p->stk[p->sp - 1]->name;
        if (top[0] == 'h' && top[1] >= '1' && top[1] <= '6' && !top[2]) p->sp--;
    }
    else if (eq(name, "a")) { static const char *const st[] = {"table","td","th","html", 0}; if ((k = find_open(p, "a", NULL, st)) > 0) p->sp = k; }

    Node *n = new_node(d, 0);
    n->name = arena_strndup(&d->arena, name, strlen(name));
    if (nat) {
        n->attrs = arena_alloc(&d->arena, sizeof(Attr) * nat);
        memcpy(n->attrs, at, sizeof(Attr) * nat);
        n->nattrs = nat;
    }
    append(p->stk[p->sp - 1], n);
    if (eq(name, "base")) {
        const char *h = node_attr(n, "href");
        if (h) { char abs[2048]; url_resolve(d->base, h, abs, sizeof abs); d->base = arena_strndup(&d->arena, abs, strlen(abs)); }
    }
    if (in_list(name, VOID_TAGS) || (selfclose && !(eq(name, "script") || eq(name, "style") || eq(name, "title") || eq(name, "textarea")))) return;
    if (p->sp < 500) p->stk[p->sp++] = n;
}

static void end_tag(P *p, const char *name) {
    if (eq(name, "html") || eq(name, "body") || eq(name, "br") || in_list(name, VOID_TAGS)) return;
    static const char *const stop_generic[] = {"table","td","th","html", 0};
    static const char *const stop_tbl[] = {"table","html", 0};
    static const char *const stop_none[] = {"html", 0};
    const char *const *stops = stop_generic;
    if (eq(name, "table")) stops = stop_none;
    else if (eq(name, "td") || eq(name, "th") || eq(name, "tr") || eq(name, "tbody") || eq(name, "thead") || eq(name, "tfoot") || eq(name, "caption")) stops = stop_tbl;
    int k = find_open(p, name, NULL, stops);
    if (k > 0) p->sp = k;
}

static void add_text(P *p, const char *s, size_t n) {
    if (!n) return;
    Node *t = new_node(p->d, 1);
    int len;
    t->text = decode_entities(&p->d->arena, s, n, &len);
    t->tlen = len;
    t->name = "#text";
    append(p->stk[p->sp - 1], t);
}

static const char *find_close(const char *s, size_t n, size_t from, const char *name) {
    size_t nl = strlen(name);
    for (size_t i = from; i + 2 + nl <= n; i++)
        if (s[i] == '<' && s[i + 1] == '/' && !strncasecmp(s + i + 2, name, nl)) return s + i;
    return NULL;
}

static void parse_loop(P *pp, const char *s, size_t n) {
    Doc *d = pp->d;
    P p = *pp;
    size_t i = 0;
    while (i < n) {
        if (s[i] == '<') {
            if (i + 3 < n && !strncmp(s + i, "<!--", 4)) {
                const char *e = NULL;
                for (size_t j = i + 4; j + 2 < n; j++) if (s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>') { e = s + j; break; }
                i = e ? (size_t)(e - s) + 3 : n;
                continue;
            }
            if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
                while (i < n && s[i] != '>') i++;
                i++;
                continue;
            }
            if (i + 1 < n && s[i + 1] == '/') {
                if (i + 2 < n && isalpha((unsigned char)s[i + 2])) {
                    size_t j = i + 2; char nm[32]; int k = 0;
                    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/') { if (k < 31) nm[k++] = tolower((unsigned char)s[j]); j++; }
                    nm[k] = 0;
                    while (j < n && s[j] != '>') j++;
                    i = j + 1;
                    end_tag(&p, nm);
                } else {
                    while (i < n && s[i] != '>') i++;
                    i++;
                }
                continue;
            }
            if (i + 1 < n && isalpha((unsigned char)s[i + 1])) {
                size_t j = i + 1; char nm[32]; int k = 0;
                while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>' && !(s[j] == '/' && j + 1 < n && s[j + 1] == '>')) { if (k < 31) nm[k++] = tolower((unsigned char)s[j]); j++; }
                nm[k] = 0;
                Attr at[32]; int nat = 0, selfclose = 0;
                for (;;) {
                    while (j < n && (isspace((unsigned char)s[j]) || (s[j] == '/' && !(j + 1 < n && s[j + 1] == '>')))) j++;
                    if (j >= n) break;
                    if (s[j] == '>') { j++; break; }
                    if (s[j] == '/') { selfclose = 1; j += 2; break; }
                    size_t ns = j;
                    while (j < n && !isspace((unsigned char)s[j]) && s[j] != '=' && s[j] != '>' && !(s[j] == '/' && j + 1 < n && s[j + 1] == '>')) j++;
                    char an[48]; size_t al = j - ns; if (al > 47) al = 47;
                    for (size_t q = 0; q < al; q++) an[q] = tolower((unsigned char)s[ns + q]);
                    an[al] = 0;
                    while (j < n && isspace((unsigned char)s[j])) j++;
                    const char *val = ""; int vlen = 0;
                    if (j < n && s[j] == '=') {
                        j++;
                        while (j < n && isspace((unsigned char)s[j])) j++;
                        size_t vs;
                        if (j < n && (s[j] == '"' || s[j] == '\'')) {
                            char q = s[j++]; vs = j;
                            while (j < n && s[j] != q) j++;
                            val = decode_entities(&d->arena, s + vs, j - vs, &vlen);
                            if (j < n) j++;
                        } else {
                            vs = j;
                            while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>') j++;
                            val = decode_entities(&d->arena, s + vs, j - vs, &vlen);
                        }
                    }
                    if (nat < 32 && al > 0) { at[nat].name = arena_strndup(&d->arena, an, al); at[nat].val = val; nat++; }
                }
                i = j;
                start_tag(&p, nm, at, nat, selfclose);
                if (!selfclose && (eq(nm, "script") || eq(nm, "style") || eq(nm, "textarea") || eq(nm, "title") || eq(nm, "xmp"))) {
                    const char *e = find_close(s, n, i, nm);
                    size_t cend = e ? (size_t)(e - s) : n;
                    if (eq(nm, "style")) {
                        const char *media = NULL;
                        for (int q = 0; q < nat; q++) if (eq(at[q].name, "media")) media = at[q].val;
                        if (!media || !strcasestr(media, "print")) {
                            d->css = realloc(d->css, d->csslen + (cend - i) + 2);
                            memcpy(d->css + d->csslen, s + i, cend - i);
                            d->csslen += cend - i;
                            d->css[d->csslen++] = '\n';
                        }
                    } else if (eq(nm, "title")) {
                        int tl; char *tt = decode_entities(&d->arena, s + i, cend - i, &tl);
                        char *w = tt; int sp = 0;
                        for (char *r = tt; *r; r++) {
                            if (isspace((unsigned char)*r)) { sp = 1; continue; }
                            if (sp && w != tt) *w++ = ' ';
                            sp = 0; *w++ = *r;
                        }
                        *w = 0;
                        if (!d->title) d->title = tt;
                    } else if (eq(nm, "textarea") || eq(nm, "xmp")) {
                        add_text(&p, s + i, cend - i);
                    } else if (eq(nm, "script") && cend > i) {
                        Node *t = new_node(d, 1);
                        t->text = arena_strndup(&d->arena, s + i, cend - i);
                        t->tlen = (int)(cend - i);
                        t->name = "#text";
                        append(p.stk[p.sp - 1], t);
                    }
                    i = cend;
                    if (e) { while (i < n && s[i] != '>') i++; i++; }
                    end_tag(&p, nm);
                }
                continue;
            }
        }
        size_t j = i + 1;
        while (j < n) {
            if (s[j] == '<' && j + 1 < n && (isalpha((unsigned char)s[j + 1]) || s[j + 1] == '/' || s[j + 1] == '!' || s[j + 1] == '?')) break;
            j++;
        }
        add_text(&p, s + i, j - i);
        i = j;
    }
    *pp = p;
}

Doc *html_parse(const char *s, size_t n, const char *url) {
    Doc *d = calloc(1, sizeof(Doc));
    d->refs = 1; d->canvas_bg = 0xFFFFFF;
    d->base = arena_strndup(&d->arena, url, strlen(url));
    d->root = new_node(d, 0);
    d->root->name = "#root";
    P p; memset(&p, 0, sizeof p);
    p.d = d; p.stk[0] = d->root; p.sp = 1;
    parse_loop(&p, s, n);
    return d;
}

/* parsea un fragmento dentro del elemento ctx (para innerHTML) */
void html_parse_fragment(Doc *d, Node *ctx, const char *s, size_t n) {
    P p; memset(&p, 0, sizeof p);
    p.d = d; p.stk[0] = ctx; p.sp = 1; p.have_html = p.have_body = 1;
    parse_loop(&p, s, n);
}


/* ================= API para el DOM de JavaScript ================= */
static int parse_selector_list(const char *sel, Rule *out, int max) {
    int cnt = 0;
    const char *p = sel;
    while (*p && cnt < max) {
        const char *st = p; int depth = 0, br = 0; char q = 0;
        while (*p && (depth || br || q || *p != ',')) {
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') depth++; else if (*p == ')') depth--;
            else if (*p == '[') br++; else if (*p == ']') br--;
            p++;
        }
        char buf[512]; size_t l = p - st; if (l > 511) l = 511;
        memcpy(buf, st, l); buf[l] = 0;
        if (parse_selector(buf, &out[cnt])) cnt++;
        if (*p == ',') p++;
    }
    return cnt;
}

static int sel_match_any(Node *n, Rule *r, int nr) {
    for (int i = 0; i < nr; i++) if (match_rule(n, &r[i])) return 1;
    return 0;
}

static void select_rec(Node *n, Rule *r, int nr, Node **out, int max, int *cnt, int first) {
    for (Node *c = n->first; c && *cnt < max; c = c->next) {
        if (c->type != 0) continue;
        if (sel_match_any(c, r, nr)) { out[(*cnt)++] = c; if (first) return; }
        if (first && *cnt) return;
        select_rec(c, r, nr, out, max, cnt, first);
        if (first && *cnt) return;
    }
}

int dom_select(Node *scope, const char *sel, Node **out, int max, int first) {
    Rule *r = malloc(8 * sizeof(Rule));
    int nr = parse_selector_list(sel, r, 8), cnt = 0;
    if (nr) select_rec(scope, r, nr, out, max, &cnt, first);
    free(r);
    return cnt;
}

int dom_matches(Node *n, const char *sel) {
    if (n->type != 0) return 0;
    Rule *r = malloc(8 * sizeof(Rule));
    int nr = parse_selector_list(sel, r, 8);
    int m = nr ? sel_match_any(n, r, nr) : 0;
    free(r);
    return m;
}

typedef struct { char *s; size_t n, cap; } Ser;
static void ser_add(Ser *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void ser_esc(Ser *b, const char *s, int n, int attr) {
    for (int i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': ser_add(b, "&amp;", 5); break;
        case '<': if (attr) ser_add(b, "<", 1); else ser_add(b, "&lt;", 4); break;
        case '>': if (attr) ser_add(b, ">", 1); else ser_add(b, "&gt;", 4); break;
        case '"': if (attr) ser_add(b, "&quot;", 6); else ser_add(b, "\"", 1); break;
        default: ser_add(b, s + i, 1);
        }
    }
}
static void ser_node(Ser *b, Node *n) {
    if (n->type == 1) {
        int raw = n->parent && n->parent->type == 0 && (eq(n->parent->name, "script") || eq(n->parent->name, "style"));
        if (raw) ser_add(b, n->text, n->tlen); else ser_esc(b, n->text, n->tlen, 0);
        return;
    }
    if (n->name[0] == '#') { for (Node *c = n->first; c; c = c->next) ser_node(b, c); return; }
    ser_add(b, "<", 1); ser_add(b, n->name, strlen(n->name));
    for (int i = 0; i < n->nattrs; i++) {
        ser_add(b, " ", 1); ser_add(b, n->attrs[i].name, strlen(n->attrs[i].name));
        ser_add(b, "=\"", 2); ser_esc(b, n->attrs[i].val, (int)strlen(n->attrs[i].val), 1); ser_add(b, "\"", 1);
    }
    ser_add(b, ">", 1);
    if (in_list(n->name, VOID_TAGS)) return;
    for (Node *c = n->first; c; c = c->next) ser_node(b, c);
    ser_add(b, "</", 2); ser_add(b, n->name, strlen(n->name)); ser_add(b, ">", 1);
}
char *node_serialize(Node *n, int outer) {
    Ser b = {0};
    ser_add(&b, "", 0);
    if (outer) ser_node(&b, n);
    else for (Node *c = n->first; c; c = c->next) ser_node(&b, c);
    return b.s;
}
