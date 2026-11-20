/* irq.c - interrupciones: IDT, excepciones con pantalla de error, reloj PIT a 1000 Hz y preempcion de tareas
 *
 * Hasta ahora todo se leia por sondeo y la CPU nunca descansaba (98% en el htop del anfitrion). Con esto:
 *   - una excepcion (division entre cero, fallo de pagina...) muestra que paso en vez de reiniciar la maquina;
 *   - el PIT da 1000 interrupciones por segundo: los bucles que esperan (teclado, K->sleep, el escritorio) hacen
 *     "hlt" y la CPU duerme hasta la siguiente;
 *   - cada 10 ms, si la tarea interrumpida esta corriendo codigo de un programa (direccion >= 0x9000000, nunca el
 *     kernel) y hay otra tarea lista, se le quita la CPU (multitarea preemptiva "con candado grande": el codigo del
 *     kernel nunca se interrumpe a medias, asi no hacen falta candados en todos lados).
 *
 * El reloj usa su propia pila (IST1 del TSS): asi no pisa la "zona roja" de 128 bytes que el codigo de gcc normal
 * (FFmpeg de MPLAY) usa debajo de rsp. Si hay cambio de tarea, el marco se copia a la pila de la tarea, debajo de
 * su zona roja, y desde ahi se cambia; al volver se hace iretq desde ahi.
 *
 * Las llamadas a la BIOS (bios.c) regresan el PIC, el PIT y la IVT a como los espera la BIOS y luego los vuelven
 * a poner (irq_bios_enter / irq_bios_leave). Solo en 64 bits; en 32 bits todo sigue por sondeo.
 */
#include "kernel.h"

volatile u64 irq_ticks;
static int irq_on __attribute__((unused));

#ifdef __x86_64__

struct frame {                                  /* lo que dejan los stubs en la pila */
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 rip, cs, rflags, rsp, ss;
};
struct xframe {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 vec, err, rip, cs, rflags, rsp, ss;
};

struct idt_e { u16 lo, sel; u8 ist, attr; u16 mid; u32 hi, zero; } __attribute__((packed));
struct tss { u32 r0; u64 rsp0, rsp1, rsp2, r1; u64 ist[7]; u64 r2; u16 r3, iomap; } __attribute__((packed));
struct dtr { u16 lim; u64 base; } __attribute__((packed));

static struct idt_e idt[256] __attribute__((aligned(16)));
static struct tss tss __attribute__((aligned(16)));
static u64 gdt[5] __attribute__((aligned(16)));
static u8 ist1[16384] __attribute__((aligned(16)));    /* reloj */
static u8 ist2[16384] __attribute__((aligned(16)));    /* excepciones (tambien si la pila se rompio) */

/* ---- stubs ---- */
extern void irq0_entry(void), irq_other_m(void), irq_other_s(void), irq_spurious(void), nmi_entry(void);
#define EXC_DECL(n) extern void exc##n(void);
EXC_DECL(0) EXC_DECL(1) EXC_DECL(3) EXC_DECL(4) EXC_DECL(5) EXC_DECL(6) EXC_DECL(7) EXC_DECL(8) EXC_DECL(9)
EXC_DECL(10) EXC_DECL(11) EXC_DECL(12) EXC_DECL(13) EXC_DECL(14) EXC_DECL(16) EXC_DECL(17) EXC_DECL(18) EXC_DECL(19)
EXC_DECL(20) EXC_DECL(21)

