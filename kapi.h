/* kapi.h - interfaz entre el kernel y los modulos de /SYS (escritorio y apps)
 *
 * Los modulos son binarios planos enlazados a una direccion fija (ver mod.ld). El kernel los
 * carga desde "SYS:" y llama a mod_main(&api). No pueden enlazar con el kernel: todo lo que
 * necesitan lo reciben por esta tabla de punteros.
 *
 * Pantalla: coordenadas logicas (como PLOT); cada pixel logico es de 1x1, 2x2 o 3x3 pixeles
 * reales segun la pantalla. Los colores son indices del C64 (0-15) y 255 = transparente en las
 * capas. El texto es de 8x16 con la fuente VGA que esta en FONT_ADDR.
 *
 * Capas de framebuffer (layer()):  0 = FB0 terminal por defecto   1 = FB1 dibujo de programas
 *   2 = FB2 y 3 = FB3 libres para los programas, encima de todo   4 = plano del escritorio
 * Cada capa mide scr_w() x scr_h() bytes. Despues de dibujar se avisa con dirty() y se pinta con flush().
 *
 * Aplicaciones de ventana: un .APP con struct wapp. mod_main() la entrega con wm_publish() y
 * devuelve MOD_WINAPP; el escritorio crea la ventana y llama a sus funciones.
 */
#ifndef KAPI_H
#define KAPI_H

#define KAPI_VER 10
#define FONT_ADDR 0x6800            /* fuente 8x16: 16 bytes por caracter, 256 caracteres */

/* tipos de SOUND (los usa tambien el kernel) */
#define SND_SINE     0
#define SND_SQUARE   1
#define SND_TRIANGLE 2
#define SND_SAW      3
#define SND_KICK     4
#define SND_HIHAT    5
#define SND_SNARE    6
#define SND_DRUMKIT  7
#define SND_OFF      8

#define RAM_ALLOC   0
#define RAM_FREE    1
#define RAM_FREEALL 2
#define RAM_INFO    3

#define TASK_NEW       0
#define TASK_YIELD     1
#define TASK_SLEEP     2
#define TASK_EXIT      3
#define TASK_KILL      4
#define TASK_KILLRANGE 5
#define TASK_SELF      6
#define TASK_COUNT     7

/* v9: K->fsop: escribir en discos BFS (el sistema de archivos de BASIC-OS). spec = "HD0.1:/CARPETA/ARCHIVO".
 * v10: tambien FAT16 y FAT32 (con nombres largos); EXT4, CD y FAT12 son de solo lectura.
 * 0 = bien; < 0 = error (-7 solo lectura, -8 sin espacio, -9 ya existe, -10 carpeta no vacia,
 * -11 nombre no valido, -2 no existe, -5 error del disco) */
#define FSOP_WRITE     0    /* data[size] -> crea o reemplaza el archivo */
#define FSOP_MKDIR     1
#define FSOP_REMOVE    2    /* archivo o carpeta vacia */
#define FSOP_RENAME    3    /* spec2 = ruta nueva (mismo disco) */
#define FSOP_SPACE     4    /* data = unsigned long long[2]: bytes libres y totales */
#define FSOP_KIND      5    /* data = char[8]: "BFS", "FAT16"... del disco de spec */
#define FSOP_FORMAT    6    /* spec = "HD1.1:", data = etiqueta; segun el tipo en el MBR: 0xB0 BFS, 0x0B/0x0C FAT32,
                               0x04/0x06/0x0E FAT16 (max 2 GB) */

#define BTN_LEFT   1
#define BTN_RIGHT  2
#define BTN_MIDDLE 4

/* teclas que devuelve poll() ademas de ASCII/CP437 */
#define KEY_UP    145
#define KEY_DOWN  17
#define KEY_LEFT  157
#define KEY_RIGHT 29

typedef void (*kapi_dircb)(const char *name, unsigned int size, int isdir);

