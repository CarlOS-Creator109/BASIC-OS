/* bfs.c - BFS, el sistema de archivos de BASIC-OS (ver bfs.h). El mismo codigo va en el kernel y en tools/mkbfs.
 * GPL-3.0-or-later. */
#ifdef BFS_HOST
#include <string.h>
#else
#include "kernel.h"
#endif
#include "bfs.h"

typedef bfs_u8 B;
typedef bfs_u16 W;
typedef bfs_u32 D;
typedef bfs_u64 Q;

#define BS      BFS_BS
#define BITS    (BFS_BS * 8u)
#define IPB     (BFS_BS / 256u)             /* inodos por bloque */

const char *bfs_strerror(int e)
{
    switch (e) {
    case 0: return "bien";
    case BFS_EIO: return "error del disco";
    case BFS_ENOENT: return "no existe";
    case BFS_EFMT: return "no es un volumen BFS";
    case BFS_ENOSPC: return "no hay espacio";
    case BFS_EEXIST: return "ya existe";
    case BFS_ENOTDIR: return "no es una carpeta";
    case BFS_EISDIR: return "es una carpeta";
    case BFS_ENOTEMPTY: return "la carpeta no esta vacia";
    case BFS_ENAME: return "nombre no valido";
    case BFS_E2BIG: return "operacion demasiado grande";
    case BFS_EBUSY: return "no se puede (raiz o en uso)";
    }
    return "error";
}

/* ---- bloques ---- */
static int blk_rd(struct bfs *v, Q b, void *buf) { return v->rd(v->ctx, b * BFS_SPB, BFS_SPB, buf) ? BFS_EIO : 0; }
static int blk_wr(struct bfs *v, Q b, const void *buf) { return v->wr(v->ctx, b * BFS_SPB, BFS_SPB, buf) ? BFS_EIO : 0; }

/* ---- la operacion en curso: copias de los metadatos que cambia ---- */
static int tx_find(struct bfs *v, Q b)
{
    D i;
    for (i = 0; i < v->ntx; i++)
        if (v->txblk[i] == b)
            return (int)i;
    return -1;
}
static int meta_rd(struct bfs *v, Q b, void *buf)
{
    int i = tx_find(v, b);
    if (i >= 0) {
        memcpy(buf, v->tx + (Q)i * BS, BS);
        return 0;
    }
    return blk_rd(v, b, buf);
}
static int meta_wr(struct bfs *v, Q b, const void *buf)
{
    int i = tx_find(v, b);
    if (i < 0) {
        if (v->ntx >= BFS_TXMAX)
            return BFS_E2BIG;
        i = (int)v->ntx++;
        v->txblk[i] = b;
    }
    memcpy(v->tx + (Q)i * BS, buf, BS);
    return 0;
}
static void tx_abort(struct bfs *v)
{
    v->ntx = 0;
    if (blk_rd(v, 0, v->b1) == 0)
        memcpy(&v->sb, v->b1, sizeof v->sb);
}
static D fnv(D s, const B *p, Q n)
{
    Q i;
    for (i = 0; i < n; i++)
        s = (s ^ p[i]) * 16777619u;
    return s;
}
static D tx_sum(struct bfs *v, const struct bfs_jhead *h, const B *copies)
{
    D s = 2166136261u ^ (D)h->seq, i;
    (void)v;
    for (i = 0; i < h->n; i++)
        s = fnv(s, copies + (Q)i * BS, BS);
    return fnv(s, (const B *)h->blk, (Q)h->n * 8);
}
static int tx_commit(struct bfs *v)
{
    struct bfs_jhead *h = (struct bfs_jhead *)v->b2;
    D i;
    int r;
    if (!v->ntx)
        return 0;
    v->sb.jseq++;
    memset(v->b1, 0, BS);
    memcpy(v->b1, &v->sb, sizeof v->sb);
    r = meta_wr(v, 0, v->b1);
    if (r) {
        tx_abort(v);
        return r;
    }
    for (i = 0; i < v->ntx; i++)                    /* 1. copias al diario */
        if (blk_wr(v, v->sb.jstart + 1 + i, v->tx + (Q)i * BS))
            goto fail;
    memset(h, 0, BS);                                /* 2. cabecera con la suma: desde aqui la operacion ya paso */
    memcpy(h->magic, "BFS-JRN1", 8);
    h->seq = v->sb.jseq;
    h->n = v->ntx;
    for (i = 0; i < v->ntx; i++)
        h->blk[i] = v->txblk[i];
    h->sum = tx_sum(v, h, v->tx);
    if (blk_wr(v, v->sb.jstart, h))
        goto fail;
    for (i = 0; i < v->ntx; i++)                    /* 3. cada bloque a su lugar (el 0 lleva jseq: ya no se repite) */
        if (blk_wr(v, v->txblk[i], v->tx + (Q)i * BS))
            goto fail;
    v->ntx = 0;
    return 0;
fail:
    tx_abort(v);
    return BFS_EIO;
}

