/*
 * libc.c -- the C library of gork.efi: what main.c, relay.c and cJSON
 * call, on top of UEFI and the UEFI shell, and nothing else.  Files go
 * through the shell's file calls, so paths are the shell's (fs0:\dir\x,
 * or relative to its current directory), and the program must be
 * started from the shell.  The network is in net.c.  See
 * docs/build/uefi.md.
 *
 * Text is UTF-8 here and CHAR16 at the firmware: efi_wide() and
 * efi_narrow() convert.  The console takes \r\n; files get what was
 * written.
 */

#include "efi.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>

EFI_HANDLE          efi_image;
EFI_SYSTEM_TABLE   *efi_st;
EFI_BOOT_SERVICES  *efi_bs;
EFI_SHELL_PROTOCOL *efi_shell;
static EFI_SHELL_PARAMETERS_PROTOCOL *params;

int errno;
int _fltused;           /* the MSVC target wants it once floats are used */

int main(int argc, char **argv);

/* ------------------------------------------------------------------ */
/* memory: pool allocations, kept on a list so that exit() frees them  */
/* ------------------------------------------------------------------ */

struct block {
    struct block *prev, *next;
    size_t        size;
    size_t        pad;          /* 32 bytes: what follows stays aligned */
};

static struct block *blocks;

void *malloc(size_t n)
{
    struct block *b;

    if (n > (size_t) -1 - sizeof *b ||
        efi_bs->AllocatePool(EfiLoaderData, sizeof *b + n, (void **) &b)
            != EFI_SUCCESS) {
        errno = ENOMEM;
        return NULL;
    }
    b->size = n;
    b->prev = NULL;
    b->next = blocks;
    if (blocks != NULL) blocks->prev = b;
    blocks = b;
    return b + 1;
}

void free(void *p)
{
    struct block *b;

    if (p == NULL) return;
    b = (struct block *) p - 1;
    if (b->prev != NULL) b->prev->next = b->next;
    else blocks = b->next;
    if (b->next != NULL) b->next->prev = b->prev;
    efi_bs->FreePool(b);
}

void *calloc(size_t n, size_t size)
{
    void *p;

    if (size != 0 && n > (size_t) -1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    p = malloc(n * size);
    if (p != NULL) memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    void  *q;
    size_t old;

    if (p == NULL) return malloc(n);
    old = ((struct block *) p - 1)->size;
    if (n <= old) return p;
    q = malloc(n);
    if (q == NULL) return NULL;
    memcpy(q, p, old);
    free(p);
    return q;
}

/* ------------------------------------------------------------------ */
/* strings                                                            */
/* ------------------------------------------------------------------ */

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;

    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;

    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else {
        while (n--) dp[n] = sp[n];
    }
    return d;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;

    while (n--) *dp++ = (unsigned char) c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;

    for (; n > 0; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;

    for (; n > 0; n--, p++)
        if (*p == (unsigned char) c) return (void *) p;
    return NULL;
}

size_t strlen(const char *s)
{
    const char *p = s;

    while (*p) p++;
    return (size_t) (p - s);
}

char *strcpy(char *d, const char *s)
{
    char *r = d;

    while ((*d++ = *s++) != '\0')
        ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n && s[i] != '\0'; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

char *strcat(char *d, const char *s)
{
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *e = d + strlen(d);

    while (n-- > 0 && *s != '\0') *e++ = *s++;
    *e = '\0';
    return d;
}

int strcmp(const char *a, const char *b)
{
    for (; *a == *b; a++, b++)
        if (*a == '\0') return 0;
    return (unsigned char) *a - (unsigned char) *b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b) return (unsigned char) *a - (unsigned char) *b;
        if (*a == '\0') return 0;
    }
    return 0;
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        int x = tolower((unsigned char) *a), y = tolower((unsigned char) *b);

        if (x != y) return x - y;
        if (x == '\0') return 0;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b)
{
    return strncasecmp(a, b, (size_t) -1);
}

int stricmp(const char *a, const char *b) { return strcasecmp(a, b); }

int strnicmp(const char *a, const char *b, size_t n)
{
    return strncasecmp(a, b, n);
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char) c) return (char *) s;
        if (*s == '\0') return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *r = NULL;

    for (;; s++) {
        if (*s == (char) c) r = s;
        if (*s == '\0') return (char *) r;
    }
}

char *strstr(const char *h, const char *n)
{
    size_t k = strlen(n);

    for (; *h != '\0' || k == 0; h++) {
        if (strncmp(h, n, k) == 0) return (char *) h;
        if (*h == '\0') break;
    }
    return NULL;
}

size_t strspn(const char *s, const char *set)
{
    size_t n = 0;

    while (s[n] != '\0' && strchr(set, s[n]) != NULL) n++;
    return n;
}

size_t strcspn(const char *s, const char *set)
{
    size_t n = 0;

    while (s[n] != '\0' && strchr(set, s[n]) == NULL) n++;
    return n;
}

char *strpbrk(const char *s, const char *set)
{
    s += strcspn(s, set);
    return *s != '\0' ? (char *) s : NULL;
}

char *strtok(char *s, const char *delim)
{
    static char *next;

    if (s == NULL) s = next;
    if (s == NULL) return NULL;
    s += strspn(s, delim);
    if (*s == '\0') {
        next = NULL;
        return NULL;
    }
    next = s + strcspn(s, delim);
    if (*next != '\0') *next++ = '\0';
    else next = NULL;
    return s;
}

char *strdup(const char *s)
{
    char *p = malloc(strlen(s) + 1);

    return p != NULL ? strcpy(p, s) : NULL;
}

char *strerror(int e)
{
    static char other[32];

    switch (e) {
    case 0:            return "no error";
    case EPERM:        return "operation not permitted";
    case ENOENT:       return "no such file or directory";
    case EINTR:        return "interrupted";
    case EIO:          return "input/output error";
    case EBADF:        return "bad file descriptor";
    case ENOMEM:       return "out of memory";
    case EACCES:       return "permission denied";
    case EEXIST:       return "file exists";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case EMFILE:       return "too many open files";
    case ENOTTY:       return "not a terminal";
    case ENOSPC:       return "no space left on device";
    case ESPIPE:       return "illegal seek";
    case EROFS:        return "read-only file system";
    case ERANGE:       return "result out of range";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS:       return "not implemented on UEFI";
    case ENOTEMPTY:    return "directory not empty";
    case ENETDOWN:     return "no network: no address yet (DHCP)";
    case ENETUNREACH:  return "network unreachable";
    case ECONNRESET:   return "connection reset";
    case ETIMEDOUT:    return "timed out";
    case ECONNREFUSED: return "connection refused";
    case EHOSTUNREACH: return "host unreachable";
    }
    sprintf(other, "error %d", e);
    return other;
}

int efi_errno(EFI_STATUS s)
{
    switch (s) {
    case EFI_NOT_FOUND:          return ENOENT;
    case EFI_ACCESS_DENIED:      return EACCES;
    case EFI_WRITE_PROTECTED:    return EROFS;
    case EFI_VOLUME_FULL:        return ENOSPC;
    case EFI_OUT_OF_RESOURCES:   return ENOMEM;
    case EFI_INVALID_PARAMETER:  return EINVAL;
    case EFI_UNSUPPORTED:        return ENOSYS;
    case EFI_NO_MAPPING:         return ENETDOWN;
    case EFI_TIMEOUT:            return ETIMEDOUT;
    case EFI_CONNECTION_RESET:   return ECONNRESET;
    case EFI_CONNECTION_REFUSED: return ECONNREFUSED;
    case EFI_NETWORK_UNREACHABLE: return ENETUNREACH;
    case EFI_HOST_UNREACHABLE:   return EHOSTUNREACH;
    case EFI_NO_MEDIA:           return ENOENT;
    }
    return EIO;
}

/* ------------------------------------------------------------------ */
/* ctype, for ASCII                                                   */
/* ------------------------------------------------------------------ */

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int isalpha(int c)  { return islower(c) || isupper(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') ||
                             (c >= 'A' && c <= 'F'); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c)  { return (c >= 0 && c < 32) || c == 127; }
int isprint(int c)  { return c >= 32 && c < 127; }
int isgraph(int c)  { return c > 32 && c < 127; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }
int toupper(int c)  { return islower(c) ? c - 32 : c; }

/* ------------------------------------------------------------------ */
/* numbers                                                            */
/* ------------------------------------------------------------------ */

int abs(int v)   { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x)
{
    double t;

    if (x >= 9007199254740992.0 || x <= -9007199254740992.0 || x != x)
        return x;
    t = (double) (long long) x;
    return t > x ? t - 1 : t;
}

double sqrt(double x)
{
    return __builtin_sqrt(x);           /* sqrtsd, no library call */
}

static unsigned long long to_ull(const char *s, char **end, int base,
                                 int *neg)
{
    unsigned long long v = 0;
    const char        *start;
    int                d, any = 0;

    while (isspace((unsigned char) *s)) s++;
    *neg = 0;
    if (*s == '-' || *s == '+') *neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X') && isxdigit((unsigned char) s[2])) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = *s == '0' ? 8 : 10;
    }
    start = s;
    for (;; s++) {
        if (isdigit((unsigned char) *s)) d = *s - '0';
        else if (isalpha((unsigned char) *s)) d = tolower(*s) - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned) base + (unsigned) d;
        any = 1;
    }
    if (end != NULL) *end = (char *) (any ? s : start);
    return v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg;
    unsigned long long v = to_ull(s, end, base, &neg);

    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg;
    unsigned long long v = to_ull(s, end, base, &neg);

    return neg ? -(long long) v : (long long) v;
}

