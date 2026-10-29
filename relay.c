/*
 * relay.c -- transport for gork. See relay.h.
 *
 * Strict C89, for the same systems as main.c.  Four transports: BSD
 * sockets (Unix, DJGPP's Watt-32, Winsock), Watt-32's own calls (16-bit
 * DOS), Minix 2's /dev/tcp, and SPX on DOS and Windows.  Built with
 * GORK_OPENSSL, a "tls:" connection does TLS itself; built with
 * GORK_SERIAL, there is no network at all, only a serial line to
 * gork-relay -S (see docs/SERIAL.md).
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
#elif defined(GORK_MINIX2)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/hton.h>
#include <net/netlib.h>
#include <net/gen/in.h>
#include <net/gen/netdb.h>
#include <net/gen/tcp.h>
#include <net/gen/tcp_io.h>
#elif defined(GORK_SERIAL)
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifndef GORK_NO_SELECT
#include <sys/time.h>
#endif
#else
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#if (defined(GORK_AIXPS2) || defined(__hpux) || defined(GORK_NEXT) || \
     defined(GORK_SUNOS4) || defined(GORK_SVR40) || defined(GORK_PLAN9)) && \
    !defined(GORK_SERIAL)
/* htons, inet_addr */
#include <arpa/inet.h>
#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffffUL
#endif
#endif

#ifdef GORK_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
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

#if defined(_WIN32) || defined(GORK_BEOS)
/* Sockets are not file descriptors. */
#define close(fd)       closesocket(fd)
#define write(fd, p, n) send(fd, p, (int)(n), 0)
#define read(fd, p, n)  recv(fd, p, (int)(n), 0)
#endif
#ifdef _WIN32
/* Their errors are not in errno. */
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
int relay_retry_after;

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

/* The value of header `name` (lowercase) in the header block p[0..len),
   or NULL. */
static const char *header_in(const char *p, size_t len, const char *name) {
  size_t n = strlen(name), i = 0, j;

  while (i < len) {
    for (j = 0; j < n && i + j < len; j++)
      if (tolower((unsigned char)p[i + j]) != name[j])
        break;
    if (j == n && i + n < len && p[i + n] == ':')
      return p + i + n + 1;
    while (i < len && p[i] != '\n')
      i++;
    i++;
  }
  return NULL;
}

static const char *header(const char *name) {
  return header_in(rx.head.p, rx.head_len, name);
}

