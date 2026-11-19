/* bccos.c - BCC dentro de BASIC-OS (store/BCC.BIN): pide un archivo .C de la particion, lo compila y guarda el
 * programa en HDn.1:/APPS/NOMBRE.BIN (luego se corre desde PKG con su numero en la lista).
 *
 * Memoria (BCC.BIN se carga en 0x9000000 como cualquier programa de la tienda):
 *   0x9800000 - 0xAE00000  monticulo del compilador (22 MB)
 *   0xAE00000 - 0xAF00000  pila propia (1 MB; la del kernel es de 32 KB y el analizador es recursivo)
 *   0xAF00000 - 0xB000000  el .BIN que sale (1 MB)
 *   0xB200000 - 0xB250000  buffers del escritor FAT (fatw.h, igual que PKG)
 */
#include "kapi.h"
#include "mod_head.h"

typedef unsigned short u16;
typedef unsigned int u32;
void *memset(void *dst, int c, unsigned long n);
void *memcpy(void *dst, const void *src, unsigned long n);
unsigned long strlen(const char *s);

static const struct kapi *K;
static char *plat_read(const char *path, int *len);
static void plat_msg(const char *s);

#include "bcc.c"

#define MEM_LO     0x9800000ul
#define MEM_HI     0xAE00000ul
#define STACK_TOP  0xAF00000ul
#define OUT_ADDR   0xAF00000ul
#define OUT_MAX    0x100000
#define NEED_RAM   0xB300000ull
#define FW_FAT     0xB200000u
#define FW_ROOT    0xB220000u
#define FW_BUF     0xB230000u
#define FW_SCR     0xB240000u
#include "sys/fatw.h"

struct bootinfo { u32 magic; u16 w, h, pitch; u8 bpp, gfx; u32 fb; u8 rs, rp, gs, gp, bs, bp; u16 e820n; u16 extkb; } __attribute__((packed));
struct e820 { u64 base, len; u32 type, attr; } __attribute__((packed));
#define BI ((const struct bootinfo *)0x6000)
#define E8 ((const struct e820 *)0x6020)

static u64 ram_end(void)
{
    u64 end = 0;
    int i;
    if (BI->e820n) {
        for (i = 0; i < BI->e820n; i++)
            if (E8[i].type == 1 && E8[i].base <= 0x4000000ull && E8[i].base + E8[i].len > 0x4000000ull)
                end = E8[i].base + E8[i].len;
    } else
        end = ((u64)BI->extkb + 1024) * 1024;
    return end;
}

static void pr(const char *s) { K->print(s); }
static void prnum(long v) { char b[24]; sfmt_num(b, v); pr(b); }
static void plat_msg(const char *s) { pr(s); }

static char *plat_read(const char *path, int *len)
{
    unsigned total = 0;
    char tmp[16], *buf;
    int n = K->read(path, tmp, 0, &total);
    if (n < 0)
        return 0;
    buf = zalloc((long)total + 1);
    n = K->read(path, buf, (int)total, &total);
    if (n < 0)
        return 0;
    buf[n] = 0;
    *len = n;
    return buf;
}

static int readline(char *buf, int max)
{
    int len = 0;
    K->esc();
    for (;;) {
        int c = K->poll();
        if (!c) {
            if (K->esc()) { pr("\n"); return -1; }
            K->sleep(5);
            continue;
        }
        if (c == '\r' || c == '\n') { pr("\n"); buf[len] = 0; return len; }
        if ((c == 8 || c == 127 || c == 20) && len) { len--; pr("\b \b"); continue; }
        if (c >= 32 && c < 127 && len < max - 1) {
            char s[2];
            buf[len++] = (char)c;
            s[0] = (char)c; s[1] = 0;
            pr(s);
        }
    }
}

/* corre fn con la pila nueva (rsi = tope) */
int bcc_on_stack(int (*fn)(void), unsigned long top);
__asm__(".text\n.globl bcc_on_stack\nbcc_on_stack:\n"
        "push %rbp\nmov %rsp, %rbp\nmov %rsi, %rsp\ncall *%rdi\nmov %rbp, %rsp\npop %rbp\nret\n");

static char src[96];
static int out_len;

static int do_compile(void)
{
    return bcc_compile(src, (u8 *)OUT_ADDR, OUT_MAX, &out_len, (char *)MEM_LO, (long)(MEM_HI - MEM_LO));
}

static void out_name(char *name, const char *path)
{
    int i, last = 0, k = 0;
    for (i = 0; path[i]; i++)
        if (path[i] == ':' || path[i] == '/' || path[i] == '\\') last = i + 1;
    for (i = last; path[i] && path[i] != '.' && k < 8; i++) {
        char c = path[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        name[k++] = c;
    }
    name[k] = 0;
    scat(name + k, ".BIN");
}

int mod_main(const struct kapi *k)
{
    K = k;
    pr("\nBCC " BCC_VERSION " - el compilador de C de BASIC-OS\n");
    if (ram_end() < NEED_RAM) {
        pr("BCC necesita ~180 MB de RAM (en QEMU: -m 256)\n");
        return 0;
    }
    fw_init();
    for (;;) {
        char name[16];
        unsigned t0;
        int r;
        pr("\nArchivo .C para compilar (por ejemplo HD0.1:/SRC/HOLA.C; Enter = salir): ");
        if (readline(src, sizeof src) <= 0)
            break;
        out_name(name, src);
        pr("Compilando "); pr(src); pr(" ...\n");
        t0 = K->ms();
        r = bcc_on_stack(do_compile, STACK_TOP);
        if (r) {
            pr(errbuf);
            pr("\n");
            continue;
        }
        pr("Listo: "); pr(name); pr(" ("); prnum(out_len); pr(" bytes, "); prnum((long)(K->ms() - t0)); pr(" ms)\n");
        if (!fw.ok) {
            pr("(no hay disco FAT16 de la BIOS donde guardarlo)\n");
            continue;
        }
        r = fw_save(name, (const u8 *)OUT_ADDR, (u32)out_len);
        if (r) { pr("BCC: no pude guardarlo en el disco (codigo "); prnum(r); pr(")\n"); continue; }
        {
            char sp[64];
            fw_spec(sp, name);
            pr("Guardado en "); pr(sp); pr(". Correlo desde PKG (sale en la lista con su numero).\n");
        }
    }
    return 0;
}