__asm__(
    ".text\n"
    ".macro PUSHALL\n push %rax\n push %rbx\n push %rcx\n push %rdx\n push %rsi\n push %rdi\n push %rbp\n"
    " push %r8\n push %r9\n push %r10\n push %r11\n push %r12\n push %r13\n push %r14\n push %r15\n.endm\n"
    ".macro POPALL\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
    " pop %rbp\n pop %rdi\n pop %rsi\n pop %rdx\n pop %rcx\n pop %rbx\n pop %rax\n.endm\n"
    /* excepciones: sin codigo de error se mete un 0, luego el numero */
    ".macro EXC n, err\n.globl exc\\n\nexc\\n:\n .if \\err == 0\n push $0\n .endif\n push $\\n\n jmp exc_common\n.endm\n"
    "EXC 0,0\nEXC 1,0\nEXC 3,0\nEXC 4,0\nEXC 5,0\nEXC 6,0\nEXC 7,0\nEXC 8,1\nEXC 9,0\nEXC 10,1\nEXC 11,1\n"
    "EXC 12,1\nEXC 13,1\nEXC 14,1\nEXC 16,0\nEXC 17,1\nEXC 18,0\nEXC 19,0\nEXC 20,0\nEXC 21,1\n"
    "exc_common:\n PUSHALL\n mov %rsp, %rdi\n and $-16, %rsp\n cld\n call exc_handler\n"
    "1: cli\n hlt\n jmp 1b\n"
    /* NMI: se ignora */
    ".globl nmi_entry\nnmi_entry:\n iretq\n"
    /* reloj (IRQ0) */
    ".globl irq0_entry\nirq0_entry:\n PUSHALL\n mov %rsp, %rdi\n mov %rsp, %rbx\n and $-16, %rsp\n cld\n"
    " call irq0_handler\n mov %rbx, %rsp\n test %rax, %rax\n jz 2f\n"
    " mov %rsp, %rsi\n mov %rax, %rdi\n mov $20, %ecx\n rep movsq\n mov %rax, %rsp\n call irq_switch\n"
    "2: POPALL\n iretq\n"
    /* otras IRQ (estan enmascaradas; por si acaso) */
    ".globl irq_other_m\nirq_other_m:\n push %rax\n mov $0x20, %al\n outb %al, $0x20\n pop %rax\n iretq\n"
    ".globl irq_other_s\nirq_other_s:\n push %rax\n mov $0x20, %al\n outb %al, $0xA0\n outb %al, $0x20\n pop %rax\n iretq\n"
    ".globl irq_spurious\nirq_spurious:\n iretq\n");

static void set_gate(int v, void (*fn)(void), int ist)
{
    u64 a = (u64)(unsigned long)fn;
    idt[v].lo = (u16)a;
    idt[v].sel = 0x08;
    idt[v].ist = (u8)ist;
    idt[v].attr = 0x8E;                         /* presente, puerta de interrupcion de 64 bits */
    idt[v].mid = (u16)(a >> 16);
    idt[v].hi = (u32)(a >> 32);
    idt[v].zero = 0;
}

/* ---- pantalla de error ---- */
static void hex(char *o, u64 v)
{
    int i;
    for (i = 15; i >= 0; i--) { o[i] = "0123456789ABCDEF"[v & 15]; v >>= 4; }
    o[16] = 0;
}

void exc_handler(struct xframe *f)
{
    static const char *const names[] = {
        "DIVISION ENTRE CERO", "DEPURACION", "NMI", "BREAKPOINT", "DESBORDAMIENTO", "FUERA DE RANGO",
        "INSTRUCCION INVALIDA", "SIN FPU", "FALLO DOBLE", "", "TSS INVALIDO", "SEGMENTO AUSENTE", "FALLO DE PILA",
        "PROTECCION GENERAL", "FALLO DE PAGINA", "", "ERROR DE FPU", "ALINEACION", "MAQUINA", "SIMD", "VIRTUALIZACION",
        "PROTECCION DE CONTROL" };
    char b[20];
    u64 cr2;
    int i;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (vga_gui_active()) {                     /* en el escritorio: volver a la consola del kernel para que se vea */
        vga_con_select(0);
        vga_gui(0);
    }
    vga_puts("\n\n*** BASIC-OS: ERROR GRAVE ***  EXCEPCION ");
    b[0] = (char)('0' + f->vec / 10); b[1] = (char)('0' + f->vec % 10); b[2] = 0;
    vga_puts(b);
    vga_puts(" ");
    if (f->vec < sizeof names / sizeof names[0]) vga_puts(names[f->vec]);
    vga_puts("\nRIP "); hex(b, f->rip); vga_puts(b);
    vga_puts("  RSP "); hex(b, f->rsp); vga_puts(b);
    vga_puts("  CODIGO "); hex(b, f->err); vga_puts(b);
    if (f->vec == 14) { vga_puts("\nDIRECCION "); hex(b, cr2); vga_puts(b); }
    vga_puts("\nRAX "); hex(b, f->rax); vga_puts(b);
    vga_puts("  RBX "); hex(b, f->rbx); vga_puts(b);
    vga_puts("  RCX "); hex(b, f->rcx); vga_puts(b);
    vga_puts("\nRDX "); hex(b, f->rdx); vga_puts(b);
    vga_puts("  RSI "); hex(b, f->rsi); vga_puts(b);
    vga_puts("  RDI "); hex(b, f->rdi); vga_puts(b);
    if (f->rsp >= 0x1000 && f->rsp < 0x40000000ull && !(f->rsp & 7)) {     /* lo de arriba de la pila: de donde venia */
        const u64 *st = (const u64 *)(unsigned long)f->rsp;
        vga_puts("\nPILA ");
        for (i = 0; i < 3; i++) { hex(b, st[i]); vga_puts(b); vga_puts(" "); }
    }
    if (f->rip >= 0x9000000ull && f->rip < 0xB000000ull)
        vga_puts("\n(EL FALLO FUE EN UN PROGRAMA DE LA TIENDA)");
    vga_puts("\n\nPULSA UNA TECLA PARA REINICIAR\n");
    fb_flush();
    for (i = 0; i < 2000000; i++) { if (inb(0x64) & 1) inb(0x60); }    /* vaciar lo que habia */
    for (;;) {                                  /* una tecla (no el raton) */
        u8 st = inb(0x64), d;
        if (!(st & 1)) continue;
        d = inb(0x60);
        if (!(st & 0x20) && !(d & 0x80) && d != 0xE0)
            break;
    }
    outb(0x64, 0xFE);                           /* reinicio por el controlador del teclado */
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---- PIC y PIT ---- */
static u8 bios_mask1, bios_mask2;

static void pic_remap(u8 m, u8 s)
{
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, m); outb(0xA1, s);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
}
static void pit_rate(unsigned div)              /* canal 0, modo 2 */
{
    outb(0x43, 0x34);
    outb(0x40, (u8)div);
    outb(0x40, (u8)(div >> 8));
}
#define PIT_DIV 1193                            /* 1193182 / 1193 = 1000.15 Hz */

