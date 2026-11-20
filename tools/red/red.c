/* red.c - RED.BIN: pila de red propia de BASIC-OS, en C puro (sin iPXE).
 *
 * Parte 1: driver de la tarjeta Intel e1000 (la de QEMU, 8086:100E) + Ethernet, ARP, IPv4, UDP, ICMP y DHCP.
 * Es un modulo PCBM normal enlazado a 0x4000000 (como DOOM.BIN); recibe el kapi del kernel y hace todo el trabajo
 * a mano: escanea el bus PCI, mapea el BAR de la tarjeta con K->map_mmio, pone los anillos de descriptores en un
 * banco de RAM (K->ram) y sondea (el kernel no usa las interrupciones de la tarjeta).
 *
 * Mientras esto no sepa TLS, iPXE (NET.BIN / NET.APP) sigue ahi como respaldo para HTTPS: PKG, FORO y COSMOS lo
 * usan igual que antes. RED es para ir probando nuestra pila por partes.
 *
 * Uso desde BASIC:
 *   RED              o  RED "dhcp"     pide una direccion por DHCP y la muestra (IP, mascara, gateway, DNS)
 *   RED "ping 10.0.2.2"                DHCP y luego hace ping (ICMP) a esa IP
 *   RED "mac"                          solo enciende la tarjeta y muestra su direccion MAC
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);

static const struct kapi *K;

/* ---------------- puertos (ring 0) ---------------- */
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline u32  inl(u16 p)  { u32 r; __asm__ volatile("inl %1,%0" : "=a"(r) : "Nd"(p)); return r; }

/* ---------------- texto ---------------- */
static void pr(const char *s) { K->print(s); }
static void prd(u32 v)
{
    char b[12]; int i = 11; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    pr(b + i);
}
static void prip(u32 ip)        /* ip en orden de red (big-endian en memoria) */
{
    prd(ip & 0xFF); pr("."); prd((ip >> 8) & 0xFF); pr(".");
    prd((ip >> 16) & 0xFF); pr("."); prd((ip >> 24) & 0xFF);
}
static void prmac(const u8 *m)
{
    static const char *hex = "0123456789ABCDEF";
    char b[18]; int i, j = 0;
    for (i = 0; i < 6; i++) {
        b[j++] = hex[m[i] >> 4]; b[j++] = hex[m[i] & 15];
        if (i < 5) b[j++] = ':';
    }
    b[j] = 0; pr(b);
}
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ieq(const char *a, const char *b)         /* igual sin importar mayusculas */
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) == lower(*b);
}
static int istarts(const char *a, const char *b)     /* a empieza por b (sin mayusculas) */
{
    while (*b) { if (lower(*a) != lower(*b)) return 0; a++; b++; }
    return 1;
}

/* ---------------- byte-swap ---------------- */
static u16 be16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
static void wrbe16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* ================================================================= e1000 ============ */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00C0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404

#define NRX 32
#define NTX 8
#define RXBUF 2048

struct rxd { volatile u64 addr; volatile u16 len; volatile u16 csum; volatile u8 status; volatile u8 err; volatile u16 sp; };
struct txd { volatile u64 addr; volatile u16 len; volatile u8 cso; volatile u8 cmd; volatile u8 status; volatile u8 css; volatile u16 sp; };

static volatile u8 *mmio;
static struct rxd *rxr;
static struct txd *txr;
static u8 *rxbuf;                      /* NRX * RXBUF */
static u8 *txbuf;                      /* un buffer de salida */
static u32 rx_tail;
static u8  mac[6];
static u64 net_bank;                   /* banco de RAM con todo lo de DMA */

