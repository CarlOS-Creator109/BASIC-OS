/* linux.c - capa Linux minima para correr codigo de musl (y FFmpeg encima) dentro de un programa de BASIC-OS.
 *
 * musl habla con el kernel con la instruccion SYSCALL. Aqui se apunta SYSCALL (MSR LSTAR) a syscall.S, que llama a
 * bos_syscall() con el numero de Linux x86-64; cada llamada se traduce a la kapi de BASIC-OS:
 *   write/writev (1, 2) -> K->print     mmap/munmap -> paginas de la RAM alta (0xB400000 hasta el final de la RAM)
 *   arch_prctl          -> base de FS (el "thread pointer" de musl)       clock_gettime -> K->ms()
 *   exit/exit_group     -> longjmp de vuelta a mod_main (el programa termina y se vuelve a PKG)
 * Lo que no se conoce devuelve -ENOSYS (y se avisa una vez). Asi corre __libc_start_main de musl tal cual, con su malloc.
 * Se compila con -mgeneral-regs-only: no toca los registros SSE del programa.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include "kapi.h"
#include "linux.h"

const struct kapi *K;
jmp_buf bos_exit_jb;
int bos_exit_code;

/* ---- MSR ---- */
static inline uint64_t rdmsr(uint32_t m)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MSR_FSBASE 0xC0000100

static uint64_t old_efer, old_star, old_lstar, old_fmask, old_fs, cur_fs;
extern void bos_syscall_entry(void);

