/* x509.h - parser de certificados X.509 (DER) y verificación de firmas/cadena para el TLS de BN. */
#ifndef BN_X509_H
#define BN_X509_H
#include "crypto.h"

enum { SIG_RSA_PKCS1 = 1, SIG_ECDSA = 2, SIG_RSA_PSS = 3 };

typedef struct {
    const u8 *tbs; int tbs_len;             /* tbsCertificate en crudo (lo que se firma) */
    int sig_alg;                            /* SIG_* del algoritmo de la firma del cert */
    int sig_hash;                           /* 32/48/64 (SHA-256/384/512) */
    const u8 *sig; int sig_len;             /* valor de la firma (contenido del BIT STRING) */
    int key_rsa;                            /* 1 = clave pública RSA, 0 = EC P-256 */
    const u8 *n; int nlen; const u8 *e; int elen;   /* RSA */
    u8 ec[64];                              /* EC P-256: X||Y */
    const u8 *issuer; int issuer_len;       /* Name DER del emisor */
    const u8 *subject; int subject_len;     /* Name DER del sujeto */
    const u8 *san; int san_len;             /* extensión SubjectAltName (contenido), 0 si no hay */
    int is_ca;                              /* basicConstraints CA=TRUE */
} cert;

/* parsea un cert DER. 0 = ok, <0 = error */
int x509_parse(const u8 *der, int len, cert *c);
/* verifica que la firma de 'c' es válida con la clave pública de 'ca' (sobre c->tbs). 1 = ok */
int x509_check_sig(const cert *c, const cert *ca);
/* ¿el host casa con algún dNSName del SAN (con comodín *.) ? 1 = sí */
int x509_match_host(const cert *c, const char *host);

#endif
