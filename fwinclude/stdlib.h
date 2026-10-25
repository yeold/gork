#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     32767

void         *malloc(size_t n);
void         *calloc(size_t n, size_t size);
void         *realloc(void *p, size_t n);
void          free(void *p);
void          exit(int status) __attribute__((noreturn));
void          abort(void) __attribute__((noreturn));
int           atexit(void (*fn)(void));
int           atoi(const char *s);
long          atol(const char *s);
double        atof(const char *s);
long          strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
#if __STDC_VERSION__ >= 199901L
long long     strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
#endif
double        strtod(const char *s, char **end);
char         *getenv(const char *name);
int           putenv(char *s);
int           system(const char *cmd);
void          qsort(void *base, size_t n, size_t size,
                    int (*cmp)(const void *, const void *));
int           abs(int v);
long          labs(long v);
int           rand(void);
void          srand(unsigned seed);
char         *realpath(const char *path, char *out);
#endif
