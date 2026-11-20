/* crypto.c - primitivas de criptografia para el TLS de BN. C puro, sin libc (solo un memcpy/memset propios). */
#include "crypto.h"

static void xmemcpy(void *d, const void *s, u64 n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; }
static void xmemset(void *d, int c, u64 n) { u8 *a = d; while (n--) *a++ = (u8)c; }

/* ================================================================= SHA-256 ============ */
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_block(sha256_ctx *ctx, const u8 *p)
{
    u32 w[64], A, B, C, D, E, F, G, H, s0, s1, t1, t2, ch, maj; int i;
    for (i = 0; i < 16; i++) w[i] = (u32)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (i = 16; i < 64; i++) {
        s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    A = ctx->h[0]; B = ctx->h[1]; C = ctx->h[2]; D = ctx->h[3];
    E = ctx->h[4]; F = ctx->h[5]; G = ctx->h[6]; H = ctx->h[7];
    for (i = 0; i < 64; i++) {
        s1 = ROR(E,6) ^ ROR(E,11) ^ ROR(E,25); ch = (E & F) ^ (~E & G);
        t1 = H + s1 + ch + K256[i] + w[i];
        s0 = ROR(A,2) ^ ROR(A,13) ^ ROR(A,22); maj = (A & B) ^ (A & C) ^ (B & C);
        t2 = s0 + maj;
        H = G; G = F; F = E; E = D + t1; D = C; C = B; B = A; A = t1 + t2;
    }
    ctx->h[0] += A; ctx->h[1] += B; ctx->h[2] += C; ctx->h[3] += D;
    ctx->h[4] += E; ctx->h[5] += F; ctx->h[6] += G; ctx->h[7] += H;
}
void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->len = 0; c->n = 0;
}
void sha256_update(sha256_ctx *c, const u8 *p, u64 n)
{
    c->len += n;
    while (n) {
        int take = 64 - c->n; if ((u64)take > n) take = (int)n;
        xmemcpy(c->buf + c->n, p, take); c->n += take; p += take; n -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}
void sha256_final(sha256_ctx *c, u8 out[32])
{
    u64 bits = c->len * 8; int i; u8 pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0; while (c->n != 56) sha256_update(c, &pad, 1);
    u8 lb[8]; for (i = 0; i < 8; i++) lb[i] = (u8)(bits >> (56 - 8*i));
    sha256_update(c, lb, 8);
    for (i = 0; i < 8; i++) { out[4*i]=(u8)(c->h[i]>>24); out[4*i+1]=(u8)(c->h[i]>>16); out[4*i+2]=(u8)(c->h[i]>>8); out[4*i+3]=(u8)c->h[i]; }
}
void sha256(const u8 *p, u64 n, u8 out[32]) { sha256_ctx c; sha256_init(&c); sha256_update(&c, p, n); sha256_final(&c, out); }

/* ================================================================= HMAC / HKDF ============ */
void hmac_sha256(const u8 *key, int klen, const u8 *msg, int mlen, u8 out[32])
{
    u8 k[64], ipad[64], opad[64], ih[32]; int i; sha256_ctx c;
    xmemset(k, 0, 64);
    if (klen > 64) { sha256(key, klen, k); } else xmemcpy(k, key, klen);
    for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha256_init(&c); sha256_update(&c, ipad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, ih);
    sha256_init(&c); sha256_update(&c, opad, 64); sha256_update(&c, ih, 32); sha256_final(&c, out);
}
void hkdf_extract(const u8 *salt, int slen, const u8 *ikm, int ilen, u8 prk[32])
{
    u8 zero[32]; if (!salt || slen == 0) { xmemset(zero, 0, 32); salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}
void hkdf_expand(const u8 *prk, const u8 *info, int ilen, u8 *out, int olen)
{
    u8 t[32], buf[32 + 256 + 1]; int done = 0, tl = 0; u8 ctr = 1;
    while (done < olen) {
        int bl = 0, take;
        if (tl) { xmemcpy(buf, t, 32); bl = 32; }
        xmemcpy(buf + bl, info, ilen); bl += ilen;
        buf[bl++] = ctr++;
        hmac_sha256(prk, 32, buf, bl, t); tl = 32;
        take = olen - done < 32 ? olen - done : 32;
        xmemcpy(out + done, t, take); done += take;
    }
}

/* ================================================================= AES-128 ============ */
static const u8 sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static u32 aes_sw(u32 x) { return (u32)sbox[x & 0xff] | (u32)sbox[(x>>8)&0xff]<<8 | (u32)sbox[(x>>16)&0xff]<<16 | (u32)sbox[(x>>24)&0xff]<<24; }
void aes128_init(aes128 *a, const u8 key[16])
{
    int i; u32 *rk = a->rk;
    for (i = 0; i < 4; i++) rk[i] = (u32)key[4*i]<<24 | key[4*i+1]<<16 | key[4*i+2]<<8 | key[4*i+3];
    static const u8 rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    for (i = 4; i < 44; i++) {
        u32 t = rk[i-1];
        if (i % 4 == 0) t = aes_sw((t << 8) | (t >> 24)) ^ ((u32)rcon[i/4 - 1] << 24);
        rk[i] = rk[i-4] ^ t;
    }
}
static u8 xtime(u8 x) { return (u8)((x << 1) ^ ((x >> 7) * 0x1b)); }
static u8 gmul(u8 a, u8 b) { u8 p = 0; while (b) { if (b & 1) p ^= a; a = xtime(a); b >>= 1; } return p; }
void aes128_encrypt(const aes128 *a, const u8 in[16], u8 out[16])
{
    u8 s[16]; int r, i, c;
    xmemcpy(s, in, 16);
    for (i = 0; i < 16; i++) s[i] ^= (u8)(a->rk[i/4] >> (24 - 8*(i%4)));
    for (r = 1; r <= 10; r++) {
        u8 t[16];
        for (i = 0; i < 16; i++) t[i] = sbox[s[i]];
        /* ShiftRows */
        u8 sr[16] = { t[0],t[5],t[10],t[15], t[4],t[9],t[14],t[3], t[8],t[13],t[2],t[7], t[12],t[1],t[6],t[11] };
        if (r < 10) {
            for (c = 0; c < 4; c++) {
                u8 *col = sr + 4*c, a0=col[0],a1=col[1],a2=col[2],a3=col[3];
                s[4*c+0] = (u8)(gmul(a0,2)^gmul(a1,3)^a2^a3);
                s[4*c+1] = (u8)(a0^gmul(a1,2)^gmul(a2,3)^a3);
                s[4*c+2] = (u8)(a0^a1^gmul(a2,2)^gmul(a3,3));
                s[4*c+3] = (u8)(gmul(a0,3)^a1^a2^gmul(a3,2));
            }
        } else xmemcpy(s, sr, 16);
        for (i = 0; i < 16; i++) s[i] ^= (u8)(a->rk[4*r + i/4] >> (24 - 8*(i%4)));
    }
    xmemcpy(out, s, 16);
}

/* ================================================================= GHASH / GCM ============ */
static void gf_mul(u8 *x, const u8 *y)      /* x = x*y en GF(2^128), bit a bit */
{
    u8 z[16], v[16]; int i, b;
    xmemset(z, 0, 16); xmemcpy(v, y, 16);
    for (i = 0; i < 128; i++) {
        if (x[i/8] & (0x80 >> (i%8))) for (b = 0; b < 16; b++) z[b] ^= v[b];
        u8 lsb = v[15] & 1;
        for (b = 15; b > 0; b--) v[b] = (u8)((v[b] >> 1) | (v[b-1] << 7));
        v[0] >>= 1;
        if (lsb) v[0] ^= 0xe1;
    }
    xmemcpy(x, z, 16);
}
static void ghash(const u8 H[16], const u8 *aad, int al, const u8 *c, int cl, u8 out[16])
{
    u8 y[16]; int i;
    xmemset(y, 0, 16);
    for (i = 0; i < al; i += 16) { int n = al - i < 16 ? al - i : 16, b; for (b = 0; b < n; b++) y[b] ^= aad[i+b]; gf_mul(y, H); }
    for (i = 0; i < cl; i += 16) { int n = cl - i < 16 ? cl - i : 16, b; for (b = 0; b < n; b++) y[b] ^= c[i+b]; gf_mul(y, H); }
    { u64 ab = (u64)al * 8, cb = (u64)cl * 8; u8 lb[16];
      for (i = 0; i < 8; i++) lb[i] = (u8)(ab >> (56 - 8*i));
      for (i = 0; i < 8; i++) lb[8+i] = (u8)(cb >> (56 - 8*i));
      for (i = 0; i < 16; i++) y[i] ^= lb[i];
      gf_mul(y, H); }
    xmemcpy(out, y, 16);
}
static void gctr(const aes128 *a, const u8 icb[16], const u8 *in, int n, u8 *out)
{
    u8 ctr[16], ks[16]; int i, b;
    xmemcpy(ctr, icb, 16);
    for (i = 0; i < n; i += 16) {
        int m = n - i < 16 ? n - i : 16;
        aes128_encrypt(a, ctr, ks);
        for (b = 0; b < m; b++) out[i+b] = in[i+b] ^ ks[b];
        for (b = 15; b >= 12; b--) { if (++ctr[b]) break; }      /* incrementa los 32 bits bajos */
    }
}
static void gcm_core(const u8 key[16], const u8 *iv, int ivl, const u8 *aad, int al,
                     const u8 *in, int n, u8 *out, u8 tag[16], int enc, const u8 *cin)
{
    aes128 a; u8 H[16], J0[16], S[16], T[16]; int i;
    aes128_init(&a, key);
    xmemset(H, 0, 16); aes128_encrypt(&a, H, H);
    if (ivl == 12) { xmemcpy(J0, iv, 12); J0[12]=0; J0[13]=0; J0[14]=0; J0[15]=1; }
    else { u8 z[16]; xmemset(z,0,16); ghash(H, 0, 0, iv, ivl, J0); (void)z; }
    /* CTR empieza en J0+1. GHASH va siempre sobre el texto CIFRADO: al cifrar es la salida, al descifrar la entrada
     * (lo calculamos antes de descifrar, por si in y out son el mismo buffer). */
    { u8 c0[16]; xmemcpy(c0, J0, 16); for (i=15;i>=12;i--){ if(++c0[i]) break; }
      if (enc) { gctr(&a, c0, in, n, out); ghash(H, aad, al, out, n, S); }
      else     { ghash(H, aad, al, in, n, S); gctr(&a, c0, in, n, out); } }
    (void)cin;
    gctr(&a, J0, S, 16, T);
    xmemcpy(tag, T, 16);
}
void aes128_gcm_encrypt(const u8 key[16], const u8 *iv, int ivl, const u8 *aad, int al,
                        const u8 *in, int n, u8 *out, u8 tag[16])
{
    gcm_core(key, iv, ivl, aad, al, in, n, out, tag, 1, 0);
}
int aes128_gcm_decrypt(const u8 key[16], const u8 *iv, int ivl, const u8 *aad, int al,
                       const u8 *in, int n, const u8 tag[16], u8 *out)
{
    u8 T[16]; int i, diff = 0;
    gcm_core(key, iv, ivl, aad, al, in, n, out, T, 0, in);
    for (i = 0; i < 16; i++) diff |= T[i] ^ tag[i];
    return diff ? 1 : 0;
}

/* ================================================================= X25519 ============ */
/* representacion de 16 limbs de 16 bits (estilo TweetNaCl, dominio publico) */
typedef long long i64;
typedef i64 gf[16];
static const gf _121665 = { 0xDB41, 1 };
static void car25519(gf o) { int i; i64 c; for (i = 0; i < 16; i++) { o[i] += (1LL<<16); c = o[i] >> 16; o[(i+1)*(i<15)] += c - 1 + 37*(c-1)*(i==15); o[i] -= c << 16; } }
static void sel25519(gf p, gf q, int b) { i64 t, c = ~(b - 1); int i; for (i = 0; i < 16; i++) { t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; } }
static void pack25519(u8 *o, const gf n)
{
    int i, j, b; gf m, t; for (i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1); m[i-1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1); b = (int)((m[15] >> 16) & 1); m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (i = 0; i < 16; i++) { o[2*i] = (u8)(t[i] & 0xff); o[2*i+1] = (u8)(t[i] >> 8); }
}
static void unpack25519(gf o, const u8 *n) { int i; for (i = 0; i < 16; i++) o[i] = n[2*i] + ((i64)n[2*i+1] << 8); o[15] &= 0x7fff; }
static void A(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { int i; for (i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void M(gf o, const gf a, const gf b)
{
    i64 t[31]; int i, j; for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++) for (j = 0; j < 16; j++) t[i+j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}
static void S(gf o, const gf a) { M(o, a, a); }
static void inv25519(gf o, const gf i) { gf c; int a; for (a = 0; a < 16; a++) c[a] = i[a]; for (a = 253; a >= 0; a--) { S(c, c); if (a != 2 && a != 4) M(c, c, i); } for (a = 0; a < 16; a++) o[a] = c[a]; }
void x25519(u8 out[32], const u8 scalar[32], const u8 point[32])
{
    u8 z[32]; gf x, a, b, c, d, e, f; i64 r; int i;
    for (i = 0; i < 32; i++) z[i] = scalar[i];
    z[0] &= 248; z[31] &= 127; z[31] |= 64;
    unpack25519(x, point);
    for (i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (i = 254; i >= 0; i--) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, (int)r); sel25519(c, d, (int)r);
        A(e, a, c); Z(a, a, c); A(c, b, d); Z(b, b, d);
        S(d, e); S(f, a); M(a, c, a); M(c, b, e);
        A(e, a, c); Z(a, a, c); S(b, a); Z(c, d, f);
        M(a, c, _121665); A(a, a, d); M(c, c, a); M(a, d, f);
        M(d, b, x); S(b, e);
        sel25519(a, b, (int)r); sel25519(c, d, (int)r);
    }
    inv25519(c, c); M(a, a, c);
    pack25519(out, a);
}
void x25519_base(u8 out[32], const u8 scalar[32]) { u8 nine[32] = {9}; x25519(out, scalar, nine); }
