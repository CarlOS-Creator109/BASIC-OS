/* elfmod.c - cargar modulos en formato ELF64 ademas del PCBM plano.
 *
 * Idea: el loco de la comunidad compila su driver con gcc normal, SIN mod.ld, SIN mod_head.h,
 * SIN objcopy, asi:
 *
 *     gcc -ffreestanding -nostdlib -nostartfiles -fpic -pie -fno-stack-protector \
 *         -fcf-protection=none -mno-red-zone -mcmodel=small -Wl,-e,mod_main \
 *         -o DRIVER.ELF driver.c
 *
 * El driver solo incluye kapi.h y habla con el kernel por el puntero K que recibe mod_main
 * (misma ABI que los modulos PCBM). Al ser PIE (ET_DYN) se puede cargar en cualquier direccion:
 * el loader copia los segmentos y aplica las reubicaciones R_X86_64_RELATIVE el mismo.
 *
 * Tambien acepta ET_EXEC ya enlazado a 'base' (sin reubicaciones), por si alguien quiere la ruta
 * de toda la vida.
 *
 * Este archivo no depende de libc: solo necesita un memcpy y un memset (los del kernel).
 */

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

/* --- estructuras ELF64 (lo justo) --- */
struct ehdr {
    u8  ident[16];
    u16 type;          /* 2 = ET_EXEC, 3 = ET_DYN (PIE) */
    u16 machine;       /* 62 = EM_X86_64 */
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};
struct phdr {
    u32 type;          /* 1 = PT_LOAD, 2 = PT_DYNAMIC */
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};
struct dyn {
    u64 tag;
    u64 val;
};
struct rela {
    u64 offset;
    u64 info;          /* bajo 32 = tipo, alto 32 = simbolo */
    u64 addend;
};

#define ET_EXEC 2
#define ET_DYN  3
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define DT_NULL   0
#define DT_RELA   7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define R_X86_64_RELATIVE 8

#define ELFERR_MAGIC   -2    /* no es ELF */
#define ELFERR_CLASS   -3    /* no es ELF64 x86-64 */
#define ELFERR_TYPE    -4    /* ni ET_EXEC ni ET_DYN */
#define ELFERR_RANGE   -5    /* un segmento no cabe en [dst, dst+cap) */
#define ELFERR_RELOC   -6    /* reubicacion que no se soporta */
#define ELFERR_NOENTRY -7

/* Carga el ELF que esta en 'f' (len bytes) en la memoria 'dst' (cap bytes de capacidad).
 * 'dst' es la direccion real de carga (en el kernel = la base del modulo).
 * Devuelve 0 y deja en *entry_out la direccion absoluta de mod_main; negativo = error. */
int elf64_load(const u8 *f, unsigned len, void *dst, unsigned long cap, unsigned long *entry_out)
{
    const struct ehdr *e;
    unsigned long dstbase = (unsigned long)dst;
    unsigned long bias;
    u64 min_vaddr = ~0ull;
    int i;

    if (len < sizeof(struct ehdr)) return ELFERR_MAGIC;
    e = (const struct ehdr *)f;
    if (e->ident[0] != 0x7f || e->ident[1] != 'E' || e->ident[2] != 'L' || e->ident[3] != 'F')
        return ELFERR_MAGIC;
    if (e->ident[4] != 2 /*ELFCLASS64*/ || e->ident[5] != 1 /*little-endian*/ || e->machine != 62)
        return ELFERR_CLASS;
    if (e->type != ET_EXEC && e->type != ET_DYN) return ELFERR_TYPE;
    if (e->phoff + (u64)e->phnum * e->phentsize > len) return ELFERR_RANGE;

    /* menor vaddr de los PT_LOAD: para PIE (ET_DYN) suele ser 0 y se vuelve dstbase */
    for (i = 0; i < e->phnum; i++) {
        const struct phdr *p = (const struct phdr *)(f + e->phoff + (u64)i * e->phentsize);
        if (p->type == PT_LOAD && p->vaddr < min_vaddr) min_vaddr = p->vaddr;
    }
    if (min_vaddr == ~0ull) return ELFERR_TYPE;      /* sin segmentos cargables */

    /* ET_DYN: lo movemos a dstbase. ET_EXEC: respetamos su vaddr (debe caer en el rango). */
    bias = (e->type == ET_DYN) ? (dstbase - min_vaddr) : 0;

    /* copiar cada PT_LOAD a vaddr+bias, con el bss (memsz>filesz) en cero */
    for (i = 0; i < e->phnum; i++) {
        const struct phdr *p = (const struct phdr *)(f + e->phoff + (u64)i * e->phentsize);
        unsigned long at;
        if (p->type != PT_LOAD) continue;
        at = (unsigned long)(p->vaddr + bias);
        if (at < dstbase || at + p->memsz > dstbase + cap) return ELFERR_RANGE;
        if (p->offset + p->filesz > len) return ELFERR_RANGE;
        memcpy((void *)at, f + p->offset, (unsigned long)p->filesz);
        if (p->memsz > p->filesz)
            memset((void *)(at + p->filesz), 0, (unsigned long)(p->memsz - p->filesz));
    }

    /* reubicaciones: para PIE solo deberian venir R_X86_64_RELATIVE (punteros globales) */
    if (e->type == ET_DYN) {
        u64 rela = 0, relasz = 0, relaent = sizeof(struct rela);
        for (i = 0; i < e->phnum; i++) {
            const struct phdr *p = (const struct phdr *)(f + e->phoff + (u64)i * e->phentsize);
            const struct dyn *d;
            if (p->type != PT_DYNAMIC) continue;
            d = (const struct dyn *)(f + p->offset);
            for (; (const u8 *)d < f + p->offset + p->filesz && d->tag != DT_NULL; d++) {
                if (d->tag == DT_RELA)    rela    = d->val;   /* vaddr de la tabla */
                else if (d->tag == DT_RELASZ)  relasz  = d->val;
                else if (d->tag == DT_RELAENT) relaent = d->val;
            }
        }
        if (relasz) {
            u64 off;
            for (off = 0; off < relasz; off += relaent) {
                /* la tabla RELA vive dentro de un PT_LOAD, asi que ya esta en memoria en rela+bias */
                const struct rela *r = (const struct rela *)(unsigned long)(rela + bias + off);
                u32 t = (u32)(r->info & 0xffffffffu);
                if (t == 0 /*R_X86_64_NONE*/) continue;
                if (t != R_X86_64_RELATIVE) return ELFERR_RELOC;
                /* *(ubicacion) = bias + addend */
                *(u64 *)(unsigned long)(r->offset + bias) = (u64)bias + r->addend;
            }
        }
    }

    if (!e->entry) return ELFERR_NOENTRY;
    *entry_out = (unsigned long)(e->entry + bias);
    return 0;
}
