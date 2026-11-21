/* tls.c - cliente TLS 1.3 (RFC 8446) para BN. Suite TLS_AES_128_GCM_SHA256, grupo X25519.
 * Handshake + datos de aplicacion (3b) + verificación del certificado: cadena, firma y host (3c). */
#include "tls.h"
#include "x509.h"

void tls_trust(tls_conn *c, const u8 *roots_der, int len) { c->roots = roots_der; c->roots_len = len; }

static void xcpy(void *d, const void *s, int n) { u8 *a = d; const u8 *b = s; while (n-- > 0) *a++ = *b++; }
static void log_(tls_conn *c, const char *s) { if (c->log) c->log(s); }
static int diff(const u8 *a, const u8 *b, int n) { int d = 0; while (n-- > 0) d |= *a++ ^ *b++; return d; }

/* ---- aleatorio (lo siembra el que llama con tls_seed antes, si quiere; aqui un contador + mezcla) ---- */
static u32 rng_state;
void tls_seed(u32 s) { rng_state = s ^ 0x9e3779b9u; }
static void rng(u8 *p, int n) { int i; for (i = 0; i < n; i++) { rng_state = rng_state * 1664525u + 1013904223u; p[i] = (u8)(rng_state >> 17); } }

/* ---- HKDF-Expand-Label y Derive-Secret (TLS 1.3) ---- */
static void expand_label(const u8 secret[32], const char *label, const u8 *ctx, int ctxlen, u8 *out, int outlen)
{
    u8 info[2 + 1 + 20 + 1 + 32]; int i = 0, ll = 0;
    const char *pre = "tls13 ";
    char full[20]; int fl = 0;
    while (pre[fl - 0] && fl < 6) { full[fl] = pre[fl]; fl++; }   /* "tls13 " */
    { int k = 0; while (label[k]) { full[fl++] = label[k]; k++; } }
    ll = fl;
    info[i++] = (u8)(outlen >> 8); info[i++] = (u8)outlen;
    info[i++] = (u8)ll;
    { int k; for (k = 0; k < ll; k++) info[i++] = (u8)full[k]; }
    info[i++] = (u8)ctxlen;
    { int k; for (k = 0; k < ctxlen; k++) info[i++] = ctx[k]; }
    hkdf_expand(secret, info, i, out, outlen);
}
/* hash actual del transcript (clona el ctx) */
static void transcript(tls_conn *c, u8 out[32]) { sha256_ctx t = c->th; sha256_final(&t, out); }
static void derive_secret(const u8 secret[32], const char *label, const u8 thash[32], u8 out[32])
{
    expand_label(secret, label, thash, 32, out, 32);
}
static void key_iv(const u8 secret[32], u8 key[16], u8 iv[12])
{
    expand_label(secret, "key", 0, 0, key, 16);
    expand_label(secret, "iv", 0, 0, iv, 12);
}

/* ---- transporte: leer exactamente n bytes del stream TCP, con buffer ---- */
static int fill(tls_conn *c)
{
    int r;
    if (c->rpos < c->rlen) return 1;
    c->rpos = c->rlen = 0;
    r = c->rcv(c->io, c->rbuf, (int)sizeof c->rbuf);
    if (r <= 0) return r;
    c->rlen = r;
    return 1;
}
static int read_n(tls_conn *c, u8 *out, int n)
{
    int got = 0;
    while (got < n) {
        if (c->rpos >= c->rlen) { int r = fill(c); if (r <= 0) return got ? got : r; }
        int take = c->rlen - c->rpos; if (take > n - got) take = n - got;
        xcpy(out + got, c->rbuf + c->rpos, take); c->rpos += take; got += take;
    }
    return got;
}

/* ---- leer un record TLS; descifra si ya hay claves. Devuelve el tipo real (0x16/0x17/0x15) y pone los datos en
 *      out[*len]; 0 si se cierra, <0 error ---- */
