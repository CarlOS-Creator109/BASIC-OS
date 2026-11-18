/* cstub.h - lo poco de libc que usan html.c y layout.c del motor de Cosmos Browser (implementado en libc.c) */
#ifndef CSTUB_H
#define CSTUB_H
#include <stddef.h>
#include <stdint.h>
void *malloc(size_t n);
void *calloc(size_t a, size_t b);
void *realloc(void *p, size_t n);
void free(void *p);
char *strdup(const char *s);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strcasestr(const char *h, const char *n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strtok_r(char *s, const char *delim, char **save);
size_t strcspn(const char *s, const char *rej);
long strtol(const char *s, char **end, int base);
float strtof(const char *s, char **end);
double strtod(const char *s, char **end);
double atof(const char *s);
int atoi(const char *s);
int abs(int v);
int snprintf(char *out, size_t cap, const char *fmt, ...);
int tolower(int c);
int toupper(int c);
int isspace(int c);
int isalpha(int c);
int isdigit(int c);
int isalnum(int c);
int isxdigit(int c);
int isupper(int c);
int islower(int c);
int ispunct(int c);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
double floor(double x);
double ceil(double x);
double fabs(double x);
double sqrt(double x);
float floorf(float x);
float ceilf(float x);
float fabsf(float x);
float roundf(float x);
#define NULL ((void *)0)
#endif
