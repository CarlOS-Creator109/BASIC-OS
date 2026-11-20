/* x509.c - parser DER de certificados X.509 + verificación de firma y nombre, para el TLS de BN. */
#include "x509.h"

/* ---- lector TLV de ASN.1/DER ---- */
typedef struct { const u8 *p; int len; } der;
static int d_tag(der *in, int *tag, der *out)       /* lee un TLV; out = su contenido; avanza in */
{
    const u8 *p = in->p; int n = in->len, l, i;
    if (n < 2) return -1;
    *tag = p[0];
    if ((p[1] & 0x80) == 0) { l = p[1]; p += 2; n -= 2; }
    else {
        int nb = p[1] & 0x7F; if (nb == 0 || nb > 3 || n < 2 + nb) return -1;
        l = 0; for (i = 0; i < nb; i++) l = (l << 8) | p[2 + i];
        p += 2 + nb; n -= 2 + nb;
    }
    if (l < 0 || l > n) return -1;
    out->p = p; out->len = l;
    in->p = p + l; in->len = n - l;
    return 0;
}
/* entra en un SEQUENCE/SET/contexto y deja su contenido en out */
static int d_into(der *in, int want, der *out) { int t; if (d_tag(in, &t, out)) return -1; return t == want ? 0 : -1; }

static int oid_eq(const der *o, const u8 *b, int bl) { int i; if (o->len != bl) return 0; for (i = 0; i < bl; i++) if (o->p[i] != b[i]) return 0; return 1; }

/* algoritmo de firma por su OID (en un AlgorithmIdentifier SEQ) */
static void sig_from_oid(const der *oid, int *alg, int *hash)
{
    static const u8 rsa256[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B};
    static const u8 rsa384[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C};
    static const u8 rsa512[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D};
    static const u8 rsapss[] = {0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0A};
    static const u8 ec256[]  = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02};
    static const u8 ec384[]  = {0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03};
    *alg = 0; *hash = 32;
    if (oid_eq(oid, rsa256, 9)) { *alg = SIG_RSA_PKCS1; *hash = 32; }
    else if (oid_eq(oid, rsa384, 9)) { *alg = SIG_RSA_PKCS1; *hash = 48; }
    else if (oid_eq(oid, rsa512, 9)) { *alg = SIG_RSA_PKCS1; *hash = 64; }
    else if (oid_eq(oid, rsapss, 9)) { *alg = SIG_RSA_PSS; *hash = 32; }
    else if (oid_eq(oid, ec256, 8)) { *alg = SIG_ECDSA; *hash = 32; }
    else if (oid_eq(oid, ec384, 8)) { *alg = SIG_ECDSA; *hash = 48; }
}

static void parse_spki(const der *spki, cert *c)
{
    static const u8 ecpk[] = {0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01};   /* id-ecPublicKey */
    der s = *spki, algseq, oid, bits; int t;
    c->key_rsa = 1;
    if (d_into(&s, 0x30, &algseq)) return;                            /* AlgorithmIdentifier */
    { der a = algseq; if (d_into(&a, 0x06, &oid) == 0 && oid_eq(&oid, ecpk, 7)) c->key_rsa = 0; }
    if (d_tag(&s, &t, &bits) || t != 0x03) return;                    /* subjectPublicKey BIT STRING */
    { const u8 *kp = bits.p + 1; int kl = bits.len - 1;               /* salta el byte de bits sin usar */
      if (c->key_rsa) {
          der k = { kp, kl }, seq, in, n, e;
          if (d_into(&k, 0x30, &seq)) return;
          in = seq;
          if (d_into(&in, 0x02, &n)) return;                          /* modulo */
          if (d_into(&in, 0x02, &e)) return;                          /* exponente */
          while (n.len > 0 && n.p[0] == 0) { n.p++; n.len--; }        /* quita el 00 de signo */
          c->n = n.p; c->nlen = n.len; c->e = e.p; c->elen = e.len;
      } else {
          if (kl == 65 && kp[0] == 0x04) { int i; for (i = 0; i < 64; i++) c->ec[i] = kp[1 + i]; }
      }
    }
}

/* recorre las extensiones buscando SAN (2.5.29.17) y basicConstraints (2.5.29.19) */
static void parse_exts(der exts, cert *c)
{
    static const u8 san[] = {0x55,0x1D,0x11}, bc[] = {0x55,0x1D,0x13};
    der list; int t;
    if (d_into(&exts, 0x30, &list)) return;
    while (list.len > 0) {
        der ext, oid, val; int tt;
        if (d_into(&list, 0x30, &ext)) return;
        if (d_into(&ext, 0x06, &oid)) continue;
        if (d_tag(&ext, &tt, &val)) continue;
        if (tt == 0x01) { if (d_tag(&ext, &tt, &val)) continue; }     /* saltar critical BOOLEAN */
        if (oid_eq(&oid, san, 3)) { c->san = val.p; c->san_len = val.len; }
        else if (oid_eq(&oid, bc, 3)) { der b = val, seq; if (d_into(&b, 0x30, &seq) == 0 && seq.len >= 3 && seq.p[0] == 0x01 && seq.p[2]) c->is_ca = 1; }
        (void)t;
    }
}

