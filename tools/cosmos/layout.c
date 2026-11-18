/* Cosmos Browser - motor de layout (bloques, líneas, listas, tablas) */
#include "cosmos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

static int eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

typedef struct {
    Page *pg; Doc *doc; float zoom;
    int y, pend, maxx, mode;           /* mode: 0 normal, 1 ancho preferido, 2 ancho mínimo */
    int fl_lw, fl_lb, fl_rw, fl_rb;    /* floats activos: ancho y borde inferior (izq./der.) */
    Node *curnd;
} Ctx;
#define Z(v) ((int)((v) * c->zoom + 0.5f))

/* ---------- items ---------- */
static int push_item(Ctx *c) {
    Page *p = c->pg;
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 1024; p->it = realloc(p->it, (size_t)p->cap * sizeof(Item)); }
    memset(&p->it[p->n], 0, sizeof(Item));
    p->it[p->n].nd = c->curnd;
    return p->n++;
}
static int add_rect(Ctx *c, int x, int y, int w, int h, uint32_t col) {
    int i = push_item(c);
    Item *t = &c->pg->it[i];
    t->kind = IT_RECT; t->x = x; t->y = y; t->w = w; t->h = h; t->color = col;
    return i;
}
static void add_frame(Ctx *c, int x, int y, int w, int h, uint32_t col) {
    int i = push_item(c);
    Item *t = &c->pg->it[i];
    t->kind = IT_FRAME; t->x = x; t->y = y; t->w = w; t->h = h; t->color = col;
}
static void add_anchor(Ctx *c, const char *id, int y) {
    Page *p = c->pg;
    if (c->mode || !id || !*id) return;
    if (p->nan == p->can) { p->can = p->can ? p->can * 2 : 64; p->an = realloc(p->an, (size_t)p->can * sizeof(Anchor)); }
    p->an[p->nan].id = id; p->an[p->nan].y = y; p->nan++;
}
static void flush_pend(Ctx *c) { c->y += c->pend; c->pend = 0; }

static int font_of(Style *s) {
    if (s->mono) return s->bold ? FONT_MONOBOLD : FONT_MONO;
    if (s->bold) return FONT_BOLD;
    if (s->italic) return FONT_ITALIC;
    return FONT_SANS;
}
static int px_of(Ctx *c, Style *s) {
    int p = (int)(s->size * c->zoom + 0.5f);
    return p < 6 ? 6 : p;
}
static float attr_num(Node *n, const char *name) {
    const char *v = node_attr(n, name);
    if (!v || strchr(v, '%')) return -1;
    float f = (float)atof(v);
    return f > 0 ? f : -1;
}

/* ---------- línea ---------- */
enum { A_WORD, A_IMG, A_CTRL, A_BR, A_SPACE };
enum { CT_TEXT, CT_BUTTON, CT_CHECK, CT_RADIO, CT_AREA, CT_SELECT };

typedef struct {
    Node *nd;
    uint8_t kind, nowrap, space, ul, st, ctype, checked, placeholder, has_bg;
    int font, px, lhpx; uint32_t color, bg;
    const char *s; int len; const char *href;
    int w, h, asc, desc, x, spw;
    Image *img; const char *label; int llen;
} Atom;

typedef struct { Ctx *c; int x0, w, align; Atom *a; int na, ca; int curx; int space; } Line;

static void line_init(Line *L, Ctx *c, int x0, int w, int align) {
    memset(L, 0, sizeof *L);
    L->c = c; L->x0 = x0; L->w = w; L->align = align;
}

static int fit_len(int font, int px, const char *s, int len, int maxw) {
    const char *p = s, *e = s + len;
    int w = 0;
    while (p < e) {
        const char *q = p;
        int cp = utf8_next(&q, e);
        char tmp[4]; int tl = utf8_put(tmp, cp);
        int cw = text_w(font, px, tmp, tl);
        if (w + cw > maxw) return (int)(p - s);
        w += cw; p = q;
    }
    return len;
}

static char *page_str(Ctx *c, const char *s, int n) { return arena_strndup(&c->pg->arena, s, (size_t)n); }

static void emit_ctrl(Ctx *c, Atom *a, int x, int y) {
    int px = a->px;
    switch (a->ctype) {
    case CT_CHECK: case CT_RADIO:
        add_rect(c, x, y, a->w, a->h, 0xFFFFFF);
        add_frame(c, x, y, a->w, a->h, 0x767676);
        if (a->checked) add_rect(c, x + a->w / 4 + 1, y + a->h / 4 + 1, a->w / 2 - 1, a->h / 2 - 1, 0x1a56db);
        return;
    case CT_BUTTON:
        add_rect(c, x, y, a->w, a->h, 0xEFEFEF);
        add_frame(c, x, y, a->w, a->h, 0x767676);
        break;
    default:
        add_rect(c, x, y, a->w, a->h, 0xFFFFFF);
        add_frame(c, x, y, a->w, a->h, 0x767676);
    }
    if (a->label && a->llen > 0) {
        int pad = Z(4);
        int maxw = a->w - 2 * pad - (a->ctype == CT_SELECT ? Z(12) : 0);
        int len = fit_len(FONT_SANS, px, a->label, a->llen, maxw);
        int tw = text_w(FONT_SANS, px, a->label, len);
        int tx = a->ctype == CT_BUTTON ? x + (a->w - tw) / 2 : x + pad;
        int ty = y + (a->h - (font_asc(FONT_SANS, px) + font_desc(FONT_SANS, px))) / 2;
        if (a->ctype == CT_AREA) ty = y + pad;
        int i = push_item(c);
        Item *t = &c->pg->it[i];
        t->kind = IT_TEXT; t->x = tx; t->y = ty; t->w = tw; t->h = px; t->font = FONT_SANS; t->px = px;
        t->color = a->placeholder ? 0x757575 : 0x111111; t->s = a->label; t->len = len;
    }
    if (a->ctype == CT_SELECT) {
        const char *ar = "\xe2\x96\xbe";
        int i = push_item(c);
        Item *t = &c->pg->it[i];
        t->kind = IT_TEXT; t->x = x + a->w - Z(14); t->y = y + (a->h - px) / 2; t->w = Z(10); t->h = px;
        t->font = FONT_SANS; t->px = px; t->color = 0x444444; t->s = ar; t->len = 3;
    }
}

