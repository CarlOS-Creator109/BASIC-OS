#include <stdio.h>
#include <string.h>

int primo(int n)
{
    int d;
    if (n < 2) return 0;
    for (d = 2; d * d <= n; d++)
        if (n % d == 0) return 0;
    return 1;
}

int main(void)
{
    char nombre[40];
    int i, n = 0;
    printf("Hola desde C en BASIC-OS!\n");
    printf("Como te llamas? ");
    if (fgets(nombre, sizeof nombre, stdin)) nombre[strcspn(nombre, "\n")] = 0;
    printf("Mucho gusto, %s. Primos hasta 100:\n", nombre);
    for (i = 0; i <= 100; i++)
        if (primo(i)) printf("%d%s", i, ++n % 10 ? " " : "\n");
    printf("\n(%d primos) a%co: %d\n", n, 0xA4, 2026);
    printf("Año con UTF-8: ñandú\n");
    return 0;
}
