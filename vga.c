/* vga.c - consola de texto de PC BASIC
 *
 * Dos motores con la misma interfaz:
 *   - grafico: framebuffer lineal VBE (la resolucion mas alta que soporte el monitor) dibujando la fuente VGA
 *     8x16 que stage2 copio de la BIOS. Borde de 16 px alrededor del area de texto.
 *   - texto: VGA 80x25 si la BIOS no ofrece un modo VBE usable.
 *
 * Los colores son los 16 del C64 (por defecto: texto blanco, fondo azul, borde azul
 * claro). Entiende los codigos de control de PETSCII mas usados (CHR$(147) = limpiar,
 * colores, cursor, reverso).
 *
 * La pantalla se guarda en `cells` (RAM) y solo se redibujan las celdas que cambian:
 * leer el framebuffer es muy lento, escribirlo (write-combining) es rapido.
 */
#include "kernel.h"

#define FONT     ((const u8 *)0x6800)
#define VGA_TEXT ((volatile u16 *)0xB8000)
#define MAXC     256
#define MAXR     100
#define BORDER   16

/* color del C64 (0-15) -> color de texto VGA (solo para el modo de texto) */
static const u8 c64vga[16] = { 0, 15, 4, 3, 5, 2, 1, 14, 6, 6, 12, 8, 7, 10, 9, 7 };

/* paleta del C64 en RGB */
static const u8 c64rgb[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0xFF, 0xFF, 0xFF }, { 0x68, 0x37, 0x2B }, { 0x70, 0xA4, 0xB2 },
    { 0x6F, 0x3D, 0x86 }, { 0x58, 0x8D, 0x43 }, { 0x35, 0x28, 0x79 }, { 0xB8, 0xC7, 0x6F },
    { 0x6F, 0x4F, 0x25 }, { 0x43, 0x39, 0x00 }, { 0x9A, 0x67, 0x59 }, { 0x44, 0x44, 0x44 },
    { 0x6C, 0x6C, 0x6C }, { 0x9A, 0xD2, 0x84 }, { 0x6C, 0x5E, 0xB5 }, { 0x95, 0x95, 0x95 }
};

static int gfx;                          /* 1 = framebuffer, 0 = texto 80x25 */
static int x0, y0;                       /* origen de la cuadricula en pixeles */
static u32 scr_w, scr_h, pitch, bpp;
static int scale = 1;                    /* tamano de pixel logico (2 en pantallas >= 1600) */
static int gdirty;                       /* hay pixeles de PLOT/LINE sobre las celdas */
static volatile u8 *fb;
static u32 pal[16];
static u8 rs, rp, gs, gp, bs, bp;

/* Una consola = cuadricula de texto + color + cursor. La 0 es la del kernel; las 1 y 2 son las
 * de las ventanas de terminal del escritorio. Con ptxt != NULL la consola dibuja en buffers de
 * indices de color (modo escritorio) en vez de escribir directo en el framebuffer. */
struct con {
    u16 ccells[MAXR * MAXC];             /* ch | fg << 8 | bg << 12 */
    u16 cshown[MAXR * MAXC];             /* lo que hay dibujado ahora */
    int ncol, nrow;                      /* tamano de la cuadricula de texto */
    int crow, ccol;                      /* ccol == ncol: salto de linea pendiente */
    int cfg, cbg, crev;
    u8 *ptxt, *pgfx;                     /* plano de texto y plano de PLOT */
    int stride;
    con_dirty_fn dirty;
    void *user;
    int used;
};
#define NCON 3
static struct con con_all[NCON];
static struct con *C = &con_all[0];
#define cells (C->ccells)
#define shown (C->cshown)
#define cols  (C->ncol)
#define rows  (C->nrow)
#define row   (C->crow)
#define col   (C->ccol)
#define fg_c  (C->cfg)
#define bg_c  (C->cbg)
#define rev   (C->crev)

static void mouse_hide(void);
static void sel_clear(void);
static int mx, my, mbtn, mshow, mr, mc, mused;
static int gui;                          /* 1 = escritorio: el cursor lo dibuja el modulo */
static int sel_on, sel_a, sel_b, hl_lo = -1, hl_hi = -1;
static u8 clip[1024];
static int clip_n;

static int border_c = 14;

/* ---- pixeles ---- */

static u32 rgb(u8 r, u8 g, u8 b)
{
    return ((u32)(r >> (8 - rs)) << rp) | ((u32)(g >> (8 - gs)) << gp) |
           ((u32)(b >> (8 - bs)) << bp);
}

static void px_put(volatile u8 *p, u32 v)
{
    if (bpp == 32) {
        *(volatile u32 *)p = v;
    } else if (bpp == 24) {
        p[0] = (u8)v;
        p[1] = (u8)(v >> 8);
        p[2] = (u8)(v >> 16);
    } else {
        *(volatile u16 *)p = (u16)v;
    }
}