static void emit_line(Line *L) {
    Ctx *c = L->c;
    if (!L->na) return;
    int asc = 0, desc = 0, maxpx = 0;
    for (int i = 0; i < L->na; i++) {
        Atom *a = &L->a[i];
        if (a->asc > asc) asc = a->asc;
        if (a->desc > desc) desc = a->desc;
        if (a->px > maxpx) maxpx = a->px;
    }
    int extra = maxpx / 5;
    int lh = asc + desc + extra;
    int lhpx = 0;
    for (int i = 0; i < L->na; i++) if (L->a[i].lhpx > lhpx) lhpx = L->a[i].lhpx;
    if (lhpx > 0) { lh = lhpx > asc + desc ? lhpx : asc + desc; extra = lh - (asc + desc); }
    flush_pend(c);
    int base = c->y + asc + extra / 2;
    int off = 0;
    if (!c->mode) {
        if (L->align == 1) off = (L->w - L->curx) / 2;
        else if (L->align == 2) off = L->w - L->curx;
        if (off < 0) off = 0;
    }
    for (int i = 0; i < L->na; i++) {
        Atom *a = &L->a[i];
        int x = L->x0 + off + a->x;
        if (a->kind == A_WORD) {
            if (a->has_bg && !c->mode) {
                int bwid = a->w;
                if (i + 1 < L->na) { Atom *nx = &L->a[i + 1]; if (nx->kind == A_WORD && nx->has_bg && nx->bg == a->bg) bwid = nx->x - a->x; }
                add_rect(c, x, base - a->asc - 1, bwid, a->asc + a->desc + 2, a->bg);
            }
            int k = push_item(c);
            Item *t = &c->pg->it[k];
            t->kind = IT_TEXT; t->x = x; t->y = base - a->asc; t->w = a->w; t->h = a->asc + a->desc;
            if (i + 1 < L->na) {   /* si sigue una palabra del mismo enlace, cubre el espacio */
                Atom *nx = &L->a[i + 1];
                if (nx->kind == A_WORD && nx->space && a->href && nx->href == a->href && nx->ul == a->ul && nx->st == a->st) t->w = nx->x - a->x;
            }
            t->font = a->font; t->px = a->px; t->color = a->color; t->ul = a->ul; t->st = a->st;
            t->s = a->s; t->len = a->len; t->href = a->href; t->nd = a->nd;
        } else if (a->kind == A_IMG) {
            int y = base - a->h;
            if (a->img) {
                int k = push_item(c);
                Item *t = &c->pg->it[k];
                t->kind = IT_IMG; t->x = x; t->y = y; t->w = a->w; t->h = a->h; t->img = a->img; t->href = a->href; t->nd = a->nd;
            } else {
                int k = add_rect(c, x, y, a->w, a->h, 0xEDEDED);
                c->pg->it[k].href = a->href;
                add_frame(c, x, y, a->w, a->h, 0xB0B0B0);
                if (a->label && a->llen) {
                    int len = fit_len(FONT_SANS, a->px, a->label, a->llen, a->w - 4);
                    int k2 = push_item(c);
                    Item *t = &c->pg->it[k2];
                    t->kind = IT_TEXT; t->x = x + 3; t->y = y + 2; t->w = a->w; t->h = a->px; t->font = FONT_SANS;
                    t->px = a->px; t->color = 0x555555; t->s = a->label; t->len = len; t->href = a->href;
                }
            }
        } else if (a->kind == A_CTRL) {
            emit_ctrl(c, a, x, base - a->asc);
        } else if (a->kind == A_SPACE && a->has_bg && !c->mode && a->w > 0) {
            add_rect(c, x, base - a->asc - 1, a->w, a->asc + a->desc + 2, a->bg);
        }
    }
    int right = L->x0 + off + L->curx;
    if (right > c->maxx) c->maxx = right;
    c->y += lh;
    L->na = 0; L->curx = 0;
}

static void line_push(Line *L, Atom *a) {
    Ctx *c = L->c;
    int sp = 0;
    if (L->na > 0 && a->space) sp = a->spw;
    if (L->na > 0 && !a->nowrap && L->curx + sp + a->w > L->w) { emit_line(L); sp = 0; }
    a->x = L->na > 0 ? L->curx + sp : 0;
    L->curx = a->x + a->w;
    if (L->na == L->ca) { L->ca = L->ca ? L->ca * 2 : 32; L->a = realloc(L->a, (size_t)L->ca * sizeof(Atom)); }
    L->a[L->na++] = *a;
    (void)c;
}

static void push_word(Line *L, Style *s, const char *w, int len, int space, int nowrap) {
    Ctx *c = L->c;
    int font = font_of(s), px = px_of(c, s);
    Atom a;
    memset(&a, 0, sizeof a);
    a.kind = A_WORD; a.font = font; a.px = px; a.color = s->color; a.ul = s->ul; a.st = s->strike;
    a.nd = s->nd; a.href = s->href; a.nowrap = nowrap; a.asc = font_asc(font, px); a.desc = font_desc(font, px);
    a.spw = text_w(font, px, " ", 1);
    a.space = space;
    a.has_bg = s->ibg; a.bg = s->ibgc;
    if (s->lh > 0) a.lhpx = (int)(s->lh * px + 0.5f);
    int tw = text_w(font, px, w, len);
    if (tw > L->w && !nowrap && c->mode == 0 && L->w >= 30) {
        if (L->na > 0) emit_line(L);
        while (len > 0) {
            int k = fit_len(font, px, w, len, L->w);
            if (k <= 0) { const char *q = w; utf8_next(&q, w + len); k = (int)(q - w); }
            a.s = w; a.len = k; a.w = text_w(font, px, w, k); a.space = 0;
            line_push(L, &a);
            w += k; len -= k;
            if (len > 0) emit_line(L);
        }
        return;
    }
    a.s = w; a.len = len; a.w = tw;
    line_push(L, &a);
}

static void push_space(Line *L, Style *s, int w, int withbg) {
    if (w <= 0) return;
    Ctx *c = L->c;
    int font = font_of(s), px = px_of(c, s);
    Atom a;
    memset(&a, 0, sizeof a);
    a.kind = A_SPACE; a.font = font; a.px = px; a.asc = font_asc(font, px); a.desc = font_desc(font, px);
    a.w = w; a.nowrap = 1; a.space = 0;
    if (withbg && s->has_bg) { a.has_bg = 1; a.bg = s->bg; }
    line_push(L, &a);
}

static void push_br(Line *L, Style *s) {
    Ctx *c = L->c;
    int font = font_of(s), px = px_of(c, s);
    Atom a;
    memset(&a, 0, sizeof a);
    a.kind = A_BR; a.font = font; a.px = px; a.asc = font_asc(font, px); a.desc = font_desc(font, px);
    a.nowrap = 1;
    line_push(L, &a);
    emit_line(L);
}

