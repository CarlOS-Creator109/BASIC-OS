/* mkbvc.c - convierte un video cualquiera (lo que lea ffmpeg) al formato .BVC de PLAY.APP
 *
 *   cc -O2 -o tools/mkbvc tools/mkbvc.c
 *   tools/mkbvc entrada.mp4 salida.bvc [-w 240] [-h 180] [-f 20] [-r 11025] [-g] [-t 10]
 *
 *   -w -h  tamano del video (<= 640x480). Se conserva la proporcion con barras negras.
 *   -f     cuadros por segundo      -r  audio PCM 8 bits (4000..48000 Hz, 0 = sin audio)
 *   -g     gris de 8 bits en vez de color RGB332 (mucho mas liviano; ideal para Bad Apple)
 *   -t     umbral: un pixel solo se actualiza si cambia mas de T (0 = sin perdida, por defecto 10)
 *
 * Los pixeles de cada cuadro se guardan ORDENADOS DEL CENTRO HACIA AFUERA (ver sys/play.c, donde esta el formato).
 * Necesita ffmpeg en el PATH. GPL-3.0-or-later.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static unsigned W = 240, H = 180, ARATE = 11025, THR = 10;
static int GRAY;
static double FPS = 20;

static uint32_t *ord;           /* (y << 16) | x */
static int cmp_key(const void *a, const void *b)
{
    uint32_t pa = *(const uint32_t *)a, pb = *(const uint32_t *)b;
    int ya = pa >> 16, xa = pa & 0xFFFF, yb = pb >> 16, xb = pb & 0xFFFF;
    long ka = (long)(2 * xa - (int)(W - 1)) * (2 * xa - (int)(W - 1)) + (long)(2 * ya - (int)(H - 1)) * (2 * ya - (int)(H - 1));
    long kb = (long)(2 * xb - (int)(W - 1)) * (2 * xb - (int)(W - 1)) + (long)(2 * yb - (int)(H - 1)) * (2 * yb - (int)(H - 1));
    if (ka != kb)
        return ka < kb ? -1 : 1;
    if (ya != yb)
        return ya < yb ? -1 : 1;
    return xa < xb ? -1 : (xa > xb);
}

static void quote(char *dst, size_t n, const char *s)
{
    size_t k = 0;
    dst[k++] = '\'';
    for (; *s && k + 6 < n; s++) {
        if (*s == '\'') {
            memcpy(dst + k, "'\\''", 4);
            k += 4;
        } else {
            dst[k++] = *s;
        }
    }
    dst[k++] = '\'';
    dst[k] = 0;
}

static void le16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void le32(FILE *f, uint32_t v) { le16(f, v & 0xFFFF); le16(f, v >> 16); }

static void expand(uint8_t v, int *r, int *g, int *b)
{
    if (GRAY) {
        *r = *g = *b = v;
    } else {
        *r = (v >> 5) * 255 / 7;
        *g = ((v >> 2) & 7) * 255 / 7;
        *b = (v & 3) * 255 / 3;
    }
}

static int dist(uint8_t a, uint8_t b)
{
    int r1, g1, b1, r2, g2, b2;
    expand(a, &r1, &g1, &b1);
    expand(b, &r2, &g2, &b2);
    return abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2);
}

