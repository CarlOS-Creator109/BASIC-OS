/* basic.h - interprete de BASIC y la capa de plataforma que necesita */
#ifndef BASIC_H
#define BASIC_H

/* Interprete (basic.c) */
void basic_init(void);
int  basic_exec_line(char *line);   /* devuelve 1 si ejecuto un comando directo */
void basic_banner(void);
int  basic_bytes_free(void);

/* Plataforma: la implementa el kernel (kernel.c) o el arnes de pruebas */
void         plat_putc(char c);
int          plat_col(void);
void         plat_clear(void);
void         plat_color(int fg, int bg);
int          plat_readline(char *buf, int max);
int          plat_break(void);        /* 1 si el usuario pulso ESC */
void         plat_reboot(void);
int          plat_peek(unsigned int addr);
void         plat_poke(unsigned int addr, int val);
unsigned int plat_seed(void);
int          plat_pci_line(int i, char *out);   /* texto del dispositivo PCI i, 0 = no hay mas */
int          plat_drive_line(int i, char *out);  /* linea de DRIVES, 0 = no hay mas */
const char  *plat_audio_info(void);             /* estado del audio (texto) */
int          plat_mouse(int what);              /* 0 = x, 1 = y (coordenadas de PLOT), 2 = botones */
int          plat_inkey(void);        /* tecla pendiente (0 = ninguna), sin esperar */
void         plat_beep(int ms, int hz);
void         plat_sleep(int ms);
int          plat_sound(int kind, int hz, int arg, int ms);   /* 1 = sono; 0 = no hay tarjeta */
void         plat_sndwait(void);                              /* espera a que acaben las voces (ESC corta) */
int          plat_cd_toc(int i, char *out);                   /* linea de pista i; 0 = fin; <0 = error */
int          plat_cd_play(int track);                         /* 0 = bien, <0 = error */
void         plat_cd_stop(void);
int          plat_cd_eject(void);
int          plat_cd_status(int *track, int *secs);           /* 1 = suena */
int          plat_exec(const char *name);                     /* ejecuta un modulo de /SYS; <0 = error */
void         plat_locate(int col, int row);
void         basic_load_text(const char *text, int n);        /* reemplaza el programa en memoria */
/* Graficos: coordenadas logicas, origen arriba a la izquierda. c = 0-15 (C64), -1 = color de texto */
int          plat_gfx_w(void);        /* 0 si no hay modo grafico */
int          plat_gfx_h(void);
void         plat_plot(int x, int y, int c);
void         plat_hline(int x1, int x2, int y, int c);

#endif
