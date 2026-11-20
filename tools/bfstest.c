/* bfstest.c - prueba de BFS en Linux: operaciones al azar contra un modelo en memoria, revision (fsck) y cortes de luz
 * simulados a media operacion (el disco deja de escribir en un punto al azar; al montar, el diario repara).
 *   cc -O2 -I. -DBFS_HOST -o tools/bfstest tools/bfstest.c bfs.c && tools/bfstest [semilla]      (make bfs-test) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bfs.h"

#define DISK_MB 64
#define NF 40
static unsigned char *disk;
static long write_budget = -1;                 /* escrituras que quedan antes del "apagon" (-1 = sin limite) */
static int crashed;

static int m_rd(void *c, bfs_u64 lba, bfs_u32 n, void *buf) { (void)c; memcpy(buf, disk + lba * 512, (size_t)n * 512); return 0; }
static int m_wr(void *c, bfs_u64 lba, bfs_u32 n, const void *buf)
{
    (void)c;
    if (write_budget == 0) { crashed = 1; return 1; }
    if (write_budget > 0) write_budget--;
    memcpy(disk + lba * 512, buf, (size_t)n * 512);
    return 0;
}

struct mf { char name[64]; unsigned char *data; size_t size; int live; };
static struct mf model[NF];
static struct bfs v;

static int last_op, last_crash;
static void fail(const char *a, int seed, int step)
{
    printf("FALLO (semilla %d, paso %d, op %d, apagon %d): %s\n", seed, step, last_op, last_crash, a);
    exit(1);
}

static void verify(int seed, int step)
{
    char msg[512];
    int i;
    if (bfs_check(&v, msg, sizeof msg)) fail(msg, seed, step);
    for (i = 0; i < NF; i++) {
        struct bfs_inode in;
        bfs_u32 ino;
        int r = bfs_lookup(&v, model[i].name, &ino, &in);
        if (!model[i].live) { if (r == 0) fail("existe un archivo borrado", seed, step); continue; }
        if (r) fail("falta un archivo", seed, step);
        if (in.size != model[i].size) fail("tamano distinto", seed, step);
        {
            unsigned char *b = malloc(in.size + 1);
            if (bfs_read(&v, &in, 0, b, in.size) != (long long)in.size || memcmp(b, model[i].data, in.size))
                fail("contenido distinto", seed, step);
            free(b);
        }
    }
}

int main(int argc, char **argv)
{
    int seed = argc > 1 ? atoi(argv[1]) : 1, step, i, crashes = 0, ok = 0;
    disk = calloc(DISK_MB, 1 << 20);
    v.rd = m_rd; v.wr = m_wr; v.tx = malloc((size_t)BFS_TXMAX * BFS_BS);
    srand((unsigned)seed);
    if (bfs_mkfs(&v, DISK_MB * 2048, "PRUEBA") || bfs_mount(&v)) fail("mkfs", seed, 0);
    bfs_mkdir(&v, "/D");
    bfs_mkdir(&v, "/D/Sub carpeta");
    for (i = 0; i < NF; i++)
        snprintf(model[i].name, sizeof model[i].name, "%s/archivo número %d.dat",
                 i % 3 == 0 ? "" : (i % 3 == 1 ? "/D" : "/D/Sub carpeta"), i);
    for (step = 1; step <= 3000; step++) {
        int f = rand() % NF, op = rand() % 10, r;
        int crash = (rand() % 6) == 0 && op < 9;   /* renombrar son dos operaciones: sin apagon */
        size_t sz = 0;
        unsigned char *nd = 0;
        if (op < 6) {                              /* escribir / reemplazar */
            sz = (size_t)(rand() % 4 == 0 ? rand() % 3000000 : rand() % 20000);
            nd = malloc(sz + 1);
            for (i = 0; i < (int)sz; i++) nd[i] = (unsigned char)(rand() ^ step);
        }
        last_op = op; last_crash = crash;
        crashed = 0;
        write_budget = crash ? rand() % 40 : -1;
        if (op < 6) r = bfs_write(&v, model[f].name, nd, sz);
        else if (op < 9) r = bfs_remove(&v, model[f].name);
        else {                                     /* renombrar a otro hueco y de vuelta */
            char tmp[80];
            snprintf(tmp, sizeof tmp, "/temporal %d", step);
            r = bfs_rename(&v, model[f].name, tmp);
            if (!r) r = bfs_rename(&v, tmp, model[f].name);
        }
        write_budget = -1;
        if (crashed) {                             /* apagon: montar de nuevo (repite el diario) */
            crashes++;
            if (bfs_mount(&v)) fail("no monta despues del apagon", seed, step);
            {
                struct bfs_inode in;
                bfs_u32 ino;
                int ex = bfs_lookup(&v, model[f].name, &ino, &in) == 0;
                /* quedo lo viejo o lo nuevo, pero entero */
                if (op < 6 && ex && in.size == sz) {
                    unsigned char *b = malloc(sz + 1);
                    if (bfs_read(&v, &in, 0, b, sz) == (long long)sz && !memcmp(b, nd, sz)) {
                        free(model[f].data); model[f].data = nd; nd = 0; model[f].size = sz; model[f].live = 1;
                    }
                    free(b);
                } else if (op >= 6 && op < 9 && !ex) {
                    model[f].live = 0;
                }
            }
        } else if (op < 6) {
            if (r) fail(bfs_strerror(r), seed, step);
            free(model[f].data); model[f].data = nd; nd = 0; model[f].size = sz; model[f].live = 1;
            ok++;
        } else if (op < 9) {
            if (model[f].live && r) fail("no pudo borrar", seed, step);
            if (!model[f].live && r != BFS_ENOENT) fail("borro algo que no existia", seed, step);
            model[f].live = 0;
            ok++;
        } else ok++;
        free(nd);
        if (step % 50 == 0 || crashed) verify(seed, step);
    }
    if (bfs_mount(&v)) fail("mount final", seed, step);
    verify(seed, step);
    printf("bfstest: semilla %d bien: %d operaciones, %d apagones a media operacion, %llu MB libres\n", seed, ok, crashes,
           (unsigned long long)(v.sb.free_blocks * 4 / 1024));
    return 0;
}