/* ---- montar: superbloque y repetir el diario ---- */
int bfs_mount(struct bfs *v)
{
    struct bfs_jhead *h = (struct bfs_jhead *)v->b2;
    D i;
    v->ntx = 0;
    if (blk_rd(v, 0, v->b1))
        return BFS_EIO;
    memcpy(&v->sb, v->b1, sizeof v->sb);
    if (memcmp(v->sb.magic, "BFS-BOS1", 8) || v->sb.version != BFS_VERSION || v->sb.bsize != BS ||
        v->sb.data_start >= v->sb.nblocks || v->sb.jblocks < BFS_TXMAX + 2)
        return BFS_EFMT;
    if (!v->tx)
        return 0;                                   /* solo lectura sin memoria para el diario: no se repite */
    if (blk_rd(v, v->sb.jstart, h))
        return BFS_EIO;
    if (memcmp(h->magic, "BFS-JRN1", 8) || h->seq != v->sb.jseq + 1 || !h->n || h->n > BFS_TXMAX)
        return 0;
    for (i = 0; i < h->n; i++)
        if (blk_rd(v, v->sb.jstart + 1 + i, v->tx + (Q)i * BS))
            return BFS_EIO;
    if (tx_sum(v, h, v->tx) != h->sum)
        return 0;                                   /* la cabecera no alcanzo a escribirse entera: no paso */
    for (i = 0; i < h->n; i++)
        if (blk_wr(v, h->blk[i], v->tx + (Q)i * BS))
            return BFS_EIO;
    if (blk_rd(v, 0, v->b1))
        return BFS_EIO;
    memcpy(&v->sb, v->b1, sizeof v->sb);
    return 0;
}

/* ---- mapas de bits ---- */
static int bits_set(struct bfs *v, Q start, Q first, Q n, int val)
{
    Q cur = ~0ull, i;
    int r;
    for (i = first; i < first + n; i++) {
        Q bi = start + i / BITS;
        D o = (D)(i % BITS);
        if (bi != cur) {
            if (cur != ~0ull && (r = meta_wr(v, cur, v->bb)))
                return r;
            if ((r = meta_rd(v, bi, v->bb)))
                return r;
            cur = bi;
        }
        if (val) v->bb[o / 8] |= (B)(1u << (o % 8));
        else v->bb[o / 8] &= (B)~(1u << (o % 8));
    }
    if (cur != ~0ull)
        return meta_wr(v, cur, v->bb);
    return 0;
}

/* busca un tramo libre de hasta want bloques (el primero que encuentre desde hint) y lo marca usado */
static int alloc_run(struct bfs *v, Q want, Q *ostart, Q *olen)
{
    Q n = v->sb.nblocks, from, to, i, cur = ~0ull;
    int pass;
    if (!want || v->sb.free_blocks == 0)
        return BFS_ENOSPC;
    from = (v->hint >= v->sb.data_start && v->hint < n) ? v->hint : v->sb.data_start;
    for (pass = 0; pass < 2; pass++) {
        to = pass ? from : n;
        i = pass ? v->sb.data_start : from;
        while (i < to) {
            Q bi = v->sb.bmap_start + i / BITS;
            D o = (D)(i % BITS);
            if (bi != cur) {
                if (meta_rd(v, bi, v->bb))
                    return BFS_EIO;
                cur = bi;
            }
            if ((o & 7) == 0 && v->bb[o / 8] == 0xFF && i + 8 <= to) {
                i += 8;
                continue;
            }
            if (!(v->bb[o / 8] & (1u << (o % 8)))) {      /* libre: hasta donde llega */
                Q s = i, len = 0;
                while (i < n && len < want && len < 0xFFFFFFFFull) {
                    Q bj = v->sb.bmap_start + i / BITS;
                    D oj = (D)(i % BITS);
                    if (bj != cur) {
                        if (meta_rd(v, bj, v->bb))
                            return BFS_EIO;
                        cur = bj;
                    }
                    if (v->bb[oj / 8] & (1u << (oj % 8)))
                        break;
                    len++;
                    i++;
                }
                if (bits_set(v, v->sb.bmap_start, s, len, 1))
                    return BFS_EIO;
                v->sb.free_blocks -= len;
                v->hint = s + len;
                *ostart = s;
                *olen = len;
                return 0;
            }
            i++;
        }
    }
    return BFS_ENOSPC;
}
static int free_run(struct bfs *v, Q start, Q len)
{
    int r;
    if (!len)
        return 0;
    if (start < v->sb.data_start || start + len > v->sb.nblocks)
        return BFS_EFMT;
    r = bits_set(v, v->sb.bmap_start, start, len, 0);
    if (!r)
        v->sb.free_blocks += len;
    return r;
}

static int alloc_ino(struct bfs *v, D *out)
{
    Q i, n = v->sb.ninodes, cur = ~0ull, k;
    if (!v->sb.free_inodes)
        return BFS_ENOSPC;
    for (k = 0; k < n; k++) {
        Q bi;
        D o;
        i = (v->ihint + k) % n;
        if (i < 2)
            continue;
        bi = v->sb.imap_start + i / BITS;
        o = (D)(i % BITS);
        if (bi != cur) {
            if (meta_rd(v, bi, v->bb))
                return BFS_EIO;
            cur = bi;
        }
        if (!(v->bb[o / 8] & (1u << (o % 8)))) {
            v->bb[o / 8] |= (B)(1u << (o % 8));
            if (meta_wr(v, bi, v->bb))
                return BFS_E2BIG;
            v->sb.free_inodes--;
            v->ihint = (D)(i + 1);
            *out = (D)i;
            return 0;
        }
    }
    return BFS_ENOSPC;
}

