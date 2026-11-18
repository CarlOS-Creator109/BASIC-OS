/* play.c - PLAY.APP: reproductor de video .BVC ("BASIC Video Centro")
 *
 * La gracia del formato: el cuadro NO se dibuja por filas desde el borde, sino del CENTRO hacia afuera
 * (en circulos). Los pixeles van guardados en ese orden. Si la maquina va justa de tiempo, lo que se queda
 * sin actualizar es el borde y no el medio de la imagen, asi que la parte importante siempre esta al dia.
 * Audio: PCM de 8 bits sin signo, mono, intercalado cuadro por cuadro.
 *
 * Archivo (todo little endian):
 *   0   "BVC1"
 *   4   u16 ancho (<= 640)   6  u16 alto (<= 480)
 *   8   u32 fps * 1000       12 u32 numero de cuadros
 *   16  u32 muestras de audio por segundo (4000..48000, 0 = sin audio)
 *   20  u8  modo: 0 = color RGB332, 1 = gris de 8 bits
 *   21..31 reservado
 *   32  cuadros:  u32 bytes_video, u32 bytes_audio, video, audio
 * Video de un cuadro = lista de ordenes sobre los pixeles EN ORDEN CENTRO-FUERA (indice i = 0 en el centro):
 *   0x00..0x7F  n = valor+1 pixeles nuevos (siguen n bytes)
 *   0x80..0xFF  n = (valor&0x7F)+1 pixeles que no cambian
 * El orden: se ordenan los pixeles por dx^2+dy^2 (dx = 2x-(w-1), dy = 2y-(h-1)); a igual distancia, por fila y columna.
 *
 * Uso:  PLAY "VIDEO.BVC"   (o  EXEC "PLAY.APP"  para VIDEO.BVC).  Espacio = pausa, ESC = salir.
 * Memoria: copia de pantalla 0x6000000, tablas 0x8000000, el archivo entero desde 0x8800000.
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
typedef short s16;

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

static const struct kapi *K;

#define SAVE_BASE  0x6000000u
#define SAVE_MAX   0x2000000u
#define ORDER_ADDR 0x8000000u           /* u32 por pixel: (y << 16) | x */
#define CUR_ADDR   0x8200000u           /* lo que deberia verse */
#define SHOWN_ADDR 0x8300000u           /* lo que hay en pantalla */
#define COUNT_ADDR 0x8400000u
#define VBUF       0x8800000u
#define RAM_TOP    0x3C000000u          /* el kernel mapea el primer GB */
#define MAXW 640
#define MAXH 480

struct bootinfo {
    u32 magic;
    u16 w, h, pitch;
    u8  bpp, gfx;
    u32 fb;
    u8  rs, rp, gs, gp, bs, bp;
    u16 e820n;
    u16 extkb;
} __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static u64 ram_end(void)
{
    const struct bootinfo *bi = BI;
    int i;
    u64 end = 0;
    if (bi->e820n) {
        for (i = 0; i < bi->e820n; i++) {
            const struct e820 *e = &E8[i];
            if (e->type == 1 && e->base <= 0x4000000ull && e->base + e->len > 0x4000000ull)
                end = e->base + e->len;
        }
    } else {
        end = ((u64)bi->extkb + 1024) * 1024;
    }
    return end;
}

/* ---- pantalla ---- */
static volatile u8 *fbp;
static u32 pitch, bytespp, scw, sch;
static u32 pal[256];
static int scale, ox, oy;
static u32 save_size;
static int saved;

static void fill_px(int x, int y, int w, int h, u32 v)
{
    int i, j;
    for (j = 0; j < h; j++) {
        volatile u8 *p = fbp + (u32)(y + j) * pitch + (u32)x * bytespp;
        for (i = 0; i < w; i++, p += bytespp) {
            if (bytespp == 4)
                *(volatile u32 *)p = v;
            else if (bytespp == 2)
                *(volatile u16 *)p = (u16)v;
            else {
                p[0] = (u8)v;
                p[1] = (u8)(v >> 8);
                p[2] = (u8)(v >> 16);
            }
        }
    }
}

