/* mplay.c - MPLAY.BIN: reproductor de video y audio para BASIC-OS hecho con FFmpeg (paquete opcional de la tienda).
 *
 * FFmpeg se compila con musl y corre encima de la capa Linux de linux.c: aqui se usa como en cualquier Linux.
 * Formatos: MP4/MOV, MKV/WebM, AVI, OGG, FLV, MPEG-TS/PS, MP3, WAV, FLAC, AAC. Video: H.264, H.265, MPEG-4, MPEG-1/2,
 * VP8, VP9, Theora, MJPEG. Audio: AAC, MP3, MP2, Vorbis, Opus, FLAC, AC-3, PCM.
 *
 * Uso: PKG -> MPLAY (pregunta el archivo, p. ej. HD0.1:VIDEO.MP4), o EXEC con K->args = "MPLAY:archivo".
 * Teclas: Espacio = pausa, flechas izquierda/derecha = -10 s / +10 s, arriba/abajo = volumen, ESC = salir.
 * El archivo se carga entero en la RAM alta (hace falta RAM: unos 60 MB mas que el archivo; en QEMU usa -m 512).
 * Licencia: GPL-2.0-or-later (FFmpeg va como LGPL-2.1 o posterior).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include "kapi.h"
#include "mod_head.h"
#include "linux.h"

#define HEAP_LO   0xB400000ull            /* debajo: el programa (0x9000000), NET/PKG (0xB000000-0xB300000) */
#define NEED_RAM  (HEAP_LO + 48ull * 1024 * 1024)

struct bootinfo { uint32_t magic; uint16_t w, h, pitch; uint8_t bpp, gfx; uint32_t fb; uint8_t rs, rp, gs, gp, bs, bp;
                  uint16_t e820n; uint16_t extkb; } __attribute__((packed));
struct e820 { uint64_t base, len; uint32_t type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static uint64_t ram_end(void)
{
    uint64_t end = 0;
    int i;
    if (BI->e820n) {
        for (i = 0; i < BI->e820n; i++)
            if (E8[i].type == 1 && E8[i].base <= 0x4000000ull && E8[i].base + E8[i].len > 0x4000000ull)
                end = E8[i].base + E8[i].len;
    } else
        end = ((uint64_t)BI->extkb + 1024) * 1024;
    if (end > 0x40000000ull) end = 0x40000000ull;     /* el kernel mapea 1 GB */
    return end;
}

static void say(const char *s) { K->print(s); }

/* ---------- leer una linea en la consola ---------- */
static int readline(char *buf, int max)
{
    int len = 0;
    for (;;) {
        int c = K->poll();
        if (!c) { K->sleep(5); if (K->esc()) return -1; continue; }
        if (c == '\r' || c == '\n') { say("\n"); buf[len] = 0; return len; }
        if ((c == 8 || c == 127 || c == 20) && len) { len--; say("\b \b"); continue; }
        if (c >= 32 && c < 127 && len < max - 1) {
            char s[2] = { (char)c, 0 };
            buf[len++] = (char)c;
            say(s);
        }
    }
}

/* ---------- el archivo, entero en memoria, y un AVIOContext que lee de ahi ---------- */
static uint8_t *fdata;
static int64_t fsize, fpos;

static int io_read(void *o, uint8_t *b, int n)
{
    (void)o;
    if (fpos >= fsize) return AVERROR_EOF;
    if (n > fsize - fpos) n = (int)(fsize - fpos);
    memcpy(b, fdata + fpos, (size_t)n);
    fpos += n;
    return n;
}
static int64_t io_seek(void *o, int64_t off, int whence)
{
    (void)o;
    if (whence & AVSEEK_SIZE) return fsize;
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_SET) fpos = off;
    else if (whence == SEEK_CUR) fpos += off;
    else if (whence == SEEK_END) fpos = fsize + off;
    else return -1;
    if (fpos < 0) fpos = 0;
    if (fpos > fsize) fpos = fsize;
    return fpos;
}

static int load_file(const char *spec)
{
    char probe[512];
    unsigned total = 0;
    int r;
    r = K->read(spec, probe, (int)sizeof probe, &total);
    bos_after_kernel();
    if (r < 0 || total == 0) return -1;
    if ((uint64_t)total + 40ull * 1024 * 1024 > bos_free_ram()) return -2;
    fdata = malloc(total);
    if (!fdata) return -2;
    say("Cargando el archivo...\n");
    r = K->read(spec, (char *)fdata, (int)total, &total);
    bos_after_kernel();
    if (r < 0 || (unsigned)r != total) return -1;
    fsize = total;
    fpos = 0;
    return 0;
}

/* ---------- pantalla ---------- */
static uint8_t *fb, *saved;
static int sw, sh, pitch, bpp;
static enum AVPixelFormat fbfmt;