static void irq_kernel_mode(void)
{
    struct dtr d;
    pic_remap(0x20, 0x28);
    outb(0x21, 0xFE);                           /* solo IRQ0 (el reloj) */
    outb(0xA1, 0xFF);
    pit_rate(PIT_DIV);
    d.lim = sizeof idt - 1;
    d.base = (u64)(unsigned long)idt;
    __asm__ volatile("lidt %0" : : "m"(d));
}

static void irq_bios_mode(void)
{
    struct dtr d;
    d.lim = 0x3FF;
    d.base = 0;
    __asm__ volatile("lidt %0" : : "m"(d));
    pit_rate(0);                                /* 65536: los 18.2 Hz de siempre */
    pic_remap(0x08, 0x70);
    outb(0x21, bios_mask1);
    outb(0xA1, bios_mask2);
}

static u64 irq_save_off(void)
{
    u64 fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) : : "memory");
    return fl;
}

/* alrededor de cada llamada a la BIOS (bios.c). Mientras la BIOS trabaja el reloj de 1000 Hz no corre: se mide
 * cuanto tardo con el PIT a 18.2 Hz (contador de la BIOS en 0x46C + lo que bajo el canal 0) y se suma a irq_ticks,
 * para que K->ms() no se atrase con cada lectura del disco. */
static u32 bios_t0;
static u32 bda_ticks(void) { u32 v; __asm__ volatile("movl 0x46C, %0" : "=r"(v)); return v; }
static u64 bios_rem;
u64 irq_bios_enter(void)
{
    u64 fl;
    if (!irq_on) return 0;
    fl = irq_save_off();
    irq_bios_mode();
    bios_t0 = bda_ticks();
    return fl | 1;
}
void irq_bios_leave(u64 st)
{
    u32 c, t1;
    u64 counts;
    if (!st) return;
    __asm__ volatile("cli");
    outb(0x43, 0x00);                           /* congelar la cuenta del canal 0 */
    c = inb(0x40);
    c |= (u32)inb(0x40) << 8;
    t1 = bda_ticks();
    counts = (u64)(t1 - bios_t0) * 65536ull + (c ? 65536u - c : 0) + bios_rem;
    irq_ticks += counts / 1193;                 /* 1193 cuentas = 1 ms */
    bios_rem = counts % 1193;
    irq_kernel_mode();
    if (st & 0x200) __asm__ volatile("sti");
}

