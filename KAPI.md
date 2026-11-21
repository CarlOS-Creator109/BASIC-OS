# Kapi — la ABI de BASIC-OS

**Kapi** es la interfaz entre el kernel y los módulos (apps de ventana, programas de pantalla
completa, drivers, BN, DOOM…). Un módulo **no enlaza con el kernel**: todo lo que necesita lo recibe
por una tabla de punteros, el `struct kapi`. Esa tabla es la única puerta al sistema.

```c
#include "kapi.h"

int mod_main(const struct kapi *K)
{
    K->print("hola\n");
    return MOD_OK;
}
```

El kernel carga el módulo (PCBM plano o ELF, ver `ELF-LOADER.md`), y llama a `mod_main(&kapi)`.
Dentro no hay libc: nada de `printf`, `malloc`, `memcpy`… solo `K`. Si necesitas memoria, pídela con
`K->ram`. Si necesitas copiar bytes, escribe tu propio bucle.

**Versión actual: `KAPI_VER 11`** (campo `K->ver`). Los campos se agregan al final entre versiones;
los viejos no se mueven, así que un módulo viejo sigue cargando. Comprueba `K->ver >= N` antes de usar
algo de una versión nueva.

## Códigos de retorno de `mod_main`

| Valor | Significado |
| --- | --- |
| `MOD_OK` (0) | Terminó; vuelve al escritorio o la consola. |
| `MOD_WINAPP` (2) | Era una app de ventana: la publicaste con `wm_publish()` (ver más abajo). |

---

## Pantalla y gráficos

Coordenadas **lógicas** como en `PLOT`; cada píxel lógico son 1×1, 2×2 o 3×3 píxeles reales según la
pantalla. Colores = índices del C64 (0–15); 255 = transparente en las capas. Texto 8×16 con la fuente
VGA en `FONT_ADDR` (16 bytes por carácter, 256 caracteres).

| Función | Qué hace |
| --- | --- |
| `int scr_w(void)` / `int scr_h(void)` | Ancho/alto lógicos. `0` si no hay modo gráfico. |
| `unsigned char *layer(int n)` | Puntero a una capa de framebuffer. `0`=FB0 terminal, `1`=FB1 dibujo de programas, `2`/`3`=libres encima de todo, `4`=plano del escritorio. Cada capa mide `scr_w()×scr_h()` bytes. |
| `void dirty(int x,int y,int w,int h)` | Marca esa zona como repintar. |
| `void flush(void)` | Pinta lo marcado en pantalla. |
| `void *pool(unsigned *bytes)` | Memoria libre para superficies (deja el tamaño en `*bytes`). |
| `void cursor(int on)` | Enciende/apaga el cursor de texto. |
| `int col(void)` | Columna del cursor de texto en la consola actual. *(v6)* |
| `int overlay(int op,int *r)` | Hueco de video a todo color en el escritorio que el compositor no pisa. `op 0`=poner (`r`=x,y,w,h lógicos → regresa el rect en píxeles reales), `1`=quitar, `2`=ya pinté. Solo uno a la vez; quítalo al perder el foco. *(v6)* |

---

## Entrada

| Función | Qué hace |
| --- | --- |
| `int poll(void)` | Tecla pendiente o `0`. De paso atiende el ratón y el audio. Devuelve ASCII/CP437, o `KEY_UP/DOWN/LEFT/RIGHT`. |
| `void mouse(int *x,int *y,int *btn)` | Posición y botones del ratón (`BTN_LEFT/RIGHT/MIDDLE`). |
| `int esc(void)` | `1` si se pulsó ESC desde la última vez. |
| `int rawin(int op,int *a,int *b)` | Entrada cruda para juegos a pantalla completa (scancodes set 1 y ratón sin pasar por el escritorio). `op 0`=activar, `1`=desactivar, `2`=siguiente scancode (make/break con prefijos E0/E1) o `-1`, `3`=ratón (`*a`=dx, `*b`=dy acumulados; devuelve botones). |

---

## Tiempo

