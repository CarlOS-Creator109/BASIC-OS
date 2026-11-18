/* lib32.c - aritmetica de 64 bits para la variante de 32 bits (ARCH=32)
 *
 * Con -m32 gcc llama a __udivdi3 / __umoddi3 / __divdi3 / __moddi3 (libgcc) para dividir enteros
 * de 64 bits. El kernel no enlaza con libgcc, asi que van aqui. Solo se compila con ARCH=32.
 */
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          s64;

static u64 udivmod64(u64 n, u64 d, u64 *rem)
{
    u64 q = 0, r = 0;
    int i;

    if (d == 0) {                       /* como el hardware: division por cero */
        volatile u32 z = 0;
        return (u64)(1u / z);
    }
    if ((n >> 32) == 0 && (d >> 32) == 0) {          /* caso comun: cabe en 32 bits */
        u32 a = (u32)n, b = (u32)d;
        if (rem)
            *rem = a % b;
        return a / b;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

u64 __udivmoddi4(u64 n, u64 d, u64 *rem) { return udivmod64(n, d, rem); }

u64 __udivdi3(u64 n, u64 d) { return udivmod64(n, d, 0); }

u64 __umoddi3(u64 n, u64 d)
{
    u64 r;
    udivmod64(n, d, &r);
    return r;
}

s64 __divdi3(s64 n, s64 d)
{
    int neg = 0;
    u64 un = (u64)n, ud = (u64)d;

    if (n < 0) { un = 0 - un; neg ^= 1; }
    if (d < 0) { ud = 0 - ud; neg ^= 1; }
    un = udivmod64(un, ud, 0);
    return neg ? -(s64)un : (s64)un;
}

s64 __moddi3(s64 n, s64 d)
{
    int neg = n < 0;
    u64 un = (u64)n, ud = (u64)d, r;

    if (n < 0) un = 0 - un;
    if (d < 0) ud = 0 - ud;
    udivmod64(un, ud, &r);
    return neg ? -(s64)r : (s64)r;
}
