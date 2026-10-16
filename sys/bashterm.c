/* bashterm.c - BashTerminal (modulo /SYS/BASHTERM.APP)
 *
 * PC BASIC carga un kernel Linux real (BZIMAGE) y su initramfs (INITRD.GZ) desde la particion FAT
 * del mismo disco/USB y le entrega la maquina con el protocolo de arranque x86 de 64 bits:
 * misma RAM (mapa E820), misma CPU, misma pantalla (el framebuffer VBE que ya dejo configurado
 * la BIOS) y el mismo disco, que Linux monta en /mnt. Dentro corre bash estatico + busybox.
 * No hay vuelta a PC BASIC: se reinicia la maquina.
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);

static const struct kapi *K;

struct bootinfo {                       /* igual que kernel.h: lo deja stage2.asm en 0x6000 */
    u32 magic;
    u16 w, h, pitch;
    u8  bpp, gfx;
    u32 fb;
    u8  rs, rp, gs, gp, bs, bp;
    u16 e820n;
    u16 extkb;
} __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));

#define BI   ((const struct bootinfo *)0x6000)
#define E8   ((const struct e820 *)0x6020)

#define TMP_ADDR   0x1000000u           /* aqui se lee BZIMAGE completo (max 14 MB) */
#define TMP_MAX    0xE00000
#define KLOAD      0x2000000u           /* nucleo protegido, alineado a 2 MB */
#define BP_ADDR    0x90000u             /* boot_params (zero page) */
#define CMD_ADDR   0x91000u
#ifdef TCMODE                           /* Tiny Core Linux (CorePlus, 32 bits): instalacion "frugal" en la particion FAT */
#define CFGNAME    "TINYCORE.CFG"
#define DEF_KERNEL "BOOT/VMLINUZ"
#define DEF_INITRD "BOOT/CORE.GZ"
#define DEF_CMD    "loglevel=3 cde showapps desktop=flwm_topside kmap=qwerty/la-latin1 waitusb=10 tz=CST6 host=basic-os"
#else                                   /* BashTerminal: kernel propio + bash */
#define CFGNAME    "BASHTERM.CFG"
#define DEF_KERNEL "BZIMAGE"
#define DEF_INITRD "INITRD.GZ"
#define DEF_CMD    "console=ttyS0,115200 console=tty0 rdinit=/init loglevel=3"
#endif

static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { wr16(p, v); wr16(p + 2, v >> 16); }

static const char *const where[] = { "HD0.1:", "HD1.1:", "HD2.1:", "HD0:", "HD1:", "FD1:", "FD0:" };

static int load(const char *name, void *dst, int max, unsigned *total)
{
    char spec[40];
    int i, j, k, r;
    for (i = 0; i < (int)(sizeof where / sizeof where[0]); i++) {
        for (j = 0; where[i][j]; j++)
            spec[j] = where[i][j];
        for (k = 0; name[k]; k++)
            spec[j + k] = name[k];
        spec[j + k] = 0;
        r = K->read(spec, (char *)dst, max, total);
        if (r > 0)
            return r;
    }
    return -1;
}

static struct { u16 limit; u64 base; } __attribute__((packed)) gdtr;
static u64 gdt[4];

static void handoff(u64 entry, u64 bp)
{
    gdt[0] = 0;
    gdt[1] = 0;
    gdt[2] = 0x00AF9A000000FFFFULL;     /* __BOOT_CS = 0x10: codigo de 64 bits */
    gdt[3] = 0x00CF92000000FFFFULL;     /* __BOOT_DS = 0x18 */
    gdtr.limit = sizeof gdt - 1;
    gdtr.base = (u64)gdt;
    __asm__ volatile(
        "cli\n\t"
        "lgdt %0\n\t"
        "mov $0x18, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x10\n\t"
        "lea 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "jmp *%%rbx\n\t"
        : : "m"(gdtr), "b"(entry), "S"(bp) : "rax", "memory");
    for (;;) ;
}


/* ---- entrada de 32 bits: vuelve de modo largo a modo protegido sin paginacion ---- */
u32 h32_entry, h32_bp;
static u64 gdt32[4] = { 0, 0, 0x00CF9A000000FFFFULL, 0x00CF92000000FFFFULL };
struct { u16 limit; u32 base; } __attribute__((packed)) gdtr32;