static void inline_text(Line *L, Node *tn) {
    Style *s = &tn->st;
    Ctx *c = L->c;
    const char *p = tn->text, *e = p + tn->tlen;
    if (s->ttrans && tn->tlen > 0) {
        char *buf = arena_alloc(&c->pg->arena, (size_t)tn->tlen + 1);
        int cap = 1;
        for (int i = 0; i < tn->tlen; i++) {
            unsigned char ch = (unsigned char)tn->text[i];
            unsigned char nx = i + 1 < tn->tlen ? (unsigned char)tn->text[i + 1] : 0;
            int up = s->ttrans == 1 || (s->ttrans == 3 && cap);
            int lo = s->ttrans == 2 || (s->ttrans == 3 && !cap);
            if (ch == 0xC3 && nx) {
                buf[i] = ch; i++;
                if (up && nx >= 0xA0 && nx <= 0xBE && nx != 0xB7) nx -= 0x20;
                else if (lo && nx >= 0x80 && nx <= 0x9E && nx != 0x97) nx += 0x20;
                buf[i] = nx; cap = 0;
                continue;
            }
            if (up && ch >= 'a' && ch <= 'z') ch -= 32;
            else if (lo && ch >= 'A' && ch <= 'Z') ch += 32;
            buf[i] = ch;
            cap = isspace(ch) || ch == '-' || ch == '(';
        }
        buf[tn->tlen] = 0;
        p = buf; e = buf + tn->tlen;
    }
    if (s->pre) {
        while (p < e) {
            const char *q = p;
            while (q < e && *q != '\n') q++;
            if (q > p) {
                int need = 0;
                for (const char *r = p; r < q; r++) if (*r == '\t' || *r == '\r') need = 1;
                if (need) {
                    char *buf = arena_alloc(&c->pg->arena, (size_t)(q - p) * 8 + 1);
                    int o = 0;
                    for (const char *r = p; r < q; r++) {
                        if (*r == '\t') { do buf[o++] = ' '; while (o % 8); }
                        else if (*r != '\r') buf[o++] = *r;
                    }
                    if (o) push_word(L, s, buf, o, 0, 1);
                } else push_word(L, s, p, (int)(q - p), 0, 1);
            }
            if (q < e) { push_br(L, s); q++; }
            p = q;
        }
        L->space = 0;
        return;
    }
    while (p < e) {
        if (isspace((unsigned char)*p)) { p++; L->space = 1; continue; }
        const char *q = p;
        while (q < e && !isspace((unsigned char)*q)) q++;
        push_word(L, s, p, (int)(q - p), L->space, s->nowrap);
        L->space = 0;
        p = q;
    }
}

static void gather_text(Node *n, char *buf, int *len, int cap) {
    for (Node *ch = n->first; ch; ch = ch->next) {
        if (ch->type == 1) {
            const char *p = ch->text, *e = p + ch->tlen;
            while (p < e) {
                if (isspace((unsigned char)*p)) { if (*len > 0 && buf[*len - 1] != ' ' && *len < cap - 1) buf[(*len)++] = ' '; p++; continue; }
                if (*len < cap - 1) buf[(*len)++] = *p;
                p++;
            }
        } else if (ch->st.display != D_NONE || eq(ch->name, "option")) gather_text(ch, buf, len, cap);
    }
    while (*len > 0 && buf[*len - 1] == ' ') (*len)--;
}

static void inline_img(Line *L, Node *n) {
    Ctx *c = L->c;
    Style *s = &n->st;
    const char *alt = node_attr(n, "alt");
    float wa = attr_num(n, "width"), ha = attr_num(n, "height");
    if (s->width >= 0) wa = s->width;
    if (s->height >= 0) ha = s->height;
    int w = 0, h = 0;
    Image *im = n->img;
    int px = px_of(c, s);
    if (im) {
        if (wa > 0 && ha > 0) { w = Z(wa); h = Z(ha); }
        else if (wa > 0) { w = Z(wa); h = (int)((float)im->h * w / im->w + 0.5f); }
        else if (ha > 0) { h = Z(ha); w = (int)((float)im->w * h / im->h + 0.5f); }
        else { w = Z(im->w); h = Z(im->h); }
    } else {
        if (wa > 0 && ha > 0) { w = Z(wa); h = Z(ha); }
        else if (alt && *alt) { w = text_w(FONT_SANS, px, alt, (int)strlen(alt)) + Z(8); h = px + Z(6); }
        else if (wa > 0 || ha > 0) { w = Z(wa > 0 ? wa : ha); h = Z(ha > 0 ? ha : wa); }
        else return;
    }
    if (c->mode == 0 && w > L->w && L->w > 0) { h = (int)((float)h * L->w / w + 0.5f); w = L->w; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Atom a;
    memset(&a, 0, sizeof a);
    a.kind = A_IMG; a.w = w; a.h = h; a.asc = h; a.desc = 0; a.px = px; a.img = im; a.href = s->href; a.nd = s->nd;
    a.space = L->space; a.spw = text_w(FONT_SANS, px, " ", 1);
    if (!im && alt) { a.label = alt; a.llen = (int)strlen(alt); }
    L->space = 0;
    line_push(L, &a);
}

static void inline_ctrl(Line *L, Node *n) {
    Ctx *c = L->c;
    Style *s = &n->st;
    const char *nm = n->name;
    int px = px_of(c, s);
    Atom a;
    memset(&a, 0, sizeof a);
    a.kind = A_CTRL; a.px = px; a.href = NULL;
    a.space = L->space; a.spw = text_w(FONT_SANS, px, " ", 1);
    char *lab = NULL; int ll = 0;
    if (eq(nm, "input")) {
        const char *type = node_attr(n, "type");
        if (!type) type = "text";
        if (!strcasecmp(type, "hidden")) return;
        const char *val = node_attr(n, "value");
        if (!strcasecmp(type, "checkbox") || !strcasecmp(type, "radio")) {
            a.ctype = !strcasecmp(type, "radio") ? CT_RADIO : CT_CHECK;
            a.w = a.h = Z(13);
            a.checked = node_attr(n, "checked") != NULL;
        } else if (!strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "reset") || !strcasecmp(type, "image")) {
            a.ctype = CT_BUTTON;
            const char *l = val ? val : (!strcasecmp(type, "reset") ? "Restablecer" : (!strcasecmp(type, "image") ? "Enviar" : "Enviar"));
            lab = (char *)l; ll = (int)strlen(l);
            a.w = text_w(FONT_SANS, px, l, ll) + Z(16); a.h = px + Z(10);
        } else {
            a.ctype = CT_TEXT;
            int size = node_attr(n, "size") ? atoi(node_attr(n, "size")) : 20;
            if (size < 2) size = 20;
            a.w = (int)(size * px * 0.55f) + Z(8);
            if (s->width >= 0) a.w = Z(s->width);
            if (a.w > L->w && c->mode == 0 && L->w > 40) a.w = L->w;
            a.h = px + Z(10);
            if (val && *val) {
                if (!strcasecmp(type, "password")) { lab = page_str(c, "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2", 18); ll = 18; }
                else { lab = (char *)val; ll = (int)strlen(val); }
            } else {
                const char *ph = node_attr(n, "placeholder");
                if (ph && *ph) { lab = (char *)ph; ll = (int)strlen(ph); a.placeholder = 1; }
            }
        }
    } else if (eq(nm, "button")) {
        char buf[256]; int len = 0;
        gather_text(n, buf, &len, sizeof buf);
        a.ctype = CT_BUTTON;
        if (!len) { strcpy(buf, "Bot\xc3\xb3n"); len = (int)strlen(buf); }
        lab = page_str(c, buf, len); ll = len;
        a.w = text_w(FONT_SANS, px, lab, ll) + Z(16); a.h = px + Z(10);
    } else if (eq(nm, "textarea")) {
        a.ctype = CT_AREA;
        int cols = node_attr(n, "cols") ? atoi(node_attr(n, "cols")) : 20;
        int rows = node_attr(n, "rows") ? atoi(node_attr(n, "rows")) : 2;
        if (cols < 2) cols = 20;
        if (rows < 1) rows = 2;
        if (rows > 20) rows = 20;
        a.w = (int)(cols * px * 0.55f) + Z(8);
        if (a.w > L->w && c->mode == 0 && L->w > 40) a.w = L->w;
        a.h = (int)(rows * px * 1.3f) + Z(8);
        char buf[200]; int len = 0;
        gather_text(n, buf, &len, sizeof buf);
        if (len) { lab = page_str(c, buf, len); ll = len; }
    } else { /* select */
        a.ctype = CT_SELECT;
        char buf[128]; int len = 0;
        Node *sel = NULL;
        for (Node *ch = n->first; ch; ch = ch->next) {
            Node *cand = NULL;
            if (ch->type == 0 && eq(ch->name, "option")) cand = ch;
            else if (ch->type == 0 && eq(ch->name, "optgroup")) for (Node *o = ch->first; o; o = o->next) if (o->type == 0 && eq(o->name, "option")) { cand = o; break; }
            if (cand) { if (!sel) sel = cand; if (node_attr(cand, "selected")) { sel = cand; break; } }
        }
        if (sel) gather_text(sel, buf, &len, sizeof buf);
        lab = page_str(c, buf, len); ll = len;
        a.w = text_w(FONT_SANS, px, lab, ll) + Z(30);
        if (a.w < Z(60)) a.w = Z(60);
        a.h = px + Z(10);
    }
    a.label = lab; a.llen = ll;
    a.asc = (int)(a.h * 0.75f); a.desc = a.h - a.asc;
    L->space = 0;
    line_push(L, &a);
}

