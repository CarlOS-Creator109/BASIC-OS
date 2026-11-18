/* main.c - bas2bin para Linux: compila un programa de PC BASIC (.BAS) a un modulo de la tienda (.BIN).
 *   cc -O2 -I. -o tools/bas2bin/bas2bin tools/bas2bin/main.c        (lo hace make)
 *   bas2bin [-p] JUEGO.BAS JUEGO.BIN       -p = empaquetador (sin -p: nativo)
 * El .BIN se sube con PKG (tecla S) o se pone en HD0.1:/APPS/ y PKG lo ejecuta.  GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tools/bas2bin/b2b.c"

int main(int argc, char **argv)
{
    int nat = 1, i, n, r;
    const char *in = 0, *outp = 0;
    static char src[1 << 20];
    static unsigned char out[4 << 20];
    char msg[160];
    FILE *f;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--paquete")) nat = 0;
        else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--nativo")) nat = 1;
        else if (!in) in = argv[i];
        else if (!outp) outp = argv[i];
        else in = 0;
    }
    if (!in || !outp) {
        fprintf(stderr, "uso: bas2bin [-p|-n] PROGRAMA.BAS PROGRAMA.BIN\n"
                        "  -n  nativo (por defecto): codigo x86-64, solo PC BASIC de 64 bits\n"
                        "  -p  empaquetador: el programa va dentro y lo corre el interprete\n");
        return 2;
    }
    if (!(f = fopen(in, "rb"))) { perror(in); return 1; }
    n = (int)fread(src, 1, sizeof src - 1, f);
    fclose(f);
    r = b2b_compile(src, n, nat, out, (int)sizeof out, msg, (int)sizeof msg);
    if (r < 0) { fprintf(stderr, "bas2bin: %s: %s\n", in, msg); return 1; }
    if (!(f = fopen(outp, "wb")) || fwrite(out, 1, (size_t)r, f) != (size_t)r || fclose(f)) { perror(outp); return 1; }
    printf("bas2bin: %s -> %s (%s, %d bytes)\n", in, outp, nat ? "nativo" : "empaquetado", r);
    return 0;
}
