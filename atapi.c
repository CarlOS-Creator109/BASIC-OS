/* atapi.c - lectoras de CD/DVD IDE (ATAPI) en los puertos clasicos 0x1F0/0x170
 *
 * Protocolo PACKET por PIO: se manda el CDB de 12 bytes y se leen los datos por bloques que el
 * propio dispositivo anuncia (registros de cuenta). Sirve a las lectoras en modo IDE/compatible
 * (placas hasta ~2010, o SATA en modo "IDE"); en modo AHCI no hay ningun registro aqui.
 * Registra cada lectora como CDn (sectores de 2048 bytes) con soporte SCSI para el CD de audio.
 */
#include "kernel.h"

#define R_DATA 0
#define R_ERR  1
#define R_CNT  2
#define R_LBA0 3
#define R_LBA1 4
#define R_LBA2 5
#define R_DEV  6
#define R_CMD  7
#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

struct ata {
    u16 io, ctl;
    int slave;
};

static struct ata ata_devs[4];
static struct blk atapi_blks[4];

static void insw(u16 port, void *buf, int words)
{
    __asm__ volatile("rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}

static void outsw(u16 port, const void *buf, int words)
{
    __asm__ volatile("rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

static u8 st(struct ata *a) { return inb((u16)(a->io + R_CMD)); }

static void delay400(struct ata *a)
{
    int i;
    for (i = 0; i < 4; i++)
        (void)inb(a->ctl);
}

static int wait_not_busy(struct ata *a, int ms)
{
    int i;
    for (i = 0; i < ms * 10; i++) {
        u8 s = st(a);
        if (s == 0xFF)
            return -1;
        if (!(s & ST_BSY))
            return 0;
        if ((i % 10) == 9)
            delay_ms(1);
        else
            __asm__ volatile("pause");
    }
    return -1;
}

static int select(struct ata *a)
{
    outb((u16)(a->io + R_DEV), (u8)(0xA0 | (a->slave << 4)));
    delay400(a);
    return wait_not_busy(a, 200);
}

static u16 inw(u16 port)
{
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Un comando PACKET. data/len: datos de entrada (len = 0 si no hay). 0 = bien */
static int packet(struct ata *a, const u8 *cdb, int cdblen, void *data, int len)
{
    u8 pk[12];
    u8 *p = data;
    int i, lim = len > 0xFFFE ? 0xFFFE : len;
    u8 s;

    for (i = 0; i < 12; i++)
        pk[i] = i < cdblen ? cdb[i] : 0;
    if (select(a))
        return 1;
    outb((u16)(a->io + R_ERR), 0);              /* PIO, sin DMA */
    outb((u16)(a->io + R_LBA1), (u8)lim);
    outb((u16)(a->io + R_LBA2), (u8)(lim >> 8));
    outb((u16)(a->io + R_CMD), 0xA0);           /* PACKET */
    delay400(a);
    for (i = 0; i < 1000; i++) {                /* esperar DRQ para el CDB */
        s = st(a);
        if (!(s & ST_BSY) && (s & ST_DRQ))
            break;
        if ((s & ST_ERR) || s == 0xFF)
            return 1;
        delay_ms(1);
    }
    if (i == 1000)
        return 1;
    outsw((u16)(a->io + R_DATA), pk, 6);
    for (;;) {
        int bc, w;
        if (wait_not_busy(a, 12000))            /* hasta 12 s: el disco puede estar arrancando */
            return 1;
        s = st(a);
        if (s & (ST_ERR | ST_DF))
            return 1;
        if (!(s & ST_DRQ))
            return 0;
        bc = inb((u16)(a->io + R_LBA1)) | (inb((u16)(a->io + R_LBA2)) << 8);
        w = bc / 2;
        if (p && len >= bc) {
            insw((u16)(a->io + R_DATA), p, w);
            p += bc;
            len -= bc;
        } else {                                /* sobra: descartar */
            while (w-- > 0)
                (void)inw((u16)(a->io + R_DATA));
        }
    }
}

static int atapi_scsi(struct blk *b, const u8 *cdb, int cdblen, void *data, int dlen)
{
    struct ata *a = b->priv;
    int tries, r = 1;
    for (tries = 0; tries < 3 && r; tries++) {
        r = packet(a, cdb, cdblen, data, dlen);
        if (r) {                                /* disco recien puesto o arrancando */
            u8 rs[12] = { 0x03, 0, 0, 0, 18, 0 }, buf[18];
            packet(a, rs, 6, buf, 18);
            delay_ms(250);
        }
    }
    return r ? 1 : 0;
}

static int atapi_read(struct blk *b, u64 lba, int n, void *buf)
{
    int done = 0;
    while (done < n) {
        int c = n - done < 16 ? n - done : 16;
        u32 l = (u32)(lba + (u64)done);
        u8 cb[10] = { 0x28, 0, (u8)(l >> 24), (u8)(l >> 16), (u8)(l >> 8), (u8)l, 0,
                      (u8)(c >> 8), (u8)c, 0 };
        if (atapi_scsi(b, cb, 10, (u8 *)buf + (size_t)done * 2048, c * 2048))
            return 1;
        done += c;
    }
    return 0;
}

static int identify(struct ata *a)
{
    u16 id[256];
    int i;
    u8 s;

    if (select(a))
        return 0;
    outb((u16)(a->io + R_CMD), 0xA1);           /* IDENTIFY PACKET DEVICE */
    delay400(a);
    for (i = 0; i < 300; i++) {
        s = st(a);
        if (s == 0xFF || s == 0)
            return 0;
        if (!(s & ST_BSY))
            break;
        delay_ms(1);
    }
    s = st(a);
    if ((s & ST_ERR) || !(s & ST_DRQ))
        return 0;                               /* disco ATA normal o nada */
    insw((u16)(a->io + R_DATA), id, 256);
    return ((id[0] >> 14) & 3) == 2 && ((id[0] >> 8) & 0x1F) == 5;   /* ATAPI, tipo CD/DVD */
}

void atapi_probe(void)
{
    static const u16 ios[2] = { 0x1F0, 0x170 }, ctls[2] = { 0x3F6, 0x376 };
    int ch, sl, n = 0;

    for (ch = 0; ch < 2; ch++) {
        if (inb(ios[ch] + R_CMD) == 0xFF)       /* canal inexistente (bus flotante) */
            continue;
        for (sl = 0; sl < 2; sl++) {
            struct ata *a = &ata_devs[n];
            struct blk *b = &atapi_blks[n];
            u8 cap[8], cb[10] = { 0x25 };
            a->io = ios[ch];
            a->ctl = ctls[ch];
            a->slave = sl;
            outb(a->ctl, 0x02);                 /* nIEN: sin interrupciones, solo sondeo */
            if (!identify(a))
                continue;
            memset(b, 0, sizeof *b);
            b->name[0] = 'C'; b->name[1] = 'D';
            b->bios = -1;
            b->bps = 2048;
            b->rd = atapi_read;
            b->scsi = atapi_scsi;
            b->priv = a;
            if (!atapi_scsi(b, cb, 10, cap, 8)) {
                u32 last = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) | ((u32)cap[2] << 8) | cap[3];
                b->sectors = (u64)last + 1;
            }
            blk_add(b);
            n++;
            if (n >= 4)
                return;
        }
    }
}
