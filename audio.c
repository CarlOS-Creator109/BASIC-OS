/* audio.c - motor de audio
 *
 * Una tarjeta (HDA o AC'97) reproduce un anillo de 64 KB (16384 cuadros estereo de 16 bits a 48 kHz)
 * en bucle. audio_service() mira donde va la reproduccion y escribe por delante el siguiente trozo,
 * mezclando hasta 8 voces sinteticas (ondas, bombo, charles, caja, bateria automatica) y,
 * opcionalmente, un CD de audio remuestreado de 44.1 a 48 kHz. No hay interrupciones: se llama
 * desde el sondeo del teclado y desde las esperas, asi que las voces siguen sonando mientras
 * BASIC ejecuta el programa.
 */
#include "audio.h"

#define RING_BYTES 65536u
#define CHUNK      2048u                /* 512 cuadros */
#define AHEAD      24576u               /* cuanto se escribe por delante de la reproduccion */
#define AHEAD_STREAM 16384u             /* con flujo en vivo (DOOM): menos latencia */
#define NVOICE     8
#define RATE       48000

u64 g_tsc_hz;

static s16 ring[RING_BYTES / 2] __attribute__((aligned(4096)));

/* ---- seno ---- */

static const u16 qsine[65] = {          /* seno de 0 a 90 grados, amplitud 32767 */
        0,  804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279,
    24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268,
    29621, 29956, 30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137,
    32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767
};

static int sin4096(u32 ph)              /* ph: 0..4095 = una vuelta; resultado +-32767 */
{
    int q = (int)((ph >> 10) & 3), idx = (int)((ph & 1023) >> 4), frac = (int)(ph & 15);
    int a, b;
    if (q & 1) {
        a = qsine[64 - idx];
        b = qsine[idx >= 63 ? 0 : 63 - idx];
        a = a + (b - a) * frac / 16;
    } else {
        a = qsine[idx];
        b = qsine[idx + 1 > 64 ? 64 : idx + 1];
        a = a + (b - a) * frac / 16;
    }
    return q & 2 ? -a : a;
}

/* ---- voces ---- */

struct voice {
    u8  on, kind;
    u32 phase, step;                    /* acumulador de fase de 32 bits */
    u32 left, age;                      /* cuadros restantes / transcurridos */
    int e;                              /* envolvente 0..65535 */
    int f256, extra;                    /* frecuencia (1/256 Hz) y barrido del bombo */
    int lp;                             /* filtro del charles */
    u32 rng;
    int amp;
    int dk_type, dk_step;               /* bateria automatica */
    u32 dk_next, dk_len, dk_t;
    int dk_base;
};

static struct voice vox[NVOICE];
static u32 noise_seed = 12345;

