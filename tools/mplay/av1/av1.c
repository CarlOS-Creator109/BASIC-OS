/* av1.c - MPLAYAV1.BIN: plugin de MPLAY para video AV1 con dav1d (VideoLAN, BSD-2). Ver tools/mplay/plugin.h. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dav1d/dav1d.h>
#include "../plugin.h"
#include "kapi.h"

/* la cabecera PCBM: base, fin y la entrada (mod_main devuelve el struct del plugin) */
const struct mp_plugin *mod_main(const struct kapi *k);
__asm__(".section .header,\"a\"\n"
        ".long 0x4D424350\n"
        ".long __mod_end\n"
        ".long mod_main\n"
        ".long MODBASE\n"
        ".text\n");

static void *av1_open(const uint8_t *extra, int size)
{
    Dav1dSettings s;
    Dav1dContext *c = 0;
    dav1d_default_settings(&s);
    s.n_threads = 1;                          /* BASIC-OS no tiene hilos */
    s.max_frame_delay = 1;
    s.logger.callback = 0;
    if (dav1d_open(&c, &s) < 0) return 0;
    /* av1C (MP4/MKV): 4 bytes de cabecera y luego el sequence header en OBUs */
    if (extra && size > 4 && (extra[0] & 0x80)) {
        {
            Dav1dData d;
            uint8_t *p = dav1d_data_create(&d, (size_t)(size - 4));
            if (p) {
                memcpy(p, extra + 4, (size_t)(size - 4));
                if (dav1d_send_data(c, &d) < 0) dav1d_data_unref(&d);
                if (d.sz) dav1d_data_unref(&d);
            }
        }
    }
    return c;
}

/* lo que no entro en dav1d se guarda aqui hasta que se saquen cuadros */
struct pend { Dav1dData d; int has; };
static struct pend pending;

static int av1_send(void *h, const uint8_t *data, int size, int64_t pts)
{
    Dav1dContext *c = h;
    int r;
    if (!pending.has) {
        uint8_t *p = dav1d_data_create(&pending.d, (size_t)size);
        if (!p) return -1;
        memcpy(p, data, (size_t)size);
        pending.d.m.timestamp = pts;
        pending.has = 1;
    }
    r = dav1d_send_data(c, &pending.d);
    if (r == DAV1D_ERR(EAGAIN)) return 1;      /* que saquen cuadros y vuelvan a mandar el mismo paquete */
    if (!pending.d.sz) pending.has = 0;
    if (r < 0) { if (pending.has) dav1d_data_unref(&pending.d); pending.has = 0; return -1; }
    return pending.has ? 1 : 0;
}

static int av1_get(void *h, struct mp_picture *out)
{
    Dav1dPicture *pic = malloc(sizeof *pic);
    int r, hbd;
    if (!pic) return -1;
    memset(pic, 0, sizeof *pic);
    r = dav1d_get_picture((Dav1dContext *)h, pic);
    if (r == DAV1D_ERR(EAGAIN)) { free(pic); return 0; }
    if (r < 0) { free(pic); return -1; }
    hbd = pic->p.bpc > 8;
    out->w = pic->p.w;
    out->h = pic->p.h;
    switch (pic->p.layout) {
    case DAV1D_PIXEL_LAYOUT_I400: out->fmt = hbd ? MP_GRAY10 : MP_GRAY8; break;
    case DAV1D_PIXEL_LAYOUT_I422: out->fmt = hbd ? MP_YUV422P10 : MP_YUV422P; break;
    case DAV1D_PIXEL_LAYOUT_I444: out->fmt = hbd ? MP_YUV444P10 : MP_YUV444P; break;
    default: out->fmt = hbd ? MP_YUV420P10 : MP_YUV420P; break;
    }
    out->data[0] = pic->data[0]; out->data[1] = pic->data[1]; out->data[2] = pic->data[2];
    out->stride[0] = pic->stride[0]; out->stride[1] = pic->stride[1]; out->stride[2] = pic->stride[1];
    out->pts = pic->m.timestamp;
    out->priv = pic;
    return 1;
}

static void av1_release(void *h, struct mp_picture *p)
{
    (void)h;
    if (p->priv) { dav1d_picture_unref((Dav1dPicture *)p->priv); free(p->priv); p->priv = 0; }
}

static void av1_flush(void *h)
{
    if (pending.has) { dav1d_data_unref(&pending.d); pending.has = 0; }
    dav1d_flush((Dav1dContext *)h);
}

static void av1_close(void *h)
{
    Dav1dContext *c = h;
    if (pending.has) { dav1d_data_unref(&pending.d); pending.has = 0; }
    dav1d_close(&c);
}

static struct mp_plugin plug = {
    MPPL_MAGIC, MPPL_VERSION, "AV1 (dav1d)", "av1",
    av1_open, av1_send, av1_get, av1_release, av1_flush, av1_close
};

/* MPLAY la llama con k = NULL. Si la llama PKG (k != NULL), solo avisa: el plugin ya quedo guardado en APPS. */
const struct mp_plugin *mod_main(const struct kapi *k)
{
    static char name[48];
    if (k) {
        k->print("MPLAYAV1: plugin de AV1 para MPLAY. Ya quedo instalado; MPLAY lo usa solo cuando un video es AV1.\n");
        return 0;
    }
    strcpy(name, "AV1 (dav1d ");
    strncat(name, dav1d_version(), 20);
    strcat(name, ")");
    plug.name = name;
    return &plug;
}
