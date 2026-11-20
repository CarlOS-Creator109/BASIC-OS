/* ext4.c - lectura de ext2, ext3 y ext4 (las particiones de Linux), solo lectura
 *
 * Superbloque, descriptores de grupo (de 32 o 64 bytes), inodos, archivos por tramos (extents, ext4) o por bloques
 * indirectos (ext2/3), y carpetas (las de indice htree se leen bloque por bloque: sus hojas son entradas normales).
 * No escribe nada ni repasa el diario de ext3/4: si Linux no se apago bien, lo que estaba a medio escribir no se ve.
 * Bloques de 1, 2 o 4 KB. No entiende archivos cifrados ni datos dentro del inodo (inline_data): esos dan error.
 * GPL-3.0-or-later. */
#include "kernel.h"
#include "ext4.h"

#define BMAX 4096u

static int (*xrd)(u64 lba, u32 n, void *buf);  /* sectores de 512 de la particion */
static u32 bs, spb, ipg, isz, desc_sz, first_data;
static u64 gdt_blk;
static u32 incompat;
static u8 xb[BMAX];                             /* bloque de trabajo */
static u8 lvl[5][BMAX];                         /* un bloque por nivel del arbol de tramos o de indirectos */
static u8 inode_buf[256];
static u8 dbuf[BMAX];                           /* bloque de carpeta */

static u32 g16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static u32 g32(const u8 *p) { return g16(p) | g16(p + 2) << 16; }

static int rblk(u64 b, void *buf) { return xrd(b * spb, spb, buf) ? EXT_EIO : 0; }

int ext_mount(int (*rd)(u64 lba, u32 n, void *buf))
{
    u8 *s = xb;
    u32 logbs, rocompat;
    xrd = rd;
    if (rd(2, 2, s))                            /* el superbloque esta en el byte 1024 */
        return EXT_EIO;
    if (g16(s + 56) != 0xEF53)
        return EXT_EFMT;
    logbs = g32(s + 24);
    if (logbs > 2)
        return EXT_EFMT;                        /* bloques de mas de 4 KB */
    bs = 1024u << logbs;
    spb = bs / 512;
    first_data = g32(s + 20);
    ipg = g32(s + 40);
    isz = g32(s + 76) ? g16(s + 88) : 128;
    incompat = g32(s + 76) ? g32(s + 96) : 0;
    rocompat = g32(s + 100);
    (void)rocompat;
    if (incompat & ~(0x2u | 0x4u | 0x40u | 0x80u | 0x200u | 0x2000u | 0x4000u | 0x8000u))
        return EXT_EFMT;                        /* compresion, meta_bg, cifrado, casefold...: no */
    desc_sz = (incompat & 0x80) ? g16(s + 254) : 32;
    if (desc_sz < 32 || desc_sz > 64 || !ipg || isz < 128 || isz > 256 * 4)
        return EXT_EFMT;
    gdt_blk = (u64)first_data + 1;
    return 0;
}

static int read_inode(u32 ino, u8 *out)
{
    u32 g = (ino - 1) / ipg, idx = (ino - 1) % ipg, off;
    u64 dblk, itab, pos;
    if (!ino)
        return EXT_ENOENT;
    off = g * desc_sz;
    dblk = gdt_blk + off / bs;
    if (rblk(dblk, xb))
        return EXT_EIO;
    itab = g32(xb + off % bs + 8);
    if (desc_sz >= 64)
        itab |= (u64)g32(xb + off % bs + 0x28) << 32;
    pos = (u64)idx * isz;
    if (rblk(itab + pos / bs, xb))
        return EXT_EIO;
    memcpy(out, xb + pos % bs, isz < 256 ? isz : 256);
    return 0;
}

static u64 isize(const u8 *in) { return (u64)g32(in + 4) | (u64)g32(in + 108) << 32; }
static int isdir(const u8 *in) { return (g16(in) & 0xF000) == 0x4000; }

/* recorre los bloques del archivo en orden: cb(bloque logico, bloque fisico (0 = hueco), cuantos) */
typedef int (*run_cb)(u64 lb, u64 pb, u32 n, void *ctx);

