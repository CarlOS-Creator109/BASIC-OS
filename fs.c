/* fs.c - FAT12/16/32 e ISO9660 (solo lectura, nombres 8.3 / ISO nivel 1) y BFS (bfs.c: lectura y escritura,
 * nombres largos). Sin libc. */
#include "kernel.h"
#include "fs.h"
#include "sys.h"
#include "kapi.h"
#include "bfs.h"
#include "ext4.h"

#define SECBUF 4096

static struct blk *dev;
static u32 part_lba;                    /* inicio de la particion (sectores del dispositivo) */
static int is_iso;
static int is_bfs;
static int is_ext;                      /* ext2/3/4 (ext4.c, solo lectura) */

/* ---- BFS: el volumen montado y sus lecturas/escrituras por sectores ---- */
static struct bfs kbfs;
/* el diario necesita 512 KB: los ultimos del banco SISTEMA (0xB000000-0xC000000), que nadie mas usa. Fijo y no un
 * banco aparte para no caer encima de la memoria de programas que usan direcciones fijas (INSTALL desde 0xC000000). */
#define BFS_TXMEM 0xBF80000ull
static int kb_io(int write, u64 lba, u32 n, u8 *p)
{
    while (n) {
        u32 c = n > 32 ? 32 : n;
        if (write ? blk_write(dev, (u64)part_lba + lba, (int)c, p) : blk_read(dev, (u64)part_lba + lba, (int)c, p))
            return 1;
        p += c * 512;
        lba += c;
        n -= c;
    }
    return 0;
}
static int kb_rd(void *ctx, u64 lba, u32 n, void *buf) { (void)ctx; return kb_io(0, lba, n, buf); }
static int kb_wr(void *ctx, u64 lba, u32 n, const void *buf) { (void)ctx; return kb_io(1, lba, n, (u8 *)buf); }
static int bfs_try(u32 lba0)
{
    part_lba = lba0;
    if (dev->bps != 512)
        return FS_EFMT;
    kbfs.rd = kb_rd;
    kbfs.wr = kb_wr;
    if (!kbfs.tx && ram_ok(BFS_TXMEM + 0x80000ull - 1))  /* memoria para el diario (sin ella: solo leer) */
        kbfs.tx = (u8 *)(unsigned long)BFS_TXMEM;
    if (bfs_mount(&kbfs))
        return FS_EFMT;
    is_bfs = 1;
    is_iso = 0;
    return FS_OK;
}
static int ext_rd(u64 lba, u32 n, void *buf) { return kb_io(0, lba, n, buf); }
static int ext_try(u32 lba0)
{
    part_lba = lba0;
    if (dev->bps != 512 || ext_mount(ext_rd))
        return FS_EFMT;
    is_ext = 1;
    is_iso = 0;
    return FS_OK;
}

static int bfs_err(int e)
{
    switch (e) {
    case 0: return FS_OK;
    case BFS_ENOENT: case BFS_ENOTDIR: return FS_ENOENT;
    case BFS_EFMT: return FS_EFMT;
    case BFS_ENOSPC: return FS_ENOSPC;
    case BFS_EEXIST: return FS_EEXIST;
    case BFS_EISDIR: return FS_EISDIR;
    case BFS_ENOTEMPTY: return FS_ENOTEMPTY;
    case BFS_ENAME: return FS_ENAME;
    case BFS_E2BIG: return FS_E2BIG;
    case BFS_EBUSY: return FS_EROFS;
    }
    return FS_EIO;
}

/* ---- FAT ---- */
static u32 bpb_bps, spc, rsvd, nfats, fatsz, rootsecs, first_data, root_clus, nclusters;
static u32 fat_lba, root_lba;
static int fat_type;                    /* 12, 16, 32 */

static u8 sec[SECBUF];                  /* buffer para directorios y datos */
static u8 fsec[SECBUF];                 /* cache de un sector de la FAT */
static u32 fsec_lba = 0xFFFFFFFFu;

static u32 le16(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8); }
static u32 le32(const u8 *p) { return le16(p) | (le16(p + 2) << 16); }

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Lee n sectores del dispositivo (del volumen: relativos a part_lba) */
static int rd(u32 lba, int n, void *buf)
{
    int chunk;
    u8 *p = buf;
    while (n > 0) {
        chunk = n > 32 ? 32 : n;
        if (blk_read(dev, (u64)part_lba + lba, chunk, p))
            return 1;
        p += (size_t)chunk * dev->bps;
        lba += (u32)chunk;
        n -= chunk;
    }
    return 0;
}

static int fsec_dirty;
static u32 fsi_sec;                     /* FAT32: sector de FSInfo (0 = no hay) */
static int wr(u32 lba, int n, const void *buf)
{
    const u8 *p = buf;
    while (n > 0) {
        int c = n > 32 ? 32 : n;
        if (blk_write(dev, (u64)part_lba + lba, c, p))
            return 1;
        p += (size_t)c * 512;
        lba += (u32)c;
        n -= c;
    }
    return 0;
}
/* el sector de la FAT que esta en memoria, si se cambio, va a todas las copias de la FAT */
static int fat_flush(void)
{
    u32 k;
    if (!fsec_dirty)
        return 0;
    for (k = 0; k < nfats; k++)
        if (wr(fat_lba + k * fatsz + fsec_lba, 1, fsec))
            return 1;
    fsec_dirty = 0;
    return 0;
}

static int fat_byte(u32 off)
{
    u32 s = off / bpb_bps;
    if (fsec_lba != s) {
        if (fat_flush())
            return -1;
        if (rd(fat_lba + s, 1, fsec))
            return -1;
        fsec_lba = s;
    }
    return fsec[off % bpb_bps];
}

/* Siguiente cluster; >= 0x0FFFFFF8 = fin; < 0 = error */
static long fat_next(u32 c)
{
    int a, b, d, e;
    if (fat_type == 12) {
        u32 off = c + c / 2;
        u32 v;
        a = fat_byte(off);
        b = fat_byte(off + 1);
        if (a < 0 || b < 0)
            return -1;
        v = (u32)a | ((u32)b << 8);
        v = (c & 1) ? v >> 4 : v & 0xFFF;
        return v >= 0xFF8 ? 0x0FFFFFFF : (long)v;
    }
    if (fat_type == 16) {
        u32 off = c * 2, v;
        a = fat_byte(off);
        b = fat_byte(off + 1);
        if (a < 0 || b < 0)
            return -1;
        v = (u32)a | ((u32)b << 8);
        return v >= 0xFFF8 ? 0x0FFFFFFF : (long)v;
    }
    {
        u32 off = c * 4, v;
        a = fat_byte(off);
        b = fat_byte(off + 1);
        d = fat_byte(off + 2);
        e = fat_byte(off + 3);
        if (a < 0 || b < 0 || d < 0 || e < 0)
            return -1;
        v = ((u32)a | ((u32)b << 8) | ((u32)d << 16) | ((u32)e << 24)) & 0x0FFFFFFF;
        return v >= 0x0FFFFFF8 ? 0x0FFFFFFF : (long)v;
    }
}

static u32 clus_lba(u32 c) { return first_data + (c - 2) * spc; }

