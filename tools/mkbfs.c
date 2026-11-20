/* mkbfs.c - BFS (el sistema de archivos de BASIC-OS) desde Linux: crear discos, meter y sacar archivos.
 *
 *   mkbfs nuevo   DISCO.img MB [--arranque pc-basic.img] [--etiqueta NOMBRE]
 *                 disco nuevo con una particion BFS (tipo 0xB0) desde el sector 4096. Con --arranque, los primeros 4096
 *                 sectores son los de pc-basic.img (arranque, kernel, /SYS): el disco arranca BASIC-OS
 *   mkbfs meter   DISCO.img ORIGEN [DESTINO]     archivo o carpeta entera (DESTINO dentro del disco, por defecto /nombre)
 *   mkbfs sacar   DISCO.img RUTA [DESTINO]       archivo o carpeta entera
 *   mkbfs ls      DISCO.img [RUTA]
 *   mkbfs mkdir   DISCO.img RUTA      mkbfs rm DISCO.img RUTA      mkbfs mv DISCO.img DE A
 *   mkbfs info    DISCO.img           mkbfs revisar DISCO.img
 *
 * El volumen se busca en la tabla de particiones (tipo 0xB0); si no hay tabla, el archivo entero es el volumen.
 *   cc -O2 -I. -DBFS_HOST -o tools/mkbfs tools/mkbfs.c bfs.c          (lo hace make)
 * GPL-3.0-or-later. */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bfs.h"

#define START 4096u                            /* la particion empieza aqui (lo de antes es el arranque) */

static FILE *disk;
static uint64_t part_lba;
static struct bfs vol;

static void die(const char *a, const char *b)
{
    fprintf(stderr, "mkbfs: %s%s%s\n", a, b ? ": " : "", b ? b : "");
    exit(1);
}

static int d_rd(void *ctx, bfs_u64 lba, bfs_u32 n, void *buf)
{
    (void)ctx;
    if (fseeko(disk, (off_t)((part_lba + lba) * 512), SEEK_SET)) return 1;
    return fread(buf, 512, n, disk) != n;
}
static int d_wr(void *ctx, bfs_u64 lba, bfs_u32 n, const void *buf)
{
    (void)ctx;
    if (fseeko(disk, (off_t)((part_lba + lba) * 512), SEEK_SET)) return 1;
    return fwrite(buf, 512, n, disk) != n;
}

static void open_disk(const char *path)
{
    unsigned char mbr[512];
    int i, r;
    disk = fopen(path, "r+b");
    if (!disk) die("no puedo abrir", path);
    part_lba = 0;
    if (fread(mbr, 1, 512, disk) == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA)
        for (i = 0; i < 4; i++)
            if (mbr[0x1BE + i * 16 + 4] == BFS_PTYPE) {
                part_lba = (uint32_t)(mbr[0x1BE + i * 16 + 8] | mbr[0x1BE + i * 16 + 9] << 8 |
                                      mbr[0x1BE + i * 16 + 10] << 16 | (uint32_t)mbr[0x1BE + i * 16 + 11] << 24);
                break;
            }
    vol.rd = d_rd;
    vol.wr = d_wr;
    vol.tx = malloc((size_t)BFS_TXMAX * BFS_BS);
    if ((r = bfs_mount(&vol)))
        die("el disco no tiene un volumen BFS", bfs_strerror(r));
}

static void cmd_new(int argc, char **argv)
{
    const char *out = argv[2], *boot = 0, *label = "BASIC-OS";
    uint64_t mb = strtoull(argv[3], 0, 10), total, psect;
    static unsigned char head[START * 512];
    int i, r;
    for (i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--arranque") && i + 1 < argc) boot = argv[++i];
        else if (!strcmp(argv[i], "--etiqueta") && i + 1 < argc) label = argv[++i];
        else die("opcion desconocida", argv[i]);
    }
    if (mb < 8) die("el disco tiene que ser de 8 MB o mas", 0);
    total = mb * 2048;
    psect = total - START;
    disk = fopen(out, "w+b");
    if (!disk) die("no puedo crear", out);
    if (ftruncate(fileno(disk), (off_t)(total * 512))) die("no pude darle el tamano", strerror(errno));
    memset(head, 0, sizeof head);
    if (boot) {
        FILE *f = fopen(boot, "rb");
        if (!f) die("no puedo abrir", boot);
        if (!fread(head, 1, sizeof head, f)) die("arranque vacio", boot);
        fclose(f);
        for (i = 0x1BE; i < 0x1FE; i++)
            if (head[i]) die("el sector de arranque usa el lugar de la tabla de particiones", boot);
    }
    {
        unsigned char *e = head + 0x1BE;
        uint32_t s32 = START, p32 = psect > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)psect;
        e[0] = 0x80; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[4] = BFS_PTYPE; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        memcpy(e + 8, &s32, 4);
        memcpy(e + 12, &p32, 4);
        head[0x1FE] = 0x55;
        head[0x1FF] = 0xAA;
    }
    if (fwrite(head, 1, sizeof head, disk) != sizeof head) die("no pude escribir el arranque", 0);
    part_lba = START;
    vol.rd = d_rd;
    vol.wr = d_wr;
    vol.tx = malloc((size_t)BFS_TXMAX * BFS_BS);
    if ((r = bfs_mkfs(&vol, psect, label))) die("no pude crear el volumen", bfs_strerror(r));
    printf("%s: %llu MB, BFS \"%s\" desde el sector %u: %llu bloques de 4 KB (%llu libres), %llu inodos\n", out,
           (unsigned long long)mb, label, START, (unsigned long long)vol.sb.nblocks,
           (unsigned long long)vol.sb.free_blocks, (unsigned long long)vol.sb.ninodes);
}

