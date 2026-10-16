/* audio.h - motor de audio: voces sinteticas + CD de audio sobre un anillo DMA */
#ifndef AUDIO_H
#define AUDIO_H
#include "kernel.h"

#define SND_SINE     0
#define SND_SQUARE   1
#define SND_TRIANGLE 2
#define SND_SAW      3
#define SND_KICK     4
#define SND_HIHAT    5
#define SND_SNARE    6
#define SND_DRUMKIT  7
#define SND_OFF      8

/* drivers de tarjeta (hda.c, ac97.c) */
int  hda_open(void);
void hda_start(void *ring, u32 bytes);
u32  hda_pos(void);
void hda_stop(void);
const char *hda_info(void);
int  ac97_open(void);
void ac97_start(void *ring, u32 bytes);
u32  ac97_pos(void);
void ac97_stop(void);
const char *ac97_info(void);

int  audio_open(void);                  /* 1 = hay tarjeta (HDA o AC'97) */
const char *audio_info(void);
/* Lanza una voz y vuelve de inmediato. Para OFF corta todo. 0 = no hay tarjeta. */
int  audio_sound(int kind, int hz, int arg, int ms);
void audio_service(void);               /* llenar el anillo: llamar seguido */
int  audio_busy(void);                  /* 1 = suena algo */
void audio_stop(void);

/* CD de audio: lee n sectores crudos de 2352 bytes desde lba. 0 = bien */
typedef int (*cd_read_fn)(unsigned int lba, int n, void *buf);
int  audio_cd_play(cd_read_fn fn, unsigned int lba, unsigned int end_lba);
void audio_cd_stop(void);
int  audio_cd_active(void);
unsigned int audio_cd_lba(void);
/* PCM: ADPCM IMA 4 bits mono; samples = numero de muestras. 0 = no hay tarjeta */
int  audio_pcm_play(const void *adpcm, u32 samples, u32 rate);
void audio_pcm_stop(void);
int  audio_pcm_active(void);
unsigned int audio_pcm_ms(void);
/* Flujo PCM crudo (DOOM): estereo s16 intercalado a 11025 Hz que empuja la aplicacion.
 *   op 0 = iniciar (1 = bien, 0 = no hay tarjeta), op 1 = empujar n cuadros (devuelve cuantos entraron),
 *   op 2 = parar, op 3 = cuadros en cola (aun sin sonar) */
int  audio_stream_ctl(int op, const void *data, u32 n);
extern u64 g_tsc_hz;                    /* sysinfo.c */

#endif
