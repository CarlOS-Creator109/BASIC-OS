#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* calculadora recursiva */
static const char *p;
static long expr(void);
static long atom(void)
{
    long v;
    while (*p == ' ') p++;
    if (*p == '(') { p++; v = expr(); while (*p == ' ') p++; if (*p == ')') p++; return v; }
    if (*p == '-') { p++; return -atom(); }
    v = 0;
    while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
    return v;
}
static long term(void)
{
    long v = atom();
    for (;;) {
        while (*p == ' ') p++;
        if (*p == '*') { p++; v *= atom(); }
        else if (*p == '/') { p++; v /= atom(); }
        else if (*p == '%') { p++; v %= atom(); }
        else return v;
    }
}
static long expr(void)
{
    long v = term();
    for (;;) {
        while (*p == ' ') p++;
        if (*p == '+') { p++; v += term(); }
        else if (*p == '-') { p++; v -= term(); }
        else return v;
    }
}
static long calc(const char *s) { p = s; return expr(); }

typedef struct { char nombre[16]; int edad; } persona;
typedef int (*binop)(int, int);
static int f_add(int a, int b) { return a + b; }
static int f_mul(int a, int b) { return a * b; }
static int f_max(int a, int b) { return a > b ? a : b; }
static struct { const char *n; binop f; } ops[] = { {"add", f_add}, {"mul", f_mul}, {"max", f_max}, {0, 0} };

static unsigned hash(const char *s) { unsigned h = 5381; while (*s) h = h * 33 ^ (unsigned char)*s++; return h; }

static void reverse(char *s) { char *e = s + strlen(s) - 1; while (s < e) { char t = *s; *s++ = *e; *e-- = t; } }

int main(void)
{
    persona gente[3];
    persona *pp;
    int i, j, total = 0;
    char buf[100];
    unsigned short us = 65535;
    unsigned char bytes[8];
    long long ll = -1;
    printf("%ld %ld %ld %ld\n", calc("1 + 2 * 3"), calc("(1+2)*3"), calc("100 / 7 % 5"), calc("-(4-10)*2"));
    strcpy(gente[0].nombre, "Ana"); gente[0].edad = 30;
    strcpy(gente[1].nombre, "Beto"); gente[1].edad = 25;
    strcpy(gente[2].nombre, "Carlos"); gente[2].edad = 12;
    for (pp = gente; pp < gente + 3; pp++) { pp->edad += 1; total += pp->edad; }
    printf("total=%d %s %d\n", total, gente[2].nombre, (int)(&gente[2] - &gente[0]));
    for (i = 0; ops[i].n; i++) printf("%s=%d ", ops[i].n, ops[i].f(6, 7));
    printf("\n");
    printf("%u %u\n", hash("BASIC-OS"), hash(""));
    strcpy(buf, "abcdef"); reverse(buf); printf("%s\n", buf);
    us++; printf("us=%d\n", us);
    for (i = 0; i < 8; i++) bytes[i] = (unsigned char)(i * 37);
    { unsigned long w = *(unsigned long *)bytes; printf("%lx\n", w); }
    printf("%lld %d\n", ll, (int)sizeof(long long));
    {
        int a[10], *q = a;
        for (i = 0; i < 10; i++) *q++ = i * i;
        q = a;
        printf("%d %d %d\n", *q++, *q, *++q);
        printf("%d\n", q[3] + *(a + 9));
    }
    {
        char m[4][8];
        for (i = 0; i < 4; i++) for (j = 0; j < 7; j++) m[i][j] = 'a' + i + j;
        for (i = 0; i < 4; i++) m[i][7] = 0;
        printf("%s %s\n", m[0], m[3]);
    }
    {
        int x = 10;
        x += 5; x -= 3; x *= 4; x /= 3; x %= 7; x <<= 3; x >>= 1; x |= 1; x &= 0xff; x ^= 0x55;
        printf("x=%d\n", x);
    }
    {
        char *s = "Hola, Mundo";
        int up = 0, lo = 0;
        for (; *s; s++) { if (isupper(*s)) up++; else if (islower(*s)) lo++; }
        printf("%d %d\n", up, lo);
    }
    sprintf(buf, "%d-%s-%c-%x", -42, "tx", 'q', 3054);
    printf("%s %d\n", buf, (int)strlen(buf));
    {
        int c = 0; long big = 1;
        while (big < 1000000000000L) { big *= 3; c++; }
        printf("%d %ld\n", c, big);
    }
    printf("%d %d\n", (char)200, (unsigned char)-1);
    printf("%d\n", (int)(unsigned char)(300 & 0xff));
    return 0;
}
