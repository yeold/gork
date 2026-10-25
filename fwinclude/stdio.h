#ifndef _STDIO_H
#define _STDIO_H
#include <stdarg.h>
#include <sys/types.h>

typedef struct _FILE FILE;

#define EOF          (-1)
#define BUFSIZ       4096
#define FILENAME_MAX 260
#define L_tmpnam     260
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2
#define _IOFBF       0
#define _IOLBF       1
#define _IONBF       2

extern FILE *stdin, *stdout, *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
int    fclose(FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);
int    fflush(FILE *f);
int    fseek(FILE *f, long off, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);
int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *s, const char *fmt, ...);
int    snprintf(char *s, size_t n, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsprintf(char *s, const char *fmt, va_list ap);
int    vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int    sscanf(const char *s, const char *fmt, ...);
int    remove(const char *path);
int    rename(const char *from, const char *to);
char  *tmpnam(char *s);
void   perror(const char *s);
#endif
