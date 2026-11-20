/* tls.h - cliente TLS 1.3 propio de BN (BOS NET), sobre crypto.c. Suite TLS_AES_128_GCM_SHA256, grupo X25519.
 *
 * Parte 3b: hace el handshake y cifra/descifra los datos de aplicacion. TODAVIA NO verifica el certificado del
 * servidor (eso es la 3c), asi que el canal va cifrado pero NO autenticado: sirve para probar, no es seguro aun.
 *
 * El transporte (TCP) se pasa por callbacks, para poder usarlo igual en Linux (sockets, para las pruebas) que
 * dentro de BASIC-OS (la pila TCP de bn.c).
 */
#ifndef BN_TLS_H
#define BN_TLS_H
#include "crypto.h"

typedef int (*tls_send_fn)(void *ctx, const u8 *p, int n);   /* manda n bytes; devuelve n o <0 */
typedef int (*tls_recv_fn)(void *ctx, u8 *p, int n);         /* lee hasta n bytes; >0 leidos, 0 fin, <0 error */

typedef struct {
    void *io; tls_send_fn snd; tls_recv_fn rcv;
    const char *host;                      /* para el SNI */
    void (*log)(const char *s);            /* opcional: mensajes de diagnostico (NULL = callado) */

    sha256_ctx th;                         /* hash del transcript */
    u8 priv[32], pub[32];                  /* X25519 efimero */
    u8 hs_secret[32], cli_hs[32], srv_hs[32];
    u8 cli_key[16], cli_iv[12], srv_key[16], srv_iv[12];
    u8 cli_ap[32], srv_ap[32];
    u8 cli_apkey[16], cli_apiv[12], srv_apkey[16], srv_apiv[12];
    u64 cli_seq, srv_seq;
    int app;                               /* 0 = claves de handshake, 1 = claves de aplicacion */

    u8 rbuf[20000]; int rlen, rpos;        /* buffer de lectura del stream TCP */
    u8 hs[32768]; int hslen, hspos;        /* mensajes de handshake re-ensamblados */
    u8 app_in[20000]; int app_inlen, app_inpos;   /* datos de aplicacion descifrados pendientes */

    /* verificación del certificado (3c) */
    const u8 *roots; int roots_len;        /* raíces de confianza (certs DER concatenados), 0 si no hay */
    u8 certmsg[16384]; int certmsg_len;    /* mensaje Certificate (contenido) */
    u8 cverify[1024]; int cverify_len;     /* mensaje CertificateVerify (contenido) */
    u8 th_cert[32];                        /* hash del transcript hasta Certificate (para CertificateVerify) */
    int verified;                          /* 1 = cadena + firma + host verificados contra una raíz */
} tls_conn;

/* raíces de confianza: certs DER concatenados. Llamar antes de tls_connect. */
void tls_trust(tls_conn *c, const u8 *roots_der, int len);

/* hace el handshake completo. 0 = canal listo; <0 = error */
void tls_seed(u32 s);   /* siembra el generador de la clave efimera (hazlo antes de tls_connect) */
int tls_connect(tls_conn *c, void *io, tls_send_fn snd, tls_recv_fn rcv, const char *host);
/* manda datos de aplicacion cifrados. 0 = ok */
int tls_write(tls_conn *c, const u8 *data, int n);
/* lee datos de aplicacion descifrados (hasta max). >0 leidos, 0 fin/cierre, <0 error */
int tls_read(tls_conn *c, u8 *out, int max);

#endif