static u32 rr(u32 r) { return *(volatile u32 *)(mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

static int eeprom(u32 word, u16 *out)
{
    u32 v; int t;
    wr(REG_EERD, (word << 8) | 1);
    for (t = 0; t < 10000; t++) {
        v = rr(REG_EERD);
        if (v & 0x10) { *out = (u16)(v >> 16); return 0; }
    }
    return 1;
}

/* escanea PCI, mapea el BAR, resetea la tarjeta y arma los anillos. 0 = bien */
static int nic_init(void)
{
    int bus, dev, found = 0;
    u32 bar = 0, i; u16 w;

    for (bus = 0; bus < 256 && !found; bus++)
        for (dev = 0; dev < 32; dev++) {
            u32 a = 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11);
            outl(0xCF8, a);
            u32 id = inl(0xCFC);
            if ((id & 0xFFFF) != 0x8086) continue;
            u16 d = (u16)(id >> 16);
            /* familia e1000 "copper" que emula QEMU y tarjetas reales comunes */
            if (d == 0x100E || d == 0x100F || d == 0x1010 || d == 0x1026 || d == 0x10D3 ||
                d == 0x153A || d == 0x10EA || d == 0x1502) {
                /* command: bus master + memoria */
                outl(0xCF8, a | 0x04);
                u32 cmd = inl(0xCFC);
                outl(0xCF8, a | 0x04); outl(0xCFC, (cmd & 0xFFFF0000u) | cmd | 0x06);
                outl(0xCF8, a | 0x10);              /* BAR0 */
                bar = inl(0xCFC) & 0xFFFFFFF0u;
                found = 1; break;
            }
        }
    if (!found) { pr("RED: no encuentro una tarjeta e1000 (en QEMU usa -nic user,model=e1000)\n"); return 1; }

    mmio = (volatile u8 *)K->map_mmio(bar, 0x20000);
    if (!mmio) { pr("RED: no pude mapear el BAR de la tarjeta\n"); return 1; }

    /* memoria para DMA: un banco de 32 MB (en <1 GB queda identidad = su direccion fisica) */
    net_bank = K->ram(RAM_ALLOC, 0, 1, "RED");
    if (!net_bank || net_bank >= 0x40000000ull) {
        pr("RED: sin banco de RAM utilizable para DMA (prueba con menos de 1 GB)\n");
        return 1;
    }
    rxr   = (struct rxd *)(unsigned long)net_bank;
    txr   = (struct txd *)(unsigned long)(net_bank + 0x1000);
    rxbuf = (u8 *)(unsigned long)(net_bank + 0x2000);
    txbuf = (u8 *)(unsigned long)(net_bank + 0x2000 + (u64)NRX * RXBUF);

    /* reset */
    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rr(REG_CTRL) | 0x04000000u);       /* RST */
    for (i = 0; i < 100000; i++) if (!(rr(REG_CTRL) & 0x04000000u)) break;
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rr(REG_ICR);
    wr(REG_CTRL, (rr(REG_CTRL) | 0x41) & ~0x80u);   /* SLU | FD, sin LRST */

    /* MAC: de RAL/RAH (QEMU ya lo puso); si no, de la EEPROM */
    {
        u32 ral = rr(REG_RAL0), rah = rr(REG_RAH0);
        if (ral || (rah & 0xFFFF)) {
            mac[0] = (u8)ral; mac[1] = (u8)(ral >> 8); mac[2] = (u8)(ral >> 16); mac[3] = (u8)(ral >> 24);
            mac[4] = (u8)rah; mac[5] = (u8)(rah >> 8);
        } else {
            for (i = 0; i < 3; i++) { if (eeprom(i, &w)) { pr("RED: no pude leer la MAC\n"); return 1; } mac[2*i] = (u8)w; mac[2*i+1] = (u8)(w >> 8); }
        }
        wr(REG_RAL0, (u32)mac[0] | mac[1] << 8 | mac[2] << 16 | (u32)mac[3] << 24);
        wr(REG_RAH0, (u32)mac[4] | mac[5] << 8 | 0x80000000u);   /* AV */
    }

    for (i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);   /* filtro multicast en cero */

    /* anillo RX */
    for (i = 0; i < NRX; i++) {
        rxr[i].addr = net_bank + 0x2000 + (u64)i * RXBUF;
        rxr[i].status = 0;
    }
    wr(REG_RDBAL, (u32)(net_bank));
    wr(REG_RDBAH, (u32)(net_bank >> 32));
    wr(REG_RDLEN, NRX * 16);
    wr(REG_RDH, 0);
    wr(REG_RDT, NRX - 1);
    rx_tail = 0;
    wr(REG_RCTL, 0x2 | 0x8000 | 0x04000000u);       /* EN | BAM | SECRC, BSIZE=2048 */

    /* anillo TX */
    for (i = 0; i < NTX; i++) { txr[i].addr = 0; txr[i].status = 1; txr[i].cmd = 0; }
    wr(REG_TDBAL, (u32)(net_bank + 0x1000));
    wr(REG_TDBAH, (u32)((net_bank + 0x1000) >> 32));
    wr(REG_TDLEN, NTX * 16);
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    wr(REG_TIPG, 10 | (10 << 10) | (10 << 20));
    wr(REG_TCTL, 0x2 | 0x8 | (0x0F << 4) | (0x40 << 12));   /* EN | PSP | CT | COLD */
    return 0;
}

