/* bcc.h - la cabecera de la biblioteca de BCC. Todos los encabezados estandar (<stdio.h>, <stdlib.h>, <string.h>,
 * <ctype.h>, <stdarg.h>, <stdint.h>...) son este mismo archivo. <bos.h> tambien: trae las funciones de BASIC-OS. */
#ifndef __BCC_H
#define __BCC_H

typedef unsigned long size_t;
typedef long ssize_t;
typedef long ptrdiff_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;
typedef long intmax_t;
typedef unsigned long uintmax_t;
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long int64_t;
typedef unsigned long uint64_t;
typedef long off_t;
typedef long time_t;
typedef long clock_t;
typedef int wchar_t;

typedef char *va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) ((void)0)
#define va_copy(d, s) ((d) = (s))
#define __builtin_va_list va_list

#define NULL ((void *)0)
#define EOF (-1)
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#define offsetof(type, member) ((size_t)&((type *)0)->member)

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN (-128)
#define CHAR_MAX 127
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U
#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL
#define LLONG_MIN LONG_MIN
#define LLONG_MAX LONG_MAX
#define ULLONG_MAX ULONG_MAX
#define INT8_MAX 127
#define INT16_MAX 32767
#define INT32_MAX 2147483647
#define INT64_MAX 9223372036854775807L
#define UINT8_MAX 255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615UL
#define SIZE_MAX 18446744073709551615UL

#define RAND_MAX 32767
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define CLOCKS_PER_SEC 1000
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* teclas que no son letras (las da bos_key/bos_inkey) */
#define KEY_UP    145
#define KEY_DOWN  17
#define KEY_LEFT  157
#define KEY_RIGHT 29
#define KEY_ESC   27

typedef struct __bcc_file FILE;
extern FILE *stdin, *stdout, *stderr;
extern int errno;

/* stdio */
int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
int getchar(void);
char *fgets(char *buf, int n, FILE *f);
int fgetc(FILE *f);
int getc(FILE *f);
int ungetc(int c, FILE *f);
FILE *fopen(const char *name, const char *mode);
int fclose(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int fseek(FILE *f, long off, int whence);
long ftell(FILE *f);
void rewind(FILE *f);
int fflush(FILE *f);
int scanf(const char *fmt, ...);
int sscanf(const char *s, const char *fmt, ...);
void perror(const char *s);

/* stdlib */
void *malloc(size_t n);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int abs(int x);
long labs(long x);
int rand(void);
void srand(unsigned seed);
void exit(int code);
void abort(void);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
char *getenv(const char *name);
#define atoll atol
#define strtoll strtol
#define strtoull strtoul
#define llabs labs

/* string */
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strtok(char *s, const char *delim);
size_t strspn(const char *s, const char *a);
size_t strcspn(const char *s, const char *r);
char *strpbrk(const char *s, const char *a);
char *strerror(int e);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
#define stricmp strcasecmp
#define strnicmp strncasecmp

/* ctype */
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int isprint(int c);
int ispunct(int c);
int iscntrl(int c);
int isgraph(int c);
int toupper(int c);
int tolower(int c);

/* time */
clock_t clock(void);
time_t time(time_t *t);

/* assert */
void __bcc_assert(const char *what, const char *file, int line);
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __bcc_assert(#x, __FILE__, __LINE__))
#endif

/* ---- BASIC-OS (bos.h) ---- */
void bos_print(const char *s);              /* escribe en la consola tal cual */
int  bos_key(void);                         /* espera una tecla (letras como las da el sistema: MAYUSCULAS sin Shift) */
int  bos_inkey(void);                       /* tecla pendiente o 0, sin esperar */
int  bos_esc(void);                         /* 1 si se pulso ESC desde la ultima vez */
unsigned bos_ms(void);                      /* milisegundos desde el arranque */
void bos_sleep(int ms);
int  bos_basic(const char *stmt);           /* ejecuta una sentencia de BASIC: PLOT, COLOR, SOUND, LOCATE... */
int  bos_eval(const char *expr, int *val);  /* evalua una expresion de BASIC */
void bos_sound(int kind, int hz, int arg, int ms);
int  bos_scr_w(void);                       /* ancho de la pantalla (0 = modo texto) */
int  bos_scr_h(void);
void bos_mouse(int *x, int *y, int *btn);
const char *bos_args(void);
int  bos_readline(char *buf, int max);      /* lee una linea con eco; -1 si ESC */
int  bos_read_file(const char *spec, char *buf, int max);   /* lee un archivo (HD0.1:/RUTA/X.TXT); bytes o <0 */
unsigned bos_seed(void);
void *bos_ram(unsigned banks);              /* pide bancos de RAM de 32 MB seguidos (salto de RAM); NULL si no hay */
int   bos_ram_free(void *p);                /* devuelve los bancos que dio bos_ram */
unsigned bos_ram_info(int what);            /* 0 = bancos en total, 1 = libres, 3 = MB de RAM */
#define sleep_ms bos_sleep

#endif