int relay_header(const struct buf *head, const char *name, char *out,
                 size_t size) {
  const char *v = header_in(head->p, head->len, name);
  size_t n;

  if (v == NULL || size == 0)
    return -1;
  v += strspn(v, " \t");
  n = strcspn(v, "\r\n");
  while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t'))
    n--;
  if (n >= size)
    n = size - 1;
  memcpy(out, v, n);
  out[n] = '\0';
  return 0;
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
    v = header("retry-after");
    if (v != NULL) {
      long ra = strtol(v, NULL, 10);

      if (ra > 0)
        relay_retry_after = ra > 300 ? 300 : (int)ra;
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
  static char last_host[256];
  static DWORD last_ip;
  DWORD ip;
  int status = 0;

  /* Left set, Watt-32 ends the program when it finds no packet driver:
     in the TUI with the screen as curses left it. */
  _watt_do_exit = 0;
  if (!started && sock_init() != 0) {
    fprintf(stderr, "gork: no network: is a packet driver loaded?\n");
    return -1;
  }
  started = 1;
  if (last_ip != 0 && strcmp(host, last_host) == 0) {
    ip = last_ip;
  } else {
    ip = resolve((char *)host);         /* a name, or a dotted address */
    if (ip == 0) {
      fprintf(stderr, "gork: cannot resolve %s\n", host);
      return -1;
    }
    if (strlen(host) < sizeof last_host) {
      strcpy(last_host, host);
      last_ip = ip;
    }
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
#elif defined(GORK_SERIAL)
/* No network: a serial line to gork-relay -S.  The line carries frames
   in SLIP's framing: END, then TYPE ID DATA SUM(2), then END, where an END
   or ESC byte inside is sent as ESC ESC_END or ESC ESC_ESC.  TYPE is 'O'
   (open), 'D' (data) or 'C' (close), ID the request's number, DATA at
   most FRAME_MAX bytes, SUM a Fletcher-16 of TYPE to the end of DATA,
   big-endian.  A request is an 'O', then the bytes of the HTTP request in
   'D's; the reply comes back in 'D's with the same ID, and a 'C' ends it.
   Frames of any other ID are left over from an abandoned request, and
   ignored.  docs/SERIAL.md has the rest. */

#define FRAME_MAX 1024
#define F_END 0xc0
#define F_ESC 0xdb
#define F_ESC_END 0xdc
#define F_ESC_ESC 0xdd

static int ser_fd = -1;
static char ser_spec[256];      /* what ser_fd was opened as */
static unsigned char ser_id;
static int ser_closed;          /* the relay sent this request's 'C' */
static int ser_synced;          /* a frame of this request has come */

static struct frame {
  int esc, bad;
  size_t n, len;                /* bytes so far; the DATA of a whole one */
  unsigned char b[FRAME_MAX + 4];       /* type, id, data, sum */
} ser_rx;

static unsigned fletcher(const unsigned char *p, size_t n) {
  unsigned a = 0, b = 0;

  while (n-- > 0) {
    a = (a + *p++) % 255;
    b = (b + a) % 255;
  }
  return b << 8 | a;
}

/* One byte from the line.  1: a whole frame is in f; 0: not yet; -1: a
   damaged frame.  Damage ends at the next END, so it never takes the
   frames after it along. */
static int frame_byte(struct frame *f, int c) {
  size_t n = f->n;
  int bad = f->bad;

  if (c == F_END) {
    f->n = 0;
    f->esc = f->bad = 0;
    if (n == 0 && !bad)
      return 0;                 /* between frames */
    if (bad || n < 4 ||
        fletcher(f->b, n - 2) != ((unsigned)f->b[n - 2] << 8 | f->b[n - 1]))
      return -1;
    f->len = n - 4;
    return 1;
  }
  if (bad)
    return 0;
  if (f->esc) {
    f->esc = 0;
    if (c == F_ESC_END)
      c = F_END;
    else if (c == F_ESC_ESC)
      c = F_ESC;
    else
      f->bad = 1;
  } else if (c == F_ESC) {
    f->esc = 1;
    return 0;
  }
  if (n == sizeof f->b)
    f->bad = 1;
  if (!f->bad)
    f->b[f->n++] = (unsigned char)c;
  return 0;
}

static int ser_write(const unsigned char *p, size_t n) {
  while (n > 0) {
    int w = (int)write(ser_fd, p, n);

    if (w < 0) {
      if (errno == EINTR)
        fprintf(stderr, "gork: timeout while sending\n");
      else
        fprintf(stderr, "gork: serial line: %s\n", strerror(errno));
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int ser_frame(int type, const char *p, size_t n) {
  static unsigned char raw[FRAME_MAX + 4], out[2 * (FRAME_MAX + 4) + 2];
  unsigned sum;
  size_t i, o = 0;

  raw[0] = (unsigned char)type;
  raw[1] = ser_id;
  memcpy(raw + 2, p, n);
  sum = fletcher(raw, n + 2);
  raw[n + 2] = (unsigned char)(sum >> 8);
  raw[n + 3] = (unsigned char)sum;
  out[o++] = F_END;             /* ends whatever noise came before */
  for (i = 0; i < n + 4; i++) {
    if (raw[i] == F_END || raw[i] == F_ESC) {
      out[o++] = F_ESC;
      out[o++] = raw[i] == F_END ? F_ESC_END : F_ESC_ESC;
    } else {
      out[o++] = raw[i];
    }
  }
  out[o++] = F_END;
  return ser_write(out, o);
}

static const struct {
  long baud;
  speed_t code;
} speeds[] = {
  { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
  { 19200, B19200 }, { 38400, B38400 },
#ifdef B57600
  { 57600, B57600 },
#endif
#ifdef B115200
  { 115200, B115200 },
#endif
#ifdef B230400
  { 230400, B230400 },
#endif
  { 0, B0 }
};

/* "serial:DEVICE[:BAUD[:rtscts]]": open the line, raw, 8N1.  0 or -1. */
static int ser_open(const char *spec) {
  char dev[256], *p, *q;
  long baud = 9600;
  int i, rtscts = 0;
  struct termios t;

  if (strlen(spec) >= sizeof dev) {
    fprintf(stderr, "gork: serial device name too long\n");
    return -1;
  }
  strcpy(dev, spec + 7);
  if ((p = strchr(dev, ':')) != NULL) {
    *p++ = '\0';
    if ((q = strchr(p, ':')) != NULL) {
      *q++ = '\0';
      if (strcmp(q, "rtscts") != 0) {
        fprintf(stderr, "gork: serial: want rtscts after the speed, not "
                        "'%s'\n", q);
        return -1;
      }
      rtscts = 1;
    }
    baud = strtol(p, &q, 10);
    if (*q != '\0')
      baud = -1;
  }
  for (i = 0; speeds[i].baud != 0 && speeds[i].baud != baud; i++)
    ;
  if (speeds[i].baud == 0) {
    fprintf(stderr, "gork: serial: %s is not a speed this system has\n",
            p != NULL ? p : "?");
    return -1;
  }
#ifndef CRTSCTS
  if (rtscts) {
    fprintf(stderr, "gork: serial: this system has no RTS/CTS flow "
                    "control\n");
    return -1;
  }
#endif
  /* O_NONBLOCK: do not wait for a carrier the null modem may not give */
  ser_fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (ser_fd < 0) {
    fprintf(stderr, "gork: %s: %s\n", dev, strerror(errno));
    return -1;
  }
  fcntl(ser_fd, F_SETFL, fcntl(ser_fd, F_GETFL) & ~O_NONBLOCK);
  fcntl(ser_fd, F_SETFD, FD_CLOEXEC);   /* run_command's children */
  if (tcgetattr(ser_fd, &t) != 0) {
    fprintf(stderr, "gork: %s: not a serial line: %s\n", dev,
            strerror(errno));
    close(ser_fd);
    ser_fd = -1;
    return -1;
  }
  t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL |
                 IXON | IXOFF);
  t.c_iflag |= IGNPAR;
  t.c_oflag &= ~OPOST;
  t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | HUPCL);
  t.c_cflag |= CS8 | CREAD | CLOCAL;
#ifdef CRTSCTS
  if (rtscts)
    t.c_cflag |= CRTSCTS;
  else
    t.c_cflag &= ~CRTSCTS;
#endif
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  cfsetispeed(&t, speeds[i].code);
  cfsetospeed(&t, speeds[i].code);
  if (tcsetattr(ser_fd, TCSANOW, &t) != 0) {
    fprintf(stderr, "gork: %s: %s\n", dev, strerror(errno));
    close(ser_fd);
    ser_fd = -1;
    return -1;
  }
  strcpy(ser_spec, spec);
  /* a number of its own, so that a reply to an earlier gork is not taken
     for ours */
  ser_id = (unsigned char)(time(NULL) ^ getpid());
  return 0;
}

/* Start a request: the line, opened once and kept, and an 'O'. */
static int tcp_connect(const char *host, long port) {
  (void)port;
  if (strncmp(host, "serial:", 7) != 0) {
    fprintf(stderr, "gork: this gork talks over a serial line only: want "
                    "serial:DEVICE[:BAUD] and not %s\n", host);
    return -1;
  }
  if (ser_fd >= 0 && strcmp(host, ser_spec) != 0) {
    close(ser_fd);
    ser_fd = -1;
  }
  if (ser_fd < 0 && ser_open(host) != 0)
    return -1;
  tcflush(ser_fd, TCIFLUSH);    /* what is left of an abandoned reply */
  ser_id++;
  ser_closed = ser_synced = 0;
  return ser_frame('O', NULL, 0) == 0 ? ser_fd : -1;
}

static int write_all(int fd, const char *p, size_t n) {
  (void)fd;
  while (n > 0) {
    size_t part = n > FRAME_MAX ? FRAME_MAX : n;

    if (ser_frame('D', p, part) != 0)
      return -1;
    p += part;
    n -= part;
  }
  return 0;
}

/* The line stays open for the next request; the relay is told this one
   is over, unless it said so first. */
static void conn_close(int fd) {
  (void)fd;
  if (!ser_closed)
    ser_frame('C', NULL, 0);
}

/* Returns 0 at the relay's 'C', -1 on error, -2 if relay_abort asked to
   stop. */
static int read_to_eof(int fd) {
  unsigned char chunk[READ_CHUNK];
#ifndef GORK_NO_SELECT
  fd_set rd;
  struct timeval tv;
#endif
  int r, i, rc;

  for (;;) {
#ifndef GORK_NO_SELECT
    /* wake every 200 ms so that the user can abort */
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    r = select(fd + 1, &rd, NULL, NULL, &tv);
    if (r == 0) {
      if (relay_abort != NULL && relay_abort())
        return -2;
      continue;
    }
    if (r < 0) {
      if (errno == EINTR)
        fprintf(stderr, "gork: timeout on the serial line after %lu "
                        "bytes\n", rx.total);
      else
        perror("select");
      return -1;
    }
#endif
    r = (int)read(fd, chunk, sizeof chunk);
    if (r <= 0) {
      if (r < 0 && errno == EINTR)
        fprintf(stderr, "gork: timeout on the serial line after %lu "
                        "bytes\n", rx.total);
      else
        fprintf(stderr, "gork: serial line: %s\n",
                r == 0 ? "hung up" : strerror(errno));
      return -1;
    }
    for (i = 0; i < r; i++) {
      rc = frame_byte(&ser_rx, chunk[i]);
      /* Before the first frame of ours, damage is the end of an
         abandoned reply, cut short: not ours to worry about. */
      if (rc < 0 && !ser_synced)
        continue;
      if (rc < 0) {
        fprintf(stderr, "gork: a damaged frame on the serial line, after "
                        "%lu bytes: try a lower speed, or rtscts\n",
                rx.total);
        return -1;
      }
      if (rc == 0 || ser_rx.b[1] != ser_id)
        continue;
      ser_synced = 1;
      if (ser_rx.b[0] == 'C') {
        ser_closed = 1;
        return 0;
      }
      if (ser_rx.b[0] == 'D') {
        rc = rx_take((const char *)ser_rx.b + 2, ser_rx.len);
        if (rc != 0)
          return rc > 0 ? 0 : -1;
      }
    }
  }
}
#else /* not GORK_DOS16 or GORK_SERIAL */

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

#ifdef GORK_OPENSSL
/* TLS on the connection, for a "tls:" host: no relay needed. */
static SSL_CTX *tls_ctx;
static SSL *ssl;

static void tls_errors(const char *what) {
  unsigned long e = ERR_get_error();
  char msg[256];

  if (e == 0) {
    fprintf(stderr, "gork: %s: %s\n", what,
            errno != 0 ? strerror(errno) : "the server closed the connection");
    return;
  }
  ERR_error_string_n(e, msg, sizeof msg);
  fprintf(stderr, "gork: %s: %s\n", what, msg);
  ERR_clear_error();
}

/* The handshake, checking the certificate against the system's trusted
   ones (or SSL_CERT_FILE / SSL_CERT_DIR) and the name `host`.  0 or -1. */
static int tls_start(int fd, const char *host) {
  X509_VERIFY_PARAM *param;
  long v;

  if (tls_ctx == NULL) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    tls_ctx = SSL_CTX_new(SSLv23_client_method());
#else
    tls_ctx = SSL_CTX_new(TLS_client_method());
#endif
    if (tls_ctx == NULL) {
      tls_errors("TLS");
      return -1;
    }
    SSL_CTX_set_options(tls_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    /* servers that close without close_notify: the reply knows its end */
    SSL_CTX_set_options(tls_ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    SSL_CTX_set_mode(tls_ctx, SSL_MODE_AUTO_RETRY);
    SSL_CTX_set_verify(tls_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(tls_ctx) != 1) {
      tls_errors("TLS: no trusted certificates");
      return -1;
    }
  }
  ssl = SSL_new(tls_ctx);
  if (ssl == NULL) {
    tls_errors("TLS");
    return -1;
  }
  SSL_set_fd(ssl, fd);
  param = SSL_get0_param(ssl);
  /* an address is checked as one, a name by name, and sent with SNI */
  if (X509_VERIFY_PARAM_set1_ip_asc(param, host) != 1) {
    SSL_set_tlsext_host_name(ssl, host);
    X509_VERIFY_PARAM_set1_host(param, host, 0);
  }
  errno = 0;
  if (SSL_connect(ssl) == 1)
    return 0;
  v = SSL_get_verify_result(ssl);
  if (errno == EINTR)
    fprintf(stderr, "gork: timeout in the TLS handshake with %s\n", host);
  else if (v != X509_V_OK)
    fprintf(stderr, "gork: TLS with %s: certificate: %s\n", host,
            X509_verify_cert_error_string(v));
  else
    tls_errors("TLS handshake");
  SSL_free(ssl);
  ssl = NULL;
  return -1;
}

/* SSL_write and SSL_read, as write and read: -1 with errno EINTR on the
   timeout, -3 for any other failure, said already. */
static int tls_io(int rd, char *p, size_t n) {
  int r, e;

  errno = 0;
  r = rd ? SSL_read(ssl, p, (int)n) : SSL_write(ssl, p, (int)n);
  if (r > 0)
    return r;
  e = SSL_get_error(ssl, r);
  if (rd && (e == SSL_ERROR_ZERO_RETURN ||
             (e == SSL_ERROR_SYSCALL && errno == 0 && ERR_peek_error() == 0)))
    return 0;
  if (e == SSL_ERROR_SYSCALL && errno == EINTR)
    return -1;
  tls_errors(rd ? "TLS read" : "TLS write");
  return -3;
}
#define TLS_PENDING() (ssl != NULL && SSL_pending(ssl) > 0)
#else
#define TLS_PENDING() 0
#endif

#ifdef GORK_MINIX2
/* No sockets: a connection is a /dev/tcp opened and set up by ioctl. */
static int tcp_connect(const char *host, long port) {
  static char last_host[256];
  static ipaddr_t last_addr;
  nwio_tcpconf_t conf;
  nwio_tcpcl_t cl;
  struct hostent *he;
  char *dev = getenv("TCP_DEVICE");
  int fd;

  memset(&conf, 0, sizeof conf);
  if (last_host[0] != '\0' && strcmp(host, last_host) == 0) {
    conf.nwtc_remaddr = last_addr;
  } else {
    he = gethostbyname(host);
    if (he == NULL || he->h_addr_list[0] == NULL) {
      fprintf(stderr, "gork: cannot resolve %s\n", host);
      return -1;
    }
    memcpy(&conf.nwtc_remaddr, he->h_addr_list[0], sizeof conf.nwtc_remaddr);
    if (strlen(host) < sizeof last_host) {
      strcpy(last_host, host);
      last_addr = conf.nwtc_remaddr;
    }
  }
  if (dev == NULL)
    dev = TCP_DEVICE;
  fd = open(dev, O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "gork: %s: %s\n", dev, strerror(errno));
    return -1;
  }
  conf.nwtc_flags = NWTC_LP_SEL | NWTC_SET_RA | NWTC_SET_RP;
  conf.nwtc_remport = htons((tcpport_t)port);
  memset(&cl, 0, sizeof cl);
  if (ioctl(fd, NWIOSTCPCONF, &conf) < 0 || ioctl(fd, NWIOTCPCONN, &cl) < 0) {
    perror("connect");
    close(fd);
    return -1;
  }
  return fd;
}
#else
static int tcp_connect(const char *host, long port) {
  static char last_host[256];
  static struct in_addr last_addr;
  struct hostent *he;
  struct sockaddr_in addr;
  int fd;

#ifdef _WIN32
  if (winsock_start() != 0)
    return -1;
#endif

  memset(&addr, 0, sizeof addr);
  if (last_host[0] != '\0' && strcmp(host, last_host) == 0) {
    addr.sin_addr = last_addr;
  } else {
#if defined(_WIN32) || defined(GORK_AIXPS2) || defined(__EMX__) || \
    defined(GORK_NEXT) || defined(GORK_SUNOS4) || defined(GORK_SVR40) || \
    defined(GORK_PLAN9)
    /* gethostbyname() on Windows 9x, AIX PS/2, OS/2 Warp 3, OPENSTEP,
       SunOS 4, SVR4.0 and Plan 9's APE does not take a numeric address. */
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
    if (strlen(host) < sizeof last_host) {
      strcpy(last_host, host);
      last_addr = addr.sin_addr;
    }
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
#endif /* GORK_MINIX2 */

static int write_all(int fd, const char *p, size_t n) {
  size_t off = 0;

#ifdef __MSDOS__
  if (spx)
    return dosspx_write(p, n);
#endif

  while (off < n) {
    ssize_t w;

#ifdef GORK_OPENSSL
    if (ssl != NULL) {
      w = tls_io(0, (char *)p + off, n - off);
      if (w == -3)
        return -1;
    } else
#endif
    w = write(fd, p + off, n - off);
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
#ifdef GORK_OPENSSL
  if (ssl != NULL) {
    SSL_free(ssl);
    ssl = NULL;
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
#ifndef GORK_NO_SELECT
  fd_set rd;
  struct timeval tv;
#endif
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
    /* Wake every 200 ms so the user can abort a slow reply.  Without
       select (Minix 2) the read blocks, and only the timeout ends it. */
#ifndef GORK_NO_SELECT
#ifndef GORK_NO_FORK
    if (relay_abort != NULL && !TLS_PENDING())
#else
    if (!TLS_PENDING())   /* TLS may hold bytes it has read already */
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
#endif
#ifdef GORK_OPENSSL
    if (ssl != NULL) {
      r = tls_io(1, chunk, sizeof chunk);
      if (r == -3)
        return -1;
    } else
#endif
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
  struct buf extra;
  int status;

  if (buf_init(&extra) != 0 ||
      (!relay_bearer &&
       (add(&extra, "anthropic-version: 2023-06-01\r\nx-api-key: ") != 0 ||
        add(&extra, api_key) != 0 || add(&extra, "\r\n") != 0)) ||
      (relay_bearer && *api_key != '\0' &&
       (add(&extra, "Authorization: Bearer ") != 0 ||
        add(&extra, api_key) != 0 || add(&extra, "\r\n") != 0))) {
    fprintf(stderr, "gork: out of memory\n");
    buf_free(&extra);
    buf_init(resp);             /* initialised either way, as promised */
    return -1;
  }
  status = relay_http("POST", relay_host, relay_port, api_host, path,
                      extra.p, body, body_len, resp, NULL);
  buf_free(&extra);
  return status;
}

int relay_http(const char *method, const char *conn_host, long conn_port,
               const char *host, const char *path, const char *extra,
               const char *body, size_t body_len, struct buf *resp,
               struct buf *head_out) {
  struct buf head;
  char num[32];
  int fd, tls;
  int status, rc;

  if (buf_init(resp) != 0) {
    fprintf(stderr, "gork: out of memory\n");
    return -1;
  }
  relay_retry_after = 0;
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
      add(&head, method) != 0 || add(&head, " ") != 0 ||
      add(&head, path) != 0 ||
      add(&head, " HTTP/1.0\r\nHost: ") != 0 || add(&head, host) != 0 ||
      add(&head, "\r\nUser-Agent: gork/0\r\n"
                 "Content-Type: application/json\r\n") != 0 ||
      add(&head, extra) != 0 ||
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

  spx = strncmp(conn_host, "spx:", 4) == 0;
  tls = strncmp(conn_host, "tls:", 4) == 0;
#ifndef GORK_OPENSSL
  if (tls) {
    fprintf(stderr, "gork: %s: this gork has no TLS (configure "
                    "--with-openssl): reach it through a relay\n",
            conn_host + 4);
    buf_free(&head);
    buf_free(&rx.head);
    return -1;
  }
#endif
#ifdef GORK_SERIAL
  alarm(relay_timeout);         /* the 'O' may wait for the line too */
#endif
  fd = spx ? spx_connect(conn_host, (int)conn_port)
           : tcp_connect(conn_host + (tls ? 4 : 0), conn_port);
  if (fd < 0) {
    alarm(0);
    buf_free(&head);
    buf_free(&rx.head);
    return fd == -2 ? -2 : -1;
  }

  alarm(relay_timeout);

  rc = -1;
  if (
#ifdef GORK_OPENSSL
      (tls && tls_start(fd, conn_host + 4) != 0) ||
#endif
      write_all(fd, head.p, head.len) != 0 ||
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

  if (head_out != NULL) {       /* the caller's now, headers only */
    rx.head.len = rx.head_len;
    rx.head.p[rx.head_len] = '\0';
    *head_out = rx.head;
    return status;
  }
  buf_free(&rx.head);
  return status;
}
