/* libcdemo.c - demo de la mini-libc y los descriptores de archivo (kapi v11).
 * Compilar con: make store   y subirlo al hub, o enlazarlo como cualquier .BIN de la tienda. */
#include "sys/ui.h"
#include "sys/libc.h"
#include "mod_head.h"

int mod_main(const struct kapi *k)
{
    char *p; int fd, n; char bufr[64];
    K = k;
    if (!lc_init()) { c_puts("sin memoria\n"); return 1; }

    p = (char *)c_malloc(128);
    c_strlcpy(p, "hola mundo", 128);
    c_strcat(p, " desde la libc");
    c_printf("cadena: %s (largo %d)\n", p, c_strlen(p));
    c_printf("hex de 48879 = %x, atoi(\"-42\") = %d\n", 48879u, c_atoi("-42"));

    fd = c_open("HD0.1:/LIBCTEST.TXT", O_RDWR | O_NEW);
    if (fd >= 0) {
        c_write(fd, p, c_strlen(p));
        c_seek(fd, 0, SEEK_SET);
        n = c_read(fd, bufr, (int)sizeof bufr - 1);
        bufr[n > 0 ? n : 0] = 0;
        c_printf("lei de vuelta %d bytes: %s (tam %d)\n", n, bufr, c_fsize(fd));
        c_close(fd);
    } else c_printf("no pude abrir: %d\n", fd);

    c_free(p);
    lc_done();
    return MOD_OK;
}