static void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 v)
{
    u32 i, j, bytes = bpp / 8;
    for (j = 0; j < h; j++) {
        volatile u8 *p = fb + (y + j) * pitch + x * bytes;
        for (i = 0; i < w; i++, p += bytes)
            px_put(p, v);
    }
}

/* ---- dibujo de celdas ---- */

/* Celda en un plano de indices (modo escritorio): 8x16 pixeles logicos */
static void draw_plane(int r, int c, u16 v)
{
    const u8 *g = FONT + (v & 0xFF) * 16;
    u8 f = (u8)((v >> 8) & 15), b = (u8)((v >> 12) & 15);
    u8 *d = C->ptxt + (r * 16) * C->stride + c * 8;
    int i, j;
    for (i = 0; i < 16; i++, d += C->stride)
        for (j = 0; j < 8; j++)
            d[j] = (g[i] & (0x80 >> j)) ? f : b;
    if (C->dirty)
        C->dirty(C->user, c * 8, r * 16, 8, 16);
}

static int splash_on, splash_init;

static void draw_value(int r, int c, u16 v)
{
    u8 ch = (u8)(v & 0xFF);

    if (splash_on)
        return;
    int f = (v >> 8) & 15, b = (v >> 12) & 15;

    if (C->ptxt) {
        draw_plane(r, c, v);
        return;
    }
    if (!gfx) {
        VGA_TEXT[r * 80 + c] = (u16)((((c64vga[b] << 4) | c64vga[f]) << 8) | ch);
        return;
    }

    const u8 *g = FONT + ch * 16;
    int i;
    volatile u8 *dst;

    if (scale > 1) {
        u32 bytes = bpp / 8;
        int j, dy, dx;
        dst = fb + (u32)(y0 + r * 16 * scale) * pitch + (u32)(x0 + c * 8 * scale) * bytes;
        for (i = 0; i < 16; i++) {
            for (dy = 0; dy < scale; dy++, dst += pitch)
                for (j = 0; j < 8; j++) {
                    u32 pv = (g[i] & (0x80 >> j)) ? pal[f] : pal[b];
                    for (dx = 0; dx < scale; dx++)
                        px_put(dst + (u32)(j * scale + dx) * bytes, pv);
                }
        }
        return;
    }
    dst = fb + (u32)(y0 + r * 16) * pitch + (u32)(x0 + c * 8) * (bpp / 8);

    if (bpp == 32) {
        u32 pf = pal[f], pb = pal[b];
        u64 pr[4];
        pr[0] = pb | ((u64)pb << 32);
        pr[1] = pb | ((u64)pf << 32);
        pr[2] = pf | ((u64)pb << 32);
        pr[3] = pf | ((u64)pf << 32);
        for (i = 0; i < 16; i++, dst += pitch) {
            u8 bits = g[i];
            volatile u64 *d = (volatile u64 *)dst;
            d[0] = pr[bits >> 6];
            d[1] = pr[(bits >> 4) & 3];
            d[2] = pr[(bits >> 2) & 3];
            d[3] = pr[bits & 3];
        }
    } else {
        u32 bytes = bpp / 8;
        for (i = 0; i < 16; i++, dst += pitch) {
            u8 bits = g[i];
            int j;
            for (j = 0; j < 8; j++)
                px_put(dst + j * bytes, (bits & (0x80 >> j)) ? pal[f] : pal[b]);
        }
    }
}

static void refresh(void)
{
    int r, c;
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) {
            int i = r * MAXC + c;
            if (cells[i] != shown[i]) {
                draw_value(r, c, cells[i]);
                shown[i] = cells[i];
            }
        }
}

static u16 blank_cell(void)
{
    return (u16)(' ' | (fg_c << 8) | (bg_c << 12));
}

static void put_cell(int r, int c, u16 v)
{
    int i = r * MAXC + c;
    cells[i] = v;
    if (shown[i] != v) {
        draw_value(r, c, v);
        shown[i] = v;
    }
}

static void paint_border(void)
{
    int i;
    if (!gfx)
        return;
    if (gui) {                           /* con el escritorio solo se repinta el marco, no lo de dentro */
        int tw = con_all[0].ncol * 8 * scale, th = con_all[0].nrow * 16 * scale;
        fill_rect(0, 0, scr_w, (u32)y0, pal[border_c]);
        fill_rect(0, (u32)(y0 + th), scr_w, scr_h - (u32)(y0 + th), pal[border_c]);
        fill_rect(0, (u32)y0, (u32)x0, (u32)th, pal[border_c]);
        fill_rect((u32)(x0 + tw), (u32)y0, scr_w - (u32)(x0 + tw), (u32)th, pal[border_c]);
        return;
    }
    fill_rect(0, 0, scr_w, scr_h, pal[border_c]);
    for (i = 0; i < MAXR * MAXC; i++)
        shown[i] = 0xFFFF;
}

