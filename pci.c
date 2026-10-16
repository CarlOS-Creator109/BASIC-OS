/* pci.c - enumeracion PCI (mecanismo 1: puertos 0xCF8/0xCFC). Base de los drivers. */
#include "kernel.h"

#define MAXDEV 64

struct pci_dev {
    u8 bus, dev, fn;
    u16 vendor, device;
    u8 class, sub, prog;
};

static struct pci_dev devs[MAXDEV];
static int ndev = -1;

static void outl(u16 port, u32 v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static u32 inl(u16 port)
{
    u32 r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

u32 pci_read32(int bus, int dev, int fn, int off)
{
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (u32)(off & 0xFC));
    return inl(0xCFC);
}

void pci_write32(int bus, int dev, int fn, int off, u32 v)
{
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (u32)(off & 0xFC));
    outl(0xCFC, v);
}

static void scan(void);

/* n-esimo dispositivo con esa clase/subclase/prog-if (prog = -1: cualquiera). 1 = encontrado */
int pci_find(int class, int sub, int prog, int nth, int *bus, int *dev, int *fn)
{
    int i;
    if (ndev < 0)
        scan();
    for (i = 0; i < ndev; i++)
        if (devs[i].class == class && devs[i].sub == sub && (prog < 0 || devs[i].prog == prog) && nth-- == 0) {
            *bus = devs[i].bus; *dev = devs[i].dev; *fn = devs[i].fn;
            return 1;
        }
    return 0;
}

static void scan(void)
{
    int b, d, f;
    ndev = 0;
    for (b = 0; b < 256 && ndev < MAXDEV; b++)
        for (d = 0; d < 32 && ndev < MAXDEV; d++)
            for (f = 0; f < 8 && ndev < MAXDEV; f++) {
                u32 id = pci_read32(b, d, f, 0), cl;
                if ((id & 0xFFFF) == 0xFFFF) {
                    if (f == 0)
                        break;                  /* sin dispositivo: saltar sus funciones */
                    continue;
                }
                cl = pci_read32(b, d, f, 8);
                devs[ndev].bus = (u8)b; devs[ndev].dev = (u8)d; devs[ndev].fn = (u8)f;
                devs[ndev].vendor = (u16)id; devs[ndev].device = (u16)(id >> 16);
                devs[ndev].class = (u8)(cl >> 24); devs[ndev].sub = (u8)(cl >> 16);
                devs[ndev].prog = (u8)(cl >> 8);
                ndev++;
                if (f == 0 && !(pci_read32(b, d, f, 0x0C) & 0x00800000))
                    break;                      /* no es multifuncion */
            }
}

static const char *vendor_name(u16 v)
{
    switch (v) {
    case 0x8086: return "INTEL";
    case 0x1002: return "AMD/ATI";
    case 0x1022: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x10EC: return "REALTEK";
    case 0x1969: return "ATHEROS";
    case 0x14E4: return "BROADCOM";
    case 0x168C: return "QUALCOMM ATH";
    case 0x1B21: return "ASMEDIA";
    case 0x1106: return "VIA";
    case 0x1234: return "QEMU";
    case 0x1AF4: return "VIRTIO";
    case 0x15AD: return "VMWARE";
    case 0x80EE: return "VIRTUALBOX";
    default:     return "?";
    }
}

static const char *class_name(u8 c, u8 s, u8 p)
{
    switch (c) {
    case 0x01:
        if (s == 0x01) return "IDE";
        if (s == 0x06) return p == 1 ? "SATA AHCI" : "SATA";
        if (s == 0x08) return "NVME";
        if (s == 0x00) return "SCSI";
        return "STORAGE";
    case 0x02: return "ETHERNET";
    case 0x03: return "VGA";
    case 0x04: return s == 0x03 ? "AUDIO HDA" : "MULTIMEDIA";
    case 0x06: return "BRIDGE";
    case 0x0C:
        if (s == 0x03) {
            if (p == 0x00) return "USB UHCI";
            if (p == 0x10) return "USB OHCI";
            if (p == 0x20) return "USB EHCI";
            if (p == 0x30) return "USB XHCI";
            return "USB";
        }
        return "SERIAL BUS";
    case 0x0D: return "WIRELESS";
    default:   return "OTHER";
    }
}

static char *hex(char *o, u32 v, int digits)
{
    int i;
    for (i = digits - 1; i >= 0; i--)
        *o++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return o;
}

static char *str(char *o, const char *s)
{
    while (*s)
        *o++ = *s++;
    return o;
}

/* Linea de texto del dispositivo i; 0 si no hay mas */
int pci_line(int i, char *out)
{
    const struct pci_dev *d;
    char *o = out;

    if (ndev < 0)
        scan();
    if (i < 0 || i >= ndev)
        return 0;
    d = &devs[i];
    o = hex(o, d->bus, 2); *o++ = ':';
    o = hex(o, d->dev, 2); *o++ = '.';
    o = hex(o, d->fn, 1);  *o++ = ' ';
    o = hex(o, d->vendor, 4); *o++ = ':';
    o = hex(o, d->device, 4); *o++ = ' ';
    o = str(o, class_name(d->class, d->sub, d->prog));
    *o++ = ' ';
    o = str(o, vendor_name(d->vendor));
    *o = 0;
    return 1;
}