static u32 tx_tail;
static void nic_send(const u8 *data, u32 len)
{
    struct txd *d = &txr[tx_tail];
    int t;
    if (len > RXBUF) len = RXBUF;
    memcpy(txbuf, data, len);
    d->addr = net_bank + 0x2000 + (u64)NRX * RXBUF;
    d->len = (u16)len;
    d->cso = 0; d->css = 0; d->sp = 0;
    d->status = 0;
    d->cmd = 0x1 | 0x2 | 0x8;                        /* EOP | IFCS | RS */
    __asm__ volatile("" ::: "memory");
    tx_tail = (tx_tail + 1) % NTX;
    wr(REG_TDT, tx_tail);
    for (t = 0; t < 1000000; t++) { if (d->status & 0x0F) break; }
}

/* recibe una trama (sin copiar: devuelve puntero y longitud). 0 = no hay nada. */
static u8 *nic_recv(u32 *len)
{
    struct rxd *d = &rxr[rx_tail];
    if (!(d->status & 0x1)) return 0;               /* DD */
    *len = d->len;
    return rxbuf + (u64)rx_tail * RXBUF;
}
static void nic_recv_done(void)
{
    rxr[rx_tail].status = 0;
    wr(REG_RDT, rx_tail);
    rx_tail = (rx_tail + 1) % NRX;
}

/* ================================================================= pila IP ============ */
static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static u32 my_ip, my_mask, my_gw, my_dns;
static u8  gw_mac[6];
static int gw_known;

