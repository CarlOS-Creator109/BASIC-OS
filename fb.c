/* fb.c - capas de framebuffer y compositor (solo se usan con el escritorio)
 *
 * Cada capa es un buffer de indices de color del C64 (0-15; 255 = transparente), del tamano
 * del area logica de la pantalla (la misma que usa PLOT). De abajo hacia arriba:
 *
 *   capa 0  FB0  terminal por defecto (el texto de la consola del kernel)
 *   capa 1  FB1  area de dibujo de los programas encima del kernel (PLOT, LINE, apps)
 *   capa 2       plano del escritorio (ventanas; lo maneja GUI.SYS)
 *   capa 3  FB2  libre: los programas dibujan encima de si mismos y de las ventanas
 *   capa 4  FB3  libre, la mas alta (HUD, efectos)
 *
 * fb_present() mezcla las capas en el framebuffer real (con el factor de escala de la
 * pantalla) y dibuja encima el cursor del raton. Los cambios se avisan con fb_dirty() y se
 * pintan juntos con fb_flush().
 */
#include "kernel.h"

#define NL 5
#define POOL_BASE 0x1000000u            /* 16 MB: sobre los modulos y el bss del kernel */

static u8 *lay[NL];
static u8 on[NL];
static int lw, lh, opened;
static struct vgeom g;
static int dx0, dy0, dx1, dy1;          /* union de lo cambiado; dx1 <= dx0 = nada */
static int cur_x, cur_y, cur_on;
static u32 pool_ptr, pool_end;
static u32 rowbuf[3200 * 3 + 16];       /* una fila de pantalla en pixeles reales */

#define CUR_W 11
#define CUR_H 16
static const char *const cur_img[CUR_H] = {
    "X..........", "XX.........", "X#X........", "X##X.......",
    "X###X......", "X####X.....", "X#####X....", "X######X...",
    "X#######X..", "X########X.", "X#####XXXXX", "X##X##X....",
    "X#X.X##X...", "XX..X##X...", "X....X##X..", ".....XXXX.."
};

static u64 usable_end(void)
{
    const struct bootinfo *bi = BOOTINFO;
    u64 end = 0;
    int i;
    if (bi->e820n) {
        for (i = 0; i < bi->e820n; i++) {
            const struct e820 *e = &E820_TABLE[i];
            if (e->type == 1 && e->base <= POOL_BASE && e->base + e->len > POOL_BASE)
                end = e->base + e->len;
        }
    } else {
        end = ((u64)bi->extkb + 1024) * 1024;
    }
    if (end > 0x40000000ull)
        end = 0x40000000ull;            /* la memoria mapeada con identidad llega a 1 GB */
    return end;
}

/* Reserva las capas. 0 = bien, <0 = no hay memoria suficiente */
int fb_open(void)
{
    u32 size, i;
    u64 end;

    vga_geom(&g);
    lw = g.w;
    lh = g.h;
    if (lw <= 0 || lh <= 0 || lw > 1600)
        return -1;
    size = (u32)(lw * lh);
    end = usable_end();
    if (end < (u64)POOL_BASE + 5ull * size + 3ull * size)
        return -1;                      /* ni las capas ni tres ventanas caben */
    for (i = 0; i < NL; i++) {
        lay[i] = (u8 *)(u64)(POOL_BASE + i * size);
        memset(lay[i], i == 0 ? 6 : 255, size);
        on[i] = (i <= 1);
    }
    pool_ptr = (POOL_BASE + NL * size + 4095u) & ~4095u;
    pool_end = (u32)end;
    dx0 = dy0 = dx1 = dy1 = 0;
    cur_on = 0;
    opened = 1;
    return 0;
}

void fb_close(void)
{
    opened = 0;
    cur_on = 0;
}

/* Capa n (0-4); los programas obtienen asi su buffer y quedan activos */
u8 *fb_layer(int n)
{
    if (!opened || n < 0 || n >= NL)
        return NULL;
    on[n] = 1;
    return lay[n];
}

/* Memoria libre detras de las capas (superficies de ventanas) */
void *fb_pool(unsigned int *bytes)
{
    if (!opened) {
        *bytes = 0;
        return NULL;
    }
    *bytes = pool_end - pool_ptr;
    return (void *)(u64)pool_ptr;
}

