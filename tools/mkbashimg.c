/* mkbashimg.c - arma pc-basic-linux.img: PC BASIC + particion FAT16 compartida con Linux.
 * uso: mkbashimg salida.img pc-basic.img BZIMAGE INITRD.GZ [archivo...] [--copy ORIGEN=DESTINO ...] [--mib N]
 * LBA 0..4095 = pc-basic.img (sector de arranque con tabla de particiones, kernel, /SYS, SAVE)
 * LBA 4096..  = particion 1 FAT16 "BASICOS" (BZIMAGE, INITRD.GZ y lo que quieras)
 * Escribe la FAT16 directamente (con nombres largos VFAT): no necesita mkfs.vfat ni mtools.
 * Compilar: cc -O2 -o tools/mkbashimg tools/mkbashimg.c */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

#define START 4096u
#define SEC 512u
static void die(const char *m, const char *a) { fprintf(stderr, "mkbashimg: %s%s%s\n", m, a ? ": " : "", a ? a : ""); exit(1); }

typedef struct Node {
    char *name; int isdir; char *src; uint32_t size, first;
    struct Node **kid; int nkid, cap;
} Node;

static Node *newnode(const char *name, int isdir, const char *src) {
    Node *n = calloc(1, sizeof *n);
    n->name = strdup(name); n->isdir = isdir; n->src = src ? strdup(src) : NULL; return n;
}
static Node *find(Node *d, const char *name) {
    for (int i = 0; i < d->nkid; i++) if (!strcasecmp(d->kid[i]->name, name)) return d->kid[i];
    return NULL;
}
static Node *addkid(Node *d, const char *name, int isdir, const char *src) {
    Node *n = find(d, name);
    if (n) { if (!isdir && !n->isdir) { free(n->src); n->src = strdup(src); } return n; }
    n = newnode(name, isdir, src);
    if (d->nkid == d->cap) { d->cap = d->cap ? d->cap * 2 : 8; d->kid = realloc(d->kid, d->cap * sizeof *d->kid); }
    d->kid[d->nkid++] = n; return n;
}
static Node *mkpath(Node *root, char *path, int last_is_dir, const char *src) {
    Node *cur = root; char *save, *c, *next;
    char *p = strdup(path);
    for (c = strtok_r(p, "/", &save); c; c = next) {
        next = strtok_r(NULL, "/", &save);
        cur = addkid(cur, c, next ? 1 : last_is_dir, next ? NULL : src);
    }
    free(p); return cur;
}
static void walk(Node *d, const char *dir) {
    DIR *dp = opendir(dir); if (!dp) die("no puedo abrir carpeta", dir);
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *p; if (asprintf(&p, "%s/%s", dir, e->d_name) < 0) die("memoria", NULL);
        struct stat s; if (stat(p, &s)) die("stat", p);
        if (S_ISDIR(s.st_mode)) walk(addkid(d, e->d_name, 1, NULL), p);
        else if (S_ISREG(s.st_mode)) addkid(d, e->d_name, 0, p);
        free(p);
    }
    closedir(dp);
}

/* ---- FAT16 ---- */
static int out_fd; static off_t pbase;          /* offset de la particion en el archivo */
static uint16_t *fat; static uint32_t nclus, nextc = 2, spc, csize;
static uint32_t data_lba, root_lba, fat_lba, fatsz, rootsec = 32;
static uint16_t fdate, ftime;

static void pw(off_t off, const void *b, size_t n) {
    if (pwrite(out_fd, b, n, pbase + off) != (ssize_t)n) die("escritura (disco lleno?)", strerror(errno));
}
static off_t cofs(uint32_t c) { return ((off_t)data_lba + (off_t)(c - 2) * spc) * SEC; }
static uint32_t alloc(uint32_t n) {             /* n clusters contiguos, devuelve el primero */
    if (!n) n = 1;
    if (nextc + n > nclus + 2) die("la particion se lleno: sube --mib", NULL);
    uint32_t f = nextc;
    for (uint32_t i = 0; i < n; i++) fat[f + i] = (i + 1 < n) ? f + i + 1 : 0xFFFF;
    nextc += n; return f;
}

