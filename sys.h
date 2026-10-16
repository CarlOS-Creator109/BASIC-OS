/* sys.h - carpeta /SYS de la imagen de arranque
 *
 * Zona de solo lectura dentro de la propia imagen, entre el kernel y la zona de SAVE:
 *   LBA 168..171  directorio: firma "PCBSYS1" + n (u32) + entradas de 32 bytes
 *                 {nombre[20], lba (u32, absoluto en la imagen), tamano (u32), reservado}
 *   LBA 172..511  datos de los archivos (alineados a sector)
 * La arma tools/mksys.py al construir la imagen. Aqui van el escritorio, las apps,
 * los iconos .icb y los programas BASIC de ejemplo: nada de eso esta dentro del kernel.
 * Se accede como "SYS:ARCHIVO" (DIR, TYPE, LOAD...).
 */
#ifndef SYS_H
#define SYS_H

#define SYS_DIR_LBA   168
#define SYS_DIR_SECT  4
#define SYS_DATA_LBA  172
#define SYS_END_LBA   512

typedef void (*sys_cb)(const char *name, unsigned int size);

int sys_list(sys_cb cb);                              /* 0 = bien, <0 = error de disco */
/* Lee SYS:name en buf. Devuelve bytes o <0 (FS_*). *total = tamano real */
int sys_read(const char *name, char *buf, int max, unsigned int *total);

#endif