void bos_sse_on(void)
{
    unsigned long c;
    unsigned mx = 0x1F80;
    __asm__ volatile("mov %%cr0, %0" : "=r"(c));
    c &= ~(1ul << 2);                       /* EM */
    c &= ~(1ul << 3);                       /* TS */
    c |= (1ul << 1);                        /* MP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(c) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(c));
    c |= (1ul << 9) | (1ul << 10);          /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" : : "r"(c) : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mx));
}

/* despues de llamar al kernel (puede pasar por la BIOS y recargar FS o tocar CR0): dejar todo como lo quiere musl */
void bos_after_kernel(void)
{
    bos_sse_on();
    wrmsr(MSR_FSBASE, cur_fs);
}

/* ---- memoria: paginas de 4 KB entre heap_lo y heap_hi, con un mapa de bits ---- */
#define PG 4096ul
static uint64_t heap_lo, heap_hi;
static unsigned npages, hint;
static uint8_t pagemap[(1u << 20) / 8];      /* hasta 4 GB de RAM alta */

static int pg_used(unsigned i) { return pagemap[i >> 3] & (1 << (i & 7)); }
static void pg_set(unsigned i, int on) { if (on) pagemap[i >> 3] |= (uint8_t)(1 << (i & 7)); else pagemap[i >> 3] &= (uint8_t)~(1 << (i & 7)); }

static long mem_alloc(uint64_t len)
{
    unsigned need = (unsigned)((len + PG - 1) / PG), run = 0, i, start = 0, pass;
    if (!need || need > npages)
        return -12;
    for (pass = 0; pass < 2; pass++) {
        run = 0;
        for (i = pass ? 0 : hint; i < npages; i++) {
            if (pg_used(i)) { run = 0; continue; }
            if (!run) start = i;
            if (++run == need) {
                unsigned k;
                for (k = start; k < start + need; k++) pg_set(k, 1);
                hint = start + need;
                memset((void *)(heap_lo + (uint64_t)start * PG), 0, (size_t)need * PG);
                return (long)(heap_lo + (uint64_t)start * PG);
            }
        }
    }
    return -12;                                 /* ENOMEM */
}

static long mem_free(uint64_t addr, uint64_t len)
{
    uint64_t a;
    if (addr < heap_lo || addr >= heap_hi)
        return -22;
    for (a = addr & ~(PG - 1); a < addr + len && a < heap_hi; a += PG) {
        unsigned i = (unsigned)((a - heap_lo) / PG);
        pg_set(i, 0);
        if (i < hint) hint = i;
    }
    return 0;
}

uint64_t bos_free_ram(void)
{
    unsigned i, n = 0;
    for (i = 0; i < npages; i++) if (!pg_used(i)) n++;
    return (uint64_t)n * PG;
}

/* ---- salida ---- */
static void out(const char *p, size_t n)
{
    char b[257];
    while (n) {
        size_t c = n > 256 ? 256 : n, i, k = 0;
        for (i = 0; i < c; i++) if (p[i]) b[k++] = p[i];
        b[k] = 0;
        K->print(b);
        p += c;
        n -= c;
    }
}

struct kiov { const void *base; size_t len; };
struct kts { long sec, nsec; };

static uint64_t rnd = 88172645463325252ull;

long bos_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    static unsigned char warned[512];
    (void)a5; (void)a6;
    switch (nr) {
    case 0:                                                 /* read */
        return 0;
    case 1:                                                 /* write */
        if (a1 == 1 || a1 == 2) { out((const char *)a2, (size_t)a3); return a3; }
        return -9;
    case 20: {                                              /* writev */
        const struct kiov *v = (const struct kiov *)a2;
        long i, t = 0;
        if (a1 != 1 && a1 != 2) return -9;
        for (i = 0; i < a3; i++) { out((const char *)v[i].base, v[i].len); t += (long)v[i].len; }
        return t; }
    case 9:                                                 /* mmap: solo memoria anonima */
        if (!(a4 & 0x20)) return -19;                       /* ENODEV: no hay archivos que mapear */
        return mem_alloc((uint64_t)a2);
    case 11:                                                /* munmap */
        return mem_free((uint64_t)a1, (uint64_t)a2);
    case 12:                                                /* brk: no hay; musl usa mmap */
        return a1 ? -12 : 0;
    case 25:                                                /* mremap: que musl copie */
        return -12;
    case 10: case 28:                                       /* mprotect, madvise */
        return 0;
    case 158:                                               /* arch_prctl */
        if (a1 == 0x1002) { cur_fs = (uint64_t)a2; wrmsr(MSR_FSBASE, cur_fs); return 0; }
        if (a1 == 0x1003) { *(uint64_t *)a2 = cur_fs; return 0; }
        return -22;
    case 218: case 39: case 186: case 110:                  /* set_tid_address, getpid, gettid, getppid */
        return 1;
    case 13: case 14: case 131: case 273: case 302: case 334: case 202: case 24:
        return 0;                                           /* senales, pila alterna, robust_list, prlimit, rseq, futex, yield */
    case 16:                                                /* ioctl: no hay terminal */
        return -25;
    case 228: {                                             /* clock_gettime */
        struct kts *ts = (struct kts *)a2;
        unsigned ms = K->ms();
        ts->sec = ms / 1000;
        ts->nsec = (long)(ms % 1000) * 1000000;
        return 0; }
    case 96: {                                              /* gettimeofday */
        struct kts *tv = (struct kts *)a1;
        unsigned ms = K->ms();
        if (tv) { tv->sec = ms / 1000; tv->nsec = (long)(ms % 1000) * 1000; }
        return 0; }
    case 318: {                                             /* getrandom */
        unsigned char *b = (unsigned char *)a1;
        long i;
        rnd ^= K->ms();
        for (i = 0; i < a2; i++) {
            rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17;
            b[i] = (unsigned char)rnd;
        }
        return a2; }
    case 63: {                                              /* uname */
        char *u = (char *)a1;
        memset(u, 0, 65 * 6);
        strcpy(u, "BASIC-OS"); strcpy(u + 65, "basicos"); strcpy(u + 130, "0002");
        strcpy(u + 195, "MPLAY"); strcpy(u + 260, "x86_64");
        return 0; }
    case 60: case 231:                                      /* exit, exit_group */
        bos_exit_code = (int)a1;
        longjmp(bos_exit_jb, 1);                            /* (si ya volvimos a mod_main, ese contexto ya no existe:
                                                               los programas residentes no deben llamar a exit) */
    }
    if (nr >= 0 && nr < 512 && !warned[nr]) {
        char b[64];
        int k = 0;
        long v = nr;
        char d[12];
        int n = 0;
        warned[nr] = 1;
        memcpy(b, "(MPLAY: syscall ", 16); k = 16;
        do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) b[k++] = d[--n];
        memcpy(b + k, " sin emular)\n", 14);
        K->print(b);
    }
    return -38;                                             /* ENOSYS */
}

