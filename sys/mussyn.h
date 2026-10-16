/* mussyn.h - musica de DOOM (formato MUS) con el sintetizador de 8 bits de DoomProject (chip.c), en ENTEROS.
 *
 * Es el chip.c de Carlos (osciladores cuadrado/triangulo de 4 bits/sierra/hexagonal, LFSR de ruido de la NES,
 * envolvente ADSR 5/30 ms/0.8/30 ms, caida exponencial por instrumento y la bateria kick/snare/hihat/tom/crash/ride/clap
 * de CTTS) pasado a punto fijo: el kernel no tiene SSE ni libm. Los senos, la onda hexagonal, las frecuencias de las
 * notas, los decaimientos exp(-t/tau) y los resonadores estan precalculados en mustab.h (tools/gen_mustab.py).
 * Sale mono a MUS_SR (22050 Hz), 15 bits con signo. */
#ifndef MUSSYN_H
#define MUSSYN_H
#include "mustab.h"

typedef unsigned char mu8;
typedef unsigned int mu32;
typedef unsigned long long mu64;
typedef long long ms64;

#define MUS_MAXV 28
#define MUS_BUF  98304
#define SMP(ms)  ((int)((long)MUS_SR * (ms) / 1000))

enum { MW_SQ, MW_TRI, MW_SAW, MW_HEX, MW_NOISE, MW_NOISES };
typedef struct { int w; mu32 duty, k; int atk, rel, vib, bits; } MPatch;

static MPatch mpatch(int p)
{
    MPatch d = { MW_SQ, 0x80000000u, 0, SMP(5), SMP(30), 0, 0 };
    switch (p >> 3) {
    case 0:  d.duty = 0x40000000u; d.k = KMS_450; break;                       /* pianos */
    case 1:  d.w = MW_TRI; d.k = KMS_500; d.bits = 4; break;                   /* percusion cromatica */
    case 2:  break;                                                            /* organos */
    case 3:  d.duty = 0x20000000u; d.k = KMS_600; break;                       /* guitarras */
    case 4:  d.w = MW_TRI; d.bits = 4; d.k = KMS_1200; break;                  /* bajos */
    case 5:  d.w = MW_SAW; d.atk = SMP(50); d.rel = SMP(80); d.vib = 38; break; /* cuerdas */
    case 6:  d.atk = SMP(20); break;                                           /* metales */
    case 7:  d.w = MW_SAW; d.atk = SMP(15); d.vib = 26; break;                 /* lenguetas */
    case 8:  d.w = MW_TRI; d.bits = 4; d.vib = 26; break;                      /* flautas */
    case 9:  d.duty = 0x40000000u; d.vib = 51; break;                          /* lead cuadrado */
    case 10: d.w = MW_SAW; d.vib = 51; break;                                  /* lead sierra */
    case 11: d.w = MW_TRI; d.bits = 4; d.atk = SMP(80); d.rel = SMP(120); break; /* pads */
    case 12: d.w = MW_HEX; d.k = KMS_700; break;                               /* fx */
    case 13: d.w = MW_HEX; d.k = KMS_500; break;                               /* etnicos */
    case 14: d.w = MW_NOISES; d.k = KMS_120; break;                            /* percusivos */
    default: d.w = MW_NOISE; d.k = KMS_300; break;                             /* sfx */
    }
    if (p >= 29 && p <= 31) { d.w = MW_SQ; d.duty = 0x20000000u; d.k = 0; }    /* guitarra distorsionada: sostenida */
    if (p >= 32 && p <= 39) { d.w = MW_TRI; d.bits = 4; d.k = (p == 33 || p == 34) ? 0 : KMS_1200; }
    if (p >= 38 && p <= 39) { d.w = MW_SQ; d.duty = 0x80000000u; d.k = 0; }    /* bajos sinteticos */
    return d;
}