int x509_parse(const u8 *der_, int len, cert *c)
{
    der top = { der_, len }, cc, inner, x, alg, oid, sv; int t, tt, i;
    for (i = 0; i < (int)sizeof *c; i++) ((u8 *)c)[i] = 0;
    if (d_into(&top, 0x30, &cc)) return -1;                           /* Certificate { tbs, sigAlg, sigValue } */
    /* tbsCertificate: guardamos su TLV completo (es lo que se firma) */
    { der w = cc; if (d_tag(&w, &tt, &inner) || tt != 0x30) return -1; c->tbs = cc.p; c->tbs_len = (int)(w.p - cc.p); cc = w; }
    /* dentro del tbs */
    if (inner.len && inner.p[0] == 0xA0) d_tag(&inner, &t, &x);       /* version [0] */
    if (d_into(&inner, 0x02, &x)) return -1;                          /* serial */
    if (d_into(&inner, 0x30, &x)) return -1;                          /* signature algid */
    { der w = inner; if (d_tag(&w, &t, &x) || t != 0x30) return -1; c->issuer = inner.p; c->issuer_len = (int)(w.p - inner.p); inner = w; }
    if (d_into(&inner, 0x30, &x)) return -1;                          /* validity */
    { der w = inner; if (d_tag(&w, &t, &x) || t != 0x30) return -1; c->subject = inner.p; c->subject_len = (int)(w.p - inner.p); inner = w; }
    { der w = inner; if (d_tag(&w, &t, &x) || t != 0x30) return -1; parse_spki(&x, c); inner = w; }   /* SPKI */
    while (inner.len > 0) { der w = inner; if (d_tag(&w, &t, &x)) break; if (t == 0xA3) { parse_exts(x, c); break; } inner = w; }
    /* signatureAlgorithm + signatureValue */
    if (d_into(&cc, 0x30, &alg)) return -1;
    { der a = alg; if (d_into(&a, 0x06, &oid) == 0) sig_from_oid(&oid, &c->sig_alg, &c->sig_hash); }
    if (d_tag(&cc, &tt, &sv) || tt != 0x03) return -1;
    c->sig = sv.p + 1; c->sig_len = sv.len - 1;
    return 0;
}

/* r,s de una firma ECDSA DER (SEQ{INTEGER r, INTEGER s}) -> 32 bytes cada uno */
static int ecdsa_rs(const u8 *sig, int slen, u8 r[32], u8 s[32])
{
    der in = { sig, slen }, seq, ri, si; int i;
    if (d_into(&in, 0x30, &seq)) return -1;
    if (d_into(&seq, 0x02, &ri)) return -1;
    if (d_into(&seq, 0x02, &si)) return -1;
    while (ri.len > 32 && ri.p[0] == 0) { ri.p++; ri.len--; }
    while (si.len > 32 && si.p[0] == 0) { si.p++; si.len--; }
    if (ri.len > 32 || si.len > 32) return -1;
    for (i = 0; i < 32; i++) r[i] = s[i] = 0;
    for (i = 0; i < ri.len; i++) r[32 - ri.len + i] = ri.p[i];
    for (i = 0; i < si.len; i++) s[32 - si.len + i] = si.p[i];
    return 0;
}

int x509_check_sig(const cert *c, const cert *ca)
{
    u8 h[64];
    hash_by_len(c->sig_hash, c->tbs, c->tbs_len, h);
    if (c->sig_alg == SIG_ECDSA) {
        u8 r[32], s[32];
        if (ca->key_rsa) return 0;                                   /* clave de la CA no es EC */
        if (ecdsa_rs(c->sig, c->sig_len, r, s)) return 0;
        return ecdsa_p256_verify(ca->ec, h, c->sig_hash, r, s);
    }
    if (!ca->key_rsa) return 0;
    if (c->sig_alg == SIG_RSA_PKCS1)
        return rsa_pkcs1_verify(ca->n, ca->nlen, ca->e, ca->elen, c->sig, c->sig_len, h, c->sig_hash);
    if (c->sig_alg == SIG_RSA_PSS)
        return rsa_pss_verify(ca->n, ca->nlen, ca->e, ca->elen, c->sig, c->sig_len, h, c->sig_hash);
    return 0;
}

/* compara ignorando mayúsculas; pat puede empezar por "*." */
static int host_eq(const char *pat, int pl, const char *host)
{
    int i;
    if (pl > 2 && pat[0] == '*' && pat[1] == '.') {                   /* comodín: *.dominio */
        const char *dot = host; while (*dot && *dot != '.') dot++;
        if (!*dot) return 0;
        host = dot + 1; pat += 2; pl -= 2;
    }
    for (i = 0; i < pl; i++) {
        char a = pat[i], b = host[i];
        if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32;
        if (!b || a != b) return 0;
    }
    return host[pl] == 0;
}
int x509_match_host(const cert *c, const char *host)
{
    der outer = { c->san, c->san_len }, list; int t;
    if (!c->san) return 0;
    if (d_into(&outer, 0x30, &list)) { list.p = c->san; list.len = c->san_len; }   /* entra al SEQUENCE OF GeneralName */
    while (list.len > 0) {
        der name;
        if (d_tag(&list, &t, &name)) break;
        if (t == 0x82)                                               /* dNSName [2] */
            if (host_eq((const char *)name.p, name.len, host)) return 1;
    }
    return 0;
}
