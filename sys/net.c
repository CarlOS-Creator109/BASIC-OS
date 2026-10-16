/* net.c - NET.APP: carga NET.BIN (iPXE para BASIC-OS) desde la particion FAT y lo ejecuta
 *
 * NET.BIN es el nucleo de iPXE (drivers de red + pila TCP/IP) enlazado a 0x700000 (tools/net/build.sh).
 * No cabe en /SYS, asi que vive en la particion grande (HD0.1:, ver tools/mkbashimg.c).
 * Uso desde BASIC:  NET              consola de iPXE (dhcp, ping, imgfetch...; "exit" vuelve)
 *                   NET "dhcp"       ejecuta un comando y vuelve
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char u8;
typedef unsigned int u32;

#define NET_BASE 0x700000u
#define NET_MAX  0x800000u          /* hasta 0xF00000 */

struct bos_boot {
    int (*poll)(void);
    void (*print)(const char *s);
    void *(*map_mmio)(unsigned long long phys, unsigned size);
    unsigned (*ms)(void);
    int (*read)(const char *spec, char *buf, int max, unsigned *total);
    int (*write)(const char *spec, const void *buf, unsigned n);
    const char *args;
};

struct hdr { u32 magic, end, entry, base; };

static const char *const where[] = {
    "HD0.1:NET.BIN", "HD1.1:NET.BIN", "HD2.1:NET.BIN", "HD0:NET.BIN", "HD1:NET.BIN", "FD1:NET.BIN", "FD0:NET.BIN", 0
};

int mod_main(const struct kapi *K)
{
    static struct bos_boot b;
    const struct hdr *h = (const struct hdr *)NET_BASE;
    char *dst = (char *)NET_BASE;
    unsigned total = 0;
    int i, n = -1;
    int (*entry)(const struct bos_boot *);

    for (i = 0; where[i]; i++) {
        n = K->read(where[i], dst, (int)NET_MAX, &total);
        if (n > 16)
            break;
    }
    if (n <= 16) {
        K->print("NET.BIN no esta en el disco. Armalo con tools/net/build.sh y copialo a la particion\n"
                 "(tools/linux/mktinycore.sh ya lo incluye si existe tools/net/NET.BIN).\n");
        return 1;
    }
    if (h->magic != 0x4D424350u || h->base != NET_BASE || h->end < NET_BASE + (unsigned)n ||
        h->end > NET_BASE + NET_MAX || h->entry < NET_BASE || h->entry >= NET_BASE + (unsigned)n) {
        K->print("NET.BIN esta danado o no es de esta version\n");
        return 1;
    }
    for (u8 *p = (u8 *)(NET_BASE + (unsigned)n); p < (u8 *)h->end; p++)
        *p = 0;
    b.poll = K->poll;
    b.print = K->print;
    b.map_mmio = K->map_mmio;
    b.ms = K->ms;
    b.read = K->read;
    b.write = 0;
    b.args = K->args;
    entry = (int (*)(const struct bos_boot *))(unsigned long)h->entry;
    return entry(&b);
}