/* ---- inodos ---- */
static int ino_rd(struct bfs *v, D ino, struct bfs_inode *in)
{
    if (!ino || ino >= v->sb.ninodes)
        return BFS_EFMT;
    if (meta_rd(v, v->sb.itab_start + ino / IPB, v->ib))
        return BFS_EIO;
    memcpy(in, v->ib + (ino % IPB) * 256, 256);
    return 0;
}
static int ino_wr(struct bfs *v, D ino, const struct bfs_inode *in)
{
    Q b = v->sb.itab_start + ino / IPB;
    if (meta_rd(v, b, v->ib))
        return BFS_EIO;
    memcpy(v->ib + (ino % IPB) * 256, in, 256);
    return meta_wr(v, b, v->ib);
}

/* tramo i del archivo (de los 12 del inodo o del bloque extra) */
static int ext_get(struct bfs *v, const struct bfs_inode *in, D i, struct bfs_ext *e)
{
    if (i < BFS_NEXT) {
        *e = in->ext[i];
        return 0;
    }
    if (!in->xblock || i >= BFS_NEXT + BFS_XEXT)
        return BFS_EFMT;
    if (meta_rd(v, in->xblock, v->xb))
        return BFS_EIO;
    memcpy(e, v->xb + (i - BFS_NEXT) * 16, 16);
    return 0;
}

/* bloque de disco que guarda el bloque fb del archivo (0 = fuera) */
static Q file_block(struct bfs *v, const struct bfs_inode *in, Q fb)
{
    D i;
    struct bfs_ext e;
    for (i = 0; i < in->next; i++) {
        if (ext_get(v, in, i, &e))
            return 0;
        if (fb < e.len)
            return e.start + fb;
        fb -= e.len;
    }
    return 0;
}

/* suelta todos los bloques del archivo */
static int free_extents(struct bfs *v, struct bfs_inode *in)
{
    D i;
    struct bfs_ext e;
    int r;
    for (i = 0; i < in->next; i++) {
        if ((r = ext_get(v, in, i, &e)))
            return r;
        if ((r = free_run(v, e.start, e.len)))
            return r;
    }
    if (in->xblock && (r = free_run(v, in->xblock, 1)))
        return r;
    in->next = 0;
    in->xblock = 0;
    return 0;
}

/* agrega un tramo al final (lo junta con el ultimo si siguen) */
static int add_extent(struct bfs *v, struct bfs_inode *in, Q start, Q len)
{
    struct bfs_ext e;
    int r;
    if (in->next) {
        if ((r = ext_get(v, in, in->next - 1, &e)))
            return r;
        if (e.start + e.len == start && (Q)e.len + len <= 0xFFFFFFFFull) {
            e.len += (D)len;
            if (in->next - 1 < BFS_NEXT) {
                in->ext[in->next - 1] = e;
                return 0;
            }
            memcpy(v->xb + (in->next - 1 - BFS_NEXT) * 16, &e, 16);
            return meta_wr(v, in->xblock, v->xb);
        }
    }
    e.start = start;
    e.len = (D)len;
    e.pad = 0;
    if (in->next < BFS_NEXT) {
        in->ext[in->next++] = e;
        return 0;
    }
    if (in->next >= BFS_NEXT + BFS_XEXT)
        return BFS_ENOSPC;                          /* disco demasiado fragmentado para este archivo */
    if (!in->xblock) {
        Q xs, xl;
        if ((r = alloc_run(v, 1, &xs, &xl)))
            return r;
        in->xblock = xs;
        memset(v->xb, 0, BS);
    } else if (meta_rd(v, in->xblock, v->xb)) {
        return BFS_EIO;
    }
    memcpy(v->xb + (in->next - BFS_NEXT) * 16, &e, 16);
    in->next++;
    return meta_wr(v, in->xblock, v->xb);
}

/* ---- lectura ---- */
long long bfs_read(struct bfs *v, const struct bfs_inode *in, Q off, void *buf, Q n)
{
    B *out = buf;
    Q done = 0, fpos = 0;
    D i;
    struct bfs_ext e;
    if (off >= in->size)
        return 0;
    if (n > in->size - off)
        n = in->size - off;
    for (i = 0; i < in->next && done < n; i++) {
        Q ebytes, skip;
        if (ext_get(v, in, i, &e))
            return BFS_EIO;
        ebytes = (Q)e.len * BS;
        if (off + done >= fpos + ebytes) {
            fpos += ebytes;
            continue;
        }
        skip = off + done - fpos;                   /* bytes del tramo antes de lo que queremos */
        while (done < n && skip < ebytes) {
            Q blk = e.start + skip / BS, inb = skip % BS, want = n - done;
            if (inb == 0 && want >= BS) {           /* bloques enteros: directo al buffer, de una vez */
                Q nb = want / BS, left = e.len - skip / BS;
                if (nb > left) nb = left;
                if (nb > 256) nb = 256;
                if (v->rd(v->ctx, blk * BFS_SPB, (D)(nb * BFS_SPB), out + done))
                    return BFS_EIO;
                done += nb * BS;
                skip += nb * BS;
            } else {
                Q c = BS - inb;
                if (c > want) c = want;
                if (blk_rd(v, blk, v->b1))
                    return BFS_EIO;
                memcpy(out + done, v->b1 + inb, c);
                done += c;
                skip += c;
            }
        }
        fpos += ebytes;
    }
    return (long long)done;
}