static int lfsr_step(mu32 *st, int tap)
{
    mu32 fb = (*st & 1) ^ ((*st >> tap) & 1);
    *st = (*st >> 1) | (fb << 14);
    return (*st & 1) ? -1 : 1;
}
static int clamp15(int x) { return x < -32767 ? -32767 : (x > 32767 ? 32767 : x); }
static int mres(const int *c, ms64 *y1, ms64 *y2, int x)     /* resonador de 2 polos, estado en Q20 */
{
    ms64 y = ((ms64)c[0] * x >> 15) + ((ms64)c[1] * *y1 >> 20) + ((ms64)c[2] * *y2 >> 20);
    *y2 = *y1;
    *y1 = y;
    return (int)(y >> 5);
}

/* ---- bateria ---- */
enum { DK_KICK, DK_SNARE, DK_HAT, DK_TOM, DK_CRASH, DK_RIDE, DK_CLAP };
typedef struct {
    int on, k, vol, nr, freq;
    mu32 t, dur, ph, sa, sb, e1, e2, e3, k1, k2, k3;
    const int (*rc)[3];
    ms64 y1[6], y2[6];
} MDrum;

static void mdrum_init(MDrum *d, int note, int vol)
{
    int i;
    for (i = 0; i < (int)sizeof(*d); i++) ((mu8 *)d)[i] = 0;
    d->on = 1; d->vol = vol; d->sa = d->sb = 1;
    d->e1 = d->e2 = d->e3 = 0xFFFFFFFFu;
    switch (note) {
    case 35: case 36: d->k = DK_KICK; d->freq = 60; d->dur = SMP(250); d->k1 = KMS_120; d->k2 = KMS_45; d->k3 = KMS_4; break;
    case 38: case 40: case 37: d->k = DK_SNARE; d->freq = 190; d->dur = SMP(200); d->k1 = KMS_70; d->k2 = KMS_90; break;
    case 39: d->k = DK_CLAP; d->dur = SMP(150); d->rc = mus_res_clap; d->nr = 1; d->k1 = KMS_8; d->k2 = KMS_150; d->e2 = 0; break;
    case 42: case 44: d->k = DK_HAT; d->dur = SMP(60); d->k1 = KMS_20; break;
    case 46: d->k = DK_HAT; d->dur = SMP(300); d->k1 = KMS_100; break;
    case 41: case 43: case 45: case 47: case 48: case 50: {
        static const int F[] = { 80, 100, 120, 140, 170, 200 };
        d->k = DK_TOM; d->dur = SMP(350);
        d->freq = F[(note == 41) ? 0 : (note == 43) ? 1 : (note == 45) ? 2 : (note == 47) ? 3 : (note == 48) ? 4 : 5];
        d->k1 = KMS_180; d->k2 = KMS_60; break; }
    case 49: case 52: case 55: case 57:
        d->k = DK_CRASH; d->dur = SMP(1200); d->k1 = KMS_700; d->rc = mus_res_crash; d->nr = 6; break;
    case 51: case 53: case 59:
        d->k = DK_RIDE; d->dur = SMP(800); d->k1 = KMS_400; d->k2 = KMS_520; d->rc = mus_res_ride; d->nr = 5; break;
    default: d->k = DK_HAT; d->dur = SMP(80); d->k1 = KMS_30; break;
    }
}
#define DECAY(e, k) ((mu32)((mu64)(e) * (k) >> 32))
static int mdrum_next(MDrum *d)
{
    mu32 t = d->t++;
    int a1 = (int)(d->e1 >> 17), x = 0, nz, i;
    if (t > d->dur) { d->on = 0; return 0; }
    switch (d->k) {
    case DK_KICK: {
        int a3 = (int)(d->e3 >> 17);
        mu32 fq = 33 * 256 + (mu32)((mu64)(147 * 256) * d->e2 >> 32);
        d->ph += (mu32)((mu64)fq * 194781u >> 8);
        nz = lfsr_step(&d->sa, 1) * 32767;
        x = (mus_sin[d->ph >> 23] * a1 >> 15) + ((nz * a3 >> 15) * 11469 >> 15);
        d->e1 = DECAY(d->e1, d->k1); d->e2 = DECAY(d->e2, d->k2); d->e3 = DECAY(d->e3, d->k3);
        break; }
    case DK_SNARE: {
        int a2 = (int)(d->e2 >> 17);
        d->ph += (mu32)((mu64)d->freq * 194781u);
        nz = lfsr_step(&d->sa, 1) * 32767;
        x = ((mus_sin[d->ph >> 23] * a1 >> 15) * 13107 >> 15) + ((nz * a2 >> 15) * 27853 >> 15);
        d->e1 = DECAY(d->e1, d->k1); d->e2 = DECAY(d->e2, d->k2);
        break; }
    case DK_HAT:
        nz = (lfsr_step(&d->sa, 1) + lfsr_step(&d->sb, 6)) * 16384;
        x = nz * a1 >> 15;
        d->e1 = DECAY(d->e1, d->k1);
        break;
    case DK_TOM: {
        mu32 fq = (mu32)d->freq * 218 + (mu32)((mu64)((mu32)d->freq * 243) * d->e2 >> 32);   /* 0.85F + 0.95F*exp, Q8 */
        d->ph += (mu32)((mu64)fq * 194781u >> 8);
        x = mus_sin[d->ph >> 23] * a1 >> 15;
        d->e1 = DECAY(d->e1, d->k1); d->e2 = DECAY(d->e2, d->k2);
        break; }
    case DK_CRASH: {
        int s = 0;
        nz = (lfsr_step(&d->sa, 1) + lfsr_step(&d->sb, 6)) * 16384;
        for (i = 0; i < 6; i++) s += mres(d->rc[i], &d->y1[i], &d->y2[i], nz);
        x = (((nz * 14746 >> 15) + (s * 8738 >> 15)) * a1) >> 15;
        d->e1 = DECAY(d->e1, d->k1);
        break; }
    case DK_RIDE: {
        int by, s = 0, a2 = (int)(d->e2 >> 17);
        nz = lfsr_step(&d->sa, 1) * 32767;
        by = mres(d->rc[0], &d->y1[0], &d->y2[0], nz);
        for (i = 0; i < 4; i++) s += mres(d->rc[1 + i], &d->y1[1 + i], &d->y2[1 + i], nz);
        x = ((by * 29491 >> 15) * a2 >> 15) + (((s * 4506 >> 15) + (nz * 3932 >> 15)) * a1 >> 15);
        d->e1 = DECAY(d->e1, d->k1); d->e2 = DECAY(d->e2, d->k2);
        break; }
    case DK_CLAP: {
        int y, env, a2 = (int)(d->e2 >> 17);
        if (t == 0 || t == (mu32)SMP(12) || t == (mu32)SMP(24) || t == (mu32)SMP(38)) d->e1 = 0xFFFFFFFFu;
        if (t == (mu32)SMP(45)) d->e2 = 0xFFFFFFFFu / 5 * 3;                     /* 0.6 */
        a1 = (int)(d->e1 >> 17); a2 = (int)(d->e2 >> 17);
        env = a1 > a2 ? a1 : a2;
        nz = lfsr_step(&d->sa, 1) * 32767;
        y = mres(d->rc[0], &d->y1[0], &d->y2[0], nz);
        x = ((y * env >> 15) * 26214) >> 15;
        d->e1 = DECAY(d->e1, d->k1);
        if (t >= (mu32)SMP(45)) d->e2 = DECAY(d->e2, d->k2);
        break; }
    }
    return clamp15(clamp15(x) * d->vol >> 15);
}