static void repaint_all(void)
{
    int i;
    for (i = 0; i < MAXR * MAXC; i++)
        shown[i] = 0xFFFF;
    gdirty = 0;
}

static void scroll(void)
{
    int r, c;
    if (gdirty)
        repaint_all();
    for (r = 0; r < rows - 1; r++)
        for (c = 0; c < cols; c++)
            cells[r * MAXC + c] = cells[(r + 1) * MAXC + c];
    for (c = 0; c < cols; c++)
        cells[(rows - 1) * MAXC + c] = blank_cell();
    row = rows - 1;
    refresh();
}

static void plane_clear_gfx(void)
{
    int y, w = cols * 8, h = rows * 16;
    for (y = 0; y < h; y++)
        memset(C->pgfx + y * C->stride, 255, (size_t)w);
    if (C->dirty)
        C->dirty(C->user, 0, 0, w, h);
}

void vga_clear(void)
{
    int r, c;
    if (!C->ptxt) {
        mouse_hide();
        sel_clear();
    } else if (C->pgfx) {
        plane_clear_gfx();
    }
    if (gdirty)
        repaint_all();
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            cells[r * MAXC + c] = blank_cell();
    row = col = 0;
    refresh();
}


/* ---- pantalla de carga "BOS. BASIC OS": gabinete de torre de PC beige amarillento ---- */

static void sp_rect(int x, int y, int w, int h, int r, int g, int b)
{
    if (w <= 0 || h <= 0)
        return;
    fill_rect((u32)x, (u32)y, (u32)w, (u32)h, rgb((u8)r, (u8)g, (u8)b));
}

static void sp_screw(int cx, int cy, int rad, int cross)
{
    int x, y;
    for (y = -rad; y <= rad; y++)
        for (x = -rad; x <= rad; x++) {
            int d = x * x + y * y;
            if (d > rad * rad)
                continue;
            if (d > (rad - 1) * (rad - 1))
                sp_rect(cx + x, cy + y, 1, 1, 90, 82, 60);                 /* aro oscuro */
            else if (x + y < -rad / 2)
                sp_rect(cx + x, cy + y, 1, 1, 214, 214, 218);              /* brillo */
            else
                sp_rect(cx + x, cy + y, 1, 1, 160, 160, 166);
        }
    sp_rect(cx - rad + 2, cy - 1, 2 * rad - 3, 2, 60, 56, 50);              /* ranura */
    if (cross)
        sp_rect(cx - 1, cy - rad + 2, 2, 2 * rad - 3, 60, 56, 50);
}

static void sp_plate(int x, int y, int w, int h, int u)
{
    sp_rect(x, y, w, h, 96, 96, 102);
    sp_rect(x + u, y + u, w - 2 * u, h - 2 * u, 168, 168, 174);
    sp_rect(x + u, y + u, w - 2 * u, u, 214, 214, 220);
    sp_rect(x + u, y + h - 2 * u, w - 2 * u, u, 120, 120, 126);
}

static void sp_text(int x, int y, const char *t, int s, int r, int g, int b)
{
    for (; *t; t++, x += 8 * s) {
        const u8 *gl = FONT + (u8)*t * 16;
        int i, j;
        for (i = 0; i < 16; i++)
            for (j = 0; j < 8; j++)
                if (gl[i] & (0x80 >> j))
                    sp_rect(x + j * s, y + i * s, s, s, r, g, b);
    }
}

static int sp_len(const char *t) { int n = 0; while (t[n]) n++; return n; }

