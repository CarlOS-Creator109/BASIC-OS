/* Cosmos Browser - cabecera común (COS-Mods) */
#ifndef COSMOS_H
#define COSMOS_H
#include <stdint.h>
#include <stddef.h>

#define COSMOS_VERSION "0.5"

/* ================= arena ================= */
typedef struct ABlk ABlk;
typedef struct { ABlk *head; } Arena;
void *arena_alloc(Arena *a, size_t n);              /* memoria en cero */
char *arena_strndup(Arena *a, const char *s, size_t n);
void arena_free(Arena *a);

/* ================= gfx ================= */
enum { FONT_SANS, FONT_BOLD, FONT_ITALIC, FONT_MONO, FONT_MONOBOLD, NFONTS };

typedef struct { int w, h; uint32_t *px; int cx0, cy0, cx1, cy1; } Fb;   /* 0xRRGGBB */
typedef struct { int w, h; uint32_t *px; } Image;                        /* 0xAARRGGBB */

int  gfx_init(const char *extra_font_dir);
int  text_w(int font, int px, const char *s, int len);
int  font_asc(int font, int px);
int  font_desc(int font, int px);
int  utf8_next(const char **p, const char *end);       /* devuelve codepoint */
int  utf8_put(char *out, int cp);

void fb_init(Fb *f, int w, int h);
void fb_resize(Fb *f, int w, int h);
void fb_clip(Fb *f, int x, int y, int w, int h);
void fb_noclip(Fb *f);
void fb_rect(Fb *f, int x, int y, int w, int h, uint32_t c);
void fb_frame(Fb *f, int x, int y, int w, int h, uint32_t c);
void fb_rrect(Fb *f, int x, int y, int w, int h, int r, uint32_t c);
int  fb_text(Fb *f, int x, int y, int font, int px, uint32_t c, const char *s, int len); /* y = tope del renglón */
void fb_image(Fb *f, Image *im, int x, int y, int w, int h);
Image *image_decode(const uint8_t *data, size_t len);
void image_free(Image *im);

/* ================= red / archivos ================= */
typedef struct {
    uint8_t *data; size_t len;
    char final_url[2048]; char ctype[128]; int status; char err[160];
} Resp;

uint8_t *read_file(const char *path, size_t *len);
int  fetch_url(const char *url, Resp *r, size_t maxlen, int timeout_s);   /* 0 = ok */
void resp_free(Resp *r);
void url_resolve(const char *base, const char *ref, char *out, size_t cap);
void omnibox_to_url(const char *in, char *out, size_t cap);
const char *builtin_page(const char *name);
extern char cosmos_dir[1024];                                             /* carpeta del ejecutable (fonts/, logo.png) */                               /* pages.c */

int  run_capture(char *const argv[], const char *errfile, char **out, volatile int *pid);  /* stdout completo en *out; devuelve código de salida */
int  is_video_url(const char *url, int *direct);                          /* YouTube o archivo de video directo */

/* ================= reproductor de video (yt-dlp + ffmpeg) ================= */
enum { PL_RESOLVING, PL_PLAYING, PL_PAUSED, PL_ENDED, PL_ERROR };
typedef struct Player Player;
extern void (*player_audio_sink)(const void *data, int bytes);   /* PCM s16 estéreo 44100 Hz */
extern void (*player_audio_clear)(void);
Player *player_open(const char *url, int direct, int w, int h);
void player_close(Player *p);
void player_toggle_pause(Player *p);
void player_seek(Player *p, double delta);
void player_seek_abs(Player *p, double t);
void player_volume(Player *p, int delta);
void player_mute(Player *p);
int  player_state(Player *p);
double player_pos(Player *p);
double player_dur(Player *p);
int  player_vol(Player *p);                                     /* 0..100, -1 si silenciado */
void player_info(Player *p, char *title, int tcap, char *err, int ecap);
int  player_new_frame(Player *p);                              /* 1 si hay cuadro nuevo desde la última vez */
Image *player_lock_frame(Player *p);                           /* NULL si no hay; hay que llamar unlock */
void player_unlock_frame(Player *p);
int  player_hw(Player *p);                                      /* 0 software, 1 VA-API copia, 2 VA-API GPU */
int  player_quality(Player *p);                                 /* alto en px: 144..1080 */
void player_set_quality(Player *p, int dir);                    /* -1 baja, +1 sube; reinicia en la misma posición */