/* ---- meter ---- */
static unsigned long long n_files, n_dirs, n_bytes;

static void put_file(const char *src, const char *dst)
{
    FILE *f = fopen(src, "rb");
    struct stat st;
    void *buf;
    int r;
    if (!f || fstat(fileno(f), &st)) die("no puedo leer", src);
    buf = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (!buf || (st.st_size && fread(buf, 1, (size_t)st.st_size, f) != (size_t)st.st_size)) die("no puedo leer", src);
    fclose(f);
    if ((r = bfs_write(&vol, dst, buf, (bfs_u64)st.st_size))) {
        fprintf(stderr, "mkbfs: %s: %s\n", dst, bfs_strerror(r));
        exit(1);
    }
    free(buf);
    n_files++;
    n_bytes += (unsigned long long)st.st_size;
}

static void put_tree(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st)) die("no existe", src);
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *e;
        int r = bfs_mkdir(&vol, dst);
        if (r && r != BFS_EEXIST) { fprintf(stderr, "mkbfs: %s: %s\n", dst, bfs_strerror(r)); exit(1); }
        n_dirs++;
        d = opendir(src);
        if (!d) die("no puedo abrir", src);
        while ((e = readdir(d))) {
            char s2[4096], d2[4096];
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(s2, sizeof s2, "%s/%s", src, e->d_name);
            snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
            put_tree(s2, d2);
        }
        closedir(d);
    } else {
        put_file(src, dst);
    }
}

static void cmd_put(int argc, char **argv)
{
    char dst[4096];
    const char *src = argv[3], *base = strrchr(src, '/');
    struct bfs_inode in;
    bfs_u32 ino;
    base = base ? base + 1 : src;
    open_disk(argv[2]);
    if (argc > 4) {
        snprintf(dst, sizeof dst, "%s", argv[4]);
        /* si DESTINO es una carpeta que ya existe, adentro */
        if (bfs_lookup(&vol, dst, &ino, &in) == 0 && in.type == BFS_T_DIR) {
            size_t n = strlen(dst);
            snprintf(dst + n, sizeof dst - n, "%s%s", (n && dst[n - 1] == '/') ? "" : "/", base);
        }
    } else {
        snprintf(dst, sizeof dst, "/%s", base);
    }
    put_tree(src, dst);
    printf("mkbfs: %llu archivos, %llu carpetas, %.1f MB -> %s\n", n_files, n_dirs, n_bytes / 1048576.0, dst);
}

/* ---- sacar / ls ---- */
struct item { char name[256]; int type; bfs_u64 size; };
struct items { struct item *v; int n, cap; };

static int collect_cb(void *ctx, const char *name, int nlen, bfs_u32 ino, int type, bfs_u64 size)
{
    struct items *it = ctx;
    (void)ino;
    if (it->n == it->cap) {
        it->cap = it->cap ? it->cap * 2 : 64;
        it->v = realloc(it->v, (size_t)it->cap * sizeof *it->v);
    }
    memcpy(it->v[it->n].name, name, (size_t)nlen);
    it->v[it->n].name[nlen] = 0;
    it->v[it->n].type = type;
    it->v[it->n].size = size;
    it->n++;
    return 0;
}

static void get_tree(const char *src, const char *dst)
{
    struct bfs_inode in;
    bfs_u32 ino;
    int r, i;
    if ((r = bfs_lookup(&vol, src, &ino, &in))) { fprintf(stderr, "mkbfs: %s: %s\n", src, bfs_strerror(r)); exit(1); }
    if (in.type == BFS_T_DIR) {
        struct items it = { 0, 0, 0 };
        mkdir(dst, 0755);
        bfs_list(&vol, ino, collect_cb, &it);
        for (i = 0; i < it.n; i++) {
            char s2[4096], d2[4096];
            snprintf(s2, sizeof s2, "%s/%s", src, it.v[i].name);
            snprintf(d2, sizeof d2, "%s/%s", dst, it.v[i].name);
            get_tree(s2, d2);
        }
        free(it.v);
        n_dirs++;
    } else {
        void *buf = malloc(in.size ? in.size : 1);
        FILE *f;
        long long got = bfs_read(&vol, &in, 0, buf, in.size);
        if (got != (long long)in.size) die("error al leer", src);
        f = fopen(dst, "wb");
        if (!f || fwrite(buf, 1, in.size, f) != in.size || fclose(f)) die("no puedo escribir", dst);
        free(buf);
        n_files++;
        n_bytes += in.size;
    }
}

