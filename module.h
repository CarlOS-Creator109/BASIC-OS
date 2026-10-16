/* module.h - cargador de modulos de /SYS (ver kapi.h) */
#ifndef MODULE_H
#define MODULE_H

/* Cada modulo se enlaza a su propia direccion (la lleva en la cabecera): GUI.SYS en 0x300000 y las
 * aplicaciones de ventana en 0x400000, 0x480000, 0x500000... para poder estar abiertas a la vez. */
#define MOD_MAX      0x70000u       /* tamano maximo de un modulo con su bss */

#define MODERR_NOFILE  -1
#define MODERR_BADHDR  -2
#define MODERR_TOOBIG  -3
#define MODERR_BUSY    -4      /* el escritorio ya esta abierto */
#define MODERR_NOMEM   -5      /* no hay memoria para las capas */

/* Carga "SYS:name" en base y lo ejecuta. Devuelve el codigo de mod_main o un MODERR_*. */
int mod_exec(const char *name, unsigned int base);

#endif
