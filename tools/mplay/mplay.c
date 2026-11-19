/* mplay.c - MPLAY.BIN: reproductor de video y audio para BASIC-OS hecho con FFmpeg (paquete opcional de la tienda).
 *
 * Es una app de VENTANA con icono en el escritorio (bloque BICN, ver sys/binicon.h): PKG lo guarda en APPS y el escritorio
 * pone su icono. FFmpeg se compila con musl y corre sobre la capa Linux de linux.c (modo residente: musl sigue vivo
 * mientras la ventana este abierta; cada callback entra con bos_enter()).
 *
 * Video a todo color: con la ventana activa se pide al kernel un "hueco" (K->overlay, kapi 6) y los cuadros se pintan
 * directo en el framebuffer. Sin foco (u otra ventana encima) se muestran en los 16 colores del escritorio con tramado.
 *
 * Formatos: MP4/MOV, MKV/WebM, AVI, OGG, FLV, MPEG-TS/PS, MP3, WAV, FLAC, AAC. Video: H.264, H.265, MPEG-4, MPEG-1/2,
 * VP8, VP9, Theora, MJPEG; AV1 con el plugin MPLAYAV1.BIN (dav1d, ver plugin.h). Audio: AAC, MP3, MP2, Vorbis, Opus, FLAC, AC-3, PCM.
 * Teclas: Espacio = pausa/seguir, flechas izq/der = -10/+10 s, arriba/abajo = volumen, A = abrir otro archivo.
 * Licencia: GPL-2.0-or-later (FFmpeg va como LGPL-2.1 o posterior).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include "kapi.h"
#include "mod_head.h"
#include "sys/binicon.h"
#include "linux.h"
#include "plugin.h"
#include <libavutil/imgutils.h>

BIN_ICON("MPLAY",
         "ICB\n"
         "................\n"
         ".00000000000000.\n"
         ".0BBBBBBBBBBBB0.\n"
         ".0EEEEEEEEEEEE0.\n"
         ".0EEE1EEEEEEEE0.\n"
         ".0EEE11EEEEEEE0.\n"
         ".0EEE111EEEEEE0.\n"
         ".0EEE1111EEEEE0.\n"
         ".0EEE11111EEEE0.\n"
         ".0EEE1111EEEEE0.\n"
         ".0EEE111EEEEEE0.\n"
         ".0EEE11EEEEEEE0.\n"
         ".0EEE1EEEEEEEE0.\n"
         ".0BBBBBBBBBBBB0.\n"
         ".00000000000000.\n"
         "................\n");

#define HEAP_LO   0xB400000ull            /* debajo: el programa (0x9000000), NET/PKG (0xB000000-0xB300000) */
#define NEED_RAM  (HEAP_LO + 48ull * 1024 * 1024)

struct bootinfo { uint32_t magic; uint16_t w, h, pitch; uint8_t bpp, gfx; uint32_t fb; uint8_t rs, rp, gs, gp, bs, bp;
                  uint16_t e820n; uint16_t extkb; } __attribute__((packed));
struct e820 { uint64_t base, len; uint32_t type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static uint64_t ram_end(void)
{
    uint64_t end = 0;
    int i;
    if (BI->e820n) {
        for (i = 0; i < BI->e820n; i++)
            if (E8[i].type == 1 && E8[i].base <= 0x4000000ull && E8[i].base + E8[i].len > 0x4000000ull)
                end = E8[i].base + E8[i].len;
    } else
        end = ((uint64_t)BI->extkb + 1024) * 1024;
    if (end > 0x40000000ull) end = 0x40000000ull;     /* el kernel mapea 1 GB */
    return end;
}

/* ================= dibujo en la superficie de la ventana (un indice de color del C64 por pixel) ================= */
enum { BLACK, WHITE, RED, CYAN, PURPLE, GREEN, BLUE, YELLOW, ORANGE, BROWN, LRED, DGREY, GREY, LGREEN, LBLUE, LGREY };
static const uint8_t c64[16][3] = {
    { 0x00, 0x00, 0x00 }, { 0xFF, 0xFF, 0xFF }, { 0x68, 0x37, 0x2B }, { 0x70, 0xA4, 0xB2 },
    { 0x6F, 0x3D, 0x86 }, { 0x58, 0x8D, 0x43 }, { 0x35, 0x28, 0x79 }, { 0xB8, 0xC7, 0x6F },
    { 0x6F, 0x4F, 0x25 }, { 0x43, 0x39, 0x00 }, { 0x9A, 0x67, 0x59 }, { 0x44, 0x44, 0x44 },
    { 0x6C, 0x6C, 0x6C }, { 0x9A, 0xD2, 0x84 }, { 0x6C, 0x5E, 0xB5 }, { 0x95, 0x95, 0x95 }
};
#define FONT ((const uint8_t *)0x6800)
#define CB 30                                 /* alto de la barra de controles */

static struct wctx *C;

static void rect(int x, int y, int w, int h, int col)
{
    int j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > C->w) w = C->w - x;
    if (y + h > C->h) h = C->h - y;
    if (w <= 0 || h <= 0) return;
    for (j = 0; j < h; j++) memset(C->pix + (size_t)(y + j) * C->stride + x, col, (size_t)w);
}
static void frame_rect(int x, int y, int w, int h, int col)
{
    rect(x, y, w, 1, col); rect(x, y + h - 1, w, 1, col); rect(x, y, 1, h, col); rect(x + w - 1, y, 1, h, col);
}
static void text(int x, int y, const char *s, int fg, int bg)
{
    for (; *s; s++, x += 8) {
        const uint8_t *g = FONT + (uint8_t)*s * 16;
        int i, j;
        if (x + 8 > C->w) break;
        for (j = 0; j < 16; j++) {
            if (y + j < 0 || y + j >= C->h) continue;
            for (i = 0; i < 8; i++) {
                if (x + i < 0) continue;
                if (g[j] & (0x80 >> i)) C->pix[(size_t)(y + j) * C->stride + x + i] = (uint8_t)fg;
                else if (bg >= 0) C->pix[(size_t)(y + j) * C->stride + x + i] = (uint8_t)bg;
            }
        }
    }
}
static void button(int x, int y, int w, int h, const char *label, int on)
{
    int n = (int)strlen(label);
    rect(x, y, w, h, on ? LBLUE : LGREY);
    frame_rect(x, y, w, h, DGREY);
    rect(x + 1, y + 1, w - 2, 1, WHITE);
    text(x + (w - n * 8) / 2, y + (h - 16) / 2, label, on ? WHITE : BLACK, -1);
}
static int inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

