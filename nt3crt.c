/*
 * nt3crt.c -- for gorknt3.exe, the Windows build that uses crtdll.dll.
 *
 * Windows NT 3.1 and 3.5 have no msvcrt.dll, and the one of a later
 * Windows does not load there.  They have crtdll.dll, its ancestor, and
 * MinGW can link against it; but MinGW's own library still calls four
 * functions by the names msvcrt.dll gave them.  Here they are, on top of
 * crtdll's.  Only _stat needs more than a new name: crtdll's struct stat
 * has a 16-bit st_dev, so the fields sit elsewhere.
 */
#include <string.h>

struct crt_stat {               /* crtdll's struct stat */
    short          st_dev;
    unsigned short st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    short          st_rdev;
    long           st_size;
    long           st_atime;
    long           st_mtime;
    long           st_ctime;
};

struct msv_stat {               /* msvcrt's struct _stat32 */
    unsigned int   st_dev;
    unsigned short st_ino;
    unsigned short st_mode;
    short          st_nlink;
    short          st_uid;
    short          st_gid;
    unsigned int   st_rdev;
    long           st_size;
    long           st_atime;
    long           st_mtime;
    long           st_ctime;
};

/* crtdll's functions, under names of our own.  No header is included for
   the four below: they declare them as imports from a DLL. */
extern long crt_time(long *) __asm__("_time");
extern long crt_findfirst(const char *, void *) __asm__("__findfirst");
extern int  crt_findnext(long, void *) __asm__("__findnext");
extern int  crt_stat(const char *, struct crt_stat *) __asm__("__stat");

long _time32(long *t)
{
    long now = crt_time(NULL);

    if (t != NULL) *t = now;
    return now;
}

/* the find data is laid out the same in both */
long _findfirst32(const char *name, void *d)
{
    return crt_findfirst(name, d);
}

int _findnext32(long h, void *d)
{
    return crt_findnext(h, d);
}

int _stat32(const char *path, struct msv_stat *out)
{
    struct crt_stat s;

    if (crt_stat(path, &s) != 0) return -1;
    memset(out, 0, sizeof *out);
    out->st_dev   = (unsigned int) s.st_dev;
    out->st_ino   = s.st_ino;
    out->st_mode  = s.st_mode;
    out->st_nlink = s.st_nlink;
    out->st_uid   = s.st_uid;
    out->st_gid   = s.st_gid;
    out->st_rdev  = (unsigned int) s.st_rdev;
    out->st_size  = s.st_size;
    out->st_atime = s.st_atime;
    out->st_mtime = s.st_mtime;
    out->st_ctime = s.st_ctime;
    return 0;
}
