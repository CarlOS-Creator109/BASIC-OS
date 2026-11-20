/* cmsave.c - COSMOS guarda en el disco lo que baja (videos .MP4 para MPLAY): HDn.1:/VIDEOS/NOMBRE
 * Usa el escritor FAT16 de PKG/BCC (sys/fatw.h). Va en su propio archivo y se compila sin SSE (-mgeneral-regs-only):
 * las llamadas a la BIOS de los discos apagan SSE y el resto de COSMOS lo vuelve a prender (sse_on). GPL-3.0-or-later. */
#include "kapi.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

static const struct kapi *K;

static void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
static void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

#define FW_FAT     0xB200000u           /* las mismas zonas que PKG (nunca corren a la vez) */
#define FW_ROOT    0xB220000u
#define FW_BUF     0xB230000u
#define FW_SCR     0xB240000u
#include "sys/fatw.h"

/* guarda data[size] como HDn.1:/<carpeta>/<name>. dir11 = carpeta en formato FAT ("VIDEOS     ").
 * where (>= 40 bytes) recibe la ruta. 0 = bien; 100 = no hay disco FAT de la BIOS; 1 = nombre que no es 8.3; 4 = disco lleno */
int cm_save(const struct kapi *k, const char *dir11, const char *name, const void *data, unsigned size, char *where)
{
    int r, i, j = 0;
    K = k;
    fw_init();
    if (!fw.ok)
        return 100;
    r = fw_save_in(dir11, name, (const u8 *)data, size);
    if (where) {
        for (i = 0; fw.dev[i]; i++) where[j++] = fw.dev[i];
        where[j++] = '.'; where[j++] = '1'; where[j++] = ':'; where[j++] = '/';
        for (i = 0; i < 8 && dir11[i] != ' '; i++) where[j++] = dir11[i];
        where[j++] = '/';
        for (i = 0; name[i] && j < 39; i++) where[j++] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
        where[j] = 0;
    }
    (void)memcpy;
    (void)fw_delete;
    (void)fw_spec;
    (void)fw_save;
    return r;
}
