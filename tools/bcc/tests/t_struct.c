#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct punto { int x, y; } punto;
struct nodo { int val; struct nodo *sig; };
typedef union { int i; unsigned char b[4]; } U;
enum color { ROJO, VERDE = 5, AZUL };
struct cfg { const char *nombre; int n; int v[3]; };
struct cfg tabla[] = { {"a", 1, {1, 2, 3}}, {"b", 2}, {.n = 7, .nombre = "c"} };
int (*op)(int, int);
int suma(int a, int b) { return a + b; }
int resta(int a, int b) { return a - b; }
int aplicar(int (*f)(int, int), int a, int b) { return f(a, b); }
int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
int main(void)
{
    punto p = {3, 4}, q;
    struct nodo *lista = 0, *n;
    U u;
    int i, v[] = {5, 3, 9, 1, 7};
    char s[20] = "hola";
    q = p;
    q.x += 10;
    printf("%d %d %d %d\n", p.x, p.y, q.x, q.y);
    for (i = 0; i < 4; i++) { n = malloc(sizeof *n); n->val = i * i; n->sig = lista; lista = n; }
    for (n = lista; n; n = n->sig) printf("%d ", n->val);
    printf("\n");
    u.i = 0x01020304;
    printf("%d %d\n", u.b[0], u.b[3]);
    printf("%d %d %d\n", ROJO, VERDE, AZUL);
    for (i = 0; i < 3; i++) printf("%s %d %d %d\n", tabla[i].nombre, tabla[i].n, tabla[i].v[0], tabla[i].v[2]);
    op = suma; printf("%d ", op(2, 3)); op = resta; printf("%d %d\n", op(2, 3), aplicar(suma, 10, 20));
    qsort(v, 5, sizeof v[0], cmp);
    for (i = 0; i < 5; i++) printf("%d ", v[i]);
    printf("\n");
    strcat(s, " mundo");
    printf("%s %d %s\n", s, (int)sizeof(struct cfg), strchr(s, 'm'));
    {
        int m[3][4], j, t = 0;
        for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) m[i][j] = i * 10 + j;
        for (i = 0; i < 3; i++) t += m[i][3];
        printf("t=%d %d\n", t, m[2][1]);
    }
    {
        char *dup = strdup("copia");
        char *tok, buf[] = "a,b,,c";
        printf("%s\n", dup);
        for (tok = strtok(buf, ","); tok; tok = strtok(0, ",")) printf("[%s]", tok);
        printf("\n");
    }
    {
        static int cuenta;
        cuenta++;
        printf("static=%d\n", cuenta);
    }
    goto fin;
    printf("no se ve\n");
fin:
    printf("fin %s\n", __func__);
    return 0;
}
