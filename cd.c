/* cd.c - reproduccion de CD de audio (CD-DA) por la tarjeta de sonido
 *
 * Funciona con cualquier lectora que tenga soporte SCSI/MMC en blk (ATAPI IDE y USB):
 * READ TOC (0x43) para las pistas y READ CD (0xBE) para sacar los sectores crudos de 2352
 * bytes, que el motor de audio (audio.c) remuestrea de 44.1 a 48 kHz y manda al anillo DMA.
 */
#include "kernel.h"
#include "audio.h"
#include "cd.h"

static struct blk *drv;                         /* lectora con el disco actual */
static int ntracks, first_track;
static unsigned int tlba[100];                  /* [i] = inicio de la pista first_track + i; [ntracks] = lead-out */
static u8 tdata[100];                           /* 1 = pista de datos */
static int play_track, play_start;

static int read_cd(unsigned int lba, int n, void *buf)
{
    u8 cb[12];
    cb[0] = 0xBE;
    cb[1] = 0x04;                               /* sector tipo CD-DA */
    cb[2] = (u8)(lba >> 24); cb[3] = (u8)(lba >> 16); cb[4] = (u8)(lba >> 8); cb[5] = (u8)lba;
    cb[6] = 0; cb[7] = (u8)(n >> 8); cb[8] = (u8)n;
    cb[9] = 0x10;                               /* solo datos de usuario */
    cb[10] = 0; cb[11] = 0;
    return drv->scsi(drv, cb, 12, buf, n * 2352);
}

static int load_toc(struct blk *b)
{
    static u8 buf[4 + 8 * 100];
    u8 cb[10] = { 0x43, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int len, i, n;

    cb[7] = (u8)(sizeof buf >> 8);
    cb[8] = (u8)sizeof buf;
    memset(buf, 0, sizeof buf);
    if (b->scsi(b, cb, 10, buf, (int)sizeof buf))
        return CD_ENODISC;
    len = (buf[0] << 8) | buf[1];
    n = (len - 2) / 8;                          /* descriptores (incluye el lead-out 0xAA) */
    if (n < 2 || n > 100)
        return CD_ENODISC;
    first_track = buf[2];
    ntracks = n - 1;
    for (i = 0; i < n; i++) {
        const u8 *d = buf + 4 + i * 8;
        tlba[i] = ((u32)d[4] << 24) | ((u32)d[5] << 16) | ((u32)d[6] << 8) | d[7];
        tdata[i] = (d[1] & 4) ? 1 : 0;
    }
    drv = b;
    return 0;
}

/* Busca una lectora con disco (la actual primero) y lee su TOC */
static int find_disc(void)
{
    int i, r = CD_ENODRIVE;
    if (drv && !load_toc(drv))
        return 0;
    for (i = 0; i < blk_count(); i++) {
        struct blk *b = blk_get(i);
        if (b->scsi && b->name[0] == 'C') {
            r = load_toc(b);
            if (!r)
                return 0;
        }
    }
    return r;
}

int cd_toc(int i, char *out)
{
    char *o = out;
    unsigned int len, s;
    int t;

    if (i == 0) {
        int r = find_disc();
        if (r)
            return r;
    }
    if (i >= ntracks)
        return 0;
    t = first_track + i;
    len = (tlba[i + 1] - tlba[i]) / 75;         /* segundos */
    *o++ = 'T'; *o++ = 'R'; *o++ = 'A'; *o++ = 'C'; *o++ = 'K'; *o++ = ' ';
    if (t >= 10)
        *o++ = (char)('0' + t / 10);
    *o++ = (char)('0' + t % 10);
    *o++ = ' ';
    s = len;
    *o++ = (char)('0' + s / 600 % 10);
    *o++ = (char)('0' + s / 60 % 10);
    *o++ = ':';
    *o++ = (char)('0' + s % 60 / 10);
    *o++ = (char)('0' + s % 10);
    *o++ = ' ';
    {
        const char *k = tdata[i] ? "DATA" : "AUDIO";
        while (*k)
            *o++ = *k++;
    }
    *o = 0;
    return 1;
}

int cd_play(int track)
{
    int i, r;
    unsigned int end;

    r = find_disc();
    if (r)
        return r;
    if (track <= 0)
        track = first_track;
    i = track - first_track;
    if (i < 0 || i >= ntracks)
        return CD_ENOTRACK;
    if (tdata[i])
        return CD_EDATA;
    end = tlba[ntracks];
    {
        int j;
        for (j = i + 1; j < ntracks; j++)           /* parar antes de una pista de datos */
            if (tdata[j]) {
                end = tlba[j];
                break;
            }
    }
    if (!audio_open())
        return CD_ENOAUDIO;
    audio_stop();
    if (!audio_cd_play(read_cd, tlba[i], end))
        return CD_EREAD;
    play_track = track;
    play_start = i;
    audio_service();
    return 0;
}

void cd_stop(void)
{
    audio_cd_stop();
}

int cd_eject(void)
{
    u8 cb[6] = { 0x1B, 0, 0, 0, 0x02, 0 };      /* START STOP UNIT: expulsar */
    int r = find_disc();
    struct blk *b = drv;
    audio_cd_stop();
    if (r && !b)
        return r;
    if (!b || b->scsi(b, cb, 6, NULL, 0))
        return CD_ENODRIVE;
    drv = NULL;
    return 0;
}

/* Pista actual (0 = parado) y segundos transcurridos dentro de ella */
int cd_status(int *track, int *secs)
{
    unsigned int lba;
    int i, t = play_start;

    if (!audio_cd_active()) {
        *track = 0;
        *secs = 0;
        return 0;
    }
    lba = audio_cd_lba();                       /* lo ya leido (va ~0.2 s por delante) */
    for (i = play_start; i < ntracks; i++)
        if (lba >= tlba[i])
            t = i;
    *track = first_track + t;
    *secs = (int)((lba - tlba[t]) / 75);
    return 1;
}
