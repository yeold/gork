/*
 * relay.c -- transport for gork. See relay.h.
 *
 * Strict C89, for the same systems as main.c.  Three transports: BSD
 * sockets (Unix, DJGPP's Watt-32, Winsock), Watt-32's own calls (16-bit
 * DOS), and SPX on DOS and Windows.
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

static int add(struct buf *b, const char *s) {
  return buf_add(b, s, strlen(s));
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
/* the reply, as it arrives                                           */
/* ------------------------------------------------------------------ */

void (*relay_stream)(const char *p, size_t n);

/* What is known of the reply so far.  The headers are kept; the body goes
   to relay_stream as it comes, if that is set and the status is 200, and
   into `body` otherwise.  A chunked body is decoded on the way: HTTP/1.0
   was asked for so that there would be none, but servers send them. */
static struct {
  struct buf head;
  struct buf *body;
  size_t head_len;      /* 0 until the headers are complete */
  unsigned long total;  /* bytes received, for the messages */
  int status;
  int chunked, cstate, blank;
  int have_len;         /* a Content-Length was given */
  unsigned long left;   /* bytes still due: of the chunk, or of the body */
  int done;             /* the reply is complete; no need to wait for EOF */
} rx;

enum { C_SIZE, C_EXT, C_DATA, C_AFTER, C_TRAILER };

/* The value of header `name` (lowercase), or NULL. */
static const char *header(const char *name) {
  size_t n = strlen(name), i = 0, j;

  while (i < rx.head_len) {
    for (j = 0; j < n && i + j < rx.head_len; j++)
      if (tolower((unsigned char)rx.head.p[i + j]) != name[j])
        break;
    if (j == n && rx.head.p[i + n] == ':')
      return rx.head.p + i + n + 1;
    while (i < rx.head_len && rx.head.p[i] != '\n')
      i++;
    i++;
  }
  return NULL;
}

static int rx_emit(const char *p, size_t n) {
  if (n == 0)
    return 0;
  if (relay_stream != NULL && rx.status == 200) {
    relay_stream(p, n);
    return 0;
  }
  return buf_add(rx.body, p, n);
}

static int rx_body(const char *p, size_t n) {
  if (!rx.chunked) {
    if (rx.have_len) {
      if (n >= rx.left) {
        n = (size_t)rx.left;
        rx.done = 1;
      }
      rx.left -= n;
    }
    return rx_emit(p, n);
  }
  while (n > 0 && !rx.done) {
    int c = (unsigned char)*p;

    switch (rx.cstate) {
    case C_SIZE:                /* the length, in hex */
      if (isxdigit(c)) {
        rx.left = rx.left * 16 +
                  (unsigned long)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
        break;
      }
      rx.cstate = C_EXT;
      /* fall through */
    case C_EXT:                 /* and whatever else is on that line */
      if (c == '\n') {
        rx.cstate = rx.left > 0 ? C_DATA : C_TRAILER;
        rx.blank = 1;
      }
      break;
    case C_DATA: {
      size_t m = n < rx.left ? n : (size_t)rx.left;

      if (rx_emit(p, m) != 0)
        return -1;
      rx.left -= m;
      if (rx.left == 0)
        rx.cstate = C_AFTER;
      p += m;
      n -= m;
      continue;
    }
    case C_AFTER:               /* the line end after the data */
      if (c == '\n')
        rx.cstate = C_SIZE;
      break;
    case C_TRAILER:             /* after the last chunk: to an empty line */
      if (c == '\n') {
        if (rx.blank)
          rx.done = 1;
        rx.blank = 1;
      } else if (c != '\r') {
        rx.blank = 0;
      }
      break;
    }
    p++;
    n--;
  }
  return 0;
}

/* Take in what was just received.  0: go on reading; 1: the reply is
   complete; -1: failed, and said so. */
static int rx_take(const char *p, size_t n) {
  const char *v;
  size_t off;
  int rc;

  rx.total += n;
  if (rx.head_len != 0) {
    rc = rx_body(p, n);
  } else {
    if (buf_add(&rx.head, p, n) != 0) {
      fprintf(stderr, "gork: out of memory reading socket\n");
      return -1;
    }
    off = find_body(rx.head.p, rx.head.len);
    if (off == 0)
      return 0;
    rx.head_len = off;
    rx.status = find_status(rx.head.p, off);
    v = header("transfer-encoding");
    rx.chunked = v != NULL && ci_contains(v, strcspn(v, "\r\n"), "chunked");
    v = header("content-length");
    if (v != NULL && !rx.chunked) {
      rx.have_len = 1;
      rx.left = strtoul(v, NULL, 10);
      if (rx.left == 0)
        rx.done = 1;
    }
    rc = rx_body(rx.head.p + off, rx.head.len - off);
    rx.head.len = off;
  }
  if (rc != 0) {
#ifdef GORK_DOS16
    fprintf(stderr, "gork: the reply is too large for the 16-bit build "
                    "(%lu bytes so far)\n", rx.total);
#else
    fprintf(stderr, "gork: out of memory reading socket\n");
#endif
    return -1;
  }
  return rx.done;
}

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
static int read_to_eof(int fd) {
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
      int rc = rx_take(chunk, (size_t)r);

      if (rc != 0)
        return rc > 0 ? 0 : -1;
      continue;
    }
    if (!open)
      return 0;
    if (deadline != 0 && time(NULL) >= deadline) {
      fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
              rx.total);
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
/* SPX on Windows.  Windows Me never reports the end of an SPX connection
   to select(), so the socket is polled; rx_take() knows the reply is
   complete from its Content-Length. */
