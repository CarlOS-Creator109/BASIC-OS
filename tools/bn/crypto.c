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

/* ================================================================= SHA-384/512 ============ */
static const u64 K512[80] = {
0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };
#define RR64(x,n) (((x) >> (n)) | ((x) << (64 - (n))))
static void sha512_block(sha512_ctx *c, const u8 *p)
{
    u64 w[80], A,B,C,D,E,F,G,H,s0,s1,t1,t2,ch,maj; int i;
    for (i = 0; i < 16; i++) { w[i] = 0; int k; for (k = 0; k < 8; k++) w[i] = w[i] << 8 | p[8*i+k]; }
    for (i = 16; i < 80; i++) {
        s0 = RR64(w[i-15],1) ^ RR64(w[i-15],8) ^ (w[i-15] >> 7);
        s1 = RR64(w[i-2],19) ^ RR64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    A=c->h[0];B=c->h[1];C=c->h[2];D=c->h[3];E=c->h[4];F=c->h[5];G=c->h[6];H=c->h[7];
    for (i = 0; i < 80; i++) {
        s1 = RR64(E,14)^RR64(E,18)^RR64(E,41); ch=(E&F)^(~E&G);
        t1 = H+s1+ch+K512[i]+w[i];
        s0 = RR64(A,28)^RR64(A,34)^RR64(A,39); maj=(A&B)^(A&C)^(B&C);
        t2 = s0+maj;
        H=G;G=F;F=E;E=D+t1;D=C;C=B;B=A;A=t1+t2;
    }
    c->h[0]+=A;c->h[1]+=B;c->h[2]+=C;c->h[3]+=D;c->h[4]+=E;c->h[5]+=F;c->h[6]+=G;c->h[7]+=H;
}
static void sha512_run(sha512_ctx *c, const u8 *p, u64 n, u8 *out, int outlen)
{
    u64 bits; int i; u8 pad = 0x80;
    c->len = 0; c->n = 0;
    while (n) { int take = 128 - c->n; if ((u64)take > n) take = (int)n; { int k; for (k=0;k<take;k++) c->buf[c->n+k]=p[k]; } c->n+=take; c->len+=take; p+=take; n-=take; if (c->n==128){ sha512_block(c,c->buf); c->n=0; } }
    bits = c->len * 8;
    c->buf[c->n++] = pad;
    if (c->n > 112) { while (c->n < 128) c->buf[c->n++]=0; sha512_block(c,c->buf); c->n=0; }
    while (c->n < 112) c->buf[c->n++]=0;
    for (i = 0; i < 8; i++) c->buf[c->n++]=0;              /* parte alta de la longitud (0) */
    for (i = 0; i < 8; i++) c->buf[c->n++]=(u8)(bits >> (56 - 8*i));
    sha512_block(c, c->buf);
    for (i = 0; i < outlen; i++) out[i] = (u8)(c->h[i/8] >> (56 - 8*(i%8)));
}
void sha512(const u8 *p, u64 n, u8 out[64])
{
    sha512_ctx c;
    c.h[0]=0x6a09e667f3bcc908ULL;c.h[1]=0xbb67ae8584caa73bULL;c.h[2]=0x3c6ef372fe94f82bULL;c.h[3]=0xa54ff53a5f1d36f1ULL;
    c.h[4]=0x510e527fade682d1ULL;c.h[5]=0x9b05688c2b3e6c1fULL;c.h[6]=0x1f83d9abfb41bd6bULL;c.h[7]=0x5be0cd19137e2179ULL;
    sha512_run(&c, p, n, out, 64);
}
void sha384(const u8 *p, u64 n, u8 out[48])
{
    sha512_ctx c;
    c.h[0]=0xcbbb9d5dc1059ed8ULL;c.h[1]=0x629a292a367cd507ULL;c.h[2]=0x9159015a3070dd17ULL;c.h[3]=0x152fecd8f70e5939ULL;
    c.h[4]=0x67332667ffc00b31ULL;c.h[5]=0x8eb44a8768581511ULL;c.h[6]=0xdb0c2e0d64f98fa7ULL;c.h[7]=0x47b5481dbefa4fa4ULL;
    sha512_run(&c, p, n, out, 48);
}

/* ================================================================= bignum 256 + P-256 ============ */
/* enteros de 256 bits en 4 limbs de 64 (poco significativo primero) */
typedef struct { u64 v[4]; } bn;
static const bn P256P = { { 0xFFFFFFFFFFFFFFFFULL, 0x00000000FFFFFFFFULL, 0x0000000000000000ULL, 0xFFFFFFFF00000001ULL } };
static const bn P256N = { { 0xF3B9CAC2FC632551ULL, 0xBCE6FAADA7179E84ULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFF00000000ULL } };
static const bn P256GX = { { 0xF4A13945D898C296ULL, 0x77037D812DEB33A0ULL, 0xF8BCE6E563A440F2ULL, 0x6B17D1F2E12C4247ULL } };
static const bn P256GY = { { 0xCBB6406837BF51F5ULL, 0x2BCE33576B315eceULL, 0x8EE7EB4A7C0F9E16ULL, 0x4FE342E2FE1A7F9BULL } };

static int bn_iszero(const bn *a) { return !(a->v[0]|a->v[1]|a->v[2]|a->v[3]); }
static int bn_cmp(const bn *a, const bn *b) { int i; for (i=3;i>=0;i--){ if(a->v[i]<b->v[i])return -1; if(a->v[i]>b->v[i])return 1; } return 0; }
static int bn_add(bn *r, const bn *a, const bn *b) { unsigned __int128 c=0; int i; for(i=0;i<4;i++){ c += (unsigned __int128)a->v[i]+b->v[i]; r->v[i]=(u64)c; c>>=64; } return (int)c; }
static int bn_sub(bn *r, const bn *a, const bn *b) { unsigned __int128 br=0; int i; for(i=0;i<4;i++){ unsigned __int128 t=(unsigned __int128)a->v[i]-b->v[i]-br; r->v[i]=(u64)t; br=(t>>64)&1; } return (int)br; }
static void bn_set(bn *r, const bn *a) { r->v[0]=a->v[0];r->v[1]=a->v[1];r->v[2]=a->v[2];r->v[3]=a->v[3]; }
static void bn_zero(bn *r) { r->v[0]=r->v[1]=r->v[2]=r->v[3]=0; }
static int bn_bit(const bn *a, int i) { return (int)((a->v[i>>6] >> (i&63)) & 1); }
/* r = a mod m  (a tiene 512 bits en lo[4]+hi[4]); division larga bit a bit */
static void bn_addmod(bn *r, const bn *a, const bn *b, const bn *m) { bn t; int c = bn_add(&t,a,b); if (c || bn_cmp(&t,m)>=0) bn_sub(&t,&t,m); bn_set(r,&t); }
static void bn_submod(bn *r, const bn *a, const bn *b, const bn *m) { bn t; int br = bn_sub(&t,a,b); if (br) bn_add(&t,&t,m); bn_set(r,&t); }
/* multiplicacion 256x256 -> 512, luego reduccion mod m por resta desplazada */
static void bn_mulmod(bn *r, const bn *a, const bn *b, const bn *m)
{
    u64 prod[8]; int i,j; for(i=0;i<8;i++) prod[i]=0;
    for (i=0;i<4;i++){ unsigned __int128 c=0; for(j=0;j<4;j++){ unsigned __int128 t=(unsigned __int128)a->v[i]*b->v[j]+prod[i+j]+c; prod[i+j]=(u64)t; c=t>>64; } prod[i+4]+=(u64)c; }
    /* reducir: res de 512 bits mod m (256), bit a bit desde el mas alto */
    { bn res; bn_zero(&res); int k;
      for (k=511;k>=0;k--){
          /* res = res*2 + bit k de prod */
          u64 carry = (res.v[3]>>63)&1;
          res.v[3]=(res.v[3]<<1)|(res.v[2]>>63); res.v[2]=(res.v[2]<<1)|(res.v[1]>>63); res.v[1]=(res.v[1]<<1)|(res.v[0]>>63); res.v[0]=(res.v[0]<<1)|((prod[k>>6]>>(k&63))&1);
          if (carry || bn_cmp(&res,m)>=0) bn_sub(&res,&res,m);
      }
      bn_set(r,&res);
    }
}
static void bn_modexp(bn *r, const bn *base, const bn *exp, const bn *m) { bn res, b; bn_zero(&res); res.v[0]=1; bn_set(&b,base); int i; for(i=0;i<256;i++){ if(bn_bit(exp,i)) bn_mulmod(&res,&res,&b,m); bn_mulmod(&b,&b,&b,m); } bn_set(r,&res); }
static void bn_inv(bn *r, const bn *a, const bn *m) { bn e, two; bn_zero(&two); two.v[0]=2; bn_sub(&e,m,&two); bn_modexp(r,a,&e,m); }   /* a^(m-2) */

/* punto en afin; inf=1 => infinito */
/* punto en coordenadas Jacobianas: (X,Y,Z) = afin (X/Z^2, Y/Z^3). Z=0 => infinito.
 * Hace suma y doblado SIN inversiones (solo una inversion al final), ~100x mas rapido que afin. */
typedef struct { bn X, Y, Z; } jpt;
static void fsq(bn *r, const bn *a) { bn_mulmod(r, a, a, &P256P); }
static void fmul(bn *r, const bn *a, const bn *b) { bn_mulmod(r, a, b, &P256P); }
static void fadd(bn *r, const bn *a, const bn *b) { bn_addmod(r, a, b, &P256P); }
static void fsub(bn *r, const bn *a, const bn *b) { bn_submod(r, a, b, &P256P); }
static void j_dbl(jpt *r, const jpt *p)
{
    bn delta, gamma, beta, alpha, t, u, X3, Y3, Z3;
    if (bn_iszero(&p->Z)) { *r = *p; return; }
    fsq(&delta, &p->Z);
    fsq(&gamma, &p->Y);
    fmul(&beta, &p->X, &gamma);
    fsub(&t, &p->X, &delta); fadd(&u, &p->X, &delta); fmul(&t, &t, &u);   /* (X-d)(X+d), con a=-3 */
    fadd(&alpha, &t, &t); fadd(&alpha, &alpha, &t);                        /* 3t */
    fsq(&X3, &alpha);
    fadd(&t, &beta, &beta); fadd(&t, &t, &t); fadd(&t, &t, &t);            /* 8beta */
    fsub(&X3, &X3, &t);
    fadd(&Z3, &p->Y, &p->Z); fsq(&Z3, &Z3); fsub(&Z3, &Z3, &gamma); fsub(&Z3, &Z3, &delta);
    fadd(&t, &beta, &beta); fadd(&t, &t, &t); fsub(&t, &t, &X3); fmul(&Y3, &alpha, &t);
    fsq(&u, &gamma); fadd(&u, &u, &u); fadd(&u, &u, &u); fadd(&u, &u, &u); /* 8 gamma^2 */
    fsub(&Y3, &Y3, &u);
    r->X = X3; r->Y = Y3; r->Z = Z3;
}
static void j_add(jpt *r, const jpt *a, const jpt *b)
{
    bn z1z1, z2z2, u1, u2, s1, s2, h, rr, i2, j, v, t, X3, Y3, Z3;
    if (bn_iszero(&a->Z)) { *r = *b; return; }
    if (bn_iszero(&b->Z)) { *r = *a; return; }
    fsq(&z1z1, &a->Z); fsq(&z2z2, &b->Z);
    fmul(&u1, &a->X, &z2z2); fmul(&u2, &b->X, &z1z1);
    fmul(&s1, &a->Y, &b->Z); fmul(&s1, &s1, &z2z2);
    fmul(&s2, &b->Y, &a->Z); fmul(&s2, &s2, &z1z1);
    fsub(&h, &u2, &u1);
    fsub(&rr, &s2, &s1); fadd(&rr, &rr, &rr);
    if (bn_iszero(&h)) { if (bn_iszero(&rr)) { j_dbl(r, a); return; } bn_zero(&r->X); bn_zero(&r->Y); bn_zero(&r->Z); return; }
    fadd(&t, &h, &h); fsq(&i2, &t);
    fmul(&j, &h, &i2);
    fmul(&v, &u1, &i2);
    fsq(&X3, &rr); fsub(&X3, &X3, &j); fsub(&X3, &X3, &v); fsub(&X3, &X3, &v);
    fsub(&t, &v, &X3); fmul(&Y3, &rr, &t); fmul(&t, &s1, &j); fadd(&t, &t, &t); fsub(&Y3, &Y3, &t);
    fadd(&Z3, &a->Z, &b->Z); fsq(&Z3, &Z3); fsub(&Z3, &Z3, &z1z1); fsub(&Z3, &Z3, &z2z2); fmul(&Z3, &Z3, &h);
    r->X = X3; r->Y = Y3; r->Z = Z3;
}
static void j_mul(jpt *r, const bn *k, const bn *px, const bn *py)       /* k * (afin px,py) */
{
    jpt acc, P; int i;
    bn_zero(&acc.X); bn_zero(&acc.Y); bn_zero(&acc.Z);                    /* infinito */
    bn_set(&P.X, px); bn_set(&P.Y, py); bn_zero(&P.Z); P.Z.v[0] = 1;
    for (i = 255; i >= 0; i--) { jpt t; j_dbl(&t, &acc); acc = t; if (bn_bit(k, i)) { j_add(&t, &acc, &P); acc = t; } }
    *r = acc;
}
static void bn_frombe(bn *r, const u8 *p) { int i; for (i=0;i<4;i++){ u64 w=0,k; for(k=0;k<8;k++) w=w<<8|p[i*8+k]; r->v[3-i]=w; } }

int ecdsa_p256_verify(const u8 pub[64], const u8 *hash, int hlen, const u8 r[32], const u8 s[32])
{
    bn rr, ss, z, w, u1, u2, zinv, z2, x; jpt A, B, R;
    bn_frombe(&rr, r); bn_frombe(&ss, s);
    if (bn_iszero(&rr) || bn_iszero(&ss) || bn_cmp(&rr,&P256N)>=0 || bn_cmp(&ss,&P256N)>=0) return 0;
    { u8 zb[32]; int i, n = hlen < 32 ? hlen : 32; for (i=0;i<32;i++) zb[i]=0; for (i=0;i<n;i++) zb[i]=hash[i]; bn_frombe(&z, zb); }
    bn_inv(&w, &ss, &P256N);                        /* w = s^-1 mod n */
    bn_mulmod(&u1, &z, &w, &P256N);
    bn_mulmod(&u2, &rr, &w, &P256N);
    { bn qx, qy; bn_frombe(&qx, pub); bn_frombe(&qy, pub + 32);
      j_mul(&A, &u1, &P256GX, &P256GY);             /* u1*G */
      j_mul(&B, &u2, &qx, &qy); }                   /* u2*Q */
    j_add(&R, &A, &B);
    if (bn_iszero(&R.Z)) return 0;
    bn_inv(&zinv, &R.Z, &P256P);                    /* la unica inversion de todo el verify */
    fsq(&z2, &zinv);
    fmul(&x, &R.X, &z2);                            /* x afin = X / Z^2 */
    if (bn_cmp(&x,&P256N) >= 0) bn_sub(&x,&x,&P256N);
    return bn_cmp(&x,&rr) == 0 ? 1 : 0;
}

/* ================================================================= RSA (verificar firma) ============ */
/* enteros grandes para RSA: limbs de 32 bits, poco significativo primero. Hasta 4096 bits. */
#define RL 132
static void big_frombe(u32 *r, int L, const u8 *b, int blen)
{
    int i; for (i = 0; i < L; i++) r[i] = 0;
    for (i = 0; i < blen; i++) { int bit = (blen - 1 - i) * 8; r[bit >> 5] |= (u32)b[i] << (bit & 31); }
}
static void big_tobe(const u32 *a, u8 *b, int blen)
{
    int i; for (i = 0; i < blen; i++) { int bit = (blen - 1 - i) * 8; b[i] = (u8)(a[bit >> 5] >> (bit & 31)); }
}
static int big_cmp(const u32 *a, const u32 *b, int L) { int i; for (i = L - 1; i >= 0; i--) { if (a[i] < b[i]) return -1; if (a[i] > b[i]) return 1; } return 0; }
static void big_sub(u32 *a, const u32 *b, int L) { u64 br = 0; int i; for (i = 0; i < L; i++) { u64 t = (u64)a[i] - b[i] - br; a[i] = (u32)t; br = (t >> 32) & 1; } }
/* r = a*b mod m, todo de L limbs. Reduccion bit a bit. */
static void big_mulmod(u32 *r, const u32 *a, const u32 *b, const u32 *m, int L)
{
    static u32 prod[2 * RL]; u32 rem[RL + 1]; int i, k;
    for (i = 0; i < 2 * L; i++) prod[i] = 0;
    for (i = 0; i < L; i++) { u64 c = 0; for (k = 0; k < L; k++) { u64 t = (u64)a[i] * b[k] + prod[i + k] + c; prod[i + k] = (u32)t; c = t >> 32; } prod[i + L] += (u32)c; }
    for (i = 0; i <= L; i++) rem[i] = 0;
    for (k = 2 * L * 32 - 1; k >= 0; k--) {
        int carry = (rem[L - 1] >> 31) & 1;                 /* el bit que se sale de L limbs */
        for (i = L; i > 0; i--) rem[i] = (rem[i] << 1) | (rem[i - 1] >> 31);
        rem[0] = (rem[0] << 1) | ((prod[k >> 5] >> (k & 31)) & 1);
        (void)carry;
        if (rem[L] || big_cmp(rem, m, L) >= 0) { big_sub(rem, m, L); rem[L] = 0; }
    }
    for (i = 0; i < L; i++) r[i] = rem[i];
}
/* out = in^e mod n (todo big-endian). k = tamano del modulo en bytes. Devuelve k o -1. */
static int rsa_pub(u8 *out, const u8 *n, int nlen, const u8 *e, int elen, const u8 *in, int inlen)
{
    u32 N[RL], B[RL], R[RL], T[RL]; int L = (nlen + 3) / 4, i, bit, started = 0;
    if (L > RL || inlen > nlen) return -1;
    big_frombe(N, L, n, nlen); big_frombe(B, L, in, inlen);
    if (big_cmp(B, N, L) >= 0) return -1;
    for (i = 0; i < L; i++) R[i] = 0;
    R[0] = 1;
    for (i = elen * 8 - 1; i >= 0; i--) {
        bit = (e[elen - 1 - (i >> 3)] >> (i & 7)) & 1;
        if (!started) { if (!bit) continue; started = 1; }
        big_mulmod(T, R, R, N, L); for (int j = 0; j < L; j++) R[j] = T[j];     /* cuadrado */
        if (bit) { big_mulmod(T, R, B, N, L); for (int j = 0; j < L; j++) R[j] = T[j]; }
    }
    big_tobe(R, out, nlen);
    return nlen;
}

void hash_by_len(int hl, const u8 *d, int dl, u8 *out)
{
    if (hl == 48) sha384(d, dl, out);
    else if (hl == 64) sha512(d, dl, out);
    else sha256(d, dl, out);
}
/* MGF1 con la funcion hash del largo hl */
static void mgf1(const u8 *seed, int sl, u8 *mask, int mlen, int hl)
{
    u8 cnt[4], h[64], buf[256 + 4]; int i, done = 0; u32 c = 0;
    while (done < mlen) {
        cnt[0]=(u8)(c>>24);cnt[1]=(u8)(c>>16);cnt[2]=(u8)(c>>8);cnt[3]=(u8)c;
        for (i = 0; i < sl; i++) buf[i] = seed[i];
        buf[sl]=cnt[0];buf[sl+1]=cnt[1];buf[sl+2]=cnt[2];buf[sl+3]=cnt[3];
        hash_by_len(hl, buf, sl + 4, h);
        for (i = 0; i < hl && done < mlen; i++) mask[done++] ^= h[i];
        c++;
    }
}
/* PKCS#1 v1.5: verifica que sig^e == 00 01 FF..FF 00 DigestInfo||hash */
int rsa_pkcs1_verify(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int siglen, const u8 *hash, int hashlen)
{
    static const u8 oid256[] = {0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20};
    static const u8 oid384[] = {0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30};
    static const u8 oid512[] = {0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40};
    u8 em[512]; const u8 *oid = hashlen==48?oid384:(hashlen==64?oid512:oid256); int ol = 19, i, p;
    if (nlen > 512 || siglen != nlen) return 0;
    if (rsa_pub(em, n, nlen, e, elen, sig, siglen) < 0) return 0;
    if (em[0] != 0x00 || em[1] != 0x01) return 0;
    for (i = 2; i < nlen && em[i] == 0xFF; i++) ;
    if (i < 10 || em[i] != 0x00) return 0;
    p = i + 1;
    if (p + ol + hashlen != nlen) return 0;
    for (i = 0; i < ol; i++) if (em[p + i] != oid[i]) return 0;
    for (i = 0; i < hashlen; i++) if (em[p + ol + i] != hash[i]) return 0;
    return 1;
}
/* RSA-PSS (MGF1 con el mismo hash, salt = hashlen): verifica la firma del mensaje cuyo hash es mhash */
int rsa_pss_verify(const u8 *n, int nlen, const u8 *e, int elen, const u8 *sig, int siglen, const u8 *mhash, int hashlen)
{
    u8 em[512], db[512], hp[64], mp[8 + 64 + 64], hh[64]; int i, emlen = nlen, dblen, slen = hashlen;
    int embits = nlen * 8 - 1;                     /* para RSA el modulo es multiplo de 8, emBits = 8k-1 */
    if (nlen > 512 || siglen != nlen) return 0;
    if (rsa_pub(em, n, nlen, e, elen, sig, siglen) < 0) return 0;
    if (em[emlen - 1] != 0xbc) return 0;
    dblen = emlen - hashlen - 1;
    for (i = 0; i < dblen; i++) db[i] = em[i];
    { const u8 *H = em + dblen; u8 mask[512];
      for (i = 0; i < dblen; i++) mask[i] = 0;
      mgf1(H, hashlen, mask, dblen, hashlen);
      for (i = 0; i < dblen; i++) db[i] ^= mask[i];
      db[0] &= (u8)(0xFF >> (8 * emlen - embits));
      /* db = PS(00..) || 0x01 || salt */
      for (i = 0; i < dblen - slen - 1; i++) if (db[i] != 0) return 0;
      if (db[dblen - slen - 1] != 0x01) return 0;
      /* M' = 00000000 00000000 || mHash || salt ; H' = Hash(M') */
      for (i = 0; i < 8; i++) mp[i] = 0;
      for (i = 0; i < hashlen; i++) mp[8 + i] = mhash[i];
      for (i = 0; i < slen; i++) mp[8 + hashlen + i] = db[dblen - slen + i];
      hash_by_len(hashlen, mp, 8 + hashlen + slen, hh);
      for (i = 0; i < hashlen; i++) if (hh[i] != H[i]) return 0;
    }
    (void)hp;
    return 1;
}
