/* mouse.c - raton PS/2 por sondeo (como moused en la consola de los BSD)
 *
 * El controlador 8042 recibe los bytes del raton por el mismo puerto 0x60 que el teclado
 * (bit 5 del estado = dato del puerto auxiliar). kbd_poll() nos pasa esos bytes. Cada
 * paquete son 3 bytes: botones/signos, dx, dy. La logica del cursor y de la seleccion
 * esta en vga_mouse() (vga.c).
 */
#include "kernel.h"

static void wait_in(void)
{
    int i;
    for (i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
}

static int wait_out(void)
{
    int i;
    for (i = 0; i < 100000; i++)
        if (inb(0x64) & 1)
            return 1;
    return 0;
}

static void aux_write(u8 b)
{
    wait_in();
    outb(0x64, 0xD4);                   /* el siguiente byte va al raton */
    wait_in();
    outb(0x60, b);
    if (wait_out())                     /* ACK (0xFA): se descarta en mouse_byte() */
        (void)inb(0x60);
}

void mouse_init(void)
{
    u8 cfg;

    wait_in();
    outb(0x64, 0xA8);                   /* habilitar el puerto auxiliar */
    wait_in();
    outb(0x64, 0x20);                   /* leer configuracion */
    if (wait_out()) {
        cfg = inb(0x60);
        cfg &= (u8)~0x20;               /* reloj del raton activo */
        wait_in();
        outb(0x64, 0x60);
        wait_in();
        outb(0x60, cfg);
    }
    aux_write(0xF6);                    /* valores por defecto */
    aux_write(0xF4);                    /* empezar a enviar datos */
}

static u8 pkt[3];
static int npkt;
static int raw_mode, raw_dx, raw_dy, raw_btn;     /* modo crudo (DOOM): acumula en vez de mover el cursor */

void mouse_raw_mode(int on)
{
    raw_mode = on;
    raw_dx = raw_dy = raw_btn = 0;
    npkt = 0;
    if (!on)
        vga_mouse(0, 0, 0);             /* sincroniza los botones (se solto el raton mientras era crudo) */
}

int mouse_raw_get(int *dx, int *dy)
{
    if (dx) *dx = raw_dx;
    if (dy) *dy = raw_dy;
    raw_dx = raw_dy = 0;
    return raw_btn;
}

void mouse_byte(u8 b)
{
    if (npkt == 0 && (b == 0xFA || !(b & 8)))
        return;                         /* ACK o byte fuera de sincronia */
    pkt[npkt++] = b;
    if (npkt == 3) {
        int dx = pkt[1] - (((int)pkt[0] << 4) & 0x100);
        int dy = pkt[2] - (((int)pkt[0] << 3) & 0x100);
        npkt = 0;
        if (pkt[0] & 0xC0)              /* desbordamiento: descartar */
            return;
        if (raw_mode) {
            raw_dx += dx;
            raw_dy += -dy;
            raw_btn = pkt[0] & 7;
            return;
        }
        vga_mouse(dx, -dy, pkt[0] & 7);
    }
}
