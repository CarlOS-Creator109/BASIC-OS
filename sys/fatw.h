/* fatw.h - escritor FAT16 minimo (lo usan PKG, BCC y COSMOS): guarda archivos en HDn.1:/APPS/ (o otra carpeta de la raiz).
 * Se incluye una vez, despues de definir K (const struct kapi *), los tipos u8/u16/u32, memset/memcpy y
 * FW_FAT, FW_ROOT, FW_BUF, FW_SCR (zonas de RAM: 128 KB, 16 KB, 64 KB y 64 KB). Solo discos de la BIOS. */
#ifndef FATW_H
#define FATW_H

/* ---- escritor FAT16 minimo: guarda los programas en HDn.1:/APPS/ del disco de arranque (solo discos de la BIOS) ---- */
static struct {
    int ok, bfs; char dev[8], pn; u32 part, spc, res, nfat, rootent, fatsz, root_lba, data_lba, nclus, dlo, dhi, csize;
} fw;

/* ---- disco BFS (kapi v9): lo escribe el kernel con K->fsop ---- */
static void fw_bspec(char *o, const char *dir11, const char *name)
{
    int j = 0, i;
    for (i = 0; fw.dev[i]; i++) o[j++] = fw.dev[i];
    o[j++] = '.'; o[j++] = fw.pn; o[j++] = ':'; o[j++] = '/';
    for (i = 0; i < 8 && dir11[i] && dir11[i] != ' '; i++) o[j++] = dir11[i];
    if (name) {
        o[j++] = '/';
        for (i = 0; name[i] && j < 60; i++) o[j++] = name[i];
    }
    o[j] = 0;
}
static int fw_bfs_save(const char *dir11, const char *name, const u8 *data, u32 size)
{
    char sp[64];
    int r;
    fw_bspec(sp, dir11, 0);
    K->fsop(FSOP_MKDIR, sp, 0, 0, 0);               /* si ya existe, da igual */
    fw_bspec(sp, dir11, name);
    r = K->fsop(FSOP_WRITE, sp, data, size, 0);
    return r == 0 ? 0 : (r == -8 ? 4 : (r == -11 ? 1 : 3));
}
/* se asignan en fw_init() porque FW_* puede ser una zona dada por el asignador en runtime */
static u16 *FATP;
static u8 *FROOT;
static u8 *FBUF;
static u8 *FSCR;

static int fw_wr(u32 lba, u32 n, const u8 *src)           /* escribe y relee: 0 = bien */
{
    while (n) {
        u32 c = n > 128 ? 128 : n, i;
        if (K->blk(fw.dev, 1, lba, c, (void *)src) || K->blk(fw.dev, 0, lba, c, FSCR))
            return 1;
        for (i = 0; i < c * 512; i++)
            if (FSCR[i] != src[i])
                return 1;
        lba += c; src += c * 512; n -= c;
    }
    return 0;
}
static void fat_set(u32 c, u32 v)
{
    u32 sc = c / 256;
    FATP[c] = (u16)v;
    if (sc < fw.dlo) fw.dlo = sc;
    if (sc > fw.dhi) fw.dhi = sc;
}
static int fw_flush_fat(void)
{
    u32 n, k;
    if (fw.dlo > fw.dhi)
        return 0;
    n = fw.dhi - fw.dlo + 1;
    for (k = 0; k < (u32)fw.nfat; k++)
        if (fw_wr(fw.part + fw.res + k * fw.fatsz + fw.dlo, n, (const u8 *)FATP + fw.dlo * 512))
            return 1;
    fw.dlo = 0xFFFFFFFFu; fw.dhi = 0;
    return 0;
}
static u32 clus_lba(u32 c) { return fw.data_lba + (c - 2) * fw.spc; }