static int fat_mount(u32 lba0)
{
    u8 *b = sec;
    u32 total, datasec;

    part_lba = lba0;
    if (rd(0, 1, b))
        return FS_EIO;
    if (b[510] != 0x55 || b[511] != 0xAA)
        return FS_EFMT;
    bpb_bps = le16(b + 11);
    spc = b[13];
    rsvd = le16(b + 14);
    nfats = b[16];
    if (bpb_bps != dev->bps || !spc || (spc & (spc - 1)) || !nfats || !rsvd)
        return FS_EFMT;
    fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    rootsecs = (le16(b + 17) * 32 + bpb_bps - 1) / bpb_bps;
    if (!fatsz || total < rsvd + nfats * fatsz + rootsecs)
        return FS_EFMT;
    datasec = total - (rsvd + nfats * fatsz + rootsecs);
    nclusters = datasec / spc;
    fat_type = nclusters < 4085 ? 12 : (nclusters < 65525 ? 16 : 32);
    fat_lba = rsvd;
    root_lba = rsvd + nfats * fatsz;
    first_data = root_lba + rootsecs;
    root_clus = fat_type == 32 ? le32(b + 44) : 0;
    fsi_sec = fat_type == 32 ? le16(b + 48) : 0;
    if (fsi_sec >= rsvd) fsi_sec = 0;
    fsec_lba = 0xFFFFFFFFu;
    fsec_dirty = 0;
    is_iso = 0;
    return FS_OK;
}

/* ---- ISO9660 ---- */
static u32 iso_root_lba, iso_root_size;

static int iso_rd(u32 lba2k, int n, void *buf)      /* sectores de 2048 */
{
    u32 f = 2048 / dev->bps;
    if (dev->bps > 2048)
        return 1;
    return blk_read(dev, (u64)lba2k * f, n * (int)f, buf);
}

static int iso_mount(void)
{
    u8 *b = sec;
    if (dev->bps > 2048)
        return FS_EFMT;
    if (iso_rd(16, 1, b))
        return FS_EIO;
    if (b[0] != 1 || b[1] != 'C' || b[2] != 'D' || b[3] != '0' || b[4] != '0' || b[5] != '1')
        return FS_EFMT;
    iso_root_lba = le32(b + 156 + 2);
    iso_root_size = le32(b + 156 + 10);
    is_iso = 1;
    return FS_OK;
}

/* ---- entradas de directorio comunes ---- */
struct ent {
    char name[256];                     /* UTF-8 (nombre largo de FAT, si lo tiene) */
    u32 size, clus;                     /* clus = primer cluster (FAT) o LBA de extension (ISO) */
    int dir;
};

typedef int (*ent_cb)(const struct ent *e, void *ctx);   /* distinto de 0 = parar */

static void fat_name(const u8 *d, char *out)
{
    int i = 0, k;
    for (k = 0; k < 8 && d[k] != ' '; k++)
        out[i++] = (char)d[k];
    if (d[8] != ' ') {
        out[i++] = '.';
        for (k = 8; k < 11 && d[k] != ' '; k++)
            out[i++] = (char)d[k];
    }
    out[i] = 0;
}

