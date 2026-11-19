#include <stdio.h>
#include <bos.h>

__callback void contador(void *arg)
{
    int i;
    for (i = 1; i <= 5; i++) {
        printf("  [%s] vuelta %d\n", (char *)arg, i);
        bos_dormir(700);
    }
    printf("  [%s] termine\n", (char *)arg);
}

int main(void)
{
    int a = bos_tarea(contador, "TAREA A", "A"), b = bos_tarea(contador, "TAREA B", "B");
    printf("tareas creadas: %d y %d (yo soy la %d)\n", a, b, bos_tarea_actual());
    bos_basic("TAREAS");
    bos_dormir(4500);
    bos_basic("TAREAS");
    printf("main: listo\n");
    return 0;
}