long strtol(const char *s, char **end, int base)
{
    long long v = strtoll(s, end, base);

    if (v > LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    if (v < LONG_MIN) {
        errno = ERANGE;
        return LONG_MIN;
    }
    return (long) v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long) strtoull(s, end, base);
}

int atoi(const char *s)  { return (int) strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

/* x * 10^e, by the powers 10^1, 10^2, 10^4, ... 10^256 */
static double scale10(double x, int e)
{
    static const double p[] = { 1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64,
                                1e128, 1e256 };
    int i, neg = e < 0;

    if (neg) e = -e;
    for (i = 0; e != 0 && i < 9; i++, e >>= 1)
        if (e & 1) x = neg ? x / p[i] : x * p[i];
    if (e != 0) x = neg ? 0.0 : x * 1e256 * 1e256;
    return x;
}

double strtod(const char *s, char **end)
{
    const char        *p = s;
    unsigned long long m = 0;
    int                neg = 0, e = 0, digits = 0, any = 0;
    double             v;

    while (isspace((unsigned char) *p)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    for (; isdigit((unsigned char) *p); p++, any = 1) {
        if (digits < 19) {
            m = m * 10 + (unsigned) (*p - '0');
            if (m != 0) digits++;
        } else {
            e++;
        }
    }
    if (*p == '.') {
        for (p++; isdigit((unsigned char) *p); p++, any = 1) {
            if (digits < 19) {
                m = m * 10 + (unsigned) (*p - '0');
                if (m != 0) digits++;
                e--;
            }
        }
    }
    if (!any) {
        if (end != NULL) *end = (char *) s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int         en = 0, eneg = 0;

        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (isdigit((unsigned char) *q)) {
            while (isdigit((unsigned char) *q)) {
                if (en < 10000) en = en * 10 + (*q - '0');
                q++;
            }
            e += eneg ? -en : en;
            p = q;
        }
    }
    if (end != NULL) *end = (char *) p;
    v = scale10((double) m, e);
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, NULL); }

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* heap sort: no recursion, no extra memory */
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *))
{
    char  *a = base;
    size_t i, end, root, child;

    if (n < 2) return;
    for (i = n / 2; i-- > 0;) {
        for (root = i; (child = 2 * root + 1) < n; root = child) {
            if (child + 1 < n && cmp(a + child * size, a + (child + 1) * size) < 0)
                child++;
            if (cmp(a + root * size, a + child * size) >= 0) break;
            swap_bytes(a + root * size, a + child * size, size);
        }
    }
    for (end = n - 1; end > 0; end--) {
        swap_bytes(a, a + end * size, size);
        for (root = 0; (child = 2 * root + 1) < end; root = child) {
            if (child + 1 < end && cmp(a + child * size, a + (child + 1) * size) < 0)
                child++;
            if (cmp(a + root * size, a + child * size) >= 0) break;
            swap_bytes(a + root * size, a + child * size, size);
        }
    }
}

static unsigned long rand_state = 1;
int rand(void)
{
    rand_state = rand_state * 1103515245 + 12345;
    return (int) ((rand_state >> 16) & RAND_MAX);
}
void srand(unsigned seed) { rand_state = seed; }

static struct lconv c_locale = { "." };
struct lconv *localeconv(void) { return &c_locale; }
char *setlocale(int cat, const char *name)
{
    (void) cat;
    (void) name;
    return "C";
}

/* ------------------------------------------------------------------ */
/* UTF-8 and CHAR16                                                   */
/* ------------------------------------------------------------------ */

/* The next code point of UTF-8 at *s; a byte that is not UTF-8 stands
   for itself, as Latin-1. */
static unsigned long utf8_next(const unsigned char **s, const unsigned char *e)
{
    const unsigned char *p = *s;
    unsigned long        c = *p++;
    int                  more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    unsigned long        v = c & (0x3f >> more);
    int                  i;

    for (i = 0; i < more; i++) {
        if (p + i >= e || (p[i] & 0xc0) != 0x80) {
            *s = *s + 1;
            return c;
        }
        v = v << 6 | (p[i] & 0x3f);
    }
    *s = p + more;
    return more ? v : c;
}

/* n bytes of UTF-8 as CHAR16 into w (room for 2n + 1), \n as \r\n if
   `crlf`: the number of CHAR16 written */
static size_t to_wide(const char *s, size_t n, CHAR16 *w, int crlf)
{
    const unsigned char *p = (const unsigned char *) s, *e = p + n;
    size_t               k = 0;

    while (p < e) {
        unsigned long c = utf8_next(&p, e);

        if (c == '\n' && crlf) w[k++] = '\r';
        if (c >= 0x10000) {             /* outside the BMP: a surrogate pair */
            c -= 0x10000;
            w[k++] = (CHAR16) (0xd800 + (c >> 10));
            w[k++] = (CHAR16) (0xdc00 + (c & 0x3ff));
        } else {
            w[k++] = (CHAR16) c;
        }
    }
    w[k] = 0;
    return k;
}

CHAR16 *efi_wide(const char *s, int path)
{
    size_t  n = strlen(s), i;
    CHAR16 *w = malloc((2 * n + 1) * sizeof *w);

    if (w == NULL) return NULL;
    to_wide(s, n, w, 0);
    if (path)
        for (i = 0; w[i] != 0; i++)
            if (w[i] == '/') w[i] = '\\';
    return w;
}

/* CHAR16 as UTF-8 into out (room for 3 bytes each, and the NUL) */
static size_t to_narrow(const CHAR16 *w, size_t n, char *out)
{
    unsigned char *o = (unsigned char *) out;
    size_t         i;

    for (i = 0; i < n; i++) {
        unsigned long c = w[i];

        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n &&
            w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (w[i + 1] - 0xdc00);
            i++;
        }
        if (c < 0x80) {
            *o++ = (unsigned char) c;
        } else if (c < 0x800) {
            *o++ = (unsigned char) (0xc0 | c >> 6);
            *o++ = (unsigned char) (0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            *o++ = (unsigned char) (0xe0 | c >> 12);
            *o++ = (unsigned char) (0x80 | ((c >> 6) & 0x3f));
            *o++ = (unsigned char) (0x80 | (c & 0x3f));
        } else {
            *o++ = (unsigned char) (0xf0 | c >> 18);
            *o++ = (unsigned char) (0x80 | ((c >> 12) & 0x3f));
            *o++ = (unsigned char) (0x80 | ((c >> 6) & 0x3f));
            *o++ = (unsigned char) (0x80 | (c & 0x3f));
        }
    }
    *o = '\0';
    return (size_t) ((char *) o - out);
}

static size_t wlen(const CHAR16 *w)
{
    size_t n = 0;

    while (w[n] != 0) n++;
    return n;
}

char *efi_narrow(const CHAR16 *w)
{
    size_t n = wlen(w);
    char  *s = malloc(3 * n + 1);

    if (s != NULL) to_narrow(w, n, s);
    return s;
}

/* ------------------------------------------------------------------ */
/* descriptors                                                        */
/* ------------------------------------------------------------------ */

enum { K_FREE, K_CONIN, K_CONOUT, K_FILE, K_SOCK };

struct ofile {                  /* what a descriptor stands for */
    int               kind;
    int               refs;     /* descriptors that share it (dup) */
    SHELL_FILE_HANDLE h;
    int               append;   /* K_FILE: O_APPEND */
    int               sock;     /* K_SOCK: its number in net.c */
    char             *path;     /* K_FILE: for system()'s redirection */
};

#define NFD 32
static struct ofile *fds[NFD];
static struct ofile  con_in  = { K_CONIN, 1, NULL, 0, 0, NULL };
static struct ofile  con_out = { K_CONOUT, 1, NULL, 0, 0, NULL };
static struct ofile  con_err = { K_CONOUT, 1, NULL, 0, 0, NULL };

static int new_fd(struct ofile *o)
{
    int fd;

    for (fd = 0; fd < NFD; fd++)
        if (fds[fd] == NULL) {
            fds[fd] = o;
            return fd;
        }
    errno = EMFILE;
    return -1;
}

static struct ofile *get_fd(int fd)
{
    if (fd < 0 || fd >= NFD || fds[fd] == NULL) {
        errno = EBADF;
        return NULL;
    }
    return fds[fd];
}

/* For net.c: a descriptor for socket number `sock`, or -1. */
int efi_fd_new_socket(int sock)
{
    struct ofile *o = calloc(1, sizeof *o);
    int           fd;

    if (o == NULL) return -1;
    o->kind = K_SOCK;
    o->refs = 1;
    o->sock = sock;
    if ((fd = new_fd(o)) < 0) free(o);
    return fd;
}