/* ---- carpetas ---- */
static int name_eq(const B *a, int na, const char *b, int nb)
{
    int i;
    if (na != nb)
        return 0;
    for (i = 0; i < na; i++) {
        int x = a[i], y = (B)b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y)
            return 0;
    }
    return 1;
}
static D rec_need(int nlen) { return (D)((8 + nlen + 7) & ~7); }

int bfs_list(struct bfs *v, D dino, bfs_dir_cb cb, void *ctx)
{
    struct bfs_inode d, f;
    Q fb, nb;
    if (ino_rd(v, dino, &d))
        return BFS_EIO;
    if (d.type != BFS_T_DIR)
        return BFS_ENOTDIR;
    nb = d.size / BS;
    for (fb = 0; fb < nb; fb++) {
        D off = 0;
        Q b = file_block(v, &d, fb);
        if (!b || meta_rd(v, b, v->db))
            return BFS_EIO;
        while (off + 8 <= BS) {
            struct bfs_dirent *e = (struct bfs_dirent *)(v->db + off);
            if (e->rec < 8 || off + e->rec > BS)
                return BFS_EFMT;
            if (e->ino) {
                char nm[BFS_NAMEMAX + 1];
                int k, stop;
                Q sz = 0;
                for (k = 0; k < e->nlen; k++) nm[k] = (char)v->db[off + 8 + k];
                nm[k] = 0;
                if (e->type == BFS_T_FILE && ino_rd(v, e->ino, &f) == 0)
                    sz = f.size;
                stop = cb(ctx, nm, e->nlen, e->ino, e->type, sz);
                if (stop)
                    return 0;
                if (meta_rd(v, b, v->db))        /* el callback pudo usar los buffers */
                    return BFS_EIO;
            }
            off += e->rec;
        }
    }
    return 0;
}

/* busca un nombre en la carpeta: 0 = esta (ino/tipo), ENOENT = no */
static int dir_find(struct bfs *v, D dino, const char *name, int nlen, D *ino, int *type)
{
    struct bfs_inode d;
    Q fb, nb;
    if (ino_rd(v, dino, &d))
        return BFS_EIO;
    if (d.type != BFS_T_DIR)
        return BFS_ENOTDIR;
    if (nlen == 1 && name[0] == '.') {
        *ino = dino;
        *type = BFS_T_DIR;
        return 0;
    }
    if (nlen == 2 && name[0] == '.' && name[1] == '.') {
        *ino = d.parent ? d.parent : BFS_ROOT;
        *type = BFS_T_DIR;
        return 0;
    }
    nb = d.size / BS;
    for (fb = 0; fb < nb; fb++) {
        D off = 0;
        Q b = file_block(v, &d, fb);
        if (!b || meta_rd(v, b, v->db))
            return BFS_EIO;
        while (off + 8 <= BS) {
            struct bfs_dirent *e = (struct bfs_dirent *)(v->db + off);
            if (e->rec < 8 || off + e->rec > BS)
                return BFS_EFMT;
            if (e->ino && name_eq(v->db + off + 8, e->nlen, name, nlen)) {
                *ino = e->ino;
                *type = e->type;
                return 0;
            }
            off += e->rec;
        }
    }
    return BFS_ENOENT;
}

static void put_ent(B *p, D ino, W rec, const char *name, int nlen, int type)
{
    struct bfs_dirent *e = (struct bfs_dirent *)p;
    int k;
    e->ino = ino;
    e->rec = rec;
    e->nlen = (B)nlen;
    e->type = (B)type;
    for (k = 0; k < nlen; k++) p[8 + k] = (B)name[k];
}

