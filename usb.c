/* usb.c - USB 2.0 (EHCI) + almacenamiento masivo (BOT / SCSI): lectoras de CD y memorias USB
 *
 * Solo lectura, por sondeo (sin interrupciones). Dispositivos de baja y plena velocidad
 * se ceden a la controladora companera (UHCI/OHCI), que no tiene driver aqui. xHCI
 * (USB 3.0) tampoco. Las unidades encontradas se dan de alta en blk.c como CDn / USBn.
 */
#include "kernel.h"

/* ---- registros EHCI ---- */
#define CAP_LENGTH   0x00
#define CAP_HCSPARAMS 0x04
#define CAP_HCCPARAMS 0x08
#define OP_CMD       0x00
#define OP_STS       0x04
#define OP_INTR      0x08
#define OP_SEGMENT   0x10
#define OP_ASYNC     0x18
#define OP_CONFIG    0x40
#define OP_PORTSC    0x44

#define CMD_RUN      (1u << 0)
#define CMD_RESET    (1u << 1)
#define CMD_ASE      (1u << 5)
#define STS_HALTED   (1u << 12)
#define STS_ASS      (1u << 15)

#define PORT_CCS     (1u << 0)
#define PORT_CSC     (1u << 1)
#define PORT_PE      (1u << 2)
#define PORT_PEC     (1u << 3)
#define PORT_OC      (1u << 4)
#define PORT_OCC     (1u << 5)
#define PORT_RESET   (1u << 8)
#define PORT_PP      (1u << 12)
#define PORT_OWNER   (1u << 13)

/* ---- estructuras DMA ---- */
struct qtd {
    volatile u32 next, alt, token, buf[5];
    volatile u32 ext[5];
    u32 pad[3];
} __attribute__((aligned(32)));

struct qh {
    volatile u32 hlink, epchar, epcaps, cur;
    volatile u32 next, alt, token, buf[5];
    volatile u32 ext[5];
    u32 pad[7];
} __attribute__((aligned(64)));

#define TOK_ACTIVE 0x80u
#define TOK_HALT   0x40u
#define TOK_ERR    0x38u               /* buffer, babble, xact */
#define PID_OUT 0
#define PID_IN  1
#define PID_SETUP 2

static struct qh  qh0;
static struct qtd qtds[4];
static u8 usb_setup[8]  __attribute__((aligned(64)));
static u8 usb_cbw[64]   __attribute__((aligned(64)));
static u8 usb_data[32768] __attribute__((aligned(4096)));

struct ehci {
    volatile u8 *cap, *op;
    int nports;
};

struct udev {
    struct ehci *hc;
    int addr, mps0;
    int bin, bout, mpin, mpout;         /* extremos bulk */
    int tog[2];                         /* toggle siguiente: [0] = IN, [1] = OUT */
    u32 tag;
    int type;                           /* tipo SCSI: 0 disco, 5 CD */
};

#define MAXDEV 4
#define MAXHC  3
static struct ehci hcs[MAXHC];
static struct udev udevs[MAXDEV];
static struct blk usb_blks[MAXDEV];
static int probed;

static u32 rd32v(volatile u8 *p, int off) { return *(volatile u32 *)(p + off); }
static void wr32v(volatile u8 *p, int off, u32 v) { *(volatile u32 *)(p + off) = v; }

static u32 phys(const void *p) { return (u32)(unsigned long)p; }

static void qtd_setbuf(struct qtd *q, const void *p, int len)
{
    u32 a = phys(p);
    int k;
    (void)len;
    q->buf[0] = a;
    for (k = 1; k < 5; k++)
        q->buf[k] = (a & ~0xFFFu) + (u32)k * 0x1000u;
    for (k = 0; k < 5; k++)
        q->ext[k] = 0;
}

static void qtd_set(struct qtd *q, int pid, int len, int toggle, int ioc, const void *buf, struct qtd *next)
{
    q->next = next ? phys(next) : 1;
    q->alt = 1;
    if (buf)
        qtd_setbuf(q, buf, len);
    q->token = TOK_ACTIVE | ((u32)pid << 8) | (3u << 10) | ((u32)ioc << 15) | ((u32)len << 16) |
               ((u32)toggle << 31);
}

