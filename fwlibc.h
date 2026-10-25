/*
 * fwlibc.h -- between fwlibc.c, the C library shared by gork's firmware
 * ports, and the port's own libc.c.
 */
#ifndef GORK_FWLIBC_H
#define GORK_FWLIBC_H

#include <stddef.h>
#include <stdint.h>

/* From fwlibc.c. */
void fw_stdio_init(void);       /* stdin, stdout, stderr on 0, 1, 2 */

/* From the port. */
int fw_fd_valid(int fd);        /* an open descriptor? */
int fw_text_fd(int fd);         /* a file that gets text mode (CR LF)? */
#ifdef GORK_EFI
size_t to_narrow(const uint16_t *w, size_t n, char *out);  /* UTF-16 to 8 */
#endif

#endif