/* ---- nombres largos de FAT (VFAT): se juntan las entradas 0x0F que van antes de la corta ---- */
static u16 lfn[260];
static int lfn_ok, lfn_sum;
static u8 sfn_sum(const u8 *d)
{
    u8 s = 0;
    int i;
    for (i = 0; i < 11; i++) s = (u8)(((s & 1) << 7) + (s >> 1) + d[i]);
    return s;
}
static void lfn_part(const u8 *d)
{
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int seq = d[0] & 0x1F, k;
    if (d[0] & 0x40) {                              /* la ultima parte va primero */
        for (k = 0; k < 260; k++) lfn[k] = 0;
        lfn_ok = 1;
        lfn_sum = d[13];
    }
    if (!lfn_ok || seq < 1 || seq > 20 || d[13] != lfn_sum) { lfn_ok = 0; return; }
    for (k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = (u16)le16(d + pos[k]);
}
static int utf8_put(char *o, int n, int max, u32 c)
{
    if (c < 0x80) { if (n < max) o[n++] = (char)c; }
    else if (c < 0x800) { if (n + 1 < max) { o[n++] = (char)(0xC0 | c >> 6); o[n++] = (char)(0x80 | (c & 63)); } }
    else if (n + 2 < max) { o[n++] = (char)(0xE0 | c >> 12); o[n++] = (char)(0x80 | ((c >> 6) & 63)); o[n++] = (char)(0x80 | (c & 63)); }
    return n;
}
/* entrada corta de 32 bytes -> ent (con su nombre largo si lo trae). 0 = no es una entrada que se muestre */
static int fat_entry(const u8 *d, struct ent *e)
{
    if (d[0] == 0xE5) { lfn_ok = 0; return 0; }
    if (d[11] == 0x0F) { lfn_part(d); return 0; }
    if (d[11] & 0x08) { lfn_ok = 0; return 0; }
    if (lfn_ok && lfn_sum == sfn_sum(d)) {
        int k, n = 0;
        for (k = 0; k < 260 && lfn[k] && lfn[k] != 0xFFFF; k++) n = utf8_put(e->name, n, 255, lfn[k]);
        e->name[n] = 0;
    } else {
        fat_name(d, e->name);
    }
    lfn_ok = 0;
    e->size = le32(d + 28);
    e->clus = le16(d + 26) | (le16(d + 20) << 16);
    e->dir = (d[11] & 0x10) != 0;
    return 1;
}

/* Recorre un directorio. clus: primer cluster (0 = raiz FAT12/16; en ISO = LBA de la extension) */
static int walk_dir(u32 clus, u32 isosize, ent_cb cb, void *ctx)
{
    struct ent e;
    u32 i;

    if (is_iso) {
        u32 pos = 0, lba = clus;
        u8 *s = sec;
        while (pos < isosize) {
            u32 off = 0;
            if (iso_rd(lba, 1, s))
                return FS_EIO;
            while (off < 2048) {
                u32 len = s[off], nl;
                if (!len)
                    break;                          /* resto del sector sin usar */
                nl = s[off + 32];
                if (!(nl == 1 && (s[off + 33] == 0 || s[off + 33] == 1))) {
                    u32 k, n = 0;
                    for (k = 0; k < nl && n < 38; k++) {
                        char c = (char)s[off + 33 + k];
                        if (c == ';')
                            break;
                        e.name[n++] = c;
                    }
                    if (n && e.name[n - 1] == '.')
                        n--;
                    e.name[n] = 0;
                    e.clus = le32(s + off + 2);
                    e.size = le32(s + off + 10);
                    e.dir = (s[off + 25] & 2) != 0;
                    if (cb(&e, ctx))
                        return FS_OK;
                }
                off += len;
            }
            lba++;
            pos += 2048;
        }
        return FS_OK;
    }

    lfn_ok = 0;
    if (clus == 0 && fat_type != 32) {              /* raiz fija de FAT12/16 */
        for (i = 0; i < rootsecs; i++) {
            u32 off;
            if (rd(root_lba + i, 1, sec))
                return FS_EIO;
            for (off = 0; off < bpb_bps; off += 32) {
                const u8 *d = sec + off;
                if (d[0] == 0)
                    return FS_OK;
                if (!fat_entry(d, &e))
                    continue;
                if (cb(&e, ctx))
                    return FS_OK;
            }
        }
        return FS_OK;
    }
    if (clus == 0)
        clus = root_clus;
    while (clus >= 2 && clus < 0x0FFFFFF8) {
        u32 s2;
        long nx;
        for (s2 = 0; s2 < spc; s2++) {
            u32 off;
            if (rd(clus_lba(clus) + s2, 1, sec))
                return FS_EIO;
            for (off = 0; off < bpb_bps; off += 32) {
                const u8 *d = sec + off;
                if (d[0] == 0)
                    return FS_OK;
                if (!fat_entry(d, &e))
                    continue;
                if (cb(&e, ctx))
                    return FS_OK;
            }
        }
        nx = fat_next(clus);
        if (nx < 0)
            return FS_EIO;
        clus = (u32)nx;
    }
    return FS_OK;
}

/* ---- rutas ---- */
struct find { const char *name; int len; struct ent found; int ok; };

static int find_cb(const struct ent *e, void *ctx)
{
    struct find *f = ctx;
    int i;
    for (i = 0; i < f->len; i++)
        if (up(e->name[i]) != up(f->name[i]))
            return 0;
    if (e->name[f->len])
        return 0;
    f->found = *e;
    f->ok = 1;
    return 1;
}

/* Interpreta "HD0:" / "HD1.2:" y deja montado el volumen. Devuelve el resto de la ruta. */
static int mount_spec(const char *spec, const char **rest)
{
    int n = 0, len, di, part = 0, r, i;
    const char *p;
    u8 mbr[512];

    while (spec[n] && spec[n] != ':')
        n++;
    if (!spec[n])
        return FS_ENODEV;
    *rest = spec + n + 1;
    len = n;
    for (i = 0; i < n; i++)
        if (spec[i] == '.') {
            len = i;
            part = spec[i + 1] - '0';
            break;
        }
    di = blk_find(spec, len);
    if (di < 0)
        return FS_ENODEV;
    dev = blk_get(di);
    is_bfs = 0;
    is_ext = 0;
    p = spec;
    (void)p;

    r = iso_mount();
    if (r == FS_OK)
        return FS_OK;
    if (r == FS_EIO && dev->bps != 512)
        return FS_EIO;
    part_lba = 0;
    if (blk_read(dev, 0, 1, mbr))
        return FS_EIO;
    if (!part) {                                    /* sin particion: disquete o primera FAT (o BFS) */
        if (fat_mount(0) == FS_OK)
            return FS_OK;
        for (i = 0; i < 4; i++) {
            u8 t = mbr[446 + i * 16 + 4];
            if (t == BFS_PTYPE && mbr[510] == 0x55 && bfs_try(le32(mbr + 446 + i * 16 + 8)) == FS_OK)
                return FS_OK;
            if (t == 0x83 && mbr[510] == 0x55 && ext_try(le32(mbr + 446 + i * 16 + 8)) == FS_OK)
                return FS_OK;
            if ((t == 1 || t == 4 || t == 6 || t == 0xB || t == 0xC || t == 0xE) &&
                fat_mount(le32(mbr + 446 + i * 16 + 8)) == FS_OK)
                return FS_OK;
        }
        return FS_EFMT;
    }
    if (part < 1 || part > 4 || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return FS_EFMT;
    if (mbr[446 + (part - 1) * 16 + 4] == BFS_PTYPE)
        return bfs_try(le32(mbr + 446 + (part - 1) * 16 + 8));
    if (mbr[446 + (part - 1) * 16 + 4] == 0x83)        /* Linux: ext2/3/4 */
        return ext_try(le32(mbr + 446 + (part - 1) * 16 + 8));
    return fat_mount(le32(mbr + 446 + (part - 1) * 16 + 8));
}

/* Sigue la ruta hasta un directorio/archivo. dir_only = 1: debe ser directorio o vacio */
static int resolve(const char *path, struct ent *out)
{
    struct find f;
    struct ent cur;
    int r;

    cur.dir = 1;
    cur.clus = is_iso ? iso_root_lba : 0;
    cur.size = is_iso ? iso_root_size : 0;
    cur.name[0] = 0;
    for (;;) {
        int n = 0;
        while (*path == '/' || *path == '\\')
            path++;
        if (!*path)
            break;
        while (path[n] && path[n] != '/' && path[n] != '\\')
            n++;
        if (!cur.dir)
            return FS_ENOENT;
        f.name = path;
        f.len = n;
        f.ok = 0;
        r = walk_dir(cur.clus, cur.size, find_cb, &f);
        if (r)
            return r;
        if (!f.ok)
            return FS_ENOENT;
        cur = f.found;
        if (!is_iso && cur.dir && cur.clus == 0)
            cur.clus = 0;                           /* ".." a la raiz */
        path += n;
    }
    *out = cur;
    return FS_OK;
}

static fs_cb user_cb;
static int list_cb(const struct ent *e, void *ctx)
{
    (void)ctx;
    user_cb(e->name, e->size, e->dir);
    return 0;
}

static int bfs_list_cb(void *ctx, const char *name, int nlen, u32 ino, int type, u64 size)
{
    (void)ctx; (void)nlen; (void)ino;
    user_cb(name, size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)size, type == BFS_T_DIR);
    return 0;
}

static void ext_list_cb(const char *name, u32 ino, int dir)
{
    u64 sz = 0;
    if (!dir) ext_stat(ino, &sz, 0);
    user_cb(name, sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)sz, dir);
}

static int is_sys(const char *spec, const char **rest)
{
    if (up(spec[0]) == 'S' && up(spec[1]) == 'Y' && up(spec[2]) == 'S' && spec[3] == ':') {
        *rest = spec + 4;
        return 1;
    }
    return 0;
}

static fs_cb sys_user_cb;
static void sys_list_cb(const char *name, unsigned int size)
{
    sys_user_cb(name, size, 0);
}

int fs_dir(const char *spec, fs_cb cb)
{
    const char *rest;
    struct ent e;
    int r;

    if (is_sys(spec, &rest)) {                      /* SYS: carpeta /SYS de la imagen */
        sys_user_cb = cb;
        return sys_list(sys_list_cb);
    }
    r = mount_spec(spec, &rest);
    if (r)
        return r;
    user_cb = cb;
    if (is_bfs) {
        struct bfs_inode in;
        u32 ino;
        r = bfs_lookup(&kbfs, rest, &ino, &in);
        if (r)
            return bfs_err(r);
        if (in.type != BFS_T_DIR)
            return FS_ENOENT;
        return bfs_err(bfs_list(&kbfs, ino, bfs_list_cb, NULL));
    }
    if (is_ext) {
        u32 ino;
        int dir;
        if (ext_lookup(rest, &ino, &dir, 0)) return FS_ENOENT;
        if (!dir) return FS_ENOENT;
        return ext_list(ino, ext_list_cb) ? FS_EIO : FS_OK;
    }
    r = resolve(rest, &e);
    if (r)
        return r;
    if (!e.dir)
        return FS_ENOENT;
    return walk_dir(e.clus, e.size, list_cb, NULL);
}

