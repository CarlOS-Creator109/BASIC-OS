#include <stdio.h>
#include <string.h>
int main(void)
{
    char nombre[64], linea[100];
    int n = 0, edad = 0;
    FILE *f;
    printf("Nombre: ");
    if (fgets(nombre, sizeof nombre, stdin)) nombre[strcspn(nombre, "\n")] = 0;
    printf("Edad: ");
    scanf("%d", &edad);
    printf("Hola %s, en 10 anos tendras %d\n", nombre, edad + 10);
    f = fopen("datos.txt", "r");
    if (!f) { printf("sin archivo\n"); return 1; }
    while (fgets(linea, sizeof linea, f)) printf("%d: %s", ++n, linea);
    fclose(f);
    return 0;
}
