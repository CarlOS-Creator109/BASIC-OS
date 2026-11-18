/* binwin.h - aviso compartido "hay un programa de la tienda con ventana abierto".
 *
 * Los programas de la tienda (.BIN) viven en 0x9000000. Si uno abre una ventana en el escritorio, su codigo se queda ahi
 * mientras la ventana este abierta, y cualquiera que use esa memoria (PKG al bajar otro programa, FORO al enviar, COSMOS
 * al bajar una pagina, PLAY al cargar un video) lo pisaria. PKG enciende este aviso al abrir la ventana y lo apaga cuando
 * se cierra; los demas lo miran antes de tocar 0x9000000.
 * El aviso vive en memoria alta fija (ningun modulo usa 0xB2F0000) con dos firmas para no confundirse con basura.
 */
#ifndef BINWIN_H
#define BINWIN_H

#define BINWIN_ADDR  0xB2F0000u
#define BINWIN_MAGIC 0x4E494257u            /* 'WBIN' */

struct binwin {
    unsigned int magic, check;              /* check = ~magic */
    char name[16];                          /* el programa que tiene la ventana */
    unsigned char spare[488];               /* PKG guarda aqui su copia de la struct wapp */
};

#define BINWIN ((volatile struct binwin *)(unsigned long)BINWIN_ADDR)

/* nombre del programa con ventana abierta, o 0 si no hay ninguno. ram = fin de la RAM (no se mira si no llega). */
static inline const char *binwin_busy(unsigned long long ram)
{
    if (ram < BINWIN_ADDR + 0x1000u)
        return 0;
    if (BINWIN->magic == BINWIN_MAGIC && BINWIN->check == ~BINWIN_MAGIC)
        return (const char *)BINWIN->name;
    return 0;
}

static inline void binwin_clear(void)
{
    BINWIN->magic = 0;
    BINWIN->check = 0;
}

#endif
