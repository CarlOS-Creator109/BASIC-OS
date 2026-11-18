/* bvcplay.c - reproductor y depurador de .BVC para Linux (SDL2), para no tener que reiniciar la maquina
 *
 *   cc -O2 -o tools/bvcplay tools/bvcplay.c $(pkg-config --cflags --libs sdl2)      (Arch: sudo pacman -S sdl2)
 *
 *   bvcplay video.bvc                  reproduce (ventana + audio)
 *   bvcplay video.bvc --budget 3000    simula una maquina lenta: solo se pintan 3000 pixeles por cuadro, del centro hacia afuera
 *   bvcplay video.bvc --info [-v]      cabecera, estadisticas y verificacion del archivo (con -v, cada cuadro); no abre ventana
 *   bvcplay video.bvc --dump N f.ppm   guarda el cuadro N (ya con los cambios acumulados) como PPM
 *   opciones:  -s N (escala de la ventana)   --noreveal (sin la revelacion lenta del primer cuadro)   --mute
 *
 * Teclas:  espacio = pausa   <- / -> = cuadro -1 / +1   abajo / arriba = -5 s / +5 s   b = alternar presupuesto lento
 *          r = repetir el primer cuadro revelandose del centro   ESC o q = salir
 * El formato esta descrito en sys/play.c; aqui se decodifica igual que alli (mismo orden centro-fuera).
 * GPL-3.0-or-later.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;

static u8 *file;
static size_t fsize;
static u32 W, H, N, NF, FPS1000, ARATE, MODE;
static u32 *ordr;            /* (y << 16) | x */
static u8 *cur, *shown;
static u32 *pix;             /* ARGB de la imagen ya pintada */
static u32 pal[256];
static size_t *foff;         /* desplazamiento de cada cuadro (cabecera de 8 bytes incluida) */
static u32 cursor;
static long budget_val = 3000;
static int budget_on;

static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u32 rd16(const u8 *p) { return p[0] | (p[1] << 8); }

static int cmp_key(const void *a, const void *b)
{
    u32 pa = *(const u32 *)a, pb = *(const u32 *)b;
    int ya = pa >> 16, xa = pa & 0xFFFF, yb = pb >> 16, xb = pb & 0xFFFF;
    long ka = (long)(2 * xa - (int)(W - 1)) * (2 * xa - (int)(W - 1)) + (long)(2 * ya - (int)(H - 1)) * (2 * ya - (int)(H - 1));
    long kb = (long)(2 * xb - (int)(W - 1)) * (2 * xb - (int)(W - 1)) + (long)(2 * yb - (int)(H - 1)) * (2 * yb - (int)(H - 1));
    if (ka != kb)
        return ka < kb ? -1 : 1;
    if (ya != yb)
        return ya < yb ? -1 : 1;
    return xa < xb ? -1 : (xa > xb);
}

/* devuelve cuantos pixeles nuevos trae el cuadro (-1 si esta mal formado). Si cur es NULL solo verifica */
static long decode_frame(u32 f, u8 *dst, long *skips, int *bad)
{
    const u8 *p = file + foff[f] + 8;
    u32 vb = rd32(file + foff[f]), k = 0, pos = 0;
    long lit = 0, sk = 0;
    *bad = 0;
    while (k < vb && pos < N) {
        u8 t = p[k++];
        if (t & 0x80) {
            pos += (t & 0x7F) + 1;
            sk += (t & 0x7F) + 1;
        } else {
            u32 c = (u32)t + 1;
            if (k + c > vb || pos + c > N) {
                *bad = 1;
                return -1;
            }
            if (dst)
                memcpy(dst + pos, p + k, c);
            pos += c;
            k += c;
            lit += c;
        }
    }
    if (pos != N)
        *bad = 2;                   /* el cuadro no cubre todos los pixeles */
    if (skips)
        *skips = sk;
    return lit;
}