static int read_record(tls_conn *c, u8 *out, int *len, int encrypted)
{
    u8 hdr[5]; int n, type;
    for (;;) {
        if (read_n(c, hdr, 5) != 5) return -1;
        type = hdr[0]; n = hdr[3] << 8 | hdr[4];
        if (n < 0 || n > (int)sizeof c->rbuf) return -1;
        if (read_n(c, out, n) != n) return -1;
        if (type == 0x14) continue;                 /* ChangeCipherSpec: ignorar */
        if (!encrypted) { *len = n; return type; }
        /* descifrar AEAD: nonce = iv ^ seq, AAD = cabecera del record */
        {
            u8 nonce[12], aad[5]; int i; u64 seq = c->app ? c->srv_seq : c->srv_seq;
            const u8 *key = c->app ? c->srv_apkey : c->srv_key;
            const u8 *iv  = c->app ? c->srv_apiv : c->srv_iv;
            if (n < 16) return -1;
            xcpy(nonce, iv, 12);
            for (i = 0; i < 8; i++) nonce[11 - i] ^= (u8)(seq >> (8 * i));
            xcpy(aad, hdr, 5);
            if (aes128_gcm_decrypt(key, nonce, 12, aad, 5, out, n - 16, out + n - 16, out)) { log_(c, "TLS: tag invalido\n"); return -1; }
            c->srv_seq++;
            /* TLSInnerPlaintext: quita el relleno de ceros; el ultimo byte != 0 es el tipo real */
            { int m = n - 16; while (m > 0 && out[m - 1] == 0) m--; if (m <= 0) return -1; type = out[m - 1]; *len = m - 1; return type; }
        }
    }
}

/* ---- mandar un record (cifrado si encrypted) ---- */
static int send_record(tls_conn *c, int type, const u8 *data, int n, int encrypted)
{
    static u8 rec[20000]; int i;
    if (!encrypted) {
        rec[0] = (u8)type; rec[1] = 0x03; rec[2] = 0x03; rec[3] = (u8)(n >> 8); rec[4] = (u8)n;
        xcpy(rec + 5, data, n);
        return c->snd(c->io, rec, n + 5) == n + 5 ? 0 : -1;
    }
    {
        u8 nonce[12], aad[5]; int inner = n + 1, ct = inner + 16;
        u64 seq = c->app ? c->cli_seq : c->cli_seq;
        const u8 *key = c->app ? c->cli_apkey : c->cli_key;
        const u8 *iv  = c->app ? c->cli_apiv : c->cli_iv;
        xcpy(rec + 5, data, n); rec[5 + n] = (u8)type;    /* tipo real dentro */
        rec[0] = 0x17; rec[1] = 0x03; rec[2] = 0x03; rec[3] = (u8)(ct >> 8); rec[4] = (u8)ct;
        xcpy(aad, rec, 5);
        xcpy(nonce, iv, 12);
        for (i = 0; i < 8; i++) nonce[11 - i] ^= (u8)(seq >> (8 * i));
        aes128_gcm_encrypt(key, nonce, 12, aad, 5, rec + 5, inner, rec + 5, rec + 5 + inner);
        c->cli_seq++;
        return c->snd(c->io, rec, 5 + ct) == 5 + ct ? 0 : -1;
    }
}

