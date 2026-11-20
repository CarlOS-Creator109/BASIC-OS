/* bn.c - BN.BIN (BOS NET): pila de red propia de BASIC-OS, en C puro (sin iPXE).
 *
 * Driver de la tarjeta Intel e1000 (la de QEMU, 8086:100E) + Ethernet, ARP, IPv4, UDP, ICMP, DHCP, DNS, TCP y HTTP.
 * Es un modulo PCBM normal enlazado a 0x4000000 (como DOOM.BIN); recibe el kapi del kernel y hace todo a mano:
 * escanea PCI, mapea el BAR con K->map_mmio, pone los anillos de descriptores en un banco de RAM (K->ram) y sondea
 * (el kernel no usa las interrupciones de la tarjeta).
 *
 * Mientras BN no sepa TLS, iPXE (NET.BIN / NET.APP) sigue ahi para HTTPS: PKG, FORO y COSMOS lo usan igual.
 *
 * Uso desde BASIC:
 *   BN                o  BN "dhcp"          pide IP por DHCP y la muestra
 *   BN "mac"                                enciende la tarjeta y muestra su MAC
 *   BN "ping 10.0.2.2"                      DHCP y luego ping (ICMP)
 *   BN "dns ejemplo.com"                    DHCP y resuelve el nombre a IP
 *   BN "get http://10.0.2.2:8080/hola.txt"  DHCP, baja la pagina por HTTP y la muestra
 */
#include "kapi.h"
#include "mod_head.h"
#include "tls.h"

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
static int ieq(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) == lower(*b);
}
static int istarts(const char *a, const char *b)
{
    while (*b) { if (lower(*a) != lower(*b)) return 0; a++; b++; }
    return 1;
}

/* ---------------- byte-swap ---------------- */
static void wrbe16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wrbe32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

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
static u8 *rxbuf;
static u8 *txbuf;
static u8 *httpbuf;                     /* cuerpo de la respuesta HTTP, en el banco */
#define HTTPMAX (8u * 1024 * 1024)
static u32 rx_tail, tx_tail;
static u8  mac[6];           /* la que usamos (con la firma IMBOS) */
static u8  real_mac[6];      /* la que trae la tarjeta */
static u64 net_bank;

/* firma secreta IMBOS ("soy BOS"): se mezcla (XOR) con la MAC real, no se ve a simple vista; la tarjeta manda con ella */
static void imbos_mac(void)
{
    static const char sig[6] = { 'I','M','B','O','S', 'I'^'M'^'B'^'O'^'S' };
    int i;
    for (i = 0; i < 6; i++) real_mac[i] = mac[i];
    for (i = 0; i < 6; i++) mac[i] ^= (u8)sig[i];
    mac[0] = (u8)((mac[0] | 0x02) & ~0x01);     /* unicast + administrada localmente (MAC valida) */
}

