/*
 * relay.c -- transport for gork. See relay.h.
 *
 * Target: gcc 2.95 / 3.2, glibc 2.1+, Linux 2.2+. Strict C89.
 */

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "relay.h"

#define BUF_INITIAL 8192
#define READ_CHUNK 4096

int (*relay_abort)(void);
int relay_timeout = 120;

/* ------------------------------------------------------------------ */
/* growable byte buffer                                               */
/* ------------------------------------------------------------------ */

int buf_init(struct buf *b) {
  b->p = (char *)malloc(BUF_INITIAL);
  if (b->p == NULL)
    return -1;
  b->len = 0;
  b->cap = BUF_INITIAL;
  b->p[0] = '\0';
  return 0;
}

int buf_add(struct buf *b, const char *src, size_t n) {
  size_t want = b->len + n + 1;

  if (want > b->cap) {
    size_t ncap = b->cap;
    char *np;
    while (ncap < want)
      ncap *= 2;
    np = (char *)realloc(b->p, ncap);
    if (np == NULL)
      return -1;
    b->p = np;
    b->cap = ncap;
  }
  memcpy(b->p + b->len, src, n);
  b->len += n;
  b->p[b->len] = '\0';
  return 0;
}

void buf_free(struct buf *b) {
  free(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* timeout: unblock a stuck syscall with EINTR, no SA_RESTART         */
/* ------------------------------------------------------------------ */

static void on_alarm(int sig) { (void)sig; }

static int install_alarm(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_alarm;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: we want EINTR */
  if (sigaction(SIGALRM, &sa, (struct sigaction *)0) != 0) {
    perror("sigaction");
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* connect / read / write                                             */
/* ------------------------------------------------------------------ */

static int tcp_connect(const char *host, int port) {
  struct hostent *he;
  struct sockaddr_in addr;
  int fd;

  he = gethostbyname(host);
  if (he == NULL || he->h_addr_list[0] == NULL) {
    fprintf(stderr, "gork: cannot resolve %s\n", host);
    return -1;
  }
  if (he->h_addrtype != AF_INET) {
    fprintf(stderr, "gork: %s is not IPv4\n", host);
    return -1;
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}

static int write_all(int fd, const char *p, size_t n) {
  size_t off = 0;

  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) {
        fprintf(stderr, "gork: timeout while sending\n");
        return -1;
      }
      perror("write");
      return -1;
    }
    if (w == 0) {
      fprintf(stderr, "gork: short write\n");
      return -1;
    }
    off += (size_t)w;
  }
  return 0;
}

/* Returns 0 at EOF, -1 on error, -2 if relay_abort asked to stop. */
static int read_to_eof(int fd, struct buf *b) {
  char chunk[READ_CHUNK];
  fd_set rd;
  struct timeval tv;
  ssize_t r;

  for (;;) {
    /* Wake every 200 ms so the user can abort a slow reply. */
    if (relay_abort != NULL) {
      FD_ZERO(&rd);
      FD_SET(fd, &rd);
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      r = select(fd + 1, &rd, NULL, NULL, &tv);
      if (r == 0) {
        if (relay_abort())
          return -2;
        continue;
      }
      if (r < 0) {
        if (errno == EINTR)
          fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
                  (unsigned long)b->len);
        else
          perror("select");
        return -1;
      }
    }
    r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      if (buf_add(b, chunk, (size_t)r) != 0) {
        fprintf(stderr, "gork: out of memory reading socket\n");
        return -1;
      }
      continue;
    }
    if (r == 0)
      return 0; /* clean EOF */
    if (errno == EINTR) {
      fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
              (unsigned long)b->len);
      return -1;
    }
    fprintf(stderr, "gork: read from socket: %s\n", strerror(errno));
    return -1;
  }
}

/* ------------------------------------------------------------------ */
/* response parsing                                                   */
/* ------------------------------------------------------------------ */

/* Offset of the body after CRLF CRLF, or 0 if the separator is absent. */
static size_t find_body(const char *p, size_t len) {
  size_t i;

  if (len < 4)
    return 0;
  for (i = 0; i + 3 < len; i++) {
    if (p[i] == '\r' && p[i + 1] == '\n' && p[i + 2] == '\r' &&
        p[i + 3] == '\n')
      return i + 4;
  }
  return 0;
}

/* Case-insensitive substring search. Header blocks are small, so a naive
   scan is fine; `needle` must already be lowercase.
   ponytail: O(n*m), only ever run over response headers. */
