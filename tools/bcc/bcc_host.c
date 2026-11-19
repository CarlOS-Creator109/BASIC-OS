/* bcc_host.c - BCC para Linux: bcc programa.c [-o NOMBRE.BIN]
 * Hace el mismo .BIN que BCC.BIN dentro de BASIC-OS. Se sube a la tienda con PKG o se copia con mcopy a /APPS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *plat_read(const char *path, int *len);
static void plat_msg(const char *s);

#include "bcc.c"

static char *plat_read(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = zalloc(n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    fclose(f);
    buf[n] = 0;
    *len = (int)n;
    return buf;
}

static void plat_msg(const char *s) { fputs(s, stderr); }

int main(int argc, char **argv)
{
    const char *in = 0, *out = 0;
    char defout[256];
    static u8 obuf[32 << 20];
    long memsize = 512L << 20;
    char *mem;
    int i, olen = 0;
    FILE *f;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) { printf("BCC %s (BASIC-OS COMPILER C)\n", BCC_VERSION); return 0; }
        else if (argv[i][0] == '-') { fprintf(stderr, "uso: bcc programa.c [-o NOMBRE.BIN]\n"); return 2; }
        else in = argv[i];
    }
    if (!in) { fprintf(stderr, "uso: bcc programa.c [-o NOMBRE.BIN]\n"); return 2; }
    if (!out) {
        const char *b = strrchr(in, '/');
        int k = 0;
        b = b ? b + 1 : in;
        while (b[k] && b[k] != '.' && k < 8) { defout[k] = (char)(b[k] >= 'a' && b[k] <= 'z' ? b[k] - 32 : b[k]); k++; }
        strcpy(defout + k, ".BIN");
        out = defout;
    }
    mem = malloc((size_t)memsize);
    if (!mem) { fprintf(stderr, "bcc: sin memoria\n"); return 1; }
    if (bcc_compile(in, obuf, (int)sizeof obuf, &olen, mem, memsize)) {
        fprintf(stderr, "%s\n", errbuf);
        return 1;
    }
    f = fopen(out, "wb");
    if (!f || fwrite(obuf, 1, (size_t)olen, f) != (size_t)olen) { fprintf(stderr, "bcc: no pude escribir %s\n", out); return 1; }
    fclose(f);
    printf("%s: %d bytes\n", out, olen);
    return 0;
}