/* The socket number behind fd, or -1. */
int efi_fd_socket(int fd)
{
    struct ofile *o = fd >= 0 && fd < NFD ? fds[fd] : NULL;

    return o != NULL && o->kind == K_SOCK ? o->sock : -1;
}

/* Write CHAR16 text to the console (or wherever the shell sent it). */
static void con_write_wide(struct ofile *o, CHAR16 *w, size_t k)
{
    SHELL_FILE_HANDLE h = params == NULL ? NULL :
                          o == &con_err ? params->StdErr : params->StdOut;
    UINTN             bytes = k * sizeof *w;

    if (h != NULL && efi_shell != NULL)
        efi_shell->WriteFile(h, &bytes, w);
    else
        efi_st->ConOut->OutputString(efi_st->ConOut, w);
}

static ssize_t con_write(struct ofile *o, const char *p, size_t n)
{
    CHAR16 buf[1025];
    size_t done = 0;

    /* in pieces that end on a character, at most 512 bytes each */
    while (done < n) {
        size_t part = n - done > 512 ? 512 : n - done;

        while (done + part < n && part > 0 &&
               ((unsigned char) p[done + part] & 0xc0) == 0x80)
            part--;
        if (part == 0) part = n - done > 512 ? 512 : n - done;
        con_write_wide(o, buf, to_wide(p + done, part, buf, 1));
        done += part;
    }
    return (ssize_t) n;
}

/* A line from the console, as UTF-8, kept until it is read. */
static ssize_t con_read(char *p, size_t n)
{
    static char   line[1024 * 3 + 2];
    static size_t have, at;
    CHAR16        w[1024];
    UINTN         bytes = sizeof w - sizeof *w;
    size_t        k;

    if (at == have) {
        if (params == NULL || efi_shell == NULL ||
            efi_shell->ReadFile(params->StdIn, &bytes, w) != EFI_SUCCESS)
            return 0;
        k = bytes / sizeof *w;
        if (k == 0) return 0;
        have = to_narrow(w, k, line);
        at = 0;
    }
    if (n > have - at) n = have - at;
    memcpy(p, line + at, n);
    at += n;
    return (ssize_t) n;
}

static int full_path(const char *path, char *out);

/* path as the shell takes it: absolute, "." and ".." out, backslashes */
static CHAR16 *wide_path(const char *path)
{
    char *p = malloc(4096);
    CHAR16 *w;

    if (p == NULL) return NULL;
    w = full_path(path, p) == 0 ? efi_wide(p, 1) : NULL;
    free(p);
    return w;
}

int open(const char *path, int flags, ...)
{
    struct ofile     *o;
    CHAR16           *w;
    SHELL_FILE_HANDLE h = NULL;
    EFI_FILE_INFO    *info;
    EFI_STATUS        s;
    int               fd, exists, wr = (flags & O_ACCMODE) != O_RDONLY;
    uint64_t          mode = EFI_FILE_MODE_READ | (wr ? EFI_FILE_MODE_WRITE : 0);

    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->OpenFileByName(w, &h, EFI_FILE_MODE_READ);
    exists = s == EFI_SUCCESS;
    if (exists) {
        info = efi_shell->GetFileInfo(h);
        if (info != NULL && (info->Attribute & EFI_FILE_DIRECTORY) && wr) {
            efi_bs->FreePool(info);
            efi_shell->CloseFile(h);
            free(w);
            errno = EISDIR;
            return -1;
        }
        if (info != NULL) efi_bs->FreePool(info);
        efi_shell->CloseFile(h);
        h = NULL;
    }
    if (exists && (flags & O_CREAT) && (flags & O_EXCL)) {
        free(w);
        errno = EEXIST;
        return -1;
    }
    if (!exists && !(flags & O_CREAT)) {
        free(w);
        errno = ENOENT;
        return -1;
    }
    if (exists && wr && (flags & O_TRUNC)) {
        s = efi_shell->DeleteFileByName(w);
        if (s != EFI_SUCCESS) {
            free(w);
            errno = efi_errno(s);
            return -1;
        }
        exists = 0;
    }
    s = exists ? efi_shell->OpenFileByName(w, &h, mode)
               : efi_shell->CreateFile(w, 0, &h);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    o = calloc(1, sizeof *o);
    if (o == NULL || (o->path = strdup(path)) == NULL) {
        free(o);
        efi_shell->CloseFile(h);
        return -1;
    }
    o->kind = K_FILE;
    o->refs = 1;
    o->h = h;
    o->append = (flags & O_APPEND) != 0;
    if ((fd = new_fd(o)) < 0) {
        efi_shell->CloseFile(h);
        free(o->path);
        free(o);
    }
    return fd;
}

ssize_t read(int fd, void *p, size_t n)
{
    struct ofile *o = get_fd(fd);
    UINTN         k = n;
    EFI_STATUS    s;

    if (o == NULL) return -1;
    switch (o->kind) {
    case K_CONIN:
        return con_read(p, n);
    case K_SOCK:
        return efi_sock_read(o->sock, p, n);
    case K_FILE:
        s = efi_shell->ReadFile(o->h, &k, p);
        if (s != EFI_SUCCESS) {
            errno = efi_errno(s);
            return -1;
        }
        return (ssize_t) k;
    }
    errno = EBADF;
    return -1;
}

ssize_t write(int fd, const void *p, size_t n)
{
    struct ofile *o = get_fd(fd);
    UINTN         k = n;
    EFI_STATUS    s;

    if (o == NULL) return -1;
    switch (o->kind) {
    case K_CONOUT:
        return con_write(o, p, n);
    case K_SOCK:
        return efi_sock_write(o->sock, p, n);
    case K_FILE:
        if (o->append) efi_shell->SetFilePosition(o->h, (uint64_t) -1);
        s = efi_shell->WriteFile(o->h, &k, (void *) p);
        if (s != EFI_SUCCESS) {
            errno = efi_errno(s);
            return -1;
        }
        return (ssize_t) k;
    }
    errno = EBADF;
    return -1;
}

int close(int fd)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL) return -1;
    fds[fd] = NULL;
    if (--o->refs > 0) return 0;
    if (o->kind == K_FILE) {
        efi_shell->CloseFile(o->h);
        free(o->path);
        free(o);
    } else if (o->kind == K_SOCK) {
        efi_sock_close(o->sock);
        free(o);
    }
    return 0;
}

int dup2(int fd, int to)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL || to < 0 || to >= NFD) {
        errno = EBADF;
        return -1;
    }
    if (fd == to) return to;
    if (fds[to] != NULL) close(to);
    fds[to] = o;
    o->refs++;
    return to;
}

int dup(int fd)
{
    struct ofile *o = get_fd(fd);
    int           to;

    if (o == NULL) return -1;
    if ((to = new_fd(o)) >= 0) o->refs++;
    return to;
}

off_t lseek(int fd, off_t off, int whence)
{
    struct ofile *o = get_fd(fd);
    uint64_t      pos = 0, size = 0;

    if (o == NULL) return -1;
    if (o->kind != K_FILE) {
        errno = ESPIPE;
        return -1;
    }
    if (whence == SEEK_CUR) efi_shell->GetFilePosition(o->h, &pos);
    if (whence == SEEK_END) efi_shell->GetFileSize(o->h, &size);
    pos = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? pos : size) + off;
    if (efi_shell->SetFilePosition(o->h, pos) != EFI_SUCCESS) {
        errno = EINVAL;
        return -1;
    }
    return (off_t) pos;
}

int fsync(int fd)
{
    struct ofile *o = get_fd(fd);

    if (o == NULL) return -1;
    if (o->kind == K_FILE) efi_shell->FlushFile(o->h);
    return 0;
}

int isatty(int fd)
{
    struct ofile *o = fd >= 0 && fd < NFD ? fds[fd] : NULL;

    return o != NULL && (o->kind == K_CONIN || o->kind == K_CONOUT);
}

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */

static volatile clock_t ticks;
static EFI_EVENT        tick_event;

static void EFIAPI on_tick(EFI_EVENT e, void *ctx)
{
    (void) e;
    (void) ctx;
    ticks++;
}

clock_t clock(void) { return ticks; }

/* days since 1970-01-01 of a civil date */
static long long days_from_civil(int y, int m, int d)
{
    long long era;
    unsigned  yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned) (y - era * 400);
    doy = (unsigned) ((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long) doe - 719468;
}

/* The firmware's clock, taken to be UTC.  ponytail: its TimeZone is not
   applied; most firmware leaves it unspecified anyway. */
time_t time(time_t *t)
{
    EFI_TIME now;
    time_t   v = 0;

    if (efi_st->RuntimeServices->GetTime(&now, NULL) == EFI_SUCCESS)
        v = (time_t) days_from_civil(now.Year, now.Month, now.Day) * 86400 +
            now.Hour * 3600 + now.Minute * 60 + now.Second;
    if (t != NULL) *t = v;
    return v;
}

