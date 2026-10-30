/*
 * serline.h -- the serial port itself, under the frames relay.c sends to
 * gork-relay -S (docs/SERIAL.md).  One implementation per system in
 * serline.c: termios on Unix, the UART on DOS, the comm API on Windows.
 * Each says what went wrong on stderr before it returns -1.
 */

#ifndef GORK_SERLINE_H
#define GORK_SERLINE_H

#include <stddef.h>

/* Open DEVICE at `baud`, 8N1, raw, with RTS/CTS if `rtscts`.  0 or -1. */
int line_open(const char *dev, long baud, int rtscts);

/* All of p[n]: 0, or -1 (also when the alarm goes off, on Unix, and on
   the other systems when CTS stays down past `timeout`). */
int line_write(const unsigned char *p, size_t n);

/* What has come, into p[n]: waits for it about 200 ms at most, so that
   the caller can poll for Esc.  The count, 0 if nothing came, -1, or -2
   (not said) when the alarm went off, on Unix. */
int line_read(unsigned char *p, size_t n);

/* Throw away what has come and not been read. */
void line_flush(void);

void line_close(void);

#endif /* GORK_SERLINE_H */
