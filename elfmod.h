/* elfmod.h - cargar modulos ELF64 (PIE o ET_EXEC) ademas del formato PCBM plano. */
#ifndef ELFMOD_H
#define ELFMOD_H

/* Carga el ELF que esta en 'f' (len bytes) a la memoria 'dst' (cap bytes de capacidad).
 * 'dst' es la direccion real de carga; para PIE (ET_DYN) el loader reubica solo.
 * Deja en *entry_out la direccion absoluta de mod_main.
 * Devuelve 0 si todo bien, o un ELFERR_* (< 0). */
int elf64_load(const unsigned char *f, unsigned len, void *dst,
               unsigned long cap, unsigned long *entry_out);

/* 1 si los primeros bytes de 'buf' son la firma ELF (\x7fELF); 0 si no. */
static inline int is_elf(const void *buf, int n)
{
    const unsigned char *b = (const unsigned char *)buf;
    return n >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

#endif
