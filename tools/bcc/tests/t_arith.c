#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int g = 5;
int arr[5] = {1, 2, 3, 4, 5};
char *names[] = {"uno", "dos", "tres"};
static int sq(int x) { return x * x; }
long fact(long n) { return n <= 1 ? 1 : n * fact(n - 1); }
int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
int sum(int n, ...) { va_list ap; int s = 0, i; va_start(ap, n); for (i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }
int main(void)
{
    int i, a = 7, b = -3;
    unsigned u = 4000000000u;
    unsigned char uc = 250;
    signed char sc = -5;
    short sh = -1234;
    long l = 1234567890123L;
    printf("%d %d %d %d %d\n", a + b, a - b, a * b, a / b, a % b);
    printf("%u %u %u\n", u, u / 3, u + 500000000u);
    uc += 10; sc *= 30;
    printf("%d %d %d\n", uc, sc, sh * 2);
    printf("%ld %ld\n", l * 3, l >> 4);
    printf("%d %d %d\n", -7 >> 1, 7 << 3, (unsigned)-7 >> 28);
    printf("%d %d %d %d\n", a < b, a > b, (unsigned)a < (unsigned)b, -1 < 0u);
    printf("%d %ld %d\n", sq(9), fact(15), fib(20));
    for (i = 0; i < 5; i++) g += arr[i];
    printf("g=%d %s %s %c\n", g, names[2], names[0] + 1, *names[1]);
    printf("sum=%d\n", sum(4, 10, 20, 30, 40));
    printf("[%5d] [%-5d] [%05d] [%x] [%X] [%#x] [%o] [%c] [%s] [%.2s] [%8s] [%-8s]|\n", 42, 42, 42, 255, 255, 255, 8, 'Z', "texto", "abc", "der", "izq");
    printf("%%  %p\n", (void *)0);
    {
        char buf[64];
        snprintf(buf, sizeof buf, "n=%d s=%s", 12, "ok");
        printf("%s len=%d cmp=%d\n", buf, (int)strlen(buf), strcmp("abc", "abd") < 0);
    }
    printf("%d %d\n", atoi("-321"), (int)strtol("ff", 0, 16));
    i = 0;
    while (1) { if (++i > 10) break; if (i % 2) continue; printf("%d ", i); }
    printf("\n");
    do { i--; } while (i > 3);
    printf("i=%d\n", i);
    switch (i) { case 1: printf("uno\n"); break; case 3: printf("tres\n"); case 4: printf("cae\n"); break; default: printf("otro\n"); }
    {
        int x = 3, *p = &x, **pp = &p;
        **pp = 9;
        printf("x=%d %d\n", x, p == &x);
    }
    printf("%d %d\n", (int)sizeof(long), (int)sizeof arr);
    printf("%d\n", 3 > 2 ? 10 : 20);
    printf("%d %d %d\n", !0, !5, ~0);
    printf("%d\n", (1 && 0) || (2 && 3));
    return 0;
}