/* ---- arranque: SYSCALL apuntando aqui y musl con su __libc_start_main ---- */
extern int __libc_start_main(int (*main)(int, char **, char **), int argc, char **argv,
                             void (*i)(void), void (*f)(void), void (*l)(void));

static void msr_on(void)
{
    wrmsr(MSR_STAR, (old_star & 0xFFFFFFFFull) | (0x08ull << 32) | (0x08ull << 48));   /* CS 0x08, SS 0x10: los del kernel */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)bos_syscall_entry);
    wrmsr(MSR_FMASK, 0x400);                                /* DF a cero al entrar */
    wrmsr(MSR_EFER, old_efer | 1);                          /* SCE: la instruccion SYSCALL existe */
}

/* arranca musl y llama a main_fn. Devuelve 1 si main_fn llamo a bos_resident() (el programa sigue vivo: ventana)
 * o 0 si termino (exit o return); en ese caso todo queda como estaba. */
int bos_linux_start(const struct kapi *k, uint64_t lo, uint64_t hi, int (*main_fn)(int, char **, char **))
{
    static char *vec[16];
    static unsigned char random16[16];
    int i, r;
    K = k;
    heap_lo = (lo + PG - 1) & ~(PG - 1);
    heap_hi = hi & ~(PG - 1);
    if (heap_hi - heap_lo > (uint64_t)sizeof pagemap * 8 * PG)
        heap_hi = heap_lo + (uint64_t)sizeof pagemap * 8 * PG;
    npages = (unsigned)((heap_hi - heap_lo) / PG);
    memset(pagemap, 0, sizeof pagemap);
    hint = 0;
    for (i = 0; i < 16; i++) random16[i] = (unsigned char)(K->ms() * 131 + i * 7);

    old_efer = rdmsr(MSR_EFER); old_star = rdmsr(MSR_STAR); old_lstar = rdmsr(MSR_LSTAR);
    old_fmask = rdmsr(MSR_FMASK); old_fs = rdmsr(MSR_FSBASE);
    msr_on();
    bos_sse_on();

    vec[0] = (char *)"mplay"; vec[1] = 0;                   /* argv */
    vec[2] = 0;                                             /* envp */
    vec[3] = (char *)6;  vec[4] = (char *)4096;             /* AT_PAGESZ */
    vec[5] = (char *)25; vec[6] = (char *)random16;         /* AT_RANDOM */
    vec[7] = (char *)0;  vec[8] = 0;                        /* AT_NULL */

    r = setjmp(bos_exit_jb);
    if (!r)
        __libc_start_main(main_fn, 1, vec, 0, 0, 0);
    if (r == 2)
        return 1;                                           /* sigue vivo */
    bos_linux_stop();
    return 0;
}

void bos_resident(void) { longjmp(bos_exit_jb, 2); }

/* al entrar desde el escritorio (callbacks de la ventana): FS, SSE y SYSCALL como los quiere musl */
void bos_enter(void)
{
    msr_on();
    bos_after_kernel();
}

void bos_linux_stop(void)
{
    wrmsr(MSR_FSBASE, old_fs);
    wrmsr(MSR_EFER, old_efer);
    wrmsr(MSR_LSTAR, old_lstar);
    wrmsr(MSR_STAR, old_star);
    wrmsr(MSR_FMASK, old_fmask);
}

int bos_linux_run(const struct kapi *k, uint64_t lo, uint64_t hi, int (*main_fn)(int, char **, char **))
{
    if (bos_linux_start(k, lo, hi, main_fn)) {              /* no deberia quedarse vivo: se apaga igual */
        bos_linux_stop();
        return 0;
    }
    return bos_exit_code;
}
