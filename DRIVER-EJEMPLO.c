/* DRIVER-EJEMPLO.c - plantilla de modulo para BASIC-OS en formato ELF.
 *
 * Esto es TODO lo que necesitas entender para escribir un modulo: incluyes kapi.h, escribes
 * mod_main(K), y hablas con el sistema por el puntero K (la ABI del kernel). No enlazas con el
 * kernel, no usas libc, no necesitas mod.ld ni objcopy.
 *
 * Compilar (gcc pelon, nada mas):
 *
 *   gcc -ffreestanding -nostdlib -nostartfiles -fpic -pie \
 *       -fno-stack-protector -fcf-protection=none -mno-red-zone -mcmodel=small \
 *       -Wl,-e,mod_main -Wl,--no-dynamic-linker \
 *       -o MIDRIVER.ELF DRIVER-EJEMPLO.c
 *
 * Copia MIDRIVER.ELF a la particion (o subelo a la tienda) y el kernel lo carga directo:
 * detecta la firma ELF, reubica el PIE donde toque y salta a mod_main con el kapi real.
 *
 * Reglas de la ABI:
 *  - La unica puerta al sistema es K (struct kapi *). Todo lo demas que uses tiene que estar
 *    dentro de este mismo archivo: no hay printf, malloc, ni nada de libc.
 *  - Para memoria usa K->ram(RAM_ALLOC, ...), no un malloc.
 *  - mod_main devuelve 0 si todo bien.
 */
#include "kapi.h"

int mod_main(const struct kapi *K)
{
    /* saludo */
    K->print("\n  [MIDRIVER] hola desde un modulo ELF cargado por el kernel\n");

    /* ejemplo: leer la hora del sistema */
    {
        unsigned t = K->ms();
        char buf[16]; int i = 0, v = (int)(t / 1000);
        /* mini itoa porque no hay libc */
        char tmp[16]; int j = 0;
        if (v == 0) tmp[j++] = '0';
        while (v > 0) { tmp[j++] = (char)('0' + v % 10); v /= 10; }
        while (j > 0) buf[i++] = tmp[--j];
        buf[i] = 0;
        K->print("  segundos desde el arranque: ");
        K->print(buf);
        K->print("\n");
    }

    return 0;
}
