/* fs.h - sistemas de archivos: FAT12/16/32 e ISO9660 (solo lectura) y BFS, el de BASIC-OS (lectura y escritura) */
#ifndef FS_H
#define FS_H

#define FS_OK       0
#define FS_ENODEV  -1       /* no existe la unidad */
#define FS_ENOENT  -2       /* no existe el archivo o directorio */
#define FS_EFMT    -3       /* sistema de archivos no reconocido */
#define FS_E2BIG   -4       /* no cabe en el buffer */
#define FS_EIO     -5       /* error de lectura */
#define FS_EISDIR  -6
#define FS_EROFS   -7       /* solo se puede escribir en BFS */
#define FS_ENOSPC  -8
#define FS_EEXIST  -9
#define FS_ENOTEMPTY -10
#define FS_ENAME   -11

typedef void (*fs_cb)(const char *name, unsigned int size, int isdir);

/* spec: "HD0:", "HD1.2:/DIR/", "CD0:ARCHIVO.TXT" ... */
int fs_dir(const char *spec, fs_cb cb);
/* Lee el archivo en buf (hasta max bytes). Devuelve los bytes leidos o un error < 0.
 * *total recibe el tamano real del archivo (puede ser mayor que max). */
int fs_read(const char *spec, char *buf, int max, unsigned int *total);

/* escritura (solo en particiones BFS). spec: "HD0.1:/CARPETA/ARCHIVO" */
int fs_write(const char *spec, const void *data, unsigned int size);     /* crea o reemplaza */
int fs_mkdir(const char *spec);
int fs_remove(const char *spec);                                          /* archivo o carpeta vacia */
int fs_rename(const char *spec, const char *to);                          /* to: otra ruta del mismo disco */
int fs_space(const char *spec, unsigned long long *free_bytes, unsigned long long *total_bytes);
const char *fs_kind(void);
int fs_format(const char *spec, const char *label);                      /* "HD1.1:" (particion tipo 0xB0) */                                                /* "BFS", "FAT16"... del ultimo montado */

#endif