double difftime(time_t a, time_t b) { return (double) (a - b); }

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    long long        z = *t / 86400, s = *t % 86400, era;
    unsigned         doe, yoe, doy, mp;

    if (s < 0) {
        s += 86400;
        z--;
    }
    tm.tm_hour = (int) (s / 3600);
    tm.tm_min = (int) (s / 60 % 60);
    tm.tm_sec = (int) (s % 60);
    tm.tm_wday = (int) ((z + 4) % 7 + 7) % 7;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned) (z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    tm.tm_mday = (int) (doy - (153 * mp + 2) / 5 + 1);
    tm.tm_mon = (int) (mp < 10 ? mp + 2 : mp - 10);
    tm.tm_year = (int) (yoe + era * 400 + (tm.tm_mon < 2)) - 1900;
    tm.tm_yday = (int) (days_from_civil(tm.tm_year + 1900, tm.tm_mon + 1,
                                        tm.tm_mday) -
                        days_from_civil(tm.tm_year + 1900, 1, 1));
    tm.tm_isdst = 0;
    return &tm;
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm)
{
    int y = tm->tm_year + 1900 + tm->tm_mon / 12, m = tm->tm_mon % 12;

    if (m < 0) {
        m += 12;
        y--;
    }
    return (time_t) days_from_civil(y, m + 1, 1) * 86400 +
           (long long) (tm->tm_mday - 1) * 86400 + tm->tm_hour * 3600 +
           tm->tm_min * 60 + tm->tm_sec;
}

size_t strftime(char *s, size_t n, const char *fmt, const struct tm *tm)
{
    /* only what an ISO date needs: %Y %m %d %H %M %S */
    size_t k = 0;

    for (; *fmt != '\0' && k + 1 < n; fmt++) {
        char part[16];
        int  v;

        if (*fmt != '%' || fmt[1] == '\0') {
            s[k++] = *fmt;
            continue;
        }
        switch (*++fmt) {
        case 'Y': v = tm->tm_year + 1900; sprintf(part, "%04d", v); break;
        case 'm': sprintf(part, "%02d", tm->tm_mon + 1); break;
        case 'd': sprintf(part, "%02d", tm->tm_mday); break;
        case 'H': sprintf(part, "%02d", tm->tm_hour); break;
        case 'M': sprintf(part, "%02d", tm->tm_min); break;
        case 'S': sprintf(part, "%02d", tm->tm_sec); break;
        default:  part[0] = *fmt; part[1] = '\0'; break;
        }
        if (k + strlen(part) + 1 > n) break;
        strcpy(s + k, part);
        k += strlen(part);
    }
    s[k] = '\0';
    return k;
}

unsigned sleep(unsigned secs)
{
    clock_t end = ticks + (clock_t) secs * CLOCKS_PER_SEC;

    while (ticks < end) {
        efi_net_poll();
        efi_bs->Stall(10000);
    }
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void) tz;
    tv->tv_sec = (long) time(NULL);
    tv->tv_usec = 0;
    return 0;
}

pid_t getpid(void) { return 1; }

/* ------------------------------------------------------------------ */
/* signals: none; Ctrl-C is the shell's ExecutionBreak event           */
/* ------------------------------------------------------------------ */

sighandler_t signal(int sig, sighandler_t fn)
{
    (void) sig;
    (void) fn;
    return SIG_DFL;
}

int sigaction(int sig, const struct sigaction *sa, struct sigaction *old)
{
    (void) sig;
    (void) sa;
    if (old != NULL) memset(old, 0, sizeof *old);
    return 0;
}

int sigemptyset(sigset_t *s)
{
    *s = 0;
    return 0;
}

int raise(int sig)
{
    exit(128 + sig);
}

int kill(pid_t pid, int sig)
{
    (void) pid;
    (void) sig;
    errno = ENOSYS;
    return -1;
}

/* Ctrl-C at the shell: end the program, as SIGINT would. */
void efi_check_break(void)
{
    if (efi_shell != NULL && efi_shell->ExecutionBreak != NULL &&
        efi_bs->CheckEvent(efi_shell->ExecutionBreak) == EFI_SUCCESS) {
        fputs("^C\n", stderr);
        exit(130);
    }
}

/* select() for reading: sockets, files and the console. */
int select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    clock_t end = 0;
    fd_set  out;
    int     fd, ready;

    (void) ex;
    if (tv != NULL)
        end = ticks + tv->tv_sec * CLOCKS_PER_SEC +
              tv->tv_usec / (1000000 / CLOCKS_PER_SEC);
    for (;;) {
        FD_ZERO(&out);
        ready = 0;
        efi_net_poll();
        for (fd = 0; fd < n && fd < NFD; fd++) {
            struct ofile *o = fds[fd];

            if (rd == NULL || !FD_ISSET(fd, rd) || o == NULL) continue;
            if (o->kind != K_SOCK || efi_sock_ready(o->sock)) {
                FD_SET(fd, &out);
                ready++;
            }
        }
        if (ready > 0 || (tv != NULL && ticks >= end)) break;
        efi_check_break();
        efi_bs->Stall(1000);
    }
    if (rd != NULL) *rd = out;
    if (wr != NULL) FD_ZERO(wr);
    return ready;
}

/* ------------------------------------------------------------------ */
/* paths: fs0:/dir/file                                               */
/* ------------------------------------------------------------------ */

#define PATHMAX 4096

/* The shell's current directory as fs0:/dir, into out[PATHMAX]. */
static int cwd(char *out)
{
    const CHAR16 *w = efi_shell != NULL ? efi_shell->GetCurDir(NULL) : NULL;
    char         *s, *p;

    if (w == NULL) {
        errno = ENOENT;                 /* no file system chosen yet */
        return -1;
    }
    if ((s = efi_narrow(w)) == NULL) return -1;
    if (strlen(s) >= PATHMAX) {
        free(s);
        errno = ENAMETOOLONG;
        return -1;
    }
    for (p = s; *p != '\0' && *p != ':'; p++)
        *p = (char) tolower((unsigned char) *p);
    for (; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
    strcpy(out, s);
    free(s);
    return 0;
}

char *getcwd(char *buf, size_t size)
{
    char p[PATHMAX];

    if (cwd(p) != 0) return NULL;
    if (strlen(p) >= size) {
        errno = ERANGE;
        return NULL;
    }
    return strcpy(buf, p);
}

int chdir(const char *path)
{
    (void) path;
    errno = ENOSYS;
    return -1;
}

/* path, absolute and with "." and ".." taken out, into out[PATHMAX]:
   map:/a/b.  Whether it exists is not checked. */
static int full_path(const char *path, char *out)
{
    char        in[PATHMAX], *p, *w, *start;
    const char *colon = strchr(path, ':');

    if (colon != NULL && strcspn(path, "/\\") > (size_t) (colon - path)) {
        if (strlen(path) >= PATHMAX) goto toolong;
        strcpy(in, path);
    } else {
        if (cwd(in) != 0) return -1;
        if (path[0] == '/' || path[0] == '\\') {    /* this map's root */
            in[strcspn(in, ":") + 1] = '\0';
        } else {
            if (strlen(in) + 1 >= PATHMAX) goto toolong;
            strcat(in, "/");
        }
        if (strlen(in) + strlen(path) >= PATHMAX) goto toolong;
        strcat(in, path);
    }
    for (p = in; *p != '\0'; p++)
        if (*p == '\\') *p = '/';

    /* the map's name in lower case, then one name at a time */
    for (p = in, w = out; *p != ':'; p++)
        *w++ = (char) tolower((unsigned char) *p);
    *w++ = ':';
    p++;
    start = w;
    while (*p != '\0') {
        size_t n;

        while (*p == '/') p++;
        n = strcspn(p, "/");
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (w > start && *--w != '/')
                ;
        } else if (n > 0 && !(n == 1 && p[0] == '.')) {
            *w++ = '/';
            memmove(w, p, n);
            w += n;
        }
        p += n;
    }
    if (w == start) *w++ = '/';
    *w = '\0';
    return 0;

toolong:
    errno = ENAMETOOLONG;
    return -1;
}

char *realpath(const char *path, char *out)
{
    char        p[PATHMAX];
    struct stat st;

    if (full_path(path, p) != 0) return NULL;
    if (stat(p, &st) != 0) return NULL;     /* ENOENT if nothing is there */
    if (out == NULL) return strdup(p);
    return strcpy(out, p);
}

/* ------------------------------------------------------------------ */
/* files by name                                                      */
/* ------------------------------------------------------------------ */

static time_t efi_to_time(const EFI_TIME *t)
{
    if (t->Year < 1970) return 0;
    return (time_t) days_from_civil(t->Year, t->Month, t->Day) * 86400 +
           t->Hour * 3600 + t->Minute * 60 + t->Second;
}

static void fill_stat(const EFI_FILE_INFO *info, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = (info->Attribute & EFI_FILE_DIRECTORY) ? S_IFDIR | 0755
                  : (info->Attribute & EFI_FILE_READ_ONLY) ? S_IFREG | 0444
                  : S_IFREG | 0644;
    st->st_size = (off_t) info->FileSize;
    st->st_mtime = efi_to_time(&info->ModificationTime);
    st->st_atime = efi_to_time(&info->LastAccessTime);
    st->st_ctime = efi_to_time(&info->CreateTime);
    st->st_nlink = 1;
}

/* Open by name for reading: 0, or -1 with errno. */
static int open_name(const char *path, SHELL_FILE_HANDLE *h, uint64_t mode)
{
    CHAR16    *w;
    EFI_STATUS s;

    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->OpenFileByName(w, h, mode);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return 0;
}

