/*
 * fwlibc.c -- the part of the C library of gork's firmware ports (UEFI,
 * barebox) that does not depend on the firmware: strings, ctype,
 * numbers, printf and the stdio streams.  The port's own libc.c gives it
 * the descriptors (open, read, write, lseek, close), errno, and the
 * fw_* calls of fwlibc.h; on UEFI, also the UTF-16 files the shell's `>`
 * writes and text mode (CR LF in files).
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fwlibc.h"

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
    uint16_t high;              /* utf16: half a surrogate pair, or 0 */
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
        f->text = strchr(mode, 'b') == NULL && fw_text_fd(f->fd);
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    if (!fw_fd_valid(fd)) return NULL;
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

#ifdef GORK_EFI
/* n bytes of UTF-16LE from a file, as UTF-8 into out[size]: the bytes
   made.  A byte or a half surrogate pair at the end waits in f. */
static ssize_t utf16_to_utf8(FILE *f, const char *raw, size_t n, char *out,
                             size_t size)
{
    uint16_t w[BUFSIZ / 3];
    size_t k = 0, i = 0;

    (void) size;            /* callers read at most size / 3 * 2 bytes */
    if (f->high != 0) {
        w[k++] = f->high;
        f->high = 0;
    }
    if (f->odd >= 0 && n > 0) {
        w[k++] = (uint16_t) (f->odd | (unsigned char) raw[0] << 8);
        f->odd = -1;
        i = 1;
    }
    for (; i + 1 < n; i += 2)
        w[k++] = (uint16_t) ((unsigned char) raw[i] |
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

#endif

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

#ifdef GORK_EFI
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
        } else
#endif
        {
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


/* The standard streams, on descriptors 0, 1 and 2: before anything else. */
void fw_stdio_init(void)
{
    f_in.fd = 0;
    f_out.fd = 1;
    f_err.fd = 2;
    f_in.unget = f_out.unget = f_err.unget = EOF;
    f_out.linebuf = 1;
}