/* ---- voces melodicas ---- */
typedef struct {
    int on, ch, note, held, stage, vel, last;
    MPatch p;
    mu32 ph, t, ea, astep, dstep, et, relr, rstep, ns, vph;
} MVoice;

static MVoice mv[MUS_MAXV];
static MDrum md[8];
static struct { int prog, vol, bend; } mch[16];
static mu8 msy_song[MUS_BUF];
static int msy_len, msy_pos, msy_start, msy_playing, msy_looping, msy_paused, msy_wait;
static int msy_gk = 64 * 99;                    /* 0.12 * volumen/127 * 1.6 en Q16 (volumen 0..127) */
static int msy_lastvel[16];

static void mus_chreset(void)
{
    int i;
    for (i = 0; i < 16; i++) { mch[i].prog = 0; mch[i].vol = 100; mch[i].bend = 128; msy_lastvel[i] = 100; }
}
static void mus_alloff(void)
{
    int i;
    for (i = 0; i < (int)sizeof(mv); i++) ((mu8 *)mv)[i] = 0;
    for (i = 0; i < (int)sizeof(md); i++) ((mu8 *)md)[i] = 0;
}
static void mus_noteon(int ch, int note, int vel)
{
    int s = -1, i;
    MVoice *v;
    if (ch == 15) {
        int dv = vel * 29491 / 127;                                   /* vel/127 * 0.9 en Q15 */
        for (i = 0; i < 8; i++) if (!md[i].on) { mdrum_init(&md[i], note, dv); return; }
        mdrum_init(&md[0], note, dv);
        return;
    }
    for (i = 0; i < MUS_MAXV; i++) if (!mv[i].on) { s = i; break; }
    if (s < 0) {
        mu32 mt = 0;
        s = 0;
        for (i = 0; i < MUS_MAXV; i++) if (mv[i].t >= mt) { mt = mv[i].t; s = i; }
    }
    v = &mv[s];
    for (i = 0; i < (int)sizeof(*v); i++) ((mu8 *)v)[i] = 0;
    v->on = 1; v->ch = ch; v->note = note; v->held = 1; v->p = mpatch(mch[ch].prog); v->vel = vel; v->ns = 1;
    v->astep = 65536u / (mu32)v->p.atk; if (!v->astep) v->astep = 1;
    v->dstep = (65536u - 52429u) / (mu32)SMP(30);
    v->et = 0xFFFFFFFFu; v->relr = 65536u;
    v->rstep = 65536u / (mu32)v->p.rel; if (!v->rstep) v->rstep = 1;
}
static void mus_noteoff(int ch, int note)
{
    int i;
    for (i = 0; i < MUS_MAXV; i++) if (mv[i].on && mv[i].held && mv[i].ch == ch && mv[i].note == note) mv[i].held = 0;
}
static int mv_next(MVoice *v)
{
    MPatch *p = &v->p;
    int pitch = v->note * 256 + (mch[v->ch].bend - 128) * 4, ix, s = 0;
    mu32 a, b, inc, e;
    if (p->vib > 0 && v->t > (mu32)SMP(150)) {                       /* vibrato de 5.5 Hz tras 0.15 s */
        pitch += (p->vib * mus_sin[v->vph >> 23]) >> 15;
        v->vph += 1071293u;
    }
    if (pitch < 0) pitch = 0;
    ix = pitch >> 8;
    a = mus_inc[ix]; b = mus_inc[ix + 1];
    inc = a + (mu32)((mu64)(b - a) * (mu32)(pitch & 255) >> 8);
    v->ph += inc;
    switch (p->w) {
    case MW_SQ: s = v->ph < p->duty ? 32767 : -32767; break;
    case MW_TRI: {
        int x = (int)(v->ph >> 16), tri = x < 32768 ? x : 65535 - x;
        s = tri * 2 - 32768;
        if (p->bits) {
            int u = (s + 32768) >> 1, q = (1 << p->bits) - 1, k = (u * q + 16384) >> 15;
            s = (k * 65536) / q - 32768;
        }
        break; }
    case MW_SAW: s = (int)(v->ph >> 16) - 32768; break;
    case MW_HEX: s = mus_hex[v->ph >> 24]; break;
    default:
        if (v->ph < inc || v->t == 0) v->last = lfsr_step(&v->ns, p->w == MW_NOISE ? 1 : 6) * 32767;
        s = v->last;
        break;
    }
    if (v->stage == 0) { e = v->ea; v->ea += v->astep; if (v->ea >= 65536u) { v->ea = 65536u; v->stage = 1; } }
    else if (v->stage == 1) { e = v->ea; v->ea -= v->dstep; if (v->ea <= 52429u) { v->ea = 52429u; v->stage = 2; } }
    else e = 52429u;
    if (p->k) { e = (mu32)((mu64)e * (v->et >> 16) >> 16); v->et = DECAY(v->et, p->k); }
    v->t++;
    if (!v->held) {
        if (v->relr <= v->rstep) { v->on = 0; return 0; }
        v->relr -= v->rstep;
        e = (mu32)((mu64)e * v->relr >> 16);
    }
    return (int)((ms64)s * e >> 16) * v->vel * mch[v->ch].vol / 16129;
}