int stat(const char *path, struct stat *st)
{
    SHELL_FILE_HANDLE h;
    EFI_FILE_INFO    *info;

    if (open_name(path, &h, EFI_FILE_MODE_READ) != 0) return -1;
    info = efi_shell->GetFileInfo(h);
    efi_shell->CloseFile(h);
    if (info == NULL) {
        errno = EIO;
        return -1;
    }
    fill_stat(info, st);
    efi_bs->FreePool(info);
    return 0;
}

int lstat(const char *path, struct stat *st) { return stat(path, st); }

int fstat(int fd, struct stat *st)
{
    struct ofile  *o = get_fd(fd);
    EFI_FILE_INFO *info;

    if (o == NULL) return -1;
    memset(st, 0, sizeof *st);
    if (o->kind != K_FILE) {
        st->st_mode = S_IFCHR | 0600;
        return 0;
    }
    if ((info = efi_shell->GetFileInfo(o->h)) == NULL) {
        errno = EIO;
        return -1;
    }
    fill_stat(info, st);
    efi_bs->FreePool(info);
    return 0;
}

int access(const char *path, int mode)
{
    struct stat st;

    (void) mode;
    return stat(path, &st);
}

int chmod(const char *path, mode_t mode)
{
    (void) path;
    (void) mode;
    return 0;                   /* FAT keeps no modes */
}

int fchmod(int fd, mode_t mode)
{
    (void) fd;
    (void) mode;
    return 0;
}

mode_t umask(mode_t m)
{
    (void) m;
    return 022;
}

int mkdir(const char *path, mode_t mode)
{
    SHELL_FILE_HANDLE h;
    CHAR16           *w;
    EFI_STATUS        s;
    struct stat       st;

    (void) mode;
    if (stat(path, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->CreateFile(w, EFI_FILE_DIRECTORY, &h);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    efi_shell->CloseFile(h);
    return 0;
}

int remove(const char *path)
{
    CHAR16    *w;
    EFI_STATUS s;

    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((w = wide_path(path)) == NULL) return -1;
    s = efi_shell->DeleteFileByName(w);
    free(w);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return 0;
}

int unlink(const char *path) { return remove(path); }
int rmdir(const char *path)  { return remove(path); }

/* rename() where the firmware renames nothing (U-Boot): a copy, then
   the old one removed. */
static int copy_over(const char *from, const char *to)
{
    char   buf[4096];
    int    in, out, e = 0;
    ssize_t n;

    if ((in = open(from, O_RDONLY)) < 0) return -1;
    if ((out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
        e = errno;
        close(in);
        errno = e;
        return -1;
    }
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t) n) != n) {
            e = errno ? errno : ENOSPC;
            break;
        }
    if (n < 0) e = errno;
    close(in);
    close(out);
    if (e != 0) {
        remove(to);
        errno = e;
        return -1;
    }
    return remove(from);
}

/* The new name goes in the file's information: as \dir\name, from the
   root of its file system, so both must be on the same one.  An existing
   `to` is removed first, so this is not atomic. */
int rename(const char *from, const char *to)
{
    char              f[PATHMAX], t[PATHMAX];
    SHELL_FILE_HANDLE h;
    EFI_FILE_INFO    *info, *ninfo;
    CHAR16           *w;
    struct stat       st;
    size_t            mf, mt, wn;
    EFI_STATUS        s;

    if (full_path(from, f) != 0 || full_path(to, t) != 0) return -1;
    mf = strcspn(f, ":");
    mt = strcspn(t, ":");
    if (mf != mt || strncmp(f, t, mf) != 0) {
        errno = EINVAL;                 /* another file system */
        return -1;
    }
    if (strcasecmp(f, t) == 0) return 0;
    if (open_name(f, &h, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE) != 0)
        return -1;
    if (stat(t, &st) == 0 && remove(t) != 0) {
        efi_shell->CloseFile(h);
        return -1;
    }
    info = efi_shell->GetFileInfo(h);
    w = efi_wide(t + mt + 1, 1);
    if (info == NULL || w == NULL) {
        if (info != NULL) efi_bs->FreePool(info);
        free(w);
        efi_shell->CloseFile(h);
        errno = ENOMEM;
        return -1;
    }
    wn = wlen(w);
    ninfo = malloc(sizeof *ninfo + wn * sizeof *w);
    if (ninfo == NULL) {
        efi_bs->FreePool(info);
        free(w);
        efi_shell->CloseFile(h);
        return -1;
    }
    memcpy(ninfo, info, sizeof *ninfo);
    memcpy(ninfo->FileName, w, (wn + 1) * sizeof *w);
    ninfo->Size = sizeof *ninfo + wn * sizeof *w;
    s = efi_shell->SetFileInfo(h, ninfo);
    efi_bs->FreePool(info);
    free(ninfo);
    free(w);
    efi_shell->CloseFile(h);
    if (s == EFI_ACCESS_DENIED || s == EFI_UNSUPPORTED || s == EFI_WRITE_PROTECTED)
        return copy_over(f, t);
    if (s != EFI_SUCCESS) {
        errno = efi_errno(s);
        return -1;
    }
    return 0;
}

/* tmpnam(): in HOME, the directory gork.efi is in */
char *tmpnam(char *s)
{
    static char   name[L_tmpnam];
    static int    n;
    const char   *home = getenv("HOME");
    struct stat   st;

    if (s == NULL) s = name;
    if (home == NULL || strlen(home) + 16 >= L_tmpnam) return NULL;
    do
        sprintf(s, "%s%sgork%04d.tmp", home,
                home[strlen(home) - 1] == '/' ? "" : "/", n++ % 10000);
    while (stat(s, &st) == 0 && n < 10000);
    return s;
}

/* ------------------------------------------------------------------ */
/* directories                                                        */
/* ------------------------------------------------------------------ */

struct _DIR {
    SHELL_FILE_HANDLE    h;
    EFI_SHELL_FILE_INFO *list;
    EFI_LIST_ENTRY      *at;
    struct dirent        ent;
};

DIR *opendir(const char *path)
{
    DIR        *d;
    struct stat st;
    EFI_STATUS  s;

    if (stat(path, &st) != 0) return NULL;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return NULL;
    }
    if ((d = calloc(1, sizeof *d)) == NULL) return NULL;
    if (open_name(path, &d->h, EFI_FILE_MODE_READ) != 0) {
        free(d);
        return NULL;
    }
    s = efi_shell->FindFilesInDir(d->h, &d->list);
    if (s != EFI_SUCCESS) {
        efi_shell->CloseFile(d->h);
        free(d);
        errno = efi_errno(s);
        return NULL;
    }
    d->at = d->list != NULL ? d->list->Link.Flink : NULL;
    return d;
}

struct dirent *readdir(DIR *d)
{
    EFI_SHELL_FILE_INFO *f;
    char                *name;

    if (d->list == NULL || d->at == &d->list->Link) return NULL;
    f = (EFI_SHELL_FILE_INFO *) d->at;
    d->at = d->at->Flink;
    name = efi_narrow(f->FileName);
    if (name == NULL) return NULL;
    strncpy(d->ent.d_name, name, sizeof d->ent.d_name - 1);
    d->ent.d_name[sizeof d->ent.d_name - 1] = '\0';
    free(name);
    return &d->ent;
}