static uint8_t sfsum(const uint8_t *s) { uint8_t c = 0; for (int i = 0; i < 11; i++) c = ((c & 1) << 7) + (c >> 1) + s[i]; return c; }
static int valid83(const char *n, uint8_t out[11]) {   /* nombre ya valido como 8.3 en mayusculas */
    memset(out, ' ', 11);
    const char *dot = strrchr(n, '.'); size_t bl = dot ? (size_t)(dot - n) : strlen(n), el = dot ? strlen(dot + 1) : 0;
    if (!bl || bl > 8 || el > 3 || (dot && strchr(n, '.') != dot)) return 0;
    for (const char *c = n; *c; c++) {
        if (*c == '.') continue;
        if (*c >= 'a' && *c <= 'z') return 0;
        if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-$~!#%&'()@^`{}", *c)) return 0;
    }
    memcpy(out, n, bl); if (dot) memcpy(out + 8, dot + 1, el); return 1;
}
static uint8_t (*used)[11]; static int nused, capused;
static void used_reset(void) { nused = 0; }
static int alias_used(const uint8_t *s) { for (int i = 0; i < nused; i++) if (!memcmp(used[i], s, 11)) return 1; return 0; }
static void used_add(const uint8_t *s) {
    if (nused == capused) { capused = capused ? capused * 2 : 64; used = realloc(used, capused * 11); }
    memcpy(used[nused++], s, 11);
}
static void alias(const char *name, uint8_t out[11]) {   /* NOMBRE~N.EXT unico en el directorio */
    char base[9] = "", ext[4] = ""; int bl = 0, el = 0;
    const char *dot = strrchr(name, '.');
    for (const char *c = name; *c; c++) {
        if (*c == ' ' || *c == '.') continue;
        char u = (*c >= 'a' && *c <= 'z') ? *c - 32 : *c;
        if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-$~!#%&'()@^`{}", u)) u = '_';
        if (dot && c > dot) { if (el < 3) ext[el++] = u; }
        else if (bl < 6) base[bl++] = u;
    }
    for (int n = 1; n < 1000000; n++) {
        char sfx[12]; int sl = snprintf(sfx, sizeof sfx, "~%d", n);
        memset(out, ' ', 11); memcpy(out, base, bl); memcpy(out + (bl + sl > 8 ? 8 - sl : bl), sfx, sl); memcpy(out + 8, ext, el);
        if (!alias_used(out)) return;
    }
    die("no pude generar nombre corto", name);
}
static int cmpn(const void *a, const void *b) { return strcasecmp((*(Node **)a)->name, (*(Node **)b)->name); }

/* escribe el contenido de d (hijos ya con first/size) en buf; devuelve bytes */
static size_t dirbuild(Node *d, uint32_t self, uint32_t parent, uint8_t *buf, int isroot) {
    size_t o = 0; used_reset();
    #define ENT(a, nm, at, fc, sz) do { memcpy(buf + o, nm, 11); buf[o + 11] = at; memset(buf + o + 12, 0, 20); \
        *(uint16_t *)(buf + o + 14) = ftime; *(uint16_t *)(buf + o + 16) = fdate; *(uint16_t *)(buf + o + 18) = fdate; \
        *(uint16_t *)(buf + o + 22) = ftime; *(uint16_t *)(buf + o + 24) = fdate; \
        *(uint16_t *)(buf + o + 26) = (uint16_t)(fc); *(uint32_t *)(buf + o + 28) = (sz); o += 32; } while (0)
    if (isroot) { ENT(0, "BASICOS    ", 0x08, 0, 0); }
    else { ENT(0, ".          ", 0x10, self, 0); ENT(0, "..         ", 0x10, parent, 0); }
    qsort(d->kid, d->nkid, sizeof *d->kid, cmpn);
    for (int i = 0; i < d->nkid; i++) { uint8_t s[11]; if (valid83(d->kid[i]->name, s)) used_add(s); }
    for (int i = 0; i < d->nkid; i++) {
        Node *k = d->kid[i]; uint8_t s[11];
        int lfn = !valid83(k->name, s);
        if (lfn) { alias(k->name, s); used_add(s);
            size_t L = strlen(k->name); int ne = (int)((L + 12) / 13); uint8_t ck = sfsum(s);
            for (int e = ne; e >= 1; e--) {
                uint8_t *p = buf + o; memset(p, 0, 32);
                p[0] = (uint8_t)(e | (e == ne ? 0x40 : 0)); p[11] = 0x0F; p[13] = ck;
                static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int j = 0; j < 13; j++) {
                    size_t idx = (size_t)(e - 1) * 13 + j; uint16_t ch = idx < L ? (uint8_t)k->name[idx] : idx == L ? 0 : 0xFFFF;
                    memcpy(p + pos[j], &ch, 2);
                }
                o += 32;
            }
        }
        ENT(0, s, k->isdir ? 0x10 : 0x20, k->first, k->isdir ? 0 : k->size);
        if (!isroot && o > (size_t)1 << 30) die("directorio enorme", NULL);
        if (isroot && o > 512 * 32) die("raiz FAT16 llena (maximo 512 entradas)", NULL);
    }
    return o;
}

