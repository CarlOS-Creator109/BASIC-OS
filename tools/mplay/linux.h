/* linux.h - capa Linux de MPLAY (ver linux.c) */
#ifndef BOS_LINUX_H
#define BOS_LINUX_H
#include <stdint.h>
struct kapi;
extern const struct kapi *K;
int bos_linux_run(const struct kapi *k, uint64_t heap_lo, uint64_t heap_hi, int (*main_fn)(int, char **, char **));
void bos_after_kernel(void);          /* llamar despues de K->read (la BIOS puede tocar FS y CR0) */
void bos_sse_on(void);
uint64_t bos_free_ram(void);
#endif
