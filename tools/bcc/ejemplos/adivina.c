/* adivina.c - juego de adivinar un numero (BCC) */
#include <stdio.h>
#include <stdlib.h>
#include <bos.h>

int main(void)
{
    int secreto, intento = 0, n = 0;
    char linea[32];
    srand(bos_seed());
    secreto = rand() % 100 + 1;
    printf("Pense un numero del 1 al 100. Adivinalo!\n");
    while (intento != secreto) {
        printf("Tu numero: ");
        if (!fgets(linea, sizeof linea, stdin)) break;
        intento = atoi(linea);
        n++;
        if (intento < secreto) printf("Mas grande.\n");
        else if (intento > secreto) printf("Mas chico.\n");
    }
    if (intento == secreto) printf("Si! Era %d. Lo lograste en %d intentos.\n", secreto, n);
    return 0;
}
