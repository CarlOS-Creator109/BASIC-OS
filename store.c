/* store.c - directorio y ranuras de programas (ver store.h). Sin libc. */
#include "store.h"

#define DIR_LBA   512
#define DATA_LBA  520
#define SLOT_SECT 80
#define ENTRY_OFF 16
#define ENTRY_LEN 24

static unsigned char dir[1024];
static int dir_loaded;
static const char magic[8] = { 'P', 'C', 'B', 'F', 'S', '0', '1', 0 };

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int name_eq(const unsigned char *a, const char *b)
{
    int i;
    for (i = 0; i < 16; i++) {
        char x = up((char)a[i]), y = i < STORE_NAME_MAX ? up(b[i]) : 0;
        if (x != y)
            return 0;
        if (!x)
            return 1;
    }
    return 1;
}

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static void wr32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static unsigned char *ent(int i) { return dir + ENTRY_OFF + i * ENTRY_LEN; }

int store_open(void)
{
    int i;
    if (dir_loaded)
        return STORE_OK;
    if (plat_disk_read(DIR_LBA, dir) || plat_disk_read(DIR_LBA + 1, dir + 512))
        return STORE_ENODEV;
    for (i = 0; i < 8; i++)
        if (dir[i] != (unsigned char)magic[i])
            break;
    if (i < 8) {                                /* disco sin formatear: directorio vacio */
        for (i = 0; i < (int)sizeof dir; i++)
            dir[i] = 0;
        for (i = 0; i < 8; i++)
            dir[i] = (unsigned char)magic[i];
    }
    dir_loaded = 1;
    return STORE_OK;
}

static int dir_flush(void)
{
    if (plat_disk_write(DIR_LBA, dir) || plat_disk_write(DIR_LBA + 1, dir + 512))
        return STORE_ENODEV;
    return STORE_OK;
}

static int find(const char *name)
{
    int i;
    for (i = 0; i < STORE_SLOTS; i++)
        if (ent(i)[0] && name_eq(ent(i), name))
            return i;
    return -1;
}

int store_entry(int i, char *name, int *len)
{
    int k;
    if (store_open() || i < 0 || i >= STORE_SLOTS || !ent(i)[0])
        return 0;
    for (k = 0; k < 16; k++)
        name[k] = (char)ent(i)[k];
    name[16] = 0;
    *len = (int)rd32(ent(i) + 16);
    return 1;
}

int store_save(const char *name, const char *data, int len)
{
    unsigned char sec[512];
    int s, i, k, off, n, r;

    if (len < 0 || len > STORE_SLOT_BYTES)
        return STORE_E2BIG;
    if (!name[0])
        return STORE_ENOENT;
    r = store_open();
    if (r)
        return r;
    s = find(name);
    if (s < 0)
        for (i = 0; i < STORE_SLOTS; i++)
            if (!ent(i)[0]) {
                s = i;
                break;
            }
    if (s < 0)
        return STORE_EFULL;

    for (off = 0; off < len; off += 512) {      /* datos primero, directorio al final */
        n = len - off < 512 ? len - off : 512;
        for (k = 0; k < 512; k++)
            sec[k] = k < n ? (unsigned char)data[off + k] : 0;
        if (plat_disk_write((unsigned int)(DATA_LBA + s * SLOT_SECT + off / 512), sec))
            return STORE_ENODEV;
    }
    for (k = 0; k < 16; k++)
        ent(s)[k] = 0;
    for (k = 0; k < STORE_NAME_MAX && name[k]; k++)
        ent(s)[k] = (unsigned char)up(name[k]);
    wr32(ent(s) + 16, (unsigned int)len);
    wr32(ent(s) + 20, 0);
    return dir_flush();
}

int store_load(const char *name, char *buf, int max)
{
    unsigned char sec[512];
    int s, len, off, n, k, r;

    r = store_open();
    if (r)
        return r;
    s = find(name);
    if (s < 0)
        return STORE_ENOENT;
    len = (int)rd32(ent(s) + 16);
    if (len > max || len > STORE_SLOT_BYTES)
        return STORE_E2BIG;
    for (off = 0; off < len; off += 512) {
        if (plat_disk_read((unsigned int)(DATA_LBA + s * SLOT_SECT + off / 512), sec))
            return STORE_ENODEV;
        n = len - off < 512 ? len - off : 512;
        for (k = 0; k < n; k++)
            buf[off + k] = (char)sec[k];
    }
    return len;
}

int store_delete(const char *name)
{
    int s, r = store_open();
    if (r)
        return r;
    s = find(name);
    if (s < 0)
        return STORE_ENOENT;
    ent(s)[0] = 0;
    return dir_flush();
}