static uint8_t *dbuf; static size_t dcap;
static size_t dir_entries(Node *d, int isroot) {      /* entradas de 32 bytes que ocupa el directorio */
    size_t n = isroot ? 1 : 2;
    for (int i = 0; i < d->nkid; i++) n += 1 + (strlen(d->kid[i]->name) + 12) / 13;
    return n;
}
static uint32_t place_dir(Node *d, uint32_t parent) {
    /* reservamos el cluster del directorio antes que los hijos (sus ".." lo necesitan) */
    uint32_t pre = 0;
    if (!d->first) {
        /* reservamos el cluster del directorio ya (suficiente para muchos hijos: se reasigna si hace falta) */
        size_t est = dir_entries(d, 0) * 32; uint32_t n = (uint32_t)((est + csize - 1) / csize);
        pre = alloc(n); d->first = pre;
    }
    for (int i = 0; i < d->nkid; i++) {
        Node *k = d->kid[i];
        if (k->isdir) { place_dir(k, d->first); continue; }
        struct stat s; int fd = open(k->src, O_RDONLY); if (fd < 0 || fstat(fd, &s)) die("no puedo leer", k->src);
        k->size = (uint32_t)s.st_size;
        if (!k->size) { k->first = 0; close(fd); continue; }
        k->first = alloc((k->size + csize - 1) / csize);
        static char buf[1 << 20]; off_t off = cofs(k->first); ssize_t n; uint32_t left = k->size;
        while (left && (n = read(fd, buf, left < sizeof buf ? left : sizeof buf)) > 0) { pw(off, buf, n); off += n; left -= n; }
        if (left) die("lectura incompleta", k->src);
        close(fd);
    }
    size_t need = ((dir_entries(d, 0) * 32 + csize - 1) / csize) * csize;
    if (dcap < need) { dcap = need; dbuf = realloc(dbuf, dcap); }
    memset(dbuf, 0, dcap);
    dirbuild(d, d->first, parent, dbuf, 0);
    pw(cofs(d->first), dbuf, need);
    return d->first;
}

