/* blk.c - dispositivos de bloques
 *
 * Los discos ATA/SATA/IDE y las disqueteras los ve la BIOS (en el modo en que este
 * configurada la controladora), y aqui se leen con INT 13h: extendido (AH=42h) cuando
 * existe y CHS (AH=02h) en disquetes. Las lectoras USB se anaden desde usb.c con su
 * propia funcion de lectura.
 */
#include "kernel.h"

#define MAXBLK 16
#define EDD_BUF  0x8F60
#define DAP      0x8F40

static struct blk devs[MAXBLK];
static int ndev;
static int probed;

static void set_name(struct blk *b, const char *pre, int n)
{
    int i = 0;
    while (*pre)
        b->name[i++] = *pre++;
    b->name[i++] = (char)('0' + n);
    b->name[i] = 0;
}

void blk_add(struct blk *b)
{
    int n = 0, i, len = 0;
    if (ndev >= MAXBLK)
        return;
    while (b->name[len] && !(b->name[len] >= '0' && b->name[len] <= '9'))
        len++;
    for (i = 0; i < ndev; i++) {                  /* numerar por tipo: CD0, CD1, USB0... */
        int k;
        for (k = 0; k < len && devs[i].name[k] == b->name[k]; k++)
            ;
        if (k == len && devs[i].name[len] >= '0' && devs[i].name[len] <= '9')
            n++;
    }
    devs[ndev] = *b;
    devs[ndev].name[len] = (char)('0' + n);
    devs[ndev].name[len + 1] = 0;
    ndev++;
}

static int geometry(struct blk *b)
{
    struct bregs r;
    r.ax = 0x0800;
    r.bx = 0;
    r.cx = 0;
    r.dx = (u16)b->bios;
    r.si = 0;
    if (bios_int13(&r))
        return 0;
    b->spt = r.cx & 0x3F;
    b->heads = (u32)(r.dx >> 8) + 1;
    b->cyls = (u32)(((r.cx >> 6) & 3) << 8 | (r.cx >> 8)) + 1;
    return b->spt != 0;
}

static void probe(void)
{
    struct bregs r;
    struct blk b;
    int n, count, boot;

    if (probed)
        return;
    probed = 1;
    boot = *(volatile u8 *)0x5FF0;

    r.ax = 0x0800; r.bx = 0; r.cx = 0; r.dx = 0; r.si = 0;     /* disqueteras */
    count = 0;
    if (!bios_int13(&r))
        count = r.dx & 0xFF;
    for (n = 0; n < count && n < 2; n++) {
        memset(&b, 0, sizeof b);
        b.bios = n;
        b.bps = 512;
        if (!geometry(&b))
            continue;
        b.sectors = (u64)b.cyls * b.heads * b.spt;
        b.boot = (boot == n);
        set_name(&b, "FD", n);
        blk_add(&b);
    }

    r.ax = 0x0800; r.bx = 0; r.cx = 0; r.dx = 0x80; r.si = 0;  /* discos duros */
    count = 0;
    if (!bios_int13(&r))
        count = r.dx & 0xFF;
    if (boot >= 0x80 && boot - 0x80 >= count)
        count = boot - 0x80 + 1;                  /* la unidad de arranque siempre */
    if (count > 8)
        count = 8;
    for (n = 0; n < count; n++) {
        memset(&b, 0, sizeof b);
        b.bios = 0x80 + n;
        b.bps = 512;
        r.ax = 0x4100; r.bx = 0x55AA; r.cx = 0; r.dx = (u16)b.bios; r.si = 0;
        if (!bios_int13(&r) && r.bx == 0xAA55 && (r.cx & 1)) {
            u8 *eb = (u8 *)EDD_BUF;
            memset(eb, 0, 0x42);
            eb[0] = 0x1E;
            r.ax = 0x4800; r.dx = (u16)b.bios; r.si = EDD_BUF;
            if (!bios_int13(&r)) {
                u32 bps = (u32)eb[0x18] | ((u32)eb[0x19] << 8);
                u64 sec = 0;
                int k;
                for (k = 7; k >= 0; k--)
                    sec = (sec << 8) | eb[0x10 + k];
                if (bps) {
                    b.edd = 1;
                    b.bps = bps;
                    b.sectors = sec;
                }
            }
        }
        geometry(&b);
        if (!b.edd) {
            if (!b.spt)
                continue;                         /* no hay tal unidad */
            b.sectors = (u64)b.cyls * b.heads * b.spt;
        }
        b.boot = (boot == b.bios);
        set_name(&b, "HD", n);
        blk_add(&b);
    }

    atapi_probe();                                /* lectoras IDE (ATAPI) en los puertos clasicos */
    usb_probe();                                  /* lectoras y memorias USB (EHCI) */
}

