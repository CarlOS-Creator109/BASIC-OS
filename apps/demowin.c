/* demowin.c - DEMOWIN.BIN: ejemplo de un programa de la tienda con ventana, hecho con WTK (sys/wtk.h).
 * Se compila con  make store  y se sube al hub como cualquier .BIN. PKG lo baja y, como devuelve MOD_WINAPP, el escritorio
 * abre su ventana (si estabas en la consola, PKG abre el escritorio solo).
 *
 * Muestra todos los controles: menu, etiqueta, caja de texto, botones, casilla, lista, barra de progreso, marco y un lienzo
 * donde se dibuja con el raton. Las anclas hacen que todo se acomode al cambiar el tamano de la ventana.
 */
#include "sys/wtk.h"
#include "mod_head.h"

#define CW 96
#define CH 64
static unsigned char pic[CW * CH];             /* el dibujo: un color por punto (escala x2 en pantalla) */
static char nombre[32];
static int sonido = 1, color_sel = 0, avance, pausa;
static int id_lienzo, id_estado;

static const char *const colores[] = { "Azul", "Rojo", "Verde", "Amarillo", "Naranja", "Morado", "Negro", "Blanco" };
static const int color_c64[] = { C_BLUE, C_RED, C_GREEN, C_YELLOW, C_ORANGE, C_PURPLE, C_BLACK, C_WHITE };
static const char *const m_archivo[] = { "Acerca de", "Limpiar dibujo", "Cerrar" };
static const char *const m_ver[] = { "Pausar la barra", "Seguir la barra" };

static void limpiar(int id)
{
    (void)id;
    memset(pic, C_WHITE, sizeof pic);
    wt_text(id_estado, "Dibujo limpio");
}

static void saludar(int id)
{
    char t[80];
    int k = 0, i;
    const char *a = "Hola, ";
    (void)id;
    for (i = 0; a[i]; i++) t[k++] = a[i];
    for (i = 0; nombre[i] && k < 70; i++) t[k++] = nombre[i];
    if (!nombre[0]) { a = "persona sin nombre"; for (i = 0; a[i]; i++) t[k++] = a[i]; }
    t[k++] = '!';
    t[k] = 0;
    if (sonido)
        K->sound(SND_SINE, 880, 0, 120);
    wt_msg(t);
}

static void elegir_color(int id)
{
    (void)id;
    wt_text(id_estado, wt_ev == WT_EV_ACTIVATE ? "Color elegido (doble clic)" : "Pinta en el lienzo con el raton");
}

static void dibujar(struct surf *s, int w, int h)
{
    int x, y, esc = 2;
    s_rect(s, 0, 0, w, h, C_DGREY);
    for (y = 0; y < CH; y++)
        for (x = 0; x < CW; x++)
            s_rect(s, 1 + x * esc, 1 + y * esc, esc, esc, pic[y * CW + x]);
}

static void pintar(int id, int x, int y, int btn)
{
    int px = (x - 1) / 2, py = (y - 1) / 2, dx, dy;
    (void)id;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int qx = px + dx, qy = py + dy;
            if (qx >= 0 && qy >= 0 && qx < CW && qy < CH)
                pic[qy * CW + qx] = (unsigned char)((btn & 2) ? C_WHITE : color_c64[color_sel]);   /* derecho = borrar */
        }
}

static void menu_archivo(int item)
{
    if (item == 0)
        wt_msg("DEMOWIN: ejemplo de WTK, el kit de ventanas de BASIC-OS. Todo esto son unas 150 lineas de C.");
    else if (item == 1)
        limpiar(0);
    else
        wt_close();
}

static void menu_ver(int item) { pausa = item == 0; wt_text(id_estado, pausa ? "Barra en pausa" : "Barra corriendo"); }

static void tick(void)
{
    static unsigned last;
    unsigned now = K->ms();
    if (!pausa && now - last > 50) {
        last = now;
        avance = (avance + 1) % 101;
        wt_redraw();
    }
}

static void armar(void)
{
    wt_menu("Archivo", m_archivo, 3, menu_archivo);
    wt_menu("Ver", m_ver, 2, menu_ver);
    wt_label(8, 10, "Tu nombre:");
    wt_anchor(wt_input(96, 8, 108, nombre, sizeof nombre, saludar), WT_FILLW);
    wt_anchor(wt_button(8, 8, 100, 22, "Saludar", saludar), WT_RIGHT);
    wt_check(8, 38, "Sonido al saludar", &sonido, 0);
    wt_box(8, 60, 120, 172, "Color");
    wt_list(14, 78, 108, 148, colores, 8, &color_sel, elegir_color);
    id_lienzo = wt_canvas(136, 66, CW * 2 + 2, CH * 2 + 2, dibujar, pintar);
    wt_button(136, 202, 90, 22, "Limpiar", limpiar);
    wt_anchor(wt_progress(8, 30, 8, 16, &avance, 100), WT_BOTTOM | WT_FILLW);
    id_estado = wt_label(8, 6, "Pinta en el lienzo con el raton");
    wt_anchor(id_estado, WT_BOTTOM);
    wt_on_tick(tick);
}

int mod_main(const struct kapi *k)
{
    K = k;
    memset(pic, C_WHITE, sizeof pic);
    nombre[0] = 0;
    return wt_window("DEMOWIN (WTK)", 360, 300, 1, armar);
}