| Función | Qué hace |
| --- | --- |
| `unsigned ms(void)` | Milisegundos desde el arranque. |
| `void sleep(int ms)` | Espera ese tiempo (sigue atendiendo el sistema). |
| `void set_tick(void (*fn)(void))` | Registra una función que se llama ~60 veces/s desde cualquier espera del kernel. |
| `unsigned seed(void)` | Una semilla para azar. |

---

## Audio

| Función | Qué hace |
| --- | --- |
| `void sound(int kind,int hz,int arg,int ms)` | Nota/efecto con las voces del kernel. `kind` = `SND_SINE/SQUARE/TRIANGLE/SAW/KICK/HIHAT/SNARE/DRUMKIT/OFF`. |
| `int pcm_play(const void *adpcm,unsigned samples,unsigned rate)` | Reproduce ADPCM IMA 4 bits mono. `0` si no hay tarjeta. |
| `void pcm_stop(void)` | Corta el PCM. |
| `unsigned pcm_ms(void)` | Ms de PCM que ya sonaron. |
| `int stream(int op,const void *data,unsigned n)` | Audio en vivo estéreo s16 intercalado (11025 Hz por defecto), mezclado por la tarjeta. `op 0`=iniciar (`1` ok / `0` sin tarjeta), `1`=empujar `n` cuadros (devuelve cuántos entraron), `2`=parar, `3`=cuadros en cola, `4`=tasa (`n`=Hz), `5`=ganancia 1..16. |

---

## Archivos, discos y ejecución

Las rutas son tipo `"HD0.1:/CARPETA/ARCHIVO"`.

| Función | Qué hace |
| --- | --- |
| `int dir(const char *spec,kapi_dircb cb)` | Lista una carpeta; llama `cb(name,size,isdir)` por entrada. |
| `int read(const char *spec,char *buf,int max,unsigned *total)` | Lee un archivo a `buf` (hasta `max`); deja el tamaño real en `*total`. |
| `int exec(const char *name)` | Carga un módulo de `/SYS` y devuelve el código de su `mod_main`. |
| `int blk(const char *name,int op,u64 lba,unsigned n,void *buf)` | Disco crudo por nombre (`"HD0"`, `"USB0"`…). `op 0`=leer `n` sectores desde `lba`, `1`=escribir (solo discos de la BIOS), `2`=tamaño en sectores, `3`=banderas (1=arranque, 2=escribible). `0` ok, `-1` no existe. |
| `int fsop(int op,const char *spec,const void *data,u64 size,const char *spec2)` | Escribir en el FS (BFS; v10 también FAT16/32). `op`=`FSOP_WRITE/MKDIR/REMOVE/RENAME/SPACE/KIND/FORMAT`. `0` ok; `<0` error (`-7` solo lectura, `-8` sin espacio, `-9` ya existe, `-2` no existe, `-5` error de disco…). *(v9/v10)* |
| `int saved(const char *name,char *buf,int max)` | Copia el texto de un programa BASIC guardado con `SAVE`. Lo usa PKG para subir al hub. *(v4)* |
| `int drive_line(int i,char *out)` | Línea `i` de la lista de unidades (para menús de archivos). |

---

## El intérprete de BASIC

| Función | Qué hace |
| --- | --- |
| `int load_basic(const char *text,int n)` | Reemplaza el programa BASIC en memoria. |
| `void basic_run(const char *line)` | Ejecuta una línea como si se hubiera escrito. |
| `int basic_busy(void)` | `1` si BASIC está ocupado. |
| `int basic_do(const char *stmt)` | Ejecuta una sentencia sin imprimir `READY` ni tocar el programa en curso. `0` ok, `-1` error. *(v5, para binarios de bas2bin)* |
| `int basic_eval(const char *expr,int *val)` | Evalúa una expresión y deja el resultado en `*val`. *(v5)* |

---

## Consolas de texto (ventanas de terminal)

Cada consola tiene su plano de texto y su plano de `PLOT`.

| Función | Qué hace |
| --- | --- |
| `int con_attach(id,txt,gfx,stride,cols,rows,cb,user)` | Crea/engancha una consola con sus búferes. |
| `void con_detach(int id)` | La quita. |
| `void con_resize(int id,int cols,int rows)` | La redimensiona. |
| `void con_putc(int id,int ch)` | Escribe un carácter. |
| `void con_select(int id)` | A qué consola va la salida de BASIC. |
| `void con_cursor(int id,int on)` | Cursor de esa consola. |

