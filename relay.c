/*
 * relay.c -- transport for gork. See relay.h.
 *
 * Target: gcc 2.95 / 3.2, glibc 2.1+, Linux 2.2+. Strict C89.
 */

#include <sys/types.h>          /* first: 4.4BSD <netinet/in.h> needs it */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port.h"
#ifdef _WIN32
#include <winsock.h>            /* 1.1: all that Windows 95 has */
#include <wsipx.h>
#elif defined(GORK_DOS16)
#include <tcp.h>                /* Watt-32's own calls: its 16-bit library
                                   has no BSD sockets */
#else
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "relay.h"

#ifdef GORK_NO_FORK
/* A SIGALRM that lands inside Watt-32 aborts the program, and Windows has
   none, so there the timeout is a deadline that read_to_eof polls.  Only
   the read is covered; the connect times out by itself. */
#include <time.h>
static time_t deadline;
#define alarm(s) (deadline = (s) ? time(NULL) + (s) : 0)
#endif

#ifdef _WIN32
/* Sockets are not file descriptors, and their errors are not in errno. */
#define close(fd)       closesocket(fd)
#define write(fd, p, n) send(fd, p, (int)(n), 0)
#define read(fd, p, n)  recv(fd, p, (int)(n), 0)
static void sock_perror(const char *what) {
  fprintf(stderr, "gork: %s: Winsock error %d\n", what, WSAGetLastError());
}
#else
#define sock_perror perror
#endif

#define BUF_INITIAL 8192
#define BUF_LIMIT 61440u        /* 16-bit DOS: the most one buffer holds */
#define READ_CHUNK 4096

int (*relay_abort)(void);
int relay_timeout = 120;
int relay_bearer;

/* The connection is SPX and not TCP: on DOS, where it is no descriptor. */
static int spx;

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
#ifdef GORK_DOS16
    /* no block of memory passes 64K, and size_t wraps there */
    if (want > BUF_LIMIT || want < b->len)
      return -1;
    while (ncap < want)
      ncap = ncap > BUF_LIMIT / 2 ? BUF_LIMIT : ncap * 2;
#else
    while (ncap < want)
      ncap *= 2;
#endif
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

#if defined(_WIN32) || defined(GORK_DOS16)
#define install_alarm() 0
#else
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
#endif

/* ------------------------------------------------------------------ */
/* connect / read / write                                             */
/* ------------------------------------------------------------------ */

#ifdef GORK_DOS16
/* One connection at a time, on Watt-32's own socket.  It is polled:
   tcp_tick() is what makes the library run. */
static tcp_Socket sock16;

static int tcp_connect(const char *host, long port) {
  static int started;
  DWORD ip;
  int status = 0;

  if (!started && sock_init() != 0) {
    fprintf(stderr, "gork: no network: is a packet driver loaded?\n");
    return -1;
  }
  started = 1;
  ip = resolve((char *)host);           /* a name, or a dotted address */
  if (ip == 0) {
    fprintf(stderr, "gork: cannot resolve %s\n", host);
    return -1;
  }
  if (!tcp_open(&sock16, 0, ip, (WORD)port, NULL)) {
    fprintf(stderr, "gork: connect: no route to %s\n", host);
    return -1;
  }
  sock_wait_established(&sock16, sock_delay, NULL, &status);
  return 0;
sock_err:
  fprintf(stderr, "gork: connect: %s\n",
          status == -1 ? sockerr(&sock16) : "timed out");
  return -1;
}