static void inline_node(Line *L, Node *n) {
    if (n->type == 1) { inline_text(L, n); return; }
    Ctx *c = L->c;
    if (n->st.display == D_NONE) return;
    const char *nm = n->name;
    const char *id = node_attr(n, "id");
    if (!id) id = node_attr(n, "name");
    if (id) add_anchor(c, id, c->y + c->pend);
    if (eq(nm, "br")) { push_br(L, &n->st); L->space = 0; return; }
    if (eq(nm, "img")) { inline_img(L, n); return; }
    if (eq(nm, "input") || eq(nm, "button") || eq(nm, "textarea") || eq(nm, "select")) { inline_ctrl(L, n); return; }
    Style *es = &n->st;
    int lead_m = Z(es->ml), trail_m = Z(es->mr), lead_p = Z(es->pl) + Z(es->bw[3]), trail_p = Z(es->pr) + Z(es->bw[1]);
    if (es->display == D_INLINE && !c->mode) { push_space(L, es, lead_m, 0); push_space(L, es, lead_p, 1); }
    else if (es->display == D_INLINE) { push_space(L, es, lead_m + lead_p, 0); }
    for (Node *ch = n->first; ch; ch = ch->next) inline_node(L, ch);
    if (es->display == D_INLINE && !c->mode) { push_space(L, es, trail_p, 1); push_space(L, es, trail_m, 0); }
    else if (es->display == D_INLINE) { push_space(L, es, trail_m + trail_p, 0); }
}

/* ---------- bloques ---------- */
static void layout_block(Ctx *c, Node *n, int x, int w);
static void layout_block0(Ctx *c, Node *n, int x, int w);
static void layout_table(Ctx *c, Node *t, int x, int w);
static void layout_flex(Ctx *c, Node *n, int x, int w);

static int is_blockish(Node *n) {
    return n->type == 0 && (n->st.display == D_BLOCK || n->st.display == D_LISTITEM || n->st.display == D_TABLE);
}

static void cell_measure(Ctx *c, Node *cell);
static void place_float(Ctx *c, Node *ch, int x, int w) {
    Style *s = &ch->st;
    int left = s->flt == 1;
    int ml = Z(s->ml), mr = Z(s->mr), mb = Z(s->mb), mt = Z(s->mt);
    int sy = c->y, sp = c->pend;
    int fy = c->y + c->pend;
    int lx = x + ((fy < c->fl_lb) ? c->fl_lw : 0);
    int rw = (fy < c->fl_rb) ? c->fl_rw : 0;
    int avail = w - (lx - x) - rw;
    if (avail < 40) { fy = c->fl_lb > c->fl_rb ? c->fl_lb : c->fl_rb; lx = x; rw = 0; avail = w; c->fl_lw = c->fl_rw = c->fl_lb = c->fl_rb = 0; }
    int isimg = eq(ch->name, "img");
    int fw;
    if (isimg) {
        Line T; line_init(&T, c, 0, 100000, 0);
        int san = c->pg->nan, sn = c->pg->n;
        inline_node(&T, ch);
        fw = T.curx + ml + mr;
        free(T.a);
        c->pg->nan = san; c->pg->n = sn;
    } else {
        int pads = Z(s->pl) + Z(s->pr) + Z(s->bw[1]) + Z(s->bw[3]);
        if (s->width >= 0) fw = Z(s->width) + ml + mr + (s->bsizing ? 0 : pads);
        else if (s->wpct >= 0) fw = (int)(w * s->wpct / 100);
        else { cell_measure(c, ch); fw = ch->pref_w + ml + mr + Z(s->bw[1]) + Z(s->bw[3]); }
    }
    if (fw > avail) fw = avail;
    if (fw < 1) fw = 1;
    int fx = left ? lx : x + w - rw - fw;
    c->y = fy; c->pend = 0;
    if (isimg) {
        Line L; line_init(&L, c, fx + ml, fw - ml - mr, 0);
        inline_node(&L, ch);
        emit_line(&L);
        free(L.a);
        c->y += mb + mt;
    } else {
        Style save = *s;
        s->width = s->wpct = s->maxw = s->maxwpct = -1;
        s->ml_auto = s->mr_auto = 0;
        layout_block(c, ch, fx, fw);
        *s = save;
    }
    int h = c->y + c->pend - fy;
    c->y = sy; c->pend = sp;
    if (left) { if (fy < c->fl_lb) c->fl_lw += fw; else c->fl_lw = fw; if (fy + h > c->fl_lb || fy >= c->fl_lb) c->fl_lb = fy + h; }
    else { if (fy < c->fl_rb) c->fl_rw += fw; else c->fl_rw = fw; if (fy + h > c->fl_rb || fy >= c->fl_rb) c->fl_rb = fy + h; }
}

