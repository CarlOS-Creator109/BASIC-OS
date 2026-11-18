/* b2b.h - bas2bin: de programa BASIC (.BAS) a modulo de la tienda (.BIN, PCBM en 0x9000000)
 *
 * Dos modos:
 *   empaquetador  el .BIN lleva el texto del programa y lo corre con el interprete del kernel (todo el lenguaje).
 *   nativo        el programa se traduce a codigo x86-64. Lo que el compilador no traduce el mismo (PLOT, SOUND,
 *                 PEEK, LOCATE...) se calcula en nativo y se le pasa al interprete ya con numeros (kapi 5).
 * Cada modo tiene su "runtime": un modulo PCBM ya compilado (rt_pack.c / rt_native.c) que va embebido en el
 * compilador (b2b_blobs.h). El compilador copia el runtime, le pega detras el programa y parcha unos campos.
 * Mismo codigo en Linux (tools/bas2bin/bas2bin) y dentro de PKG.APP (tecla C).  GPL-3.0-or-later.
 */
#ifndef B2B_H
#define B2B_H

#define B2B_BASE   0x9000000u               /* los programas de la tienda se enlazan aqui */
#define B2B_PMAGIC 0x50423242u              /* 'B2BP' extension del runtime empaquetador */
#define B2B_NMAGIC 0x4E423242u              /* 'B2BN' extension del runtime nativo */

/* runtime empaquetador: el compilador llena text/len (direccion absoluta del texto y su largo) */
struct b2b_pext { unsigned int magic, text, len, pad; };

/* runtime nativo: lo llena el enlazador (direcciones de las variables del runtime); code/lines/nlines los
 * parcha el compilador. Todo es de 64 bits: el modo nativo solo existe en PC BASIC de 64 bits. */
struct b2b_next {
    unsigned int magic, ver;
    unsigned long long vars, svars, saved_rsp, fns, code, lines, nlines;
};
struct b2b_line { int num; unsigned int addr; };

/* indices de la tabla de funciones del runtime nativo (fns[i], 8 bytes cada una) */
enum {
    RT_LINE, RT_PRN, RT_PRS, RT_PRTAB, RT_PRNL, RT_INN, RT_INS, RT_DIV, RT_MOD, RT_FN,
    RT_LEN, RT_ASC, RT_VAL, RT_CHR, RT_STR, RT_LEFT, RT_RIGHT, RT_MID, RT_INKEY, RT_CAT,
    RT_SSET, RT_SCMP, RT_FOR, RT_NEXT, RT_GOTO, RT_GPUSH, RT_GPOP, RT_STOP, RT_FAIL,
    RT_CB_BEGIN, RT_CB_NUM, RT_CB_STR, RT_CB_WORD, RT_CB_DO, RT_EV_BEGIN, RT_EV_END,
    RT_NFUNCS
};
enum { FN_ABS, FN_SGN, FN_SQR, FN_RND };

/* compila src (texto BASIC, lineas "10 PRINT ...") a out. native = 1 nativo, 0 empaquetador.
 * Devuelve el tamano del .BIN, o -1 con el error en msg. */
int b2b_compile(const char *src, int n, int native, unsigned char *out, int max, char *msg, int msgmax);

#endif