static u32 packrgb(u32 r, u32 g, u32 b)
{
    const struct bootinfo *bi = BI;
    u32 rs = bi->rs, rp = bi->rp, gs = bi->gs, gp = bi->gp, bs = bi->bs, bp = bi->bp;
    if (!rs && !gs && !bs) {
        if (bi->bpp == 16) { rs = 5; rp = 11; gs = 6; gp = 5; bs = 5; bp = 0; }
        else               { rs = 8; rp = 16; gs = 8; gp = 8; bs = 8; bp = 0; }
    }
    return ((r >> (8 - rs)) << rp) | ((g >> (8 - gs)) << gp) | ((b >> (8 - bs)) << bp);
}

static void make_palette(int mode)
{
    u32 i;
    for (i = 0; i < 256; i++) {
        if (mode == 1)
            pal[i] = packrgb(i, i, i);
        else
            pal[i] = packrgb((i >> 5) * 255 / 7, ((i >> 2) & 7) * 255 / 7, (i & 3) * 255 / 3);
    }
}

/* ---- el video ---- */
static u32 vw, vh, vn, nframes, arate, fps1000, vmode;
static u32 *order;
static u8 *cur, *shown;
static u32 cursor;                      /* siguiente pixel (en orden centro-fuera) por revisar en el cuadro actual */

static void build_order(void)
{
    u32 *cnt = (u32 *)(unsigned long)COUNT_ADDR;
    u32 maxk = (vw - 1) * (vw - 1) + (vh - 1) * (vh - 1), x, y, i, sum = 0;
    for (i = 0; i <= maxk + 1; i++)
        cnt[i] = 0;
    for (y = 0; y < vh; y++)
        for (x = 0; x < vw; x++) {
            int dx = 2 * (int)x - (int)(vw - 1), dy = 2 * (int)y - (int)(vh - 1);
            cnt[(u32)(dx * dx + dy * dy)]++;
        }
    for (i = 0; i <= maxk; i++) {
        u32 c = cnt[i];
        cnt[i] = sum;
        sum += c;
    }
    for (y = 0; y < vh; y++)
        for (x = 0; x < vw; x++) {
            int dx = 2 * (int)x - (int)(vw - 1), dy = 2 * (int)y - (int)(vh - 1);
            order[cnt[(u32)(dx * dx + dy * dy)]++] = (y << 16) | x;
        }
}

/* pinta hasta max pixeles distintos, avanzando desde cursor; devuelve cuantos reviso */
static int draw_some(int budget)
{
    int done = 0;
    while (cursor < vn && done < budget) {
        u32 i = cursor++;
        done++;
        if (cur[i] != shown[i]) {
            u32 o = order[i];
            shown[i] = cur[i];
            fill_px(ox + (int)(o & 0xFFFF) * scale, oy + (int)(o >> 16) * scale, scale, scale, pal[cur[i]]);
        }
    }
    return done;
}

static int decode(const u8 *p, u32 len)
{
    u32 pos = 0, k = 0;
    while (k < len && pos < vn) {
        u8 t = p[k++];
        if (t & 0x80) {
            pos += (u32)(t & 0x7F) + 1;
        } else {
            u32 c = (u32)t + 1;
            if (k + c > len || pos + c > vn)
                return 0;
            memcpy(cur + pos, p + k, c);
            pos += c;
            k += c;
        }
    }
    return 1;
}

/* ---- audio ---- */
static int snd;

static u32 queued(void) { return (u32)K->stream(3, 0, 0); }

static void push_audio(const u8 *a, u32 n)
{
    s16 tmp[512 * 2];
    while (n) {
        u32 c = n > 512 ? 512 : n, i;
        int took;
        for (i = 0; i < c; i++) {
            s16 v = (s16)(((int)a[i] - 128) * 40);      /* el mezclador de la kapi multiplica x6 */
            tmp[i * 2] = v;
            tmp[i * 2 + 1] = v;
        }
        took = K->stream(1, tmp, c);
        if (took <= 0) {
            K->poll();
            K->sleep(1);
            continue;
        }
        a += took;
        n -= (u32)took;
    }
}

/* ---- teclas: 1 = salir ---- */
static int paused;
static unsigned pause_t0;
static unsigned t_start, t_load;

static int keys(void)
{
    int k;
    while ((k = K->poll()) != 0) {
        if (k == 27)
            return 1;
        if (k == ' ') {
            paused = !paused;
            if (paused) {
                pause_t0 = K->ms();      /* con sonido la cola se vacia sola y queda en silencio */
            } else {
                t_start += K->ms() - pause_t0;
            }
        }
    }
    return 0;
}