static void layout_children(Ctx *c, Node *n, int x, int w) {
    int s_lw = c->fl_lw, s_lb = c->fl_lb, s_rw = c->fl_rw, s_rb = c->fl_rb;
    c->fl_lw = c->fl_lb = c->fl_rw = c->fl_rb = 0;
    Node *ch = n->first;
    while (ch) {
        if (ch->type == 0 && ch->st.display == D_NONE) { ch = ch->next; continue; }
        if (ch->type == 0 && ch->st.flt && !c->mode) { place_float(c, ch, x, w); ch = ch->next; continue; }
        int ex = x, ew = w;
        if (!c->mode) {
            int cy = c->y + c->pend;
            if (ch->type == 0 && ch->st.clearf && (c->fl_lb > cy || c->fl_rb > cy)) {
                int nb = c->fl_lb > c->fl_rb ? c->fl_lb : c->fl_rb;
                c->y = nb; c->pend = 0; c->fl_lw = c->fl_lb = c->fl_rw = c->fl_rb = 0;
            } else {
                if (cy < c->fl_lb) { ex += c->fl_lw; ew -= c->fl_lw; }
                if (cy < c->fl_rb) ew -= c->fl_rw;
                if (ew < 80 && (cy < c->fl_lb || cy < c->fl_rb)) {
                    int nb = c->fl_lb > c->fl_rb ? c->fl_lb : c->fl_rb;
                    c->y = nb; c->pend = 0; ex = x; ew = w; c->fl_lw = c->fl_lb = c->fl_rw = c->fl_rb = 0;
                }
            }
        }
        if (is_blockish(ch)) { layout_block(c, ch, ex, ew); ch = ch->next; continue; }
        Line L;
        line_init(&L, c, ex, ew, n->st.align);
        while (ch && !is_blockish(ch)) {
            if (ch->type == 0 && ch->st.flt && !c->mode) break;
            if (!(ch->type == 0 && ch->st.display == D_NONE)) inline_node(&L, ch);
            ch = ch->next;
        }
        emit_line(&L);
        free(L.a);
    }
    if (!c->mode) {
        int fb = c->fl_lb > c->fl_rb ? c->fl_lb : c->fl_rb;
        if (fb > c->y + c->pend) { c->y = fb; c->pend = 0; }
    }
    c->fl_lw = s_lw; c->fl_lb = s_lb; c->fl_rw = s_rw; c->fl_rb = s_rb;
}

static void list_marker(Ctx *c, Node *n, int left) {
    Style *s = &n->st;
    if (s->list_none || c->mode) return;
    int px = px_of(c, s);
    int font = font_of(s);
    char buf[24];
    int len;
    int ordered = n->parent && n->parent->type == 0 && eq(n->parent->name, "ol");
    if (ordered) {
        int idx = 0;
        const char *st = node_attr(n->parent, "start");
        if (st) idx = atoi(st) - 1;
        for (Node *q = n->parent->first; q; q = q->next) {
            if (q->type == 0 && eq(q->name, "li")) idx++;
            if (q == n) break;
        }
        len = snprintf(buf, sizeof buf, "%d.", idx);
    } else { strcpy(buf, "\xe2\x80\xa2"); len = 3; }
    char *str = page_str(c, buf, len);
    int tw = text_w(font, px, str, len);
    int i = push_item(c);
    Item *t = &c->pg->it[i];
    t->kind = IT_TEXT; t->x = left - tw - Z(8); t->y = c->y + (px / 5) / 2; t->w = tw; t->h = px; t->font = font; t->px = px;
    t->color = s->color; t->s = str; t->len = len;
}

static void layout_block0(Ctx *c, Node *n, int x, int w) {
    Style *s = &n->st;
    const char *id = node_attr(n, "id");
    if (id) add_anchor(c, id, c->y + c->pend);
    if (s->display == D_TABLE) {
        int mt = Z(s->mt), mb = Z(s->mb);
        c->y += (c->pend > mt ? c->pend : mt);
        c->pend = 0;
        layout_table(c, n, x, w);
        c->pend = mb;
        return;
    }
    int ml = Z(s->ml), mr = Z(s->mr), mt = Z(s->mt), mb = Z(s->mb);
    int pl = Z(s->pl), pr = Z(s->pr), pt = Z(s->pt), pb = Z(s->pb);
    if (eq(n->name, "hr")) {
        int gap = c->pend > mt ? c->pend : mt;
        c->y += gap; c->pend = 0;
        int avail = w - ml - mr;
        if (avail < 1) avail = 1;
        int hw = avail;
        if (!c->mode) {
            if (s->width >= 0 && Z(s->width) < hw) hw = Z(s->width);
            else if (s->wpct >= 0) hw = (int)(avail * s->wpct / 100);
        }
        int hx = x + ml + (avail - hw) / 2;
        add_rect(c, hx, c->y, hw, 2, s->has_bg ? s->bg : 0x9A9A9A);
        c->y += 2;
        c->pend = mb;
        if (hx + hw > c->maxx) c->maxx = hx + hw;
        return;
    }
    int bT = Z(s->bw[0]), bR = Z(s->bw[1]), bB = Z(s->bw[2]), bL = Z(s->bw[3]);
    if (s->bw[0] > 0 && bT < 1) bT = 1;
    if (s->bw[1] > 0 && bR < 1) bR = 1;
    if (s->bw[2] > 0 && bB < 1) bB = 1;
    if (s->bw[3] > 0 && bL < 1) bL = 1;
    int hpad = pl + pr + bL + bR;
    int avail = w - ml - mr;
    if (avail < 1) avail = 1;
    int cw = avail - hpad;
    if (!c->mode) {
        int extra = s->bsizing ? 0 : hpad;           /* border-box: el ancho ya incluye relleno y borde */
        if (s->width >= 0) cw = Z(s->width) - (s->bsizing ? hpad : 0);
        else if (s->wpct >= 0) cw = (int)(w * s->wpct / 100) - hpad;
        if (s->maxw >= 0 && cw > Z(s->maxw) - (s->bsizing ? hpad : 0)) cw = Z(s->maxw) - (s->bsizing ? hpad : 0);
        else if (s->maxwpct >= 0 && cw > (int)(w * s->maxwpct / 100) - (s->bsizing ? hpad : 0)) cw = (int)(w * s->maxwpct / 100) - (s->bsizing ? hpad : 0);
        (void)extra;
        if (cw > avail - hpad) cw = avail - hpad;
    }
    if (cw < 1) cw = 1;
    int boxw = cw + hpad;
    int freew = avail - boxw;
    if (freew > 0 && !c->mode) {
        if (s->ml_auto && s->mr_auto) ml += freew / 2;
        else if (s->ml_auto) ml += freew;
    }
    int left = x + ml;
    int eager = s->has_bg || pt > 0 || bT > 0 || s->height >= 0 || s->minh >= 0;
    if (eager) { c->y += (c->pend > mt ? c->pend : mt); c->pend = 0; }
    else if (mt > c->pend) c->pend = mt;
    int start_y = c->y;
    int slot = -1;
    if (s->has_bg && !c->mode) slot = add_rect(c, left, start_y, boxw, 0, s->bg);
    c->y += pt + bT;
    if (s->display == D_LISTITEM) { flush_pend(c); list_marker(c, n, left + pl + bL); }
    if (s->isflex || s->isgrid) layout_flex(c, n, left + pl + bL, cw);
    else layout_children(c, n, left + pl + bL, cw);
    if (pb > 0 || bB > 0 || s->has_bg || s->height >= 0 || s->minh >= 0) {
        flush_pend(c);
        c->y += pb + bB;
        if (!c->mode) {
            if (s->height >= 0 && c->y - start_y < Z(s->height)) c->y = start_y + Z(s->height);
            if (s->minh >= 0 && c->y - start_y < Z(s->minh)) c->y = start_y + Z(s->minh);
        }
        c->pend = mb;
    } else if (mb > c->pend) c->pend = mb;
    int boxh = c->y - start_y;
    if (slot >= 0) {
        c->pg->it[slot].h = boxh;
        if (s->radius > 0) { int r = Z(s->radius), m = (boxw < boxh ? boxw : boxh) / 2; c->pg->it[slot].rad = (uint8_t)(r > m ? (m > 255 ? 255 : m) : (r > 255 ? 255 : r)); }
    }
    if (!c->mode && (bT || bR || bB || bL)) {
        uint32_t col[4];
        for (int i = 0; i < 4; i++) col[i] = (s->bcmask >> i) & 1 ? s->bc[i] : s->color;
        if (bT) add_rect(c, left, start_y, boxw, bT, col[0]);
        if (bB) add_rect(c, left, start_y + boxh - bB, boxw, bB, col[2]);
        if (bL) add_rect(c, left, start_y + bT, bL, boxh - bT - bB, col[3]);
        if (bR) add_rect(c, left + boxw - bR, start_y + bT, bR, boxh - bT - bB, col[1]);
    }
}