/* ---- aplicaciones de ventana ---- */
struct wctx {
    unsigned char *pix;             /* superficie de la zona interior: un indice de color por pixel */
    int stride;                     /* bytes por fila de la superficie */
    int w, h;                       /* tamano de la zona interior (cambia al redimensionar) */
    int sx, sy;                     /* posicion de la zona interior en la pantalla */
    int focus;                      /* 1 = es la ventana activa */
    void *priv;                     /* libre para la aplicacion */
    void *wm;                       /* del escritorio: no tocar */
    /* servicios del escritorio */
    void (*damage)(struct wctx *c, int x, int y, int w, int h);   /* repintar esa zona (coord. de la ventana) */
    void (*close)(struct wctx *c);
    void (*title)(struct wctx *c, const char *t);
    void (*launch)(const char *app);                              /* "TERMINAL" o un .APP */
    void (*run_basic)(const char *line);                          /* ejecuta en una terminal (la abre si hace falta) */
};

struct wapp {
    const char *title;
    int w, h;                       /* tamano inicial de la zona interior */
    int minw, minh;                 /* minimo al redimensionar */
    int resizable;
    void (*start)(struct wctx *c);
    void (*frame)(struct wctx *c);  /* unas 60 veces por segundo: animar y dibujar */
    void (*key)(struct wctx *c, int k);
    void (*mouse)(struct wctx *c, int x, int y, int btn, int prev);   /* coord. de la zona interior */
    void (*resize)(struct wctx *c); /* ya con c->w, c->h, c->pix nuevos */
    void (*stop)(struct wctx *c);
};

typedef void (*kapi_dirty_cb)(void *user, int x, int y, int w, int h);