int closedir(DIR *d)
{
    if (d->list != NULL) efi_shell->FreeFileList(&d->list);
    efi_shell->CloseFile(d->h);
    free(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the environment: the shell's variables                             */
/* ------------------------------------------------------------------ */

#define NENV 32
static char *env_set[NENV];             /* putenv()'s, "NAME=value" */
static char *env_got[NENV];             /* what getenv() handed out */
static char  home_dir[PATHMAX];         /* where gork.efi is */

char *getenv(const char *name)
{
    size_t        n = strlen(name);
    const CHAR16 *v;
    CHAR16       *w;
    char         *s;
    int           i;

    for (i = 0; i < NENV; i++)
        if (env_set[i] != NULL && strncmp(env_set[i], name, n) == 0 &&
            env_set[i][n] == '=')
            return env_set[i] + n + 1;
    v = NULL;
    if (efi_shell != NULL && (w = efi_wide(name, 0)) != NULL) {
        v = efi_shell->GetEnv(w);
        free(w);
    }
    if (v == NULL)
        return strcmp(name, "HOME") == 0 && home_dir[0] != '\0' ? home_dir
                                                                : NULL;
    if ((s = efi_narrow(v)) == NULL) return NULL;
    /* the same name gets the same slot, so asking again leaks nothing */
    for (i = 0; i < NENV; i++)
        if (env_got[i] != NULL && strncmp(env_got[i], name, n) == 0 &&
            env_got[i][n] == '=')
            break;
    if (i == NENV)
        for (i = 0; i < NENV && env_got[i] != NULL; i++)
            ;
    if (i == NENV) i = 0;
    free(env_got[i]);
    env_got[i] = malloc(n + 1 + strlen(s) + 1);
    if (env_got[i] == NULL) {
        free(s);
        return NULL;
    }
    sprintf(env_got[i], "%s=%s", name, s);
    free(s);
    return env_got[i] + n + 1;
}

int putenv(char *s)
{
    size_t n = strcspn(s, "=");
    int    i, free_slot = -1;

    for (i = 0; i < NENV; i++) {
        if (env_set[i] != NULL && strncmp(env_set[i], s, n) == 0 &&
            env_set[i][n] == '=') {
            env_set[i] = s;
            return 0;
        }
        if (env_set[i] == NULL && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        errno = ENOMEM;
        return -1;
    }
    env_set[free_slot] = s;
    return 0;
}

/* ------------------------------------------------------------------ */
/* system(): the shell runs the line                                  */
/* ------------------------------------------------------------------ */

/* " >>a \"fs0:\dir\file\"" at out: `op` and path as the shell's parser
   takes them */
static void shell_path(const char *path, char *out, const char *op)
{
    char  p[PATHMAX], *c;

    if (full_path(path, p) != 0) strcpy(p, path);
    for (c = p; *c != '\0'; c++)
        if (*c == '/') *c = '\\';
    sprintf(out, "%s\"%s\"", op, p);
}

/* Where fd 1 points at a file (as run_command points 1 and 2), the
   shell is told to send the command's output there, as ASCII.  It will
   not send stderr to the same file, so that goes to a second one, added
   to the first at the end. */
int system(const char *cmd)
{
    struct ofile *o1 = fds[1], *o2 = fds[2];
    char         *line, err[L_tmpnam];
    CHAR16       *w;
    EFI_STATUS    s, status = EFI_SUCCESS;
    int           to_file = o1 != NULL && o1->kind == K_FILE;
    int           both = to_file && o2 == o1 && tmpnam(err) != NULL;
    size_t        n;

    if (cmd == NULL) return efi_shell != NULL;
    if (efi_shell == NULL) {
        errno = ENOSYS;
        return -1;
    }
    n = strlen(cmd) + 64 + (to_file ? 2 * PATHMAX : 0);
    if ((line = malloc(n)) == NULL) return -1;
    /* EDK2 runs the line as `Shell.efi -exit LINE`: a shell of its own,
       which would show its banner and the mapping table first */
    strcpy(line, "-nomap -noversion -nostartup ");
    strcat(line, cmd);
    if (to_file) {
        /* the command writes the file itself: let go of it meanwhile */
        efi_shell->CloseFile(o1->h);
        o1->h = NULL;
        shell_path(o1->path, line + strlen(line), " >>a ");
        if (both) shell_path(err, line + strlen(line), " 2>a ");
    }
    w = efi_wide(line, 0);
    free(line);
    if (w == NULL) return -1;
    s = efi_shell->Execute(&efi_image, w, NULL, &status);
    free(w);
    if (to_file &&
        open_name(o1->path, &o1->h, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE)
            == 0) {
        /* back to the end of the file, then what went to stderr */
        efi_shell->SetFilePosition(o1->h, (uint64_t) -1);
        if (both) {
            FILE  *f = fopen(err, "r");
            char   buf[1024];
            size_t k;

            while (f != NULL && (k = fread(buf, 1, sizeof buf, f)) > 0)
                write(1, buf, k);
            if (f != NULL) fclose(f);
        }
    }
    if (both) remove(err);
    if (s != EFI_SUCCESS) return 1;
    return EFI_ERROR(status) ? (int) (status & 0xff) : (int) status;
}

/* ------------------------------------------------------------------ */
/* printf                                                             */
/* ------------------------------------------------------------------ */

struct out {
    char  *p;           /* to memory, if not NULL */
    size_t size, len;   /* room in p; characters produced */
    FILE  *f;           /* else to a stream */
};

static void out_put(struct out *o, const char *s, size_t n)
{
    if (o->p != NULL) {
        size_t i;

        for (i = 0; i < n; i++)
            if (o->len + i + 1 < o->size) o->p[o->len + i] = s[i];
    } else if (o->f != NULL) {
        fwrite(s, 1, n, o->f);
    }
    o->len += n;
}

static void out_pad(struct out *o, char c, int n)
{
    while (n-- > 0) out_put(o, &c, 1);
}

/* x >= 0 as decimal digits: d[0..n), point after `*exp10 + 1` digits */
static int float_digits(double x, char *d, int want, int *exp10)
{
    int e = 0, i;

    if (x == 0) {
        for (i = 0; i < want; i++) d[i] = '0';
        *exp10 = 0;
        return want;
    }
    while (x >= 1e16) { x /= 1e16; e += 16; }
    while (x >= 10)   { x /= 10;   e++; }
    while (x < 1e-16) { x *= 1e16; e -= 16; }
    while (x < 1)     { x *= 10;   e--; }
    for (i = 0; i < want; i++) {
        int v = (int) x;

        if (v > 9) v = 9;
        d[i] = (char) ('0' + v);
        x = (x - v) * 10;
    }
    if (x >= 5) {                       /* round the last digit */
        for (i = want - 1; i >= 0 && d[i] == '9'; i--) d[i] = '0';
        if (i >= 0) {
            d[i]++;
        } else {
            memmove(d + 1, d, (size_t) want - 1);
            d[0] = '1';
            e++;
        }
    }
    *exp10 = e;
    return want;
}

/* %e %f %g of a finite x >= 0 into s (room for 400) */
static size_t fmt_float(char *s, double x, char conv, int prec, int alt)
{
    char d[48];
    int  e, i, n = 0, ndig, fixed, trim = 0;

    if (prec < 0) prec = 6;
    if (conv == 'g' || conv == 'G') {
        if (prec == 0) prec = 1;
        if (prec > 40) prec = 40;
        float_digits(x, d, prec, &e);
        fixed = e >= -4 && e < prec;
        prec = fixed ? prec - 1 - e : prec - 1;
        trim = !alt;
        conv = fixed ? 'f' : (conv == 'g' ? 'e' : 'E');
        if (conv == 'f') {
            ndig = e + 1 + prec;
        } else {
            ndig = prec + 1;
        }
    } else if (conv == 'f' || conv == 'F') {
        if (prec > 40) prec = 40;
        float_digits(x, d, 1, &e);              /* just the exponent */
        ndig = e + 1 + prec;
        if (ndig > 40) ndig = 40;
        if (ndig < 1) ndig = 0;
        if (ndig > 0) float_digits(x, d, ndig, &e);
        if (e + 1 + prec != ndig && ndig > 0)   /* rounding carried */
            ndig = e + 1 + prec > 40 ? 40 : e + 1 + prec;
    } else {
        if (prec > 40) prec = 40;
        ndig = prec + 1;
        float_digits(x, d, ndig, &e);
    }

    if (conv == 'f' || conv == 'F') {
        int point = e + 1;                      /* digits before the point */

        if (ndig <= 0) {
            /* rounds to zero */
            s[n++] = '0';
            if (prec > 0 || alt) s[n++] = '.';
            for (i = 0; i < prec; i++) s[n++] = '0';
        } else {
            if (point <= 0) {
                s[n++] = '0';
            } else {
                for (i = 0; i < point; i++) s[n++] = i < ndig ? d[i] : '0';
            }
            if (prec > 0 || alt) s[n++] = '.';
            for (i = 0; i < prec; i++) {
                int k = point + i;

                s[n++] = k < 0 ? '0' : k < ndig ? d[k] : '0';
            }
        }
        if (trim && memchr(s, '.', (size_t) n) != NULL) {
            while (s[n - 1] == '0') n--;
            if (s[n - 1] == '.') n--;
        }
    } else {
        s[n++] = d[0];
        if (prec > 0 || alt) s[n++] = '.';
        for (i = 1; i <= prec; i++) s[n++] = d[i];
        if (trim && prec > 0) {
            while (s[n - 1] == '0') n--;
            if (s[n - 1] == '.') n--;
        }
        s[n++] = conv;
        s[n++] = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        if (e >= 100) s[n++] = (char) ('0' + e / 100);
        s[n++] = (char) ('0' + e / 10 % 10);
        s[n++] = (char) ('0' + e % 10);
    }
    return (size_t) n;
}

static int format(struct out *o, const char *f, va_list ap)
{
    for (; *f != '\0'; f++) {
        char        num[440], *body;
        const char *pre = "";
        int         left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        int         width = 0, prec = -1, lng = 0, neg = 0;
        size_t      n;
        unsigned long long u;
        int         base = 10, upper = 0;

        if (*f != '%') {
            const char *e = strchr(f, '%');

            n = e != NULL ? (size_t) (e - f) : strlen(f);
            out_put(o, f, n);
            f += n - 1;
            continue;
        }
        for (f++;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        if (*f == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            f++;
        } else {
            while (isdigit((unsigned char) *f)) width = width * 10 + *f++ - '0';
        }
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') {
                prec = va_arg(ap, int);
                f++;
            } else {
                while (isdigit((unsigned char) *f)) prec = prec * 10 + *f++ - '0';
            }
        }
        for (;; f++) {
            if (*f == 'l') lng++;
            else if (*f == 'z' || *f == 'j' || *f == 't') lng = 2;
            else if (*f == 'h' || *f == 'L') ;
            else break;
        }
        body = num + sizeof num;
        n = 0;
        switch (*f) {
        case '\0':
            return 0;
        case '%':
            out_put(o, "%", 1);
            continue;
        case 'c':
            num[0] = (char) va_arg(ap, int);
            body = num;
            n = 1;
            prec = -1;
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);

            if (s == NULL) s = "(null)";
            if (prec >= 0) {
                const char *z = memchr(s, '\0', (size_t) prec);
                n = z != NULL ? (size_t) (z - s) : (size_t) prec;
            } else {
                n = strlen(s);
            }
            if (!left) out_pad(o, ' ', width - (int) n);
            out_put(o, s, n);
            if (left) out_pad(o, ' ', width - (int) n);
            continue;
        }
        case 'd': case 'i': {
            long long v = lng >= 2 ? va_arg(ap, long long)
                        : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);

            neg = v < 0;
            u = neg ? -(unsigned long long) v : (unsigned long long) v;
            goto integer;
        }
        case 'p':
            u = (unsigned long long) (uintptr_t) va_arg(ap, void *);
            base = 16;
            pre = "0x";
            goto integer;
        case 'o': base = 8;  goto unsig;
        case 'X': upper = 1; /* FALLTHROUGH */
        case 'x': base = 16; /* FALLTHROUGH */
        case 'u':
        unsig:
            u = lng >= 2 ? va_arg(ap, unsigned long long)
              : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (alt && base == 16 && u != 0) pre = upper ? "0X" : "0x";
            if (alt && base == 8) pre = "0";
        integer:
            do {
                int dgt = (int) (u % (unsigned) base);

                *--body = (char) (dgt < 10 ? '0' + dgt
                                           : (upper ? 'A' : 'a') + dgt - 10);
                u /= (unsigned) base;
            } while (u != 0);
            n = (size_t) (num + sizeof num - body);
            if (prec == 0 && n == 1 && *body == '0') n = 0;
            if (neg) pre = "-";
            else if (plus && (*f == 'd' || *f == 'i')) pre = "+";
            else if (space && (*f == 'd' || *f == 'i')) pre = " ";
            break;
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double x = va_arg(ap, double);

            if (x < 0 || (x == 0 && 1 / x < 0)) {
                neg = 1;
                x = -x;
            }
            pre = neg ? "-" : plus ? "+" : space ? " " : "";
            if (x != x) {
                body = "nan";
                n = 3;
            } else if (x > 1.7976931348623157e308) {
                body = "inf";
                n = 3;
            } else {
                body = num;
                n = fmt_float(num, x, *f, prec, alt);
            }
            prec = -1;
            break;
        }
        default:
            out_put(o, f - 1, 2);
            continue;
        }

        /* sign or prefix, zeros of precision, digits, within width */
        {
            int digits = (int) n, zeros = 0, total;

            if (prec > digits) zeros = prec - digits;
            total = (int) strlen(pre) + zeros + digits;
            if (zero && !left && prec < 0 && width > total) {
                zeros += width - total;
                total = width;
            }
            if (!left) out_pad(o, ' ', width - total);
            out_put(o, pre, strlen(pre));
            out_pad(o, '0', zeros);
            out_put(o, body, n);
            if (left) out_pad(o, ' ', width - total);
        }
    }
    return 0;
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
    struct out o;

    o.p = s;
    o.size = n;
    o.len = 0;
    o.f = NULL;
    format(&o, fmt, ap);
    if (n > 0) s[o.len < n ? o.len : n - 1] = '\0';
    return (int) o.len;
}