static int mus_rd(void) { return msy_pos < msy_len ? msy_song[msy_pos++] : 0; }
static void mus_events(void)                    /* procesa eventos hasta el siguiente retardo */
{
    for (;;) {
        int b, last, type, ch;
        if (msy_pos >= msy_len) { msy_playing = 0; return; }
        b = mus_rd(); last = b & 0x80; type = (b >> 4) & 7; ch = b & 15;
        switch (type) {
        case 0: mus_noteoff(ch, mus_rd() & 127); break;
        case 1: {
            int n = mus_rd(), vel = -1;
            if (n & 0x80) vel = mus_rd() & 127;
            n &= 127;
            if (vel < 0) vel = msy_lastvel[ch]; else msy_lastvel[ch] = vel;
            mus_noteon(ch, n, vel);
            break; }
        case 2: mch[ch].bend = mus_rd(); break;
        case 3: mus_rd(); break;
        case 4: { int c = mus_rd(), val = mus_rd(); if (c == 0) mch[ch].prog = val & 127; else if (c == 3) mch[ch].vol = val & 127; break; }
        case 5: break;
        case 6: if (msy_looping) { msy_pos = msy_start; mus_chreset(); mus_alloff(); } else { msy_playing = 0; return; } break;
        default: mus_rd(); break;
        }
        if (last) {
            int t = 0, x;
            do { x = mus_rd(); t = t * 128 + (x & 127); } while (x & 0x80);
            msy_wait = (int)((long)t * 315 / 2);                    /* ticks de 1/140 s a MUS_SR */
            if (msy_wait > 0) return;
        }
    }
}

