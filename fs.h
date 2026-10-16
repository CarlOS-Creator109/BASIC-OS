/* fs.h - lectura de sistemas de archivos: FAT12/16/32 e ISO9660 (CD) */
#ifndef FS_H
#define FS_H

#define FS_OK       0
#define FS_ENODEV  -1       /* no existe la unidad */
#define FS_ENOENT  -2       /* no existe el archivo o directorio */
#define FS_EFMT    -3       /* sistema de archivos no reconocido */
#define FS_E2BIG   -4       /* no cabe en el buffer */
#define FS_EIO     -5       /* error de lectura */
#define FS_EISDIR  -6

typedef void (*fs_cb)(const char *name, unsigned int size, int isdir);

/* spec: "HD0:", "HD1.2:/DIR/", "CD0:ARCHIVO.TXT" ... */
int fs_dir(const char *spec, fs_cb cb);
/* Lee el archivo en buf (hasta max bytes). Devuelve los bytes leidos o un error < 0.
 * *total recibe el tamano real del archivo (puede ser mayor que max). */
int fs_read(const char *spec, char *buf, int max, unsigned int *total);

#endif