void vga_splash(int pct, const char *msg)
{
    int W = (int)scr_w, H = (int)scr_h;
    int u = H / 360 < 1 ? 1 : H / 360;
    int bw = W * 6 / 10, bh = 14 * u, bx = (W - bw) / 2, by = H * 62 / 100;
    int i, n, ts = 3 * u, tw, px, py;

    if (!gfx)
        return;
    splash_on = 1;
    if (!splash_init) {
        splash_init = 1;
        sp_rect(0, 0, W, H, 218, 208, 164);                                  /* chapa beige */
        sp_rect(0, 0, W, 5 * u, 238, 230, 192);                              /* biseles del marco */
        sp_rect(0, 0, 5 * u, H, 238, 230, 192);
        sp_rect(0, H - 5 * u, W, 5 * u, 160, 150, 108);
        sp_rect(W - 5 * u, 0, 5 * u, H, 160, 150, 108);
        sp_rect(8 * u, 8 * u, W - 16 * u, u, 176, 166, 124);                 /* panel interior */
        sp_rect(8 * u, H - 9 * u, W - 16 * u, u, 176, 166, 124);
        sp_rect(8 * u, 8 * u, u, H - 16 * u, 176, 166, 124);
        sp_rect(W - 9 * u, 8 * u, u, H - 16 * u, 176, 166, 124);

        sp_screw(16 * u, 16 * u, 5 * u, 1);                                  /* tornillos de esquina */
        sp_screw(W - 16 * u, 16 * u, 5 * u, 0);
        sp_screw(16 * u, H - 16 * u, 5 * u, 0);
        sp_screw(W - 16 * u, H - 16 * u, 5 * u, 1);

        for (i = 0; i < 9; i++)                                              /* rejilla de ventilacion */
            sp_rect(W / 2 - 40 * u, 14 * u + i * 4 * u, 80 * u, 2 * u, 120, 110, 80);

        sp_plate(10 * u, H / 2 - 30 * u, 14 * u, 60 * u, u);                 /* chapas laterales */
        sp_screw(17 * u, H / 2 - 20 * u, 3 * u, 1);
        sp_screw(17 * u, H / 2 + 20 * u, 3 * u, 0);
        sp_plate(W - 24 * u, H / 2 - 30 * u, 14 * u, 60 * u, u);
        sp_screw(W - 17 * u, H / 2 - 20 * u, 3 * u, 0);
        sp_screw(W - 17 * u, H / 2 + 20 * u, 3 * u, 1);

        sp_rect(W - 90 * u, H - 40 * u, 66 * u, 20 * u, 150, 140, 100);      /* bahia de disquete */
        sp_rect(W - 88 * u, H - 38 * u, 62 * u, 16 * u, 200, 190, 148);
        sp_rect(W - 82 * u, H - 32 * u, 40 * u, 3 * u, 40, 36, 30);
        sp_rect(W - 34 * u, H - 32 * u, 4 * u, 3 * u, 60, 200, 60);           /* LED */
        sp_plate(24 * u, H - 36 * u, 40 * u, 12 * u, u);                     /* chapa con remaches */
        sp_screw(30 * u, H - 30 * u, 2 * u, 0);
        sp_screw(58 * u, H - 30 * u, 2 * u, 1);

        tw = 13 * 8 * ts;                                                    /* titulo */
        px = (W - tw) / 2;
        py = H * 28 / 100;
        sp_text(px + u * 2, py + u * 2, "BOS. BASIC OS", ts, 246, 240, 214);
        sp_text(px, py, "BOS. BASIC OS", ts, 74, 58, 36);
        n = sp_len("PC BASIC V1.2 (64-BIT)");
        sp_text((W - n * 8 * u) / 2, py + 16 * ts + 6 * u, "PC BASIC V1.2 (64-BIT)", u, 110, 96, 64);

        sp_plate(bx - 16 * u, by - 8 * u, bw + 32 * u, bh + 16 * u, u);      /* marco de la barra */
        sp_screw(bx - 8 * u, by + bh / 2, 3 * u, 1);
        sp_screw(bx + bw + 8 * u, by + bh / 2, 3 * u, 0);
        sp_rect(bx - 2 * u, by - 2 * u, bw + 4 * u, bh + 4 * u, 70, 62, 44);
        sp_rect(bx, by, bw, bh, 236, 230, 204);
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    n = (bw - 2 * u) / (8 * u);                                              /* segmentos */
    for (i = 0; i < n * pct / 100; i++)
        sp_rect(bx + u + i * 8 * u, by + 2 * u, 6 * u, bh - 4 * u, 52, 64, 96);
    if (msg) {
        char num[8];
        int k = 0, v = pct;
        sp_rect(bx - 16 * u, by + bh + 12 * u, bw + 32 * u, 18 * u, 218, 208, 164);
        sp_text(bx, by + bh + 14 * u, msg, u, 90, 76, 48);
        if (v >= 100) num[k++] = '1', num[k++] = '0', num[k++] = '0';
        else {
            if (v >= 10) num[k++] = (char)('0' + v / 10);
            num[k++] = (char)('0' + v % 10);
        }
        num[k++] = '%'; num[k] = 0;
        sp_text(bx + bw - k * 8 * u, by + bh + 14 * u, num, u, 90, 76, 48);
    }
}

void vga_splash_end(void)
{
    if (!splash_on)
        return;
    splash_on = 0;
    splash_init = 0;
    paint_border();
    repaint_all();
    refresh();
}

/* modo "video de texto" para modulos de pantalla completa (YAAI):
 * op 0 = preparar (fondo blanco, texto negro) y devuelve cols | filas << 16
 * op 1 = pintar un cuadro de w x h caracteres centrado; op 2 = restaurar */
static int vid_fg, vid_bg, vid_bd;

int vga_vid(int op, const u8 *chars, int w, int h)
{
    int r, c, ox, oy;
    if (gui)
        return -1;
    if (op == 0) {
        vid_fg = fg_c; vid_bg = bg_c; vid_bd = border_c;
        vga_cursor_show(0);
        vga_set_fg(0);
        vga_set_bg(1);
        vga_set_border(1);
        vga_clear();
        return cols | (rows << 16);
    }
    if (op == 2) {
        vga_set_fg(vid_fg);
        vga_set_bg(vid_bg);
        vga_set_border(vid_bd);
        vga_clear();
        vga_cursor_show(1);
        return 0;
    }
    if (w > cols || h > rows)
        return -1;
    ox = (cols - w) / 2;
    oy = (rows - h) / 2;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            cells[(oy + r) * MAXC + ox + c] = (u16)(chars[r * w + c] | (0 << 8) | (1 << 12));
    refresh();
    return 0;
}

void vga_init(void)
{
    const struct bootinfo *bi = BOOTINFO;
    int i;

    C = &con_all[0];
    C->ptxt = C->pgfx = NULL;
    C->used = 1;
    fg_c = 1;                            /* texto blanco */
    bg_c = 6;                            /* fondo azul */
    border_c = 14;                       /* borde azul claro */
    rev = 0;
    scale = 1;
    gdirty = 0;
    gfx = bi->gfx && bi->fb && (bi->bpp == 32 || bi->bpp == 24 || bi->bpp == 16);

    if (gfx) {
        scr_w = bi->w;
        scr_h = bi->h;
        pitch = bi->pitch;
        bpp = bi->bpp;
        fb = (volatile u8 *)(u64)bi->fb;
        rs = bi->rs; rp = bi->rp; gs = bi->gs; gp = bi->gp; bs = bi->bs; bp = bi->bp;
        if (!rs && !gs && !bs) {         /* BIOS sin informacion de mascaras */
            if (bpp == 16) { rs = 5; rp = 11; gs = 6; gp = 5; bs = 5; bp = 0; }
            else           { rs = 8; rp = 16; gs = 8; gp = 8; bs = 8; bp = 0; }
        }
        for (i = 0; i < 16; i++)
            pal[i] = rgb(c64rgb[i][0], c64rgb[i][1], c64rgb[i][2]);
        scale = scr_w >= 2880 ? 3 : (scr_w >= 1600 ? 2 : 1);
        cols = (int)(scr_w - 2 * BORDER * scale) / (8 * scale);
        rows = (int)(scr_h - 2 * BORDER * scale) / (16 * scale);
        if (cols > MAXC) cols = MAXC;
        if (rows > MAXR) rows = MAXR;
        x0 = (((int)scr_w - cols * 8 * scale) / 2) & ~1;
        y0 = ((int)scr_h - rows * 16 * scale) / 2;
    } else {
        cols = 80;
        rows = 25;
        x0 = y0 = 0;
        outb(0x3D4, 0x0A);               /* ocultar el cursor de hardware */
        outb(0x3D5, 0x20);
    }

    for (i = 0; i < MAXR * MAXC; i++)
        shown[i] = 0xFFFF;
    mshow = 0;
    hl_lo = -1;
    sel_on = 0;
    mx = cols * 4;
    my = rows * 8;
    paint_border();
    vga_clear();
}

int vga_cols(void)  { return cols; }
int vga_rows(void)  { return rows; }
int vga_is_gfx(void) { return gfx; }
int vga_width(void)  { return gfx ? (int)scr_w : 0; }
int vga_height(void) { return gfx ? (int)scr_h : 0; }
int vga_bpp(void)    { return gfx ? (int)bpp : 0; }

/* ---- graficos: PLOT / LINE (coordenadas logicas dentro del area de texto) ---- */

int vga_main_w(void) { return gfx ? con_all[0].ncol * 8 : 0; }
int vga_main_h(void) { return gfx ? con_all[0].nrow * 16 : 0; }
int vga_gfx_w(void) { return gfx ? cols * 8 : 0; }
int vga_gfx_h(void) { return gfx ? rows * 16 : 0; }

/* rectangulo en el plano de PLOT de la consola actual (modo escritorio) */
static void plane_rect(int x, int y, int w, int h, int c)
{
    int j, maxx = cols * 8, maxy = rows * 16;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > maxx) w = maxx - x;
    if (y + h > maxy) h = maxy - y;
    if (w <= 0 || h <= 0)
        return;
    for (j = 0; j < h; j++)
        memset(C->pgfx + (y + j) * C->stride + x, c, (size_t)w);
    if (C->dirty)
        C->dirty(C->user, x, y, w, h);
}