static int wait_sts(struct ehci *h, u32 mask, u32 val, int ms)
{
    int i;
    for (i = 0; i < ms; i++) {
        if ((rd32v(h->op, OP_STS) & mask) == val)
            return 1;
        delay_ms(1);
    }
    return (rd32v(h->op, OP_STS) & mask) == val;
}

static void async_stop(struct ehci *h)
{
    wr32v(h->op, OP_CMD, rd32v(h->op, OP_CMD) & ~CMD_ASE);
    wait_sts(h, STS_ASS, 0, 100);
}

/* Ejecuta la cadena qtds[0..n-1] contra (addr, ep). 0 = bien, -1 = error, -2 = STALL, -3 = timeout */
static int run_chain(struct udev *d, int ep, int mps, int n, int timeout_ms)
{
    struct ehci *h = d->hc;
    int i, t, res = 0;

    async_stop(h);
    qh0.hlink = phys(&qh0) | 2;
    qh0.epchar = (u32)d->addr | ((u32)ep << 8) | (2u << 12) | (1u << 14) | (1u << 15) | ((u32)mps << 16);
    qh0.epcaps = 1u << 30;
    qh0.cur = 0;
    qh0.next = phys(&qtds[0]);
    qh0.alt = 1;
    qh0.token = 0;
    {
        int k;
        for (k = 0; k < 5; k++) {
            qh0.buf[k] = 0;
            qh0.ext[k] = 0;
        }
    }
    wr32v(h->op, OP_ASYNC, phys(&qh0));
    wr32v(h->op, OP_STS, 0x3F);
    wr32v(h->op, OP_CMD, rd32v(h->op, OP_CMD) | CMD_ASE);
    wait_sts(h, STS_ASS, STS_ASS, 100);

    for (t = 0; t < timeout_ms; t++) {
        int active = 0;
        for (i = 0; i < n; i++) {
            u32 tok = qtds[i].token;
            if (tok & TOK_HALT) {
                res = (tok & TOK_ERR) ? -1 : -2;
                goto done;
            }
            if (tok & TOK_ERR) {
                res = -1;
                goto done;
            }
            if (tok & TOK_ACTIVE) {
                active = 1;
                break;
            }
        }
        if (!active)
            goto done;
        delay_ms(1);
    }
    res = -3;
done:
    async_stop(h);
    return res;
}

static int ctrl(struct udev *d, u8 rt, u8 rq, u16 val, u16 idx, u16 len, void *data)
{
    int in = (rt & 0x80) != 0, n = 0, r;

    usb_setup[0] = rt;
    usb_setup[1] = rq;
    usb_setup[2] = (u8)val;
    usb_setup[3] = (u8)(val >> 8);
    usb_setup[4] = (u8)idx;
    usb_setup[5] = (u8)(idx >> 8);
    usb_setup[6] = (u8)len;
    usb_setup[7] = (u8)(len >> 8);
    qtd_set(&qtds[n], PID_SETUP, 8, 0, 0, usb_setup, &qtds[n + 1]);
    n++;
    if (len) {
        if (!in)
            memcpy(usb_data, data, len);
        qtd_set(&qtds[n], in ? PID_IN : PID_OUT, len, 1, 0, usb_data, &qtds[n + 1]);
        n++;
    }
    qtd_set(&qtds[n], in ? PID_OUT : PID_IN, 0, 1, 1, NULL, NULL);
    n++;
    r = run_chain(d, 0, d->mps0, n, 2000);
    if (!r && in && len)
        memcpy(data, usb_data, len);
    return r;
}

/* Transferencia bulk de len bytes en usb_data. dir 0 = IN, 1 = OUT. Devuelve bytes o error < 0 */
static int bulk(struct udev *d, int dir, void *buf, int len)
{
    int ep = dir ? d->bout : d->bin, mps = dir ? d->mpout : d->mpin, r, left;

    if (dir)
        memcpy(usb_data, buf, len);
    qtd_set(&qtds[0], dir ? PID_OUT : PID_IN, len, d->tog[dir ? 1 : 0], 1, usb_data, NULL);
    r = run_chain(d, ep & 15, mps, 1, 10000);
    if (r)
        return r;
    d->tog[dir ? 1 : 0] = (int)(qtds[0].token >> 31);
    left = (int)((qtds[0].token >> 16) & 0x7FFF);
    if (!dir)
        memcpy(buf, usb_data, (size_t)(len - left));
    return len - left;
}