__asm__(
    ".text\n"
    ".globl h32_go\n"
    "h32_go:\n"
    "    cli\n"
    "    lgdt gdtr32(%rip)\n"
    "    pushq $0x10\n"
    "    leaq h32_pm(%rip), %rax\n"
    "    pushq %rax\n"
    "    lretq\n"
    ".code32\n"
    "h32_pm:\n"
    "    movw $0x18, %ax\n"
    "    movw %ax, %ds\n"
    "    movw %ax, %es\n"
    "    movw %ax, %ss\n"
    "    movw %ax, %fs\n"
    "    movw %ax, %gs\n"
    "    movl %cr0, %eax\n"
    "    andl $0x7FFFFFFF, %eax\n"          /* sin paginacion */
    "    movl %eax, %cr0\n"
    "    movl $0xC0000080, %ecx\n"          /* EFER.LME = 0 */
    "    rdmsr\n"
    "    andl $0xFFFFFEFF, %eax\n"
    "    wrmsr\n"
    "    movl %cr4, %eax\n"
    "    andl $0xFFFFFF5F, %eax\n"          /* PAE y PGE fuera */
    "    movl %eax, %cr4\n"
    "    xorl %ebp, %ebp\n"
    "    xorl %edi, %edi\n"
    "    xorl %ebx, %ebx\n"
    "    movl h32_bp, %esi\n"
    "    movl h32_entry, %eax\n"
    "    jmp *%eax\n"
    ".code64\n");
extern void h32_go(void);

/* BASHTERM.CFG / TINYCORE.CFG (opcional, en la particion FAT): kernel=RUTA, initrd=RUTA, cmdline=TEXTO */
static char cfg_kernel[64] = DEF_KERNEL, cfg_initrd[64] = DEF_INITRD, cfg_cmd[512] = DEF_CMD;

static void cfg_load(void)
{
    static char buf[1024];
    unsigned tot = 0;
    int n = load(CFGNAME, buf, sizeof buf - 1, &tot), i = 0;
    if (n <= 0)
        return;
    buf[n] = 0;
    while (i < n) {
        char *line = buf + i, *val = 0, *dst = 0;
        int max = 0, j = 0;
        while (i < n && buf[i] != '\n' && buf[i] != '\r')
            i++;
        while (i < n && (buf[i] == '\n' || buf[i] == '\r'))
            buf[i++] = 0;
        for (j = 0; line[j]; j++)
            if (line[j] == '=') { line[j] = 0; val = line + j + 1; break; }
        if (!val)
            continue;
        if (line[0] == 'k') { dst = cfg_kernel; max = sizeof cfg_kernel; }
        else if (line[0] == 'i') { dst = cfg_initrd; max = sizeof cfg_initrd; }
        else if (line[0] == 'c') { dst = cfg_cmd; max = sizeof cfg_cmd; }
        if (dst) {
            for (j = 0; val[j] && j < max - 1; j++)
                dst[j] = val[j];
            dst[j] = 0;
        }
    }
}