---

## Memoria

| Función | Qué hace |
| --- | --- |
| `u64 ram(int op,u64 a,unsigned n,const char *who)` | Bancos de RAM de 32 MB ("salto de RAM"). `RAM_ALLOC` (`n` bancos seguidos → dirección o `0`), `RAM_FREE` (`a`=dirección), `RAM_FREEALL` (`who`=dueño), `RAM_INFO` (`n`: 0 total, 1 libres, 2 tamaño, 3 fin de RAM). *(v7)* |
| `void *map_mmio(u64 phys,unsigned size)` | Mapea un BAR de PCI (sin cache) en 0..4 GB. `NULL` si no se puede. **Esta es la que usan los drivers de hardware.** |
| `void *pool(unsigned *bytes)` | Ver *Pantalla*: memoria libre para superficies. |

---

## Tareas cooperativas *(v8)*

| Función | Qué hace |
| --- | --- |
| `long task(int op,void (*fn)(void*),void *arg,const char *name)` | `TASK_NEW` (`fn,arg,name` → número), `TASK_YIELD`, `TASK_SLEEP` (`arg`=ms), `TASK_EXIT`, `TASK_KILL` (`arg`=número), `TASK_KILLRANGE` (`fn`=desde, `arg`=hasta), `TASK_SELF`, `TASK_COUNT`. |

---

## Video de texto a pantalla completa

| Función | Qué hace |
| --- | --- |
| `int vid(int op,const unsigned char *chars,int w,int h)` | `op 0`=iniciar (`cols\|filas<<16`), `1`=cuadro, `2`=fin. |

---

## Archivos por descriptor *(v11)*

El modelo de E/S de Unix: cada descriptor lleva **su propia posición**, así que se lee y escribe por
tramos y te puedes mover con `f_seek`. Es la base sobre la que después se pueden construir pipes y
redirección. Para cargar un archivo entero de un golpe sigue estando `read`.

| Función | Qué hace |
| --- | --- |
| `int f_open(const char *spec,int mode)` | Abre y devuelve el descriptor, o un `FDERR_*`. `mode` = `O_RD`/`O_WR`/`O_RDWR`, más `O_NEW` (crear o vaciar) y `O_APP` (escribir al final). |
| `int f_read(int fd,void *buf,int n)` | Lee hasta `n` bytes desde la posición actual. `0` = fin de archivo. |
| `int f_write(int fd,const void *buf,int n)` | Escribe `n` bytes en la posición actual. |
| `int f_seek(int fd,int off,int whence)` | Mueve la posición. `whence` = `SEEK_SET`/`SEEK_CUR`/`SEEK_END`. Devuelve la posición nueva. |
| `int f_close(int fd)` | Cierra y vuelca al disco si hubo cambios. |

Errores: `FDERR_BADF` (-1), `FDERR_NOENT` (-2), `FDERR_MANY` (-3), `FDERR_NOMEM` (-4),
`FDERR_BIG` (-5, por ahora 4 MB por descriptor), `FDERR_IO` (-6).

> Por dentro, hoy, al abrir se trae el archivo entero a un banco de RAM y al cerrar se vuelca,
> porque `fs.c` todavía no sabe leer desde un offset. La **interfaz** ya es la definitiva: el día
> que el sistema de archivos lea por tramos se cambia `fd.c` por dentro y ningún programa se entera.

---

## La mini-libc (`sys/libc.h`)

Una cabecera que te deja escribir programas como en Unix en vez de llamar a `K->algo` a mano:

```c
#include "sys/ui.h"      // declara K
#include "sys/libc.h"
...
lc_init();                                     // una vez, antes de usar malloc
char *p = c_malloc(128);
int fd = c_open("HD0.1:/NOTAS.TXT", O_RDWR | O_NEW);
c_write(fd, "hola\n", 5);
c_close(fd);
c_printf("listo: %s (%d)\n", p, c_strlen(p));
c_free(p); lc_done();
```