static void clear_halt(struct udev *d, int dir)
{
    int ep = dir ? d->bout : d->bin;
    ctrl(d, 0x02, 1, 0, (u16)ep, 0, NULL);          /* CLEAR_FEATURE(ENDPOINT_HALT) */
    d->tog[dir ? 1 : 0] = 0;
}

/* Comando SCSI por BOT. Devuelve 0 = bien, 1 = fallo del comando, <0 = error de transporte */
static int bot(struct udev *d, const u8 *cb, int cblen, void *data, int dlen, int in)
{
    u8 csw[16];
    u32 tag = ++d->tag;
    int r, k;

    memset(usb_cbw, 0, 32);
    usb_cbw[0] = 'U'; usb_cbw[1] = 'S'; usb_cbw[2] = 'B'; usb_cbw[3] = 'C';
    usb_cbw[4] = (u8)tag; usb_cbw[5] = (u8)(tag >> 8); usb_cbw[6] = (u8)(tag >> 16); usb_cbw[7] = (u8)(tag >> 24);
    usb_cbw[8] = (u8)dlen; usb_cbw[9] = (u8)(dlen >> 8); usb_cbw[10] = (u8)(dlen >> 16); usb_cbw[11] = (u8)(dlen >> 24);
    usb_cbw[12] = in ? 0x80 : 0;
    usb_cbw[14] = (u8)cblen;
    for (k = 0; k < cblen; k++)
        usb_cbw[15 + k] = cb[k];
    r = bulk(d, 1, usb_cbw, 31);
    if (r < 0)
        return r;
    if (dlen) {
        r = bulk(d, in ? 0 : 1, data, dlen);
        if (r == -2)
            clear_halt(d, in ? 0 : 1);
        else if (r < 0)
            return r;
    }
    r = bulk(d, 0, csw, 13);
    if (r == -2) {
        clear_halt(d, 0);
        r = bulk(d, 0, csw, 13);
    }
    if (r < 13)
        return r < 0 ? r : -1;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S')
        return -1;
    return csw[12] ? 1 : 0;
}

static void sense(struct udev *d)
{
    u8 cb[6] = { 0x03, 0, 0, 0, 18, 0 }, buf[18];
    bot(d, cb, 6, buf, 18, 1);
}

static int tur(struct udev *d)
{
    u8 cb[6] = { 0, 0, 0, 0, 0, 0 };
    int r = bot(d, cb, 6, NULL, 0, 0);
    if (r)
        sense(d);
    return r;
}

static int usb_scsi(struct blk *b, const u8 *cdb, int cdblen, void *data, int dlen)
{
    struct udev *d = b->priv;
    int tries, r = 1;
    for (tries = 0; tries < 2 && r; tries++) {
        r = bot(d, cdb, cdblen, data, dlen, 1);
        if (r) {
            sense(d);
            delay_ms(100);
        }
    }
    return r ? 1 : 0;
}

static int usb_read(struct blk *b, u64 lba, int n, void *buf)
{
    struct udev *d = b->priv;
    int per = 16384 / (int)b->bps, done = 0;

    while (done < n) {
        int c = n - done < per ? n - done : per, tries, r = 1;
        u32 l = (u32)(lba + (u64)done);
        u8 cb[10];
        cb[0] = 0x28; cb[1] = 0;
        cb[2] = (u8)(l >> 24); cb[3] = (u8)(l >> 16); cb[4] = (u8)(l >> 8); cb[5] = (u8)l;
        cb[6] = 0; cb[7] = (u8)(c >> 8); cb[8] = (u8)c; cb[9] = 0;
        for (tries = 0; tries < 3 && r; tries++) {
            r = bot(d, cb, 10, (u8 *)buf + (size_t)done * b->bps, c * (int)b->bps, 1);
            if (r) {
                sense(d);
                delay_ms(100);
            }
        }
        if (r)
            return 1;
        done += c;
    }
    return 0;
}

/* ---- enumeracion ---- */