static int dir_add(struct bfs *v, D dino, const char *name, int nlen, D ino, int type)
{
    struct bfs_inode d;
    Q fb, nb, b, s, l;
    D need = rec_need(nlen);
    int r;
    if ((r = ino_rd(v, dino, &d)))
        return r;
    nb = d.size / BS;
    for (fb = 0; fb < nb; fb++) {
        D off = 0;
        b = file_block(v, &d, fb);
        if (!b || meta_rd(v, b, v->db))
            return BFS_EIO;
        while (off + 8 <= BS) {
            struct bfs_dirent *e = (struct bfs_dirent *)(v->db + off);
            D used = e->ino ? rec_need(e->nlen) : 0;
            if (e->rec < 8 || off + e->rec > BS)
                return BFS_EFMT;
            if (e->rec - used >= need) {
                if (e->ino) {                       /* partir: el viejo se queda con lo suyo */
                    W rest = (W)(e->rec - used);
                    e->rec = (W)used;
                    put_ent(v->db + off + used, ino, rest, name, nlen, type);
                } else {
                    put_ent(v->db + off, ino, e->rec, name, nlen, type);
                }
                return meta_wr(v, b, v->db);
            }
            off += e->rec;
        }
    }
    /* sin lugar: un bloque mas para la carpeta */
    if ((r = alloc_run(v, 1, &s, &l)))
        return r;
    memset(v->db, 0, BS);
    put_ent(v->db, ino, (W)BS, name, nlen, type);
    if ((r = meta_wr(v, s, v->db)))
        return r;
    if ((r = ino_rd(v, dino, &d)) || (r = add_extent(v, &d, s, 1)))
        return r;
    d.size += BS;
    return ino_wr(v, dino, &d);
}

static int dir_del(struct bfs *v, D dino, const char *name, int nlen)
{
    struct bfs_inode d;
    Q fb, nb;
    if (ino_rd(v, dino, &d))
        return BFS_EIO;
    nb = d.size / BS;
    for (fb = 0; fb < nb; fb++) {
        D off = 0, prev = ~0u;
        Q b = file_block(v, &d, fb);
        if (!b || meta_rd(v, b, v->db))
            return BFS_EIO;
        while (off + 8 <= BS) {
            struct bfs_dirent *e = (struct bfs_dirent *)(v->db + off);
            if (e->rec < 8 || off + e->rec > BS)
                return BFS_EFMT;
            if (e->ino && name_eq(v->db + off + 8, e->nlen, name, nlen)) {
                if (prev != ~0u) {                  /* el hueco se lo queda la entrada de antes */
                    struct bfs_dirent *p = (struct bfs_dirent *)(v->db + prev);
                    p->rec = (W)(p->rec + e->rec);
                } else {
                    e->ino = 0;
                    e->nlen = 0;
                }
                return meta_wr(v, b, v->db);
            }
            prev = off;
            off += e->rec;
        }
    }
    return BFS_ENOENT;
}

static int count_cb(void *ctx, const char *n, int nl, D ino, int t, Q sz)
{
    (void)n; (void)nl; (void)ino; (void)t; (void)sz;
    (*(int *)ctx)++;
    return 1;
}

/* ---- rutas: "/CARPETA/ARCHIVO", "/" o "\" ---- */
static int walk(struct bfs *v, const char *path, D *parent, const char **last, int *lastlen)
{
    D cur = BFS_ROOT, ino;
    int type, r;
    for (;;) {
        int n = 0;
        while (*path == '/' || *path == '\\')
            path++;
        while (path[n] && path[n] != '/' && path[n] != '\\')
            n++;
        if (!n) {                                   /* la ruta era la raiz o terminaba en / */
            *parent = cur;
            *last = path;
            *lastlen = 0;
            return 0;
        }
        {
            const char *q = path + n;
            while (*q == '/' || *q == '\\') q++;
            if (!*q) {                              /* ultimo nombre */
                *parent = cur;
                *last = path;
                *lastlen = n;
                return 0;
            }
        }
        r = dir_find(v, cur, path, n, &ino, &type);
        if (r)
            return r;
        if (type != BFS_T_DIR)
            return BFS_ENOTDIR;
        cur = ino;
        path += n;
    }
}

int bfs_lookup(struct bfs *v, const char *path, D *ino, struct bfs_inode *in)
{
    D parent, i;
    const char *last;
    int n, type, r;
    if ((r = walk(v, path, &parent, &last, &n)))
        return r;
    if (!n)
        i = parent;
    else if ((r = dir_find(v, parent, last, n, &i, &type)))
        return r;
    if (ino)
        *ino = i;
    return in ? ino_rd(v, i, in) : 0;
}

static int valid_name(const char *s, int n)
{
    int i;
    if (n <= 0 || n > (int)BFS_NAMEMAX)
        return 0;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return 0;
    for (i = 0; i < n; i++)
        if ((B)s[i] < 32 || s[i] == ':' || s[i] == '/' || s[i] == '\\')
            return 0;
    return 1;
}

static Q now_time(void) { return 0; }

