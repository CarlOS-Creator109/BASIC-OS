/* mmio.c - mapea registros de dispositivos PCI (BAR) en el espacio de direcciones
 *
 * entry.asm deja identidad con paginas de 2 MB en el primer GB. Los BAR suelen estar
 * entre 2 y 4 GB, asi que se agregan paginas de 2 MB sin cache (PCD) en las tablas.
 */
#include "kernel.h"

#define PDPT ((volatile u64 *)0x71000)

static u64 extra_pd[4][512] __attribute__((aligned(4096)));
static int nextra;

void *map_mmio(u64 phys, u32 size)
{
    u64 first = phys >> 21, last = (phys + size - 1) >> 21, p;

    if (phys + size > 0x100000000ULL || !size)
        return NULL;
    for (p = first; p <= last; p++) {
        u64 gb = p >> 9, idx = p & 511;
        volatile u64 *pd;
        if (!(PDPT[gb] & 1)) {
            if (nextra >= 4)
                return NULL;
            pd = extra_pd[nextra++];
            PDPT[gb] = (u64)(unsigned long)pd | 3;
        } else {
            pd = (volatile u64 *)(unsigned long)(PDPT[gb] & ~0xFFFULL);
        }
        if (gb == 0 && idx == 0)
            continue;                           /* el primer 2 MB va en paginas de 4 KB */
        if (!(pd[idx] & 1))
            pd[idx] = (p << 21) | 0x93;         /* P | RW | PS | PCD */
    }
    {
        u64 cr3;
        __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
    }
    return (void *)(unsigned long)phys;
}