void vga_plot(int x, int y, int c)
{
    if (!gfx || x < 0 || y < 0 || x >= cols * 8 || y >= rows * 16)
        return;
    if (C->pgfx) {
        plane_rect(x, y, 1, 1, (c < 0 ? fg_c : c) & 15);
        return;
    }
    fill_rect((u32)(x0 + x * scale), (u32)(y0 + y * scale), (u32)scale, (u32)scale,
              pal[(c < 0 ? fg_c : c) & 15]);
    gdirty = 1;
}

void vga_hline(int x1, int x2, int y, int c)
{
    int t;
    if (!gfx || y < 0 || y >= rows * 16)
        return;
    if (x1 > x2) { t = x1; x1 = x2; x2 = t; }
    if (x1 < 0) x1 = 0;
    if (x2 >= cols * 8) x2 = cols * 8 - 1;
    if (x1 > x2)
        return;
    if (C->pgfx) {
        plane_rect(x1, y, x2 - x1 + 1, 1, (c < 0 ? fg_c : c) & 15);
        return;
    }
    fill_rect((u32)(x0 + x1 * scale), (u32)(y0 + y * scale), (u32)((x2 - x1 + 1) * scale),
              (u32)scale, pal[(c < 0 ? fg_c : c) & 15]);
    gdirty = 1;
}