int vsprintf(char *s, const char *fmt, va_list ap)
{
    return vsnprintf(s, (size_t) INT_MAX, fmt, ap);
}

int snprintf(char *s, size_t n, const char *fmt, ...)
{
    va_list ap;
    int     r;

    va_start(ap, fmt);
    r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *s, const char *fmt, ...)
{
    va_list ap;
    int     r;

    va_start(ap, fmt);
    r = vsnprintf(s, (size_t) INT_MAX, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct out o;

    o.p = NULL;
    o.size = o.len = 0;
    o.f = f;
    format(&o, fmt, ap);
    return (int) o.len;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int     r;

    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int printf(const char *fmt, ...)
{
    va_list ap;
    int     r;

    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* Enough for cJSON's check of a number it printed: %lg, %d, %u, %x. */
int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int     got = 0;
    char   *end;

    va_start(ap, fmt);
    for (; *fmt != '\0'; fmt++) {
        int lng = 0;

        if (*fmt != '%') {
            if (isspace((unsigned char) *fmt)) {
                while (isspace((unsigned char) *s)) s++;
            } else if (*s++ != *fmt) {
                break;
            }
            continue;
        }
        fmt++;
        while (*fmt == 'l') {
            lng++;
            fmt++;
        }
        if (*fmt == 'g' || *fmt == 'f' || *fmt == 'e') {
            double v = strtod(s, &end);

            if (end == s) break;
            if (lng) *va_arg(ap, double *) = v;
            else *va_arg(ap, float *) = (float) v;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            long v = strtol(s, &end, *fmt == 'x' ? 16 : 10);

            if (end == s) break;
            if (lng) *va_arg(ap, long *) = v;
            else *va_arg(ap, int *) = (int) v;
        } else {
            break;
        }
        s = end;
        got++;
    }
    va_end(ap);
    return got;
}

/* ------------------------------------------------------------------ */
/* stdio streams                                                      */
/* ------------------------------------------------------------------ */

struct _FILE {
    int    fd;
    int    eof, err;
    int    unget;               /* a character pushed back, or EOF */
    int    linebuf;             /* flush at each newline: the console */
    char   buf[BUFSIZ];
    size_t len, at;             /* write: len bytes buffered; read: buf[at..len) */
    int    reading;
    int    text;                /* CR LF in the file, \n here, as on DOS */
    int    cr;                  /* text: a CR held back at the end of buf */
    int    started;             /* text: the first read is done */
    int    utf16;               /* text: the file is UTF-16 (FF FE first),
                                   as the shell's `>` writes: read as UTF-8 */
    int    odd;                 /* utf16: a byte held back, or -1 */
    CHAR16 high;                /* utf16: half a surrogate pair, or 0 */
    FILE  *next;
};

static FILE  f_in, f_out, f_err;
FILE        *stdin = &f_in, *stdout = &f_out, *stderr = &f_err;
static FILE *streams;

static FILE *new_stream(int fd)
{
    FILE *f = calloc(1, sizeof *f);

    if (f == NULL) return NULL;
    f->fd = fd;
    f->unget = EOF;
    f->next = streams;
    streams = f;
    return f;
}

/* Text mode, unless the mode says "b", for files: not the console, whose
   layer adds the CRs itself. */
static FILE *set_mode(FILE *f, const char *mode)
{
    if (f != NULL)
        f->text = strchr(mode, 'b') == NULL && fds[f->fd]->kind == K_FILE;
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    if (get_fd(fd) == NULL) return NULL;
    return set_mode(new_stream(fd), mode);
}

FILE *fopen(const char *path, const char *mode)
{
    int   flags, fd;
    FILE *f;

    switch (mode[0]) {
    case 'r': flags = strchr(mode, '+') ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default:
        errno = EINVAL;
        return NULL;
    }
    if (mode[0] == 'r') {
        struct stat st;

        /* a directory reads as its entries: refuse it, as Unix does */
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            errno = EISDIR;
            return NULL;
        }
    }
    if ((fd = open(path, flags, 0666)) < 0) return NULL;
    if ((f = set_mode(new_stream(fd), mode)) == NULL) close(fd);
    return f;
}

int fflush(FILE *f)
{
    size_t done = 0;

    if (f == NULL) {
        FILE *g;

        fflush(stdout);
        fflush(stderr);
        for (g = streams; g != NULL; g = g->next) fflush(g);
        return 0;
    }
    if (f->reading) return 0;
    while (done < f->len) {
        ssize_t w = write(f->fd, f->buf + done, f->len - done);

        if (w <= 0) {
            f->err = 1;
            f->len = 0;
            return EOF;
        }
        done += (size_t) w;
    }
    f->len = 0;
    return 0;
}

int fclose(FILE *f)
{
    FILE **pp;
    int    r = fflush(f);

    if (close(f->fd) != 0) r = EOF;
    for (pp = &streams; *pp != NULL; pp = &(*pp)->next)
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    if (f != stdin && f != stdout && f != stderr) free(f);
    return r;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    const char *s = p;
    size_t      total = size * n, i;

    if (total == 0) return 0;
    if (f->reading) {
        f->reading = 0;
        f->len = f->at = 0;
    }
    for (i = 0; i < total; i++) {
        if (f->text && s[i] == '\n') {
            if (f->len + 1 >= sizeof f->buf && fflush(f) != 0) return i / size;
            f->buf[f->len++] = '\r';
        }
        f->buf[f->len++] = s[i];
        if (f->len == sizeof f->buf || (f->linebuf && s[i] == '\n'))
            if (fflush(f) != 0) return i / size;
    }
    if (f == stderr && fflush(f) != 0) return 0;
    return n;
}

/* n bytes of UTF-16LE from a file, as UTF-8 into out[size]: the bytes
   made.  A byte or a half surrogate pair at the end waits in f. */
static ssize_t utf16_to_utf8(FILE *f, const char *raw, size_t n, char *out,
                             size_t size)
{
    CHAR16 w[BUFSIZ / 3];
    size_t k = 0, i = 0;

    (void) size;            /* callers read at most size / 3 * 2 bytes */
    if (f->high != 0) {
        w[k++] = f->high;
        f->high = 0;
    }
    if (f->odd >= 0 && n > 0) {
        w[k++] = (CHAR16) (f->odd | (unsigned char) raw[0] << 8);
        f->odd = -1;
        i = 1;
    }
    for (; i + 1 < n; i += 2)
        w[k++] = (CHAR16) ((unsigned char) raw[i] |
                           (unsigned char) raw[i + 1] << 8);
    if (i < n) f->odd = (unsigned char) raw[i];
    if (k > 0 && w[k - 1] >= 0xd800 && w[k - 1] < 0xdc00) f->high = w[--k];
    return (ssize_t) to_narrow(w, k, out);
}

static ssize_t read_utf16(FILE *f, char *out, size_t size)
{
    char    raw[BUFSIZ / 3 * 2 - 4];
    ssize_t r = read(f->fd, raw, sizeof raw);

    if (r <= 0) return r;
    r = utf16_to_utf8(f, raw, (size_t) r, out, size);
    return r > 0 ? r : read_utf16(f, out, size);
}

static int fill(FILE *f)
{
    ssize_t r;

    if (!f->reading) {
        fflush(f);
        f->reading = 1;
        f->len = f->at = 0;
    }
    if (f->at < f->len) return 0;
    if (f == stdin) fflush(stdout);
    if (!f->text) {
        r = read(f->fd, f->buf, sizeof f->buf);
        if (r <= 0) {
            if (r < 0) f->err = 1;
            else f->eof = 1;
            return EOF;
        }
        f->len = (size_t) r;
        f->at = 0;
        return 0;
    }

    /* text: CR LF to \n.  buf[0] is kept for a CR held back from the
       end of the last read, until it is known whether \n follows. */
    for (;;) {
        size_t i, k = 0, start = f->cr ? 0 : 1;

        if (!f->started) {
            /* two bytes first: FF FE starts UTF-16, as the shell's `>`
               writes it, which is then read as UTF-8 */
            f->started = 1;
            r = read(f->fd, f->buf + 1, 2);
            if (r == 2 && (unsigned char) f->buf[1] == 0xff &&
                (unsigned char) f->buf[2] == 0xfe) {
                f->utf16 = 1;
                f->odd = -1;
                continue;
            }
        } else if (f->utf16) {
            r = read_utf16(f, f->buf + 1, sizeof f->buf - 1);
        } else {
            r = read(f->fd, f->buf + 1, sizeof f->buf - 1);
        }
        if (r < 0) {
            f->err = 1;
            return EOF;
        }
        if (r == 0) {
            if (!f->cr) {
                f->eof = 1;
                return EOF;
            }
            f->cr = 0;                  /* a CR at the very end */
            f->buf[0] = '\r';
            f->len = 1;
            f->at = 0;
            return 0;
        }
        f->buf[0] = '\r';
        f->cr = 0;
        for (i = start; i < (size_t) r + 1; i++) {
            if (f->buf[i] == '\r') {
                if (i == (size_t) r) {
                    f->cr = 1;          /* is \n next?  the next read says */
                    break;
                }
                if (f->buf[i + 1] == '\n') continue;
            }
            f->buf[k++] = f->buf[i];
        }
        if (k > 0) {
            f->len = k;
            f->at = 0;
            return 0;
        }
    }
}

int fgetc(FILE *f)
{
    if (f->unget != EOF) {
        int c = f->unget;

        f->unget = EOF;
        return c;
    }
    if (fill(f) != 0) return EOF;
    return (unsigned char) f->buf[f->at++];
}

int getc(FILE *f)    { return fgetc(f); }
int getchar(void)    { return fgetc(stdin); }

int ungetc(int c, FILE *f)
{
    if (c == EOF) return EOF;
    f->unget = c;
    f->eof = 0;
    return c;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    char  *d = p;
    size_t total = size * n, got = 0;

    if (total == 0) return 0;
    if (f->unget != EOF) {
        d[got++] = (char) f->unget;
        f->unget = EOF;
    }
    while (got < total) {
        size_t k;

        if (fill(f) != 0) break;
        k = f->len - f->at;
        if (k > total - got) k = total - got;
        memcpy(d + got, f->buf + f->at, k);
        f->at += k;
        got += k;
    }
    return got / size;
}

char *fgets(char *s, int n, FILE *f)
{
    int i = 0, c = 0;

    if (n <= 0) return NULL;
    while (i < n - 1 && (c = fgetc(f)) != EOF) {
        s[i++] = (char) c;
        if (c == '\n') break;
    }
    if (i == 0 && c == EOF) return NULL;
    s[i] = '\0';
    return s;
}

int fputc(int c, FILE *f)
{
    char ch = (char) c;

    return fwrite(&ch, 1, 1, f) == 1 ? (unsigned char) c : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);

    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s)
{
    return fputs(s, stdout) == 0 && fputc('\n', stdout) != EOF ? 0 : EOF;
}