static int cmp_item(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    if (x->type != y->type) return y->type - x->type;
    return strcasecmp(x->name, y->name);
}

static void cmd_ls(int argc, char **argv)
{
    struct bfs_inode in;
    struct items it = { 0, 0, 0 };
    bfs_u32 ino;
    int r, i;
    open_disk(argv[2]);
    if ((r = bfs_lookup(&vol, argc > 3 ? argv[3] : "/", &ino, &in))) die(argc > 3 ? argv[3] : "/", bfs_strerror(r));
    if (in.type != BFS_T_DIR) { printf("%12llu  %s\n", (unsigned long long)in.size, argv[3]); return; }
    bfs_list(&vol, ino, collect_cb, &it);
    qsort(it.v, (size_t)it.n, sizeof *it.v, cmp_item);
    for (i = 0; i < it.n; i++) {
        if (it.v[i].type == BFS_T_DIR) printf("       <DIR>  %s/\n", it.v[i].name);
        else printf("%12llu  %s\n", (unsigned long long)it.v[i].size, it.v[i].name);
    }
    free(it.v);
}

static void cmd_info(int argc, char **argv)
{
    struct bfs_super *s = &vol.sb;
    (void)argc;
    open_disk(argv[2]);
    printf("BFS \"%s\" (version %u), particion en el sector %llu\n", s->label, s->version, (unsigned long long)part_lba);
    printf("  %llu bloques de 4 KB = %.1f MB; libres %llu (%.1f MB)\n", (unsigned long long)s->nblocks,
           s->nblocks * 4096.0 / 1048576, (unsigned long long)s->free_blocks, s->free_blocks * 4096.0 / 1048576);
    printf("  inodos: %llu (libres %llu); diario: %llu bloques, operaciones hechas: %llu\n",
           (unsigned long long)s->ninodes, (unsigned long long)s->free_inodes, (unsigned long long)s->jblocks,
           (unsigned long long)s->jseq);
    printf("  mapa de bloques %llu+%llu, mapa de inodos %llu+%llu, tabla de inodos %llu+%llu, datos desde %llu\n",
           (unsigned long long)s->bmap_start, (unsigned long long)s->bmap_blocks, (unsigned long long)s->imap_start,
           (unsigned long long)s->imap_blocks, (unsigned long long)s->itab_start, (unsigned long long)s->itab_blocks,
           (unsigned long long)s->data_start);
}

static void usage(void)
{
    fprintf(stderr,
            "uso: mkbfs nuevo DISCO.img MB [--arranque pc-basic.img] [--etiqueta NOMBRE]\n"
            "     mkbfs meter DISCO.img ORIGEN [DESTINO]    (archivo o carpeta)\n"
            "     mkbfs sacar DISCO.img RUTA [DESTINO]\n"
            "     mkbfs ls DISCO.img [RUTA]      mkbfs mkdir DISCO.img RUTA\n"
            "     mkbfs rm DISCO.img RUTA        mkbfs mv DISCO.img DE A\n"
            "     mkbfs info DISCO.img           mkbfs revisar DISCO.img\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int r;
    if (argc < 3) usage();
    if (!strcmp(argv[1], "nuevo") && argc >= 4) cmd_new(argc, argv);
    else if (!strcmp(argv[1], "meter") && argc >= 4) cmd_put(argc, argv);
    else if (!strcmp(argv[1], "sacar") && argc >= 4) {
        const char *b = strrchr(argv[3], '/');
        open_disk(argv[2]);
        get_tree(argv[3], argc > 4 ? argv[4] : (b && b[1] ? b + 1 : "bfs-salida"));
        printf("mkbfs: %llu archivos, %.1f MB\n", n_files, n_bytes / 1048576.0);
    } else if (!strcmp(argv[1], "ls")) cmd_ls(argc, argv);
    else if (!strcmp(argv[1], "info")) cmd_info(argc, argv);
    else if (!strcmp(argv[1], "mkdir") && argc >= 4) {
        open_disk(argv[2]);
        if ((r = bfs_mkdir(&vol, argv[3]))) die(argv[3], bfs_strerror(r));
    } else if (!strcmp(argv[1], "rm") && argc >= 4) {
        open_disk(argv[2]);
        if ((r = bfs_remove(&vol, argv[3]))) die(argv[3], bfs_strerror(r));
    } else if (!strcmp(argv[1], "mv") && argc >= 5) {
        open_disk(argv[2]);
        if ((r = bfs_rename(&vol, argv[3], argv[4]))) die(argv[3], bfs_strerror(r));
    } else if (!strcmp(argv[1], "revisar")) {
        char msg[512];
        open_disk(argv[2]);
        r = bfs_check(&vol, msg, sizeof msg);
        if (r) { printf("BFS con problemas: %s\n", msg); return 1; }
        printf("BFS bien: %llu MB libres de %.0f MB\n", (unsigned long long)(vol.sb.free_blocks * 4 / 1024),
               vol.sb.nblocks * 4096.0 / 1048576);
    } else usage();
    if (disk && fclose(disk)) die("no pude cerrar el disco", strerror(errno));
    return 0;
}