static int spx_read_to_eof(int fd) {
  char chunk[READ_CHUNK];
  u_long on = 1;
  DWORD asked = GetTickCount();
  int r;

  ioctlsocket(fd, FIONBIO, &on);
  for (;;) {
    r = recv(fd, chunk, sizeof chunk, 0);
    if (r > 0) {
      int rc = rx_take(chunk, (size_t)r);

      if (rc != 0)
        return rc > 0 ? 0 : -1;
      continue;
    }
    if (r == 0)
      return 0;
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
      if (WSAGetLastError() == WSAEDISCON)
        return 0;
      sock_perror("read from socket");
      return -1;
    }
    if (deadline != 0 && time(NULL) >= deadline) {
      fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
              rx.total);
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
static int read_to_eof(int fd) {
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
        int rc = rx_take(chunk, (size_t)got);

        if (rc != 0)
          return rc > 0 ? 0 : -1;
        continue;
      }
      if (deadline != 0 && time(NULL) >= deadline) {
        fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
                rx.total);
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
    return spx_read_to_eof(fd);
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
                  rx.total);
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
                  rx.total);
        else
          sock_perror("select");
        return -1;
      }
    }
    r = read(fd, chunk, sizeof chunk);
    if (r > 0) {
      int rc = rx_take(chunk, (size_t)r);

      if (rc != 0)
        return rc > 0 ? 0 : -1;
      continue;
    }
    if (r == 0)
      return 0; /* clean EOF */
    if (errno == EINTR) {
      fprintf(stderr, "gork: timeout on socket after %lu bytes\n",
              rx.total);
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

int relay_post(const char *relay_host, long relay_port, const char *api_host,
               const char *path, const char *api_key, const char *body,
               size_t body_len, struct buf *resp) {
  struct buf head;
  char num[32];
  int fd;
  int status, rc;

  if (buf_init(resp) != 0) {
    fprintf(stderr, "gork: out of memory\n");
    return -1;
  }
  memset(&rx, 0, sizeof rx);
  rx.body = resp;
  if (buf_init(&rx.head) != 0) {
    fprintf(stderr, "gork: out of memory\n");
    return -1;
  }

  /* The head grows as it must: path, host and key are as long as the
     configuration made them. */
  sprintf(num, "%lu", (unsigned long)body_len);
  if (buf_init(&head) != 0 ||
      add(&head, "POST ") != 0 || add(&head, path) != 0 ||
      add(&head, " HTTP/1.0\r\nHost: ") != 0 || add(&head, api_host) != 0 ||
      add(&head, "\r\nUser-Agent: gork/0\r\n"
                 "Content-Type: application/json\r\n") != 0 ||
      (!relay_bearer &&
       (add(&head, "anthropic-version: 2023-06-01\r\nx-api-key: ") != 0 ||
        add(&head, api_key) != 0 || add(&head, "\r\n") != 0)) ||
      (relay_bearer && *api_key != '\0' &&
       (add(&head, "Authorization: Bearer ") != 0 ||
        add(&head, api_key) != 0 || add(&head, "\r\n") != 0)) ||
      add(&head, "Content-Length: ") != 0 || add(&head, num) != 0 ||
      add(&head, "\r\nConnection: close\r\n\r\n") != 0) {
    fprintf(stderr, "gork: out of memory\n");
    buf_free(&rx.head);
    return -1;
  }

  if (install_alarm() != 0) {
    buf_free(&head);
    buf_free(&rx.head);
    return -1;
  }

  spx = strncmp(relay_host, "spx:", 4) == 0;
  fd = spx ? spx_connect(relay_host, (int)relay_port)
           : tcp_connect(relay_host, relay_port);
  if (fd < 0) {
    buf_free(&head);
    buf_free(&rx.head);
    return fd == -2 ? -2 : -1;
  }

  alarm(relay_timeout);

  rc = -1;
  if (write_all(fd, head.p, head.len) != 0 ||
      write_all(fd, body, body_len) != 0 || (rc = read_to_eof(fd)) != 0) {
    alarm(0);
    conn_close(fd);
    buf_free(&head);
    buf_free(&rx.head);
    return rc == -2 ? -2 : -1;
  }
  buf_free(&head);

  alarm(0);
  conn_close(fd);

  if (rx.total == 0) {
    fprintf(stderr, "gork: empty reply -- relay closed without data\n");
    buf_free(&rx.head);
    return -1;
  }
  if (rx.head_len == 0) {
    fprintf(stderr, "gork: no header/body separator in reply\n");
    buf_free(&rx.head);
    return -1;
  }
  status = rx.status;
  if (status < 0) {
    fprintf(stderr, "gork: unparseable status line\n");
    buf_free(&rx.head);
    return -1;
  }
  if (rx.chunked && !rx.done) {
    fprintf(stderr, "gork: the reply ended in the middle of a chunk\n");
    buf_free(&rx.head);
    return -1;
  }

  if (getenv("GORK_TRACE") != NULL)
    fwrite(rx.head.p, 1, rx.head_len, stderr);

  buf_free(&rx.head);
  return status;
}