static void layout_block(Ctx *c, Node *n, int x, int w) {
    Node *sv = c->curnd; c->curnd = n;
    layout_block0(c, n, x, w);
    c->curnd = sv;
}

/* ---------- tablas ---------- */
typedef struct { Node *cell; int col, span; } TCell;
typedef struct { Node *row; TCell *cells; int nc; } TRow;

static void cell_measure(Ctx *c, Node *cell) {
    Doc *d = c->doc;
    if (cell->meas_gen == d->meas_gen) return;
    Page *pg = c->pg;
    int sn = pg->n, sy = c->y, sp = c->pend, sx = c->maxx, sm = c->mode, san = pg->nan;
    int padl = Z(cell->st.pl), padr = Z(cell->st.pr);
    c->mode = 1; c->y = 0; c->pend = 0; c->maxx = 0;
    layout_children(c, cell, 0, 100000);
    int pref = c->maxx;
    c->mode = 2; c->y = 0; c->pend = 0; c->maxx = 0;
    layout_children(c, cell, 0, 1);
    int mn = c->maxx;
    c->mode = sm; c->y = sy; c->pend = sp; c->maxx = sx;
    pg->n = sn; pg->nan = san;
    cell->pref_w = pref + padl + padr;
    cell->min_w = mn + padl + padr;
    if (cell->st.width >= 0) {
        int ww = Z(cell->st.width) + padl + padr;
        cell->pref_w = ww > cell->min_w ? ww : cell->min_w;
    }
    if (cell->pref_w < cell->min_w) cell->pref_w = cell->min_w;
    cell->meas_gen = d->meas_gen;
}

static int attr_int(Node *n, const char *name, int def) {
    const char *v = node_attr(n, name);
    return v ? atoi(v) : def;
}

static void layout_table(Ctx *c, Node *t, int x, int w) {
    Style *ts = &t->st;
    int nrows = 0, rcap = 0;
    TRow *rows = NULL;
    Node *caption = NULL;
    for (Node *ch = t->first; ch; ch = ch->next) {
        if (ch->type != 0) continue;
        if (eq(ch->name, "caption")) { caption = ch; continue; }
        Node *list[2] = { ch, NULL };
        if (eq(ch->name, "thead") || eq(ch->name, "tbody") || eq(ch->name, "tfoot")) {
            for (Node *r = ch->first; r; r = r->next) {
                if (r->type == 0 && eq(r->name, "tr")) {
                    if (nrows == rcap) { rcap = rcap ? rcap * 2 : 16; rows = realloc(rows, rcap * sizeof(TRow)); }
                    rows[nrows].row = r; rows[nrows].cells = NULL; rows[nrows].nc = 0; nrows++;
                }
            }
            continue;
        }
        if (eq(list[0]->name, "tr")) {
            if (nrows == rcap) { rcap = rcap ? rcap * 2 : 16; rows = realloc(rows, rcap * sizeof(TRow)); }
            rows[nrows].row = ch; rows[nrows].cells = NULL; rows[nrows].nc = 0; nrows++;
        }
    }
    if (caption && !c->mode) layout_block(c, caption, x, w);
    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        int nc = 0, ccap = 0, col = 0;
        TCell *cells = NULL;
        for (Node *cl = rows[r].row->first; cl; cl = cl->next) {
            if (cl->type != 0 || cl->st.display == D_NONE) continue;
            if (!eq(cl->name, "td") && !eq(cl->name, "th")) continue;
            if (nc == ccap) { ccap = ccap ? ccap * 2 : 8; cells = realloc(cells, ccap * sizeof(TCell)); }
            int span = attr_int(cl, "colspan", 1);
            if (span < 1) span = 1;
            if (span > 50) span = 50;
            cells[nc].cell = cl; cells[nc].col = col; cells[nc].span = span; nc++;
            col += span;
        }
        rows[r].cells = cells; rows[r].nc = nc;
        if (col > ncols) ncols = col;
    }
    if (ncols == 0) { for (int r = 0; r < nrows; r++) free(rows[r].cells); free(rows); return; }

    int *cmin = calloc(ncols + 1, sizeof(int)), *cpref = calloc(ncols + 1, sizeof(int)), *cw = calloc(ncols + 1, sizeof(int));
    float *cpct = calloc(ncols + 1, sizeof(float));
    for (int r = 0; r < nrows; r++)
        for (int k = 0; k < rows[r].nc; k++) {
            TCell *tc = &rows[r].cells[k];
            cell_measure(c, tc->cell);
            if (tc->span == 1) {
                if (tc->cell->min_w > cmin[tc->col]) cmin[tc->col] = tc->cell->min_w;
                if (tc->cell->pref_w > cpref[tc->col]) cpref[tc->col] = tc->cell->pref_w;
                if (tc->cell->st.wpct >= 0 && tc->cell->st.wpct > cpct[tc->col]) cpct[tc->col] = tc->cell->st.wpct;
            }
        }
    for (int r = 0; r < nrows; r++)
        for (int k = 0; k < rows[r].nc; k++) {
            TCell *tc = &rows[r].cells[k];
            if (tc->span == 1) continue;
            int sm = 0, sp = 0;
            for (int j = 0; j < tc->span; j++) { sm += cmin[tc->col + j]; sp += cpref[tc->col + j]; }
            if (sm < tc->cell->min_w) cmin[tc->col + tc->span - 1] += tc->cell->min_w - sm;
            if (sp < tc->cell->pref_w) cpref[tc->col + tc->span - 1] += tc->cell->pref_w - sp;
        }
    for (int i = 0; i < ncols; i++) if (cpref[i] < cmin[i]) cpref[i] = cmin[i];

    int spacing = Z(attr_int(t, "cellspacing", 2));
    int border = attr_int(t, "border", 0);
    if (c->mode) {
        int total = spacing * (ncols + 1);
        for (int i = 0; i < ncols; i++) total += (c->mode == 1 ? cpref[i] : cmin[i]);
        if (x + total > c->maxx) c->maxx = x + total;
        goto done;
    }
    {
        int target = -1;
        if (ts->width >= 0) target = Z(ts->width);
        else if (ts->wpct >= 0) target = (int)(w * ts->wpct / 100);
        if (target > w) target = w;
        int inner = (target >= 0 ? target : w) - spacing * (ncols + 1);
        if (inner < ncols) inner = ncols;
        int fixed = 0, fixedcols = 0;
        for (int i = 0; i < ncols; i++) if (cpct[i] > 0) { cw[i] = (int)(inner * cpct[i] / 100); if (cw[i] < cmin[i]) cw[i] = cmin[i]; fixed += cw[i]; fixedcols++; }
        int pmin = 0, ppref = 0;
        for (int i = 0; i < ncols; i++) if (cpct[i] <= 0) { pmin += cmin[i]; ppref += cpref[i]; }
        int rem = inner - fixed;
        if (fixedcols < ncols) {
            if (rem >= ppref) {
                int extra = rem - ppref;
                for (int i = 0; i < ncols; i++) if (cpct[i] <= 0) {
                    cw[i] = cpref[i];
                    if (target >= 0 && extra > 0) cw[i] += ppref > 0 ? (int)((long)extra * cpref[i] / ppref) : extra / (ncols - fixedcols);
                }
            } else if (rem >= pmin && ppref > pmin) {
                for (int i = 0; i < ncols; i++) if (cpct[i] <= 0) cw[i] = cmin[i] + (int)((long)(cpref[i] - cmin[i]) * (rem - pmin) / (ppref - pmin));
            } else {
                for (int i = 0; i < ncols; i++) if (cpct[i] <= 0) cw[i] = cmin[i];
            }
        }
        int tw = spacing * (ncols + 1);
        for (int i = 0; i < ncols; i++) tw += cw[i];
        int tx = x;
        if ((ts->ml_auto && ts->mr_auto) || ts->align == 1) { if (tw < w) tx = x + (w - tw) / 2; }
        else if (ts->align == 2 && tw < w) tx = x + (w - tw);
        int ty = c->y;
        int tslot = -1;
        if (ts->has_bg) tslot = add_rect(c, tx, ty, tw, 0, ts->bg);
        c->y += spacing;
        for (int r = 0; r < nrows; r++) {
            Node *row = rows[r].row;
            int row_y = c->y;
            int rslot = -1;
            if (row->st.has_bg) rslot = add_rect(c, tx + spacing, row_y, tw - 2 * spacing, 0, row->st.bg);
            int n = rows[r].nc;
            int *cx = malloc(n * sizeof(int)), *cwid = malloc(n * sizeof(int)), *i0 = malloc(n * sizeof(int)), *i1 = malloc(n * sizeof(int));
            int *slot = malloc(n * sizeof(int)), *chh = malloc(n * sizeof(int));
            int rowh = 0;
            for (int k = 0; k < n; k++) {
                TCell *tc = &rows[r].cells[k];
                Node *cell = tc->cell;
                int px0 = tx + spacing;
                for (int j = 0; j < tc->col; j++) px0 += cw[j] + spacing;
                int wd = 0;
                for (int j = 0; j < tc->span; j++) wd += cw[tc->col + j] + (j ? spacing : 0);
                cx[k] = px0; cwid[k] = wd;
                slot[k] = -1;
                if (cell->st.has_bg) slot[k] = add_rect(c, px0, row_y, wd, 0, cell->st.bg);
                int padl = Z(cell->st.pl), padr = Z(cell->st.pr), padt = Z(cell->st.pt), padb = Z(cell->st.pb);
                i0[k] = c->pg->n;
                c->y = row_y + padt; c->pend = 0;
                layout_children(c, cell, px0 + padl, wd - padl - padr);
                int bottom = c->y + c->pend + padb;
                c->pend = 0;
                i1[k] = c->pg->n;
                chh[k] = bottom - row_y;
                if (cell->st.height >= 0 && chh[k] < Z(cell->st.height)) chh[k] = Z(cell->st.height);
                if (chh[k] > rowh) rowh = chh[k];
            }
            for (int k = 0; k < n; k++) {
                Node *cell = rows[r].cells[k].cell;
                const char *va = node_attr(cell, "valign");
                if (!va) va = node_attr(row, "valign");
                int dy = 0;
                if (!va || !strcasecmp(va, "middle") || !strcasecmp(va, "center")) dy = (rowh - chh[k]) / 2;
                else if (!strcasecmp(va, "bottom")) dy = rowh - chh[k];
                if (dy > 0) for (int q = i0[k]; q < i1[k]; q++) c->pg->it[q].y += dy;
                if (slot[k] >= 0) c->pg->it[slot[k]].h = rowh;
                if (border > 0) add_frame(c, cx[k], row_y, cwid[k], rowh, 0x808080);
            }
            if (rslot >= 0) c->pg->it[rslot].h = rowh;
            free(cx); free(cwid); free(i0); free(i1); free(slot); free(chh);
            c->y = row_y + rowh + spacing;
        }
        if (tslot >= 0) c->pg->it[tslot].h = c->y - ty;
        if (border > 0) add_frame(c, tx, ty, tw, c->y - ty, 0x808080);
        c->pend = 0;
        if (tx + tw > c->maxx) c->maxx = tx + tw;
    }