/* ---- construir y mandar el ClientHello ---- */
static int send_client_hello(tls_conn *c)
{
    u8 b[512]; int i = 0, exts, extsp, bodyp, hsp;
    static const u8 h32[32];
    b[i++] = 0x16; b[i++] = 0x03; b[i++] = 0x01;   /* record: handshake, legacy 0x0301 */
    bodyp = i; i += 2;                               /* longitud del record (se rellena) */
    hsp = i;
    b[i++] = 0x01; i += 3;                           /* handshake: ClientHello, long 3 bytes (se rellena) */
    b[i++] = 0x03; b[i++] = 0x03;                    /* client_version 0x0303 */
    rng(b + i, 32); i += 32;                         /* random */
    b[i++] = 32; rng(b + i, 32); i += 32;            /* legacy_session_id (32 al azar) */
    b[i++] = 0x00; b[i++] = 0x02; b[i++] = 0x13; b[i++] = 0x01;   /* cipher_suites: TLS_AES_128_GCM_SHA256 */
    b[i++] = 0x01; b[i++] = 0x00;                    /* compression: null */
    extsp = i; i += 2;                               /* longitud de extensiones */
    exts = i;
    /* supported_versions: TLS 1.3 */
    b[i++]=0x00;b[i++]=0x2b; b[i++]=0x00;b[i++]=0x03; b[i++]=0x02; b[i++]=0x03;b[i++]=0x04;
    /* supported_groups: x25519 */
    b[i++]=0x00;b[i++]=0x0a; b[i++]=0x00;b[i++]=0x04; b[i++]=0x00;b[i++]=0x02; b[i++]=0x00;b[i++]=0x1d;
    /* signature_algorithms (para que el server mande certificado) */
    b[i++]=0x00;b[i++]=0x0d; b[i++]=0x00;b[i++]=0x0a; b[i++]=0x00;b[i++]=0x08;
    b[i++]=0x04;b[i++]=0x03; b[i++]=0x08;b[i++]=0x04; b[i++]=0x08;b[i++]=0x05; b[i++]=0x04;b[i++]=0x01;   /* ecdsa_p256_sha256, rsa_pss_sha256, rsa_pss_sha384, rsa_pkcs1_sha256 */
    /* key_share: x25519 + nuestra clave publica */
    x25519_base(c->pub, c->priv);
    b[i++]=0x00;b[i++]=0x33; b[i++]=0x00;b[i++]=0x26; b[i++]=0x00;b[i++]=0x24;
    b[i++]=0x00;b[i++]=0x1d; b[i++]=0x00;b[i++]=0x20; xcpy(b + i, c->pub, 32); i += 32;
    /* server_name (SNI) */
    if (c->host && c->host[0]) {
        int hl = 0; while (c->host[hl]) hl++;
        b[i++]=0x00;b[i++]=0x00; b[i++]=(u8)((hl+5)>>8);b[i++]=(u8)(hl+5);
        b[i++]=(u8)((hl+3)>>8);b[i++]=(u8)(hl+3); b[i++]=0x00; b[i++]=(u8)(hl>>8);b[i++]=(u8)hl;
        { int k; for (k = 0; k < hl; k++) b[i++] = (u8)c->host[k]; }
    }
    (void)h32;
    /* rellenar longitudes */
    { int el = i - exts; b[extsp] = (u8)(el >> 8); b[extsp + 1] = (u8)el; }
    { int hl = i - hsp - 4; b[hsp + 1] = (u8)(hl >> 16); b[hsp + 2] = (u8)(hl >> 8); b[hsp + 3] = (u8)hl; }
    { int rl = i - bodyp - 2; b[bodyp] = (u8)(rl >> 8); b[bodyp + 1] = (u8)rl; }
    sha256_update(&c->th, b + hsp, i - hsp);         /* transcript: el mensaje de handshake */
    return c->snd(c->io, b, i) == i ? 0 : -1;
}

/* lee el ServerHello (record de handshake en claro) y saca la clave publica del servidor */
static int read_server_hello(tls_conn *c, u8 srv_pub[32])
{
    u8 rec[20000]; int len, type, i, got = 0;
    type = read_record(c, rec, &len, 0);
    if (type != 0x16 || len < 4 || rec[0] != 0x02) {
        if (c->log) { char d[64]; int k; const char *hx="0123456789abcdef";
            for (k=0;k<16 && k<len;k++){ d[k*2]=hx[(rec[k]>>4)&15]; d[k*2+1]=hx[rec[k]&15]; } d[2*(len<16?len:16)]=0;
            log_(c,"TLS: no vino ServerHello; tipo="); { char t[8]; t[0]=hx[(type>>4)&15]; t[1]=hx[type&15]; t[2]=0; log_(c,t);} log_(c," datos="); log_(c,d); log_(c,"\n"); }
        return -1;
    }
    sha256_update(&c->th, rec, len);                 /* transcript */
    /* saltar: version(2)+random(32)+sid_len(1)+sid+cipher(2)+comp(1) */
    i = 4 + 2 + 32;
    { int sl = rec[i]; i += 1 + sl; }
    if (rec[i] != 0x13 || rec[i+1] != 0x01) { log_(c, "TLS: el server no eligio AES_128_GCM_SHA256\n"); return -1; }
    i += 2 + 1;
    { int el = rec[i] << 8 | rec[i+1]; i += 2; int end = i + el;
      while (i + 4 <= end) {
          int et = rec[i] << 8 | rec[i+1], eln = rec[i+2] << 8 | rec[i+3]; i += 4;
          if (et == 0x0033) {                        /* key_share */
              /* group(2) + keylen(2) + key */
              int kl = rec[i+2] << 8 | rec[i+3];
              if (kl == 32) { xcpy(srv_pub, rec + i + 4, 32); got = 1; }
          }
          i += eln;
      }
    }
    if (!got) { log_(c, "TLS: el ServerHello no trae key_share x25519\n"); return -1; }
    return 0;
}