static u32 rr(u32 r) { return *(volatile u32 *)(mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

static int eeprom(u32 word, u16 *out)
{
    u32 v; int t;
    wr(REG_EERD, (word << 8) | 1);
    for (t = 0; t < 10000; t++) { v = rr(REG_EERD); if (v & 0x10) { *out = (u16)(v >> 16); return 0; } }
    return 1;
}

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
            if (d == 0x100E || d == 0x100F || d == 0x1010 || d == 0x1026 || d == 0x10D3 ||
                d == 0x153A || d == 0x10EA || d == 0x1502) {
                outl(0xCF8, a | 0x04);
                u32 cmd = inl(0xCFC);
                outl(0xCF8, a | 0x04); outl(0xCFC, (cmd & 0xFFFF0000u) | cmd | 0x06);
                outl(0xCF8, a | 0x10);
                bar = inl(0xCFC) & 0xFFFFFFF0u;
                found = 1; break;
            }
        }
    if (!found) { pr("BN: no encuentro una tarjeta e1000 (en QEMU usa -nic user,model=e1000)\n"); return 1; }

    mmio = (volatile u8 *)K->map_mmio(bar, 0x20000);
    if (!mmio) { pr("BN: no pude mapear el BAR de la tarjeta\n"); return 1; }

    net_bank = K->ram(RAM_ALLOC, 0, 1, "BN");
    if (!net_bank || net_bank >= 0x40000000ull) {
        pr("BN: sin banco de RAM utilizable para DMA (prueba con menos de 1 GB)\n");
        return 1;
    }
    rxr   = (struct rxd *)(unsigned long)net_bank;
    txr   = (struct txd *)(unsigned long)(net_bank + 0x1000);
    rxbuf = (u8 *)(unsigned long)(net_bank + 0x2000);
    txbuf = (u8 *)(unsigned long)(net_bank + 0x2000 + (u64)NRX * RXBUF);
    httpbuf = (u8 *)(unsigned long)(net_bank + 0x200000);

    wr(REG_IMC, 0xFFFFFFFFu);
    wr(REG_CTRL, rr(REG_CTRL) | 0x04000000u);
    for (i = 0; i < 100000; i++) if (!(rr(REG_CTRL) & 0x04000000u)) break;
    wr(REG_IMC, 0xFFFFFFFFu);
    (void)rr(REG_ICR);
    wr(REG_CTRL, (rr(REG_CTRL) | 0x41) & ~0x80u);

    {
        u32 ral = rr(REG_RAL0), rah = rr(REG_RAH0);
        if (ral || (rah & 0xFFFF)) {
            mac[0] = (u8)ral; mac[1] = (u8)(ral >> 8); mac[2] = (u8)(ral >> 16); mac[3] = (u8)(ral >> 24);
            mac[4] = (u8)rah; mac[5] = (u8)(rah >> 8);
        } else {
            for (i = 0; i < 3; i++) { if (eeprom(i, &w)) { pr("BN: no pude leer la MAC\n"); return 1; } mac[2*i] = (u8)w; mac[2*i+1] = (u8)(w >> 8); }
        }
        imbos_mac();                                /* firma secreta: soy BOS */
        wr(REG_RAL0, (u32)mac[0] | mac[1] << 8 | mac[2] << 16 | (u32)mac[3] << 24);
        wr(REG_RAH0, (u32)mac[4] | mac[5] << 8 | 0x80000000u);
    }
    for (i = 0; i < 128; i++) wr(REG_MTA + i * 4, 0);

    for (i = 0; i < NRX; i++) { rxr[i].addr = net_bank + 0x2000 + (u64)i * RXBUF; rxr[i].status = 0; }
    wr(REG_RDBAL, (u32)net_bank); wr(REG_RDBAH, (u32)(net_bank >> 32));
    wr(REG_RDLEN, NRX * 16); wr(REG_RDH, 0); wr(REG_RDT, NRX - 1);
    rx_tail = 0;
    wr(REG_RCTL, 0x2 | 0x8000 | 0x04000000u);

    for (i = 0; i < NTX; i++) { txr[i].addr = 0; txr[i].status = 1; txr[i].cmd = 0; }
    wr(REG_TDBAL, (u32)(net_bank + 0x1000)); wr(REG_TDBAH, (u32)((net_bank + 0x1000) >> 32));
    wr(REG_TDLEN, NTX * 16); wr(REG_TDH, 0); wr(REG_TDT, 0);
    tx_tail = 0;
    wr(REG_TIPG, 10 | (10 << 10) | (10 << 20));
    wr(REG_TCTL, 0x2 | 0x8 | (0x0F << 4) | (0x40 << 12));
    return 0;
}