/* ================= motor HTML ================= */
enum { D_INLINE, D_BLOCK, D_NONE, D_LISTITEM, D_TABLE };

typedef struct Style {
    uint32_t color, bg;
    float size;
    float mt, mb, ml, mr, pt, pb, pl, pr;
    float width, maxw, wpct, maxwpct, height;       /* <0 = no definido */
    const char *href;
    uint8_t has_bg, bold, italic, mono, ul, strike, pre, nowrap, align, display;
    uint8_t ml_auto, mr_auto, list_none, ctag;
    float lh, radius, minh, gap, rgap, grow, basis, gridmin;
    float bw[4];                                     /* bordes: arriba, derecha, abajo, izquierda */
    uint32_t bc[4], ibgc;
    struct Node *nd;                                 /* elemento dueño (para clics de JS) */
    uint8_t bcmask, ttrans, bsizing, ibg, fdir, fwrap, just, alignit, flt, gridcols, isflex, isgrid, clearf, masked;
} Style;

typedef struct Attr { const char *name, *val; } Attr;

typedef struct Node {
    uint8_t type;                 /* 0 elemento, 1 texto */
    const char *name;
    char *text; int tlen;
    Attr *attrs; int nattrs;
    struct Node *parent, *first, *last, *next;
    Style st;
    Image *img; uint8_t img_state; const char *img_url;
    int pref_w, min_w, meas_gen;
    int rscore;
    void *js;                     /* envoltorio JS (JSValue*), cacheado */
} Node;

typedef struct Doc {
    Arena arena;
    Node *root;
    char *title, *base;
    char *css; size_t csslen;
    void *rules; int nrules;
    void *vars; int nvars, cvars;
    uint32_t canvas_bg;
    int refs;
    Node **imgs; int nimgs;
    int meas_gen;
    Node *main_node, *h1_node; int main_done;
    void *js; int dom_gen;        /* contexto QuickJS y contador de cambios del DOM */
} Doc;

extern int css_viewport_w, css_viewport_h;                 /* para @media y unidades vw/vh */
Doc *html_parse(const char *src, size_t n, const char *url);
void doc_style(Doc *d);
void doc_prepend_css(Doc *d, const char *css, size_t n);   /* hojas externas van antes de las <style> */
void doc_collect_images(Doc *d);
void doc_free(Doc *d);
Node *doc_find_main(Doc *d);                           /* contenido principal (modo lectura) */
const char *node_attr(Node *n, const char *name);
void html_parse_fragment(Doc *d, Node *ctx, const char *s, size_t n);   /* innerHTML */
int  dom_select(Node *scope, const char *sel, Node **out, int max, int first);   /* querySelector(All) */
int  dom_matches(Node *n, const char *sel);
char *node_serialize(Node *n, int outer);                               /* malloc; innerHTML / outerHTML */

/* ================= JavaScript (QuickJS) ================= */
void js_run_page(Doc *d, int local, int vw, int vh, const char *pageurl);   /* scripts de la página + ciclo de carga */
void doc_js_free(Doc *d);
int  js_dispatch(Doc *d, Node *target, const char *type);                  /* 1 si preventDefault */
int  js_tick(Doc *d, int ms);                                              /* avanza temporizadores; 1 si el DOM cambió */
int  js_has_timers(Doc *d);
void js_sync_styles(Doc *d);                                               /* <style> creados por JS -> d->css */
char *js_take_nav(Doc *d);                                                 /* location.href = ... pendiente (malloc) */
void doc_append_css(Doc *d, const char *css, size_t n);

/* ================= layout ================= */
enum { IT_TEXT, IT_RECT, IT_FRAME, IT_IMG };

typedef struct {
    uint8_t kind, ul, st, font, rad;
    int x, y, w, h, px;
    uint32_t color;
    const char *s; int len;
    const char *href;
    Image *img;
    Node *nd;
} Item;

typedef struct { const char *id; int y; } Anchor;

typedef struct Page {
    Item *it; int n, cap;
    int height;
    Anchor *an; int nan, can;
    Arena arena;
} Page;

void layout_doc(Doc *d, Page *p, int vw, float zoom, int reader);
void page_clear(Page *p);
Item *page_hit(Page *p, int x, int y);                  /* ítem con href bajo (x,y) de página */
Item *page_hit_node(Page *p, int x, int y);              /* ítem con nodo bajo (x,y), para eventos JS */
int  page_anchor(Page *p, const char *id);              /* y del ancla o -1 */

#endif