int vga_gui_active(void) { return gui; }

void vga_geom(struct vgeom *g)
{
    int i;
    g->fb = fb;
    g->pitch = pitch;
    g->bpp = bpp;
    g->x0 = x0;
    g->y0 = y0;
    g->scale = scale;
    g->w = gfx ? con_all[0].ncol * 8 : 0;
    g->h = gfx ? con_all[0].nrow * 16 : 0;
    for (i = 0; i < 16; i++)
        g->pal[i] = pal[i];
}

static void main_dirty(void *u, int x, int y, int w, int h)
{
    (void)u;
    fb_dirty(x, y, w, h);
}

/* Modo escritorio: la consola del kernel pasa a dibujar en las capas 0 (texto) y 1 (PLOT);
 * lo que habia en pantalla se conserva debajo de las ventanas. Al salir se repinta tal cual. */
int vga_gui(int on)
{
    if (on) {
        int r, c;
        if (!gfx || fb_open() < 0)
            return -1;
        mouse_hide();
        sel_clear();
        vga_cursor_show(0);
        C = &con_all[0];
        C->ptxt = fb_layer(0);
        C->pgfx = fb_layer(1);
        C->stride = C->ncol * 8;
        C->dirty = main_dirty;
        for (r = 0; r < C->nrow; r++)
            for (c = 0; c < C->ncol; c++)
                draw_plane(r, c, C->ccells[r * MAXC + c]);
        gui = 1;
        mused = 1;
        fb_dirty(0, 0, C->ncol * 8, C->nrow * 16);
        fb_flush();
        fb_cursor_move(mx, my);
        fb_cursor_show(1);
    } else {
        fb_cursor_show(0);
        fb_close();
        gui = 0;
        C = &con_all[0];
        C->ptxt = C->pgfx = NULL;
        paint_border();
        repaint_all();
        refresh();
    }
    return 0;
}

/* ---- colores (numeros del C64) ---- */

void vga_set_fg(int c)
{
    fg_c = c & 15;
}

void vga_set_bg(int c)
{
    int r, k, old = bg_c;
    mouse_hide();
    sel_clear();
    bg_c = c & 15;
    for (r = 0; r < rows; r++)
        for (k = 0; k < cols; k++) {
            u16 v = cells[r * MAXC + k];
            if (((v >> 12) & 15) == old)
                cells[r * MAXC + k] = (u16)((v & 0x0FFF) | (bg_c << 12));
        }
    refresh();
}

void vga_set_border(int c)
{
    border_c = c & 15;
    paint_border();
    refresh();
}

int vga_get_fg(void)     { return fg_c; }
int vga_get_bg(void)     { return bg_c; }
int vga_get_border(void) { return border_c; }

void vga_setcolor(int fg, int bg)
{
    vga_set_fg(fg);
    vga_set_bg(bg);
}

int vga_col(void)
{
    return col;
}

/* ---- cursor: bloque que invierte los colores de la celda ---- */

void vga_cursor_show(int on)
{
    int c = col < cols ? col : cols - 1;
    u16 v = cells[row * MAXC + c];
    if (on)
        v = (u16)((v & 0x00FF) | (((v >> 12) & 15) << 8) | (((v >> 8) & 15) << 12));
    draw_value(row, c, v);
    shown[row * MAXC + c] = v;
}


/* ---- raton: cursor de bloque, seleccion con arrastre y pegado (estilo consola BSD) ---- */


static u16 swapnib(u16 v)
{
    return (u16)((v & 0x00FF) | (((v >> 12) & 15) << 8) | (((v >> 8) & 15) << 12));
}

static void toggle_range(int lo, int hi)
{
    int i;
    for (i = lo; i <= hi; i++) {
        int k = (i / cols) * MAXC + i % cols;
        cells[k] = swapnib(cells[k]);
    }
}

