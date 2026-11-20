/* bfs.h - BFS, el sistema de archivos de BASIC-OS (BASIC-OS File System)
 *
 * Una particion (tipo 0xB0 en la tabla del MBR) dividida en bloques de 4 KB, numeros de 64 bits:
 *
 *   bloque 0                superbloque (struct bfs_super)
 *   1 .. 1+BFS_JBLOCKS-1    diario: cabecera + copias de los bloques de metadatos de la ultima operacion
 *   bmap                    mapa de bits de bloques (1 = usado)
 *   imap                    mapa de bits de inodos
 *   itab                    tabla de inodos (256 bytes cada uno, 16 por bloque; el 0 no se usa, el 1 es la raiz)
 *   ...                     datos y directorios
 *
 * Archivos por tramos (extents): 12 en el inodo y, si hacen falta mas, un bloque con 255 mas. Directorios: archivos
 * con entradas de largo variable (como ext2): nombre de hasta 255 bytes en UTF-8, se respetan mayusculas y minusculas
 * pero se buscan sin distinguirlas (BASIC escribe todo en mayusculas).
 *
 * Diario de metadatos: cada operacion (guardar, crear carpeta, borrar...) junta en memoria los bloques de metadatos que
 * cambia. Los datos del archivo se escriben primero, directo a sus bloques nuevos; luego las copias de los metadatos van
 * al diario, luego la cabecera del diario (con su suma), luego cada bloque a su lugar. Si se corta la luz a la mitad,
 * al montar se repite lo que quedo en el diario (o, si la cabecera no alcanzo a escribirse, la operacion no paso).
 * El codigo (bfs.c) es el mismo en el kernel y en tools/mkbfs (Linux). GPL-3.0-or-later.
 */
#ifndef BFS_H
#define BFS_H

#define BFS_BS        4096u
#define BFS_SPB       8u                    /* sectores de 512 por bloque */
#define BFS_PTYPE     0xB0                  /* tipo de particion en el MBR */
#define BFS_VERSION   1u
#define BFS_ROOT      1u                    /* inodo de la carpeta raiz */
#define BFS_JBLOCKS   130u                  /* diario: cabecera + 128 copias + 1 de reserva */
#define BFS_TXMAX     128u                  /* bloques de metadatos que puede cambiar una operacion */
#define BFS_NEXT      12u                   /* tramos dentro del inodo */
#define BFS_XEXT      255u                  /* tramos en el bloque extra */
#define BFS_NAMEMAX   255u

#define BFS_T_FREE    0
#define BFS_T_FILE    1
#define BFS_T_DIR     2

/* errores (negativos) */
#define BFS_EIO      -1
#define BFS_ENOENT   -2
#define BFS_EFMT     -3
#define BFS_ENOSPC   -4
#define BFS_EEXIST   -5
#define BFS_ENOTDIR  -6
#define BFS_EISDIR   -7
#define BFS_ENOTEMPTY -8
#define BFS_ENAME    -9
#define BFS_E2BIG    -10                    /* la operacion cambia demasiados metadatos de una vez */
#define BFS_EBUSY    -11

typedef unsigned char bfs_u8;
typedef unsigned short bfs_u16;
typedef unsigned int bfs_u32;
typedef unsigned long long bfs_u64;

struct bfs_super {
    char magic[8];                          /* "BFS-BOS1" */
    bfs_u32 version, bsize;
    bfs_u64 nblocks;                        /* bloques de la particion */
    bfs_u64 bmap_start, bmap_blocks;
    bfs_u64 imap_start, imap_blocks;
    bfs_u64 itab_start, itab_blocks, ninodes;
    bfs_u64 jstart, jblocks;
    bfs_u64 data_start;
    bfs_u64 free_blocks, free_inodes;
    bfs_u64 jseq;                           /* ultima operacion aplicada */
    bfs_u64 mtime;
    char label[32];
};

struct bfs_ext { bfs_u64 start; bfs_u32 len, pad; };          /* 16 bytes */

struct bfs_inode {                                              /* 256 bytes */
    bfs_u16 type, flags;
    bfs_u32 parent;                         /* carpeta que lo contiene (para "..") */
    bfs_u64 size;
    bfs_u64 mtime;
    bfs_u64 xblock;                         /* bloque con mas tramos (0 = no hay) */
    bfs_u32 next;                           /* tramos usados */
    bfs_u32 pad;
    struct bfs_ext ext[BFS_NEXT];
    bfs_u8 spare[256 - 40 - 16 * BFS_NEXT];
};

struct bfs_dirent {                         /* en los bloques de una carpeta; rec multiplo de 8 */
    bfs_u32 ino;                            /* 0 = hueco */
    bfs_u16 rec;
    bfs_u8 nlen, type;
    /* nombre[nlen] (sin 0) */
};

struct bfs_jhead {                          /* bloque 0 del diario */
    char magic[8];                          /* "BFS-JRN1" */
    bfs_u64 seq;
    bfs_u32 n, sum;
    bfs_u64 blk[BFS_TXMAX];
};

/* un volumen montado. rd/wr leen y escriben sectores de 512 de la particion (lba relativo a su inicio). */
struct bfs {
    int (*rd)(void *ctx, bfs_u64 lba, bfs_u32 nsec, void *buf);
    int (*wr)(void *ctx, bfs_u64 lba, bfs_u32 nsec, const void *buf);
    void *ctx;
    struct bfs_super sb;
    bfs_u8 *tx;                             /* memoria para la operacion: BFS_TXMAX bloques (la pone quien monta) */
    bfs_u64 txblk[BFS_TXMAX];
    bfs_u32 ntx;
    bfs_u64 hint;                           /* donde empezar a buscar bloques libres */
    bfs_u32 ihint;                          /* y inodos libres */
    bfs_u8 b1[BFS_BS], b2[BFS_BS];          /* trabajo */
    bfs_u8 bb[BFS_BS];                      /* mapas de bits */
    bfs_u8 ib[BFS_BS];                      /* tabla de inodos */
    bfs_u8 db[BFS_BS];                      /* bloques de carpeta */
    bfs_u8 xb[BFS_BS];                      /* bloque de tramos extra */
};

typedef int (*bfs_dir_cb)(void *ctx, const char *name, int nlen, bfs_u32 ino, int type, bfs_u64 size);

int  bfs_mkfs(struct bfs *v, bfs_u64 nsectors, const char *label);
int  bfs_mount(struct bfs *v);              /* lee el superbloque y repite el diario si quedo algo */
int  bfs_lookup(struct bfs *v, const char *path, bfs_u32 *ino, struct bfs_inode *in);
int  bfs_list(struct bfs *v, bfs_u32 dino, bfs_dir_cb cb, void *ctx);
long long bfs_read(struct bfs *v, const struct bfs_inode *in, bfs_u64 off, void *buf, bfs_u64 n);
int  bfs_write(struct bfs *v, const char *path, const void *data, bfs_u64 size);   /* crea o reemplaza */
int  bfs_mkdir(struct bfs *v, const char *path);
int  bfs_remove(struct bfs *v, const char *path);                                   /* archivo o carpeta vacia */
int  bfs_rename(struct bfs *v, const char *from, const char *to);
int  bfs_check(struct bfs *v, char *msg, int max);                                  /* revisa el volumen (fsck) */
const char *bfs_strerror(int e);

#endif