int fseek(FILE *f, long off, int whence)
{
    if (whence == SEEK_CUR && f->reading) off -= (long) (f->len - f->at);
    fflush(f);
    f->reading = 0;
    f->len = f->at = 0;
    f->unget = EOF;
    f->eof = 0;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
    off_t pos = lseek(f->fd, 0, SEEK_CUR);

    if (pos < 0) return -1;
    if (f->reading) pos -= (off_t) (f->len - f->at);
    else pos += (off_t) f->len;
    return (long) pos;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    f->err = 0;
}

int feof(FILE *f)      { return f->eof; }
int ferror(FILE *f)    { return f->err; }
void clearerr(FILE *f) { f->eof = f->err = 0; }
int fileno(FILE *f)    { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    (void) buf;
    (void) size;
    fflush(f);
    f->linebuf = mode != _IOFBF;
    return 0;
}

void perror(const char *s)
{
    if (s != NULL && *s != '\0') fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errno));
}

/* ------------------------------------------------------------------ */
/* start and end                                                      */
/* ------------------------------------------------------------------ */

#define NATEXIT 8
static void (*at_exit[NATEXIT])(void);
static int    nat_exit;

int atexit(void (*fn)(void))
{
    if (nat_exit == NATEXIT) return -1;
    at_exit[nat_exit++] = fn;
    return 0;
}

void efi_net_end(void);

void exit(int status)
{
    int fd;

    while (nat_exit > 0) at_exit[--nat_exit]();
    fflush(NULL);
    for (fd = 3; fd < NFD; fd++)
        if (fds[fd] != NULL) close(fd);
    efi_net_end();
    if (tick_event != NULL) efi_bs->CloseEvent(tick_event);
    while (blocks != NULL) free(blocks + 1);
    /* the shell shows a nonzero status in %lasterror% */
    efi_bs->Exit(efi_image, (EFI_STATUS) (unsigned) status, 0, NULL);
    for (;;)
        ;
}

void abort(void) { exit(134); }

/* What the shell set the program going with: argv as UTF-8. */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    static EFI_GUID shell_guid  = EFI_SHELL_PROTOCOL_GUID;
    static EFI_GUID params_guid = EFI_SHELL_PARAMETERS_PROTOCOL_GUID;
    char          **argv;
    int             argc, i;

    efi_image = image;
    efi_st = st;
    efi_bs = st->BootServices;
    efi_bs->SetWatchdogTimer(0, 0, 0, NULL);
    if (efi_bs->HandleProtocol(image, &params_guid, (void **) &params)
            != EFI_SUCCESS ||
        efi_bs->LocateProtocol(&shell_guid, NULL, (void **) &efi_shell)
            != EFI_SUCCESS) {
        static CHAR16 msg[] = L"gork: start it from the UEFI shell\r\n";

        st->ConOut->OutputString(st->ConOut, msg);
        return EFI_UNSUPPORTED;
    }
    efi_bs->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, on_tick,
                        NULL, &tick_event);
    efi_bs->SetTimer(tick_event, TimerPeriodic, 10000000 / CLOCKS_PER_SEC);

    fds[0] = &con_in;
    fds[1] = &con_out;
    fds[2] = &con_err;
    f_in.fd = 0;
    f_out.fd = 1;
    f_err.fd = 2;
    f_in.unget = f_out.unget = f_err.unget = EOF;
    f_out.linebuf = 1;

    argc = (int) params->Argc;
    argv = calloc((size_t) argc + 1, sizeof *argv);
    if (argv == NULL) exit(1);
    for (i = 0; i < argc; i++)
        if ((argv[i] = efi_narrow(params->Argv[i])) == NULL) exit(1);

    /* HOME, unless the shell sets one: the directory gork.efi is in */
    if (argc > 0) {
        char  dir[PATHMAX];
        char *slash;

        if (full_path(argv[0], dir) == 0 &&
            (slash = strrchr(dir, '/')) != NULL) {
            slash[slash == strchr(dir, '/')] = '\0';    /* keep "fs0:/" */
            strcpy(home_dir, dir);
        }
    }
    exit(main(argc, argv));
}