/* ================= el archivo, entero en memoria, y un AVIOContext que lee de ahi ================= */
static uint8_t *fdata;
static int64_t fsize, fpos;

static int io_read(void *o, uint8_t *b, int n)
{
    (void)o;
    if (fpos >= fsize) return AVERROR_EOF;
    if (n > fsize - fpos) n = (int)(fsize - fpos);
    memcpy(b, fdata + fpos, (size_t)n);
    fpos += n;
    return n;
}
static int64_t io_seek(void *o, int64_t off, int whence)
{
    (void)o;
    if (whence & AVSEEK_SIZE) return fsize;
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_SET) fpos = off;
    else if (whence == SEEK_CUR) fpos += off;
    else if (whence == SEEK_END) fpos = fsize + off;
    else return -1;
    if (fpos < 0) fpos = 0;
    if (fpos > fsize) fpos = fsize;
    return fpos;
}

/* ================= estado ================= */
enum { S_PICK, S_PLAY, S_END, S_MSG };
static int state = S_PICK;
static char msg[160], cur_name[64];

static AVFormatContext *fmt;
static AVIOContext *io;
static AVCodecContext *vdec, *adec;
static SwrContext *swr;
static AVPacket *pkt;
static AVFrame *fr, *vpend, *vlast;          /* recien decodificado, el que espera su hora, el ultimo que se mostro */
static int vi = -1, ai = -1, has_vpend, has_vlast, eof;

/* ---- plugins (AV1...) ---- */
static const struct mp_plugin *plug;          /* cargado en PLUGIN_BASE; se queda mientras MPLAY este abierto */
static void *plugh;                           /* el decodificador abierto del video actual (NULL = FFmpeg) */
static int plug_more, plug_drain;

static const struct mp_plugin *load_plugin(const char *file)
{
    static const char *const dirs[] = { "HD0.1:/APPS/", "HD1.1:/APPS/", "HD2.1:/APPS/", "HD0:/APPS/", "USB0.1:/APPS/", "HD0.1:", "HD0:", 0 };
    struct hdr { uint32_t magic, end, entry, base; } *h = (struct hdr *)(uintptr_t)PLUGIN_BASE;
    int i;
    if (plug) return plug;
    for (i = 0; dirs[i]; i++) {
        char spec[48];
        unsigned total = 0;
        int n;
        snprintf(spec, sizeof spec, "%s%s", dirs[i], file);
        n = K->read(spec, (char *)(uintptr_t)PLUGIN_BASE, (int)(PLUGIN_LIMIT - PLUGIN_BASE), &total);
        bos_after_kernel();
        if (n < 16 || (unsigned)n != total) continue;
        if (h->magic != 0x4D424350u || h->base != PLUGIN_BASE || h->end > PLUGIN_LIMIT || h->end < PLUGIN_BASE + (unsigned)n ||
            h->entry < PLUGIN_BASE || h->entry >= PLUGIN_BASE + (unsigned)n)
            continue;
        memset((void *)(uintptr_t)(PLUGIN_BASE + n), 0, h->end - (PLUGIN_BASE + (unsigned)n));
        {   const struct mp_plugin *p = ((const struct mp_plugin *(*)(const void *))(uintptr_t)h->entry)(0);
            if (p && p->magic == MPPL_MAGIC && p->version == MPPL_VERSION) { plug = p; return p; } }
    }
    return 0;
}

static int pic_to_frame(const struct mp_picture *pp, AVFrame *f)
{
    static const int map[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV422P10LE,
                               AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV444P10LE, AV_PIX_FMT_GRAY8, AV_PIX_FMT_GRAY10LE };
    const uint8_t *src[4] = { pp->data[0], pp->data[1], pp->data[2], 0 };
    int ls[4] = { (int)pp->stride[0], (int)pp->stride[1], (int)pp->stride[2], 0 };
    if (pp->fmt < 0 || pp->fmt > 7) return -1;
    av_frame_unref(f);
    f->format = map[pp->fmt]; f->width = pp->w; f->height = pp->h;
    if (av_frame_get_buffer(f, 32) < 0) return -1;
    av_image_copy(f->data, f->linesize, src, ls, (enum AVPixelFormat)f->format, pp->w, pp->h);
    f->pts = f->best_effort_timestamp = pp->pts;
    f->sample_aspect_ratio = fmt && vi >= 0 ? fmt->streams[vi]->codecpar->sample_aspect_ratio : (AVRational){ 1, 1 };
    return 0;
}
static double vpts, skip_until = -1, duration;
static unsigned last_show_ms;