done:
    for (int r = 0; r < nrows; r++) free(rows[r].cells);
    free(rows); free(cmin); free(cpref); free(cw); free(cpct);
}


/* ---------- flex / grid (aproximación: filas con wrap, grow/shrink, justify, align) ---------- */
typedef struct { Node *n; int base, minw, w, h, i0, i1; float grow; int autos; } FItem;

static void flex_measure(Ctx *c, Node *n, FItem *it) {
    Style *s = &n->st;
    cell_measure(c, n);
    int extra = Z(s->ml) + Z(s->mr) + Z(s->bw[1]) + Z(s->bw[3]);
    int pref = n->pref_w + extra, mn = n->min_w + extra;
    if (s->width >= 0) { int ww = Z(s->width) + extra + (s->bsizing ? 0 : Z(s->pl) + Z(s->pr) + Z(s->bw[1]) + Z(s->bw[3])) - (s->bsizing ? Z(s->ml) + Z(s->mr) : 0); pref = ww > mn ? ww : mn; }
    it->n = n; it->grow = s->grow; it->minw = mn;
    it->base = (s->basis >= 0 && !c->mode) ? Z(s->basis) + extra + Z(s->pl) + Z(s->pr) : pref;
    if (it->base < mn && s->basis < 0) it->base = mn;
    it->autos = (s->ml_auto ? 1 : 0) + (s->mr_auto ? 1 : 0);
}