static void clear_screen(void)
{
    int y;
    for (y = 0; y < sh; y++) memset(fb + (size_t)y * pitch, 0, (size_t)sw * (bpp / 8));
}

/* ---------- audio ---------- */
static int snd, out_rate, volume = 6;
static double a_end;                         /* tiempo (s) del final de lo ultimo que se empujo */
static int64_t a_pushed;

static double audio_clock(void)
{
    return a_end - (double)K->stream(3, 0, 0) / out_rate;
}

static int quit, paused;
static unsigned pause_t;
static double t_offset;                       /* sin audio: el reloj es (ms - t0)/1000 + t_offset */
static unsigned t0;

static double clock_now(void)
{
    if (snd) return audio_clock();
    return (double)(K->ms() - t0) / 1000.0 + t_offset;
}

static int seek_req;                          /* -10 / +10 s */

static void handle_keys(void)
{
    int c;
    if (K->esc()) quit = 1;
    while ((c = K->poll()) != 0) {
        if (c == 27 || c == 'q' || c == 'Q') quit = 1;
        else if (c == ' ') {
            paused = !paused;
            if (paused) pause_t = K->ms();
            else if (!snd) t0 += K->ms() - pause_t;
        } else if (c == KEY_LEFT) seek_req = -10;
        else if (c == KEY_RIGHT) seek_req = 10;
        else if (c == KEY_UP && volume < 16) { volume++; if (snd) K->stream(5, 0, (unsigned)volume); }
        else if (c == KEY_DOWN && volume > 1) { volume--; if (snd) K->stream(5, 0, (unsigned)volume); }
    }
}

static void push_audio(const int16_t *s, int frames)
{
    while (frames > 0 && !quit) {
        int took = K->stream(1, s, (unsigned)frames);
        if (took <= 0) {
            handle_keys();
            K->sleep(2);
            continue;
        }
        s += took * 2;
        frames -= took;
    }
}

static void log_cb(void *p, int lvl, const char *fmt, va_list vl)
{
    char b[256];
    (void)p;
    if (lvl > av_log_get_level()) return;
    vsnprintf(b, sizeof b, fmt, vl);
    say(b);
}

static void put_num(const char *a, long v, const char *b)
{
    char t[96];
    snprintf(t, sizeof t, "%s%ld%s", a, v, b);
    say(t);
}

