/* fd.c - descriptores de archivo: la base del modelo de E/S de Unix en BASIC-OS.
 *
 * open / read / write / lseek / close de verdad, cada descriptor con su propia posicion. Esto es
 * lo que hace posible despues los pipes, la redireccion y el "todo es un archivo".
 *
 * Por dentro, HOY, al abrir se trae el archivo entero a un buffer de RAM y al cerrar se vuelca
 * (fs.c todavia no sabe leer desde un offset: fs_read siempre empieza en el principio). La
 * INTERFAZ ya es la de Unix, asi que el dia que fs.c lea por tramos se cambia solo aqui dentro
 * y ningun programa se entera. Esa es la gracia de poner la interfaz primero.
 *
 * Memoria: un banco de 32 MB del asignador, partido en FD_MAX ranuras. De ahi el limite de
 * tamano por archivo abierto; los binarios grandes se siguen leyendo enteros con K->read.
 */
#include "kernel.h"
#include "fs.h"
#include "kapi.h"

#define FD_MAX   8                        /* descriptores abiertos a la vez */
#define FD_SLOT  0x400000u                /* 4 MB por descriptor (8 x 4 MB = un banco de 32 MB) */

struct fdent {
    int used, dirty;
    unsigned pos, len;
    int mode;
    char spec[80];
    unsigned char *buf;
};

static struct fdent fds[FD_MAX];
static unsigned long long fd_bank;        /* banco prestado al asignador (0 = no hay) */
static int fd_open_count;

static void spec_copy(char *d, const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof ((struct fdent *)0)->spec - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* pide el banco la primera vez que hace falta. 0 = no se pudo */
static int fd_need_bank(void)
{
    if (fd_bank)
        return 1;
    fd_bank = ram_api(RAM_ALLOC, 0, 1, "FD");
    return fd_bank != 0;
}

static void fd_maybe_free_bank(void)
{
    if (fd_bank && fd_open_count == 0) {
        ram_api(RAM_FREE, fd_bank, 0, 0);
        fd_bank = 0;
    }
}

/* abre 'spec'. mode: O_RD / O_WR / O_RDWR, mas O_NEW (crear o vaciar) y O_APP (escribir al final).
 * Devuelve el descriptor (>= 0) o un FDERR_*. */
int fd_open(const char *spec, int mode)
{
    int i;
    struct fdent *f = 0;

    if (!spec || !spec[0])
        return FDERR_BADF;
    for (i = 0; i < FD_MAX; i++)
        if (!fds[i].used) { f = &fds[i]; break; }
    if (!f)
        return FDERR_MANY;
    if (!fd_need_bank())
        return FDERR_NOMEM;

    f->buf = (unsigned char *)(unsigned long)(fd_bank + (unsigned long long)i * FD_SLOT);
    f->len = f->pos = 0;
    f->dirty = 0;
    f->mode = mode;
    spec_copy(f->spec, spec);

    if (!(mode & O_NEW)) {                               /* traer lo que ya hay */
        unsigned total = 0;
        int n = fs_read(spec, (char *)f->buf, (int)FD_SLOT, &total);
        if (n < 0) {
            if (!(mode & (O_WR | O_RDWR)))               /* solo lectura y no existe: error */
                return FDERR_NOENT;
            n = 0;                                       /* para escribir, se crea al cerrar */
        }
        if (total > FD_SLOT)
            return FDERR_BIG;
        f->len = (unsigned)n;
    }
    if (mode & O_APP)
        f->pos = f->len;
    f->used = 1;
    fd_open_count++;
    return i;
}

static struct fdent *fd_get(int fd)
{
    if (fd < 0 || fd >= FD_MAX || !fds[fd].used)
        return 0;
    return &fds[fd];
}

int fd_read(int fd, void *buf, int n)
{
    struct fdent *f = fd_get(fd);
    unsigned left;
    if (!f || n < 0)
        return FDERR_BADF;
    if (f->mode & O_WR)                                  /* abierto solo para escribir */
        return FDERR_BADF;
    left = f->len - f->pos;
    if (f->pos >= f->len)
        return 0;                                        /* fin de archivo */
    if ((unsigned)n > left)
        n = (int)left;
    memcpy(buf, f->buf + f->pos, (unsigned long)n);
    f->pos += (unsigned)n;
    return n;
}

int fd_write(int fd, const void *buf, int n)
{
    struct fdent *f = fd_get(fd);
    if (!f || n < 0)
        return FDERR_BADF;
    if (!(f->mode & (O_WR | O_RDWR)))
        return FDERR_BADF;
    if (f->pos + (unsigned)n > FD_SLOT)
        n = (int)(FD_SLOT - f->pos);
    if (n <= 0)
        return FDERR_BIG;
    memcpy(f->buf + f->pos, buf, (unsigned long)n);
    f->pos += (unsigned)n;
    if (f->pos > f->len)
        f->len = f->pos;
    f->dirty = 1;
    return n;
}

/* whence: SEEK_SET 0, SEEK_CUR 1, SEEK_END 2. Devuelve la posicion nueva. */
int fd_seek(int fd, int off, int whence)
{
    struct fdent *f = fd_get(fd);
    long p;
    if (!f)
        return FDERR_BADF;
    p = whence == 1 ? (long)f->pos + off : whence == 2 ? (long)f->len + off : off;
    if (p < 0)
        return FDERR_BADF;
    if (p > (long)FD_SLOT)
        return FDERR_BIG;
    f->pos = (unsigned)p;
    return (int)p;
}

/* cierra y, si hubo cambios, escribe el archivo. 0 = bien */
int fd_close(int fd)
{
    struct fdent *f = fd_get(fd);
    int r = 0;
    if (!f)
        return FDERR_BADF;
    if (f->dirty)
        r = fs_write(f->spec, f->buf, f->len) == 0 ? 0 : FDERR_IO;
    f->used = f->dirty = 0;
    f->buf = 0;
    if (fd_open_count > 0)
        fd_open_count--;
    fd_maybe_free_bank();
    return r;
}