/* ---- escribir un archivo entero (lo crea o lo reemplaza) ---- */
int bfs_write(struct bfs *v, const char *path, const void *data, Q size)
{
    struct bfs_inode in, old;
    struct bfs_ext ex[BFS_NEXT + BFS_XEXT];
    D parent, ino = 0, nex = 0, i;
    const char *name;
    int nlen, type, r, exists;
    Q need, got = 0;
    const B *src = data;

    v->ntx = 0;
    if ((r = walk(v, path, &parent, &name, &nlen)))
        return r;
    if (!valid_name(name, nlen))
        return BFS_ENAME;
    r = dir_find(v, parent, name, nlen, &ino, &type);
    if (r && r != BFS_ENOENT)
        return r;
    exists = r == 0;
    if (exists && type == BFS_T_DIR)
        return BFS_EISDIR;
    need = (size + BS - 1) / BS;
    if (need > v->sb.free_blocks)
        return BFS_ENOSPC;
    /* 1. bloques nuevos */
    while (got < need) {
        Q s, l;
        if (nex >= BFS_NEXT + BFS_XEXT) { r = BFS_ENOSPC; goto fail; }
        if ((r = alloc_run(v, need - got, &s, &l)))
            goto fail;
        ex[nex].start = s; ex[nex].len = (D)l; ex[nex].pad = 0;
        nex++;
        got += l;
    }
    /* 2. los datos, directo a su lugar (todavia nadie los ve) */
    {
        Q pos = 0;
        for (i = 0; i < nex; i++) {
            Q k = 0;
            while (k < ex[i].len) {
                Q left = size - pos, whole = left / BS, nb = ex[i].len - k;
                if (whole) {
                    if (nb > whole) nb = whole;
                    if (nb > 256) nb = 256;
                    if (v->wr(v->ctx, (ex[i].start + k) * BFS_SPB, (D)(nb * BFS_SPB), src + pos)) { r = BFS_EIO; goto fail; }
                    pos += nb * BS;
                    k += nb;
                } else {                            /* ultimo bloque a medias: completar con ceros */
                    memset(v->b1, 0, BS);
                    memcpy(v->b1, src + pos, left);
                    if (blk_wr(v, ex[i].start + k, v->b1)) { r = BFS_EIO; goto fail; }
                    pos += left;
                    k++;
                }
            }
        }
    }
    /* 3. el inodo (el viejo suelta sus bloques) */
    if (exists) {
        if ((r = ino_rd(v, ino, &old)) || (r = free_extents(v, &old)))
            goto fail;
    } else if ((r = alloc_ino(v, &ino))) {
        goto fail;
    }
    memset(&in, 0, sizeof in);
    in.type = BFS_T_FILE;
    in.parent = parent;
    in.size = size;
    in.mtime = now_time();
    for (i = 0; i < nex; i++)
        if ((r = add_extent(v, &in, ex[i].start, ex[i].len)))
            goto fail;
    if ((r = ino_wr(v, ino, &in)))
        goto fail;
    if (!exists && (r = dir_add(v, parent, name, nlen, ino, BFS_T_FILE)))
        goto fail;
    return tx_commit(v);
fail:
    tx_abort(v);
    return r;
}

int bfs_mkdir(struct bfs *v, const char *path)
{
    struct bfs_inode in;
    D parent, ino;
    const char *name;
    int nlen, type, r;
    Q s, l;
    v->ntx = 0;
    if ((r = walk(v, path, &parent, &name, &nlen)))
        return r;
    if (!valid_name(name, nlen))
        return BFS_ENAME;
    r = dir_find(v, parent, name, nlen, &ino, &type);
    if (r == 0)
        return BFS_EEXIST;
    if (r != BFS_ENOENT)
        return r;
    if ((r = alloc_ino(v, &ino)) || (r = alloc_run(v, 1, &s, &l)))
        goto fail;
    memset(v->db, 0, BS);
    put_ent(v->db, 0, (W)BS, "", 0, 0);
    if ((r = meta_wr(v, s, v->db)))
        goto fail;
    memset(&in, 0, sizeof in);
    in.type = BFS_T_DIR;
    in.parent = parent;
    in.size = BS;
    in.next = 1;
    in.ext[0].start = s;
    in.ext[0].len = 1;
    if ((r = ino_wr(v, ino, &in)) || (r = dir_add(v, parent, name, nlen, ino, BFS_T_DIR)))
        goto fail;
    return tx_commit(v);
fail:
    tx_abort(v);
    return r;
}

static int unlink_ino(struct bfs *v, D ino)
{
    struct bfs_inode in;
    int r;
    if ((r = ino_rd(v, ino, &in)) || (r = free_extents(v, &in)))
        return r;
    in.type = BFS_T_FREE;
    if ((r = ino_wr(v, ino, &in)) || (r = bits_set(v, v->sb.imap_start, ino, 1, 0)))
        return r;
    v->sb.free_inodes++;
    return 0;
}

int bfs_remove(struct bfs *v, const char *path)
{
    D parent, ino;
    const char *name;
    int nlen, type, r, cnt = 0;
    v->ntx = 0;
    if ((r = walk(v, path, &parent, &name, &nlen)))
        return r;
    if (!nlen || !valid_name(name, nlen))
        return nlen ? BFS_ENAME : BFS_EBUSY;
    if ((r = dir_find(v, parent, name, nlen, &ino, &type)))
        return r;
    if (type == BFS_T_DIR) {
        if ((r = bfs_list(v, ino, count_cb, &cnt)))
            return r;
        if (cnt)
            return BFS_ENOTEMPTY;
    }
    if ((r = dir_del(v, parent, name, nlen)) || (r = unlink_ino(v, ino)))
        goto fail;
    return tx_commit(v);
fail:
    tx_abort(v);
    return r;
}

