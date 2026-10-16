/* sys.c - lectura de /SYS (ver sys.h). Usa la capa de bloques de la plataforma. */
#include "sys.h"
#include "fs.h"
#include "store.h"            /* plat_disk_read */

static unsigned char dirbuf[SYS_DIR_SECT * 512];
static int dir_ok;                  /* 1 = cargado, -1 = no hay /SYS */

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static int load_dir(void)
{
    static const char magic[8] = { 'P', 'C', 'B', 'S', 'Y', 'S', '1', 0 };
    int i;
    if (dir_ok)
        return dir_ok > 0 ? 0 : FS_ENODEV;
    dir_ok = -1;
    for (i = 0; i < SYS_DIR_SECT; i++)
        if (plat_disk_read((unsigned int)(SYS_DIR_LBA + i), dirbuf + i * 512))
            return FS_EIO;
    for (i = 0; i < 8; i++)
        if (dirbuf[i] != (unsigned char)magic[i])
            return FS_ENODEV;
    dir_ok = 1;
    return 0;
}

static const unsigned char *entry(int i)
{
    return dirbuf + 16 + i * 32;
}

static int count(void)
{
    int n = (int)rd32(dirbuf + 8);
    return n > (SYS_DIR_SECT * 512 - 16) / 32 ? (SYS_DIR_SECT * 512 - 16) / 32 : n;
}

int sys_list(sys_cb cb)
{
    int i, r = load_dir();
    if (r)
        return r;
    for (i = 0; i < count(); i++)
        cb((const char *)entry(i), rd32(entry(i) + 24));
    return 0;
}

int sys_read(const char *name, char *buf, int max, unsigned int *total)
{
    unsigned char sec[512];
    unsigned int lba, size, got = 0, left;
    int i, k, r = load_dir();

    if (r)
        return r;
    while (*name == '/' || *name == '\\')
        name++;
    for (i = 0; i < count(); i++) {
        const unsigned char *e = entry(i);
        for (k = 0; k < 20 && up((char)e[k]) == up(name[k]); k++)
            if (!e[k])
                goto found;
    }
    return FS_ENOENT;
found:
    lba = rd32(entry(i) + 20);
    size = rd32(entry(i) + 24);
    if (total)
        *total = size;
    left = size < (unsigned int)max ? size : (unsigned int)max;
    while (got < left) {
        unsigned int n = left - got < 512 ? left - got : 512;
        if (plat_disk_read(lba++, sec))
            return FS_EIO;
        for (k = 0; k < (int)n; k++)
            buf[got + k] = (char)sec[k];
        got += n;
    }
    return (int)got;
}