/* suma de verificacion de internet */
static u16 cksum(const u8 *p, int n, u32 start)
{
    u32 s = start; int i;
    for (i = 0; i + 1 < n; i += 2) s += (u32)p[i] << 8 | p[i + 1];
    if (i < n) s += (u32)p[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}

/* arma una trama Ethernet en txbuf-like scratch y la manda; devuelve puntero al payload para rellenar antes */
static u8 frame[RXBUF];
static int eth_start(const u8 *dst, u16 type)
{
    memcpy(frame, dst, 6);
    memcpy(frame + 6, mac, 6);
    wrbe16(frame + 12, type);
    return 14;
}

/* ---- ARP ---- */
static void arp_send(u16 op, const u8 *tha, u32 tpa, const u8 *dst)
{
    int o = eth_start(dst, 0x0806);
    u8 *a = frame + o;
    wrbe16(a + 0, 1); wrbe16(a + 2, 0x0800); a[4] = 6; a[5] = 4; wrbe16(a + 6, op);
    memcpy(a + 8, mac, 6); memcpy(a + 14, &my_ip, 4);
    memcpy(a + 18, tha, 6); memcpy(a + 24, &tpa, 4);
    nic_send(frame, o + 28);
}

/* procesa una trama entrante de servicio (ARP, ICMP eco-respuesta a nuestras peticiones).
 * Devuelve el tipo que reconocio para el que espera: 1=arp-reply guardado, 2=icmp echo reply, 0=otra */
static u32 pend_arp_ip;      /* IP cuya MAC esperamos */
static u8  pend_arp_mac[6];
static int pend_arp_ok;

static int handle(u8 *p, u32 n)
{
    u16 type;
    if (n < 14) return 0;
    type = be16(*(u16 *)(p + 12));
    if (type == 0x0806 && n >= 42) {                /* ARP */
        u8 *a = p + 14;
        u16 op = be16(*(u16 *)(a + 6));
        u32 spa; memcpy(&spa, a + 14, 4);
        if (op == 2 && pend_arp_ip && spa == pend_arp_ip) {   /* reply que esperabamos */
            memcpy(pend_arp_mac, a + 8, 6); pend_arp_ok = 1; return 1;
        }
        if (op == 1) {                              /* request: contestamos si es por nuestra IP */
            u32 tpa; memcpy(&tpa, a + 24, 4);
            if (my_ip && tpa == my_ip) arp_send(2, a + 8, spa, a + 8);
        }
        return 0;
    }
    if (type == 0x0800 && n >= 34) {                /* IPv4 */
        u8 *ip = p + 14;
        int ihl = (ip[0] & 0xF) * 4;
        if ((ip[0] >> 4) != 4 || ihl < 20) return 0;
        if (ip[9] == 1) {                           /* ICMP */
            u8 *ic = ip + ihl;
            if (ic[0] == 0) return 2;               /* echo reply */
        }
        if (ip[9] == 17) return 17;                 /* UDP: lo mira el que espera DHCP */
    }
    return 0;
}

/* espera hasta ms milisegundos una trama que handle() clasifique como "want"; devuelve su puntero/len.
 * mientras tanto contesta ARPs. 0 si venció el tiempo. */
static u8 *wait_for(int want, u32 ms, u32 *outlen)
{
    u32 t0 = K->ms();
    while (K->ms() - t0 < ms) {
        u32 n; u8 *p = nic_recv(&n);
        if (!p) { if (K->poll() == 27) return 0; continue; }
        int kind = handle(p, n);
        if (kind == want) { *outlen = n; return p; }     /* ojo: el llamante hace nic_recv_done */
        nic_recv_done();
    }
    return 0;
}

/* resuelve la MAC de una IP del mismo segmento por ARP (varios intentos) */
static int arp_resolve(u32 ip, u8 *out)
{
    int try_; u32 n;
    pend_arp_ip = ip; pend_arp_ok = 0;
    for (try_ = 0; try_ < 4; try_++) {
        arp_send(1, bcast, ip, bcast);
        if (wait_for(1, 300, &n)) { nic_recv_done(); }
        if (pend_arp_ok) { memcpy(out, pend_arp_mac, 6); pend_arp_ip = 0; return 0; }
    }
    pend_arp_ip = 0;
    return 1;
}

/* ---- IPv4 + envio a traves del gateway o directo ---- */
static u16 ip_id = 1;
static int send_ip(u32 dst, u8 proto, const u8 *payload, int plen)
{
    u8 nh[6]; u8 *ip;
    u32 route = ((dst ^ my_ip) & my_mask) ? my_gw : dst;   /* fuera del segmento -> gateway */
    if (route == my_gw && gw_known) memcpy(nh, gw_mac, 6);
    else if (arp_resolve(route, nh)) { pr("RED: no pude resolver "); prip(route); pr(" (ARP)\n"); return 1; }
    else if (route == my_gw) { memcpy(gw_mac, nh, 6); gw_known = 1; }

    int o = eth_start(nh, 0x0800);
    ip = frame + o;
    memset(ip, 0, 20);
    ip[0] = 0x45; ip[2] = (u8)((20 + plen) >> 8); ip[3] = (u8)(20 + plen);
    ip[4] = (u8)(ip_id >> 8); ip[5] = (u8)ip_id; ip_id++;
    ip[8] = 64; ip[9] = proto;
    memcpy(ip + 12, &my_ip, 4); memcpy(ip + 16, &dst, 4);
    { u16 c = cksum(ip, 20, 0); ip[10] = (u8)(c >> 8); ip[11] = (u8)c; }
    memcpy(ip + 20, payload, plen);
    nic_send(frame, o + 20 + plen);
    return 0;
}

/* ---- ICMP ping ---- */
static int do_ping(u32 ip)
{
    u8 pkt[64]; u32 n; int s, got = 0;
    pr("PING "); prip(ip); pr(":\n");
    for (s = 0; s < 4; s++) {
        u32 t0;
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 8;                                 /* echo request */
        pkt[4] = 0x13; pkt[5] = 0x37; pkt[6] = 0; pkt[7] = (u8)s;
        for (int i = 8; i < 40; i++) pkt[i] = (u8)('a' + (i & 15));
        { u16 c = cksum(pkt, 40, 0); pkt[2] = (u8)(c >> 8); pkt[3] = (u8)c; }
        t0 = K->ms();
        if (send_ip(ip, 1, pkt, 40)) return 1;
        if (wait_for(2, 1000, &n)) {
            nic_recv_done();
            pr("  respuesta de "); prip(ip); pr("  "); prd(K->ms() - t0); pr(" ms\n");
            got++;
        } else {
            pr("  sin respuesta\n");
        }
        K->sleep(300);
    }
    prd(got); pr(" de 4 respondieron\n");
    return got ? 0 : 1;
}

/* ================================================================= DHCP ============ */
static u32 dhcp_xid;
static void dhcp_send(u8 kind, u32 reqip, u32 server)
{
    u8 *u, *d; int o, dl;
    /* UDP 68->67 hacia 255.255.255.255 ; IP origen 0.0.0.0, broadcast Ethernet */
    o = eth_start(bcast, 0x0800);
    u8 *ip = frame + o;
    /* construimos primero el DHCP para saber la longitud */
    d = frame + o + 20 + 8;
    memset(d, 0, 300);
    d[0] = 1; d[1] = 1; d[2] = 6;                   /* op=BOOTREQUEST, htype=eth, hlen=6 */
    memcpy(d + 4, &dhcp_xid, 4);
    wrbe16(d + 10, 0x8000);                         /* broadcast flag */
    memcpy(d + 28, mac, 6);                         /* chaddr */
    d[236] = 0x63; d[237] = 0x82; d[238] = 0x53; d[239] = 0x63;    /* magic cookie */
    {
        u8 *op = d + 240;
        *op++ = 53; *op++ = 1; *op++ = kind;        /* message type */
        *op++ = 55; *op++ = 4; *op++ = 1; *op++ = 3; *op++ = 6; *op++ = 51;   /* param req: mask,router,dns,lease */
        if (reqip) { *op++ = 50; *op++ = 4; memcpy(op, &reqip, 4); op += 4; }
        if (server) { *op++ = 54; *op++ = 4; memcpy(op, &server, 4); op += 4; }
        *op++ = 255;
        dl = (int)(op - d);
    }
    /* UDP */
    u = frame + o + 20;
    wrbe16(u + 0, 68); wrbe16(u + 2, 67); wrbe16(u + 4, (u16)(8 + dl)); u[6] = 0; u[7] = 0;
    /* IP */
    memset(ip, 0, 20);
    ip[0] = 0x45; wrbe16(ip + 2, (u16)(20 + 8 + dl));
    ip[4] = (u8)(ip_id >> 8); ip[5] = (u8)ip_id; ip_id++;
    ip[8] = 64; ip[9] = 17;                          /* UDP */
    memset(ip + 12, 0, 4);                           /* src 0.0.0.0 */
    memset(ip + 16, 0xFF, 4);                        /* dst 255.255.255.255 */
    { u16 c = cksum(ip, 20, 0); ip[10] = (u8)(c >> 8); ip[11] = (u8)c; }
    nic_send(frame, o + 20 + 8 + dl);
}

/* busca una opcion DHCP; devuelve puntero a su valor y pone *len */
static const u8 *dhcp_opt(const u8 *d, int n, u8 want, int *len)
{
    int i = 240;
    while (i < n && d[i] != 255) {
        if (d[i] == 0) { i++; continue; }
        u8 code = d[i]; int l = d[i + 1];
        if (code == want) { *len = l; return d + i + 2; }
        i += 2 + l;
    }
    return 0;
}

/* lee un OFFER/ACK que coincida con nuestro xid; devuelve el tipo (2=offer,5=ack) o 0 */
static int dhcp_reap(u32 ms, u32 *yiaddr, u32 *server, u32 *mask, u32 *gw, u32 *dns)
{
    u32 t0 = K->ms();
    while (K->ms() - t0 < ms) {
        u32 n; u8 *p = nic_recv(&n);
        if (!p) { if (K->poll() == 27) return 0; continue; }
        if (handle(p, n) == 0 && be16(*(u16 *)(p + 12)) == 0x0806) { nic_recv_done(); continue; }
        /* miramos si es UDP 67->68 con nuestro xid */
        if (n >= 14 + 20 + 8 + 240 && be16(*(u16 *)(p + 12)) == 0x0800) {
            u8 *ip = p + 14; int ihl = (ip[0] & 0xF) * 4;
            if (ip[9] == 17) {
                u8 *udp = ip + ihl;
                if (be16(*(u16 *)(udp + 2)) == 68) {
                    u8 *d = udp + 8; int dl = (int)n - (int)(d - p);
                    int l; const u8 *o;
                    if (dl > 240 && memcmp(&dhcp_xid, d + 4, 4) == 0) {
                        int type = 0;
                        o = dhcp_opt(d, dl, 53, &l); if (o) type = o[0];
                        memcpy(yiaddr, d + 16, 4);
                        o = dhcp_opt(d, dl, 54, &l); if (o && l == 4) memcpy(server, o, 4);
                        o = dhcp_opt(d, dl, 1, &l);  if (o && l == 4) memcpy(mask, o, 4);
                        o = dhcp_opt(d, dl, 3, &l);  if (o && l >= 4) memcpy(gw, o, 4);
                        o = dhcp_opt(d, dl, 6, &l);  if (o && l >= 4) memcpy(dns, o, 4);
                        nic_recv_done();
                        return type;
                    }
                }
            }
        }
        nic_recv_done();
    }
    return 0;
}

static int do_dhcp(void)
{
    u32 yip = 0, srv = 0, mask = 0, gw = 0, dns = 0;
    int try_;
    dhcp_xid = K->seed() ^ (K->ms() << 8) ^ 0xB0510Full;
    my_ip = 0; gw_known = 0;
    for (try_ = 0; try_ < 4; try_++) {
        dhcp_send(1, 0, 0);                          /* DISCOVER */
        if (dhcp_reap(1200, &yip, &srv, &mask, &gw, &dns) == 2 && yip) break;
        yip = 0;
    }
    if (!yip) { pr("RED: nadie contesto el DHCP (no hay servidor en la red)\n"); return 1; }
    for (try_ = 0; try_ < 4; try_++) {
        dhcp_send(3, yip, srv);                      /* REQUEST */
        if (dhcp_reap(1200, &yip, &srv, &mask, &gw, &dns) == 5) {
            my_ip = yip; my_mask = mask ? mask : 0x00FFFFFF; my_gw = gw; my_dns = dns;
            return 0;
        }
    }
    pr("RED: el servidor no confirmo la IP (sin ACK)\n");
    return 1;
}

static void show_cfg(void)
{
    pr("  IP:      "); prip(my_ip); pr("\n");
    pr("  mascara: "); prip(my_mask); pr("\n");
    pr("  gateway: "); prip(my_gw); pr("\n");
    pr("  DNS:     "); prip(my_dns); pr("\n");
}

/* "10.0.2.2" -> IP en orden de red, o 0 si no es una IP valida */
static u32 parse_ip(const char *s)
{
    u32 ip = 0; int part = 0, val = 0, dig = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); dig = 1; if (val > 255) return 0; }
        else if (*s == '.' || *s == 0) {
            if (!dig || part > 3) return 0;
            ip |= (u32)val << (part * 8);
            part++; val = 0; dig = 0;
            if (*s == 0) break;
        } else return 0;
    }
    return part == 4 ? ip : 0;
}

/* ================================================================= entrada ============ */
int mod_main(const struct kapi *k)
{
    const char *a;
    K = k;
    a = K->args ? K->args : "";
    while (*a == ' ') a++;

    if (nic_init()) return 1;

    pr("RED: tarjeta e1000, MAC "); prmac(mac); pr("\n");

    if (ieq(a, "mac")) return 0;

    if (do_dhcp()) { K->ram(RAM_FREE, net_bank, 0, 0); return 1; }
    pr("DHCP:\n"); show_cfg();

    if (istarts(a, "ping")) {
        const char *h = a + 4; u32 ip;
        while (*h == ' ') h++;
        ip = parse_ip(h);
        if (!ip) pr("RED: 'ping' necesita una IP, por ejemplo RED \"ping 10.0.2.2\"\n");
        else do_ping(ip);
    }

    K->ram(RAM_FREE, net_bank, 0, 0);
    return 0;
}