static int write_all(int fd, const char *p, size_t n) {
  (void)fd;
  if (spx)
    return dosspx_write(p, n);
  while (n > 0) {
    int part = n > 8192 ? 8192 : (int)n;
    int w = sock_write(&sock16, (const BYTE *)p, part);

    if (w <= 0) {
      fprintf(stderr, "gork: write: %s\n", sockerr(&sock16));
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static void conn_close(int fd) {
  time_t end = time(NULL) + 2;

  (void)fd;
  if (spx) {
    dosspx_close();
    return;
  }
  sock_close(&sock16);
  while (tcp_tick(&sock16) && time(NULL) <= end)
    ;
  sock_abort(&sock16);
}

/* Returns 0 at EOF, -1 on error, -2 if relay_abort asked to stop. */
static int read_to_eof(int fd, struct buf *b) {
  static char chunk[1024];
  clock_t asked = clock();
  int r, open = 1;

  (void)fd;
  for (;;) {
    if (spx) {
      r = dosspx_read(chunk, sizeof chunk);
      if (r == 0 || r == -1)
        return r;
    } else {
      if (open && !tcp_tick(&sock16))
        open = 0;               /* closed: what has arrived is still there */
      r = sock_fastread(&sock16, (BYTE *)chunk, sizeof chunk);
    }
    if (r > 0) {
      if (buf_add(b, chunk, (size_t)r) != 0) {
        fprintf(stderr, "gork: the reply is too large for the 16-bit "
                        "build (%lu bytes so far)\n", (unsigned long)b->len);
        return -1;
      }
      continue;
    }
    if (!open)
      return 0;
    if (deadline != 0 && time(NULL) >= deadline) {
      fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
              (unsigned long)b->len);
      return -1;
    }
    if (relay_abort != NULL && clock() - asked > CLOCKS_PER_SEC / 5) {
      asked = clock();
      if (relay_abort())
        return -2;
    }
  }
}
#else /* not GORK_DOS16 */

#ifdef _WIN32
static int winsock_start(void) {
  static int started;
  WSADATA wsa;

  if (!started && WSAStartup(MAKEWORD(1, 1), &wsa) != 0) {
    fprintf(stderr, "gork: cannot start Winsock\n");
    return -1;
  }
  started = 1;
  return 0;
}
#endif

static int tcp_connect(const char *host, long port) {
  struct hostent *he;
  struct sockaddr_in addr;
  int fd;

#ifdef _WIN32
  if (winsock_start() != 0)
    return -1;
#endif

  memset(&addr, 0, sizeof addr);
#ifdef _WIN32
  /* gethostbyname() on Windows 9x does not take a numeric address. */
  addr.sin_addr.s_addr = inet_addr(host);
  if (addr.sin_addr.s_addr == INADDR_NONE)
#endif
  {
    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list[0] == NULL) {
      fprintf(stderr, "gork: cannot resolve %s\n", host);
      return -1;
    }
    if (he->h_addrtype != AF_INET) {
      fprintf(stderr, "gork: %s is not IPv4\n", host);
      return -1;
    }
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
  }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    sock_perror("socket");
    return -1;
  }

  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);

  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    sock_perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}

static int write_all(int fd, const char *p, size_t n) {
  size_t off = 0;

#ifdef __MSDOS__
  if (spx)
    return dosspx_write(p, n);
#endif

  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR) {
        fprintf(stderr, "gork: timeout while sending\n");
        return -1;
      }
      sock_perror("write");
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

static void conn_close(int fd) {
#ifdef __MSDOS__
  if (spx) {
    dosspx_close();
    return;
  }
#endif
  close(fd);
}

#ifdef _WIN32
static size_t find_body(const char *p, size_t len);

/* Has the whole reply arrived, going by its Content-Length?  0 as well
   when the header is missing. */
static int reply_complete(const struct buf *b) {
  size_t body = find_body(b->p, b->len), i;

  if (body == 0)
    return 0;
  for (i = 0; i + 16 < body; i++) {
    if ((i == 0 || b->p[i - 1] == '\n') &&
        tolower((unsigned char)b->p[i]) == 'c' &&
        tolower((unsigned char)b->p[i + 7]) == '-' &&
        tolower((unsigned char)b->p[i + 8]) == 'l' &&
        b->p[i + 14] == ':') {
      char name[15];
      size_t j;

      for (j = 0; j < 14; j++)
        name[j] = (char)tolower((unsigned char)b->p[i + j]);
      name[14] = '\0';
      if (strcmp(name, "content-length") == 0)
        return b->len - body >= (size_t)strtoul(b->p + i + 15, NULL, 10);
    }
  }
  return 0;
}