int bfs_rename(struct bfs *v, const char *from, const char *to)
{
    struct bfs_inode in;
    D p1, p2, ino, tino, a;
    const char *n1, *n2;
    int l1, l2, t1, t2, r;
    v->ntx = 0;
    if ((r = walk(v, from, &p1, &n1, &l1)))
        return r;
    if (!l1)
        return BFS_EBUSY;
    if ((r = dir_find(v, p1, n1, l1, &ino, &t1)))
        return r;
    if ((r = walk(v, to, &p2, &n2, &l2)))
        return r;
    if (!l2) {                                      /* "/": a la raiz con el mismo nombre */
        n2 = n1;
        l2 = l1;
    } else if (dir_find(v, p2, n2, l2, &tino, &t2) == 0 && t2 == BFS_T_DIR && tino != ino) {
        p2 = tino;                                  /* destino = carpeta que ya existe: adentro, mismo nombre */
        n2 = n1;
        l2 = l1;
    }
    if (!valid_name(n2, l2))
        return BFS_ENAME;
    for (a = p2; t1 == BFS_T_DIR; ) {               /* no se puede meter una carpeta dentro de si misma */
        if (a == ino)
            return BFS_EBUSY;
        if (a == BFS_ROOT)
            break;
        if ((r = ino_rd(v, a, &in)))
            return r;
        a = in.parent ? in.parent : BFS_ROOT;
    }
    r = dir_find(v, p2, n2, l2, &tino, &t2);
    if (r == 0) {
        if (tino == ino) {                          /* solo cambian mayusculas/minusculas */
            if ((r = dir_del(v, p1, n1, l1)) || (r = dir_add(v, p2, n2, l2, ino, t1)))
                goto fail;
            return tx_commit(v);
        }
        if (t2 == BFS_T_DIR || t1 == BFS_T_DIR)
            return BFS_EEXIST;
        if ((r = dir_del(v, p2, n2, l2)) || (r = unlink_ino(v, tino)))   /* reemplazar un archivo */
            goto fail;
    } else if (r != BFS_ENOENT) {
        return r;
    }
    if ((r = dir_del(v, p1, n1, l1)) || (r = dir_add(v, p2, n2, l2, ino, t1)))
        goto fail;
    if (p1 != p2) {
        if ((r = ino_rd(v, ino, &in)))
            goto fail;
        in.parent = p2;
        if ((r = ino_wr(v, ino, &in)))
            goto fail;
    }
    return tx_commit(v);
fail:
    tx_abort(v);
    return r;
}

/* ---- crear el volumen ---- */
int bfs_mkfs(struct bfs *v, Q nsectors, const char *label)
{
    Q nb = nsectors / BFS_SPB, ninodes, i, used;
    struct bfs_super *sb = &v->sb;
    struct bfs_inode root;
    int k;
    if (nb < 1024)
        return BFS_EFMT;                            /* minimo 4 MB */
    memset(sb, 0, sizeof *sb);
    memcpy(sb->magic, "BFS-BOS1", 8);
    sb->version = BFS_VERSION;
    sb->bsize = BS;
    sb->nblocks = nb;
    ninodes = (nb / 16 + IPB - 1) / IPB * IPB;     /* un inodo por cada 64 KB */
    if (ninodes < 256) ninodes = 256;
    if (ninodes > 0xFFFFFF00ull) ninodes = 0xFFFFFF00ull;
    sb->ninodes = ninodes;
    sb->jstart = 1;
    sb->jblocks = BFS_JBLOCKS;
    sb->bmap_start = sb->jstart + sb->jblocks;
    sb->bmap_blocks = (nb + BITS - 1) / BITS;
    sb->imap_start = sb->bmap_start + sb->bmap_blocks;
    sb->imap_blocks = (ninodes + BITS - 1) / BITS;
    sb->itab_start = sb->imap_start + sb->imap_blocks;
    sb->itab_blocks = ninodes / IPB;
    sb->data_start = sb->itab_start + sb->itab_blocks;
    if (sb->data_start + 64 > nb)
        return BFS_EFMT;
    for (k = 0; label && label[k] && k < 31; k++)
        sb->label[k] = label[k];
    used = sb->data_start + 1;                      /* metadatos + el bloque de la raiz */
    /* mapa de bloques: lo usado al principio y lo que pasa del final */
    for (i = 0; i < sb->bmap_blocks; i++) {
        Q first = i * BITS, b;
        memset(v->bb, 0, BS);
        for (b = 0; b < BITS; b++) {
            Q blk = first + b;
            if (blk < used || blk >= nb)
                v->bb[b / 8] |= (B)(1u << (b % 8));
        }
        if (blk_wr(v, sb->bmap_start + i, v->bb))
            return BFS_EIO;
    }
    for (i = 0; i < sb->imap_blocks; i++) {
        Q first = i * BITS, b;
        memset(v->bb, 0, BS);
        for (b = 0; b < BITS; b++)
            if (first + b < 2 || first + b >= ninodes)
                v->bb[b / 8] |= (B)(1u << (b % 8));
        if (blk_wr(v, sb->imap_start + i, v->bb))
            return BFS_EIO;
    }
    memset(v->b1, 0, BS);                           /* diario vacio */
    if (blk_wr(v, sb->jstart, v->b1))
        return BFS_EIO;
    memset(&root, 0, sizeof root);                  /* la raiz */
    root.type = BFS_T_DIR;
    root.parent = BFS_ROOT;
    root.size = BS;
    root.next = 1;
    root.ext[0].start = sb->data_start;
    root.ext[0].len = 1;
    memset(v->ib, 0, BS);
    memcpy(v->ib + (BFS_ROOT % IPB) * 256, &root, 256);
    if (blk_wr(v, sb->itab_start + BFS_ROOT / IPB, v->ib))
        return BFS_EIO;
    memset(v->db, 0, BS);
    put_ent(v->db, 0, (W)BS, "", 0, 0);
    if (blk_wr(v, sb->data_start, v->db))
        return BFS_EIO;
    sb->free_blocks = nb - used;
    sb->free_inodes = ninodes - 2;
    memset(v->b1, 0, BS);
    memcpy(v->b1, sb, sizeof *sb);
    if (blk_wr(v, 0, v->b1))
        return BFS_EIO;
    v->ntx = 0;
    v->hint = sb->data_start;
    v->ihint = 2;
    return 0;
}

