/* cd.h - reproduccion de CD de audio (ver cd.c) */
#ifndef CD_H
#define CD_H

#define CD_ENODRIVE  -1     /* no hay lectora de CD */
#define CD_ENODISC   -2     /* sin disco o no se pudo leer la tabla de contenido */
#define CD_ENOTRACK  -3
#define CD_EDATA     -4     /* esa pista es de datos */
#define CD_ENOAUDIO  -5     /* no hay tarjeta de sonido */
#define CD_EREAD     -6

int  cd_toc(int i, char *out);          /* linea de la pista i (0 = leer el disco); 0 = no hay mas, <0 = error */
int  cd_play(int track);                /* 0 = bien. track 0 = la primera. Sigue hasta el final del disco */
void cd_stop(void);
int  cd_eject(void);
int  cd_status(int *track, int *secs);  /* 1 = suena */

#endif