/* antes de pasarle la maquina a Linux (BASHTERM, Tiny Core) */
void irq_shutdown(void)
{
    if (!irq_on) return;
    __asm__ volatile("cli");
    irq_bios_mode();
    irq_on = 0;
}

/* ---- reloj ---- */
#define SLICE 10                                /* ms por turno */
u64 irq0_handler(struct frame *f)
{
    irq_ticks++;
    outb(0x20, 0x20);
    if (irq_ticks % SLICE == 0 && f->cs == 0x08 && f->rip >= 0x9000000ull && task_others_ready()) {
        u64 dst = ((f->rsp - 128) & ~15ull) - sizeof(struct frame);
        return dst;                             /* el stub copia el marco ahi y llama a irq_switch() */
    }
    return 0;
}

void irq_init(void)
{
    int i;
    u64 base;
    struct dtr d;
    /* GDT nueva: las mismas 3 entradas de entry.asm (0x08 codigo, 0x10 datos) y el TSS en 0x18 */
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFull;
    gdt[2] = 0x00CF92000000FFFFull;
    tss.ist[0] = (u64)(unsigned long)(ist1 + sizeof ist1);
    tss.ist[1] = (u64)(unsigned long)(ist2 + sizeof ist2);
    tss.iomap = sizeof tss;
    base = (u64)(unsigned long)&tss;
    gdt[3] = (sizeof tss - 1) | ((base & 0xFFFFFF) << 16) | (0x89ull << 40) | (((base >> 24) & 0xFF) << 56);
    gdt[4] = base >> 32;
    d.lim = sizeof gdt - 1;
    d.base = (u64)(unsigned long)gdt;
    __asm__ volatile("lgdt %0" : : "m"(d));
    __asm__ volatile("mov $0x18, %%ax; ltr %%ax" : : : "rax");
    for (i = 0; i < 256; i++) set_gate(i, irq_spurious, 0);
    set_gate(0, exc0, 2); set_gate(1, exc1, 2); set_gate(2, nmi_entry, 2); set_gate(3, exc3, 2); set_gate(4, exc4, 2);
    set_gate(5, exc5, 2); set_gate(6, exc6, 2); set_gate(7, exc7, 2); set_gate(8, exc8, 2); set_gate(9, exc9, 2);
    set_gate(10, exc10, 2); set_gate(11, exc11, 2); set_gate(12, exc12, 2); set_gate(13, exc13, 2);
    set_gate(14, exc14, 2); set_gate(16, exc16, 2); set_gate(17, exc17, 2); set_gate(18, exc18, 2);
    set_gate(19, exc19, 2); set_gate(20, exc20, 2); set_gate(21, exc21, 2);
    set_gate(0x20, irq0_entry, 1);
    for (i = 0x21; i < 0x28; i++) set_gate(i, irq_other_m, 0);
    for (i = 0x28; i < 0x30; i++) set_gate(i, irq_other_s, 0);
    set_gate(0x27, irq_spurious, 0);
    __asm__ volatile("cli");
    bios_mask1 = inb(0x21);
    bios_mask2 = inb(0xA1);
    irq_kernel_mode();
    irq_on = 1;
    __asm__ volatile("sti");
}

int irq_active(void) { return irq_on; }

/* la llama el stub del reloj ya sobre la pila de la tarea interrumpida: guarda SSE/x87 (el programa pudo estar a
 * media cuenta con xmm) y le pasa la CPU a otra tarea; al volver aqui la tarea sigue donde la interrumpieron */
void irq_switch(void)
{
    u8 fx[512 + 16];
    u8 *p = (u8 *)(((unsigned long)fx + 15) & ~15ul);
    __asm__ volatile("fxsave64 (%0)" : : "r"(p) : "memory");
    task_preempt();
    __asm__ volatile("fxrstor64 (%0)" : : "r"(p) : "memory");
}

/* espera hasta la siguiente interrupcion (la CPU duerme); sin reloj no hace nada */
void cpu_idle(void)
{
    if (irq_on) __asm__ volatile("sti; hlt" : : : "memory");
    else __asm__ volatile("pause");
}

#else   /* 32 bits: todo sigue por sondeo */
void irq_init(void) {}
int irq_active(void) { return 0; }
void cpu_idle(void) { __asm__ volatile("pause"); }
u64 irq_bios_enter(void) { return 0; }
void irq_bios_leave(u64 st) { (void)st; }
void irq_shutdown(void) {}
#endif