static void layout_flex(Ctx *c, Node *n, int x, int w) {
    Style *s = &n->st;
    if ((!s->isgrid && s->fdir) || (s->isgrid && !s->gridcols && s->gridmin <= 0)) { layout_children(c, n, x, w); return; }
    int cnt = 0, cap = 0;
    FItem *items = NULL;
    for (Node *ch = n->first; ch; ch = ch->next) {
        if (ch->type != 0 || ch->st.display == D_NONE) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 16; items = realloc(items, cap * sizeof(FItem)); }
        memset(&items[cnt], 0, sizeof(FItem));
        flex_measure(c, ch, &items[cnt]);
        cnt++;
    }
    if (!cnt) { free(items); layout_children(c, n, x, w); return; }
    int gap = Z(s->gap), rgap = Z(s->rgap);
    int cols = 0;
    if (s->isgrid) {
        cols = s->gridcols ? s->gridcols : (int)((w + gap) / (Z(s->gridmin) + gap + 1));
        if (cols < 1) cols = 1;
        if (cols > cnt) cols = cnt;
    }
    int i = 0, maxused = 0;
    while (i < cnt) {
        int j = i, total = 0;
        if (s->isgrid) { j = i + cols; if (j > cnt) j = cnt; }
        else if (s->fwrap) {
            total = items[i].base; j = i + 1;
            while (j < cnt && total + gap + items[j].base <= w) { total += gap + items[j].base; j++; }
        } else j = cnt;
        int n_in = j - i;
        total = 0;
        for (int k = i; k < j; k++) total += items[k].base;
        total += gap * (n_in - 1);
        int free_sp = w - total, jx = 0, jgap = gap;
        float G = 0;
        for (int k = i; k < j; k++) { items[k].w = items[k].base; G += items[k].grow; }
        if (s->isgrid) {
            int cwid = (w - gap * (cols - 1)) / cols;
            if (cwid < 1) cwid = 1;
            for (int k = i; k < j; k++) items[k].w = cwid;
            if (n_in < cols) { /* última fila incompleta: mismas columnas */ }
            total = cwid * n_in + gap * (n_in - 1);
        } else if (c->mode == 1) {
            /* medición de ancho preferido: sin crecer ni encoger */
        } else if (free_sp > 0) {
            int autos = 0;
            for (int k = i; k < j; k++) autos += items[k].autos;
            if (G > 0) {
                int left_sp = free_sp;
                for (int k = i; k < j; k++) { int add = (int)(free_sp * items[k].grow / G); items[k].w += add; left_sp -= add; }
                for (int k = i; k < j && left_sp > 0; k++) if (items[k].grow > 0) { items[k].w += left_sp; break; }
                free_sp = 0; total = w;
            } else if (autos > 0) {
                /* los márgenes automáticos absorben el espacio libre */
            } else {
                if (s->just == 1) jx = free_sp / 2;
                else if (s->just == 2) jx = free_sp;
                else if (s->just == 3 && n_in > 1) jgap = gap + free_sp / (n_in - 1);
                else if (s->just == 4) { jgap = gap + free_sp / n_in; jx = free_sp / n_in / 2; }
            }
        } else if (free_sp < 0) {
            int shrinkable = 0;
            for (int k = i; k < j; k++) if (items[k].base > items[k].minw) shrinkable += items[k].base - items[k].minw;
            int excess = -free_sp;
            if (shrinkable > 0) {
                int remaining = excess;
                for (int k = i; k < j; k++) {
                    int sh = items[k].base > items[k].minw ? items[k].base - items[k].minw : 0;
                    int cut = (int)((long)excess * sh / shrinkable);
                    if (cut > sh) cut = sh;
                    items[k].w -= cut; remaining -= cut;
                }
                (void)remaining;
            }
        }
        if (!s->isgrid && c->mode != 1) for (int k = i; k < j; k++) if (items[k].w < items[k].minw) items[k].w = items[k].minw;
        /* colocar */
        int line_y = c->y + c->pend;
        c->pend = 0;
        int px = x + jx, line_h = 0, autos_total = 0;
        for (int k = i; k < j; k++) autos_total += items[k].autos;
        int auto_share = (!s->isgrid && c->mode != 1 && free_sp > 0 && autos_total > 0 && G == 0) ? free_sp / autos_total : 0;
        int line_n0 = c->pg->n, line_an0 = c->pg->nan, px0 = px, pass = 0;
        for (;;) {
        px = px0; line_h = pass ? line_h : 0;
        for (int k = i; k < j; k++) {
            Node *it = items[k].n; Style *is = &it->st;
            Style save = *is;
            int iw = items[k].w;
            if (auto_share && is->ml_auto) px += auto_share;
            if (pass) is->minh = (float)(line_h - Z(is->mt) - Z(is->mb)) / c->zoom;
            is->width = is->wpct = is->maxw = is->maxwpct = -1;
            is->ml_auto = is->mr_auto = 0;
            if (s->isgrid) is->bsizing = 0;
            c->y = line_y; c->pend = 0;
            items[k].i0 = c->pg->n;
            layout_block(c, it, px, iw < 1 ? 1 : iw);
            items[k].i1 = c->pg->n;
            items[k].h = c->y + c->pend - line_y;
            c->pend = 0;
            if (!pass && items[k].h > line_h) line_h = items[k].h;
            *is = save;
            if (px + iw > maxused) maxused = px + iw;
            px += iw + jgap;
            if (auto_share && is->mr_auto) px += auto_share;
        }
        if (pass) break;
        int need = 0;
        if (!c->mode && s->alignit == 0 && n_in > 1) {
            for (int k = i; k < j; k++) {
                Style *is = &items[k].n->st;
                if (items[k].h < line_h && (is->has_bg || is->bw[0] > 0 || is->bw[1] > 0 || is->bw[2] > 0 || is->bw[3] > 0)) need = 1;
            }
        }
        if (!need) break;
        c->pg->n = line_n0; c->pg->nan = line_an0;
        pass = 1;
        }
        if (!c->mode && (s->alignit == 1 || s->alignit == 2)) {
            for (int k = i; k < j; k++) {
                int dy = s->alignit == 1 ? (line_h - items[k].h) / 2 : line_h - items[k].h;
                if (dy > 0) for (int q = items[k].i0; q < items[k].i1; q++) c->pg->it[q].y += dy;
            }
        }
        c->y = line_y + line_h + (j < cnt ? rgap : 0);
        c->pend = 0;
        i = j;
    }
    if (maxused > c->maxx) c->maxx = maxused;
    free(items);
}

/* ---------- API ---------- */
void page_clear(Page *p) {
    p->n = 0; p->nan = 0; p->height = 0;
    arena_free(&p->arena);
}

void layout_doc(Doc *d, Page *pg, int vw, float zoom, int reader) {
    page_clear(pg);
    Ctx C;
    memset(&C, 0, sizeof C);
    C.pg = pg; C.doc = d; C.zoom = zoom;
    d->meas_gen++;
    Node *m = reader ? doc_find_main(d) : NULL;
    if (m) {
        int colw = (int)(780 * zoom);
        if (colw > vw - 32) colw = vw - 32;
        if (colw < 100) colw = vw;
        int x = (vw - colw) / 2;
        C.y = (int)(12 * zoom);
        if (d->h1_node) { d->h1_node->st.display = D_BLOCK; layout_block(&C, d->h1_node, x, colw); }
        m->st.display = D_BLOCK;
        layout_block(&C, m, x, colw);
        C.pend += (int)(24 * zoom);
    } else layout_children(&C, d->root, 0, vw);
    pg->height = C.y + C.pend;
}

Item *page_hit(Page *p, int x, int y) {
    for (int i = p->n - 1; i >= 0; i--) {
        Item *t = &p->it[i];
        if (!t->href) continue;
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) return t;
    }
    return NULL;
}

Item *page_hit_node(Page *p, int x, int y) {
    for (int i = p->n - 1; i >= 0; i--) {
        Item *t = &p->it[i];
        if (!t->nd || t->kind == IT_FRAME) continue;
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h) return t;
    }
    return NULL;
}

int page_anchor(Page *p, const char *id) {
    for (int i = 0; i < p->nan; i++) if (!strcmp(p->an[i].id, id)) return p->an[i].y;
    return -1;
}
