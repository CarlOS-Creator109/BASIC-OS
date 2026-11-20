/* crypto.h - primitivas de criptografia para el TLS propio de BN (BOS NET).
 * Todo en C puro, sin dependencias. Las implementa crypto.c; crypttest.c las prueba contra sus vectores oficiales.
 *   SHA-256, HMAC-SHA256, HKDF-SHA256 (TLS 1.3 key schedule)
 *   AES-128 y AES-128-GCM (la suite TLS_AES_128_GCM_SHA256)
 *   X25519 (intercambio de claves de TLS 1.3)
 */
#ifndef BN_CRYPTO_H
#define BN_CRYPTO_H

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

/* ---- SHA-256 ---- */
typedef struct { u32 h[8]; u64 len; u8 buf[64]; int n; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const u8 *p, u64 n);
void sha256_final(sha256_ctx *c, u8 out[32]);
void sha256(const u8 *p, u64 n, u8 out[32]);

/* ---- HMAC / HKDF (SHA-256) ---- */
void hmac_sha256(const u8 *key, int klen, const u8 *msg, int mlen, u8 out[32]);
void hkdf_extract(const u8 *salt, int slen, const u8 *ikm, int ilen, u8 prk[32]);
void hkdf_expand(const u8 *prk, const u8 *info, int ilen, u8 *out, int olen);

/* ---- AES-128 ---- */
typedef struct { u32 rk[44]; } aes128;
void aes128_init(aes128 *a, const u8 key[16]);
void aes128_encrypt(const aes128 *a, const u8 in[16], u8 out[16]);

/* ---- AES-128-GCM ----
 * cifra: pone el texto cifrado en out[n] y el tag en tag[16].
 * descifra: devuelve 0 si el tag cuadra (y escribe el texto claro), 1 si no. */
void aes128_gcm_encrypt(const u8 key[16], const u8 *iv, int ivl, const u8 *aad, int al,
                        const u8 *in, int n, u8 *out, u8 tag[16]);
int  aes128_gcm_decrypt(const u8 key[16], const u8 *iv, int ivl, const u8 *aad, int al,
                        const u8 *in, int n, const u8 tag[16], u8 *out);

/* ---- X25519 ---- */
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32]);
void x25519_base(u8 out[32], const u8 scalar[32]);   /* punto = 9 (clave publica) */

/* ---- SHA-384 / SHA-512 (para firmas de certificados) ---- */
typedef struct { u64 h[8]; u64 len; u8 buf[128]; int n; } sha512_ctx;
void sha512(const u8 *p, u64 n, u8 out[64]);
void sha384(const u8 *p, u64 n, u8 out[48]);
void hash_by_len(int hashlen, const u8 *d, int dl, u8 *out);   /* 32/48/64 -> SHA-256/384/512 */

/* ---- ECDSA sobre P-256 (secp256r1): verificar una firma ----
 * pub = 64 bytes (X||Y, 32 cada uno, big-endian). hash = resumen del mensaje (hlen bytes).
 * r,s = 32 bytes cada uno (big-endian). Devuelve 1 si la firma es valida, 0 si no. */
int ecdsa_p256_verify(const u8 pub[64], const u8 *hash, int hlen, const u8 r[32], const u8 s[32]);

/* ---- RSA (verificar firma; exponente publico pequeno) ---- */
/* n,e = modulo y exponente (big-endian). Devuelve 1 si la firma vale, 0 si no. */
int rsa_pkcs1_verify(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int siglen, const u8 *hash, int hashlen);
int rsa_pss_verify(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int siglen, const u8 *mhash, int hashlen);

#endif