static int hc_init(struct ehci *h, int bus, int dev, int fn)
{
    u32 bar = pci_read32(bus, dev, fn, 0x10), cmd, hcs_, hccp, eecp;
    int i, caplen;

    if (bar & 1)
        return 0;
    pci_write32(bus, dev, fn, 0x04, (pci_read32(bus, dev, fn, 0x04) & 0xFFFF) | 0x6);
    h->cap = map_mmio(bar & ~0xFu, 0x1000);
    if (!h->cap)
        return 0;
    caplen = h->cap[CAP_LENGTH];
    h->op = h->cap + caplen;
    hcs_ = rd32v(h->cap, CAP_HCSPARAMS);
    hccp = rd32v(h->cap, CAP_HCCPARAMS);
    h->nports = (int)(hcs_ & 15);

    eecp = (hccp >> 8) & 0xFF;                      /* BIOS suelta la controladora */
    while (eecp >= 0x40) {
        u32 v = pci_read32(bus, dev, fn, (int)eecp);
        if ((v & 0xFF) == 1) {
            pci_write32(bus, dev, fn, (int)eecp, v | (1u << 24));
            for (i = 0; i < 1000 && (pci_read32(bus, dev, fn, (int)eecp) & (1u << 16)); i++)
                delay_ms(1);
            pci_write32(bus, dev, fn, (int)eecp + 4, 0);
            break;
        }
        eecp = (v >> 8) & 0xFF;
    }

    wr32v(h->op, OP_CMD, rd32v(h->op, OP_CMD) & ~CMD_RUN);
    for (i = 0; i < 100 && !(rd32v(h->op, OP_STS) & STS_HALTED); i++)
        delay_ms(1);
    wr32v(h->op, OP_CMD, CMD_RESET);
    for (i = 0; i < 500 && (rd32v(h->op, OP_CMD) & CMD_RESET); i++)
        delay_ms(1);
    if (rd32v(h->op, OP_CMD) & CMD_RESET)
        return 0;
    wr32v(h->op, OP_INTR, 0);
    wr32v(h->op, OP_SEGMENT, 0);
    wr32v(h->op, OP_STS, 0x3F);
    cmd = CMD_RUN | (8u << 16);                     /* sin interrupciones; umbral de 8 microtramas */
    wr32v(h->op, OP_CMD, cmd);
    wr32v(h->op, OP_CONFIG, 1);                     /* todos los puertos a EHCI */
    delay_ms(10);
    return 1;
}

static int port_reset(struct ehci *h, int p)
{
    u32 sc = rd32v(h->op, OP_PORTSC + 4 * p);
    int i;

    if (!(sc & PORT_CCS))
        return 0;
    if (((sc >> 10) & 3) == 1) {                    /* baja velocidad: a la companera */
        wr32v(h->op, OP_PORTSC + 4 * p, (sc & ~(PORT_PE | 0x2A)) | PORT_OWNER);
        return 0;
    }
    wr32v(h->op, OP_PORTSC + 4 * p, (sc & ~(PORT_PE | 0x2A)) | PORT_RESET);
    delay_ms(60);
    sc = rd32v(h->op, OP_PORTSC + 4 * p);
    wr32v(h->op, OP_PORTSC + 4 * p, sc & ~(PORT_RESET | 0x2A));
    for (i = 0; i < 100 && (rd32v(h->op, OP_PORTSC + 4 * p) & PORT_RESET); i++)
        delay_ms(1);
    delay_ms(20);
    sc = rd32v(h->op, OP_PORTSC + 4 * p);
    if (!(sc & PORT_PE)) {                          /* plena velocidad: a la companera */
        wr32v(h->op, OP_PORTSC + 4 * p, (sc & ~0x2A) | PORT_OWNER);
        return 0;
    }
    return 1;
}