/* ---- revisar (fsck): recorre el arbol y compara con los mapas ---- */
struct ck { struct bfs *v; Q blocks, inodes; int err; char *msg; int max; int depth; };

static void ck_msg(struct ck *c, const char *a)
{
    int n = 0, i;
    if (!c->msg || c->err++)
        return;
    while (c->msg[n]) n++;
    for (i = 0; a[i] && n < c->max - 1; i++) c->msg[n++] = a[i];
    c->msg[n] = 0;
}
static int bit_is(struct bfs *v, Q start, Q i)
{
    if (meta_rd(v, start + i / BITS, v->bb))
        return -1;
    return (v->bb[(i % BITS) / 8] >> (i % 8)) & 1;
}
static int ck_dir(struct ck *c, D dino);
static int ck_cb(void *ctx, const char *nm, int nl, D ino, int type, Q sz)
{
    struct ck *c = ctx;
    struct bfs_inode in;
    D i;
    struct bfs_ext e;
    (void)nm; (void)nl; (void)sz;
    if (ino_rd(c->v, ino, &in) || in.type != (W)type) { ck_msg(c, "entrada con inodo malo; "); return 0; }
    if (bit_is(c->v, c->v->sb.imap_start, ino) != 1) ck_msg(c, "inodo en uso pero libre en el mapa; ");
    c->inodes++;
    for (i = 0; i < in.next; i++) {
        Q k;
        if (ext_get(c->v, &in, i, &e) || e.start < c->v->sb.data_start || e.start + e.len > c->v->sb.nblocks) {
            ck_msg(c, "tramo fuera del disco; ");
            break;
        }
        c->blocks += e.len;
        for (k = 0; k < e.len; k += 997)
            if (bit_is(c->v, c->v->sb.bmap_start, e.start + k) != 1) { ck_msg(c, "bloque en uso pero libre en el mapa; "); break; }
    }
    if (in.xblock) c->blocks++;
    if (type == BFS_T_DIR && c->depth < 64) {
        c->depth++;
        ck_dir(c, ino);
        c->depth--;
    }
    return 0;
}
static int ck_dir(struct ck *c, D dino) { return bfs_list(c->v, dino, ck_cb, c); }

int bfs_check(struct bfs *v, char *msg, int max)
{
    struct ck c;
    struct bfs_inode root;
    Q i, used = 0, iused = 0;
    D k;
    c.v = v; c.blocks = 0; c.inodes = 1; c.err = 0; c.msg = msg; c.max = max; c.depth = 0;
    if (msg && max) msg[0] = 0;
    if (ino_rd(v, BFS_ROOT, &root) || root.type != BFS_T_DIR) { ck_msg(&c, "la raiz esta mal; "); return c.err; }
    c.blocks = root.next ? root.ext[0].len : 0;
    for (k = 1; k < root.next; k++) c.blocks += root.ext[k].len;
    ck_dir(&c, BFS_ROOT);
    for (i = v->sb.data_start; i < v->sb.nblocks; i++) {
        if ((i % BITS) == 0 || i == v->sb.data_start)
            if (meta_rd(v, v->sb.bmap_start + i / BITS, v->bb)) return -1;
        if ((v->bb[(i % BITS) / 8] >> (i % 8)) & 1) used++;
    }
    for (i = 2; i < v->sb.ninodes; i++) {
        if ((i % BITS) == 0 || i == 2)
            if (meta_rd(v, v->sb.imap_start + i / BITS, v->bb)) return -1;
        if ((v->bb[(i % BITS) / 8] >> (i % 8)) & 1) iused++;
    }
    if (used != c.blocks) ck_msg(&c, "bloques marcados que nadie usa (o al reves); ");
    if (v->sb.nblocks - v->sb.data_start - used != v->sb.free_blocks) ck_msg(&c, "contador de bloques libres mal; ");
    if (iused + 1 != c.inodes) ck_msg(&c, "inodos marcados que nadie usa; ");
    if (v->sb.ninodes - 2 - iused != v->sb.free_inodes) ck_msg(&c, "contador de inodos libres mal; ");
    return c.err;
}