static void nic_send(const u8 *data, u32 len)
{
    struct txd *d = &txr[tx_tail];
    int t;
    if (len > RXBUF) len = RXBUF;
    memcpy(txbuf, data, len);
    d->addr = net_bank + 0x2000 + (u64)NRX * RXBUF;
    d->len = (u16)len; d->cso = 0; d->css = 0; d->sp = 0; d->status = 0;
    d->cmd = 0x1 | 0x2 | 0x8;
    __asm__ volatile("" ::: "memory");
    tx_tail = (tx_tail + 1) % NTX;
    wr(REG_TDT, tx_tail);
    for (t = 0; t < 2000000; t++) if (d->status & 0x0F) break;
}
static u8 *nic_recv(u32 *len)
{
    struct rxd *d = &rxr[rx_tail];
    if (!(d->status & 0x1)) return 0;
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
static u16 ip_id = 1;
static u8  frame[RXBUF];

static u16 cksum(const u8 *p, int n, u32 start)
{
    u32 s = start; int i;
    for (i = 0; i + 1 < n; i += 2) s += (u32)p[i] << 8 | p[i + 1];
    if (i < n) s += (u32)p[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}
static int eth_start(const u8 *dst, u16 type)
{
    memcpy(frame, dst, 6); memcpy(frame + 6, mac, 6); wrbe16(frame + 12, type);
    return 14;
}

/* ---- ARP ---- */
static u32 pend_arp_ip; static u8 pend_arp_mac[6]; static int pend_arp_ok;
static void arp_send(u16 op, const u8 *tha, u32 tpa, const u8 *dst)
{
    int o = eth_start(dst, 0x0806);
    u8 *a = frame + o;
    wrbe16(a + 0, 1); wrbe16(a + 2, 0x0800); a[4] = 6; a[5] = 4; wrbe16(a + 6, op);
    memcpy(a + 8, mac, 6); memcpy(a + 14, &my_ip, 4);
    memcpy(a + 18, tha, 6); memcpy(a + 24, &tpa, 4);
    nic_send(frame, o + 28);
}
/* contesta ARPs por nuestra IP y guarda la respuesta que esperamos */
static void arp_see(u8 *p, u32 n)
{
    u8 *a = p + 14; u16 op; u32 spa;
    if (n < 42) return;
    op = rd16(a + 6); memcpy(&spa, a + 14, 4);
    if (op == 2 && pend_arp_ip && spa == pend_arp_ip) { memcpy(pend_arp_mac, a + 8, 6); pend_arp_ok = 1; }
    else if (op == 1) { u32 tpa; memcpy(&tpa, a + 24, 4); if (my_ip && tpa == my_ip) arp_send(2, a + 8, spa, a + 8); }
}
static int arp_resolve(u32 ip, u8 *out)
{
    int try_; u32 t0;
    pend_arp_ip = ip; pend_arp_ok = 0;
    for (try_ = 0; try_ < 4; try_++) {
        arp_send(1, bcast, ip, bcast);
        t0 = K->ms();
        while (K->ms() - t0 < 300) {
            u32 n; u8 *p = nic_recv(&n);
            if (!p) { if (K->poll() == 27) return 1; continue; }
            if (n >= 14 && rd16(p + 12) == 0x0806) arp_see(p, n);
            nic_recv_done();
            if (pend_arp_ok) { memcpy(out, pend_arp_mac, 6); pend_arp_ip = 0; return 0; }
        }
    }
    pend_arp_ip = 0; return 1;
}

/* ---- IPv4 ---- */
static int send_ip(u32 dst, u8 proto, const u8 *payload, int plen)
{
    u8 nh[6]; u8 *ip;
    u32 route = ((dst ^ my_ip) & my_mask) ? my_gw : dst;
    if (route == my_gw && gw_known) memcpy(nh, gw_mac, 6);
    else if (arp_resolve(route, nh)) { pr("BN: no pude resolver "); prip(route); pr(" (ARP)\n"); return 1; }
    else if (route == my_gw) { memcpy(gw_mac, nh, 6); gw_known = 1; }

    int o = eth_start(nh, 0x0800);
    ip = frame + o;
    memset(ip, 0, 20);
    ip[0] = 0x45; wrbe16(ip + 2, (u16)(20 + plen));
    ip[4] = (u8)(ip_id >> 8); ip[5] = (u8)ip_id; ip_id++;
    ip[8] = 64; ip[9] = proto;
    memcpy(ip + 12, &my_ip, 4); memcpy(ip + 16, &dst, 4);
    { u16 c = cksum(ip, 20, 0); ip[10] = (u8)(c >> 8); ip[11] = (u8)c; }
    memcpy(ip + 20, payload, plen);
    nic_send(frame, o + 20 + plen);
    return 0;
}

/* ================================================================= ICMP ping ============ */
static int do_ping(u32 ip)
{
    u8 pkt[64]; int s, got = 0;
    pr("PING "); prip(ip); pr(":\n");
    for (s = 0; s < 4; s++) {
        u32 t0, n; int ok = 0;
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 8; pkt[4] = 0x13; pkt[5] = 0x37; pkt[7] = (u8)s;
        for (int i = 8; i < 40; i++) pkt[i] = (u8)('a' + (i & 15));
        { u16 c = cksum(pkt, 40, 0); pkt[2] = (u8)(c >> 8); pkt[3] = (u8)c; }
        t0 = K->ms();
        if (send_ip(ip, 1, pkt, 40)) return 1;
        while (K->ms() - t0 < 1000 && !ok) {
            u8 *p = nic_recv(&n);
            if (!p) { if (K->poll() == 27) return 1; continue; }
            if (n >= 14 && rd16(p + 12) == 0x0806) arp_see(p, n);
            else if (n >= 34 && rd16(p + 12) == 0x0800) {
                u8 *iph = p + 14; int ihl = (iph[0] & 0xF) * 4;
                if (iph[9] == 1 && iph[ihl] == 0) ok = 1;
            }
            nic_recv_done();
        }
        if (ok) { pr("  respuesta de "); prip(ip); pr("  "); prd(K->ms() - t0); pr(" ms\n"); got++; }
        else pr("  sin respuesta\n");
        K->sleep(300);
    }
    prd(got); pr(" de 4 respondieron\n");
    return got ? 0 : 1;
}

/* ================================================================= UDP + DHCP ============ */
/* manda un UDP unicast; devuelve 0 si pudo enviar */
static int udp_send(u32 dst, u16 sport, u16 dport, const u8 *data, int n)
{
    static u8 seg[1500];
    wrbe16(seg + 0, sport); wrbe16(seg + 2, dport); wrbe16(seg + 4, (u16)(8 + n)); seg[6] = 0; seg[7] = 0;
    memcpy(seg + 8, data, n);
    /* checksum UDP (opcional en IPv4, lo dejamos en 0) */
    return send_ip(dst, 17, seg, 8 + n);
}
/* espera un UDP a nuestro puerto; devuelve puntero al payload y *len, o 0 si vence */
static u8 *udp_wait(u16 to_port, u32 ms, u32 *len)
{
    u32 t0 = K->ms();
    while (K->ms() - t0 < ms) {
        u32 n; u8 *p = nic_recv(&n);
        if (!p) { if (K->poll() == 27) return 0; continue; }
        if (n >= 14 && rd16(p + 12) == 0x0806) { arp_see(p, n); nic_recv_done(); continue; }
        if (n >= 14 + 20 + 8 && rd16(p + 12) == 0x0800) {
            u8 *iph = p + 14; int ihl = (iph[0] & 0xF) * 4; u8 *udp = iph + ihl;
            if (iph[9] == 17 && rd16(udp + 2) == to_port) {
                int ul = rd16(udp + 4);
                static u8 copy[1600];
                if (ul < 8) { nic_recv_done(); continue; }
                if (ul - 8 > (int)sizeof copy) ul = (int)sizeof copy + 8;
                memcpy(copy, udp + 8, ul - 8);
                *len = (u32)(ul - 8);
                nic_recv_done();
                return copy;
            }
        }
        nic_recv_done();
    }
    return 0;
}

static u32 dhcp_xid;
static void dhcp_send(u8 kind, u32 reqip, u32 server)
{
    int o = eth_start(bcast, 0x0800);
    u8 *ip = frame + o, *u = ip + 20, *d = u + 8; int dl;
    memset(d, 0, 300);
    d[0] = 1; d[1] = 1; d[2] = 6;
    memcpy(d + 4, &dhcp_xid, 4); wrbe16(d + 10, 0x8000);
    memcpy(d + 28, mac, 6);
    d[236] = 0x63; d[237] = 0x82; d[238] = 0x53; d[239] = 0x63;
    {
        u8 *op = d + 240;
        *op++ = 53; *op++ = 1; *op++ = kind;
        *op++ = 55; *op++ = 4; *op++ = 1; *op++ = 3; *op++ = 6; *op++ = 51;
        if (reqip) { *op++ = 50; *op++ = 4; memcpy(op, &reqip, 4); op += 4; }
        if (server) { *op++ = 54; *op++ = 4; memcpy(op, &server, 4); op += 4; }
        *op++ = 255; dl = (int)(op - d);
    }
    wrbe16(u + 0, 68); wrbe16(u + 2, 67); wrbe16(u + 4, (u16)(8 + dl)); u[6] = 0; u[7] = 0;
    memset(ip, 0, 20);
    ip[0] = 0x45; wrbe16(ip + 2, (u16)(20 + 8 + dl));
    ip[4] = (u8)(ip_id >> 8); ip[5] = (u8)ip_id; ip_id++;
    ip[8] = 64; ip[9] = 17;
    memset(ip + 12, 0, 4); memset(ip + 16, 0xFF, 4);
    { u16 c = cksum(ip, 20, 0); ip[10] = (u8)(c >> 8); ip[11] = (u8)c; }
    nic_send(frame, o + 20 + 8 + dl);
}
static const u8 *dhcp_opt(const u8 *d, int n, u8 want, int *len)
{
    int i = 240;
    while (i < n && d[i] != 255) {
        if (d[i] == 0) { i++; continue; }
        if (d[i] == want) { *len = d[i + 1]; return d + i + 2; }
        i += 2 + d[i + 1];
    }
    return 0;
}
static int dhcp_reap(u32 ms, u32 *yip, u32 *srv, u32 *mask, u32 *gw, u32 *dns)
{
    u32 n; u8 *d = udp_wait(68, ms, &n);
    int l; const u8 *o;
    if (!d || n < 240 || memcmp(&dhcp_xid, d + 4, 4)) return 0;
    int type = 0;
    o = dhcp_opt(d, n, 53, &l); if (o) type = o[0];
    memcpy(yip, d + 16, 4);
    o = dhcp_opt(d, n, 54, &l); if (o && l == 4) memcpy(srv, o, 4);
    o = dhcp_opt(d, n, 1, &l);  if (o && l == 4) memcpy(mask, o, 4);
    o = dhcp_opt(d, n, 3, &l);  if (o && l >= 4) memcpy(gw, o, 4);
    o = dhcp_opt(d, n, 6, &l);  if (o && l >= 4) memcpy(dns, o, 4);
    return type;
}
static int do_dhcp(void)
{
    u32 yip = 0, srv = 0, mask = 0, gw = 0, dns = 0; int try_;
    dhcp_xid = K->seed() ^ (K->ms() << 8) ^ 0xB0510Full;
    my_ip = 0; gw_known = 0;
    for (try_ = 0; try_ < 4; try_++) { dhcp_send(1, 0, 0); if (dhcp_reap(1200, &yip, &srv, &mask, &gw, &dns) == 2 && yip) break; yip = 0; }
    if (!yip) { pr("BN: nadie contesto el DHCP (no hay servidor en la red)\n"); return 1; }
    for (try_ = 0; try_ < 4; try_++) {
        dhcp_send(3, yip, srv);
        if (dhcp_reap(1200, &yip, &srv, &mask, &gw, &dns) == 5) {
            my_ip = yip; my_mask = mask ? mask : 0x00FFFFFF; my_gw = gw; my_dns = dns; return 0;
        }
    }
    pr("BN: el servidor no confirmo la IP (sin ACK)\n"); return 1;
}
static void show_cfg(void)
{
    pr("  IP:      "); prip(my_ip); pr("\n");
    pr("  mascara: "); prip(my_mask); pr("\n");
    pr("  gateway: "); prip(my_gw); pr("\n");
    pr("  DNS:     "); prip(my_dns); pr("\n");
}

/* ================================================================= DNS ============ */
/* escribe "www.ejemplo.com" como etiquetas 3www7ejemplo3com0; devuelve bytes escritos */
static int dns_name(u8 *o, const char *name)
{
    int w = 0, start = 0, i = 0;
    for (;;) {
        char c = name[i];
        if (c == '.' || c == 0) {
            int lenpos = w; o[w++] = 0;
            int j; for (j = start; j < i; j++) o[w++] = (u8)name[j];
            o[lenpos] = (u8)(i - start);
            if (!c) break;
            start = i + 1;
        }
        i++;
    }
    o[w++] = 0;
    return w;
}
/* salta un nombre DNS (con compresion) empezando en off; devuelve el offset siguiente */
static int dns_skip(const u8 *m, int n, int off)
{
    while (off < n) {
        u8 l = m[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) == 0xC0) return off + 2;
        off += 1 + l;
    }
    return n;
}
static u32 dns_resolve(const char *name)
{
    static u8 q[600];
    u16 id = (u16)(K->seed() ^ K->ms());
    int ql, try_;
    wrbe16(q + 0, id); wrbe16(q + 2, 0x0100);      /* RD */
    wrbe16(q + 4, 1); wrbe16(q + 6, 0); wrbe16(q + 8, 0); wrbe16(q + 10, 0);
    ql = 12 + dns_name(q + 12, name);
    wrbe16(q + ql, 1); wrbe16(q + ql + 2, 1); ql += 4;    /* A, IN */

    for (try_ = 0; try_ < 3; try_++) {
        u32 n; u8 *r;
        if (udp_send(my_dns, 40000 + (id & 0x3FF), 53, q, ql)) return 0;
        r = udp_wait(40000 + (id & 0x3FF), 1500, &n);
        if (!r || n < 12 || rd16(r) != id) continue;
        int qd = rd16(r + 4), an = rd16(r + 6), off = 12, k;
        for (k = 0; k < qd; k++) { off = dns_skip(r, n, off); off += 4; }
        for (k = 0; k < an && off + 10 <= (int)n; k++) {
            int type, rdl;
            off = dns_skip(r, n, off);
            if (off + 10 > (int)n) break;
            type = rd16(r + off); rdl = rd16(r + off + 8);
            off += 10;
            if (type == 1 && rdl == 4) { u32 ip; memcpy(&ip, r + off, 4); return ip; }   /* A */
            off += rdl;                                /* CNAME u otro: seguimos */
        }
        return 0;
    }
    return 0;
}

/* ================================================================= TCP ============ */
static u32 t_peer, t_lport, t_rport, t_snd, t_rcv;
static int t_state;                     /* 0 cerrado, 1 establecido, 2 el otro cerro (FIN) */
static u8 sb[2048]; static int sb_len, sb_pos;   /* buffer de sobra de la lectura en flujo */

static u16 tcp_cksum(const u8 *seg, int n, u32 src, u32 dst)
{
    u32 s = 0; const u8 *a = (const u8 *)&src, *b = (const u8 *)&dst;
    s += (a[0] << 8 | a[1]) + (a[2] << 8 | a[3]);
    s += (b[0] << 8 | b[1]) + (b[2] << 8 | b[3]);
    s += 6; s += n;
    int i; for (i = 0; i + 1 < n; i += 2) s += (u32)seg[i] << 8 | seg[i + 1];
    if (i < n) s += (u32)seg[i] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}
/* manda un segmento TCP con banderas flags y data[dn] */
static int tcp_out(u8 flags, const u8 *data, int dn, int mss_opt)
{
    static u8 seg[1600];
    int hl = mss_opt ? 24 : 20;
    memset(seg, 0, hl);
    wrbe16(seg + 0, (u16)t_lport); wrbe16(seg + 2, (u16)t_rport);
    wrbe32(seg + 4, t_snd); wrbe32(seg + 8, t_rcv);
    seg[12] = (u8)((hl / 4) << 4); seg[13] = flags;
    wrbe16(seg + 14, 64240);
    if (mss_opt) { seg[20] = 2; seg[21] = 4; wrbe16(seg + 22, 1460); }
    if (dn) memcpy(seg + hl, data, dn);
    { u16 c = tcp_cksum(seg, hl + dn, my_ip, t_peer); seg[16] = (u8)(c >> 8); seg[17] = (u8)c; }
    return send_ip(t_peer, 6, seg, hl + dn);
}
/* espera un segmento TCP de nuestra conexion; devuelve puntero al segmento (dentro de rx) y *seglen (TCP+datos),
 * o 0 si vence. El llamante NO hace nic_recv_done (ya se hizo copiando). */
static u8 tcpseg[1600];
static int tcp_in(u32 ms, int *seglen)
{
    u32 t0 = K->ms();
    while (K->ms() - t0 < ms) {
        u32 n; u8 *p = nic_recv(&n);
        if (!p) { if (K->poll() == 27) return 0; continue; }
        if (n >= 14 && rd16(p + 12) == 0x0806) { arp_see(p, n); nic_recv_done(); continue; }
        if (n >= 34 && rd16(p + 12) == 0x0800) {
            u8 *iph = p + 14; int ihl = (iph[0] & 0xF) * 4, tl = rd16(iph + 2) - ihl;
            u32 src; memcpy(&src, iph + 12, 4);
            if (iph[9] == 6 && src == t_peer) {
                u8 *tcp = iph + ihl;
                if (rd16(tcp) == t_rport && rd16(tcp + 2) == t_lport && tl > 0 && tl <= (int)sizeof tcpseg) {
                    memcpy(tcpseg, tcp, tl); *seglen = tl; nic_recv_done(); return 1;
                }
            }
        }
        nic_recv_done();
    }
    return 0;
}
static int tcp_connect(u32 ip, u16 port)
{
    int try_, sl;
    t_peer = ip; t_rport = port; t_lport = 20000 + (K->seed() & 0x3FFF);
    t_snd = K->seed() ^ (K->ms() << 3); t_rcv = 0; t_state = 0;
    sb_len = sb_pos = 0;
    for (try_ = 0; try_ < 5; try_++) {
        if (tcp_out(0x02, 0, 0, 1)) return 1;       /* SYN */
        while (tcp_in(800, &sl)) {
            u8 fl = tcpseg[13];
            if ((fl & 0x04)) return 1;              /* RST: nadie escucha */
            if ((fl & 0x12) == 0x12) {              /* SYN|ACK */
                t_rcv = rd32(tcpseg + 4) + 1;
                t_snd += 1;
                if (rd32(tcpseg + 8) != t_snd) continue;
                tcp_out(0x10, 0, 0, 0);             /* ACK */
                t_state = 1;
                return 0;
            }
        }
    }
    return 1;
}
static int tcp_send(const u8 *data, int n)
{
    int sl;
    if (tcp_out(0x18, data, n, 0)) return 1;        /* PSH|ACK */
    t_snd += n;
    /* esperamos el ACK (si no llega, el servidor igual nos mandara la respuesta) */
    { u32 t0 = K->ms(); while (K->ms() - t0 < 500) { if (!tcp_in(200, &sl)) break; if (rd32(tcpseg + 8) >= t_snd && (tcpseg[13] & 0x01) == 0) break; if (tcpseg[13] & 0x01) { t_rcv = rd32(tcpseg + 4) + 1; } } }
    return 0;
}
/* recibe hasta max bytes en out; devuelve cuantos (hasta que el otro cierra o vence) */
static int tcp_recv(u8 *out, int max)
{
    int got = 0, sl, idle = 0;
    while (idle < 15) {                             /* ~ varios segundos sin datos => fin */
        if (!tcp_in(400, &sl)) { idle++; continue; }
        idle = 0;
        int thl = (tcpseg[12] >> 4) * 4, dn = sl - thl;
        u8 fl = tcpseg[13];
        u32 seq = rd32(tcpseg + 4);
        if (dn > 0 && seq == t_rcv) {
            if (got + dn > max) dn = max - got;
            memcpy(out + got, tcpseg + thl, dn);
            got += dn; t_rcv += (sl - thl);
            tcp_out(0x10, 0, 0, 0);                 /* ACK */
        } else if (dn > 0) {
            tcp_out(0x10, 0, 0, 0);                 /* fuera de orden: re-ACK */
        }
        if (fl & 0x01) {                            /* FIN */
            t_rcv += 1; tcp_out(0x10, 0, 0, 0); t_state = 2; break;
        }
        if (fl & 0x04) { t_state = 0; break; }      /* RST */
        if (got >= max) break;
    }
    return got;
}
static void tcp_close(void)
{
    int sl;
    if (t_state == 1) { tcp_out(0x11, 0, 0, 0); t_snd += 1; tcp_in(400, &sl); }  /* FIN|ACK */
    t_state = 0;
}

/* ---- lectura/escritura en flujo (lo usa el TLS) ---- */
static int tcp_stream_read(u8 *out, int max)
{
    int sl;
    if (sb_pos < sb_len) { int t = sb_len - sb_pos; if (t > max) t = max; memcpy(out, sb + sb_pos, t); sb_pos += t; return t; }
    if (t_state == 2) return 0;
    for (;;) {
        if (!tcp_in(8000, &sl)) return -1;
        int thl = (tcpseg[12] >> 4) * 4, dn = sl - thl; u8 fl = tcpseg[13]; u32 seq = rd32(tcpseg + 4);
        if (fl & 0x04) { t_state = 0; return 0; }               /* RST */
        if (dn > 0 && seq == t_rcv) {
            t_rcv += dn;
            if (fl & 0x01) { t_rcv += 1; t_state = 2; }
            tcp_out(0x10, 0, 0, 0);
            if (dn > max) { memcpy(out, tcpseg + thl, max); sb_len = dn - max; memcpy(sb, tcpseg + thl + max, sb_len); sb_pos = 0; return max; }
            memcpy(out, tcpseg + thl, dn); return dn;
        }
        if (dn > 0) tcp_out(0x10, 0, 0, 0);                     /* fuera de orden: re-ACK */
        if (fl & 0x01) { t_rcv += 1; tcp_out(0x10, 0, 0, 0); t_state = 2; return 0; }
    }
}
static int tcp_stream_write(const u8 *data, int n)
{
    int off = 0;
    while (off < n) { int c = n - off; if (c > 1400) c = 1400; if (tcp_out(0x18, data + off, c, 0)) return -1; t_snd += c; off += c; }
    return 0;
}
static int io_send(void *x, const u8 *p, int n) { (void)x; return tcp_stream_write(p, n) ? -1 : n; }
static int io_recv(void *x, u8 *p, int n) { (void)x; return tcp_stream_read(p, n); }
static tls_conn tlsc;

/* ================================================================= HTTP ============ */
/* separa "http://host[:port]/ruta" ; devuelve 0 si ok */
static int url_parse(const char *url, char *host, int hmax, u16 *port, const char **path, int *tls)
{
    int i = 0;
    *tls = 0; *port = 80;
    if (istarts(url, "https://")) { url += 8; *tls = 1; *port = 443; }
    else if (istarts(url, "http://")) url += 7;
    while (url[i] && url[i] != '/' && url[i] != ':' && i < hmax - 1) { host[i] = url[i]; i++; }
    host[i] = 0;
    if (url[i] == ':') { u16 p = 0; i++; while (url[i] >= '0' && url[i] <= '9') { p = (u16)(p * 10 + (url[i] - '0')); i++; } *port = p ? p : *port; }
    *path = url[i] ? url + i : "/";
    return host[0] ? 0 : 1;
}
static u32 parse_ip(const char *s)
{
    u32 ip = 0; int part = 0, val = 0, dig = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); dig = 1; if (val > 255) return 0; }
        else if (*s == '.' || *s == 0 || *s == '/' || *s == ':') {
            if (!dig || part > 3) return 0;
            ip |= (u32)val << (part * 8); part++; val = 0; dig = 0;
            if (*s != '.') break;
        } else return 0;
    }
    return part == 4 ? ip : 0;
}
/* host -> IP: si es a.b.c.d se usa tal cual, si no se resuelve por DNS */
static u32 host_ip(const char *host)
{
    u32 ip = parse_ip(host);
    if (ip) return ip;
    pr("DNS "); pr(host); pr(" -> ");
    ip = dns_resolve(host);
    if (ip) { prip(ip); pr("\n"); } else pr("(no resolvio)\n");
    return ip;
}
static int do_get(const char *url)
{
    char host[128]; const char *path; u16 port; u32 ip; int use_tls;
    static char req[400];
    int rl = 0, i, bodyn;
    if (url_parse(url, host, sizeof host, &port, &path, &use_tls)) return 1;
    ip = host_ip(host);
    if (!ip) return 1;
    pr("Conectando a "); prip(ip); pr(":"); prd(port); pr(use_tls ? " (TLS) ...\n" : " ...\n");
    if (tcp_connect(ip, port)) { pr("BN: no pude conectar (TCP)\n"); return 1; }

    if (use_tls) {
        tlsc.log = pr;
        tls_seed(K->seed() ^ (K->ms() << 11) ^ ((u32)mac[5] << 3) ^ (u32)mac[4]);
        if (tls_connect(&tlsc, 0, io_send, io_recv, host)) { pr("BN: fallo el handshake TLS\n"); tcp_close(); return 1; }
        pr("TLS 1.3 listo (AES-128-GCM, X25519). OJO: el certificado aun no se verifica (parte 3c).\n");
    }

    { const char *s = "GET "; while (*s) req[rl++] = *s++; }
    { const char *s = path; while (*s) req[rl++] = *s++; }
    { const char *s = " HTTP/1.1\r\nHost: "; while (*s) req[rl++] = *s++; }
    { const char *s = host; while (*s) req[rl++] = *s++; }
    { const char *s = "\r\nUser-Agent: BN/1\r\nConnection: close\r\n\r\n"; while (*s) req[rl++] = *s++; }

    if (use_tls) {
        if (tls_write(&tlsc, (u8 *)req, rl)) { pr("BN: fallo al enviar (TLS)\n"); tcp_close(); return 1; }
        bodyn = 0;
        for (;;) { int r = tls_read(&tlsc, httpbuf + bodyn, HTTPMAX - 1 - bodyn); if (r <= 0) break; bodyn += r; if (bodyn >= (int)HTTPMAX - 1) break; }
    } else {
        if (tcp_send((u8 *)req, rl)) { pr("BN: fallo al enviar\n"); tcp_close(); return 1; }
        bodyn = tcp_recv(httpbuf, HTTPMAX - 1);
    }
    tcp_close();
    httpbuf[bodyn] = 0;
    if (bodyn <= 0) { pr("BN: sin respuesta\n"); return 1; }

    /* cabecera / cuerpo */
    {
        int hs = -1;
        for (i = 0; i + 3 < bodyn; i++)
            if (httpbuf[i] == '\r' && httpbuf[i+1] == '\n' && httpbuf[i+2] == '\r' && httpbuf[i+3] == '\n') { hs = i + 4; break; }
        /* primera linea de estado */
        pr("--- ");
        for (i = 0; i < bodyn && httpbuf[i] != '\r' && i < 60; i++) { char c[2] = { (char)httpbuf[i], 0 }; pr(c); }
        pr(" ---\n");
        if (hs < 0) hs = 0;
        /* imprime el cuerpo (hasta ~4 KB para no inundar la consola) */
        {
            int shown = bodyn - hs, lim = shown > 4096 ? 4096 : shown, j;
            for (j = 0; j < lim; j++) { char c[2] = { (char)httpbuf[hs + j], 0 }; pr(c); }
            if (shown > lim) { pr("\n[...]\n"); }
        }
        pr("\n("); prd((u32)(bodyn - hs)); pr(" bytes de cuerpo, "); prd((u32)bodyn); pr(" en total)\n");
    }
    return 0;
}

