/*
 * bb.h -- between the two halves of gork for barebox.
 *
 * gork's half (main.c, relay.c, cJSON, ip.c, fwlibc.c and barebox/libc.c)
 * is built against fwinclude/, with every C library name made gork_NAME
 * (barebox/rename.py), so that it meets nothing of barebox's at the link.
 * barebox's half (glue.c) is built as barebox builds its commands, with
 * barebox's headers, and does what touches barebox through these calls.
 * Plain C types only: the two halves share no other header.
 *
 * Errors are barebox's: -errno, with the Linux numbers fwinclude/ has.
 */
#ifndef GORK_BB_H
#define GORK_BB_H

#include <stddef.h>

/* memory */
void *bb_malloc(size_t n);
void *bb_realloc(void *p, size_t n);
void  bb_free(void *p);

/* files: open flags are the Linux ones, as both sides have them */
int       bb_open(const char *path, int flags, unsigned mode);
long      bb_read(int fd, void *p, size_t n);
long      bb_write(int fd, const void *p, size_t n);
int       bb_close(int fd);
long long bb_lseek(int fd, long long off, int whence);
/* stat: 0 with *isdir and *size, or -errno; follow: not lstat */
int       bb_stat(const char *path, int follow, int *isdir, long long *size);
int       bb_mkdir(const char *path);
int       bb_unlink(const char *path);
int       bb_rmdir(const char *path);
int       bb_rename(const char *from, const char *to);     /* a copy */
const char *bb_getcwd(void);
int       bb_chdir(const char *path);
void     *bb_opendir(const char *path);
const char *bb_readdir(void *d);        /* the next name, or NULL */
void      bb_closedir(void *d);

/* the environment: barebox's variables */
const char *bb_getenv(const char *name);
int       bb_setenv(const char *name, const char *value);

/* the console */
void      bb_console_write(const char *p, size_t n);
int       bb_console_getc(void);        /* a key, or -1 if none waits */
int       bb_ctrlc(void);               /* Ctrl-C pressed? */

/* time: since barebox started, and the time of day if a clock has it */
unsigned long long bb_ms(void);
long long bb_epoch(void);               /* seconds since 1970, or 0 */
void      bb_udelay(unsigned us);

/* a command line for barebox's shell: its status; with out, what it
   printed meanwhile goes into *out (bb_malloc's, *len bytes) instead of
   to the console, since barebox's shell redirects nothing */
int       bb_run(const char *line, char **out, size_t *len);

/* the end of the gork command, with its status: does not return */
void      bb_exit(int status) __attribute__((noreturn));

#endif
