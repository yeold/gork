/*
 * relaytest.c -- Milestone 0 transport check for the agent harness.
 *
 * POSTs a JSON body read from stdin to a plaintext HTTP relay and dumps
 * the reply: headers to stderr, response body to stdout.
 * No TLS, no JSON parsing, no dependencies beyond libc.
 *
 * Deliberately HTTP/1.0 with "Connection: close": the server then replies
 * without chunked transfer-encoding and closes the socket, so "read until
 * EOF" is a complete and correct framing strategy. That removes the single
 * fiddliest part of HTTP from this test.
 *
 * Target: gcc 2.95 / 3.2, glibc 2.1+, Linux 2.2+. Strict C89.
 *
 *   gcc -std=gnu89 -pedantic -Wall -W -O2 -o relaytest relaytest.c
 *
 * Usage:
 *   ANTHROPIC_API_KEY=sk-... ./relaytest <relayhost> <relayport> < req.json
 *
 * The request body is read from stdin, so request shapes -- tool schemas
 * especially -- can be iterated on by editing a file, with no recompile.
 *
 * Optional environment:
 *   AGENT_HOST   Host: header value      (default api.anthropic.com)
 *   AGENT_PATH   request path            (default /v1/messages)
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define TIMEOUT_SECS 60
#define BUF_INITIAL 8192
#define READ_CHUNK 4096

/* ------------------------------------------------------------------ */
/* growable byte buffer                                               */
/* ------------------------------------------------------------------ */

struct buf {
  char *p;
  size_t len;
  size_t cap;
};

static int buf_init(struct buf *b) {
  b->p = (char *)malloc(BUF_INITIAL);
  if (b->p == NULL)
    return -1;
  b->len = 0;
  b->cap = BUF_INITIAL;
  b->p[0] = '\0';
  return 0;
}