/* ================================================================= entrada ============ */
int mod_main(const struct kapi *k)
{
    const char *a;
    K = k;
    a = K->args ? K->args : "";
    while (*a == ' ') a++;

    if (nic_init()) return 1;
    pr("BN: tarjeta e1000\n  MAC real:  "); prmac(real_mac);
    pr("\n  MAC IMBOS: "); prmac(mac); pr("  (soy BOS)\n");
    if (ieq(a, "mac")) return 0;

    if (do_dhcp()) { K->ram(RAM_FREE, net_bank, 0, 0); return 1; }
    pr("DHCP:\n"); show_cfg();

    if (istarts(a, "ping")) {
        const char *h = a + 4; u32 ip; while (*h == ' ') h++;
        ip = parse_ip(h);
        if (!ip) pr("BN: 'ping' necesita una IP: BN \"ping 10.0.2.2\"\n"); else do_ping(ip);
    } else if (istarts(a, "dns")) {
        const char *h = a + 3; while (*h == ' ') h++;
        if (!*h) pr("BN: 'dns' necesita un nombre: BN \"dns ejemplo.com\"\n");
        else { u32 ip = dns_resolve(h); pr(h); pr(" -> "); if (ip) { prip(ip); pr("\n"); } else pr("(no resolvio)\n"); }
    } else if (istarts(a, "get") || istarts(a, "http")) {
        const char *u = a + (istarts(a, "http") && !istarts(a, "get") ? 0 : 3);
        while (*u == ' ') u++;
        if (!*u) pr("BN: 'get' necesita una URL: BN \"get http://10.0.2.2:8080/\"\n");
        else do_get(u);
    }

    K->ram(RAM_FREE, net_bank, 0, 0);
    return 0;
}