int mod_main(const struct kapi *api)
{
    u8 *bz = (u8 *)TMP_ADDR, *bp = (u8 *)BP_ADDR;
    unsigned total = 0, rtotal = 0;
    int n, rn, i, ne = 0;
    u32 setup_sects, pm_off, pm_size, init_size, align, ird, rdsz;
    u64 ram_end = 0;
    int is64;

    K = api;
#ifdef TCMODE
    K->print("\nTINY CORE LINUX SOBRE BASIC-OS (INSTALACION FRUGAL EN LA PARTICION FAT)\n");
#else
    K->print("\nBASHTERMINAL: LINUX + BASH SOBRE ESTA MAQUINA\n");
#endif
    cfg_load();
    K->print("BUSCANDO EL KERNEL EN LA PARTICION FAT DEL DISCO...\n");
    n = load(cfg_kernel, bz, TMP_MAX, &total);
    if (n < 0x2000 || total > TMP_MAX) {
        K->print("NO ENCUENTRO BZIMAGE (O NO CABE). USA EL DISCO PC-BASIC-LINUX.IMG\n");
        return 0;
    }
    is64 = rd16(bz + 0x236) & 1;
    if (rd32(bz + 0x202) != 0x53726448u || rd16(bz + 0x206) < 0x020C || bz[0x234] != 1) {
        K->print("EL KERNEL NO ES UN LINUX X86 RELOCABLE (PROTOCOLO >= 2.12)\n");
        return 0;
    }
    setup_sects = bz[0x1F1] ? bz[0x1F1] : 4;
    pm_off = (setup_sects + 1) * 512;
    pm_size = (u32)n - pm_off;
    init_size = rd32(bz + 0x260);
    align = rd32(bz + 0x230);
    if (align > 0x200000u || (KLOAD & (align - 1))) {
        K->print("ALINEACION DEL KERNEL NO SOPORTADA\n");
        return 0;
    }
    ird = (KLOAD + init_size + 0x1FFFFFu) & ~0xFFFFFu;
    ird += 0x100000u;
    K->print("CARGANDO KERNEL...\n");
    memcpy((void *)KLOAD, bz + pm_off, pm_size);

    K->print("CARGANDO EL INITRD...\n");
    rn = load(cfg_initrd, (void *)(u64)ird, 0x1000000, &rtotal);
    if (rn <= 0 || rtotal > 0x1000000u) {
        K->print("NO ENCUENTRO EL INITRD\n");
        return 0;
    }
    rdsz = (u32)rn;

    for (i = 0; i < BI->e820n; i++)
        if (E8[i].type == 1 && E8[i].base <= KLOAD && E8[i].base + E8[i].len > KLOAD)
            ram_end = E8[i].base + E8[i].len;
    if (ram_end < (u64)ird + rdsz + 0x100000u) {
        K->print("POCA RAM: LINUX NECESITA ~64 MB LIBRES\n");
        return 0;
    }

    /* ---- boot_params ---- */
    memset(bp, 0, 4096);
    memcpy(bp + 0x1F1, bz + 0x1F1, (u32)(0x202 + bz[0x201]) - 0x1F1);
    bp[0x210] = 0xFF;                           /* cargador sin id */
    bp[0x211] = (u8)((bz[0x211] | 0x01) & ~0x60);   /* LOADED_HIGH; no QUIET/KEEP_SEGMENTS */
    wr16(bp + 0x1FA, 0xFFFF);                   /* vid_mode normal */
    wr32(bp + 0x218, ird);                      /* ramdisk_image / size */
    wr32(bp + 0x21C, rdsz);
    {
        const char *c = cfg_cmd;
        u8 *d = (u8 *)CMD_ADDR;
        while ((*d++ = (u8)*c++)) ;
        wr32(bp + 0x228, CMD_ADDR);
    }
    for (i = 0; i < BI->e820n && ne < 128; i++) {   /* mapa de memoria E820 */
        u8 *e = bp + 0x2D0 + ne * 20;
        memcpy(e, &E8[i].base, 8);
        memcpy(e + 8, &E8[i].len, 8);
        wr32(e + 16, E8[i].type);
        ne++;
    }
    bp[0x1E8] = (u8)ne;
    if (BI->gfx) {                              /* screen_info: el framebuffer que ya dejo la BIOS */
        wr16(bp + 0x0F, 0x23);                  /* orig_video_isVGA = VIDEO_TYPE_VLFB */
        bp[0x0F] = 0x23;
        wr16(bp + 0x12, BI->w);
        wr16(bp + 0x14, BI->h);
        wr16(bp + 0x16, BI->bpp);
        wr32(bp + 0x18, BI->fb);
        wr32(bp + 0x1C, ((u32)BI->pitch * BI->h + 0xFFFF) >> 16);   /* en bloques de 64 KB */
        wr16(bp + 0x24, BI->pitch);
        bp[0x26] = BI->rs; bp[0x27] = BI->rp;
        bp[0x28] = BI->gs; bp[0x29] = BI->gp;
        bp[0x2A] = BI->bs; bp[0x2B] = BI->bp;
        bp[0x06] = 0x03;                        /* orig_video_mode: texto (no lo usa vesafb) */
    }

    K->print("ENTREGANDO LA MAQUINA A LINUX...\n");
    K->sleep(300);
    if (is64) {
        handoff((u64)KLOAD + 0x200, BP_ADDR);
    } else {                                    /* kernel de 32 bits: arranca en el inicio del nucleo protegido */
        gdtr32.limit = sizeof gdt32 - 1;
        gdtr32.base = (u32)(u64)gdt32;
        h32_entry = KLOAD;
        h32_bp = BP_ADDR;
        h32_go();
    }
    return 0;
}
