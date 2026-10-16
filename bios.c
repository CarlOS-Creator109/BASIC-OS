/* bios.c - llamadas a la BIOS (INT 13h) desde modo largo, via thunk.asm */
#include "kernel.h"
#include "store.h"

extern const u8 thunk_blob[], thunk_end[];

#define TH_AX  (*(volatile u16 *)0x8F00)
#define TH_BX  (*(volatile u16 *)0x8F02)
#define TH_CX  (*(volatile u16 *)0x8F04)
#define TH_DX  (*(volatile u16 *)0x8F06)
#define TH_ES  (*(volatile u16 *)0x8F08)
#define TH_RAX (*(volatile u16 *)0x8F0A)
#define TH_CF  (*(volatile u8 *)0x8F0C)
#define TH_SI  (*(volatile u16 *)0x8F0E)
#define TH_RBX (*(volatile u16 *)0x8F20)
#define TH_RCX (*(volatile u16 *)0x8F22)
#define TH_RDX (*(volatile u16 *)0x8F24)

static int thunk_ready;

/* Ejecuta INT 13h con los registros de r. Devuelve el acarreo (1 = error) y deja los
 * registros de salida en r. DS siempre es 0; ES = segmento del buffer de rebote. */
int bios_int13(struct bregs *r)
{
    u8 mask;

    if (!thunk_ready) {
        memcpy((void *)0x8000, thunk_blob, (size_t)(thunk_end - thunk_blob));
        thunk_ready = 1;
    }
    TH_AX = r->ax;
    TH_BX = r->bx;
    TH_CX = r->cx;
    TH_DX = r->dx;
    TH_SI = r->si;
    TH_ES = BOUNCE_SEG;
    mask = inb(0x21);
    outb(0x21, mask | 0x02);                  /* la BIOS no debe robar teclas (IRQ1) */
    ((void (*)(void))0x8000)();
    outb(0x21, mask);
    r->ax = TH_RAX;
    r->bx = TH_RBX;
    r->cx = TH_RCX;
    r->dx = TH_RDX;
    return TH_CF;
}

/* ---- sector suelto de la unidad de arranque (lo usa SAVE/LOAD, store.c) ---- */

static int disk_sector(int write, unsigned int lba, void *buf)
{
    u8 drive = *(volatile u8 *)0x5FF0;
    unsigned int heads = *(volatile u16 *)0x5FF2, spt = *(volatile u16 *)0x5FF4;
    unsigned int cyl, t, h, s;
    int tries, r = 1;
    struct bregs g;

    if (!heads || !spt)
        return 1;
    cyl = lba / (spt * heads);
    t = lba % (spt * heads);
    h = t / spt;
    s = t % spt + 1;
    if (cyl > 1023)
        return 1;
    if (write)
        memcpy((void *)BOUNCE_BUF, buf, 512);
    for (tries = 0; tries < 3 && r; tries++) {
        g.ax = (u16)(((write ? 0x03 : 0x02) << 8) | 1);
        g.bx = 0;
        g.cx = (u16)(((cyl & 0xFF) << 8) | ((cyl >> 2) & 0xC0) | s);
        g.dx = (u16)((h << 8) | drive);
        g.si = 0;
        r = bios_int13(&g);
        if (r) {
            g.ax = 0;
            g.dx = drive;
            bios_int13(&g);                   /* reiniciar y reintentar */
        }
    }
    if (!r && !write)
        memcpy(buf, (void *)BOUNCE_BUF, 512);
    return r;
}

int plat_disk_read(unsigned int lba, void *buf)        { return disk_sector(0, lba, buf); }
int plat_disk_write(unsigned int lba, const void *buf) { return disk_sector(1, lba, (void *)buf); }
