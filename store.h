/* store.h - almacen de programas SAVE/LOAD sobre sectores crudos de la imagen de arranque
 *
 * Zona reservada dentro de la propia imagen de 1.44 MB (LBA 512..2759), asi que nunca
 * toca otros discos ni particiones:
 *   LBA 512-513  directorio: firma "PCBFS01" + 28 entradas {nombre[16], longitud, reservado}
 *   LBA 520 + i*80  datos del programa i (hasta 80 sectores = 40960 bytes)
 */
#ifndef STORE_H
#define STORE_H

#define STORE_SLOTS      28
#define STORE_SLOT_BYTES 40960
#define STORE_NAME_MAX   15

#define STORE_OK        0
#define STORE_ENODEV   -1       /* sin disco / error de E/S */
#define STORE_ENOENT   -2       /* no existe */
#define STORE_EFULL    -3       /* directorio lleno */
#define STORE_E2BIG    -4       /* no cabe en el buffer / en la ranura */

/* Capa de bloques: la implementa la plataforma. 0 = bien. */
int plat_disk_read(unsigned int lba, void *buf512);
int plat_disk_write(unsigned int lba, const void *buf512);

int store_save(const char *name, const char *data, int len);
int store_load(const char *name, char *buf, int max);       /* devuelve bytes o error < 0 */
int store_delete(const char *name);
/* i = 0..STORE_SLOTS-1: 1 si la ranura esta en uso (rellena name y len) */
int store_entry(int i, char *name, int *len);
/* Comprueba que hay disco y deja listo el directorio */
int store_open(void);

#endif