/* SPX on Windows.  Windows Me never reports the end of an SPX connection
   to select(), so the socket is polled, and the reply is also taken as
   complete when its Content-Length has arrived. */
static int spx_read_to_eof(int fd, struct buf *b) {
  char chunk[READ_CHUNK];
  u_long on = 1;
  DWORD asked = GetTickCount();
  int r;

  ioctlsocket(fd, FIONBIO, &on);
  for (;;) {
    r = recv(fd, chunk, sizeof chunk, 0);
    if (r > 0) {
      if (buf_add(b, chunk, (size_t)r) != 0) {
        fprintf(stderr, "gork: out of memory reading socket\n");
        return -1;
      }
      if (reply_complete(b))
        return 0;
      continue;
    }
    if (r == 0)
      return 0;
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      if (WSAGetLastError() == WSAEDISCON || reply_complete(b))
        return 0;
      sock_perror("read from socket");
      return -1;
    }
    if (deadline != 0 && time(NULL) >= deadline) {
      fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
              (unsigned long)b->len);
      return -1;
    }
    if (relay_abort != NULL && GetTickCount() - asked >= 200) {
      asked = GetTickCount();
      if (relay_abort())
        return -2;
    }
    Sleep(50);
  }
}
#endif

/* Returns 0 at EOF, -1 on error, -2 if relay_abort asked to stop. */
static int read_to_eof(int fd, struct buf *b) {
  char chunk[READ_CHUNK];
  fd_set rd;
  struct timeval tv;
  ssize_t r;

#ifdef __MSDOS__
  if (spx) {
    clock_t asked = clock();

    for (;;) {
      int got = dosspx_read(chunk, sizeof chunk);

      if (got == 0 || got == -1)
        return got;
      if (got > 0) {
        if (buf_add(b, chunk, (size_t)got) != 0) {
          fprintf(stderr, "gork: out of memory reading socket\n");
          return -1;
        }
        continue;
      }
      if (deadline != 0 && time(NULL) >= deadline) {
        fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
                (unsigned long)b->len);
        return -1;
      }
      if (relay_abort != NULL && clock() - asked > CLOCKS_PER_SEC / 5) {
        asked = clock();
        if (relay_abort())
          return -2;
      }
    }
  }
#endif
#ifdef _WIN32
  if (spx)
    return spx_read_to_eof(fd, b);
#endif
  for (;;) {
    /* Wake every 200 ms so the user can abort a slow reply. */
#ifndef GORK_NO_FORK
    if (relay_abort != NULL)
#endif
    {
      FD_ZERO(&rd);
      FD_SET(fd, &rd);
      tv.tv_sec = 0;
      tv.tv_usec = 200000;
      r = select(fd + 1, &rd, NULL, NULL, &tv);
      if (r == 0) {
#ifdef GORK_NO_FORK
        if (deadline != 0 && time(NULL) >= deadline) {
          fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
                  (unsigned long)b->len);
          return -1;
        }
#endif
        if (relay_abort != NULL && relay_abort())
          return -2;
        continue;
      }
      if (r < 0) {
        if (errno == EINTR)
          fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
                  (unsigned long)b->len);
        else
          sock_perror("select");
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
#ifdef _WIN32
    sock_perror("read from socket");
#else
    fprintf(stderr, "gork: read from socket: %s\n", strerror(errno));
#endif
    return -1;
  }
}

#endif /* GORK_DOS16 */