static void draw_cursor(void)
{
    int i, j, k, l;
    u32 bytes = g.bpp / 8;
    for (j = 0; j < CUR_H; j++) {
        int y = cur_y + j;
        if (y < 0 || y >= lh)
            continue;
        for (i = 0; i < CUR_W; i++) {
            int x = cur_x + i;
            char ch = cur_img[j][i];
            u32 v;
            if (ch == '.' || x < 0 || x >= lw)
                continue;
            v = g.pal[ch == 'X' ? 0 : 1];
            for (l = 0; l < g.scale; l++) {
                volatile u8 *p = g.fb + (u32)(g.y0 + y * g.scale + l) * g.pitch +
                                 (u32)(g.x0 + x * g.scale) * bytes;
                for (k = 0; k < g.scale; k++, p += bytes) {
                    if (g.bpp == 32)
                        *(volatile u32 *)p = v;
                    else if (g.bpp == 24) {
                        p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16);
                    } else
                        *(volatile u16 *)p = (u16)v;
                }
            }
        }
    }
}

/* Mezcla las capas en el rectangulo (coordenadas logicas) y lo escribe en pantalla */
void fb_present(int x, int y, int w, int h)
{
    const u8 *act[NL];
    int nact = 0, i, j, k, l, sc = g.scale;
    u32 bytes = g.bpp / 8;

    if (!opened)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > lw) w = lw - x;
    if (y + h > lh) h = lh - y;
    if (w <= 0 || h <= 0)
        return;
    for (i = 0; i < NL; i++)
        if (on[i])
            act[nact++] = lay[i];

    for (j = y; j < y + h; j++) {
        u32 *o = rowbuf;
        u32 off = (u32)(j * lw + x);
        for (i = 0; i < w; i++, off++) {
            u8 c = 255;
            for (k = nact - 1; k >= 0; k--) {
                c = act[k][off];
                if (c < 16)
                    break;
            }
            {
                u32 v = g.pal[c & 15];
                for (l = 0; l < sc; l++)
                    *o++ = v;
            }
        }
        for (l = 0; l < sc; l++) {
            volatile u8 *p = g.fb + (u32)(g.y0 + j * sc + l) * g.pitch + (u32)(g.x0 + x * sc) * bytes;
            int n = w * sc;
            if (g.bpp == 32) {
                memcpy((void *)p, rowbuf, (size_t)n * 4);
            } else if (g.bpp == 24) {
                for (k = 0; k < n; k++, p += 3) {
                    p[0] = (u8)rowbuf[k];
                    p[1] = (u8)(rowbuf[k] >> 8);
                    p[2] = (u8)(rowbuf[k] >> 16);
                }
            } else {
                for (k = 0; k < n; k++, p += 2)
                    *(volatile u16 *)p = (u16)rowbuf[k];
            }
        }
    }
    if (cur_on && x < cur_x + CUR_W && x + w > cur_x && y < cur_y + CUR_H && y + h > cur_y)
        draw_cursor();
}

void fb_dirty(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    if (dx1 <= dx0 || dy1 <= dy0) {
        dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h;
        return;
    }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
}

void fb_flush(void)
{
    int x = dx0, y = dy0, w = dx1 - dx0, h = dy1 - dy0;
    dx0 = dy0 = dx1 = dy1 = 0;
    if (w > 0 && h > 0)
        fb_present(x, y, w, h);
}

void fb_cursor_show(int show)
{
    if (!opened || cur_on == show)
        return;
    cur_on = show;
    if (show)
        draw_cursor();
    else
        fb_present(cur_x, cur_y, CUR_W, CUR_H);
}

void fb_cursor_move(int x, int y)
{
    int ox = cur_x, oy = cur_y;
    if (x == cur_x && y == cur_y)
        return;
    cur_x = x;
    cur_y = y;
    if (!opened || !cur_on)
        return;
    {
        int was = cur_on;
        cur_on = 0;                     /* repintar lo que tapaba, sin el cursor viejo */
        fb_present(ox, oy, CUR_W, CUR_H);
        cur_on = was;
        draw_cursor();
    }
}
