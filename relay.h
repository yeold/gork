/*
 * relay.h -- plaintext HTTP/1.0 POST through a TLS-terminating relay.
 *
 * The framing decision: HTTP/1.0 + "Connection: close" means the server
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
int relay_post(const char *relay_host, long relay_port,
               const char *api_host, const char *path, const char *api_key,
               const char *body, size_t body_len,
               struct buf *resp);

/*
 * The same for any request: `method` to `host``path`, through a connection
 * to conn_host:conn_port (a relay, or the server itself), with `extra`
 * header lines, each ending in CRLF ("" for none).  If `head_out` is not
 * NULL and a status comes back, the reply's header block is handed over
 * there, for relay_header(); it is the caller's to buf_free.
 */
int relay_http(const char *method, const char *conn_host, long conn_port,
               const char *host, const char *path, const char *extra,
               const char *body, size_t body_len, struct buf *resp,
               struct buf *head_out);

/* The value of header `name` (lowercase) in a block relay_http handed over,
   trimmed, into out[size]: 0, or -1 if there is no such header. */
int relay_header(const struct buf *head, const char *name, char *out,
                 size_t size);

extern int (*relay_abort)(void);

/* If set, the body of a reply with status 200 is handed to it piece by
   piece as it arrives, and `resp` stays empty: for streamed replies.  A
   reply with any other status still ends up in `resp`. */
extern void (*relay_stream)(const char *p, size_t n);

/* Nonzero: send the key as `Authorization: Bearer` (and none if it is empty)
   instead of `x-api-key`, and no anthropic-version.  For `protocol = openai`. */
extern int relay_bearer;

/* Seconds one request (and one MCP tool call) may take; `timeout`. */
extern int relay_timeout;

#endif /* GORK_RELAY_H */