static int buf_add(struct buf *b, const char *src, size_t n) {
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

static void buf_free(struct buf *b) {
  free(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* timeout: unblock a stuck syscall with EINTR, no SA_RESTART         */
/* ------------------------------------------------------------------ */

static void on_alarm(int sig) { (void)sig; }

static void install_alarm(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_alarm;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* no SA_RESTART: we want EINTR */
  if (sigaction(SIGALRM, &sa, (struct sigaction *)0) != 0) {
    perror("sigaction");
    exit(1);
  }
}

/* ------------------------------------------------------------------ */
/* connect                                                            */
/* ------------------------------------------------------------------ */

static int tcp_connect(const char *host, int port) {
  struct hostent *he;
  struct sockaddr_in addr;
  int fd;

  he = gethostbyname(host);
  if (he == NULL || he->h_addr_list[0] == NULL) {
    fprintf(stderr, "relaytest: cannot resolve %s\n", host);
    return -1;
  }
  if (he->h_addrtype != AF_INET) {
    fprintf(stderr, "relaytest: %s is not IPv4\n", host);
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

/* ------------------------------------------------------------------ */
/* full write / read-to-EOF                                           */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char *p, size_t n) {
  size_t off = 0;

  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) {
        fprintf(stderr, "relaytest: timeout while sending\n");
        return -1;
      }
      perror("write");
      return -1;
    }
    if (w == 0) {
      fprintf(stderr, "relaytest: short write\n");
      return -1;
    }
    off += (size_t)w;
  }
  return 0;
}

/* "what" names the source (e.g. "stdin", "socket") so the diagnostics
   say which read failed. */
static int read_to_eof(int fd, struct buf *b, const char *what) {
  char chunk[READ_CHUNK];

  for (;;) {
    ssize_t r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      if (buf_add(b, chunk, (size_t)r) != 0) {
        fprintf(stderr, "relaytest: out of memory reading %s\n", what);
        return -1;
      }
      continue;
    }
    if (r == 0)
      return 0; /* clean EOF */
    if (errno == EINTR) {
      fprintf(stderr, "relaytest: timeout on %s after %lu bytes\n", what,
              (unsigned long)b->len);
      return -1;
    }
    fprintf(stderr, "relaytest: read from %s: %s\n", what, strerror(errno));
    return -1;
  }
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static const char *env_or(const char *name, const char *fallback) {
  const char *v = getenv(name);
  return (v != NULL && *v != '\0') ? v : fallback;
}

/* Split headers from body at CRLF CRLF. Returns offset of body, or 0. */
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

/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  const char *relay_host;
  const char *api_host;
  const char *path;
  const char *key;
  char head[1024];
  struct buf reqbuf;
  struct buf resp;
  int relay_port;
  int fd;
  size_t body_off;

  if (argc != 3) {
    fprintf(stderr, "usage: %s <relayhost> <relayport> < request.json\n",
            argv[0]);
    return 2;
  }
  relay_host = argv[1];
  relay_port = atoi(argv[2]);
  if (relay_port <= 0 || relay_port > 65535) {
    fprintf(stderr, "relaytest: bad port '%s'\n", argv[2]);
    return 2;
  }

  api_host = env_or("AGENT_HOST", "api.anthropic.com");
  path = env_or("AGENT_PATH", "/v1/messages");
  key = env_or("ANTHROPIC_API_KEY", "");

  /* Slurp the request body from stdin BEFORE connecting: no point
     holding an open socket while waiting on a file, and a forgotten
     redirect then blocks harmlessly instead of timing out the server. */
  if (buf_init(&reqbuf) != 0) {
    fprintf(stderr, "relaytest: out of memory\n");
    return 1;
  }
  if (read_to_eof(STDIN_FILENO, &reqbuf, "stdin") != 0) {
    buf_free(&reqbuf);
    return 1;
  }
  if (reqbuf.len == 0) {
    fprintf(stderr, "relaytest: empty request body on stdin\n");
    buf_free(&reqbuf);
    return 1;
  }

  sprintf(head,
          "POST %s HTTP/1.0\r\n"
          "Host: %s\r\n"
          "User-Agent: relaytest/1\r\n"
          "Content-Type: application/json\r\n"
          "anthropic-version: 2023-06-01\r\n"
          "x-api-key: %s\r\n"
          "Content-Length: %lu\r\n"
          "Connection: close\r\n"
          "\r\n",
          path, api_host, key, (unsigned long)reqbuf.len);

  fprintf(stderr, "--> %s:%d  (Host: %s, %lu byte body)\n", relay_host,
          relay_port, api_host, (unsigned long)reqbuf.len);

  install_alarm();

  fd = tcp_connect(relay_host, relay_port);
  if (fd < 0) {
    buf_free(&reqbuf);
    return 1;
  }

  if (buf_init(&resp) != 0) {
    fprintf(stderr, "relaytest: out of memory\n");
    close(fd);
    buf_free(&reqbuf);
    return 1;
  }

  alarm(TIMEOUT_SECS);

  if (write_all(fd, head, strlen(head)) != 0 ||
      write_all(fd, reqbuf.p, reqbuf.len) != 0) {
    alarm(0);
    close(fd);
    buf_free(&reqbuf);
    buf_free(&resp);
    return 1;
  }

  buf_free(&reqbuf); /* sent; not needed past this point */

  if (read_to_eof(fd, &resp, "socket") != 0) {
    alarm(0);
    close(fd);
    buf_free(&resp);
    return 1;
  }

  alarm(0);
  close(fd);

  fprintf(stderr, "<-- %lu bytes\n", (unsigned long)resp.len);

  if (resp.len == 0) {
    fprintf(stderr, "relaytest: empty reply -- relay closed without data\n");
    buf_free(&resp);
    return 1;
  }

  body_off = find_body(resp.p, resp.len);
  if (body_off == 0) {
    fprintf(stderr, "relaytest: no header/body separator; raw dump:\n");
    fwrite(resp.p, 1, resp.len, stdout);
    putchar('\n');
    buf_free(&resp);
    return 1;
  }

  fwrite(resp.p, 1, body_off, stderr); /* status line + headers */
  fwrite(resp.p + body_off, 1, resp.len - body_off, stdout);
  putchar('\n');

  buf_free(&resp);
  return 0;
}