static void fw_init(void)
{
    static u8 s0[512], bs[512];
    int i, nd = 0;
    u32 total, rootsec, nclus, part = 0, ndev;
    char name[8];
    FATP  = (u16 *)(unsigned long)FW_FAT;           /* las zonas del escritor, ya con el banco privado listo */
    FROOT = (u8 *)(unsigned long)FW_ROOT;
    FBUF  = (u8 *)(unsigned long)FW_BUF;
    FSCR  = (u8 *)(unsigned long)FW_SCR;
    fw.ok = 0;
    for (ndev = 0; ndev < 16; ndev++) {
        char line[64];
        int k = 0;
        if (!K->drive_line((int)ndev, line))
            continue;
        while (line[k] && line[k] != ' ' && k < 7) { name[k] = line[k]; k++; }
        name[k] = 0;
        if ((K->blk(name, 3, 0, 0, 0) & 3) == 3) { nd = 1; break; }
    }
    if (!nd)
        return;
    for (i = 0; name[i]; i++) fw.dev[i] = name[i];
    fw.dev[i] = 0;
    fw.bfs = 0;
    if (K->blk(fw.dev, 0, 0, 1, s0) || s0[510] != 0x55 || s0[511] != 0xAA)
        return;
    fw.pn = '1';
    if (K->ver >= 9 && K->fsop)                     /* BFS (y desde la v10 tambien FAT16/32): la escribe el kernel */
        for (i = 0; i < 4; i++) {
            u8 t = s0[0x1BE + 16 * i + 4];
            if (t == 0xB0 || (K->ver >= 10 && (t == 0x0B || t == 0x0C || t == 4 || t == 6 || t == 0x0E))) {
                fw.bfs = 1;
                fw.ok = 1;
                fw.pn = (char)('1' + i);
                return;
            }
        }
    for (i = 0; i < 4; i++) {
        const u8 *e = s0 + 0x1BE + 16 * i;
        if ((e[4] == 4 || e[4] == 6 || e[4] == 0x0E) && *(const u32 *)(e + 8)) { part = *(const u32 *)(e + 8); break; }
    }
    if (!part || K->blk(fw.dev, 0, part, 1, bs) || bs[510] != 0x55 || *(u16 *)(bs + 11) != 512)
        return;
    fw.part = part; fw.spc = bs[13]; fw.res = *(u16 *)(bs + 14); fw.nfat = bs[16]; fw.rootent = *(u16 *)(bs + 17);
    total = *(u16 *)(bs + 19) ? *(u16 *)(bs + 19) : *(u32 *)(bs + 32);
    fw.fatsz = *(u16 *)(bs + 22);
    if (!fw.spc || (fw.spc & (fw.spc - 1)) || !fw.res || !fw.nfat || fw.nfat > 2 || !fw.fatsz || fw.fatsz > 256 ||
        (fw.rootent & 15) || fw.rootent > 512 || fw.spc > 64)
        return;
    rootsec = fw.rootent / 16;
    if (total < fw.res + fw.nfat * fw.fatsz + rootsec)
        return;
    nclus = (total - fw.res - fw.nfat * fw.fatsz - rootsec) / fw.spc;
    if (nclus < 4085 || nclus > 65524)
        return;
    fw.nclus = nclus; fw.csize = fw.spc * 512;
    fw.root_lba = part + fw.res + fw.nfat * fw.fatsz;
    fw.data_lba = fw.root_lba + rootsec;
    memset(FROOT, 0, 16384);
    if (K->blk(fw.dev, 0, part + fw.res, fw.fatsz, FATP) || K->blk(fw.dev, 0, fw.root_lba, rootsec, FROOT))
        return;
    fw.dlo = 0xFFFFFFFFu; fw.dhi = 0;
    fw.ok = 1;
}

static void fw_spec(char *out, const char *file)      /* "HD0.1:/APPS/NOMBRE" */
{
    int k = 0, j;
    for (j = 0; fw.dev[j]; j++) out[k++] = fw.dev[j];
    out[k++] = '.'; out[k++] = '1'; out[k++] = ':'; out[k++] = '/';
    out[k++] = 'A'; out[k++] = 'P'; out[k++] = 'P'; out[k++] = 'S'; out[k++] = '/';
    for (j = 0; file && file[j] && k < 60; j++) out[k++] = file[j];
    out[k] = 0;
}

static int name11(const char *n, u8 *o)
{
    int b = 0, e = 0, dot = 0, i;
    memset(o, ' ', 11);
    for (i = 0; n[i]; i++) {
        char c = n[i];
        if (c == '.') { if (dot || !b) return 0; dot = 1; continue; }
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
        if (dot) { if (e >= 3) return 0; o[8 + e++] = (u8)c; }
        else { if (b >= 8) return 0; o[b++] = (u8)c; }
    }
    return b > 0;
}