int fs_read(const char *spec, char *buf, int max, unsigned int *total)
{
    const char *rest;
    struct ent e;
    u32 left, got = 0;
    int r;

    if (is_sys(spec, &rest))
        return sys_read(rest, buf, max, total);
    r = mount_spec(spec, &rest);
    if (r)
        return r;
    if (is_bfs) {
        struct bfs_inode in;
        long long n;
        r = bfs_lookup(&kbfs, rest, 0, &in);
        if (r)
            return bfs_err(r);
        if (in.type == BFS_T_DIR)
            return FS_EISDIR;
        if (total)
            *total = in.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)in.size;
        n = bfs_read(&kbfs, &in, 0, buf, (u64)(in.size < (u64)(unsigned)max ? in.size : (u64)(unsigned)max));
        return n < 0 ? FS_EIO : (int)n;
    }
    if (is_ext) {
        u32 ino;
        int dir;
        u64 sz;
        long long n;
        if (ext_lookup(rest, &ino, &dir, &sz)) return FS_ENOENT;
        if (dir) return FS_EISDIR;
        if (total) *total = sz > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)sz;
        n = ext_read(ino, buf, (u64)(unsigned)max, 0);
        return n < 0 ? FS_EIO : (int)n;
    }
    r = resolve(rest, &e);
    if (r)
        return r;
    if (e.dir)
        return FS_EISDIR;
    if (total)
        *total = e.size;
    left = e.size < (u32)max ? e.size : (u32)max;

    if (is_iso) {
        u32 lba = e.clus;
        while (got < left) {
            u32 n = left - got < 2048 ? left - got : 2048, k;
            if (iso_rd(lba, 1, sec))
                return FS_EIO;
            for (k = 0; k < n; k++)
                buf[got + k] = (char)sec[k];
            got += n;
            lba++;
        }
        return (int)got;
    }
    {
        /* clusters seguidos en disco = una sola lectura grande (antes: un comando por sector de 512 B) */
        u32 clus = e.clus;
        while (got < left && clus >= 2 && clus < 0x0FFFFFF8) {
            u32 first = clus, run = 1, secs, want_secs, whole;
            long nx = fat_next(clus);
            if (nx < 0)
                return FS_EIO;
            while ((u32)nx == first + run && (u64)run * spc * bpb_bps < left - got && run < 4096) {
                run++;
                nx = fat_next(first + run - 1);
                if (nx < 0)
                    return FS_EIO;
            }
            secs = run * spc;
            want_secs = (left - got + bpb_bps - 1) / bpb_bps;
            if (want_secs < secs)
                secs = want_secs;
            whole = (left - got) / bpb_bps;             /* sectores completos que van directo al buffer */
            if (whole > secs)
                whole = secs;
            if (whole && rd(clus_lba(first), (int)whole, buf + got))
                return FS_EIO;
            got += whole * bpb_bps;
            if (whole < secs) {                          /* cola: un sector parcial */
                u32 n = left - got, k;
                if (rd(clus_lba(first) + whole, 1, sec))
                    return FS_EIO;
                for (k = 0; k < n; k++)
                    buf[got + k] = (char)sec[k];
                got += n;
            }
            clus = (u32)nx;
        }
    }
    return (int)got;
}

/* ================= escritura en FAT16 / FAT32 (con nombres largos) =================
 * Archivo nuevo o reemplazado, carpeta nueva, borrar, renombrar/mover y dar formato. Los datos se escriben primero,
 * luego la FAT (en todas sus copias) y al final la entrada de la carpeta. FAT no tiene diario: si se va la luz a la mitad
 * pueden quedar clusters perdidos (no se pierde nada de lo que ya estaba). FAT12 (disquetes) no se escribe. */
#define DSMAX 4096
static u32 dsec[DSMAX], nds, dlast;     /* sectores de la carpeta que se esta tocando y su ultimo cluster */
static u8 dbuf[512];
static u32 dbuf_lba = 0xFFFFFFFFu;
static u32 fat_hint;
static long free_delta;
static u8 zsec[512];

static u32 eoc(void) { return fat_type == 32 ? 0x0FFFFFFFu : 0xFFFFu; }
static int is_end(u32 v) { return fat_type == 32 ? v >= 0x0FFFFFF8u : v >= 0xFFF8u; }

static long fat_get(u32 c)
{
    int a, b, d, e;
    if (fat_type == 16) {
        a = fat_byte(c * 2); b = fat_byte(c * 2 + 1);
        return (a < 0 || b < 0) ? -1 : (long)((u32)a | (u32)b << 8);
    }
    a = fat_byte(c * 4); b = fat_byte(c * 4 + 1); d = fat_byte(c * 4 + 2); e = fat_byte(c * 4 + 3);
    if (a < 0 || b < 0 || d < 0 || e < 0) return -1;
    return (long)(((u32)a | (u32)b << 8 | (u32)d << 16 | (u32)e << 24) & 0x0FFFFFFFu);
}
static int fat_set(u32 c, u32 v)
{
    u32 off = c * (fat_type == 32 ? 4 : 2), o;
    if (fat_byte(off) < 0) return 1;
    o = off % bpb_bps;
    fsec[o] = (u8)v; fsec[o + 1] = (u8)(v >> 8);
    if (fat_type == 32) { fsec[o + 2] = (u8)(v >> 16); fsec[o + 3] = (u8)((fsec[o + 3] & 0xF0) | ((v >> 24) & 0x0F)); }
    fsec_dirty = 1;
    return 0;
}
static int free_chain(u32 c)
{
    while (c >= 2 && c < nclusters + 2) {
        long v = fat_get(c);
        if (v < 0 || fat_set(c, 0)) return FS_EIO;
        free_delta++;
        if (is_end((u32)v) || v < 2) break;
        c = (u32)v;
    }
    return 0;
}
/* n clusters encadenados (seguidos si se puede) */
static int alloc_chain(u32 n, u32 *first)
{
    u32 c = (fat_hint >= 2 && fat_hint < nclusters + 2) ? fat_hint : 2, scanned = 0, prev = 0, got = 0;
    *first = 0;
    while (got < n && scanned < nclusters) {
        long v = fat_get(c);
        if (v < 0) return FS_EIO;
        if (v == 0) {
            if (fat_set(c, eoc()) || (prev && fat_set(prev, c))) return FS_EIO;
            if (!prev) *first = c;
            prev = c;
            got++;
            free_delta--;
        }
        if (++c >= nclusters + 2) c = 2;
        scanned++;
    }
    fat_hint = c;
    if (got < n) {
        free_chain(*first);
        *first = 0;
        return FS_ENOSPC;
    }
    return 0;
}
static int zero_clus(u32 c)
{
    u32 k;
    for (k = 0; k < spc; k++)
        if (wr(clus_lba(c) + k, 1, zsec)) return FS_EIO;
    return 0;
}
static int write_chain(u32 c, const u8 *data, u32 size)
{
    u32 csz = spc * 512, done = 0;
    while (done < size && c >= 2 && c < nclusters + 2) {
        u32 run = 1, bytes, whole;
        long nx = fat_get(c);
        if (nx < 0) return FS_EIO;
        while ((u32)nx == c + run && (u64)(run + 1) * csz <= (u64)(size - done) + csz - 1 && run < 2048) {
            nx = fat_get(c + run);
            if (nx < 0) return FS_EIO;
            run++;
        }
        bytes = size - done < run * csz ? size - done : run * csz;
        whole = bytes / 512;
        if (whole && wr(clus_lba(c), (int)whole, data + done)) return FS_EIO;
        if (bytes % 512) {
            u32 k;
            for (k = 0; k < 512; k++) sec[k] = k < bytes % 512 ? data[done + whole * 512 + k] : 0;
            if (wr(clus_lba(c) + whole, 1, sec)) return FS_EIO;
        }
        done += bytes;
        c = (u32)nx;
    }
    return done < size ? FS_EIO : 0;
}
static int fsinfo_update(void)
{
    if (fat_flush()) return FS_EIO;
    if (fsi_sec && !rd(fsi_sec, 1, sec) && le32(sec) == 0x41615252u) {
        u32 fr = le32(sec + 488);
        if (fr != 0xFFFFFFFFu) fr = (u32)((long)fr + free_delta);
        sec[488] = (u8)fr; sec[489] = (u8)(fr >> 8); sec[490] = (u8)(fr >> 16); sec[491] = (u8)(fr >> 24);
        sec[492] = (u8)fat_hint; sec[493] = (u8)(fat_hint >> 8); sec[494] = (u8)(fat_hint >> 16); sec[495] = (u8)(fat_hint >> 24);
        if (wr(fsi_sec, 1, sec)) return FS_EIO;
    }
    free_delta = 0;
    return 0;
}