static void add_device(struct ehci *h, int addr)
{
    struct udev *d = &udevs[0];
    u8 dd[18], cfg[256];
    int i, total, pos, got_if = 0, tries;
    u8 inq[36];
    u8 cb_inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    u8 cb_cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    u8 cap[8];
    struct blk *b;
    int nd = 0;

    for (i = 0; i < MAXDEV; i++)
        if (udevs[i].hc)
            nd++;
    if (nd >= MAXDEV)
        return;
    d = &udevs[nd];
    memset(d, 0, sizeof *d);
    d->hc = h;
    d->addr = 0;
    d->mps0 = 64;
    if (ctrl(d, 0x80, 6, 0x0100, 0, 18, dd))        /* GET_DESCRIPTOR(device) */
        goto fail;
    if (ctrl(d, 0x00, 5, (u16)addr, 0, 0, NULL))    /* SET_ADDRESS */
        goto fail;
    delay_ms(20);
    d->addr = addr;
    d->mps0 = dd[7] ? dd[7] : 64;
    if (ctrl(d, 0x80, 6, 0x0200, 0, 9, cfg))
        goto fail;
    total = cfg[2] | (cfg[3] << 8);
    if (total > 255)
        total = 255;
    if (ctrl(d, 0x80, 6, 0x0200, 0, (u16)total, cfg))
        goto fail;
    for (pos = 0; pos + 2 <= total && cfg[pos]; pos += cfg[pos]) {
        if (cfg[pos + 1] == 4) {                    /* interfaz: almacenamiento masivo BOT? */
            got_if = (cfg[pos + 5] == 8 && cfg[pos + 6] == 6 && cfg[pos + 7] == 0x50);
        } else if (cfg[pos + 1] == 5 && got_if && (cfg[pos + 3] & 3) == 2) {
            int ep = cfg[pos + 2], mp = cfg[pos + 4] | (cfg[pos + 5] << 8);
            if (ep & 0x80) { d->bin = ep & 0x8F; d->mpin = mp; }
            else           { d->bout = ep & 0x0F; d->mpout = mp; }
        }
    }
    if (!d->bin || !d->bout)
        goto fail;                                  /* no es almacenamiento masivo */
    d->bin |= 0x80;
    if (ctrl(d, 0x00, 9, cfg[5], 0, 0, NULL))       /* SET_CONFIGURATION */
        goto fail;
    delay_ms(20);

    if (bot(d, cb_inq, 6, inq, 36, 1))
        goto fail;
    d->type = inq[0] & 0x1F;
    b = &usb_blks[nd];
    memset(b, 0, sizeof *b);
    b->bios = -1;
    b->bps = d->type == 5 ? 2048 : 512;
    for (tries = 0; tries < 6; tries++) {           /* esperar a que la unidad este lista */
        if (!tur(d))
            break;
        delay_ms(300);
    }
    if (!bot(d, cb_cap, 10, cap, 8, 1)) {
        u32 last = ((u32)cap[0] << 24) | ((u32)cap[1] << 16) | ((u32)cap[2] << 8) | cap[3];
        u32 bs = ((u32)cap[4] << 24) | ((u32)cap[5] << 16) | ((u32)cap[6] << 8) | cap[7];
        if (bs)
            b->bps = bs;
        b->sectors = (u64)last + 1;
    }
    {
        int k = 0, n = 0;
        const char *pre = d->type == 5 ? "CD" : "USB";
        for (i = 0; i < nd; i++)
            if (usb_blks[i].name[0] == pre[0])
                n++;
        while (*pre)
            b->name[k++] = *pre++;
        b->name[k++] = (char)('0' + n);
        b->name[k] = 0;
    }
    b->rd = usb_read;
    if (d->type == 5)
        b->scsi = usb_scsi;
    b->priv = d;
    blk_add(b);
    return;
fail:
    d->hc = NULL;
}

void usb_probe(void)
{
    int n, bus, dev, fn, nh = 0, addr = 1;

    if (probed)
        return;
    probed = 1;
    for (n = 0; n < MAXHC && pci_find(0x0C, 0x03, 0x20, n, &bus, &dev, &fn); n++) {
        struct ehci *h = &hcs[nh];
        int p;
        if (!hc_init(h, bus, dev, fn))
            continue;
        nh++;
        for (p = 0; p < h->nports; p++) {
            if (!(rd32v(h->op, OP_PORTSC + 4 * p) & PORT_PP))
                wr32v(h->op, OP_PORTSC + 4 * p, rd32v(h->op, OP_PORTSC + 4 * p) | PORT_PP);
        }
        delay_ms(100);                              /* alimentacion y deteccion */
        for (p = 0; p < h->nports; p++)
            if (port_reset(h, p))
                add_device(h, addr++);
    }
}