/* deriva los secretos de handshake tras conocer el secreto compartido */
static void derive_handshake(tls_conn *c, const u8 shared[32])
{
    u8 early[32], derived[32], empty_hash[32], th[32];
    static const u8 zero[32];
    hkdf_extract(0, 0, zero, 32, early);                         /* Early Secret (PSK=0) */
    sha256((const u8 *)"", 0, empty_hash);
    derive_secret(early, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, shared, 32, c->hs_secret);         /* Handshake Secret */
    transcript(c, th);                                           /* ClientHello..ServerHello */
    derive_secret(c->hs_secret, "c hs traffic", th, c->cli_hs);
    derive_secret(c->hs_secret, "s hs traffic", th, c->srv_hs);
    key_iv(c->cli_hs, c->cli_key, c->cli_iv);
    key_iv(c->srv_hs, c->srv_key, c->srv_iv);
    c->cli_seq = c->srv_seq = 0;
}
static void derive_app(tls_conn *c)
{
    u8 derived[32], master[32], empty_hash[32], th[32];
    static const u8 zero[32];
    sha256((const u8 *)"", 0, empty_hash);
    derive_secret(c->hs_secret, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, zero, 32, master);                 /* Master Secret */
    transcript(c, th);                                           /* ClientHello..server Finished */
    derive_secret(master, "c ap traffic", th, c->cli_ap);
    derive_secret(master, "s ap traffic", th, c->srv_ap);
    key_iv(c->cli_ap, c->cli_apkey, c->cli_apiv);
    key_iv(c->srv_ap, c->srv_apkey, c->srv_apiv);
}

/* trae el siguiente mensaje de handshake (re-ensamblando records cifrados). Devuelve tipo, *len y puntero en c->hs */
static int next_hs_msg(tls_conn *c, u8 **msg, int *len)
{
    for (;;) {
        /* ya hay un mensaje completo en el buffer? */
        if (c->hslen - c->hspos >= 4) {
            u8 *m = c->hs + c->hspos;
            int ml = m[1] << 16 | m[2] << 8 | m[3];
            if (c->hslen - c->hspos >= 4 + ml) { *msg = m; *len = 4 + ml; c->hspos += 4 + ml; return m[0]; }
        }
        /* compactar y leer otro record */
        if (c->hspos) { xcpy(c->hs, c->hs + c->hspos, c->hslen - c->hspos); c->hslen -= c->hspos; c->hspos = 0; }
        {
            u8 rec[20000]; int len, type = read_record(c, rec, &len, 1);
            if (type < 0) return -1;
            if (type == 0x15) { log_(c, "TLS: alerta del servidor\n"); return -1; }
            if (type != 0x16) continue;              /* datos de app inesperados: ignora */
            if (c->hslen + len > (int)sizeof c->hs) return -1;
            xcpy(c->hs + c->hslen, rec, len); c->hslen += len;
        }
    }
}

