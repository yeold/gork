/*
 * relay.h -- plaintext HTTP/1.0 POST through a TLS-terminating relay.
 *
 * Lifted from testing/relaytest.c, which proved the transport in milestone 0.
 * Same framing decision: HTTP/1.0 + "Connection: close" means the server
 * replies unchunked and closes, so "read until EOF" is complete and correct.
 */

#ifndef GORK_RELAY_H
#define GORK_RELAY_H

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* growable byte buffer                                               */
/* ------------------------------------------------------------------ */

struct buf {
    char  *p;
    size_t len;
    size_t cap;
};

int  buf_init(struct buf *b);
int  buf_add(struct buf *b, const char *src, size_t n);
void buf_free(struct buf *b);

/*
 * POST `body` to relay_host:relay_port, which forwards to api_host`path`.
 *
 * On success returns the HTTP status code and fills `resp` with the response
 * BODY only (headers are stripped, and dumped to stderr when GORK_TRACE is
 * set). Returns -1 on transport failure. `resp` is initialised either way and
 * is the caller's to buf_free.
 *
 * If relay_abort is set, it is polled while waiting for the reply; when it
 * returns nonzero the request is abandoned and relay_post returns -2.
 *
 * A non-200 status is NOT a failure here -- the body carries the API's error
 * JSON, which the caller should report. Check the return value.
 */
int relay_post(const char *relay_host, int relay_port,
               const char *api_host, const char *path, const char *api_key,
               const char *body, size_t body_len,
               struct buf *resp);

extern int (*relay_abort)(void);

#endif /* GORK_RELAY_H */
