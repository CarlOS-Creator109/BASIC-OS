/* rt_pack.c - runtime del modo empaquetador de bas2bin: el .BIN lleva el texto del programa BASIC y lo corre
 * con el interprete del kernel. Va embebido en el compilador (b2b_blobs.h). GPL-3.0-or-later. */
#include "kapi.h"
#include "mod_head.h"
#include "tools/bas2bin/b2b.h"
#include "sys/binicon.h"

/* lugar para el icono del escritorio: bas2bin lo llena si el programa trae lineas REM ICONO / REM ICB */
__attribute__((section(".header.icon"), used, aligned(4)))
static const struct bin_icon b2b_icon_slot = { B2B_ISLOT, "", "" };

struct b2b_pext b2b_ext = { B2B_PMAGIC, 0, 0, 0 };     /* text/len los llena el compilador */

int mod_main(const struct kapi *K)
{
    if (!b2b_ext.text || !b2b_ext.len) {
        K->print("bas2bin: paquete vacio\n");
        return 1;
    }
    K->load_basic((const char *)(unsigned long)b2b_ext.text, (int)b2b_ext.len);
    if (K->ver >= 5)
        K->basic_do("RUN");                   /* sin READY y sin tocar lo que estuviera corriendo */
    else
        K->basic_run("RUN");
    return 0;
}