/* el USB nativo (EHCI) va primero: leer una memoria USB por la BIOS (INT 13h) iba a ~160 KB/s, por EHCI a varios MB/s */
static const char *const where[] = { "USB0.1:", "USB1.1:", "USB0:", "HD0.1:", "HD1.1:", "HD2.1:", "HD0:", "HD1:", "FD1:", "FD0:", "CD0:", 0 };

static int load_file(const char *name, u32 *size)
{
    char spec[100];
    u32 total = 0;
    int i, n, j, colon = 0;
    u64 end = ram_end();
    u32 room;
    if (end > RAM_TOP)
        end = RAM_TOP;
    if (end <= VBUF + 0x100000u)
        return -2;
    room = (u32)(end - VBUF);
    for (j = 0; name[j]; j++)
        if (name[j] == ':')
            colon = 1;
    for (i = 0; where[i] || (colon && i == 0); i++) {
        int p = 0;
        if (!colon)
            for (j = 0; where[i][j]; j++)
                spec[p++] = where[i][j];
        for (j = 0; name[j] && p < 98; j++)
            spec[p++] = name[j];
        spec[p] = 0;
        total = 0;
        n = K->read(spec, (char *)(unsigned long)VBUF, (int)room, &total);
        if (n > 0 && (u32)n == total) {
            *size = (u32)n;
            return 0;
        }
        if (n > 0 && (u32)n < total)
            return -3;                  /* no cabe en RAM */
        if (colon)
            break;
    }
    return -1;
}

