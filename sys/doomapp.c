/* doomapp.c - DOOM.APP: carga DOOM.BIN (el motor, ~0.4 MB) desde la particion FAT y lo ejecuta
 *
 * No cabe en /SYS, asi que va en la particion grande junto al WAD (ver tools/doom/). DOOM.BIN es un modulo
 * PCBM normal enlazado a 0x4000000; aqui solo se lee, se limpia el bss y se llama a su mod_main.
 * Uso: icono DOOM del escritorio (tecla D) o  EXEC "DOOM.APP"  desde BASIC.
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned int u32;
void *memset(void *dst, int c, unsigned long n);

#define DOOM_BASE 0x4000000u
#define DOOM_MAX  0x600000u         /* hasta 0x4600000 */

struct hdr { u32 magic, end, entry, base; };

static const char *const where[] = {
    "HD0.1:DOOM.BIN", "HD1.1:DOOM.BIN", "HD2.1:DOOM.BIN", "HD0:DOOM.BIN", "HD1:DOOM.BIN",
    "FD1:DOOM.BIN", "FD0:DOOM.BIN", "USB0.1:DOOM.BIN", "USB0:DOOM.BIN", "CD0:DOOM.BIN", 0
};

int mod_main(const struct kapi *K)
{
    unsigned total = 0;
    int i, n = -1;
    char *dst = (char *)(unsigned long)DOOM_BASE;
    const struct hdr *h = (const struct hdr *)dst;
    int (*entry)(const struct kapi *);

    for (i = 0; where[i]; i++) {
        total = 0;
        n = K->read(where[i], dst, (int)DOOM_MAX, &total);
        if (n > 0 && (unsigned)n == total)
            break;
        n = -1;
    }
    if (n < 0) {
        K->print("\nDOOM: NO ENCUENTRO DOOM.BIN EN LA PARTICION FAT\n");
        K->print("(usa la imagen pc-basic-doom.img: ver README, seccion DOOM)\n");
        return 1;
    }
    if (n < (int)sizeof *h || h->magic != 0x4D424350u || h->base != DOOM_BASE ||
        h->end < DOOM_BASE + (unsigned)n || h->end > DOOM_BASE + DOOM_MAX ||
        h->entry < DOOM_BASE || h->entry >= DOOM_BASE + (unsigned)n) {
        K->print("\nDOOM: DOOM.BIN NO ES UN MODULO VALIDO\n");
        return 1;
    }
    memset(dst + n, 0, h->end - (DOOM_BASE + (unsigned)n));
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    return entry(K);
}