/* n clusters libres encadenados; devuelve el primero (0 = no hay) */
static u32 fw_alloc(u32 n)
{
    u32 c, prev = 0, first = 0, got = 0;
    for (c = 2; c < fw.nclus + 2 && got < n; c++) {
        if (FATP[c])
            continue;
        if (prev) fat_set(prev, c); else first = c;
        prev = c;
        got++;
        FATP[c] = 0xFFFF;                       /* provisional (se marca sucio abajo) */
    }
    if (got < n) {                              /* no cabe: deshacer */
        for (c = first; c && c != 0xFFFF; ) {
            u32 nx = FATP[c];
            FATP[c] = 0;
            c = (nx >= 0xFFF8 || nx < 2) ? 0 : nx;
        }
        return 0;
    }
    fat_set(prev, 0xFFFF);
    return first;
}
static void fw_free_chain(u32 c)
{
    u32 guard = 0;
    while (c >= 2 && c < fw.nclus + 2 && guard++ <= fw.nclus) {
        u32 nx = FATP[c];
        fat_set(c, 0);
        if (nx >= 0xFFF8)
            break;
        c = nx;
    }
}

static int eq11(const u8 *a, const u8 *b)
{
    int i;
    for (i = 0; i < 11; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* Busca la entrada nm en el directorio d (0 = raiz). Devuelve 1 si existe. *ent apunta dentro de FROOT (raiz) o de FBUF
 * (el cluster que contiene la entrada) y wlba y wn dicen que sectores hay que reescribir. Con make = 1 y sin existir,
 * deja en *ent una entrada libre en blanco (si el directorio esta lleno, enlaza un cluster nuevo; la FAT queda sucia). */
static int fw_find(u32 d, const u8 *nm, int make, u8 **ent, u32 *wlba, u32 *wn)
{
    u32 c = d, i, prev = 0, guard = 0, cur = 0, fc = 0;
    int fi = -1;
    *ent = 0;
    if (!d) {
        for (i = 0; i < fw.rootent; i++) {
            u8 *e = FROOT + 32 * i;
            if (e[0] == 0) { if (fi < 0) fi = (int)i; break; }
            if (e[0] == 0xE5) { if (fi < 0) fi = (int)i; continue; }
            if ((e[11] & 0x3F) == 0x0F || (e[11] & 8)) continue;
            if (eq11(e, nm)) { *ent = e; *wlba = fw.root_lba + i / 16; *wn = 1; return 1; }
        }
        if (make && fi >= 0) { *ent = FROOT + 32 * fi; *wlba = fw.root_lba + (u32)fi / 16; *wn = 1; memset(*ent, 0, 32); }
        return 0;
    }
    while (c >= 2 && c < fw.nclus + 2 && guard++ <= fw.nclus) {
        if (K->blk(fw.dev, 0, clus_lba(c), fw.spc, FBUF))
            return 0;
        cur = c;
        for (i = 0; i < fw.csize / 32; i++) {
            u8 *e = FBUF + 32 * i;
            if (e[0] == 0) { if (!fc) { fc = c; fi = (int)i; } goto done; }
            if (e[0] == 0xE5) { if (!fc) { fc = c; fi = (int)i; } continue; }
            if ((e[11] & 0x3F) == 0x0F || (e[11] & 8)) continue;
            if (eq11(e, nm)) { *ent = e; *wlba = clus_lba(c); *wn = fw.spc; return 1; }
        }
        prev = c;
        c = FATP[c];
        if (c >= 0xFFF8)
            break;
    }
done:
    if (!make)
        return 0;
    if (fc) {                                   /* hay una libre: asegurarse de tener ese cluster en FBUF */
        if (cur != fc && K->blk(fw.dev, 0, clus_lba(fc), fw.spc, FBUF))
            return 0;
        *ent = FBUF + 32 * fi; *wlba = clus_lba(fc); *wn = fw.spc;
        memset(*ent, 0, 32);
        return 0;
    }
    {                                           /* lleno: un cluster mas al final */
        u32 nc = prev ? fw_alloc(1) : 0;
        if (!nc)
            return 0;
        fat_set(prev, nc);
        memset(FBUF, 0, fw.csize);
        *ent = FBUF; *wlba = clus_lba(nc); *wn = fw.spc;
    }
    return 0;
}

static void put_entry(u8 *e, const u8 *nm, int attr, u32 first, u32 size)
{
    memset(e, 0, 32);
    memcpy(e, nm, 11);
    e[11] = (u8)attr;
    *(u16 *)(e + 14) = 0; *(u16 *)(e + 16) = 0x0021; *(u16 *)(e + 18) = 0x0021;
    *(u16 *)(e + 22) = 0; *(u16 *)(e + 24) = 0x0021;
    *(u16 *)(e + 26) = (u16)first; *(u32 *)(e + 28) = size;
}

/* devuelve el primer cluster de APPS (lo crea si hace falta); 0 = error */
/* carpeta de la raiz (la crea si no existe): su primer cluster, 0 = no se pudo */
static u32 fw_root_dir(const u8 *nm)
{
    u8 *e; u32 lba, n, c;
    if (fw_find(0, nm, 0, &e, &lba, &n) && (e[11] & 0x10))
        return *(u16 *)(e + 26);
    if (!fw_find(0, nm, 1, &e, &lba, &n) && !e)
        return 0;
    c = fw_alloc(1);
    if (!c)
        return 0;
    memset(FBUF, 0, fw.csize);
    put_entry(FBUF, (const u8 *)".          ", 0x10, c, 0);
    put_entry(FBUF + 32, (const u8 *)"..         ", 0x10, 0, 0);
    if (fw_wr(clus_lba(c), fw.spc, FBUF))
        return 0;
    put_entry(e, nm, 0x10, c, 0);
    if (fw_flush_fat() || fw_wr(lba, n, (const u8 *)FROOT + (lba - fw.root_lba) * 512))
        return 0;
    return c;
}
static u32 fw_apps_dir(void) { return fw_root_dir((const u8 *)"APPS       "); }

/* guarda data[size] como HDn.1:/CARPETA/nombre (dir11 = nombre de la carpeta en formato FAT, 11 letras). 0 = bien */
static int fw_save_in(const char *dir11, const char *name, const u8 *data, u32 size)
{
    u8 nm[11], *e; u32 d, lba, n, first = 0, old = 0, nc, c, off;
    int found;
    if (fw.ok && fw.bfs)
        return fw_bfs_save(dir11, name, data, size);
    if (!fw.ok || !name11(name, nm))
        return 1;
    d = fw_root_dir((const u8 *)dir11);
    if (!d)
        return 2;
    found = fw_find(d, nm, 0, &e, &lba, &n);
    if (found)
        old = *(u16 *)(e + 26);
    nc = (size + fw.csize - 1) / fw.csize;
    if (nc) {
        first = fw_alloc(nc);
        if (!first)
            return 4;                           /* disco lleno */
        c = first; off = 0;
        while (nc) {
            u32 run = 1;
            while (run < nc && FATP[c + run - 1] == c + run) run++;
            if (fw_wr(clus_lba(c), run * fw.spc, data + off) ) return 5;
            off += run * fw.csize; nc -= run;
            if (nc) c = FATP[c + run - 1];
        }
    }
    found = fw_find(d, nm, 1, &e, &lba, &n);        /* una sola vez con make: un cluster nuevo de directorio aun no esta en disco */
    if (!e)
        return 3;
    put_entry(e, nm, 0x20, first, size);
    if (old) fw_free_chain(old);
    if (fw_flush_fat())
        return 6;
    if (lba >= fw.root_lba && lba < fw.root_lba + 32)
        return fw_wr(lba, n, (const u8 *)FROOT + (lba - fw.root_lba) * 512) ? 7 : 0;
    return fw_wr(lba, n, FBUF) ? 7 : 0;
}

/* guarda data[size] como HDn.1:/APPS/nombre. 0 = bien */
static int fw_save(const char *name, const u8 *data, u32 size) { return fw_save_in("APPS       ", name, data, size); }

static int fw_delete(const char *name)
{
    u8 nm[11], *e; u32 d, lba, n, first;
    if (fw.ok && fw.bfs) {
        char sp[64];
        fw_bspec(sp, "APPS       ", name);
        return K->fsop(FSOP_REMOVE, sp, 0, 0, 0) ? 2 : 0;
    }
    if (!fw.ok || !name11(name, nm))
        return 1;
    d = fw_apps_dir();
    if (!d || !fw_find(d, nm, 0, &e, &lba, &n))
        return 2;
    first = *(u16 *)(e + 26);
    e[0] = 0xE5;
    if (fw_wr(lba, n, FBUF))
        return 3;
    fw_free_chain(first);
    return fw_flush_fat() ? 4 : 0;
}

#endif
