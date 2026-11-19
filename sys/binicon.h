/* binicon.h - icono de escritorio dentro de un programa de la tienda (.BIN)
 *
 * Un .BIN puede llevar, justo detras de su cabecera de 16 bytes, un bloque 'BICN' con el nombre que se ve bajo el icono
 * y el dibujo en formato .ICB (texto: primera linea "ICB", luego una fila por linea, un digito hex por bloque de color del
 * C64 y '.' = transparente; ver sys/ui.h). El escritorio mira los .BIN de la carpeta APPS del disco (donde los guarda PKG)
 * y pone un icono por cada uno que traiga este bloque; con un clic lo carga en 0x9000000 y lo ejecuta (si devuelve
 * MOD_WINAPP, su ventana se abre en el escritorio).
 *
 *   #include "mod_head.h"
 *   #include "sys/binicon.h"
 *   BIN_ICON("MPLAY", "ICB\n" "....0000....\n" ...);
 *
 * El linker (mod.ld, mplay.ld) pone la seccion .header.icon justo despues de .header.
 */
#ifndef BINICON_H
#define BINICON_H

#define BICN_MAGIC 0x4E434942u              /* 'BICN' */
#define BICN_OFF   16                       /* despues de la cabecera PCBM */
#define BICN_ICB   400

struct bin_icon {
    unsigned int magic;
    char label[16];                         /* lo que se lee bajo el icono (en MAYUSCULAS queda mejor) */
    char icb[BICN_ICB];
};

#define BIN_ICON(lbl, icbtext)                                                                   \
    __attribute__((section(".header.icon"), used, aligned(4)))                                 \
    static const struct bin_icon bin_icon_block = { BICN_MAGIC, lbl, icbtext }

#endif