int mod_main(const struct kapi *api)
{
    const u8 *base = (const u8 *)(unsigned long)VBUF;
    const char *name;
    u32 size = 0, off, f, gate, due_ms;
    int rc, ok = 1;
    const struct bootinfo *bi = BI;

    K = api;
    if (!bi->gfx || !bi->fb || (bi->bpp != 16 && bi->bpp != 24 && bi->bpp != 32)) {
        K->print("PLAY NECESITA UN MODO GRAFICO (VESA)\n");
        return 1;
    }
    name = (K->args && K->args[0]) ? K->args : "VIDEO.BVC";
    K->print("\nPLAY: CARGANDO ");
    K->print(name);
    K->print(" ...\n");
    t_load = K->ms();
    rc = load_file(name, &size);
    t_load = K->ms() - t_load;
    if (rc == -1) {
        K->print("PLAY: NO ENCUENTRO EL ARCHIVO EN NINGUN DISCO (HD0.1: ...)\n");
        return 1;
    }
    if (rc == -2 || rc == -3) {
        K->print("PLAY: NO HAY RAM SUFICIENTE PARA ESTE VIDEO (en QEMU usa -m 512 o mas)\n");
        return 1;
    }
    {   /* cuanto tardo la lectura del disco: sirve para saber si el cuello es el USB */
        char msg[80];
        u32 kb = size / 1024, ms = t_load ? t_load : 1, i = 0, v;
        const char *a = "PLAY: LEIDOS ";
        while (*a) msg[i++] = *a++;
        v = kb; { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) msg[i++] = t[--n]; }
        a = " KB EN "; while (*a) msg[i++] = *a++;
        v = ms; { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) msg[i++] = t[--n]; }
        a = " MS ("; while (*a) msg[i++] = *a++;
        v = (u32)((u64)kb * 1000u / ms); { char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) msg[i++] = t[--n]; }
        a = " KB/S)\n"; while (*a) msg[i++] = *a++;
        msg[i] = 0;
        K->print(msg);
    }
    if (size < 32 || base[0] != 'B' || base[1] != 'V' || base[2] != 'C' || base[3] != '1') {
        K->print("PLAY: NO ES UN ARCHIVO BVC1 (usa tools/mkbvc)\n");
        return 1;
    }
    vw = *(const u16 *)(base + 4);
    vh = *(const u16 *)(base + 6);
    fps1000 = *(const u32 *)(base + 8);
    nframes = *(const u32 *)(base + 12);
    arate = *(const u32 *)(base + 16);
    vmode = base[20];
    if (!vw || !vh || vw > MAXW || vh > MAXH || !fps1000 || (arate && (arate < 4000 || arate > 48000))) {
        K->print("PLAY: CABECERA INVALIDA\n");
        return 1;
    }
    vn = vw * vh;

    fbp = (volatile u8 *)(unsigned long)bi->fb;
    pitch = bi->pitch;
    bytespp = bi->bpp / 8u;
    scw = bi->w;
    sch = bi->h;
    scale = (int)(scw / vw);
    if ((int)(sch / vh) < scale)
        scale = (int)(sch / vh);
    if (scale < 1) {
        K->print("PLAY: EL VIDEO ES MAS GRANDE QUE LA PANTALLA\n");
        return 1;
    }
    ox = ((int)scw - (int)vw * scale) / 2;
    oy = ((int)sch - (int)vh * scale) / 2;
    make_palette((int)vmode);
    order = (u32 *)(unsigned long)ORDER_ADDR;
    cur = (u8 *)(unsigned long)CUR_ADDR;
    shown = (u8 *)(unsigned long)SHOWN_ADDR;
    memset(cur, 0, vn);
    memset(shown, 0, vn);
    build_order();

    K->set_tick(0);                     /* el escritorio no debe pintar mientras corre el video */
    if (ram_end() >= (u64)SAVE_BASE + (u64)pitch * sch && (u64)pitch * sch <= SAVE_MAX) {
        save_size = pitch * sch;
        memcpy((void *)(unsigned long)SAVE_BASE, (const void *)fbp, save_size);
        saved = 1;
    }
    fill_px(0, 0, (int)scw, (int)sch, 0);
    /* los pixeles de "shown" son 0 = negro en RGB332 y gris; el fondo negro pintado coincide */

    off = 32;
    gate = arate ? (arate * 1000u / fps1000) * 2u + 16u : 0;
    f = 0;
    t_start = 0;

    /* primer cuadro: se revela despacio desde el centro (para que se vea el efecto) */
    if (nframes) {
        u32 vb, ab;
        if (off + 8 > size)
            goto fin;
        vb = *(const u32 *)(base + off);
        ab = *(const u32 *)(base + off + 4);
        if (off + 8 + vb + ab > size || !decode(base + off + 8, vb))
            goto fin;
        {
            unsigned t0 = K->ms();
            cursor = 0;
            while (cursor < vn) {
                unsigned el = K->ms() - t0;
                u32 quota = el >= 1400 ? vn : (u32)((u64)vn * el / 1400u);
                if (quota > cursor)
                    draw_some((int)(quota - cursor));
                else
                    K->sleep(1);
                if (keys())
                    goto fin;
            }
        }
        snd = arate ? K->stream(0, 0, 0) : 0;
        if (snd)
            K->stream(4, 0, arate);
        t_start = K->ms();
        if (snd)
            push_audio(base + off + 8 + vb, ab);
        off += 8 + vb + ab;
        f = 1;
    }

    for (; f < nframes; f++) {
        u32 vb, ab;
        if (off + 8 > size)
            break;
        vb = *(const u32 *)(base + off);
        ab = *(const u32 *)(base + off + 4);
        if (off + 8 + vb + ab > size)
            break;
        /* esperar a que toque este cuadro; mientras, se termina de pintar el anterior (del centro al borde) */
        for (;;) {
            int idle;
            if (keys())
                goto fin;
            if (paused) {
                K->sleep(10);
                continue;
            }
            if (snd)
                idle = queued() <= gate;
            else
                due_ms = (u32)((u64)f * 1000000u / fps1000), idle = (K->ms() - t_start) >= due_ms;
            if (idle)
                break;
            if (draw_some(64) == 0)
                K->sleep(1);
        }
        if (snd)
            push_audio(base + off + 8 + vb, ab);
        if (!decode(base + off + 8, vb))
            break;
        cursor = 0;
        off += 8 + vb + ab;
    }
    /* que termine de pintarse y de sonar */
    while (cursor < vn)
        draw_some(256);
    if (snd) {
        unsigned t0 = K->ms();
        while (queued() > 0 && K->ms() - t0 < 2000 && !keys())
            K->sleep(5);
    } else {
        K->sleep(500);
    }
fin:
    if (snd)
        K->stream(2, 0, 0);
    if (saved) {
        memcpy((void *)fbp, (const void *)(unsigned long)SAVE_BASE, save_size);
    } else {
        fill_px(0, 0, (int)scw, (int)sch, 0);
        if (K->vid(0, 0, 0, 0) >= 0)
            K->vid(2, 0, 0, 0);
    }
    return ok ? 0 : 1;
}
