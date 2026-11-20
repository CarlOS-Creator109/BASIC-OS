/* ext4.h - lectura de ext2/3/4 (ext4.c). Solo lectura. */
#ifndef EXT4_H
#define EXT4_H
#define EXT_EIO     -1
#define EXT_EFMT    -2
#define EXT_ENOENT  -3
#define EXT_ENOTDIR -4
typedef void (*ext_dir_cb)(const char *name, u32 ino, int dir);
int ext_mount(int (*rd)(u64 lba, u32 n, void *buf));      /* rd: sectores de 512 de la particion */
int ext_lookup(const char *path, u32 *ino, int *dir, u64 *size);
int ext_list(u32 ino, ext_dir_cb cb);
int ext_stat(u32 ino, u64 *size, int *dir);
long long ext_read(u32 ino, void *buf, u64 max, u64 *total);
#endif
