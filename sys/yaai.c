/* yaai.c - YAAI (You Are An Idiot): video convertido a texto + audio ADPCM comprimido
 * Modulo /SYS/YAAI.APP. Los datos estan en el disquete FD1 (YAAI.DAT):
 *   'YAAI', cuadros, bytes del video sin comprimir, bytes del video deflate, muestras de audio,
 *   frecuencia de audio (u32 cada uno), fps*10 (u16), reservado (u16), video deflate, audio ADPCM IMA.
 * El video son deltas de celdas 120x45 (varint n; n x [varint salto, glifo]) comprimidos con deflate.
 * El audio da el reloj: el cuadro que se pinta sale de los ms de audio ya reproducidos.
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

static const struct kapi *K;

#define DATA_ADDR 0x1000000u        /* aqui se lee YAAI.DAT (max 1.4 MB) */
#define DATA_MAX  0x180000u
#define VID_ADDR  0x1200000u        /* aqui se descomprime el video */
#define VCOLS 120
#define VROWS 45

/* ---- inflate (deflate crudo) ---- */

struct tree { u16 cnt[16]; u16 sym[288]; };

static const u8 *sp, *se;
static u32 btag;
static int bcnt;
static u8 *dp;

static int getbit(void)
{
    int b;
    if (!bcnt) {
        btag = sp < se ? *sp++ : 0;
        bcnt = 8;
    }
    b = (int)(btag & 1);
    btag >>= 1;
    bcnt--;
    return b;
}

static u32 getbits(int n)
{
    u32 v = 0;
    int i;
    for (i = 0; i < n; i++)
        v |= (u32)getbit() << i;
    return v;
}

static void build(struct tree *t, const u8 *len, int num)
{
    u16 off[16];
    int i;
    for (i = 0; i < 16; i++)
        t->cnt[i] = 0;
    for (i = 0; i < num; i++)
        t->cnt[len[i]]++;
    t->cnt[0] = 0;
    off[0] = off[1] = 0;
    for (i = 1; i < 15; i++)
        off[i + 1] = (u16)(off[i] + t->cnt[i]);
    for (i = 0; i < num; i++)
        if (len[i])
            t->sym[off[len[i]]++] = (u16)i;
}

static int decode(const struct tree *t)
{
    int sum = 0, cur = 0, len = 0;
    do {
        cur = 2 * cur + getbit();
        len++;
        if (len > 15)
            return -1;
        sum += t->cnt[len];
        cur -= t->cnt[len];
    } while (cur >= 0);
    return t->sym[sum + cur];
}

static u8 lbits[30], dbits[30];
static u16 lbase[30], dbase[30];

static void bits_base(u8 *bits, u16 *base, int delta, int first)
{
    int i, sum;
    for (i = 0; i < delta; i++)
        bits[i] = 0;
    for (i = 0; i < 30 - delta; i++)
        bits[i + delta] = (u8)(i / delta);
    for (i = 0, sum = first; i < 30; i++) {
        base[i] = (u16)sum;
        sum += 1 << bits[i];
    }
}

static int inflate_block(const struct tree *lt, const struct tree *dt)
{
    for (;;) {
        int sym = decode(lt);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            *dp++ = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            int length, dist, i;
            const u8 *from;
            sym -= 257;
            if (sym >= 29)
                return -1;
            length = (int)lbase[sym] + (int)getbits(lbits[sym]);
            sym = decode(dt);
            if (sym < 0 || sym >= 30)
                return -1;
            dist = (int)dbase[sym] + (int)getbits(dbits[sym]);
            from = dp - dist;
            for (i = 0; i < length; i++)
                dp[i] = from[i];
            dp += length;
        }
    }
}

static struct tree t_lit, t_dist, t_code;