static int walk_ext(const u8 *node, int level, run_cb cb, void *ctx)
{
    u32 n = g16(node + 2), depth = g16(node + 6), i;
    int r;
    if (g16(node) != 0xF30A || level > 4)
        return EXT_EFMT;
    for (i = 0; i < n; i++) {
        const u8 *e = node + 12 + i * 12;
        if (depth == 0) {
            u32 len = g16(e + 4);
            u64 pb = (u64)g16(e + 6) << 32 | g32(e + 8);
            int uninit = len > 32768;
            if (uninit) len -= 32768;
            if ((r = cb(g32(e), uninit ? 0 : pb, len, ctx)))
                return r;
        } else {
            u64 child = (u64)g16(e + 8) << 32 | g32(e + 4);
            if (rblk(child, lvl[level]))
                return EXT_EIO;
            if ((r = walk_ext(lvl[level], level + 1, cb, ctx)))
                return r;
        }
    }
    return 0;
}

/* ext2/3: bloques indirectos. ind = nivel (1 simple, 2 doble, 3 triple) */
static int walk_ind(u64 blk, int ind, u64 *lb, u64 nblocks, run_cb cb, void *ctx)
{
    u32 per = bs / 4, i;
    int r;
    if (!blk) {                                 /* hueco entero */
        u64 span = 1;
        int k;
        for (k = 0; k < ind; k++) span *= per;
        *lb += span;
        return 0;
    }
    if (rblk(blk, lvl[ind]))
        return EXT_EIO;
    for (i = 0; i < per && *lb < nblocks; i++) {
        u32 b = g32(lvl[ind] + i * 4);
        if (ind == 1) {
            if ((r = cb(*lb, b, 1, ctx)))
                return r;
            (*lb)++;
        } else {
            if ((r = walk_ind(b, ind - 1, lb, nblocks, cb, ctx)))
                return r;
            if (rblk(blk, lvl[ind]))            /* el nivel de abajo uso otro buffer, pero por si acaso */
                return EXT_EIO;
        }
    }
    return 0;
}

static int walk_file(const u8 *in, run_cb cb, void *ctx)
{
    u64 nblocks = (isize(in) + bs - 1) / bs, lb = 0;
    u32 flags = g32(in + 32), i;
    int r;
    if (flags & 0x10000000u)
        return EXT_EFMT;                        /* inline_data */
    if (flags & 0x80000u) {
        u8 root[60];
        memcpy(root, in + 40, 60);
        return walk_ext(root, 0, cb, ctx);
    }
    for (i = 0; i < 12 && lb < nblocks; i++, lb++)
        if ((r = cb(lb, g32(in + 40 + i * 4), 1, ctx)))
            return r;
    for (i = 0; i < 3 && lb < nblocks; i++)
        if ((r = walk_ind(g32(in + 40 + (12 + i) * 4), (int)i + 1, &lb, nblocks, cb, ctx)))
            return r;
    return 0;
}

/* ---- leer un archivo ---- */
struct rdx { u8 *buf; u64 max; };
static int read_cb(u64 lb, u64 pb, u32 n, void *ctx)
{
    struct rdx *x = ctx;
    u64 off = lb * bs, len = (u64)n * bs;
    if (off >= x->max)
        return 0;
    if (off + len > x->max)
        len = x->max - off;
    if (!pb)
        return 0;                               /* hueco: ya esta en ceros */
    if (len % bs == 0) {
        u64 k = 0;
        while (k < len) {                       /* de a 256 KB */
            u64 c = len - k > 262144 ? 262144 : len - k;
            if (xrd((pb + k / bs) * spb, (u32)(c / 512), x->buf + off + k))
                return EXT_EIO;
            k += c;
        }
    } else {
        u64 whole = len / bs * bs;
        if (whole && xrd(pb * spb, (u32)(whole / 512), x->buf + off))
            return EXT_EIO;
        if (rblk(pb + whole / bs, xb))
            return EXT_EIO;
        memcpy(x->buf + off + whole, xb, len - whole);
    }
    return 0;
}