static void make_pal(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 r, g, b;
        if (MODE == 1) {
            r = g = b = i;
        } else {
            r = (i >> 5) * 255 / 7;
            g = ((i >> 2) & 7) * 255 / 7;
            b = (i & 3) * 255 / 3;
        }
        pal[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

static int load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    size_t off, f = 0;
    if (!fp) { perror(path); return 0; }
    fseek(fp, 0, SEEK_END);
    fsize = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    file = malloc(fsize + 8);
    if (fread(file, 1, fsize, fp) != fsize) { fprintf(stderr, "lectura incompleta\n"); return 0; }
    fclose(fp);
    if (fsize < 32 || memcmp(file, "BVC1", 4)) { fprintf(stderr, "%s: no es un archivo BVC1\n", path); return 0; }
    W = rd16(file + 4);
    H = rd16(file + 6);
    FPS1000 = rd32(file + 8);
    NF = rd32(file + 12);
    ARATE = rd32(file + 16);
    MODE = file[20];
    if (!W || !H || W > 640 || H > 480 || !FPS1000) { fprintf(stderr, "cabecera invalida\n"); return 0; }
    N = W * H;
    foff = malloc((NF + 1) * sizeof *foff);
    for (off = 32; f < NF; f++) {
        if (off + 8 > fsize) { fprintf(stderr, "AVISO: el archivo se corta en el cuadro %zu de %u\n", f, NF); NF = (u32)f; break; }
        foff[f] = off;
        off += 8 + rd32(file + off) + rd32(file + off + 4);
        if (off > fsize) { fprintf(stderr, "AVISO: el cuadro %zu se sale del archivo\n", f); NF = (u32)f; break; }
    }
    ordr = malloc(N * 4);
    for (u32 i = 0; i < N; i++)
        ordr[i] = ((i / W) << 16) | (i % W);
    qsort(ordr, N, 4, cmp_key);
    cur = calloc(N, 1);
    shown = calloc(N, 1);
    pix = calloc(N, 4);
    make_pal();
    for (u32 i = 0; i < N; i++)
        pix[i] = 0xFF000000u;
    return 1;
}

static void reset_state(void)
{
    memset(cur, 0, N);
    memset(shown, 0, N);
    for (u32 i = 0; i < N; i++)
        pix[i] = 0xFF000000u;
    cursor = N;
}

/* pinta hasta max pixeles (revisados) desde el cursor */
static void draw_some(long max)
{
    while (cursor < N && max-- > 0) {
        u32 i = cursor++;
        if (cur[i] != shown[i]) {
            u32 o = ordr[i];
            shown[i] = cur[i];
            pix[(o >> 16) * W + (o & 0xFFFF)] = pal[cur[i]];
        }
    }
}

/* avanza al cuadro f (decodifica; no pinta) */
static int apply(u32 f)
{
    long sk;
    int bad;
    decode_frame(f, cur, &sk, &bad);
    cursor = 0;
    return bad;
}

static void do_info(int verbose)
{
    double secs = NF * 1000.0 / FPS1000;
    unsigned long long vtot = 0, atot = 0;
    u32 maxv = 0, maxf = 0, badn = 0;
    long lit_tot = 0;
    printf("archivo   : %zu bytes\n", fsize);
    printf("video     : %ux%u, %s, %.3f fps, %u cuadros (%.1f s)\n", W, H, MODE == 1 ? "gris de 8 bits" : "color RGB332", FPS1000 / 1000.0, NF, secs);
    printf("audio     : %u Hz PCM 8 bits mono\n", ARATE);
    for (u32 f = 0; f < NF; f++) {
        u32 vb = rd32(file + foff[f]), ab = rd32(file + foff[f] + 4);
        long sk;
        int bad;
        long lit = decode_frame(f, NULL, &sk, &bad);
        vtot += vb;
        atot += ab;
        if (vb > maxv) { maxv = vb; maxf = f; }
        if (bad) badn++;
        if (lit > 0)
            lit_tot += lit;
        if (verbose || bad)
            printf("  cuadro %5u: video %6u B, audio %5u B, pixeles nuevos %7ld (%.1f%%), sin cambio %7ld%s\n",
                   f, vb, ab, lit, lit * 100.0 / N, sk, bad == 1 ? "  <-- MAL FORMADO" : bad == 2 ? "  <-- no cubre todos los pixeles" : "");
    }
    printf("video     : %llu bytes en total (%.0f B/cuadro de media, maximo %u B en el cuadro %u)\n", vtot, NF ? (double)vtot / NF : 0, maxv, maxf);
    printf("audio     : %llu muestras (%.1f s)\n", atot, ARATE ? (double)atot / ARATE : 0.0);
    printf("pixeles nuevos por cuadro: %.1f%% de media\n", NF ? lit_tot * 100.0 / ((double)N * NF) : 0);
    printf("%s\n", badn ? "RESULTADO: hay cuadros mal formados" : "RESULTADO: archivo correcto");
}

int main(int argc, char **argv)
{
    const char *path = 0, *dump_out = 0;
    long dump_n = -1;
    int info = 0, verbose = 0, scale = 0, reveal = 1, mute = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info")) info = 1;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--noreveal")) reveal = 0;
        else if (!strcmp(argv[i], "--mute")) mute = 1;
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) { budget_val = atol(argv[++i]); budget_on = 1; }
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) { dump_n = atol(argv[i + 1]); dump_out = argv[i + 2]; i += 2; }
        else if (argv[i][0] != '-') path = argv[i];
        else { fprintf(stderr, "opcion desconocida: %s\n", argv[i]); return 1; }
    }
    if (!path) {
        fprintf(stderr, "uso: %s video.bvc [--info [-v]] [--dump N f.ppm] [--budget P] [-s N] [--noreveal] [--mute]\n", argv[0]);
        return 1;
    }
    if (!load(path))
        return 1;
    if (info) {
        do_info(verbose);
        return 0;
    }
    if (dump_out) {
        FILE *o;
        if (dump_n < 0 || (u32)dump_n >= NF) { fprintf(stderr, "cuadro fuera de rango (0..%u)\n", NF - 1); return 1; }
        reset_state();
        for (long f = 0; f <= dump_n; f++) {
            apply((u32)f);
            draw_some(N);
        }
        o = fopen(dump_out, "wb");
        if (!o) { perror(dump_out); return 1; }
        fprintf(o, "P6\n%u %u\n255\n", W, H);
        for (u32 k = 0; k < N; k++) {
            u8 rgb[3] = { (u8)(pix[k] >> 16), (u8)(pix[k] >> 8), (u8)pix[k] };
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
        printf("cuadro %ld -> %s\n", dump_n, dump_out);
        return 0;
    }

    /* ---- reproduccion ---- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    if (!scale) {
        scale = 1600 / (int)W;
        if (900 / (int)H < scale)
            scale = 900 / (int)H;
        if (scale < 1)
            scale = 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_Window *win = SDL_CreateWindow("bvcplay", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       (int)W * scale, (int)H * scale, SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(ren, (int)W, (int)H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)W, (int)H);

    SDL_AudioDeviceID adev = 0;
    if (ARATE && !mute) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = (int)ARATE;
        want.format = AUDIO_U8;
        want.channels = 1;
        want.samples = 1024;
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!adev)
            fprintf(stderr, "sin audio: %s\n", SDL_GetError());
    }

    u32 f = 0;                          /* siguiente cuadro por mostrar */
    int paused = 0, quit = 0;
    Uint32 t0 = SDL_GetTicks();
    long long audio_next = 0;           /* siguiente cuadro cuyo audio hay que encolar */
    reset_state();

#define QUEUE_AUDIO_UPTO(n) do { \
        while (adev && audio_next < (long long)(n) && audio_next < (long long)NF) { \
            const u8 *h = file + foff[audio_next]; \
            SDL_QueueAudio(adev, h + 8 + rd32(h), rd32(h + 4)); \
            audio_next++; \
        } } while (0)

    {   /* primer cuadro: revelado desde el centro */
        apply(0);
        if (reveal) {
            Uint32 s = SDL_GetTicks();
            while (cursor < N && !quit) {
                SDL_Event e;
                Uint32 el = SDL_GetTicks() - s;
                u32 quota = el >= 1400 ? N : (u32)((unsigned long long)N * el / 1400u);
                while (SDL_PollEvent(&e))
                    if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q)))
                        quit = 1;
                draw_some((long)quota - (long)cursor);
                SDL_UpdateTexture(tex, NULL, pix, (int)W * 4);
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, tex, NULL, NULL);
                SDL_RenderPresent(ren);
                SDL_Delay(8);
            }
        } else {
            draw_some(N);
        }
        f = 1;
        QUEUE_AUDIO_UPTO(4);
        if (adev)
            SDL_PauseAudioDevice(adev, 0);
        t0 = SDL_GetTicks();
    }

    while (!quit) {
        SDL_Event e;
        long seek_to = -1;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                quit = 1;
            if (e.type != SDL_KEYDOWN)
                continue;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: case SDLK_q: quit = 1; break;
            case SDLK_SPACE:
                paused = !paused;
                if (adev) SDL_PauseAudioDevice(adev, paused);
                if (!paused) t0 = SDL_GetTicks() - (Uint32)((unsigned long long)f * 1000000ull / FPS1000);
                break;
            case SDLK_RIGHT: seek_to = (long)f; paused = 1; break;       /* f ya es el siguiente */
            case SDLK_LEFT:  seek_to = (long)f - 2; paused = 1; break;
            case SDLK_UP:    seek_to = (long)f + (long)(5 * FPS1000 / 1000); break;
            case SDLK_DOWN:  seek_to = (long)f - (long)(5 * FPS1000 / 1000); break;
            case SDLK_b:     budget_on = !budget_on; break;
            case SDLK_r:     seek_to = 0; break;
            default: break;
            }
        }
        if (seek_to != -1) {
            if (seek_to < 0) seek_to = 0;
            if (seek_to >= (long)NF) seek_to = (long)NF - 1;
            reset_state();
            for (long k = 0; k <= seek_to; k++) {
                apply((u32)k);
                draw_some(N);
            }
            f = (u32)seek_to + 1;
            if (adev) {
                SDL_ClearQueuedAudio(adev);
                audio_next = f;
                if (!paused)
                    QUEUE_AUDIO_UPTO(f + 3);
            }
            t0 = SDL_GetTicks() - (Uint32)((unsigned long long)f * 1000000ull / FPS1000);
            if (paused)
                cursor = N;
        }
        if (!paused && f < NF) {
            Uint32 due = (Uint32)((unsigned long long)f * 1000000ull / FPS1000);
            Uint32 now = SDL_GetTicks() - t0;
            if (now >= due) {
                apply(f);
                if (budget_on)
                    draw_some(budget_val);      /* maquina lenta simulada: lo que no se alcanza a pintar es el borde */
                else
                    draw_some(N);
                f++;
                QUEUE_AUDIO_UPTO(f + 3);
                if (now > due + 4 * (1000000u / FPS1000))
                    t0 = SDL_GetTicks() - due;  /* muy atrasados: no intentar recuperar */
            }
        }
        if (!paused && budget_on == 0 && cursor < N)
            draw_some(N);
        {
            char title[160];
            snprintf(title, sizeof title, "bvcplay - cuadro %u/%u  %s%s", f ? f - 1 : 0, NF, paused ? "[pausa] " : "", budget_on ? "[lento]" : "");
            SDL_SetWindowTitle(win, title);
        }
        SDL_UpdateTexture(tex, NULL, pix, (int)W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        if (f >= NF && !paused) {
            if (adev && SDL_GetQueuedAudioSize(adev) > 0)
                SDL_Delay(10);
            else
                quit = 1;
        } else {
            SDL_Delay(2);
        }
    }
    if (adev)
        SDL_CloseAudioDevice(adev);
    SDL_Quit();
    return 0;
}
