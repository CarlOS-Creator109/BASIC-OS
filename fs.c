/* fs.c - FAT12/16/32 e ISO9660, solo lectura, nombres 8.3 / ISO nivel 1. Sin libc. */
#include "kernel.h"
#include "fs.h"
#include "sys.h"

#define SECBUF 4096

static struct blk *dev;
static u32 part_lba;                    /* inicio de la particion (sectores del dispositivo) */
static int is_iso;

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

static int fat_byte(u32 off)
{
    u32 s = off / bpb_bps;
    if (fsec_lba != s) {
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
    fsec_lba = 0xFFFFFFFFu;
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
    char name[40];
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

    if (clus == 0 && fat_type != 32) {              /* raiz fija de FAT12/16 */
        for (i = 0; i < rootsecs; i++) {
            u32 off;
            if (rd(root_lba + i, 1, sec))
                return FS_EIO;
            for (off = 0; off < bpb_bps; off += 32) {
                const u8 *d = sec + off;
                if (d[0] == 0)
                    return FS_OK;
                if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 0x08))
                    continue;
                fat_name(d, e.name);
                e.size = le32(d + 28);
                e.clus = le16(d + 26) | (le16(d + 20) << 16);
                e.dir = (d[11] & 0x10) != 0;
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
                if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 0x08))
                    continue;
                fat_name(d, e.name);
                e.size = le32(d + 28);
                e.clus = le16(d + 26) | (le16(d + 20) << 16);
                e.dir = (d[11] & 0x10) != 0;
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
    if (!part) {                                    /* sin particion: disquete o primera FAT */
        if (fat_mount(0) == FS_OK)
            return FS_OK;
        for (i = 0; i < 4; i++) {
            u8 t = mbr[446 + i * 16 + 4];
            if ((t == 1 || t == 4 || t == 6 || t == 0xB || t == 0xC || t == 0xE) &&
                fat_mount(le32(mbr + 446 + i * 16 + 8)) == FS_OK)
                return FS_OK;
        }
        return FS_EFMT;
    }
    if (part < 1 || part > 4 || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return FS_EFMT;
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
    r = resolve(rest, &e);
    if (r)
        return r;
    if (!e.dir)
        return FS_ENOENT;
    user_cb = cb;
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
        u32 clus = e.clus;
        while (got < left && clus >= 2 && clus < 0x0FFFFFF8) {
            u32 s;
            long nx;
            for (s = 0; s < spc && got < left; s++) {
                u32 n = left - got < bpb_bps ? left - got : bpb_bps, k;
                if (rd(clus_lba(clus) + s, 1, sec))
                    return FS_EIO;
                for (k = 0; k < n; k++)
                    buf[got + k] = (char)sec[k];
                got += n;
            }
            nx = fat_next(clus);
            if (nx < 0)
                return FS_EIO;
            clus = (u32)nx;
        }
    }
    return (int)got;
}