/* ---- carpetas: lista de sus sectores y ranuras de 32 bytes ---- */
static int load_dir(u32 clus)
{
    u32 k;
    nds = 0;
    dlast = 0;
    dbuf_lba = 0xFFFFFFFFu;
    if (clus == 0 && fat_type != 32) {
        for (k = 0; k < rootsecs && nds < DSMAX; k++) dsec[nds++] = root_lba + k;
        return 0;
    }
    if (clus == 0) clus = root_clus;
    while (clus >= 2 && clus < nclusters + 2) {
        long nx;
        for (k = 0; k < spc; k++) {
            if (nds >= DSMAX) return FS_E2BIG;
            dsec[nds++] = clus_lba(clus) + k;
        }
        dlast = clus;
        nx = fat_get(clus);
        if (nx < 0) return FS_EIO;
        if (is_end((u32)nx)) break;
        clus = (u32)nx;
    }
    return 0;
}
static u8 *slot(u32 i)
{
    u32 lba = dsec[i / 16];
    if (dbuf_lba != lba) {
        if (rd(lba, 1, dbuf)) return 0;
        dbuf_lba = lba;
    }
    return dbuf + (i % 16) * 32;
}
static int slot_put(u32 i, const u8 *d)
{
    u8 *p = slot(i);
    u32 k;
    if (!p) return FS_EIO;
    for (k = 0; k < 32; k++) p[k] = d[k];
    return wr(dbuf_lba, 1, dbuf) ? FS_EIO : 0;
}
static int name_is(const char *a, const char *b, int bl)
{
    int i;
    for (i = 0; i < bl; i++)
        if (!a[i] || up(a[i]) != up(b[i])) return 0;
    return a[bl] == 0;
}
struct hit { u32 slot, first; u8 d[32]; };
static int dir_find(const char *name, int nl, struct hit *h)
{
    struct ent e;
    u32 i, first = 0;
    lfn_ok = 0;
    for (i = 0; i < nds * 16; i++) {
        u8 *d = slot(i);
        char sn[13];
        if (!d) return FS_EIO;
        if (d[0] == 0) break;
        if (d[11] == 0x0F && (d[0] & 0x40)) first = i;
        else if (d[11] != 0x0F && !(lfn_ok && first < i)) first = i;
        if (!fat_entry(d, &e)) continue;
        fat_name(d, sn);
        if (name_is(e.name, name, nl) || name_is(sn, name, nl)) {
            u32 k;
            h->slot = i;
            h->first = first;
            for (k = 0; k < 32; k++) h->d[k] = d[k];
            return 0;
        }
    }
    return FS_ENOENT;
}
/* k ranuras libres seguidas (agranda la carpeta si hace falta) */
static int dir_room(u32 k, u32 *at)
{
    u32 i, run = 0, start = 0, tries;
    for (tries = 0; tries < 4; tries++) {
        for (i = 0; i < nds * 16; i++) {
            u8 *d = slot(i);
            if (!d) return FS_EIO;
            if (d[0] == 0) {                        /* de aqui al final todo esta libre */
                if (!run) start = i;
                if (nds * 16 - start >= k) { *at = start; return 0; }
                break;
            }
            if (d[0] == 0xE5) {
                if (!run++) start = i;
                if (run >= k) { *at = start; return 0; }
            } else run = 0;
        }
        if (!dlast) return FS_ENOSPC;               /* raiz fija de FAT16 llena */
        {
            u32 c, s2;
            int r = alloc_chain(1, &c);
            if (r) return r;
            if (zero_clus(c) || fat_set(dlast, c)) return FS_EIO;
            for (s2 = 0; s2 < spc && nds < DSMAX; s2++) dsec[nds++] = clus_lba(c) + s2;
            dlast = c;
            dbuf_lba = 0xFFFFFFFFu;
        }
    }
    return FS_ENOSPC;
}
static int sfn_ok(int c)
{
    const char *x = "$%'-_@~`!(){}^#&";
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    while (*x) if (*x++ == c) return 1;
    return 0;
}
/* nombre corto: el mismo si ya es 8.3 en mayusculas; si no, "NOMBRE~N.EXT" que no exista en la carpeta */
static int make_sfn(const char *nm, int nl, u8 *o, int *lfn_needed)
{
    int dot = -1, i, k, valid = 1, lower = 0, n;
    for (i = 0; i < nl; i++) if (nm[i] == '.') dot = i;
    for (i = 0; i < 11; i++) o[i] = ' ';
    for (i = 0; i < nl; i++) {
        char c = nm[i];
        if (c >= 'a' && c <= 'z') lower = 1;
        if (i == dot) continue;
        if (!sfn_ok(up(c))) valid = 0;
    }
    if (dot == 0 || (dot < 0 ? nl : dot) > 8 || (dot >= 0 && nl - dot - 1 > 3) || (dot >= 0 && nl - dot - 1 == 0)) valid = 0;
    for (i = 0; i < nl; i++) if (nm[i] == '.' && i != dot) valid = 0;
    *lfn_needed = !valid || lower;
    if (valid) {
        for (i = 0, k = 0; i < (dot < 0 ? nl : dot); i++) o[k++] = (u8)up(nm[i]);
        if (dot >= 0) for (i = dot + 1, k = 8; i < nl; i++) o[k++] = (u8)up(nm[i]);
        if (o[0] == 0xE5) o[0] = 0x05;
        return 0;
    }
    k = 0;
    for (i = 0; i < (dot < 0 ? nl : dot) && k < 6; i++) {
        char c = up(nm[i]);
        if ((u8)c < 0x80 && c != ' ' && c != '.') o[k++] = (u8)(sfn_ok(c) ? c : '_');
    }
    if (!k) o[k++] = '_';
    if (dot >= 0)
        for (i = dot + 1, n = 8; i < nl && n < 11; i++) {
            char c = up(nm[i]);
            if ((u8)c < 0x80 && c != ' ') o[n++] = (u8)(sfn_ok(c) ? c : '_');
        }
    for (n = 1; n < 1000000; n++) {                 /* ~1, ~2... hasta que no choque */
        char t[8];
        int tl = 0, m = n, base = k, j;
        struct hit h;
        u8 tmp[12];
        while (m) { t[tl++] = (char)('0' + m % 10); m /= 10; }
        if (base > 7 - tl) base = 7 - tl;
        for (j = 0; j < 8; j++) o[j] = j < base ? o[j] : ' ';
        o[base] = '~';
        for (j = 0; j < tl; j++) o[base + 1 + j] = (u8)t[tl - 1 - j];
        for (j = 0, i = 0; j < 8 && o[j] != ' '; j++) tmp[i++] = o[j];
        if (o[8] != ' ') { tmp[i++] = '.'; for (j = 8; j < 11 && o[j] != ' '; j++) tmp[i++] = o[j]; }
        tmp[i] = 0;
        if (dir_find((const char *)tmp, i, &h) == FS_ENOENT) return 0;
    }
    return FS_EEXIST;
}
static int utf8_get(const char *s, int n, int *i)
{
    u8 c = (u8)s[*i];
    int v;
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0 && *i + 1 < n) { v = (c & 31) << 6 | (s[*i + 1] & 63); *i += 2; return v; }
    if ((c & 0xF0) == 0xE0 && *i + 2 < n) { v = (c & 15) << 12 | (s[*i + 1] & 63) << 6 | (s[*i + 2] & 63); *i += 3; return v; }
    (*i)++;
    while (*i < n && ((u8)s[*i] & 0xC0) == 0x80) (*i)++;
    return '_';
}
/* agrega una entrada (con su nombre largo) a la carpeta ya cargada con load_dir */
static int dir_add(const char *nm, int nl, u8 attr, u32 clus, u32 size)
{
    static u16 u[260];
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    u8 sfn[11], d[32];
    int need, L = 0, i = 0, nent = 0, r, k, j;
    u32 at;
    if ((r = make_sfn(nm, nl, sfn, &need))) return r;
    if (need) {
        while (i < nl && L < 255) u[L++] = (u16)utf8_get(nm, nl, &i);
        nent = (L + 12) / 13;
    }
    if ((r = dir_room((u32)nent + 1, &at))) return r;
    for (k = 0; k < nent; k++) {                    /* las partes del nombre largo, de la ultima a la primera */
        int seq = nent - k;
        for (j = 0; j < 32; j++) d[j] = 0;
        d[0] = (u8)(seq | (k == 0 ? 0x40 : 0));
        d[11] = 0x0F;
        d[13] = sfn_sum(sfn);
        for (j = 0; j < 13; j++) {
            int ci = (seq - 1) * 13 + j;
            u16 ch = ci < L ? u[ci] : (ci == L ? 0 : 0xFFFF);
            d[pos[j]] = (u8)ch;
            d[pos[j] + 1] = (u8)(ch >> 8);
        }
        if ((r = slot_put(at + (u32)k, d))) return r;
    }
    for (j = 0; j < 32; j++) d[j] = 0;
    for (j = 0; j < 11; j++) d[j] = sfn[j];
    d[11] = attr;
    d[16] = 0x21; d[17] = 0x5C;                     /* fecha 2026-01-01 */
    d[24] = 0x21; d[25] = 0x5C;
    d[18] = 0x21; d[19] = 0x5C;
    d[20] = (u8)(clus >> 16); d[21] = (u8)(clus >> 24);
    d[26] = (u8)clus; d[27] = (u8)(clus >> 8);
    d[28] = (u8)size; d[29] = (u8)(size >> 8); d[30] = (u8)(size >> 16); d[31] = (u8)(size >> 24);
    return slot_put(at + (u32)nent, d);
}
static int dir_del(const struct hit *h)
{
    u32 i;
    for (i = h->first; i <= h->slot; i++) {
        u8 *d = slot(i), t[32];
        u32 k;
        if (!d) return FS_EIO;
        for (k = 0; k < 32; k++) t[k] = d[k];
        t[0] = 0xE5;
        if (slot_put(i, t)) return FS_EIO;
    }
    return 0;
}
static u32 hclus(const u8 *d) { return le16(d + 26) | le16(d + 20) << 16; }