int blk_count(void)
{
    probe();
    return ndev;
}

struct blk *blk_get(int i)
{
    probe();
    return (i >= 0 && i < ndev) ? &devs[i] : NULL;
}

int blk_find(const char *name, int len)
{
    int i, k;
    probe();
    for (i = 0; i < ndev; i++) {
        for (k = 0; k < len; k++) {
            char c = name[k];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (devs[i].name[k] != c)
                break;
        }
        if (k == len && !devs[i].name[len])
            return i;
    }
    return -1;
}

static int bios_read(struct blk *b, u64 lba, int n, void *buf)
{
    struct bregs r;
    int tries, err = 1, per = 65536 / (int)b->bps, done = 0;

    while (done < n) {
        int c = n - done < per ? n - done : per;
        u64 cur = lba + (u64)done;
        err = 1;
        for (tries = 0; tries < 3 && err; tries++) {
            if (b->edd) {
                u8 *d = (u8 *)DAP;
                int k;
                memset(d, 0, 16);
                d[0] = 0x10;
                d[2] = (u8)c;                     /* sectores */
                d[6] = (u8)(BOUNCE_SEG & 0xFF);   /* buffer = BOUNCE_SEG:0000 */
                d[7] = (u8)(BOUNCE_SEG >> 8);
                for (k = 0; k < 8; k++)
                    d[8 + k] = (u8)(cur >> (8 * k));
                r.ax = 0x4200; r.bx = 0; r.cx = 0; r.dx = (u16)b->bios; r.si = DAP;
                err = bios_int13(&r);
            } else {
                u32 spt = b->spt, hd = b->heads, cyl, t, h, s;
                if (!spt || !hd)
                    return 1;
                c = 1;                            /* CHS: un sector por llamada */
                cyl = (u32)(cur / (spt * hd));
                t = (u32)(cur % (spt * hd));
                h = t / spt;
                s = t % spt + 1;
                if (cyl > 1023)
                    return 1;
                r.ax = 0x0201;
                r.bx = 0;
                r.cx = (u16)(((cyl & 0xFF) << 8) | ((cyl >> 2) & 0xC0) | s);
                r.dx = (u16)((h << 8) | (u32)b->bios);
                r.si = 0;
                err = bios_int13(&r);
            }
            if (err) {
                r.ax = 0; r.dx = (u16)b->bios; r.bx = 0; r.cx = 0; r.si = 0;
                bios_int13(&r);                   /* reiniciar la unidad */
            }
        }
        if (err)
            return 1;
        memcpy((u8 *)buf + (size_t)done * b->bps, (void *)BOUNCE_BUF, (size_t)c * b->bps);
        done += c;
    }
    return 0;
}

int blk_read(struct blk *b, u64 lba, int n, void *buf)
{
    if (!b || n <= 0)
        return 1;
    if (b->rd)
        return b->rd(b, lba, n, buf);
    return bios_read(b, lba, n, buf);
}

static char *putnum(char *o, u64 v)
{
    char t[24];
    int i = 0;
    if (!v)
        t[i++] = '0';
    while (v) {
        t[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i)
        *o++ = t[--i];
    return o;
}

/* Linea de DRIVES para el dispositivo i */
int blk_line(int i, char *out)
{
    struct blk *b = blk_get(i);
    char *o = out;
    u64 mb;
    const char *s;

    if (!b)
        return 0;
    for (s = b->name; *s; s++)
        *o++ = *s;
    *o++ = ' ';
    mb = b->sectors * b->bps / (1024 * 1024);
    if (mb >= 10240) {
        o = putnum(o, mb / 1024);
        *o++ = 'G'; *o++ = 'B';
    } else {
        o = putnum(o, mb);
        *o++ = 'M'; *o++ = 'B';
    }
    *o++ = ' ';
    if (b->bps != 512) {
        o = putnum(o, b->bps);
        *o++ = 'B'; *o++ = '/'; *o++ = 'S'; *o++ = 'E'; *o++ = 'C'; *o++ = ' ';
    }
    if (b->boot) {
        const char *t = "BOOT";
        while (*t)
            *o++ = *t++;
    }
    *o = 0;
    return 1;
}