struct kapi {
    unsigned ver;
    int  (*scr_w)(void);                              /* 0 si no hay modo grafico */
    int  (*scr_h)(void);
    int  (*poll)(void);                               /* tecla pendiente o 0; atiende el raton y el audio */
    void (*mouse)(int *x, int *y, int *btn);
    unsigned (*ms)(void);                             /* milisegundos desde el arranque */
    void (*sleep)(int ms);
    void (*sound)(int kind, int hz, int arg, int ms);
    int  (*drive_line)(int i, char *out);
    int  (*dir)(const char *spec, kapi_dircb cb);
    int  (*read)(const char *spec, char *buf, int max, unsigned *total);
    int  (*exec)(const char *name);                   /* carga un modulo de /SYS y devuelve el codigo de mod_main */
    int  (*load_basic)(const char *text, int n);      /* reemplaza el programa BASIC en memoria */
    int  (*esc)(void);                                /* 1 si se pulso ESC desde la ultima vez */
    unsigned (*seed)(void);
    /* capas */
    unsigned char *(*layer)(int n);
    void (*dirty)(int x, int y, int w, int h);
    void (*flush)(void);
    void *(*pool)(unsigned *bytes);                   /* memoria libre para superficies */
    void (*cursor)(int on);
    /* consolas de texto con su plano de PLOT (ventanas de terminal) */
    int  (*con_attach)(int id, unsigned char *txt, unsigned char *gfx, int stride, int cols, int rows,
                       kapi_dirty_cb cb, void *user);
    void (*con_detach)(int id);
    void (*con_resize)(int id, int cols, int rows);
    void (*con_putc)(int id, int ch);
    void (*con_select)(int id);                       /* a que consola va la salida de BASIC */
    void (*con_cursor)(int id, int on);
    void (*basic_run)(const char *line);              /* ejecuta una linea como si se hubiera escrito */
    int  (*basic_busy)(void);
    void (*set_tick)(void (*fn)(void));               /* se llama ~60 veces/s desde cualquier espera del kernel */
    void (*wm_publish)(const struct wapp *app);
    const struct wapp *(*wm_take)(void);
    /* video de texto y audio PCM (modulos de pantalla completa, fuera del escritorio) */
    int  (*vid)(int op, const unsigned char *chars, int w, int h);   /* 0 = iniciar (cols|filas<<16), 1 = cuadro, 2 = fin */
    int  (*pcm_play)(const void *adpcm, unsigned samples, unsigned rate);   /* ADPCM IMA 4 bits mono; 0 = sin tarjeta */
    void (*pcm_stop)(void);
    unsigned (*pcm_ms)(void);                          /* ms de PCM que ya sonaron */
    void (*print)(const char *s);                      /* escribe en la consola (fuera del escritorio) */
    void *(*map_mmio)(unsigned long long phys, unsigned size);   /* mapea un BAR PCI (sin cache) en 0..4 GB; NULL si no se puede */
    const char *args;                                  /* NET "texto": la cadena que escribio el usuario ("" si no hay) */
    /* entrada cruda (juegos de pantalla completa, p.ej. DOOM): scancodes set 1 y raton sin pasar por el escritorio
     *   op 0 = activar (el kernel deja de interpretar teclado y raton), op 1 = desactivar,
     *   op 2 = siguiente scancode (make/break, con prefijos E0/E1) o -1 si no hay,
     *   op 3 = raton: *a = dx, *b = dy acumulados desde la ultima vez (dy hacia abajo), devuelve botones (1 izq, 2 der, 4 centro) */
    int  (*rawin)(int op, int *a, int *b);
    /* flujo de audio en vivo: estereo s16 intercalado a 11025 Hz, mezclado con las voces por la tarjeta HDA/AC'97
     *   op 0 = iniciar (1 = bien, 0 = no hay tarjeta), op 1 = empujar n cuadros desde data (devuelve cuantos entraron),
     *   op 2 = parar, op 3 = cuadros en cola sin sonar todavia, op 4 = tasa (n = Hz), op 5 = ganancia 1..16 (n; 6 por defecto) */
    int  (*stream)(int op, const void *data, unsigned n);
    /* discos por nombre ("HD0", "USB0"...):  op 0 = leer n sectores desde lba, 1 = escribir (solo discos de la BIOS),
     *   op 2 = tamano en sectores, op 3 = banderas (1 = unidad de arranque, 2 = se puede escribir). 0 = bien; -1 = no existe */
    int  (*blk)(const char *name, int op, unsigned long long lba, unsigned n, void *buf);
    /* (ver 4) programa BASIC guardado con SAVE (el almacen del disco de arranque): copia su texto a buf.
     *   Devuelve los bytes (<= max) o < 0 si no existe. Lo usa PKG para subir programas al hub. */
    int  (*saved)(const char *name, char *buf, int max);
    /* (ver 5) para los programas compilados con bas2bin (tools/bas2bin): ejecutar una sentencia o evaluar una expresion
     * del interprete sin imprimir READY y sin tocar el programa que este corriendo. 0 = bien, -1 = error (ya impreso). */
    int  (*basic_do)(const char *stmt);
    int  (*basic_eval)(const char *expr, int *val);
    int  (*col)(void);                                 /* columna del cursor de texto en la consola actual */
    /* (ver 6) hueco de video en el escritorio: la app pinta ahi directo en el framebuffer (a todo color) y el compositor
     * no lo pisa. op 0 = poner (r = x,y,w,h en coordenadas de pantalla logicas; vuelve con el rectangulo en pixeles reales),
     * op 1 = quitar, op 2 = ya pinte (redibuja el cursor encima). Solo una a la vez; quitalo al perder el foco. */
    int  (*overlay)(int op, int *r);
    /* v7: bancos de RAM de 32 MB ("salto de RAM", ver ram.c). op = RAM_ALLOC (n bancos seguidos -> direccion o 0),
     * RAM_FREE (a = direccion), RAM_FREEALL (who = dueno), RAM_INFO (n: 0 total, 1 libres, 2 tamano, 3 fin de RAM) */
    unsigned long long (*ram)(int op, unsigned long long a, unsigned n, const char *who);
    /* v8: tareas cooperativas (ver task.c). TASK_NEW fn, arg, nombre -> numero; TASK_YIELD; TASK_SLEEP arg = ms;
     * TASK_EXIT; TASK_KILL arg = numero; TASK_KILLRANGE fn = desde, arg = hasta; TASK_SELF; TASK_COUNT */
    long (*task)(int op, void (*fn)(void *), void *arg, const char *name);
    /* v9: escribir en BFS; v10: tambien FAT16/32 (ver FSOP_*) */
    int (*fsop)(int op, const char *spec, const void *data, unsigned long long size, const char *spec2);
};

/* codigos de retorno de mod_main */
#define MOD_OK       0      /* volver al escritorio / consola */
#define MOD_TERMINAL 1      /* reservado */
#define MOD_WINAPP   2      /* era una aplicacion de ventana: ver wm_publish() */

#endif