static int inflate_raw(const u8 *src, u32 n, u8 *dst)
{
    static const u8 clc[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int final;
    sp = src; se = src + n; bcnt = 0; dp = dst;
    bits_base(lbits, lbase, 4, 3);
    lbits[28] = 0;
    lbase[28] = 258;
    bits_base(dbits, dbase, 2, 1);
    do {
        int type;
        final = getbit();
        type = (int)getbits(2);
        if (type == 0) {
            u32 len;
            bcnt = 0;
            if (se - sp < 4)
                return -1;
            len = sp[0] | ((u32)sp[1] << 8);
            sp += 4;
            while (len-- && sp < se)
                *dp++ = *sp++;
        } else if (type == 1) {
            u8 l[288 + 30];
            int i;
            for (i = 0; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            for (i = 0; i < 30; i++) l[288 + i] = 5;
            build(&t_lit, l, 288);
            build(&t_dist, l + 288, 30);
            if (inflate_block(&t_lit, &t_dist) < 0)
                return -1;
        } else if (type == 2) {
            u8 l[320];
            int hlit = (int)getbits(5) + 257, hdist = (int)getbits(5) + 1, hclen = (int)getbits(4) + 4;
            int i, num = 0;
            for (i = 0; i < 19; i++) l[i] = 0;
            for (i = 0; i < hclen; i++)
                l[clc[i]] = (u8)getbits(3);
            build(&t_code, l, 19);
            while (num < hlit + hdist) {
                int sym = decode(&t_code), rep, val = 0;
                if (sym < 0)
                    return -1;
                if (sym < 16) {
                    l[num++] = (u8)sym;
                    continue;
                }
                if (sym == 16) {
                    if (!num)
                        return -1;
                    val = l[num - 1];
                    rep = 3 + (int)getbits(2);
                } else if (sym == 17) {
                    rep = 3 + (int)getbits(3);
                } else {
                    rep = 11 + (int)getbits(7);
                }
                if (num + rep > hlit + hdist)
                    return -1;
                while (rep--)
                    l[num++] = (u8)val;
            }
            build(&t_lit, l, hlit);
            build(&t_dist, l + hlit, hdist);
            if (inflate_block(&t_lit, &t_dist) < 0)
                return -1;
        } else {
            return -1;
        }
    } while (!final);
    return (int)(dp - dst);
}

/* ---- reproductor ---- */

static u8 cell[VCOLS * VROWS];
static const u8 *vp;                /* lectura del video descomprimido */

static u32 varint(void)
{
    u32 v = 0;
    int sh = 0;
    for (;;) {
        u8 b = *vp++;
        v |= (u32)(b & 127) << sh;
        if (!(b & 128))
            return v;
        sh += 7;
    }
}

static void apply_frame(void)
{
    u32 n = varint(), i, pos = 0;
    int first = 1;
    for (i = 0; i < n; i++) {
        u32 skip = varint();
        pos = first ? skip : pos + skip + 1;
        first = 0;
        if (pos < VCOLS * VROWS)
            cell[pos] = *vp;
        vp++;
    }
}

static u32 rd32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

int mod_main(const struct kapi *api)
{
    const u8 *dat = (const u8 *)DATA_ADDR;
    unsigned total = 0;
    u32 frames, vraw, vz, nsamp, rate, fps10, fi = 0, dur_ms;
    const u8 *vzp, *aud;
    int r, ok, has_audio, key, quit = 0;
    u32 t0;

    K = api;
    K->print("\nYAAI - YOU ARE AN IDIOT\nCARGANDO FD1:YAAI.DAT...\n");
    r = K->read("FD1:YAAI.DAT", (char *)DATA_ADDR, (int)DATA_MAX, &total);
    if (r < 0 || total > DATA_MAX || r < 28 || dat[0] != 'Y' || dat[1] != 'A' || dat[2] != 'A' || dat[3] != 'I') {
        K->print("NO ENCUENTRO YAAI.DAT EN FD1. PON EL DISCO YAAI.IMG COMO SEGUNDO DISQUETE.\n");
        return 0;
    }
    frames = rd32(dat + 4); vraw = rd32(dat + 8); vz = rd32(dat + 12);
    nsamp = rd32(dat + 16); rate = rd32(dat + 20);
    fps10 = dat[24] | ((u32)dat[25] << 8);
    vzp = dat + 28;
    aud = vzp + vz;
    if (28 + vz + (nsamp + 1) / 2 > (u32)r || vraw > 0x200000u || !fps10) {
        K->print("YAAI.DAT INCOMPLETO\n");
        return 0;
    }
    ok = inflate_raw(vzp, vz, (u8 *)VID_ADDR);
    if (ok != (int)vraw) {
        K->print("ERROR AL DESCOMPRIMIR EL VIDEO\n");
        return 0;
    }
    r = K->vid(0, 0, 0, 0);
    if ((r & 0xFFFF) < VCOLS || (r >> 16) < VROWS) {
        K->vid(2, 0, 0, 0);
        K->print("YAAI NECESITA UNA PANTALLA DE AL MENOS 120X45 CARACTERES (1024X768 O MAS)\n");
        return 0;
    }
    memset(cell, ' ', sizeof cell);
    vp = (const u8 *)VID_ADDR;
    dur_ms = (u32)((unsigned long long)frames * 10000 / fps10);
    K->esc();
    has_audio = K->pcm_play(aud, nsamp, rate);
    t0 = K->ms();
    while (!quit) {
        u32 t = has_audio ? K->pcm_ms() : K->ms() - t0, target;
        int changed = 0;
        if (has_audio && t == 0 && K->ms() - t0 > 400)
            has_audio = 0;                                   /* la tarjeta no arranco: seguir con el reloj */
        target = (u32)((unsigned long long)t * fps10 / 10000);
        while (fi <= target && fi < frames) {
            apply_frame();
            fi++;
            changed = 1;
        }
        if (changed)
            K->vid(1, cell, VCOLS, VROWS);
        key = K->poll();
        if (key == 27 || K->esc())
            quit = 1;
        if (fi >= frames && t >= dur_ms + 300)
            quit = 1;
    }
    K->pcm_stop();
    K->vid(2, 0, 0, 0);
    K->print("\nYAAI: YOU ARE AN IDIOT.\n");
    return 0;
}