long long ext_read(u32 ino, void *buf, u64 max, u64 *total)
{
    u8 *in = inode_buf;
    struct rdx x;
    u64 sz;
    int r;
    if ((r = read_inode(ino, in)))
        return r;
    sz = isize(in);
    if (total)
        *total = sz;
    x.buf = buf;
    x.max = sz < max ? sz : max;
    memset(buf, 0, x.max);
    if ((r = walk_file(in, read_cb, &x)))
        return r;
    return (long long)x.max;
}

/* ---- carpetas ---- */
struct dx { ext_dir_cb cb; const char *want; int wlen; u32 found; int found_dir; int done; u64 left; };
static int dir_cb(u64 lb, u64 pb, u32 n, void *ctx)
{
    struct dx *d = ctx;
    u32 k;
    (void)lb;
    for (k = 0; k < n && !d->done && d->left; k++) {
        u32 off = 0, lim = bs;
        if (!pb) { d->left -= d->left < bs ? d->left : bs; continue; }
        if (rblk(pb + k, dbuf))
            return EXT_EIO;
        if (d->left < lim) lim = (u32)d->left;
        d->left -= lim;
        while (off + 8 <= lim) {
            const u8 *e = dbuf + off;
            u32 ino = g32(e), rec = g16(e + 4), nl = e[6];
            if (rec < 8 || off + rec > bs)
                break;
            if (ino && nl && !(nl == 1 && e[8] == '.') && !(nl == 2 && e[8] == '.' && e[9] == '.')) {
                int dir = e[7] == 2;
                if (d->want) {
                    int i, same = (int)nl == d->wlen;
                    for (i = 0; same && i < d->wlen; i++) {
                        int a = e[8 + i], b = (u8)d->want[i];
                        if (a >= 'a' && a <= 'z') a -= 32;
                        if (b >= 'a' && b <= 'z') b -= 32;
                        same = a == b;
                    }
                    if (same) { d->found = ino; d->found_dir = dir; d->done = 1; return 0; }
                } else {
                    char nm[256];
                    u32 i;
                    for (i = 0; i < nl; i++) nm[i] = (char)e[8 + i];
                    nm[nl] = 0;
                    d->cb(nm, ino, dir);
                }
            }
            off += rec;
        }
    }
    return 0;
}

static int dir_walk(u32 ino, struct dx *d)
{
    u8 in[256];
    int r;
    if ((r = read_inode(ino, in)))
        return r;
    if (!isdir(in))
        return EXT_ENOTDIR;
    d->left = isize(in);
    return walk_file(in, dir_cb, d);
}

int ext_list(u32 ino, ext_dir_cb cb)
{
    struct dx d = { cb, 0, 0, 0, 0, 0, 0 };
    return dir_walk(ino, &d);
}

/* ruta "/home/carlos/x.txt" -> inodo (sin importar mayusculas) */
int ext_lookup(const char *path, u32 *ino, int *dir, u64 *size)
{
    u32 cur = 2;                                /* la raiz */
    int cdir = 1;
    u8 in[256];
    int r;
    for (;;) {
        int n = 0;
        struct dx d = { 0, 0, 0, 0, 0, 0, 0 };
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        while (path[n] && path[n] != '/' && path[n] != '\\') n++;
        if (!cdir) return EXT_ENOENT;
        d.want = path;
        d.wlen = n;
        if ((r = dir_walk(cur, &d))) return r;
        if (!d.done) return EXT_ENOENT;
        cur = d.found;
        cdir = d.found_dir;
        path += n;
    }
    if ((r = read_inode(cur, in))) return r;
    *ino = cur;
    *dir = isdir(in);
    if (size) *size = isize(in);
    return 0;
}

/* inodo -> datos del archivo (para la info de ext_list) */
int ext_stat(u32 ino, u64 *size, int *dir)
{
    u8 in[256];
    int r = read_inode(ino, in);
    if (r) return r;
    if (size) *size = isize(in);
    if (dir) *dir = isdir(in);
    return 0;
}