/* "/A/B/nombre" -> cluster de la carpeta de arriba y el nombre */
static int split_parent(const char *path, u32 *pclus, const char **nm, int *nl)
{
    char par[256];
    int n = 0, last = -1, i, r;
    struct ent e;
    while (path[n]) { if (path[n] == '/' || path[n] == '\\') last = n; n++; }
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) n--;
    last = -1;
    for (i = 0; i < n; i++) if (path[i] == '/' || path[i] == '\\') last = i;
    *nm = path + last + 1;
    *nl = n - last - 1;
    if (*nl <= 0 || *nl > 255) return FS_ENAME;
    for (i = 0; i < *nl; i++) if ((u8)(*nm)[i] < 32 || (*nm)[i] == ':' || (*nm)[i] == '*' || (*nm)[i] == '?' || (*nm)[i] == '"') return FS_ENAME;
    if (last <= 0) { *pclus = 0; return 0; }
    if (last > 254) return FS_ENAME;
    for (i = 0; i < last; i++) par[i] = path[i];
    par[last] = 0;
    if ((r = resolve(par, &e))) return r;
    if (!e.dir) return FS_ENOENT;
    *pclus = e.clus;
    return 0;
}

static int fat_wfile(const char *path, const void *data, u32 size)
{
    u32 pc, first = 0, n, csz = spc * 512;
    const char *nm;
    int nl, r, found;
    struct hit h;
    if ((r = split_parent(path, &pc, &nm, &nl)) || (r = load_dir(pc))) return r;
    found = dir_find(nm, nl, &h) == 0;
    if (found && (h.d[11] & 0x10)) return FS_EISDIR;
    n = (u32)(((u64)size + csz - 1) / csz);
    if (n && ((r = alloc_chain(n, &first)) || (r = write_chain(first, data, size)))) {
        if (first) free_chain(first);
        fsinfo_update();
        return r;
    }
    if (fat_flush()) return FS_EIO;
    if (found) {
        u32 old = hclus(h.d);
        h.d[20] = (u8)(first >> 16); h.d[21] = (u8)(first >> 24); h.d[26] = (u8)first; h.d[27] = (u8)(first >> 8);
        h.d[28] = (u8)size; h.d[29] = (u8)(size >> 8); h.d[30] = (u8)(size >> 16); h.d[31] = (u8)(size >> 24);
        if ((r = slot_put(h.slot, h.d))) return r;
        if (old) free_chain(old);
    } else if ((r = dir_add(nm, nl, 0x20, first, size))) {
        if (first) free_chain(first);
        fsinfo_update();
        return r;
    }
    return fsinfo_update();
}

static int fat_wmkdir(const char *path)
{
    u32 pc, c;
    const char *nm;
    int nl, r, k;
    struct hit h;
    u8 d[32];
    if ((r = split_parent(path, &pc, &nm, &nl)) || (r = load_dir(pc))) return r;
    if (dir_find(nm, nl, &h) == 0) return FS_EEXIST;
    if ((r = alloc_chain(1, &c))) return r;
    if (zero_clus(c)) return FS_EIO;
    for (k = 0; k < 32; k++) d[k] = 0;            /* "." y ".." */
    for (k = 0; k < 11; k++) d[k] = ' ';
    d[0] = '.'; d[11] = 0x10;
    d[26] = (u8)c; d[27] = (u8)(c >> 8); d[20] = (u8)(c >> 16); d[21] = (u8)(c >> 24);
    for (k = 0; k < 32; k++) sec[k] = d[k];
    d[1] = '.';
    d[26] = (u8)pc; d[27] = (u8)(pc >> 8); d[20] = (u8)(pc >> 16); d[21] = (u8)(pc >> 24);
    for (k = 0; k < 32; k++) sec[32 + k] = d[k];
    for (k = 64; k < 512; k++) sec[k] = 0;
    if (wr(clus_lba(c), 1, sec) || fat_flush()) return FS_EIO;
    if ((r = load_dir(pc)) || (r = dir_add(nm, nl, 0x10, c, 0))) { free_chain(c); fsinfo_update(); return r; }
    return fsinfo_update();
}