static void sel_clear(void)
{
    if (hl_lo >= 0) {
        toggle_range(hl_lo, hl_hi);
        hl_lo = -1;
        refresh();
    }
    sel_on = 0;
}

static void sel_update(void)
{
    int lo = sel_a < sel_b ? sel_a : sel_b, hi = sel_a < sel_b ? sel_b : sel_a;
    if (hl_lo >= 0)
        toggle_range(hl_lo, hl_hi);
    toggle_range(lo, hi);
    hl_lo = lo;
    hl_hi = hi;
    refresh();
}

static void mouse_hide(void)
{
    if (mshow) {
        int i = mr * MAXC + mc;
        draw_value(mr, mc, cells[i]);
        shown[i] = cells[i];
        mshow = 0;
    }
}

static void mouse_draw(void)
{
    int i;
    u16 v;
    mr = my / 16;
    mc = mx / 8;
    i = mr * MAXC + mc;
    v = swapnib(cells[i]);
    draw_value(mr, mc, v);
    shown[i] = v;
    mshow = 1;
}

static void sel_copy(void)
{
    int i, n = 0, last = 0;
    clip_n = 0;
    for (i = hl_lo; i <= hl_hi && n < (int)sizeof clip - 2; i++) {
        u8 ch = (u8)(cells[(i / cols) * MAXC + i % cols] & 0xFF);
        if (ch >= 32 && !(ch >= 128 && ch < 160)) {
            clip[n++] = ch;
            if (ch != ' ')
                last = n;
        } else {
            clip[n++] = ' ';
        }
        if (i % cols == cols - 1 && i < hl_hi) {   /* fin de fila: recortar espacios */
            n = last;
            clip[n++] = '\n';
            last = n;
        }
    }
    clip_n = last;
}

void vga_mouse(int dx, int dy, int btn)
{
    int old = mbtn, cell, k;

    if (gui) {                                  /* escritorio: solo seguir posicion y botones */
        mx += dx * 2;
        my += dy * 2;
        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx > con_all[0].ncol * 8 - 1) mx = con_all[0].ncol * 8 - 1;     /* tamano de la pantalla, no de la ventana actual */
        if (my > con_all[0].nrow * 16 - 1) my = con_all[0].nrow * 16 - 1;
        mbtn = btn;
        fb_cursor_move(mx, my);
        return;
    }
    mouse_hide();
    mused = 1;
    mx += dx * 2;
    my += dy * 2;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > cols * 8 - 1) mx = cols * 8 - 1;
    if (my > rows * 16 - 1) my = rows * 16 - 1;
    mbtn = btn;
    cell = (my / 16) * cols + mx / 8;

    if ((btn & 1) && !(old & 1)) {              /* boton izquierdo: empieza la seleccion */
        sel_clear();
        sel_on = 1;
        sel_a = sel_b = cell;
        sel_update();
    } else if ((btn & 1) && sel_on && cell != sel_b) {
        sel_b = cell;
        sel_update();
    } else if (!(btn & 1) && (old & 1) && sel_on) { /* soltar: copiar */
        sel_on = 0;
        sel_copy();
    }
    if ((btn & 6) && !(old & 6))                /* derecho o central: pegar */
        for (k = 0; k < clip_n; k++)
            kbd_inject(clip[k]);
    mouse_draw();
}

int vga_mouse_get(int what)
{
    if (!mused) {
        if (what == 2)
            return 0;
    }
    return what == 0 ? mx : (what == 1 ? my : mbtn);
}

/* ---- salida ---- */

static void newline(void)
{
    col = 0;
    row++;
    rev = 0;
    if (row >= rows)
        scroll();
}

void vga_putc(char ch)
{
    u8 c = (u8)ch;

    if (!C->ptxt) {
        mouse_hide();
        sel_clear();
    }

    switch (c) {
    case '\n': newline(); break;
    case '\r': col = 0; break;
    case '\b':
        if (col > 0) {
            col--;
            put_cell(row, col, blank_cell());
        } else if (row > 0) {
            row--;
            col = cols - 1;
            put_cell(row, col, blank_cell());
        }
        break;
    case '\t': {
        int n = 8 - (col % 8);
        while (n--)
            vga_putc(' ');
        return;
    }
    case 147: vga_clear(); return;                      /* limpiar pantalla */
    case 19:  row = col = 0; break;                     /* home */
    case 17:  if (row < rows - 1) row++; else scroll(); break;
    case 145: if (row > 0) row--; break;
    case 29:  if (col < cols - 1) col++; break;
    case 157: if (col > 0) col--; break;
    case 18:  rev = 1; break;
    case 146: rev = 0; break;
    case 5:   fg_c = 1;  break;     case 28:  fg_c = 2;  break;
    case 30:  fg_c = 5;  break;     case 31:  fg_c = 6;  break;
    case 144: fg_c = 0;  break;     case 156: fg_c = 4;  break;
    case 158: fg_c = 7;  break;     case 159: fg_c = 3;  break;
    case 129: fg_c = 8;  break;     case 149: fg_c = 9;  break;
    case 150: fg_c = 10; break;     case 151: fg_c = 11; break;
    case 152: fg_c = 12; break;     case 153: fg_c = 13; break;
    case 154: fg_c = 14; break;     case 155: fg_c = 15; break;
    default:
        if (c < 32 || (c >= 128 && c < 160))
            break;                                      /* control sin efecto */
        if (col >= cols) {
            col = 0;
            row++;
            if (row >= rows)
                scroll();
        }
        {
            int f = rev ? bg_c : fg_c, b = rev ? fg_c : bg_c;
            put_cell(row, col, (u16)(c | (f << 8) | (b << 12)));
        }
        col++;
        break;
    }
}