static u32 rnd(u32 *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

static struct voice *voice_alloc(int keep_drumkit)
{
    int i, best = 0;
    u32 oldest = 0;
    for (i = 0; i < NVOICE; i++)
        if (!vox[i].on)
            return &vox[i];
    for (i = 0; i < NVOICE; i++) {              /* robar la mas vieja que no sea la bateria */
        if (keep_drumkit && vox[i].kind == SND_DRUMKIT)
            continue;
        if (vox[i].age >= oldest) {
            oldest = vox[i].age;
            best = i;
        }
    }
    return &vox[best];
}

static u32 hz_step(int hz256)           /* paso de fase para f (en 1/256 Hz) a 48 kHz */
{
    return (u32)(((u64)(unsigned int)hz256 * 22906492u) >> 16);   /* 2^32 / 48000 / 256 en Q16 */
}

static void voice_start(int kind, int hz, int arg, int ms)
{
    struct voice *v = voice_alloc(kind != SND_DRUMKIT);
    u32 frames;

    memset(v, 0, sizeof *v);
    v->on = 1;
    v->kind = (u8)kind;
    v->rng = noise_seed = noise_seed * 7 + 99991;
    v->e = 65535;
    v->amp = 8000;
    if (hz < 0)
        hz = 0;
    switch (kind) {
    case SND_KICK:
        v->f256 = (hz >= 20 ? hz : 55) * 256;
        v->extra = v->f256 * 3;
        v->amp = 14000;
        if (ms <= 0) ms = 280;
        break;
    case SND_HIHAT:
        v->amp = 7000;
        if (ms <= 0) ms = 70;
        break;
    case SND_SNARE:
        v->f256 = (hz >= 80 ? hz : 190) * 256;
        v->amp = 9000;
        if (ms <= 0) ms = 170;
        break;
    case SND_DRUMKIT:
        v->dk_type = arg & 3;
        v->dk_base = hz >= 30 ? hz : 55;
        if (ms <= 0) ms = 4000;
        break;
    default:
        v->f256 = (hz >= 20 ? hz : 440) * 256;
        v->step = hz_step(v->f256);
        if (ms <= 0) ms = 200;
        break;
    }
    if (ms > 3600000)
        ms = 3600000;
    frames = (u32)ms * (RATE / 1000);
    v->left = frames;
}

/* patrones de 16 pasos: bombo, caja, charles ('o' = charles abierto) y tempo (BPM) */
static const struct { const char *k, *s, *h; int bpm; } kits[4] = {
    { "x.......x.......", "....x.......x...", "x.x.x.x.x.x.x.x.", 120 },   /* rock */
    { "x...x...x...x...", "....x.......x...", "..o...o...o...o.", 124 },   /* disco */
    { "x..x..x...x..x..", "....x.......x...", "x.xxx.xxx.xxx.xx",  90 },   /* hip hop */
    { "x.........x.....", "....x.......x...", "x.x.x.x.x.x.x.x.", 170 }    /* drum and bass */
};

static void drumkit_step(struct voice *v)
{
    int s = v->dk_step & 15;
    const char *k = kits[v->dk_type].k, *sn = kits[v->dk_type].s, *h = kits[v->dk_type].h;
    if (k[s] == 'x')
        voice_start(SND_KICK, v->dk_base, 0, 0);
    if (sn[s] == 'x')
        voice_start(SND_SNARE, v->dk_base * 3 + 10, 0, 0);
    if (h[s] == 'x')
        voice_start(SND_HIHAT, 0, 0, 55);
    else if (h[s] == 'o')
        voice_start(SND_HIHAT, 0, 0, 160);
    v->dk_step++;
}

static int voice_frame(struct voice *v)         /* una muestra mono */
{
    int s = 0, g, e2;
    u32 p;

    if (!v->left) {
        v->on = 0;
        return 0;
    }
    switch (v->kind) {
    case SND_SINE:   s = sin4096(v->phase >> 20); break;
    case SND_SQUARE: s = (v->phase & 0x80000000u) ? -26000 : 26000; break;
    case SND_TRIANGLE:
        p = v->phase >> 16;
        s = (int)(p < 32768 ? p : 65535 - p) * 2 - 32767;
        break;
    case SND_SAW:    s = (int)(v->phase >> 16) - 32768; break;
    case SND_KICK:
        s = sin4096(v->phase >> 20);
        v->extra -= v->extra / 1100;
        v->step = hz_step(v->f256 + v->extra);
        v->e -= v->e / 5200;
        s = (int)((long)s * v->e >> 16);
        break;
    case SND_HIHAT: {
        int n = (int)(rnd(&v->rng) & 0xFFFF) - 32768;
        v->lp += (n - v->lp) / 2;
        s = n - v->lp;
        v->e -= v->e / 600;
        s = (int)((long)s * v->e >> 16);
        break;
    }
    case SND_SNARE: {
        int n = (int)(rnd(&v->rng) & 0xFFFF) - 32768;
        v->step = hz_step(v->f256);
        s = (sin4096(v->phase >> 20) / 2 + n / 2);
        v->e -= v->e / 1800;
        s = (int)((long)s * v->e >> 16);
        break;
    }
    case SND_DRUMKIT:
        if (v->dk_t == v->dk_next) {
            drumkit_step(v);
            v->dk_next += (u32)(60 * RATE / kits[v->dk_type].bpm / 4);
        }
        v->dk_t++;
        v->left--;
        v->age++;
        return 0;
    }
    v->phase += v->step;
    g = v->age < 96 ? (int)(v->age * 256 / 96) : 256;           /* ataque 2 ms: sin clics */
    e2 = v->left < 240 ? (int)(v->left * 256 / 240) : 256;      /* y fin 5 ms */
    if (e2 < g)
        g = e2;
    v->left--;
    v->age++;
    return (int)((long)s * v->amp / 32768 * g / 256);
}

/* ---- CD de audio ---- */

#define CD_SECT   8
#define CD_FRAMES (CD_SECT * 588)

static cd_read_fn cd_fn;
static unsigned int cd_lba, cd_end, cd_start_lba;
static int cd_on;
static s16 cd_buf[(CD_FRAMES + 2) * 2];
static int cd_n, cd_i;                          /* cuadros validos / indice de lectura */
static u32 cd_frac;                             /* fraccion Q16 entre cuadros */

static int cd_refill(void)
{
    int n, sec;
    if (cd_n > 0) {                             /* conservar el ultimo cuadro para interpolar */
        cd_buf[0] = cd_buf[(cd_n - 1) * 2];
        cd_buf[1] = cd_buf[(cd_n - 1) * 2 + 1];
        cd_i -= cd_n - 1;
    } else {
        cd_buf[0] = cd_buf[1] = 0;
        cd_i = 0;
    }
    cd_n = 1;
    if (cd_lba >= cd_end)
        return 0;
    sec = (int)(cd_end - cd_lba < CD_SECT ? cd_end - cd_lba : CD_SECT);
    if (cd_fn(cd_lba, sec, cd_buf + 2)) {
        cd_lba = cd_end;                        /* error de lectura: terminar */
        return 0;
    }
    cd_lba += (unsigned int)sec;
    n = sec * 588;
    cd_n = 1 + n;
    return 1;
}

int audio_cd_play(cd_read_fn fn, unsigned int lba, unsigned int end_lba)
{
    if (!audio_open())
        return 0;
    cd_fn = fn;
    cd_lba = cd_start_lba = lba;
    cd_end = end_lba;
    cd_n = 0;
    cd_i = 0;
    cd_frac = 0;
    cd_on = 1;
    if (!cd_refill() && cd_n <= 1) {
        cd_on = 0;
        return 0;
    }
    return 1;
}

void audio_cd_stop(void) { cd_on = 0; }
int  audio_cd_active(void) { return cd_on; }
unsigned int audio_cd_lba(void) { return cd_lba; }

static void cd_mix(int *l, int *r)
{
    int a, b, fr;
    if (cd_i + 1 >= cd_n && !cd_refill() && cd_i + 1 >= cd_n) {
        cd_on = 0;
        return;
    }
    fr = (int)(cd_frac >> 8);                   /* 0..255 */
    a = cd_buf[cd_i * 2];
    b = cd_buf[(cd_i + 1) * 2];
    *l += a + (b - a) * fr / 256;
    a = cd_buf[cd_i * 2 + 1];
    b = cd_buf[(cd_i + 1) * 2 + 1];
    *r += a + (b - a) * fr / 256;
    cd_frac += 60211;                           /* 44100 / 48000 * 65536 */
    cd_i += (int)(cd_frac >> 16);
    cd_frac &= 0xFFFF;
}

/* ---- flujo PCM: ADPCM IMA de 4 bits mono, nibble bajo primero ---- */

static const s16 ima_step[89] = {7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,
    1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,
    9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const signed char ima_idx[8] = {-1,-1,-1,-1,2,4,6,8};

static int pcm_on;
static const u8 *pcm_d;
static u32 pcm_n, pcm_i, pcm_rate, pcm_frac, pcm_step;
static int pcm_pred, pcm_si, pcm_cur, pcm_next;
static u64 pcm_out;                             /* cuadros de 48 kHz ya mezclados */

static int pcm_decode(void)
{
    u32 k = pcm_i++;
    int code, step, d;
    if (k >= pcm_n)
        return pcm_pred;
    code = (pcm_d[k >> 1] >> ((k & 1) * 4)) & 15;
    step = ima_step[pcm_si];
    d = step >> 3;
    if (code & 4) d += step;
    if (code & 2) d += step >> 1;
    if (code & 1) d += step >> 2;
    pcm_pred += (code & 8) ? -d : d;
    if (pcm_pred > 32767) pcm_pred = 32767;
    if (pcm_pred < -32768) pcm_pred = -32768;
    pcm_si += ima_idx[code & 7];
    if (pcm_si < 0) pcm_si = 0;
    if (pcm_si > 88) pcm_si = 88;
    return pcm_pred;
}

static void pcm_mix(int *l, int *r)
{
    int s = pcm_cur + (int)(((long long)(pcm_next - pcm_cur) * (int)pcm_frac) >> 16);
    *l += s;
    *r += s;
    pcm_out++;
    pcm_frac += pcm_step;
    while (pcm_frac >= 65536) {
        pcm_frac -= 65536;
        pcm_cur = pcm_next;
        pcm_next = pcm_decode();
    }
    if (pcm_i > pcm_n + 2)
        pcm_on = 0;
}

int audio_pcm_play(const void *adpcm, u32 samples, u32 rate)
{
    if (!audio_open())
        return 0;
    pcm_d = (const u8 *)adpcm;
    pcm_n = samples;
    pcm_rate = rate;
    pcm_step = (u32)(((u64)rate << 16) / 48000);
    pcm_i = 0; pcm_pred = 0; pcm_si = 0; pcm_frac = 0; pcm_out = 0;
    pcm_cur = pcm_decode();
    pcm_next = pcm_decode();
    pcm_on = 1;
    audio_service();
    return 1;
}

void audio_pcm_stop(void) { pcm_on = 0; }
int  audio_pcm_active(void) { return pcm_on; }

/* ---- anillo ---- */

static int be;                                  /* 0 sin probar, 1 HDA, 2 AC'97, -1 nada */
static int running;
static u32 wr;                                  /* donde se escribe (bytes) */
static int silent_chunks;
static int in_service;
static u64 next_check;

int audio_open(void)
{
    if (be == 0) {
        if (hda_open())
            be = 1;
        else if (ac97_open())
            be = 2;
        else
            be = -1;
    }
    return be > 0;
}

const char *audio_info(void)
{
    if (!audio_open())
        return "AUDIO: PC SPEAKER (SIN HDA NI AC'97)";
    return be == 1 ? hda_info() : ac97_info();
}

/* ---- flujo PCM estereo crudo (DOOM): 16 bits a 11025 Hz, lo empuja la aplicacion ---- */
#define ST_FRAMES 8192u                         /* capacidad de la cola (potencia de 2) */
#define ST_RATE   11025u
static s16 st_buf[ST_FRAMES * 2];
static u32 st_head, st_tail;                    /* contadores libres, en cuadros */
static int st_on;
static u32 st_rate = ST_RATE;                   /* cuadros por segundo del flujo (op 4 lo cambia) */
static u32 st_frac;
static int st_l0, st_r0, st_l1, st_r1;

static void st_pop(int *l, int *r)
{
    if (st_head != st_tail) {
        u32 i = (st_tail++ & (ST_FRAMES - 1)) * 2;
        *l = st_buf[i];
        *r = st_buf[i + 1];
    } else {
        *l = *r = 0;                            /* cola vacia: silencio */
    }
}

static void st_mix(int *l, int *r)
{
    /* los efectos de Doom salen bajos (~-26 dB): ganancia x6; mix_chunk() recorta lo que pase de 16 bits */
    *l += 6 * (st_l0 + (int)(((long long)(st_l1 - st_l0) * (int)st_frac) >> 16));
    *r += 6 * (st_r0 + (int)(((long long)(st_r1 - st_r0) * (int)st_frac) >> 16));
    st_frac += (u32)(((u64)st_rate << 16) / RATE);
    while (st_frac >= 65536) {
        st_frac -= 65536;
        st_l0 = st_l1;
        st_r0 = st_r1;
        st_pop(&st_l1, &st_r1);
    }
}

int audio_stream_ctl(int op, const void *data, u32 n)
{
    switch (op) {
    case 0:
        if (!audio_open())
            return 0;
        st_head = st_tail = 0;
        st_rate = ST_RATE;
        st_frac = 0;
        st_l0 = st_r0 = st_l1 = st_r1 = 0;
        st_on = 1;
        audio_service();                        /* arranca el flujo si estaba parado */
        return 1;
    case 1: {
        const s16 *p = (const s16 *)data;
        u32 room = ST_FRAMES - (st_head - st_tail), i;
        if (!st_on)
            return 0;
        if (n > room)
            n = room;
        for (i = 0; i < n; i++, st_head++) {
            u32 k = (st_head & (ST_FRAMES - 1)) * 2;
            st_buf[k] = p[i * 2];
            st_buf[k + 1] = p[i * 2 + 1];
        }
        return (int)n;
    }
    case 2:
        st_on = 0;
        st_head = st_tail = 0;
        return 0;
    case 3:
        audio_service();                        /* "sigo vivo": que el anillo no se quede sin relleno (silencio, no bucle) */
        return (int)(st_head - st_tail);
    case 4:                                     /* tasa de muestreo del flujo: 4000..48000 Hz */
        if (n >= 4000 && n <= 48000)
            st_rate = n;
        return (int)st_rate;
    }
    return 0;
}

static int any_voice(void)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        if (vox[i].on)
            return 1;
    return 0;
}

int audio_busy(void)
{
    return any_voice() || cd_on || pcm_on || st_on;
}

static void mix_chunk(s16 *out, int frames)
{
    int f, i;
    for (f = 0; f < frames; f++) {
        int l = 0, r = 0;
        for (i = 0; i < NVOICE; i++)
            if (vox[i].on) {
                int s = voice_frame(&vox[i]);
                l += s;
                r += s;
            }
        if (cd_on)
            cd_mix(&l, &r);
        if (pcm_on)
            pcm_mix(&l, &r);
        if (st_on)
            st_mix(&l, &r);
        if (l > 32767) l = 32767;
        if (l < -32767) l = -32767;
        if (r > 32767) r = 32767;
        if (r < -32767) r = -32767;
        out[f * 2] = (s16)l;
        out[f * 2 + 1] = (s16)r;
    }
}

static u32 be_pos(void) { return be == 1 ? hda_pos() : ac97_pos(); }

static void ring_start(void)
{
    u32 off;
    memset(ring, 0, sizeof ring);
    wr = 0;
    for (off = 0; off < 8192; off += CHUNK) {   /* primeros 8 KB listos antes de arrancar */
        mix_chunk(ring + wr / 2, (int)(CHUNK / 4));
        wr += CHUNK;
    }
    if (be == 1)
        hda_start(ring, RING_BYTES);
    else
        ac97_start(ring, RING_BYTES);
    running = 1;
    silent_chunks = 0;
}

void audio_service(void)
{
    u32 pos, ahead;
    int idle;

    if (in_service || be < 0)
        return;
    if (!running) {
        if (!audio_busy() || !audio_open())
            return;
        in_service = 1;
        ring_start();
        in_service = 0;
        return;
    }
    if (g_tsc_hz) {                             /* no mas de ~2000 consultas por segundo */
        u64 t = rdtsc();
        if (t < next_check)
            return;
        next_check = t + g_tsc_hz / 2000;
    }
    in_service = 1;
    pos = be_pos();
    ahead = (wr - pos) & (RING_BYTES - 1);
    if (ahead > RING_BYTES - 8192) {            /* nos alcanzo la reproduccion: perder un trozo */
        wr = (pos + 4096) & (RING_BYTES - 1);
        ahead = 4096;
    }
    while (ahead < (st_on ? AHEAD_STREAM : AHEAD)) {
        idle = !audio_busy();
        mix_chunk(ring + wr / 2, (int)(CHUNK / 4));
        wr = (wr + CHUNK) & (RING_BYTES - 1);
        ahead += CHUNK;
        silent_chunks = idle ? silent_chunks + 1 : 0;
    }
    if (silent_chunks > (int)(RING_BYTES / CHUNK) + 2) {    /* todo el anillo en silencio: parar */
        if (be == 1)
            hda_stop();
        else
            ac97_stop();
        running = 0;
    }
    in_service = 0;
}

int audio_sound(int kind, int hz, int arg, int ms)
{
    if (!audio_open())
        return 0;
    if (kind == SND_OFF) {
        audio_stop();
        return 1;
    }
    voice_start(kind, hz, arg, ms);
    audio_service();                            /* arranca el flujo si estaba parado */
    return 1;
}

void audio_stop(void)
{
    int i;
    for (i = 0; i < NVOICE; i++)
        vox[i].on = 0;
    cd_on = 0;
    pcm_on = 0;
    st_on = 0;
}

/* milisegundos de PCM que ya sonaron (descuenta lo que sigue en el anillo) */
unsigned int audio_pcm_ms(void)
{
    u64 fr = pcm_out;
    if (running && fr) {
        u64 lag = ((wr - be_pos()) & (RING_BYTES - 1)) / 4;
        fr = fr > lag ? fr - lag : 0;
    }
    return (unsigned int)(fr * 1000 / 48000);
}