static int fat_wremove(const char *path)
{
    u32 pc, c, i;
    const char *nm;
    int nl, r;
    struct hit h;
    if ((r = split_parent(path, &pc, &nm, &nl)) || (r = load_dir(pc))) return r;
    if ((r = dir_find(nm, nl, &h))) return r;
    c = hclus(h.d);
    if (h.d[11] & 0x10) {                          /* carpeta: tiene que estar vacia */
        if ((r = load_dir(c))) return r;
        for (i = 0; i < nds * 16; i++) {
            u8 *d = slot(i);
            if (!d) return FS_EIO;
            if (d[0] == 0) break;
            if (d[0] != 0xE5 && d[0] != '.' && d[11] != 0x0F) return FS_ENOTEMPTY;
        }
        if ((r = load_dir(pc)) || (r = dir_find(nm, nl, &h))) return r;
    }
    if ((r = dir_del(&h))) return r;
    if (c) free_chain(c);
    return fsinfo_update();
}

static int fat_wrename(const char *from, const char *to)
{
    u32 p1, p2, c;
    const char *n1, *n2;
    int l1, l2, r;
    struct hit h, h2;
    u8 attr;
    u32 size;
    if ((r = split_parent(from, &p1, &n1, &l1)) || (r = load_dir(p1)) || (r = dir_find(n1, l1, &h))) return r;
    c = hclus(h.d);
    attr = h.d[11];
    size = le32(h.d + 28);
    if ((r = split_parent(to, &p2, &n2, &l2))) return r;
    if ((r = load_dir(p2))) return r;
    if (dir_find(n2, l2, &h2) == 0) {
        if (h2.d[11] & 0x10 && hclus(h2.d) != c) { /* destino = carpeta que existe: adentro, mismo nombre */
            p2 = hclus(h2.d);
            n2 = n1;
            l2 = l1;
            if ((r = load_dir(p2))) return r;
            if (dir_find(n2, l2, &h2) == 0) return FS_EEXIST;
        } else if (p1 == p2 && h2.slot == h.slot) {
            /* solo cambian mayusculas/minusculas: se borra y se vuelve a poner abajo */
        } else {
            return FS_EEXIST;
        }
    }
    if ((r = load_dir(p1)) || (r = dir_find(n1, l1, &h)) || (r = dir_del(&h))) return r;
    if ((r = load_dir(p2)) || (r = dir_add(n2, l2, attr, c, size))) return r;
    if ((attr & 0x10) && p1 != p2 && c) {           /* la carpeta movida: su ".." apunta a la nueva */
        if (rd(clus_lba(c), 1, sec)) return FS_EIO;
        if (sec[32] == '.' && sec[33] == '.') {
            sec[32 + 26] = (u8)p2; sec[32 + 27] = (u8)(p2 >> 8); sec[32 + 20] = (u8)(p2 >> 16); sec[32 + 21] = (u8)(p2 >> 24);
            if (wr(clus_lba(c), 1, sec)) return FS_EIO;
        }
    }
    return fsinfo_update();
}

/* clusters libres: FSInfo (FAT32) si es valido, si no contando la FAT */
static u64 fat_free(void)
{
    u32 c, n = 0;
    if (fsi_sec && !rd(fsi_sec, 1, sec) && le32(sec) == 0x41615252u && le32(sec + 488) <= nclusters)
        return (u64)le32(sec + 488);
    for (c = 2; c < nclusters + 2; c++) {
        long v = fat_get(c);
        if (v < 0) break;
        if (!v) n++;
    }
    return n;
}

/* ---- dar formato FAT16 o FAT32 a una particion (start/size en sectores del disco) ---- */
static int fat_mkfs(u32 start, u32 size, int f32, const char *label)
{
    static u8 z[16384];
    u8 *b = sec;
    u32 sp, rs, fz, clusters, k, rootsec = f32 ? 0 : 32;
    int i;
    part_lba = start;
    if (f32) {
        sp = size < 532480 ? 1 : size < 16777216 ? 8 : size < 33554432 ? 16 : size < 67108864 ? 32 : 64;
        rs = 32;
    } else {
        if (size > 4194304) return FS_E2BIG;       /* FAT16: maximo 2 GB */
        sp = size <= 32680 ? 2 : size <= 262144 ? 4 : size <= 524288 ? 8 : size <= 1048576 ? 16 : size <= 2097152 ? 32 : 64;
        rs = 4;
    }
    for (;;) {                                      /* tamano de la FAT: hasta que alcance */
        u32 per = f32 ? 128 : 256;
        fz = 1;
        for (k = 0; k < 8; k++) {
            clusters = (size - rs - rootsec - 2 * fz) / sp;
            fz = (clusters + 2 + per - 1) / per;
        }
        clusters = (size - rs - rootsec - 2 * fz) / sp;
        if (f32 ? clusters >= 65525 : (clusters >= 4085 && clusters < 65525)) break;
        if (f32 && sp > 1) { sp /= 2; continue; }
        if (!f32 && clusters >= 65525 && sp < 64) { sp *= 2; continue; }
        return FS_E2BIG;                            /* muy chico para FAT32 (o raro para FAT16) */
    }
    for (i = 0; i < 512; i++) b[i] = 0;
    b[0] = 0xEB; b[1] = f32 ? 0x58 : 0x3C; b[2] = 0x90;
    for (i = 0; i < 8; i++) b[3 + i] = (u8)"BASICOS "[i];
    b[11] = 0; b[12] = 2; b[13] = (u8)sp; b[14] = (u8)rs; b[15] = 0; b[16] = 2;
    if (!f32) { b[17] = 0; b[18] = 2; }            /* 512 entradas en la raiz */
    if (size < 65536 && !f32) { b[19] = (u8)size; b[20] = (u8)(size >> 8); }
    else { b[32] = (u8)size; b[33] = (u8)(size >> 8); b[34] = (u8)(size >> 16); b[35] = (u8)(size >> 24); }
    b[21] = 0xF8; b[24] = 63; b[26] = 255;
    b[28] = (u8)start; b[29] = (u8)(start >> 8); b[30] = (u8)(start >> 16); b[31] = (u8)(start >> 24);
    {
        int e = f32 ? 64 : 36;
        if (f32) {
            b[36] = (u8)fz; b[37] = (u8)(fz >> 8); b[38] = (u8)(fz >> 16); b[39] = (u8)(fz >> 24);
            b[44] = 2; b[48] = 1; b[50] = 6;
        } else {
            b[22] = (u8)fz; b[23] = (u8)(fz >> 8);
        }
        b[e] = 0x80; b[e + 2] = 0x29;
        b[e + 3] = 0x26; b[e + 4] = 0x11; b[e + 5] = 0x20; b[e + 6] = 0xB0;
        for (i = 0; i < 11; i++) b[e + 7 + i] = (u8)' ';
        for (i = 0; label && label[i] && i < 11; i++) b[e + 7 + i] = (u8)up(label[i]);
        for (i = 0; i < 8; i++) b[e + 18 + i] = (u8)(f32 ? "FAT32   " : "FAT16   ")[i];
    }
    b[510] = 0x55; b[511] = 0xAA;
    for (k = 0; k < rs; k += 32)                    /* zona reservada en ceros */
        if (wr(k, (int)(rs - k < 32 ? rs - k : 32), z)) return FS_EIO;
    if (wr(0, 1, b) || (f32 && wr(6, 1, b))) return FS_EIO;
    if (f32) {                                      /* FSInfo */
        for (i = 0; i < 512; i++) b[i] = 0;
        b[0] = 0x52; b[1] = 0x52; b[2] = 0x61; b[3] = 0x41;
        b[484] = 0x72; b[485] = 0x72; b[486] = 0x41; b[487] = 0x61;
        k = clusters - 1;
        b[488] = (u8)k; b[489] = (u8)(k >> 8); b[490] = (u8)(k >> 16); b[491] = (u8)(k >> 24);
        b[492] = 3;
        b[510] = 0x55; b[511] = 0xAA;
        if (wr(1, 1, b) || wr(7, 1, b)) return FS_EIO;
    }
    for (k = 0; k < 2 * fz + rootsec; k += 32)      /* las dos FAT (y la raiz de FAT16) en ceros */
        if (wr(rs + k, (int)(2 * fz + rootsec - k < 32 ? 2 * fz + rootsec - k : 32), z)) return FS_EIO;
    for (i = 0; i < 512; i++) b[i] = 0;
    if (f32) {
        b[0] = 0xF8; b[1] = 0xFF; b[2] = 0xFF; b[3] = 0x0F;
        b[4] = 0xFF; b[5] = 0xFF; b[6] = 0xFF; b[7] = 0x0F;
        b[8] = 0xFF; b[9] = 0xFF; b[10] = 0xFF; b[11] = 0x0F;     /* cluster 2: la raiz */
    } else {
        b[0] = 0xF8; b[1] = 0xFF; b[2] = 0xFF; b[3] = 0xFF;
    }
    if (wr(rs, 1, b) || wr(rs + fz, 1, b)) return FS_EIO;
    if (f32)
        for (k = 0; k < sp; k += 32)
            if (wr(rs + 2 * fz + k, (int)(sp - k < 32 ? sp - k : 32), z)) return FS_EIO;
    for (i = 0; i < 512; i++) b[i] = 0;             /* la etiqueta tambien va como entrada de la raiz */
    for (i = 0; i < 11; i++) b[i] = ' ';
    for (i = 0; label && label[i] && i < 11; i++) b[i] = (u8)up(label[i]);
    b[11] = 0x08;
    b[22] = 0; b[23] = 0; b[24] = 0x21; b[25] = 0x5C;
    if (wr(rs + 2 * fz, 1, b)) return FS_EIO;
    return fat_mount(start);
}