int main(int argc, char **argv)
{
    char in[1024], cmd[4096], filt[256];
    const char *src, *dst;
    FILE *pv, *pa, *out;
    uint8_t *raw, *aud = 0, *state, *ob;
    size_t alen = 0, acap = 0, N, i;
    unsigned frames = 0, fps1000, k;
    uint64_t vtotal = 0;
    long afed = 0;

    if (argc < 3) {
        fprintf(stderr, "uso: %s entrada salida.bvc [-w 240] [-h 180] [-f 20] [-r 11025] [-g] [-t 10]\n", argv[0]);
        return 1;
    }
    src = argv[1];
    dst = argv[2];
    for (k = 3; (int)k < argc; k++) {
        if (!strcmp(argv[k], "-g")) GRAY = 1;
        else if ((int)k + 1 < argc && !strcmp(argv[k], "-w")) W = (unsigned)atoi(argv[++k]);
        else if ((int)k + 1 < argc && !strcmp(argv[k], "-h")) H = (unsigned)atoi(argv[++k]);
        else if ((int)k + 1 < argc && !strcmp(argv[k], "-f")) FPS = atof(argv[++k]);
        else if ((int)k + 1 < argc && !strcmp(argv[k], "-r")) ARATE = (unsigned)atoi(argv[++k]);
        else if ((int)k + 1 < argc && !strcmp(argv[k], "-t")) THR = (unsigned)atoi(argv[++k]);
        else { fprintf(stderr, "opcion desconocida: %s\n", argv[k]); return 1; }
    }
    if (!W || !H || W > 640 || H > 480 || FPS <= 0 || (ARATE && (ARATE < 4000 || ARATE > 48000))) {
        fprintf(stderr, "parametros fuera de rango (maximo 640x480, audio 4000..48000)\n");
        return 1;
    }
    fps1000 = (unsigned)(FPS * 1000.0 + 0.5);
    N = (size_t)W * H;
    quote(in, sizeof in, src);

    /* orden centro-fuera */
    ord = malloc(N * 4);
    for (i = 0; i < N; i++)
        ord[i] = (uint32_t)(((i / W) << 16) | (i % W));
    qsort(ord, N, 4, cmp_key);

    /* audio entero a memoria */
    if (ARATE) {
        uint8_t buf[65536];
        size_t n;
        snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -vn -ac 1 -ar %u -f u8 - 2>/dev/null", in, ARATE);
        pa = popen(cmd, "r");
        if (!pa) { perror("ffmpeg"); return 1; }
        while ((n = fread(buf, 1, sizeof buf, pa)) > 0) {
            if (alen + n > acap) {
                acap = (alen + n) * 2;
                aud = realloc(aud, acap);
            }
            memcpy(aud + alen, buf, n);
            alen += n;
        }
        pclose(pa);
    }

    snprintf(filt, sizeof filt,
             "fps=%.6f,scale=%u:%u:force_original_aspect_ratio=decrease,pad=%u:%u:(ow-iw)/2:(oh-ih)/2:black",
             FPS, W, H, W, H);
    snprintf(cmd, sizeof cmd, "ffmpeg -v error -i %s -an -vf \"%s\" -f rawvideo -pix_fmt rgb24 - 2>/dev/null", in, filt);
    pv = popen(cmd, "r");
    if (!pv) { perror("ffmpeg"); return 1; }
    out = fopen(dst, "wb");
    if (!out) { perror(dst); return 1; }

    fwrite("BVC1", 1, 4, out);
    le16(out, W);
    le16(out, H);
    le32(out, fps1000);
    le32(out, 0);                       /* cuadros: se rellena al final */
    le32(out, alen ? ARATE : 0);
    fputc(GRAY ? 1 : 0, out);
    for (k = 21; k < 32; k++)
        fputc(0, out);

    raw = malloc(N * 3);
    state = calloc(N, 1);
    ob = malloc(N * 2 + 16);
    while (fread(raw, 1, N * 3, pv) == N * 3) {
        size_t o = 0, lit_start = 0, nlit = 0, skip = 0;
        long a0 = (long)((double)frames * ARATE * 1000.0 / fps1000);
        long a1 = (long)((double)(frames + 1) * ARATE * 1000.0 / fps1000);
        unsigned an = alen ? (unsigned)(a1 - a0) : 0;
        uint8_t lits[128];
        (void)lit_start;

        for (i = 0; i < N; i++) {
            uint32_t p = ord[i];
            size_t idx = (size_t)(p >> 16) * W + (p & 0xFFFF);
            const uint8_t *px = raw + idx * 3;
            uint8_t v;
            if (GRAY)
                v = (uint8_t)((px[0] * 299 + px[1] * 587 + px[2] * 114) / 1000);
            else
                v = (uint8_t)((px[0] & 0xE0) | ((px[1] >> 5) << 2) | (px[2] >> 6));
            if (frames > 0 && (unsigned)dist(v, state[i]) <= THR) {
                /* pixel que no cambia */
                if (nlit) {
                    ob[o++] = (uint8_t)(nlit - 1);
                    memcpy(ob + o, lits, nlit);
                    o += nlit;
                    nlit = 0;
                }
                if (++skip == 128) {
                    ob[o++] = 0x80 | 127;
                    skip = 0;
                }
            } else {
                if (skip) {
                    ob[o++] = (uint8_t)(0x80 | (skip - 1));
                    skip = 0;
                }
                state[i] = v;
                lits[nlit++] = v;
                if (nlit == 128) {
                    ob[o++] = 127;
                    memcpy(ob + o, lits, 128);
                    o += 128;
                    nlit = 0;
                }
            }
        }
        if (nlit) {
            ob[o++] = (uint8_t)(nlit - 1);
            memcpy(ob + o, lits, nlit);
            o += nlit;
        }
        if (skip)
            ob[o++] = (uint8_t)(0x80 | (skip - 1));

        le32(out, (uint32_t)o);
        le32(out, an);
        fwrite(ob, 1, o, out);
        for (k = 0; k < an; k++) {
            long pos = afed + (long)k;
            fputc(pos < (long)alen ? aud[pos] : 128, out);
        }
        afed += an;
        vtotal += o;
        frames++;
    }
    pclose(pv);
    fseek(out, 12, SEEK_SET);
    le32(out, frames);
    fclose(out);
    fprintf(stderr, "%s: %u cuadros de %ux%u a %.3f fps, %s, audio %u Hz (%zu muestras)\n"
            "  video %.2f MB, audio %.2f MB, total %.2f MB\n",
            dst, frames, W, H, FPS, GRAY ? "gris" : "color RGB332", alen ? ARATE : 0, alen,
            vtotal / 1048576.0, (double)afed / 1048576.0, (vtotal + (uint64_t)afed + 32 + 8ull * frames) / 1048576.0);
    free(raw); free(state); free(ob); free(ord); free(aud);
    return frames ? 0 : 1;
}
