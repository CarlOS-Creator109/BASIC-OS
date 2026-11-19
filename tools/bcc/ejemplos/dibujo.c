/* dibujo.c - graficos desde C: BCC llama a las sentencias de BASIC con bos_basic() */
#include <stdio.h>
#include <bos.h>

static void cmd(const char *fmt, int a, int b, int c, int d, int e)
{
    char s[80];
    sprintf(s, fmt, a, b, c, d, e);
    bos_basic(s);
}

int main(void)
{
    int w, h, i;
    bos_eval("XMAX", &w);
    bos_eval("YMAX", &h);
    bos_basic("CLS");
    for (i = 0; i < 16; i++)
        cmd("LINE %d,%d,%d,%d,%d", 0, i * h / 16, w, h - i * h / 16, i);
    for (i = 1; i < 8; i++)
        cmd("CIRCLE %d,%d,%d,%d", w / 2, h / 2, i * h / 18, 15 - i, 0);
    cmd("BOX %d,%d,%d,%d,%d", w / 2 - 60, 10, w / 2 + 60, 40, 6);
    bos_basic("LOCATE 2,1");
    printf("Dibujado con C (BCC). Pantalla de %d x %d. Una tecla para salir.", w, h);
    bos_key();
    bos_basic("CLS");
    return 0;
}