/* ---- verificación del certificado (3c) ---- */
static int der_rs(const u8 *sig, int sl, u8 r[32], u8 s[32])
{
    int rl, sll, i, p = 0;
    if (sl < 8 || sig[0] != 0x30) return -1;
    p = 2; if (sig[1] & 0x80) p = 2 + (sig[1] & 0x7F);
    if (sig[p] != 0x02) return -1; rl = sig[p + 1]; const u8 *rp = sig + p + 2;
    p = p + 2 + rl; if (sig[p] != 0x02) return -1; sll = sig[p + 1]; const u8 *sp = sig + p + 2;
    while (rl > 32 && *rp == 0) { rp++; rl--; } while (sll > 32 && *sp == 0) { sp++; sll--; }
    if (rl > 32 || sll > 32) return -1;
    for (i = 0; i < 32; i++) r[i] = s[i] = 0;
    for (i = 0; i < rl; i++) r[32 - rl + i] = rp[i];
    for (i = 0; i < sll; i++) s[32 - sll + i] = sp[i];
    return 0;
}
static int verify_with(const cert *k, int scheme, const u8 *sig, int sl, const u8 *hash, int hl)
{
    if (scheme == 0x0403) { u8 r[32], s[32]; if (k->key_rsa || der_rs(sig, sl, r, s)) return 0; return ecdsa_p256_verify(k->ec, hash, hl, r, s); }
    if (scheme == 0x0804 || scheme == 0x0805) { if (!k->key_rsa) return 0; return rsa_pss_verify(k->n, k->nlen, k->e, k->elen, sig, sl, hash, hl); }
    if (scheme == 0x0401) { if (!k->key_rsa) return 0; return rsa_pkcs1_verify(k->n, k->nlen, k->e, k->elen, sig, sl, hash, hl); }
    return 0;
}
/* ¿los certs del chain[m-1] encadenan a una raíz de confianza? */
static int anchored(tls_conn *c, const cert *top)
{
    const u8 *p = c->roots; int left = c->roots_len;
    while (left > 4 && p[0] == 0x30) {
        int l = p[1] & 0x80 ? 0 : p[1], used;
        if (p[1] & 0x80) { int nb = p[1] & 0x7F, i; l = 0; for (i = 0; i < nb; i++) l = l << 8 | p[2 + i]; used = 2 + nb + l; }
        else used = 2 + l;
        if (used > left) break;
        { cert R; if (x509_parse(p, used, &R) == 0) {
            if (top->issuer_len == R.subject_len && !diff(top->issuer, R.subject, R.subject_len) && x509_check_sig(top, &R)) return 1;
        } }
        p += used; left -= used;
    }
    return 0;
}
static int tls_verify(tls_conn *c)
{
    cert chain[5]; int m = 0; const u8 *p = c->certmsg; int left = c->certmsg_len;
    if (left < 4) { log_(c, "TLS: sin certificado\n"); return 1; }
    { int ctx = p[0]; p += 1 + ctx; left -= 1 + ctx; }       /* certificate_request_context */
    { int ll = p[0] << 16 | p[1] << 8 | p[2]; p += 3; left = ll < left - 0 ? ll : left; }   /* CertificateList length */
    while (left > 3 && m < 5) {
        int clen = p[0] << 16 | p[1] << 8 | p[2]; p += 3; left -= 3;
        if (clen > left) break;
        if (x509_parse(p, clen, &chain[m])) { log_(c, "TLS: certificado ilegible\n"); return 1; }
        m++; p += clen; left -= clen;
        if (left >= 2) { int el = p[0] << 8 | p[1]; p += 2 + el; left -= 2 + el; }   /* extensiones por cert */
    }
    if (!m) return 1;

    /* 1) CertificateVerify: firma del servidor con la clave de la hoja, sobre el transcript */
    {
        int scheme = c->cverify[0] << 8 | c->cverify[1];
        int slen = c->cverify[2] << 8 | c->cverify[3];
        const u8 *sig = c->cverify + 4;
        int hl = (scheme == 0x0805) ? 48 : 32;
        u8 buf[64 + 33 + 1 + 32], h[48]; int i, n = 0;
        if (slen + 4 > c->cverify_len) return 1;
        for (i = 0; i < 64; i++) buf[n++] = 0x20;
        { const char *s = "TLS 1.3, server CertificateVerify"; while (*s) buf[n++] = (u8)*s++; }
        buf[n++] = 0;
        for (i = 0; i < 32; i++) buf[n++] = c->th_cert[i];
        hash_by_len(hl, buf, n, h);
        if (!verify_with(&chain[0], scheme, sig, slen, h, hl)) { log_(c, "TLS: la firma del handshake (CertificateVerify) no cuadra\n"); return 1; }
    }
    /* 2) cadena: cada cert firmado por el siguiente */
    { int i; for (i = 0; i + 1 < m; i++) if (!x509_check_sig(&chain[i], &chain[i + 1])) { log_(c, "TLS: la cadena de certificados no cuadra\n"); return 1; } }
    /* 3) raíz de confianza */
    if (c->roots) { if (!anchored(c, &chain[m - 1])) { log_(c, "TLS: el certificado no encadena a una raíz de confianza\n"); return 1; } }
    /* 4) nombre del host */
    if (!x509_match_host(&chain[0], c->host)) { log_(c, "TLS: el certificado no es para este host\n"); return 1; }

    c->verified = c->roots ? 1 : 0;
    return 0;
}

