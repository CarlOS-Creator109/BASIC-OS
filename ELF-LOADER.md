# Cargar módulos ELF en BASIC-OS

Hasta ahora los módulos (BN, DOOM, GUI, apps) son binarios **PCBM planos**: enlazados a una
dirección fija con `mod.ld`, con la cabecera asm de `mod_head.h`, aplanados con `objcopy -O binary`.
Funciona, pero para alguien de fuera es un baile de herramientas.

`elfmod.c` agrega un camino alterno: el kernel también carga **ELF64**. Así quien escriba un driver
o programa solo tiene que entender **la ABI (`kapi.h`)** — nada del build especial.

## Para quien escribe el módulo (la comunidad)

Una sola orden de `gcc`, sin `mod.ld`, sin `objcopy`, sin `mod_head.h`:

```sh
gcc -ffreestanding -nostdlib -nostartfiles -fpic -pie \
    -fno-stack-protector -fcf-protection=none -mno-red-zone -mcmodel=small \
    -Wl,-e,mod_main -Wl,--no-dynamic-linker \
    -I. -o MIDRIVER.ELF midriver.c
```

Reglas: incluir `kapi.h`, escribir `int mod_main(const struct kapi *K)`, y hablar con el sistema
solo por `K`. Nada de libc. Ver `DRIVER-EJEMPLO.c`. Al ser **PIE** el binario se carga en cualquier
dirección: el loader lo reubica (solo trae `R_X86_64_RELATIVE`).

## Para el kernel (enganche)

`elf64_load()` es una función pura: recibe los bytes del ELF ya leídos, la memoria destino y su
capacidad, y deja la dirección absoluta de `mod_main`. No hace I/O: tú lees el archivo a un buffer
de rascado (scratch) y ella reparte los segmentos a la base del módulo.

En `module.c` (y en los loaders tipo `bnapp.c`), detecta la firma y despacha:

```c
#include "elfmod.h"

/* ... dentro del loader, 'dst' = base del módulo, 'cap' = su tamaño máximo ... */

/* 1) lee el archivo a un scratch (NO a 'dst': el layout del ELF ≠ imagen en memoria) */
n = sys_read(name, scratch, SCRATCH_MAX, &total);

if (is_elf(scratch, n)) {
    unsigned long entry;
    int r = elf64_load((const unsigned char *)scratch, (unsigned)total,
                       dst, cap, &entry);
    if (r) return MODERR_BADHDR;                 /* o un código propio por r */
    return ((int (*)(const struct kapi *))entry)(&kernel_api);
}

/* si no es ELF, sigue tu ruta PCBM de siempre (magic 'PCBM', etc.) */
```

`scratch` tiene que ser una zona libre lo bastante grande para el archivo ELF (puede ser la parte
alta de la propia región del módulo, o un buffer dedicado — tú conoces tu mapa de memoria). Para los
módulos de `/SYS` basta con `MOD_MAX`; para los grandes (estilo BN) usa su `*_MAX`.

### Soporta

- **ET_DYN (PIE)** — recomendado; reubicable, solo `R_X86_64_RELATIVE`. Es lo que produce la receta.
- **ET_EXEC** — ya enlazado a `base` (sin reubicaciones), por si alguien quiere la ruta clásica.

No soporta símbolos externos ni GOT/PLT entre módulos: la ABI es el `struct kapi`, igual que PCBM.
Si aparece una reubicación que no sea RELATIVE, `elf64_load` devuelve `ELFERR_RELOC` (-6) en vez de
cargar algo roto.

## Probarlo (en el host, sin bootear)

`tools/elf/elftest.c` carga un ELF con `elf64_load` y llama a su `mod_main` con un kapi de juguete.
Compila el driver de prueba PIE y debe imprimir y devolver 60.