static int ci_contains(const char *hay, size_t hlen, const char *needle) {
  size_t nlen = strlen(needle);
  size_t i, j;

  if (nlen == 0 || hlen < nlen)
    return 0;
  for (i = 0; i + nlen <= hlen; i++) {
    for (j = 0; j < nlen; j++) {
      if (tolower((unsigned char)hay[i + j]) != needle[j])
        break;
    }
    if (j == nlen)
      return 1;
  }
  return 0;
}

/* "HTTP/1.1 429 Too Many Requests" -> 429, or -1 if unparseable. */
static int find_status(const char *p, size_t len) {
  size_t i;
  int code;

  if (len < 12 || memcmp(p, "HTTP/", 5) != 0)
    return -1;
  for (i = 5; i < len && p[i] != ' '; i++)
    ;
  if (i + 3 >= len)
    return -1;
  code = atoi(p + i + 1);
  return (code >= 100 && code < 600) ? code : -1;
}

/* ------------------------------------------------------------------ */

int relay_post(const char *relay_host, int relay_port, const char *api_host,
               const char *path, const char *api_key, const char *body,
               size_t body_len, struct buf *resp) {
  struct buf raw;
  char head[1024];
  int fd;
  int status, rc;
  size_t body_off;

  if (buf_init(resp) != 0) {
    fprintf(stderr, "gork: out of memory\n");
    return -1;
  }
  if (buf_init(&raw) != 0) {
    fprintf(stderr, "gork: out of memory\n");
    return -1;
  }

  /* head is 1024 bytes; path, api_host and api_key are the only variable
     parts and all come from argv/environ, so bound them rather than trust
     them to fit. */
  if (strlen(path) + strlen(api_host) + strlen(api_key) > 700) {
    fprintf(stderr, "gork: request headers too long\n");
    buf_free(&raw);
    return -1;
  }

  sprintf(head,
          "POST %s HTTP/1.0\r\n"
          "Host: %s\r\n"
          "User-Agent: gork/0\r\n"
          "Content-Type: application/json\r\n"
          "anthropic-version: 2023-06-01\r\n"
          "x-api-key: %s\r\n"
          "Content-Length: %lu\r\n"
          "Connection: close\r\n"
          "\r\n",
          path, api_host, api_key, (unsigned long)body_len);

  if (install_alarm() != 0) {
    buf_free(&raw);
    return -1;
  }

  fd = tcp_connect(relay_host, relay_port);
  if (fd < 0) {
    buf_free(&raw);
    return -1;
  }

  alarm(relay_timeout);

  rc = -1;
  if (write_all(fd, head, strlen(head)) != 0 ||
      write_all(fd, body, body_len) != 0 || (rc = read_to_eof(fd, &raw)) != 0) {
    alarm(0);
    close(fd);
    buf_free(&raw);
    return rc == -2 ? -2 : -1;
  }

  alarm(0);
  close(fd);

  if (raw.len == 0) {
    fprintf(stderr, "gork: empty reply -- relay closed without data\n");
    buf_free(&raw);
    return -1;
  }

  body_off = find_body(raw.p, raw.len);
  if (body_off == 0) {
    fprintf(stderr, "gork: no header/body separator in reply\n");
    buf_free(&raw);
    return -1;
  }

  status = find_status(raw.p, raw.len);
  if (status < 0) {
    fprintf(stderr, "gork: unparseable status line\n");
    buf_free(&raw);
    return -1;
  }

  /* We ask for HTTP/1.0 with Connection: close precisely so the reply is
     unchunked and "read to EOF" is correct framing. A server that chunks
     anyway leaves chunk-size lines embedded in the body, which would
     surface much later as a baffling JSON parse error -- so say it here.
     ponytail: detect only. If a server you need does this, dechunking is
     ~30 lines right here. */
  if (ci_contains(raw.p, body_off, "transfer-encoding")) {
    fprintf(stderr, "gork: server replied with a chunked body; gork "
                    "asked for HTTP/1.0 and cannot decode it\n");
    buf_free(&raw);
    return -1;
  }

  if (getenv("GORK_TRACE") != NULL)
    fwrite(raw.p, 1, body_off, stderr);

  if (buf_add(resp, raw.p + body_off, raw.len - body_off) != 0) {
    fprintf(stderr, "gork: out of memory\n");
    buf_free(&raw);
    return -1;
  }

  buf_free(&raw);
  return status;
}