/* Una muestra mono a MUS_SR (Q15). 0 si no hay musica sonando. */
static int mus_next(void)
{
    int i, m = 0;
    if (!msy_playing || msy_paused) return 0;
    while (msy_wait <= 0 && msy_playing) {
        mus_events();
        if (msy_wait <= 0 && !msy_playing) return 0;
    }
    msy_wait--;
    for (i = 0; i < MUS_MAXV; i++) if (mv[i].on) m += mv_next(&mv[i]);
    for (i = 0; i < 8; i++) if (md[i].on) m += mdrum_next(&md[i]);
    return clamp15((int)((ms64)m * msy_gk >> 16));
}

/* ---- ganchos (los llama PureDOOM en I_RegisterSong/I_PlaySong/...) ---- */
static void mus_reg(const void *data)
{
    const mu8 *b = (const mu8 *)data;
    int len, ofs, n, i;
    msy_playing = 0;
    if (!b || b[0] != 'M' || b[1] != 'U' || b[2] != 'S' || b[3] != 0x1A) { msy_len = 0; return; }
    len = b[4] | (b[5] << 8); ofs = b[6] | (b[7] << 8);
    n = len + ofs;
    if (n > MUS_BUF) n = MUS_BUF;
    for (i = 0; i < n; i++) msy_song[i] = b[i];
    msy_len = n; msy_start = ofs;
}
static void mus_play(int loop)
{
    if (!msy_len) return;
    mus_chreset(); mus_alloff();
    msy_pos = msy_start; msy_looping = loop; msy_wait = 0; msy_paused = 0; msy_playing = 1;
}
static void mus_stop(void) { msy_playing = 0; mus_alloff(); }
static void mus_pause(int p) { msy_paused = p; }
static void mus_vol(int v) { if (v < 0) v = 0; if (v > 127) v = 127; msy_gk = v * 99; }
#endif