void vga_locate(int c, int r)
{
    if (c < 0) c = 0;
    if (r < 0) r = 0;
    if (c >= cols) c = cols - 1;
    if (r >= rows) r = rows - 1;
    col = c;
    row = r;
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

/* ---- consolas de las ventanas de terminal ---- */

static void con_blank(struct con *k)
{
    int i;
    u16 b = (u16)(' ' | (k->cfg << 8) | (k->cbg << 12));
    for (i = 0; i < MAXR * MAXC; i++) {
        k->ccells[i] = b;
        k->cshown[i] = 0xFFFF;
    }
}

int vga_con_attach(int id, u8 *txt, u8 *gfxp, int stride, int ncols, int nrows, con_dirty_fn cb, void *user)
{
    struct con *k, *save = C;
    if (id < 1 || id >= NCON || !gfx)
        return -1;
    k = &con_all[id];
    k->used = 1;
    k->cfg = 1;
    k->cbg = 6;
    k->crev = 0;
    k->crow = k->ccol = 0;
    k->ncol = ncols > MAXC ? MAXC : ncols;
    k->nrow = nrows > MAXR ? MAXR : nrows;
    k->ptxt = txt;
    k->pgfx = gfxp;
    k->stride = stride;
    k->dirty = cb;
    k->user = user;
    con_blank(k);
    C = k;
    refresh();
    C = save;
    return 0;
}

void vga_con_detach(int id)
{
    if (id < 1 || id >= NCON)
        return;
    con_all[id].used = 0;
    con_all[id].ptxt = con_all[id].pgfx = NULL;
    if (C == &con_all[id])
        C = &con_all[0];
}

void vga_con_select(int id)
{
    if (id >= 0 && id < NCON && con_all[id].used)
        C = &con_all[id];
}

void vga_con_resize(int id, int ncols, int nrows)
{
    struct con *k, *save = C;
    int r, c, delta;
    if (id < 1 || id >= NCON || !con_all[id].used)
        return;
    k = &con_all[id];
    if (ncols < 4) ncols = 4;
    if (nrows < 2) nrows = 2;
    if (ncols > MAXC) ncols = MAXC;
    if (nrows > MAXR) nrows = MAXR;
    C = k;
    if (k->crow >= nrows) {                       /* el cursor quedaria fuera: subir el texto */
        delta = k->crow - nrows + 1;
        for (r = 0; r + delta < MAXR; r++)
            for (c = 0; c < MAXC; c++)
                k->ccells[r * MAXC + c] = k->ccells[(r + delta) * MAXC + c];
        k->crow -= delta;
    }
    {
        u16 b = (u16)(' ' | (k->cfg << 8) | (k->cbg << 12));
        for (r = 0; r < MAXR; r++)
            for (c = 0; c < MAXC; c++)
                if (r >= nrows || c >= ncols || r >= k->nrow || c >= k->ncol) {
                    if (r >= k->nrow || c >= k->ncol)
                        k->ccells[r * MAXC + c] = b;      /* lo que aparece nuevo, en blanco */
                }
    }
    k->ncol = ncols;
    k->nrow = nrows;
    if (k->ccol >= ncols)
        k->ccol = ncols - 1;
    for (r = 0; r < MAXR * MAXC; r++)
        k->cshown[r] = 0xFFFF;
    refresh();
    C = save;
}

void vga_con_putc(int id, int ch)
{
    struct con *save = C;
    if (id < 0 || id >= NCON || !con_all[id].used)
        return;
    C = &con_all[id];
    vga_putc((char)ch);
    C = save;
}

void vga_con_cursor(int id, int on)
{
    struct con *save = C;
    if (id < 0 || id >= NCON || !con_all[id].used)
        return;
    C = &con_all[id];
    vga_cursor_show(on);
    C = save;
}
