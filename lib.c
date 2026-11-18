/* lib.c - funciones minimas de libc (gcc las puede pedir implicitamente) */
#include "kernel.h"

void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
#ifdef __x86_64__
    size_t q = n >> 3;
    u64 pat = (u64)(u8)c * 0x0101010101010101ull;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(pat) : "memory");
    n &= 7;
#else
    size_t q = n >> 2;
    u32 pat = (u32)(u8)c * 0x01010101u;
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(q) : "a"(pat) : "memory");
    n &= 3;
#endif
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
#ifdef __x86_64__
    size_t q = n >> 3;
    __asm__ volatile("rep movsq" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    n &= 7;
#else
    size_t q = n >> 2;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(q) : : "memory");
    n &= 3;
#endif
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return *x - *y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
