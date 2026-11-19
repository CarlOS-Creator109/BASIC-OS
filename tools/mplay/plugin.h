/* plugin.h - plugins de decodificadores para MPLAY (por ejemplo MPLAYAV1.BIN con dav1d)
 *
 * Un plugin es un .BIN aparte que MPLAY carga solo cuando hace falta (un video AV1 y FFmpeg no tiene con que leerlo).
 * Se guarda junto a MPLAY.BIN en la carpeta APPS (PKG lo baja como cualquier paquete) y se carga en PLUGIN_BASE, encima
 * de MPLAY y dentro de los 32 MB que BASIC-OS reserva para un programa de la tienda.
 * Formato: la cabecera PCBM normal (firma, fin, entrada, base = PLUGIN_BASE). La entrada no recibe nada y devuelve un
 * puntero a struct mp_plugin. El plugin trae su propia musl (enlazada estatica) y usa la capa Linux de MPLAY: malloc,
 * errno, etc. funcionan igual porque comparten SYSCALL y el puntero de hilo.
 */
#ifndef MPLAY_PLUGIN_H
#define MPLAY_PLUGIN_H
#include <stdint.h>
#include <stddef.h>

#define PLUGIN_BASE  0xAA00000u
#define PLUGIN_LIMIT 0xB000000u
#define MPPL_MAGIC   0x4C50504Du             /* 'MPPL' */
#define MPPL_VERSION 1

enum { MP_YUV420P = 0, MP_YUV420P10, MP_YUV422P, MP_YUV422P10, MP_YUV444P, MP_YUV444P10, MP_GRAY8, MP_GRAY10 };

struct mp_picture {
    int w, h, fmt;
    uint8_t *data[3];
    ptrdiff_t stride[3];
    int64_t pts;
    void *priv;                               /* del plugin (para release) */
};

struct mp_plugin {
    uint32_t magic, version;
    const char *name;                         /* "AV1 (dav1d 1.5.1)" */
    const char *codec;                        /* nombre del codec de FFmpeg que cubre: "av1" */
    void *(*open)(const uint8_t *extradata, int size);
    int  (*send)(void *h, const uint8_t *data, int size, int64_t pts);   /* 0 = bien, 1 = antes saca cuadros (get), <0 = error */
    int  (*get)(void *h, struct mp_picture *out);                        /* 1 = hay cuadro, 0 = no, <0 = error */
    void (*release)(void *h, struct mp_picture *p);
    void (*flush)(void *h);
    void (*close)(void *h);
};
#endif
