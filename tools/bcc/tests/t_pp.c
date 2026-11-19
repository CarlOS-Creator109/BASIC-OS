#include <stdio.h>
#include "t_pp.h"
#include "t_pp.h"
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define LOG(fmt, ...) printf("log: " fmt "\n", __VA_ARGS__)
#define LOG0(...) printf(__VA_ARGS__)
#define VERSION 3
#define DEBUG
#if VERSION >= 3 && defined(DEBUG)
#define MODO "nuevo"
#elif VERSION == 2
#define MODO "viejo"
#else
#define MODO "antiguo"
#endif
#ifndef NADA
int nada = 1;
#endif
#undef DEBUG
#ifdef DEBUG
#error no deberia
#endif
#define A B
#define B A
int CAT(var, 1) = 11;
int main(void)
{
    int A = 4;
    printf("%d %d\n", MAX(3, 8), CUADRADO(5));
    printf("%s %s %s\n", STR(hola mundo), XSTR(VERSION), MODO);
    printf("%d %d %d\n", var1, nada, del_header());
    LOG("%d y %s", 5, "x");
    LOG0("sin %s\n", "args");
    printf("%d\n", A);
    printf("linea %d\n", __LINE__);
#if 0
    esto no compila
#endif
    printf("%s\n", "a" "b" "c");
    return 0;
}