static int snd, out_rate, volume = 6;
static int16_t *abuf;
static int abuf_cap, a_left, a_off;          /* muestras convertidas que no cupieron en la cola (cuadros) */
static double a_end;
static int paused;
static unsigned t0, pause_t;
static double t_offset;

static double clock_now(void)
{
    if (snd && ai >= 0) return a_end - (double)(K->stream(3, 0, 0) + a_left) / out_rate;
    if (paused) return (double)(pause_t - t0) / 1000.0 + t_offset;
    return (double)(K->ms() - t0) / 1000.0 + t_offset;
}

/* ================= pantalla: hueco a todo color o tramado de 16 colores ================= */
static uint8_t lut[32768];                   /* RGB555 -> color del C64 mas cercano */
static enum AVPixelFormat fbfmt = AV_PIX_FMT_NONE;
static struct SwsContext *sws_fb, *sws_dt;
static int sfb_key[5], sdt_key[5];
static uint8_t *dbuf;                        /* cuadro escalado (BGR0) para el tramado */
static int dbuf_cap;
static int ov_on, ov_rect[4], ov_real[4];    /* hueco puesto: rectangulo logico pedido y el real */
static int have_overlay;

static void lut_init(void)
{
    int i, k;
    for (i = 0; i < 32768; i++) {
        int r = ((i >> 10) & 31) * 255 / 31, g = ((i >> 5) & 31) * 255 / 31, b = (i & 31) * 255 / 31, best = 0;
        long bd = -1;
        for (k = 0; k < 16; k++) {
            long dr = r - c64[k][0], dg = g - c64[k][1], db = b - c64[k][2];
            long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
            if (bd < 0 || d < bd) { bd = d; best = k; }
        }
        lut[i] = (uint8_t)best;
    }
}

static struct SwsContext *get_sws(struct SwsContext *s, int *key, int sw_, int sh_, int sf, int dw, int dh, int df)
{
    if (s && key[0] == sw_ && key[1] == sh_ && key[2] == sf && key[3] == dw && key[4] == dh) return s;
    sws_freeContext(s);
    key[0] = sw_; key[1] = sh_; key[2] = sf; key[3] = dw; key[4] = dh;
    return sws_getContext(sw_, sh_, sf, dw, dh, df, SWS_FAST_BILINEAR, 0, 0, 0);
}

/* el video cabe en w x h conservando la forma */
static void fit(const AVFrame *f, int w, int h, int *ow, int *oh)
{
    double ar = f->sample_aspect_ratio.num ? av_q2d(f->sample_aspect_ratio) : 1.0, fw = f->width * ar, fh = f->height;
    int dw = w, dh = (int)(w * fh / fw);
    if (dh > h) { dh = h; dw = (int)(h * fw / fh); }
    *ow = dw & ~1; *oh = dh & ~1;
    if (*ow < 2) *ow = 2;
    if (*oh < 2) *oh = 2;
}

static int vid_w(void) { return C->w; }
static int vid_h(void) { int h = C->h - CB; return h < 16 ? 16 : h; }

static void fb_black(const int *r)
{
    uint8_t *fbp = (uint8_t *)(uintptr_t)BI->fb;
    int y, bpp = BI->bpp / 8;
    for (y = 0; y < r[3]; y++) memset(fbp + (size_t)(r[1] + y) * BI->pitch + (size_t)r[0] * bpp, 0, (size_t)r[2] * bpp);
}

static void overlay_off(void)
{
    if (ov_on) { K->overlay(1, 0); ov_on = 0; }
}

/* 1 = el hueco esta puesto donde va el video ahora mismo */
static int overlay_sync(void)
{
    int r[4];
    if (!have_overlay || !C->focus || fbfmt == AV_PIX_FMT_NONE || vi < 0 || state == S_PICK || state == S_MSG) {
        overlay_off();
        return 0;
    }
    r[0] = C->sx; r[1] = C->sy; r[2] = vid_w(); r[3] = vid_h();
    if (ov_on && !memcmp(r, ov_rect, sizeof r)) return 1;
    memcpy(ov_rect, r, sizeof r);
    if (!K->overlay(0, r)) { ov_on = 0; return 0; }
    memcpy(ov_real, r, sizeof r);
    ov_on = 1;
    fb_black(ov_real);
    return 2;                                  /* recien puesto: hay que volver a pintar el ultimo cuadro */
}

static void show_overlay(const AVFrame *f)
{
    int dw, dh, dx, dy, bpp = BI->bpp / 8;
    uint8_t *dst[4] = { 0 };
    int dls[4] = { BI->pitch, 0, 0, 0 };
    fit(f, ov_real[2], ov_real[3], &dw, &dh);
    dx = ov_real[0] + (ov_real[2] - dw) / 2;
    dy = ov_real[1] + (ov_real[3] - dh) / 2;
    sws_fb = get_sws(sws_fb, sfb_key, f->width, f->height, f->format, dw, dh, fbfmt);
    if (!sws_fb) return;
    dst[0] = (uint8_t *)(uintptr_t)BI->fb + (size_t)dy * BI->pitch + (size_t)dx * bpp;
    sws_scale(sws_fb, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dls);
    K->overlay(2, 0);
}