- **Cadenas/memoria:** `c_strlen`, `c_strcmp`, `c_strncmp`, `c_strcpy`, `c_strlcpy`, `c_strcat`, `c_strchr`, `c_memcpy`, `c_memset`, `c_memcmp`, `c_atoi`, `c_itoa`
- **Memoria dinámica:** `lc_init` / `lc_done`, `c_malloc`, `c_calloc`, `c_free` (primer hueco que sirva, con fusión al liberar, sobre un banco del asignador)
- **Archivos:** `c_open`, `c_read`, `c_write`, `c_seek`, `c_close`, `c_fsize`
- **Salida:** `c_puts`, `c_putchar`, `c_printf` (`%d %u %x %s %c %%`)

Los nombres llevan `c_`/`lc_` para no chocar con el `memcpy`/`memset` del kernel. Ejemplo completo
en `apps/libcdemo.c`.

---

## Aplicaciones de ventana

Un `.APP` que quiere una ventana define un `struct wapp`, lo entrega con `wm_publish()` y devuelve
`MOD_WINAPP`; el escritorio crea la ventana y llama a los callbacks de la app.

```c
struct wapp {
    const char *title;
    int w, h;                 /* tamaño inicial de la zona interior */
    int minw, minh;           /* mínimo al redimensionar */
    int resizable;
    void (*start)(struct wctx *c);
    void (*frame)(struct wctx *c);   /* ~60 veces/s: animar y dibujar */
    void (*key)(struct wctx *c, int k);
    void (*mouse)(struct wctx *c, int x, int y, int btn, int prev);
    void (*resize)(struct wctx *c);  /* ya con c->w, c->h, c->pix nuevos */
    void (*stop)(struct wctx *c);
};
```

El `struct wctx` que recibe cada callback trae la superficie de la ventana (`pix`, `stride`, `w`, `h`),
su posición (`sx`, `sy`), si tiene el foco (`focus`), un `priv` libre para la app, y servicios del
escritorio: `damage(c,x,y,w,h)` (repintar), `close(c)`, `title(c,t)`, `launch(app)`,
`run_basic(line)`.

| Función | Qué hace |
| --- | --- |
| `void wm_publish(const struct wapp *app)` | Registra la app de ventana. |
| `const struct wapp *wm_take(void)` | La recoge (uso interno del escritorio). |

---

## Varios

| Campo | Qué es |
| --- | --- |
| `void print(const char *s)` | Escribe en la consola (fuera del escritorio). La más usada. |
| `const char *args` | Lo que el usuario escribió tras el comando (`NET "texto"` → `"texto"`; `""` si nada). |
| `unsigned ver` | Versión de la Kapi (ver arriba). |

---

## Constantes útiles

- **Sonido:** `SND_SINE`, `SND_SQUARE`, `SND_TRIANGLE`, `SND_SAW`, `SND_KICK`, `SND_HIHAT`, `SND_SNARE`, `SND_DRUMKIT`, `SND_OFF`
- **RAM:** `RAM_ALLOC`, `RAM_FREE`, `RAM_FREEALL`, `RAM_INFO`
- **Tareas:** `TASK_NEW`, `TASK_YIELD`, `TASK_SLEEP`, `TASK_EXIT`, `TASK_KILL`, `TASK_KILLRANGE`, `TASK_SELF`, `TASK_COUNT`
- **FS:** `FSOP_WRITE`, `FSOP_MKDIR`, `FSOP_REMOVE`, `FSOP_RENAME`, `FSOP_SPACE`, `FSOP_KIND`, `FSOP_FORMAT`
- **Ratón:** `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`
- **Teclas:** `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`
- **Otros:** `FONT_ADDR` (fuente 8×16), `KAPI_VER`

---

## Escribir un módulo

1. Incluye `kapi.h`, escribe `int mod_main(const struct kapi *K)`.
2. Habla con el sistema solo por `K`. Sin libc.
3. Compílalo como ELF con `gcc` (ver `ELF-LOADER.md`) o como PCBM plano (ver `mod.ld`).
4. Un driver de hardware típico: `K->map_mmio()` para el BAR de PCI, lee/escribe registros, y expón
   lo tuyo por el `kapi` o por un archivo. (La red BN es el ejemplo vivo: driver e1000 con
   `map_mmio` + polling.)

Plantilla lista en `DRIVER-EJEMPLO.c`.
