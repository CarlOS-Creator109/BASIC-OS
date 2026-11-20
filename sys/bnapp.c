/* bnapp.c - BN.APP: carga BN.BIN (la pila de red propia de BASIC-OS (BOS NET)) desde la particion y la ejecuta.
 *
 * BN.BIN es un modulo PCBM normal enlazado a 0x4000000 (como DOOM.BIN); aqui solo se lee, se limpia el bss y se
 * llama a su mod_main con el kapi real. Es BN (BOS NET), nuestra red en C puro; iPXE (NET.APP) sigue para HTTPS.
 * Uso:  RED   /   BN "dhcp"   /   BN "ping 10.0.2.2"   (desde BASIC)
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned int u32;
void *memset(void *dst, int c, unsigned long n);

#define BN_BASE 0x4000000u
#define BN_MAX  0x200000u          /* hasta 0x4200000 */

struct hdr { u32 magic, end, entry, base; };

static const char *const where[] = {
    "USB0.1:BN.BIN", "USB1.1:BN.BIN", "USB0:BN.BIN", "HD0.1:BN.BIN", "HD1.1:BN.BIN", "HD2.1:BN.BIN",
    "HD0:BN.BIN", "HD1:BN.BIN", "FD1:BN.BIN", "FD0:BN.BIN", "CD0:BN.BIN", 0
};

int mod_main(const struct kapi *K)
{
    unsigned total = 0;
    int i, n = -1;
    char *dst = (char *)(unsigned long)BN_BASE;
    const struct hdr *h = (const struct hdr *)dst;
    int (*entry)(const struct kapi *);

    for (i = 0; where[i]; i++) {
        total = 0;
        n = K->read(where[i], dst, (int)BN_MAX, &total);
        if (n > 0 && (unsigned)n == total)
            break;
        n = -1;
    }
    if (n < 0) {
        K->print("\nRED: no encuentro BN.BIN en la particion.\n");
        K->print("(va junto a NET.BIN en las imagenes; make lo copia)\n");
        return 1;
    }
    if (n < (int)sizeof *h || h->magic != 0x4D424350u || h->base != BN_BASE ||
        h->end < BN_BASE + (unsigned)n || h->end > BN_BASE + BN_MAX ||
        h->entry < BN_BASE || h->entry >= BN_BASE + (unsigned)n) {
        K->print("\nRED: BN.BIN no es un modulo valido\n");
        return 1;
    }
    memset(dst + n, 0, h->end - (BN_BASE + (unsigned)n));
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    return entry(K);
}
