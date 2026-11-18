/* cosmosapp.c - COSMOS.APP: carga COSMOS.BIN (el navegador con el motor MM de Cosmos Browser) desde la particion FAT
 *
 * No cabe en /SYS (~110 KB), asi que va en la particion grande junto a NET.BIN (ver tools/cosmos/). COSMOS.BIN es un
 * modulo PCBM enlazado a 0x7000000 que entrega su ventana con wm_publish(); aqui solo se lee, se limpia el bss y se
 * llama a su mod_main. Uso: icono COSMOS del escritorio (tecla N) o  EXEC "COSMOS.APP"  desde BASIC.
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned int u32;
void *memset(void *dst, int c, unsigned long n);

#define CM_BASE 0x7000000u
#define CM_MAX  0x400000u           /* codigo + bss hasta 0x7400000; el monticulo del motor va despues */

struct hdr { u32 magic, end, entry, base; };

static const char *const where[] = {
    "USB0.1:COSMOS.BIN", "USB1.1:COSMOS.BIN", "USB0:COSMOS.BIN", "HD0.1:COSMOS.BIN", "HD1.1:COSMOS.BIN",
    "HD2.1:COSMOS.BIN", "HD0:COSMOS.BIN", "HD1:COSMOS.BIN", "FD1:COSMOS.BIN", "FD0:COSMOS.BIN", "CD0:COSMOS.BIN", 0
};

int mod_main(const struct kapi *K)
{
    unsigned total = 0;
    int i, n = -1;
    char *dst = (char *)(unsigned long)CM_BASE;
    const struct hdr *h = (const struct hdr *)dst;
    int (*entry)(const struct kapi *);

    for (i = 0; where[i]; i++) {
        total = 0;
        n = K->read(where[i], dst, (int)CM_MAX, &total);
        if (n > 0 && (unsigned)n == total)
            break;
        n = -1;
    }
    if (n < 0) {
        K->print("\nCOSMOS: NO ENCUENTRO COSMOS.BIN EN LA PARTICION FAT\n");
        K->print("(usa pc-basic-doom.img o super-basicos.img: ver README, seccion COSMOS)\n");
        return 1;
    }
    if (n < (int)sizeof *h || h->magic != 0x4D424350u || h->base != CM_BASE ||
        h->end < CM_BASE + (unsigned)n || h->end > CM_BASE + CM_MAX ||
        h->entry < CM_BASE || h->entry >= CM_BASE + (unsigned)n) {
        K->print("\nCOSMOS: COSMOS.BIN NO ES UN MODULO VALIDO\n");
        return 1;
    }
    memset(dst + n, 0, h->end - (CM_BASE + (unsigned)n));
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    return entry(K);
}
