#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
int elf64_load(const unsigned char *f, unsigned len, void *dst, unsigned long cap, unsigned long *entry_out);
struct tkapi { void (*print)(const char *); };
static void pr(const char *s) { fputs(s, stdout); }
int main(int argc, char **argv) {
    FILE *fp = fopen(argv[1], "rb"); if (!fp) { perror("open"); return 1; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc(n); fread(buf, 1, n, fp); fclose(fp);
    unsigned long cap = 1u << 20;
    void *region = mmap(NULL, cap, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (region == MAP_FAILED) { perror("mmap"); return 1; }
    unsigned long entry = 0;
    int r = elf64_load(buf, (unsigned)n, region, cap, &entry);
    printf("elf64_load -> %d (entry=%p, base=%p)\n", r, (void*)entry, region);
    if (r) { printf("FALLO: codigo %d\n", r); return 1; }
    struct tkapi K = { pr };
    int (*mod_main)(const struct tkapi *) = (int(*)(const struct tkapi *))entry;
    int v = mod_main(&K);
    printf("mod_main devolvio %d (esperado 60)\n", v);
    return v == 60 ? 0 : 1;
}