static void show_dither(const AVFrame *f)
{
    static const int bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    int dw, dh, dx, dy, x, y, W = vid_w(), H = vid_h();
    uint8_t *dst[4] = { 0 };
    int dls[4] = { 0 };
    fit(f, W, H, &dw, &dh);
    dx = (W - dw) / 2; dy = (H - dh) / 2;
    if (dw * dh * 4 > dbuf_cap) { dbuf_cap = dw * dh * 4; free(dbuf); dbuf = malloc((size_t)dbuf_cap); }
    if (!dbuf) return;
    sws_dt = get_sws(sws_dt, sdt_key, f->width, f->height, f->format, dw, dh, AV_PIX_FMT_BGR0);
    if (!sws_dt) return;
    dst[0] = dbuf; dls[0] = dw * 4;
    sws_scale(sws_dt, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dls);
    rect(0, 0, W, H, BLACK);
    for (y = 0; y < dh; y++) {
        const uint8_t *s = dbuf + (size_t)y * dw * 4;
        uint8_t *o = C->pix + (size_t)(dy + y) * C->stride + dx;
        for (x = 0; x < dw; x++, s += 4) {
            int t = (bayer[y & 3][x & 3] - 8) * 3;
            int b = s[0] + t, g = s[1] + t, r = s[2] + t;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            o[x] = lut[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
        }
    }
    C->damage(C, 0, 0, W, H);
}

static void show_frame(const AVFrame *f)
{
    if (overlay_sync()) show_overlay(f);
    else show_dither(f);
}

/* ================= barra de controles ================= */
static int ctrl_dirty = 1;
static unsigned ctrl_t;
enum { B_OPEN = 1, B_PLAY, B_BACK, B_FWD, B_SEEK, B_VDN, B_VUP };
struct hit { int x, y, w, h, id; };
static struct hit hits[8];
static int nhits;

static void addhit(int x, int y, int w, int h, int id) { if (nhits < 8) { hits[nhits].x = x; hits[nhits].y = y; hits[nhits].w = w; hits[nhits].h = h; hits[nhits].id = id; nhits++; } }

static void fmt_time(char *o, double t)
{
    int s = t > 0 ? (int)t : 0;
    if (s >= 3600) sprintf(o, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else sprintf(o, "%02d:%02d", s / 60, s % 60);
}

static void draw_controls(void)
{
    int y = C->h - CB, x = 4, W = C->w, sx0, sx1;
    char t[40], a[16], b[16];
    double now = (state == S_PLAY || state == S_END) ? clock_now() : 0;
    nhits = 0;
    rect(0, y, W, CB, GREY);
    rect(0, y, W, 1, LGREY);
    button(x, y + 4, 56, 22, "Abrir", 0); addhit(x, y + 4, 56, 22, B_OPEN); x += 60;
    button(x, y + 4, 32, 22, state == S_PLAY && !paused ? "||" : "\x10", 0); addhit(x, y + 4, 32, 22, B_PLAY); x += 36;
    button(x, y + 4, 32, 22, "\x11\x11", 0); addhit(x, y + 4, 32, 22, B_BACK); x += 36;
    button(x, y + 4, 32, 22, "\x10\x10", 0); addhit(x, y + 4, 32, 22, B_FWD); x += 40;
    fmt_time(a, now);
    fmt_time(b, duration);
    snprintf(t, sizeof t, "%s/%s", a, b);
    sx0 = x; sx1 = W - (int)strlen(t) * 8 - 64;
    if (sx1 - sx0 > 20) {                    /* barra de avance */
        int fill = duration > 0 ? (int)((sx1 - sx0 - 2) * (now / duration)) : 0;
        if (fill < 0) fill = 0;
        if (fill > sx1 - sx0 - 2) fill = sx1 - sx0 - 2;
        rect(sx0, y + 11, sx1 - sx0, 8, DGREY);
        rect(sx0 + 1, y + 12, fill, 6, LGREEN);
        addhit(sx0, y + 4, sx1 - sx0, 22, B_SEEK);
        text(sx1 + 6, y + 7, t, WHITE, -1);
    }
    button(W - 50, y + 4, 22, 22, "-", 0); addhit(W - 50, y + 4, 22, 22, B_VDN);
    button(W - 26, y + 4, 22, 22, "+", 0); addhit(W - 26, y + 4, 22, 22, B_VUP);
    C->damage(C, 0, y, W, CB);
    ctrl_dirty = 0;
    ctrl_t = K->ms();
}

/* ================= lista de archivos ================= */
#define MAXF 200
struct fent { char spec[48]; unsigned size; };
static struct fent files[MAXF];
static int nfiles, fsel, ftop;
static char scan_base[32];
static unsigned last_click;

static int is_media(const char *n)
{
    static const char *const ext[] = { "MP4", "M4V", "MKV", "WEBM", "WEB", "AVI", "MOV", "OGG", "OGV", "FLV", "TS", "MPG", "MPEG",
                                       "MP3", "WAV", "FLAC", "FLA", "M4A", "AAC", "OPUS", "OPU", "WMV", 0 };
    const char *d = strrchr(n, '.');
    int i;
    if (!d) return 0;
    for (i = 0; ext[i]; i++) if (!strcasecmp(d + 1, ext[i])) return 1;
    return 0;
}

static void scan_cb(const char *name, unsigned size, int isdir)
{
    if (isdir || nfiles >= MAXF || !is_media(name)) return;
    snprintf(files[nfiles].spec, sizeof files[nfiles].spec, "%s%s", scan_base, name);
    files[nfiles].size = size;
    nfiles++;
}

static void scan_files(void)
{
    char ln[64];
    int i, k;
    nfiles = 0; fsel = 0; ftop = 0;
    for (i = 0; K->drive_line(i, ln) && i < 16; i++) {
        for (k = 0; ln[k] && ln[k] != ' '; k++) ;
        ln[k] = 0;
        snprintf(scan_base, sizeof scan_base, "%s:", ln);
        K->dir(scan_base, scan_cb);
        bos_after_kernel();
        snprintf(scan_base, sizeof scan_base, "%s:/VIDEOS/", ln);
        { char d[40]; snprintf(d, sizeof d, "%s:/VIDEOS", ln); K->dir(d, scan_cb); bos_after_kernel(); }
    }
}

static void draw_picker(void)
{
    int W = C->w, H = vid_h(), rows = (H - 52) / 18, i, y;
    rect(0, 0, W, H, BLUE);
    text(8, 6, "Elige un video (flechas + Enter, o doble clic)", WHITE, -1);
    text(8, 24, nfiles ? "R = buscar otra vez" : "No encontre videos en los discos (raiz o carpeta VIDEOS).", LGREY, -1);
    if (fsel < ftop) ftop = fsel;
    if (rows > 0 && fsel >= ftop + rows) ftop = fsel - rows + 1;
    for (i = ftop, y = 46; i < nfiles && y + 18 <= H; i++, y += 18) {
        char t[80];
        snprintf(t, sizeof t, "%-36s %6.1f MB", files[i].spec, files[i].size / 1048576.0);
        if (i == fsel) rect(4, y, W - 8, 18, LBLUE);
        text(8, y + 1, t, WHITE, -1);
    }
    C->damage(C, 0, 0, W, H);
}

static void draw_msg(void)
{
    int W = C->w, H = vid_h();
    rect(0, 0, W, H, BLACK);
    text(12, H / 2 - 24, cur_name, YELLOW, -1);
    text(12, H / 2, msg, WHITE, -1);
    C->damage(C, 0, 0, W, H);
}

static void redraw_all(void)
{
    if (state == S_PICK) { overlay_off(); draw_picker(); }
    else if (state == S_MSG || vi < 0) { overlay_off(); draw_msg(); }
    else {
        rect(0, 0, vid_w(), vid_h(), BLACK);
        C->damage(C, 0, 0, vid_w(), vid_h());
        if (has_vlast) show_frame(vlast);
    }
    ctrl_dirty = 1;
    draw_controls();
}

/* ================= abrir y cerrar ================= */
static void close_media(void)
{
    if (snd) { K->stream(2, 0, 0); snd = 0; }
    avcodec_free_context(&vdec);
    if (plugh) { plug->close(plugh); plugh = 0; }
    plug_more = plug_drain = 0;
    avcodec_free_context(&adec);
    swr_free(&swr);
    if (fmt) avformat_close_input(&fmt);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    free(fdata); fdata = 0; fsize = 0;
    has_vpend = has_vlast = 0;
    vi = ai = -1;
    a_left = 0; eof = 0; duration = 0;
}

static void start_audio(void)
{
    snd = K->stream(0, 0, 0);
    if (snd) { K->stream(4, 0, (unsigned)out_rate); K->stream(5, 0, (unsigned)volume); }
}

static void open_media(const char *spec)
{
    char probe[512], t[100];
    unsigned total = 0;
    int r;
    const char *nm = strrchr(spec, ':');
    close_media();
    snprintf(cur_name, sizeof cur_name, "%s", spec);
    state = S_MSG;
    r = K->read(spec, probe, (int)sizeof probe, &total);
    bos_after_kernel();
    if (r < 0 || !total) { snprintf(msg, sizeof msg, "No pude leer el archivo."); return; }
    if ((uint64_t)total + 40ull * 1024 * 1024 > bos_free_ram()) { snprintf(msg, sizeof msg, "No cabe en la RAM (en QEMU usa -m 512 o mas)."); return; }
    fdata = malloc(total);
    if (!fdata) { snprintf(msg, sizeof msg, "No hay memoria."); return; }
    r = K->read(spec, (char *)fdata, (int)total, &total);
    bos_after_kernel();
    if (r < 0 || (unsigned)r != total) { snprintf(msg, sizeof msg, "No pude leer el archivo."); close_media(); return; }
    fsize = total; fpos = 0;
    io = avio_alloc_context(av_malloc(65536), 65536, 0, 0, io_read, 0, io_seek);
    fmt = avformat_alloc_context();
    fmt->pb = io;
    if (avformat_open_input(&fmt, spec, 0, 0) < 0) { fmt = 0; snprintf(msg, sizeof msg, "Formato desconocido."); close_media(); return; }
    if (avformat_find_stream_info(fmt, 0) < 0) { snprintf(msg, sizeof msg, "No entiendo el contenido."); close_media(); return; }
    duration = fmt->duration > 0 ? (double)fmt->duration / AV_TIME_BASE : 0;
    vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    msg[0] = 0;
    if (vi >= 0) {
        const AVCodecParameters *cp = fmt->streams[vi]->codecpar;
        const AVCodec *c = avcodec_find_decoder(cp->codec_id);
        if (fmt->streams[vi]->disposition & AV_DISPOSITION_ATTACHED_PIC) c = 0, vi = -1;   /* portada de un MP3 */
        else if (c && (vdec = avcodec_alloc_context3(c)) && avcodec_parameters_to_context(vdec, cp) >= 0) {
            vdec->thread_count = 1;
            if (avcodec_open2(vdec, c, 0) < 0) avcodec_free_context(&vdec);
        }
        if (vi >= 0 && !vdec && cp->codec_id == AV_CODEC_ID_AV1) {
            if (load_plugin("MPLAYAV1.BIN")) plugh = plug->open(cp->extradata, cp->extradata_size);
            if (!plugh) {
                snprintf(msg, sizeof msg, "Es AV1: instala el plugin MPLAYAV1 desde PKG. Mientras, solo se oye el audio.");
                vi = -1;
            }
        }
        if (vi >= 0 && !vdec && !plugh) {
            snprintf(msg, sizeof msg, "No tengo decodificador para el video (%s). Solo se oye el audio.", avcodec_get_name(cp->codec_id));
            vi = -1;
        }
    }
    if (ai >= 0) {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
        if (c && (adec = avcodec_alloc_context3(c)) && avcodec_parameters_to_context(adec, fmt->streams[ai]->codecpar) >= 0)
            if (avcodec_open2(adec, c, 0) < 0) avcodec_free_context(&adec);
        if (!adec) ai = -1;
        else {
            AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
            out_rate = adec->sample_rate > 48000 ? 48000 : adec->sample_rate < 8000 ? 8000 : adec->sample_rate;
            if (swr_alloc_set_opts2(&swr, &st, AV_SAMPLE_FMT_S16, out_rate, &adec->ch_layout, adec->sample_fmt,
                                    adec->sample_rate, 0, 0) < 0 || swr_init(swr) < 0) {
                avcodec_free_context(&adec); ai = -1;
            } else start_audio();
        }
    }
    if (vi < 0 && ai < 0) { if (!msg[0]) snprintf(msg, sizeof msg, "No hay ni video ni audio que yo sepa leer."); close_media(); return; }
    if (vi < 0 && !msg[0])
        snprintf(msg, sizeof msg, "Solo audio: %s %d Hz", adec->codec->name, adec->sample_rate);
    if (!pkt) pkt = av_packet_alloc();
    if (!fr) fr = av_frame_alloc();
    if (!vpend) vpend = av_frame_alloc();
    if (!vlast) vlast = av_frame_alloc();
    a_end = 0; t0 = K->ms(); t_offset = 0; paused = 0; skip_until = -1; last_show_ms = 0;
    state = S_PLAY;
    snprintf(t, sizeof t, "MPLAY - %s", nm ? nm + 1 : spec);
    C->title(C, t);
    if (vdec) snprintf(cur_name, sizeof cur_name, "%s  (%s %dx%d)", nm ? nm + 1 : spec, vdec->codec->name, vdec->width, vdec->height);
    else if (plugh) snprintf(cur_name, sizeof cur_name, "%s  (%s)", nm ? nm + 1 : spec, plug->name);
}

/* ================= reproducir: un poco en cada cuadro del escritorio ================= */
static int push_left(void)                   /* 1 = ya no queda nada pendiente */
{
    while (a_left > 0) {
        int took = K->stream(1, abuf + a_off * 2, (unsigned)a_left);
        if (took <= 0) return 0;
        a_off += took; a_left -= took;
    }
    return 1;
}

static void seek_to(double target)
{
    if (!fmt) return;
    if (target < 0) target = 0;
    if (duration > 0 && target > duration - 1) target = duration - 1;
    if (target < 0) target = 0;
    if (av_seek_frame(fmt, -1, (int64_t)(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0) return;
    if (vdec) avcodec_flush_buffers(vdec);
    if (plugh) { plug->flush(plugh); plug_more = plug_drain = 0; }
    if (adec) avcodec_flush_buffers(adec);
    if (snd) { K->stream(2, 0, 0); start_audio(); }
    a_left = 0; has_vpend = 0; eof = 0;
    skip_until = target; a_end = target;
    t_offset = target; t0 = K->ms(); pause_t = t0;
    if (state == S_END) state = S_PLAY;
    ctrl_dirty = 1;
}

/* un cuadro recien decodificado: 1 = quedo esperando su hora (ya no hay que sacar mas por ahora) */
static int take_vframe(AVFrame *f)
{
    int64_t ts = f->best_effort_timestamp;
    double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(fmt->streams[vi]->time_base) : vpts + 1.0 / 25;
    if (skip_until >= 0) {
        if (pts < skip_until) return 0;
        skip_until = -1;
        if (!snd) { t_offset = pts; t0 = K->ms(); }
    }
    /* tarde y ya se mostro algo hace poco: este no se pinta (se alcanza al audio) */
    if (clock_now() > pts + 0.15 && K->ms() - last_show_ms < 500) { vpts = pts; return 0; }
    av_frame_unref(vpend);
    av_frame_move_ref(vpend, f);
    vpts = pts;
    has_vpend = 1;
    return 1;
}

/* plugin: sacar cuadros y, si el paquete no habia entrado entero, volver a mandarlo */
static void plug_pull(void)
{
    struct mp_picture pp;
    while (plug_drain || plug_more) {
        int g = plug->get(plugh, &pp);
        if (g > 0) {
            int took = 0;
            if (!pic_to_frame(&pp, fr)) took = take_vframe(fr);
            plug->release(plugh, &pp);
            if (took) return;                     /* sigue en la proxima vuelta */
            continue;
        }
        plug_drain = 0;
        if (plug_more) {
            int r = plug->send(plugh, 0, 0, 0);
            plug_more = r == 1;
            if (r < 0) plug_more = 0;
            plug_drain = 1;
            if (!plug_more) { plug_pull(); return; }
            continue;
        }
        if (g < 0) return;
    }
}

static void pump(void)
{
    unsigned t_end = K->ms() + 18;
    if (state != S_PLAY || paused) return;
    for (;;) {
        if (a_left && !push_left()) {
            if (!has_vpend) return;          /* la cola del audio esta llena: ya vendra */
        }
        if (has_vpend) {
            double now = clock_now();
            int starving = snd && ai >= 0 && !a_left && K->stream(3, 0, 0) == 0;
            if (now >= vpts - 0.004 || starving) {
                show_frame(vpend);
                av_frame_unref(vlast);
                av_frame_move_ref(vlast, vpend);
                has_vlast = 1; has_vpend = 0;
                last_show_ms = K->ms();
            } else
                return;
        }
        if (a_left) return;
        if ((int)(K->ms() - t_end) > 0) return;
        if (plugh && (plug_drain || plug_more)) { plug_pull(); continue; }
        if (eof) {
            if (!snd || K->stream(3, 0, 0) == 0) { state = S_END; ctrl_dirty = 1; }
            return;
        }
        if (av_read_frame(fmt, pkt) < 0) {
            eof = 1;
            continue;
        }
        if (pkt->stream_index == ai && adec) {
            if (avcodec_send_packet(adec, pkt) >= 0)
                while (avcodec_receive_frame(adec, fr) >= 0) {
                    double pts = fr->pts != AV_NOPTS_VALUE ? fr->pts * av_q2d(fmt->streams[ai]->time_base) : a_end;
                    int max = swr_get_out_samples(swr, fr->nb_samples), n, base;
                    if (skip_until >= 0 && pts + (double)fr->nb_samples / fr->sample_rate < skip_until) continue;
                    base = a_left ? a_off + a_left : 0;
                    if (!a_left) a_off = 0;
                    if (base + max > abuf_cap) {
                        int16_t *nb;
                        abuf_cap = base + max + 4096;
                        nb = realloc(abuf, (size_t)abuf_cap * 4);
                        if (!nb) break;
                        abuf = nb;
                    }
                    {   uint8_t *op = (uint8_t *)(abuf + base * 2);
                        n = swr_convert(swr, &op, abuf_cap - base, (const uint8_t **)fr->extended_data, fr->nb_samples); }
                    if (n > 0 && snd) {
                        if (a_end == 0 || pts > a_end + 0.2 || pts < a_end - 0.2) a_end = pts;
                        a_left += n;
                        a_end += (double)n / out_rate;
                    }
                }
            push_left();
        } else if (pkt->stream_index == vi && vdec) {
            if (avcodec_send_packet(vdec, pkt) >= 0)
                while (avcodec_receive_frame(vdec, fr) >= 0)
                    if (take_vframe(fr)) break;
        } else if (pkt->stream_index == vi && plugh) {
            int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            int r = plug->send(plugh, pkt->data, pkt->size, ts);
            plug_more = r == 1;
            plug_drain = 1;
            plug_pull();
        }
        av_packet_unref(pkt);
    }
}

static void toggle_pause(void)
{
    if (state == S_END) { seek_to(0); paused = 0; return; }
    if (state != S_PLAY) return;
    paused = !paused;
    if (paused) pause_t = K->ms();
    else t0 += K->ms() - pause_t;
    ctrl_dirty = 1;
}

/* ================= callbacks de la ventana ================= */
/* cada callback corre en una pila grande propia (BIGSTACK): la del kernel no alcanza para dav1d */
#define BIGSTACK (4u << 20)
static uint8_t *bigstack;
static int ev_k, ev_x, ev_y, ev_btn, ev_prev;
static void on_big(void (*fn)(void)) { if (bigstack) bos_on_stack(fn, bigstack + BIGSTACK); else fn(); }

static void start_impl(void)
{
    const char *a = K->args;
    if (a && !strncmp(a, "MPLAY:", 6) && a[6]) {
        char spec[64];
        snprintf(spec, sizeof spec, "%s", a + 6);
        ((char *)K->args)[0] = 0;
        open_media(spec);
    } else {
        scan_files();
        state = S_PICK;
    }
    redraw_all();
}

static void frame_impl(void)
{
    static int last_focus = -1;
    static unsigned last_redraw;
    struct wctx *c = C;
    if (c->focus != last_focus) { last_focus = c->focus; if (state != S_PICK) redraw_all(); }
    if (state == S_PLAY || state == S_END) {
        int r = vi >= 0 ? overlay_sync() : 0;
        if (r == 2 && has_vlast) show_overlay(vlast);          /* la ventana se movio: el hueco es nuevo */
        pump();
        if ((paused || state == S_END) && has_vlast && ov_on && K->ms() - last_redraw > 250) {
            show_overlay(vlast);                               /* tapar el rastro del cursor */
            last_redraw = K->ms();
        }
        if (ctrl_dirty || K->ms() - ctrl_t > 250) draw_controls();
    }
}

static void key_impl(void)
{
    int k = ev_k;
    if (state == S_PICK) {
        if (k == KEY_UP && fsel > 0) fsel--;
        else if (k == KEY_DOWN && fsel < nfiles - 1) fsel++;
        else if ((k == '\n' || k == '\r') && nfiles) { open_media(files[fsel].spec); redraw_all(); return; }
        else if (k == 'r' || k == 'R') scan_files();
        draw_picker();
        return;
    }
    if (k == ' ') toggle_pause();
    else if (k == KEY_LEFT) seek_to(clock_now() - 10);
    else if (k == KEY_RIGHT) seek_to(clock_now() + 10);
    else if (k == KEY_UP && volume < 16) { volume++; if (snd) K->stream(5, 0, (unsigned)volume); }
    else if (k == KEY_DOWN && volume > 1) { volume--; if (snd) K->stream(5, 0, (unsigned)volume); }
    else if (k == 'a' || k == 'A') { close_media(); overlay_off(); scan_files(); state = S_PICK; C->title(C, "MPLAY"); redraw_all(); return; }
    ctrl_dirty = 1;
}

static void mouse_impl(void)
{
    int i, x = ev_x, y = ev_y, btn = ev_btn, prev = ev_prev;
    if (!((btn & 1) && !(prev & 1))) return;     /* solo al apretar */
    if (state == S_PICK && y < vid_h()) {
        int row = ftop + (y - 46) / 18;
        unsigned now = K->ms();
        if (y >= 46 && row >= 0 && row < nfiles) {
            if (row == fsel && now - last_click < 500) { open_media(files[fsel].spec); redraw_all(); return; }
            fsel = row;
            last_click = now;
            draw_picker();
        }
        return;
    }
    for (i = 0; i < nhits; i++)
        if (inside(x, y, hits[i].x, hits[i].y, hits[i].w, hits[i].h)) {
            switch (hits[i].id) {
            case B_OPEN: close_media(); overlay_off(); scan_files(); state = S_PICK; C->title(C, "MPLAY"); redraw_all(); return;
            case B_PLAY: toggle_pause(); break;
            case B_BACK: seek_to(clock_now() - 10); break;
            case B_FWD: seek_to(clock_now() + 10); break;
            case B_SEEK: if (duration > 0) seek_to(duration * (x - hits[i].x) / hits[i].w); break;
            case B_VDN: if (volume > 1) { volume--; if (snd) K->stream(5, 0, (unsigned)volume); } break;
            case B_VUP: if (volume < 16) { volume++; if (snd) K->stream(5, 0, (unsigned)volume); } break;
            }
            ctrl_dirty = 1;
            return;
        }
    if (y < vid_h() && (state == S_PLAY || state == S_END)) toggle_pause();   /* clic en el video = pausa */
}

static void stop_impl(void)
{
    close_media();
    overlay_off();
    sws_freeContext(sws_fb); sws_fb = 0;
    sws_freeContext(sws_dt); sws_dt = 0;
}

static void w_start(struct wctx *c) { bos_enter(); C = c; on_big(start_impl); }
static void w_frame(struct wctx *c) { bos_enter(); C = c; on_big(frame_impl); }
static void w_key(struct wctx *c, int k) { bos_enter(); C = c; ev_k = k; on_big(key_impl); }
static void w_mouse(struct wctx *c, int x, int y, int btn, int prev)
{
    bos_enter(); C = c; ev_x = x; ev_y = y; ev_btn = btn; ev_prev = prev;
    on_big(mouse_impl);
}
static void w_resize(struct wctx *c) { bos_enter(); C = c; on_big(redraw_all); }
static void w_stop(struct wctx *c) { bos_enter(); C = c; on_big(stop_impl); bos_linux_stop(); }

static struct wapp app = { "MPLAY", 640, 400, 400, 240, 1, w_start, w_frame, w_key, w_mouse, w_resize, w_stop };

static void log_cb(void *p, int lvl, const char *f, va_list vl) { (void)p; (void)lvl; (void)f; (void)vl; }

static int init_main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    av_log_set_callback(log_cb);
    av_log_set_level(AV_LOG_QUIET);
    lut_init();
    if (BI->gfx && BI->fb)
        fbfmt = BI->bpp == 32 ? (BI->rp == 16 ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0) :
                BI->bpp == 24 ? (BI->rp == 16 ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24) :
                BI->bpp == 16 ? AV_PIX_FMT_RGB565LE : AV_PIX_FMT_NONE;
    have_overlay = K->ver >= 6 && K->overlay;
    bigstack = malloc(BIGSTACK);
    bos_resident();                              /* musl sigue vivo: la ventana usa todo esto despues */
    return 0;
}

int mod_main(const struct kapi *k)
{
    uint64_t end = ram_end();
    K = k;
    if (end < NEED_RAM) {
        K->print("\nMPLAY necesita al menos 240 MB de RAM (en QEMU: -m 512).\n");
        return 1;
    }
    if (!BI->gfx || !BI->fb) {
        K->print("\nMPLAY necesita modo grafico.\n");
        return 1;
    }
    if (!bos_linux_start(k, HEAP_LO, end, init_main))
        return 1;
    K->wm_publish(&app);
    return MOD_WINAPP;
}