int main(int argc, char **argv) {
    const char *src[256], *dstc[256]; int ncopy = 0;
    const char *pos[256]; int npos = 0; unsigned mib = 48;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--copy") && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (!eq) die("--copy necesita ORIGEN=DESTINO", argv[i]);
            *eq = 0; src[ncopy] = argv[i]; dstc[ncopy++] = eq + 1;
        } else if (!strcmp(argv[i], "--mib") && i + 1 < argc) mib = atoi(argv[++i]);
        else pos[npos++] = argv[i];
    }
    if (npos < 4) die("uso: mkbashimg salida.img pc-basic.img BZIMAGE INITRD.GZ [archivo...] [--copy O=D ...] [--mib N]", NULL);
    const char *out = pos[0], *base = pos[1];
    uint64_t total = (uint64_t)mib * 1024 * 1024 / SEC, psect = total - START;

    static uint8_t boot[START * SEC];
    FILE *f = fopen(base, "rb"); if (!f) die("no puedo abrir", base);
    size_t got = fread(boot, 1, sizeof boot, f); fclose(f);
    if (!got) die("base vacia", base);
    for (int i = 0x1BE; i < 0x1FE; i++) if (boot[i]) die("el sector de arranque usa el espacio de la tabla de particiones", NULL);
    uint8_t *e = boot + 0x1BE;
    e[0] = 0x80; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[4] = 0x0E; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    uint32_t s32 = START, p32 = (uint32_t)psect; memcpy(e + 8, &s32, 4); memcpy(e + 12, &p32, 4);
    boot[0x1FE] = 0x55; boot[0x1FF] = 0xAA;

    /* geometria FAT16 */
    uint32_t R = 1;
    for (spc = 1; spc <= 64; spc <<= 1) {
        fatsz = 1;
        for (int it = 0; it < 8; it++) {
            nclus = (uint32_t)((psect - R - rootsec - 2 * fatsz) / spc);
            fatsz = ((nclus + 2) * 2 + SEC - 1) / SEC;
        }
        nclus = (uint32_t)((psect - R - rootsec - 2 * fatsz) / spc);
        if (nclus <= 65000) break;
    }
    if (spc > 64 || nclus < 4085) die("tamano de particion no valido para FAT16 (usa --mib entre 16 y 2000)", NULL);
    csize = spc * SEC; fat_lba = R; root_lba = R + 2 * fatsz; data_lba = root_lba + rootsec;
    fat = calloc(nclus + 2, 2); fat[0] = 0xFFF8; fat[1] = 0xFFFF;
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    fdate = (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    ftime = (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));

    Node *root = newnode("", 1, NULL);
    for (int i = 2; i < npos; i++) {
        const char *nm = strrchr(pos[i], '/'); nm = nm ? nm + 1 : pos[i];
        char up[256]; snprintf(up, sizeof up, "%s", i == 2 ? "BZIMAGE" : i == 3 ? "INITRD.GZ" : nm);
        if (i > 3) for (char *c = up; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        struct stat s; if (stat(pos[i], &s)) die("no existe", pos[i]);
        addkid(root, up, 0, pos[i]);
    }
    for (int i = 0; i < ncopy; i++) {
        printf("preparando %s -> %s ...\n", src[i], dstc[i]); fflush(stdout);
        char *dst = strdup(dstc[i]);
        { struct stat s; if (stat(src[i], &s)) die("no existe", src[i]);
            if (S_ISDIR(s.st_mode)) { Node *d = mkpath(root, dst, 1, NULL); walk(d, src[i]); }
            else mkpath(root, dst, 0, src[i]); }
        free(dst);
    }

    out_fd = open(out, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) die("no puedo crear", out);
    if (ftruncate(out_fd, (off_t)total * SEC)) die("ftruncate", strerror(errno));
    if (pwrite(out_fd, boot, sizeof boot, 0) != (ssize_t)sizeof boot) die("escritura", strerror(errno));
    pbase = (off_t)START * SEC;

    /* datos (raiz tiene su zona fija; sus subdirectorios y archivos van al area de datos) */
    for (int i = 0; i < root->nkid; i++) {
        Node *k = root->kid[i];
        printf("escribiendo %s ...\n", k->name); fflush(stdout);
        if (k->isdir) { place_dir(k, 0); continue; }
        struct stat s; int fd = open(k->src, O_RDONLY); if (fd < 0 || fstat(fd, &s)) die("no puedo leer", k->src);
        k->size = (uint32_t)s.st_size;
        if (!k->size) { close(fd); continue; }
        k->first = alloc((k->size + csize - 1) / csize);
        static char buf[1 << 20]; off_t off = cofs(k->first); ssize_t n; uint32_t left = k->size;
        while (left && (n = read(fd, buf, left < sizeof buf ? left : sizeof buf)) > 0) { pw(off, buf, n); off += n; left -= n; }
        if (left) die("lectura incompleta", k->src);
        close(fd);
    }
    uint8_t *rb = calloc(rootsec, SEC);
    dirbuild(root, 0, 0, rb, 1);
    pw((off_t)root_lba * SEC, rb, (size_t)rootsec * SEC);

    uint8_t bs[SEC]; memset(bs, 0, sizeof bs);
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90; memcpy(bs + 3, "BASICOS ", 8);
    *(uint16_t *)(bs + 11) = SEC; bs[13] = (uint8_t)spc; *(uint16_t *)(bs + 14) = (uint16_t)R; bs[16] = 2;
    *(uint16_t *)(bs + 17) = 512; *(uint16_t *)(bs + 19) = psect < 65536 ? (uint16_t)psect : 0; bs[21] = 0xF8;
    *(uint16_t *)(bs + 22) = (uint16_t)fatsz; *(uint16_t *)(bs + 24) = 63; *(uint16_t *)(bs + 26) = 255;
    *(uint32_t *)(bs + 28) = START; *(uint32_t *)(bs + 32) = psect < 65536 ? 0 : (uint32_t)psect;
    bs[36] = 0x80; bs[38] = 0x29; *(uint32_t *)(bs + 39) = (uint32_t)t; memcpy(bs + 43, "BASICOS    ", 11); memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    pw(0, bs, SEC);
    pw((off_t)fat_lba * SEC, fat, (size_t)(nclus + 2) * 2);
    /* la FAT ocupa fatsz sectores: el resto queda en cero (calloc de la imagen dispersa); segunda copia: */
    pw((off_t)(fat_lba + fatsz) * SEC, fat, (size_t)(nclus + 2) * 2);
    close(out_fd);
    printf("%s: %u MiB, particion FAT16 en LBA %u (%llu sectores, cluster %u B, %u/%u clusters usados)\n",
           out, mib, START, (unsigned long long)psect, csize, nextc - 2, nclus);
    return 0;
}