int tls_connect(tls_conn *c, void *io, tls_send_fn snd, tls_recv_fn rcv, const char *host)
{
    u8 srv_pub[32], shared[32], th[32], fk[32], mac[32], *msg; int type, mlen;
    c->io = io; c->snd = snd; c->rcv = rcv; c->host = host;
    c->rlen = c->rpos = c->hslen = c->hspos = c->app = 0;
    c->app_inlen = c->app_inpos = 0;
    c->certmsg_len = c->cverify_len = 0; c->verified = 0;
    sha256_init(&c->th);
    rng(c->priv, 32);

    if (send_client_hello(c)) return -1;
    if (read_server_hello(c, srv_pub)) return -1;
    x25519(shared, c->priv, srv_pub);
    derive_handshake(c, shared);

    /* flight cifrado del servidor: EncryptedExtensions, Certificate, CertificateVerify, Finished */
    for (;;) {
        type = next_hs_msg(c, &msg, &mlen);
        if (type < 0) return -1;
        if (type == 20) {                            /* Finished */
            /* verificar: HMAC(finished_key, transcript hasta antes del Finished) */
            transcript(c, th);
            expand_label(c->srv_hs, "finished", 0, 0, fk, 32);
            hmac_sha256(fk, 32, th, 32, mac);
            if (mlen - 4 != 32 || diff(mac, msg + 4, 32)) { log_(c, "TLS: Finished del servidor no cuadra\n"); return -1; }
            sha256_update(&c->th, msg, mlen);        /* ahora si, al transcript */
            break;
        }
        if (type == 11) {                            /* Certificate: lo guardamos para verificar */
            int n = mlen - 4 > (int)sizeof c->certmsg ? (int)sizeof c->certmsg : mlen - 4;
            xcpy(c->certmsg, msg + 4, n); c->certmsg_len = n;
        } else if (type == 15) {                     /* CertificateVerify: firma del servidor sobre el transcript */
            transcript(c, c->th_cert);               /* el hash incluye hasta Certificate */
            { int n = mlen - 4 > (int)sizeof c->cverify ? (int)sizeof c->cverify : mlen - 4; xcpy(c->cverify, msg + 4, n); c->cverify_len = n; }
        }
        sha256_update(&c->th, msg, mlen);
    }

    /* verificar el certificado solo si hay raíces de confianza: sin ellas no se puede autenticar, y la verificación
     * (ECDSA/RSA) es cara — no tiene sentido pagarla para tirarla. Con raíces, un fallo es fatal. */
    if (c->roots) { if (tls_verify(c)) { log_(c, "TLS: el certificado no es de fiar\n"); return -2; } }
    else c->verified = 0;

    /* claves de aplicacion (con el transcript hasta el Finished del servidor) */
    derive_app(c);

    /* nuestro Finished, con las claves de handshake todavia */
    {
        u8 cfk[32], cmac[32], fin[36];
        transcript(c, th);                           /* ClientHello..server Finished */
        expand_label(c->cli_hs, "finished", 0, 0, cfk, 32);
        hmac_sha256(cfk, 32, th, 32, cmac);
        fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 32; xcpy(fin + 4, cmac, 32);
        if (send_record(c, 0x16, fin, 36, 1)) return -1;
    }
    /* a partir de aqui, claves de aplicacion en ambos sentidos */
    c->app = 1; c->cli_seq = c->srv_seq = 0;
    return 0;
}

int tls_write(tls_conn *c, const u8 *data, int n)
{
    return send_record(c, 0x17, data, n, 1);
}

int tls_read(tls_conn *c, u8 *out, int max)
{
    for (;;) {
        u8 rec[20000]; int len, type;
        if (c->app_inlen > c->app_inpos) {
            int take = c->app_inlen - c->app_inpos; if (take > max) take = max;
            xcpy(out, c->app_in + c->app_inpos, take); c->app_inpos += take; return take;
        }
        type = read_record(c, rec, &len, 1);
        if (type < 0) return -1;
        if (type == 0x17) { if (len <= 0) continue; xcpy(c->app_in, rec, len); c->app_inlen = len; c->app_inpos = 0; continue; }
        if (type == 0x16) continue;                  /* NewSessionTicket u otro post-handshake: ignorar */
        if (type == 0x15) return 0;                  /* alerta (close_notify): fin */
    }
}
