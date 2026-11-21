/* module.c - carga y ejecuta modulos de /SYS
 *
 * Cabecera al principio del binario (ver mod_head.h): firma 'PCBM', direccion final del bss,
 * direccion de mod_main. Se lee el archivo a su direccion de enlace, se pone a cero el bss y
 * se llama a mod_main(&api).
 */
#include "kernel.h"
#include "fs.h"
#include "kapi.h"
#include "module.h"
#include "sys.h"
#include "elfmod.h"

extern const struct kapi kernel_api;      /* kernel.c */

#define ELF_SCRATCH 0x2000000u            /* 32 MB: un banco de RAM para el archivo ELF antes de repartirlo */

struct modhdr {
    u32 magic;
    u32 end;
    u32 entry;
    u32 reserved;
};

/* Carga "SYS:name" en la direccion que dice su cabecera y lo ejecuta. base != 0 solo se usa si
 * el modulo no declara direccion. */
int mod_exec(const char *name, unsigned int base)
{
    struct modhdr probe;
    char *dst;
    const struct modhdr *h;
    unsigned int total = 0;
    int n;
    int (*entry)(const struct kapi *);

    n = sys_read(name, (char *)&probe, (int)sizeof probe, &total);
    if (n < 0)
        return MODERR_NOFILE;

    /* Modulo ELF: lo cargamos con elf64_load. Scratch = un banco de RAM prestado (no una direccion fija),
     * porque el layout del archivo ELF no es la imagen en memoria. El driver se compila PIE con gcc normal. */
    if (is_elf(&probe, n)) {
        unsigned long long sc;
        unsigned long entry_addr;
        int r;
        if (base == 0) base = 0x400000u;      /* PIE: no trae base, el kernel la elige (slot de app) */
        if (base < 0x300000u || base + MOD_MAX > 0xF00000u || (base & 0xFFF))
            return MODERR_BADHDR;
        sc = ram_api(RAM_ALLOC, 0, 1, "ELFLOAD");
        if (!sc)
            return MODERR_NOMEM;
        total = 0;
        n = sys_read(name, (char *)(unsigned long)sc, (int)ELF_SCRATCH, &total);
        if (n < 0 || total == 0) { ram_api(RAM_FREE, sc, 0, 0); return MODERR_NOFILE; }
        r = elf64_load((const unsigned char *)(unsigned long)sc, total,
                       (void *)(unsigned long)base, MOD_MAX, &entry_addr);
        ram_api(RAM_FREE, sc, 0, 0);
        if (r)
            return MODERR_BADHDR;
        entry = (int (*)(const struct kapi *))entry_addr;
        return entry(&kernel_api);
    }

    if (n < (int)sizeof probe || probe.magic != 0x4D424350u)
        return MODERR_BADHDR;
    if (probe.reserved)
        base = probe.reserved;
    if (base < 0x300000u || base + MOD_MAX > 0xF00000u || (base & 0xFFF))
        return MODERR_BADHDR;
    dst = (char *)(unsigned long)base;
    h = (const struct modhdr *)dst;
    n = sys_read(name, dst, (int)MOD_MAX, &total);
    if (n < 0)
        return MODERR_NOFILE;
    if (total > MOD_MAX)
        return MODERR_TOOBIG;
    if (n < (int)sizeof *h || h->magic != 0x4D424350u || h->end < base + (unsigned)n ||
        h->end > base + MOD_MAX || h->entry < base || h->entry >= base + (unsigned)n)
        return MODERR_BADHDR;
    memset(dst + n, 0, h->end - (base + (unsigned)n));
    entry = (int (*)(const struct kapi *))(unsigned long)h->entry;
    return entry(&kernel_api);
}
