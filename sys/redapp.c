/* redapp.c - RED.APP: carga RED.BIN (la pila de red propia de BASIC-OS) desde la particion y la ejecuta.
 *
 * RED.BIN es un modulo PCBM normal enlazado a 0x4000000 (como DOOM.BIN); aqui solo se lee, se limpia el bss y se
 * llama a su mod_main con el kapi real. Es nuestra red en C puro; iPXE (NET.APP) sigue para HTTPS.
 * Uso:  RED   /   RED "dhcp"   /   RED "ping 10.0.2.2"   (desde BASIC)
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned int u32;
void *memset(void *dst, int c, unsigned long n);

#define RED_BASE 0x4000000u
#define RED_MAX  0x200000u          /* hasta 0x4200000 */

struct hdr { u32 magic, end, entry, base; };

static const char *const where[] = {
    "USB0.1:RED.BIN", "USB1.1:RED.BIN", "USB0:RED.BIN", "HD0.1:RED.BIN", "HD1.1:RED.BIN", "HD2.1:RED.BIN",
    "HD0:RED.BIN", "HD1:RED.BIN", "FD1:RED.BIN", "FD0:RED.BIN", "CD0:RED.BIN", 0
};

int mod_main(const struct kapi *K)
{
    unsigned total = 0;
    int i, n = -1;
    char *dst = (char *)(unsigned long)RED_BASE;
    const struct hdr *h = (const struct hdr *)dst;
    int (*entry)(const struct kapi *);

    for (i = 0; where[i]; i++) {
        total = 0;
        n = K->read(where[i], dst, (int)RED_MAX, &total);
        if (n > 0 && (unsigned)n == total)
            break;
        n = -1;
    }
    if (n < 0) {
        K->print("\nRED: no encuentro RED.BIN en la particion.\n");
        K->print("(va junto a NET.BIN en las imagenes; make lo copia)\n");
        return 1;
    }
    if (n < (int)sizeof *h || h->magic != 0x4D424350u || h->base != RED_BASE ||
        h->end < RED_BASE + (unsigned)n || h->end > RED_BASE + RED_MAX ||
        h->entry < RED_BASE || h->entry >= RED_BASE + (unsigned)n) {
        K->print("\nRED: RED.BIN no es un modulo valido\n");
        return 1;
    }
    memset(dst + n, 0, h->end - (RED_BASE + (unsigned)n));
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    return entry(K);
}
