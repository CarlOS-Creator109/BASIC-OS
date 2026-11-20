/* x509test.c - prueba el parser X.509 y la verificación de cadena con certs DER reales.
 * uso: x509test hoja.der [intermedio.der [raiz.der]] [host] */
#include "x509.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int load(const char *path, u8 *buf, int max) { FILE *f = fopen(path, "rb"); if (!f) return -1; int n = (int)fread(buf, 1, max, f); fclose(f); return n; }

int main(int argc, char **argv)
{
    static u8 der[3][8192]; static cert c[3]; int n = 0, i; const char *host = "en.wikipedia.org";
    for (i = 1; i < argc && n < 3; i++) {
        int l = load(argv[i], der[n], sizeof der[0]);
        if (l < 0) { host = argv[i]; continue; }
        if (x509_parse(der[n], l, &c[n])) { printf("FALLO al parsear %s\n", argv[i]); return 1; }
        printf("cert %d: %s, clave %s %d bytes, %s, SAN %s\n", n,
               c[n].sig_alg==SIG_ECDSA?"ECDSA":(c[n].sig_alg==SIG_RSA_PSS?"RSA-PSS":"RSA-PKCS1"),
               c[n].key_rsa?"RSA":"EC", c[n].key_rsa?c[n].nlen:64, c[n].is_ca?"CA":"hoja", c[n].san?"sí":"no");
        n++;
    }
    if (!n) { printf("sin certs\n"); return 1; }

    printf("\nCadena:\n");
    for (i = 0; i < n; i++) {
        const cert *ca = (i + 1 < n) ? &c[i + 1] : &c[n - 1];   /* la última se verifica contra sí misma (raíz) */
        int ok = x509_check_sig(&c[i], ca);
        printf("  cert %d firmado por cert %d: %s\n", i, (i+1<n)?i+1:n-1, ok ? "VÁLIDO" : "INVÁLIDO");
    }
    printf("\nhost '%s' en el SAN de la hoja: %s\n", host, x509_match_host(&c[0], host) ? "sí" : "NO");
    return 0;
}