static int play(const char *spec)
{
    AVFormatContext *fmt = 0;
    AVIOContext *io = 0;
    AVCodecContext *vdec = 0, *adec = 0;
    struct SwsContext *sws = 0;
    SwrContext *swr = 0;
    AVPacket *pkt = 0;
    AVFrame *fr = 0;
    int vi = -1, ai = -1, r, dw = 0, dh = 0, dx = 0, dy = 0, sws_w = 0, sws_h = 0, sws_f = -1;
    int16_t *abuf = 0;
    int abuf_cap = 0;
    double skip_until = -1, vlast = 0;
    unsigned shown = 0, dropped = 0, tstart;
    size_t ssz;

    r = load_file(spec);
    if (r == -1) { say("MPLAY: no pude leer ese archivo.\n"); return 1; }
    if (r == -2) { say("MPLAY: no cabe en la RAM (en QEMU usa -m 512 o mas).\n"); return 1; }

    io = avio_alloc_context(av_malloc(65536), 65536, 0, 0, io_read, 0, io_seek);
    fmt = avformat_alloc_context();
    fmt->pb = io;
    if ((r = avformat_open_input(&fmt, spec, 0, 0)) < 0) { say("MPLAY: formato desconocido.\n"); goto out; }
    if (avformat_find_stream_info(fmt, 0) < 0) { say("MPLAY: no entiendo el contenido.\n"); goto out; }
    vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, 0, 0);
    if (vi < 0 && ai < 0) { say("MPLAY: no hay ni video ni audio que yo sepa leer.\n"); goto out; }

    if (vi >= 0) {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id);
        if (c && (vdec = avcodec_alloc_context3(c)) && avcodec_parameters_to_context(vdec, fmt->streams[vi]->codecpar) >= 0) {
            vdec->thread_count = 1;
            if (avcodec_open2(vdec, c, 0) < 0) avcodec_free_context(&vdec);
        }
        if (!vdec) { say("MPLAY: no tengo decodificador para ese video.\n"); vi = -1; }
    }
    if (ai >= 0) {
        const AVCodec *c = avcodec_find_decoder(fmt->streams[ai]->codecpar->codec_id);
        if (c && (adec = avcodec_alloc_context3(c)) && avcodec_parameters_to_context(adec, fmt->streams[ai]->codecpar) >= 0) {
            if (avcodec_open2(adec, c, 0) < 0) avcodec_free_context(&adec);
        }
        if (!adec) { say("MPLAY: no tengo decodificador para ese audio.\n"); ai = -1; }
    }
    if (vi < 0 && ai < 0) goto out;

    {   char t[200];
        snprintf(t, sizeof t, "%s: %s", spec, fmt->iformat->long_name ? fmt->iformat->long_name : fmt->iformat->name);
        say(t);
        if (vdec) { snprintf(t, sizeof t, ", video %s %dx%d", vdec->codec->name, vdec->width, vdec->height); say(t); }
        if (adec) { snprintf(t, sizeof t, ", audio %s %d Hz", adec->codec->name, adec->sample_rate); say(t); }
        if (fmt->duration > 0) put_num(", ", (long)(fmt->duration / AV_TIME_BASE), " s");
        say("\nEspacio = pausa, flechas = adelantar/atrasar 10 s y volumen, ESC = salir\n");
        K->sleep(1200); }

    if (adec) {
        AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
        out_rate = adec->sample_rate > 48000 ? 48000 : adec->sample_rate < 8000 ? 8000 : adec->sample_rate;
        if (swr_alloc_set_opts2(&swr, &st, AV_SAMPLE_FMT_S16, out_rate, &adec->ch_layout, adec->sample_fmt,
                                adec->sample_rate, 0, 0) < 0 || swr_init(swr) < 0) {
            say("MPLAY: no pude preparar el audio.\n");
            avcodec_free_context(&adec); ai = -1;
        } else {
            snd = K->stream(0, 0, 0);
            if (snd) { K->stream(4, 0, (unsigned)out_rate); K->stream(5, 0, (unsigned)volume); }
            else say("(sin tarjeta de sonido: solo video)\n");
        }
    }

    /* la pantalla: se guarda para devolverla igual al salir */
    fb = (uint8_t *)(uintptr_t)BI->fb;
    sw = BI->w; sh = BI->h; pitch = BI->pitch; bpp = BI->bpp;
    if (!BI->gfx || !fb) { if (vdec) say("MPLAY: no hay modo grafico; solo audio.\n"); vi = -1; }
    else {
        fbfmt = bpp == 32 ? (BI->rp == 16 ? AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0) :
                bpp == 24 ? (BI->rp == 16 ? AV_PIX_FMT_BGR24 : AV_PIX_FMT_RGB24) :
                bpp == 16 ? AV_PIX_FMT_RGB565LE : AV_PIX_FMT_NONE;
        if (fbfmt == AV_PIX_FMT_NONE) { say("MPLAY: no conozco ese modo de video; solo audio.\n"); vi = -1; }
        ssz = (size_t)pitch * sh;
        saved = malloc(ssz);
        if (saved) memcpy(saved, fb, ssz);
        K->cursor(0);
        clear_screen();
    }

    pkt = av_packet_alloc();
    fr = av_frame_alloc();
    tstart = t0 = K->ms();
    a_end = 0;
    av_log_set_level(AV_LOG_QUIET);

    while (!quit) {
        handle_keys();
        while (paused && !quit) { K->sleep(10); handle_keys(); }
        if (seek_req) {
            double now = clock_now(), target = now + seek_req;
            if (target < 0) target = 0;
            seek_req = 0;
            if (av_seek_frame(fmt, -1, (int64_t)(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) >= 0) {
                if (vdec) avcodec_flush_buffers(vdec);
                if (adec) avcodec_flush_buffers(adec);
                if (snd) { K->stream(2, 0, 0); K->stream(0, 0, 0); K->stream(4, 0, (unsigned)out_rate); K->stream(5, 0, (unsigned)volume); }
                skip_until = target;
                a_end = target;
                t_offset = target; t0 = K->ms();
            }
        }
        r = av_read_frame(fmt, pkt);
        if (r < 0) break;                                   /* se acabo */
        if (pkt->stream_index == ai && adec) {
            if (avcodec_send_packet(adec, pkt) >= 0)
                while (!quit && avcodec_receive_frame(adec, fr) >= 0) {
                    double pts = fr->pts != AV_NOPTS_VALUE ? fr->pts * av_q2d(fmt->streams[ai]->time_base) : a_end;
                    int max = swr_get_out_samples(swr, fr->nb_samples), n;
                    if (skip_until >= 0 && pts + (double)fr->nb_samples / fr->sample_rate < skip_until) continue;
                    if (max > abuf_cap) { abuf_cap = max + 1024; abuf = realloc(abuf, (size_t)abuf_cap * 4); }
                    n = swr_convert(swr, (uint8_t **)&abuf, abuf_cap, (const uint8_t **)fr->extended_data, fr->nb_samples);
                    if (n > 0 && snd) {
                        if (a_end == 0 || pts > a_end + 0.2 || pts < a_end - 0.2) a_end = pts;   /* primer cuadro o salto */
                        push_audio(abuf, n);
                        a_end += (double)n / out_rate;
                    }
                }
        } else if (pkt->stream_index == vi && vdec) {
            if (avcodec_send_packet(vdec, pkt) >= 0)
                while (!quit && avcodec_receive_frame(vdec, fr) >= 0) {
                    int64_t ts = fr->best_effort_timestamp;
                    double pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(fmt->streams[vi]->time_base) : vlast + 1.0 / 25;
                    double now;
                    vlast = pts;
                    if (skip_until >= 0) {
                        if (pts < skip_until) continue;
                        skip_until = -1;
                        if (!snd) { t_offset = pts; t0 = K->ms(); }
                    }
                    now = clock_now();
                    if (now > pts + 0.15) { dropped++; continue; }     /* vamos tarde: este no se pinta */
                    /* esperar a su hora. Con audio, si la cola del audio se vacio hay que seguir leyendo el archivo
                     * (en MKV el audio de este momento viene despues del cuadro): se pinta ya y el audio lo alcanza */
                    while (!quit && !seek_req && (now = clock_now()) < pts - 0.004 && (!snd || K->stream(3, 0, 0) > 0)) {
                        handle_keys();
                        while (paused && !quit) { K->sleep(10); handle_keys(); }
                        if (pts - now > 0.003) K->sleep(1);
                    }
                    if (quit || seek_req) break;
                    if (fr->width != sws_w || fr->height != sws_h || fr->format != sws_f) {
                        double ar = fr->sample_aspect_ratio.num ? av_q2d(fr->sample_aspect_ratio) : 1.0;
                        double fw = fr->width * ar, fh = fr->height;
                        sws_freeContext(sws);
                        dw = sw; dh = (int)(sw * fh / fw);
                        if (dh > sh) { dh = sh; dw = (int)(sh * fw / fh); }
                        dw &= ~1; dh &= ~1;
                        dx = (sw - dw) / 2; dy = (sh - dh) / 2;
                        sws = sws_getContext(fr->width, fr->height, fr->format, dw, dh, fbfmt, SWS_FAST_BILINEAR, 0, 0, 0);
                        sws_w = fr->width; sws_h = fr->height; sws_f = fr->format;
                        clear_screen();
                    }
                    if (sws) {
                        uint8_t *dst[4] = { fb + (size_t)dy * pitch + (size_t)dx * (bpp / 8), 0, 0, 0 };
                        int dls[4] = { pitch, 0, 0, 0 };
                        sws_scale(sws, (const uint8_t *const *)fr->data, fr->linesize, 0, fr->height, dst, dls);
                        shown++;
                    }
                }
        }
        av_packet_unref(pkt);
    }
    /* que termine de sonar lo que quedo en la cola */
    while (snd && !quit && K->stream(3, 0, 0) > 0) { handle_keys(); K->sleep(10); }

out:
    if (snd) { K->stream(2, 0, 0); snd = 0; }
    if (saved) { memcpy(fb, saved, (size_t)pitch * sh); free(saved); saved = 0; }
    if (fb) K->cursor(1);
    if (shown || dropped) {
        char t[160];
        unsigned el = K->ms() - tstart;
        snprintf(t, sizeof t, "MPLAY: %u cuadros en %u.%u s (%u saltados para ir a tiempo)\n", shown, el / 1000, (el / 100) % 10, dropped);
        say(t);
    }
    sws_freeContext(sws);
    swr_free(&swr);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&vdec);
    avcodec_free_context(&adec);
    if (fmt) avformat_close_input(&fmt);
    if (io) { av_freep(&io->buffer); avio_context_free(&io); }
    free(abuf);
    free(fdata); fdata = 0;
    return 0;
}

static int mplay_main(int argc, char **argv, char **envp)
{
    char spec[128];
    const char *a = K->args;
    (void)argc; (void)argv; (void)envp;
    av_log_set_callback(log_cb);
    K->esc();                                   /* un ESC viejo no debe cerrar la pregunta */
    av_log_set_level(AV_LOG_ERROR);
    say("\nMPLAY: reproductor de video para BASIC-OS (FFmpeg " );
    say(av_version_info());
    say(")\n");
    if (a && !strncmp(a, "MPLAY:", 6) && a[6]) {
        snprintf(spec, sizeof spec, "%s", a + 6);
        ((char *)K->args)[0] = 0;
    } else {
        say("Archivo (por ejemplo HD0.1:VIDEO.MP4; Enter = salir): ");
        if (readline(spec, sizeof spec) <= 0) return 0;
    }
    return play(spec);
}

int mod_main(const struct kapi *k)
{
    uint64_t end = ram_end();
    K = k;
    if (end < NEED_RAM) {
        K->print("\nMPLAY necesita al menos 240 MB de RAM (en QEMU: -m 512).\n");
        return 1;
    }
    bos_linux_run(k, HEAP_LO, end, mplay_main);
    return 0;
}