/* ---- escritura: BFS o FAT16/32 ---- */
static int wmount(const char *spec, const char **rest)
{
    int r;
    if (is_sys(spec, rest))
        return FS_EROFS;
    r = mount_spec(spec, rest);
    if (r)
        return r;
    if (is_bfs)
        return kbfs.tx ? FS_OK : FS_EROFS;
    if (is_iso || is_ext || (fat_type != 16 && fat_type != 32) || bpb_bps != 512)
        return FS_EROFS;                            /* CD, ext4 y FAT12: solo lectura */
    free_delta = 0;
    return FS_OK;
}

int fs_write(const char *spec, const void *data, unsigned int size)
{
    const char *rest;
    int r = wmount(spec, &rest);
    if (r) return r;
    return is_bfs ? bfs_err(bfs_write(&kbfs, rest, data, size)) : fat_wfile(rest, data, size);
}

int fs_mkdir(const char *spec)
{
    const char *rest;
    int r = wmount(spec, &rest);
    if (r) return r;
    return is_bfs ? bfs_err(bfs_mkdir(&kbfs, rest)) : fat_wmkdir(rest);
}

int fs_remove(const char *spec)
{
    const char *rest;
    int r = wmount(spec, &rest);
    if (r) return r;
    return is_bfs ? bfs_err(bfs_remove(&kbfs, rest)) : fat_wremove(rest);
}

int fs_rename(const char *spec, const char *to)
{
    const char *rest;
    int r = wmount(spec, &rest), i;
    if (r)
        return r;
    for (i = 0; to[i]; i++)                         /* "HD0.1:/NUEVO" o "/NUEVO": la unidad no cambia */
        if (to[i] == ':') {
            to += i + 1;
            break;
        }
    return is_bfs ? bfs_err(bfs_rename(&kbfs, rest, to)) : fat_wrename(rest, to);
}

int fs_space(const char *spec, unsigned long long *free_bytes, unsigned long long *total_bytes)
{
    const char *rest;
    int r = mount_spec(spec, &rest);
    if (r)
        return r;
    if (!is_bfs) {
        u64 cs = (u64)spc * bpb_bps;
        if (is_iso || is_ext || !fat_type) {
            if (free_bytes) *free_bytes = 0;
            if (total_bytes) *total_bytes = 0;
            return FS_EROFS;
        }
        if (free_bytes) *free_bytes = fat_free() * cs;
        if (total_bytes) *total_bytes = (u64)nclusters * cs;
        return FS_OK;
    }
    if (free_bytes) *free_bytes = kbfs.sb.free_blocks * BFS_BS;
    if (total_bytes) *total_bytes = (kbfs.sb.nblocks - kbfs.sb.data_start) * BFS_BS;
    return FS_OK;
}

const char *fs_kind(void)
{
    if (is_bfs) return "BFS";
    if (is_ext) return "EXT4";
    if (is_iso) return "ISO9660";
    return fat_type == 32 ? "FAT32" : (fat_type == 16 ? "FAT16" : "FAT12");
}

/* da formato BFS a la particion N del disco (su tipo en el MBR tiene que ser 0xB0): "HD1.1:" */
int fs_format(const char *spec, const char *label)
{
    int n = 0, len, part = 0, di, i;
    u8 mbr[512];
    u32 start, size;
    while (spec[n] && spec[n] != ':')
        n++;
    len = n;
    for (i = 0; i < n; i++)
        if (spec[i] == '.') {
            len = i;
            part = spec[i + 1] - '0';
            break;
        }
    if (part < 1 || part > 4)
        return FS_ENOENT;
    di = blk_find(spec, len);
    if (di < 0)
        return FS_ENODEV;
    dev = blk_get(di);
    if (dev->bps != 512 || blk_read(dev, 0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return FS_EFMT;
    start = le32(mbr + 446 + (part - 1) * 16 + 8);
    size = le32(mbr + 446 + (part - 1) * 16 + 12);
    is_bfs = is_ext = is_iso = 0;
    {
        u8 t = mbr[446 + (part - 1) * 16 + 4];
        if (t == 0x0B || t == 0x0C) return fat_mkfs(start, size, 1, label);
        if (t == 0x04 || t == 0x06 || t == 0x0E) return fat_mkfs(start, size, 0, label);
        if (t != BFS_PTYPE) return FS_EFMT;
    }
    part_lba = start;
    kbfs.rd = kb_rd;
    kbfs.wr = kb_wr;
    if (!kbfs.tx && ram_ok(BFS_TXMEM + 0x80000ull - 1))
        kbfs.tx = (u8 *)(unsigned long)BFS_TXMEM;
    if (!kbfs.tx)
        return FS_ENOSPC;
    i = bfs_mkfs(&kbfs, size, label);
    if (i)
        return bfs_err(i);
    return bfs_err(bfs_mount(&kbfs));
}