/* "spx:NET.NODE" as the 4 and 6 bytes of an IPX address; 0 or -1. */
static int spx_address(const char *host, unsigned char *net,
                       unsigned char *node) {
  const char *dot = strchr(host + 4, '.');
  unsigned long v;
  char part[9];
  int i;

  if (dot == NULL || dot == host + 4 || dot - (host + 4) > 8 ||
      strlen(dot + 1) != 12 ||
      strspn(host + 4, "0123456789abcdefABCDEF") != (size_t)(dot - host - 4) ||
      strspn(dot + 1, "0123456789abcdefABCDEF") != 12)
    return -1;
  memcpy(part, host + 4, (size_t)(dot - host - 4));
  part[dot - host - 4] = '\0';
  v = strtoul(part, NULL, 16);
  for (i = 3; i >= 0; i--, v >>= 8)
    net[i] = (unsigned char)v;
  for (i = 0; i < 6; i++) {
    memcpy(part, dot + 1 + 2 * i, 2);
    part[2] = '\0';
    node[i] = (unsigned char)strtoul(part, NULL, 16);
  }
  return 0;
}

/* Connect to the relay's SPX socket.  A descriptor, or -1; -2 if aborted. */
static int spx_connect(const char *host, int socket_no) {
  unsigned char net[4], node[6];

  if (spx_address(host, net, node) != 0) {
    fprintf(stderr, "gork: bad SPX address '%s', want spx:NET.NODE:SOCKET "
                    "in hex, as the relay prints it\n", host);
    return -1;
  }
#if defined(__MSDOS__)
  {
    int rc = dosspx_open(net, node, (unsigned)socket_no, relay_abort);
    return rc == 0 ? 0 : rc;
  }
#elif defined(_WIN32)
  {
    struct sockaddr_ipx addr;
    int fd;

    if (winsock_start() != 0)
      return -1;
    fd = socket(AF_IPX, SOCK_STREAM, NSPROTO_SPX);
    if (fd < 0) {
      fprintf(stderr, "gork: no SPX here (Winsock error %d): is the "
                      "IPX/SPX protocol installed?\n", WSAGetLastError());
      return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sa_family = AF_IPX;
    memcpy(addr.sa_netnum, net, 4);
    memcpy(addr.sa_nodenum, node, 6);
    addr.sa_socket = htons((unsigned short)socket_no);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
      sock_perror("SPX connect");
      close(fd);
      return -1;
    }
    return fd;
  }
#else
  (void)socket_no;
  fprintf(stderr, "gork: SPX is only in the DOS and Windows builds\n");
  return -1;
#endif
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

int relay_post(const char *relay_host, long relay_port, const char *api_host,
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
          "Content-Type: application/json\r\n",
          path, api_host);
  if (!relay_bearer)
    sprintf(head + strlen(head),
            "anthropic-version: 2023-06-01\r\n"
            "x-api-key: %s\r\n",
            api_key);
  else if (*api_key != '\0')
    sprintf(head + strlen(head), "Authorization: Bearer %s\r\n", api_key);
  sprintf(head + strlen(head),
          "Content-Length: %lu\r\n"
          "Connection: close\r\n"
          "\r\n",
          (unsigned long)body_len);

  if (install_alarm() != 0) {
    buf_free(&raw);
    return -1;
  }

  spx = strncmp(relay_host, "spx:", 4) == 0;
  fd = spx ? spx_connect(relay_host, (int)relay_port)
           : tcp_connect(relay_host, relay_port);
  if (fd < 0) {
    buf_free(&raw);
    return fd == -2 ? -2 : -1;
  }

  alarm(relay_timeout);

  rc = -1;
  if (write_all(fd, head, strlen(head)) != 0 ||
      write_all(fd, body, body_len) != 0 || (rc = read_to_eof(fd, &raw)) != 0) {
    alarm(0);
    conn_close(fd);
    buf_free(&raw);
    return rc == -2 ? -2 : -1;
  }

  alarm(0);
  conn_close(fd);

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
