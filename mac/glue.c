/*
 * glue.c -- the Mac half of gork for the classic Mac OS (System 7, 68k).
 *
 * gork's half (main.c, relay.c, notui.c, cJSON, fwlibc.c and
 * barebox/libc.c) is built against fwinclude/ with every C library name
 * made gork_NAME, as for barebox and GRUB (barebox/bb.h).  This half is
 * built with Retro68's headers and C library, and gives gork's the calls
 * of barebox/bb.h on the Toolbox: the File Manager, the clock, the console
 * window (console.c), and in place of gork's own TCP/IP (ip.c) the calls
 * of ip.h on MacTCP, which Open Transport also answers.
 *
 * The program is a console window with a prompt: each line is a gork
 * command line, run as gork would run from a shell, or `set NAME=value`,
 * `unset NAME` or `quit`.  A file gork.in beside the program is read
 * instead of the keyboard, and what the window shows is written to
 * gork.out too: so the tests run it with no one at the keyboard.
 *
 * Paths reach gork as /Volume/Folder/file, and the File Manager as full
 * path names, Volume:Folder:file; a `/` in a Mac name is a `:` to gork.
 */
#include <Multiverse.h>         /* all of the Toolbox, MacTCP too */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "roman.h"

/* gork's errno values (Linux's, as fwinclude/ has them) */
#define E_NOENT 2
#define E_IO 5
#define E_BADF 9
#define E_NOMEM 12
#define E_ACCES 13
#define E_BUSY 16
#define E_EXIST 17
#define E_XDEV 18
#define E_NOTDIR 20
#define E_ISDIR 21
#define E_INVAL 22
#define E_MFILE 24
#define E_NOSPC 28
#define E_ROFS 30
#define E_NAMETOOLONG 36
#define E_NOSYS 38
#define E_NOTEMPTY 39
#define E_NETDOWN 100
#define E_CONNRESET 104
#define E_TIMEDOUT 110
#define E_CONNREFUSED 111
#define E_HOSTUNREACH 113

/* and what Multiversal leaves out */
#ifndef fsRtDirID
#define fsRtDirID 2
#endif
#define AFP_ACCESS_DENIED (-5000)
#define ON_SYSTEM_DISK ((short) -32768)

#define O_ACC 0x0003
#define O_WR 0x0001
#define O_RW 0x0002
#define O_CRE 0x0040
#define O_EXC 0x0080
#define O_TRU 0x0200
#define O_APP 0x0400

/* barebox/bb.h, in plain C types */
void *bb_malloc(size_t n);
void *bb_realloc(void *p, size_t n);
void bb_free(void *p);
int bb_open(const char *path, int flags, unsigned mode);
long bb_read(int fd, void *p, size_t n);
long bb_write(int fd, const void *p, size_t n);
int bb_close(int fd);
long long bb_lseek(int fd, long long off, int whence);
int bb_stat(const char *path, int follow, int *isdir, long long *size);
int bb_mkdir(const char *path);
int bb_unlink(const char *path);
int bb_rmdir(const char *path);
int bb_rename(const char *from, const char *to);
const char *bb_getcwd(void);
int bb_chdir(const char *path);
void *bb_opendir(const char *path);
const char *bb_readdir(void *d);
void bb_closedir(void *d);
const char *bb_getenv(const char *name);
int bb_setenv(const char *name, const char *value);
const char *bb_home(void);
const char *bb_tmpdir(void);
void bb_console_write(const char *p, size_t n);
int bb_console_getc(void);
int bb_ctrlc(void);
unsigned long long bb_ms(void);
long long bb_epoch(void);
void bb_udelay(unsigned us);
int bb_run(const char *line, char **out, size_t *len);
void bb_exit(int status) __attribute__((noreturn));

/* ip.h, on MacTCP */
int ip_start(void);
void ip_poll(void);
int ip_resolve(const char *name, unsigned char *addr);
int ip_tcp_connect(const unsigned char *addr, unsigned port);
int ip_tcp_write(const void *p, size_t n);
int ip_tcp_read(void *p, size_t n);
int ip_tcp_ready(void);
void ip_tcp_close(void);
void card_pause(void);
void card_break(void);

/* gork's half */
int gork_start(int argc, char **argv);  /* barebox/libc.c: then main() */
void gork_exit(int status) __attribute__((noreturn));
extern int gork_errno;

/* What Multiversal leaves out: traps of System 7's File Manager and the
   parameter RAM's location record. */
#pragma parameter __D0 PBDirCreateSyncG(__A0)
pascal OSErr PBDirCreateSyncG(HParmBlkPtr pb) = {0x7006, 0xA260};
#pragma parameter __D0 PBCatMoveSyncG(__A0)
pascal OSErr PBCatMoveSyncG(CMovePBPtr pb) = {0x7005, 0xA260};
#pragma parameter __D0 PBHSetFInfoSyncG(__A0)
pascal OSErr PBHSetFInfoSyncG(HParmBlkPtr pb) = {0xA20D};

typedef struct {
  long latitude, longitude;
  long gmtFlags;                /* gmtDelta in the low 24 bits */
} Location;
pascal void ReadLocationG(Location *loc) =
  {0x205F, 0x203C, 0x000C, 0x00E4, 0xA051};

/* ------------------------------------------------------------------ */
/* the transcript: the window, and gork.out when a script runs        */
/* ------------------------------------------------------------------ */

static int transcript_fd = -1;
long bb_write(int fd, const void *p, size_t n);

static void out(const char *p, size_t n)
{
  con_write(p, n);
  if (transcript_fd >= 0) bb_write(transcript_fd, p, n);
}

static void outs(const char *s) { out(s, strlen(s)); }

void bb_console_write(const char *p, size_t n)
{
  out(p, n);
}

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

void *bb_malloc(size_t n) { return malloc(n); }
void *bb_realloc(void *p, size_t n) { return realloc(p, n); }
void bb_free(void *p) { free(p); }

/* ------------------------------------------------------------------ */
/* paths                                                              */
/* ------------------------------------------------------------------ */

#define PATHMAX 1024
static char cwd[PATHMAX] = "/";
static char app_dir[PATHMAX] = "/";

/* /Vol/a/b made absolute against cwd, with . and .. gone */
static int abs_path(const char *path, char *buf)
{
  char tmp[PATHMAX], *seg[PATHMAX / 2], *p, *s;
  size_t n = 0, k;

  if (path[0] == '/') {
    if (strlen(path) >= sizeof tmp) return -E_NAMETOOLONG;
    strcpy(tmp, path);
  } else {
    if (strlen(cwd) + 1 + strlen(path) >= sizeof tmp) return -E_NAMETOOLONG;
    snprintf(tmp, sizeof tmp, "%s/%s", cwd, path);
  }
  for (p = tmp; (s = strtok(p, "/")) != NULL; p = NULL) {
    if (strcmp(s, ".") == 0) continue;
    if (strcmp(s, "..") == 0) {
      if (n > 0) n--;
      continue;
    }
    seg[n++] = s;
  }
  buf[0] = '\0';
  for (k = 0; k < n; k++) {
    strcat(buf, "/");
    strcat(buf, seg[k]);
  }
  if (n == 0) strcpy(buf, "/");
  return 0;
}

/* Names on the Mac are in Mac Roman, and gork's in UTF-8: a character of
   gork's path in Mac Roman (`?` for what it lacks, or for what is not
   UTF-8), the path's / and the Mac's : changing places; and back. */
static char path_char(const char **ps)
{
  const unsigned char *s = (const unsigned char *) *ps;
  unsigned long c = *s++;
  int k;

  if (c >= 0xC0 && c < 0xF8) {
    k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    c &= 0x3F >> k;
    for (; k > 0 && (*s & 0xC0) == 0x80; k--) c = c << 6 | (*s++ & 0x3F);
    if (k > 0) c = '?';
  } else if (c >= 0x80)
    c = '?';
  *ps = (const char *) s;
  return c == '/' ? ':' : c == ':' ? '/' : roman_from_unicode(c);
}

/* a Mac name (a Pascal string) as gork's, in UTF-8: its length.  out
   takes 3 bytes for each of the name's and a '\0'. */
static size_t gork_name(const unsigned char *pn, char *out)
{
  size_t n = 0;
  int i;

  for (i = 1; i <= pn[0]; i++)
    n += roman_to_utf8(pn[i] == '/' ? ':' : pn[i], out + n);
  out[n] = '\0';
  return n;
}

/* gork's path as a full path name for the File Manager, a Pascal
   string: Vol:a:b, and Vol: for a volume.  -E_NOENT for "/", which is
   no place on a disk; -E_NAMETOOLONG past the File Manager's 255. */
static int mac_path(const char *path, Str255 out)
{
  char full[PATHMAX];
  const char *p;
  int r, n = 0;

  if ((r = abs_path(path, full)) < 0) return r;
  if (strcmp(full, "/") == 0) return -E_NOENT;
  for (p = full + 1; *p;) {
    if (n >= 255) return -E_NAMETOOLONG;
    if (*p == '/') {
      out[++n] = ':';
      p++;
    } else
      out[++n] = path_char(&p);
  }
  if (strchr(full + 1, '/') == NULL) {         /* a volume: Vol: */
    if (n >= 255) return -E_NAMETOOLONG;
    out[++n] = ':';
  }
  out[0] = n;
  return 0;
}

/* the last name of a path, as a Mac name */
static void base_name(const char *full, Str255 out)
{
  const char *b = strrchr(full, '/') + 1;
  int n = 0;

  while (*b && n < 255) out[++n] = path_char(&b);
  out[0] = n;
}

static int mac_err(OSErr e)
{
  switch (e) {
  case noErr: return 0;
  case fnfErr: case dirNFErr: case nsvErr: case bdNamErr: return -E_NOENT;
  case dupFNErr: return -E_EXIST;
  case fBsyErr: return -E_NOTEMPTY;
  case opWrErr: case permErr: case AFP_ACCESS_DENIED: return -E_ACCES;
  case wPrErr: case vLckdErr: case fLckdErr: return -E_ROFS;
  case dskFulErr: return -E_NOSPC;
  case tmfoErr: return -E_MFILE;
  case memFullErr: return -E_NOMEM;
  case badMovErr: return -E_INVAL;
  default: return -E_IO;
  }
}

/* what is at a path: 0, or -errno */
static int cat_info(const char *path, CInfoPBRec *pb, Str255 name)
{
  int r = mac_path(path, name);

  if (r < 0) return r;
  memset(pb, 0, sizeof *pb);
  pb->hFileInfo.ioNamePtr = name;
  return mac_err(PBGetCatInfoSync(pb));
}

static int is_dir(const CInfoPBRec *pb)
{
  return (pb->hFileInfo.ioFlAttrib & 0x10) != 0;
}

/* the volume a full path name is on, for indexed calls */
static short vol_ref(ConstStr255Param name)
{
  HParamBlockRec pb;
  Str255 vol;
  int n = 0;

  while (n < name[0] && name[n + 1] != ':') {
    vol[n + 1] = name[n + 1];
    n++;
  }
  vol[++n] = ':';
  vol[0] = n;
  memset(&pb, 0, sizeof pb);
  pb.volumeParam.ioNamePtr = vol;
  pb.volumeParam.ioVolIndex = -1;
  return PBHGetVInfoSync(&pb) == noErr ? pb.volumeParam.ioVRefNum : 0;
}

/* the full path of a folder, from its volume and ID */
static void dir_path(short vref, long dir, char *buf)
{
  char tmp[PATHMAX], name[3 * 63 + 1];
  CInfoPBRec pb;
  Str63 pn;

  buf[0] = '\0';
  for (;;) {
    memset(&pb, 0, sizeof pb);
    pb.dirInfo.ioNamePtr = pn;
    pb.dirInfo.ioVRefNum = vref;
    pb.dirInfo.ioFDirIndex = -1;
    pb.dirInfo.ioDrDirID = dir;
    if (PBGetCatInfoSync(&pb) != noErr) break;
    gork_name(pn, name);
    if (strlen(name) + 1 + strlen(buf) >= sizeof tmp) break;
    snprintf(tmp, sizeof tmp, "/%s%s", name, buf);
    strcpy(buf, tmp);
    if (dir == fsRtDirID) break;
    dir = pb.dirInfo.ioDrParID;
  }
  if (buf[0] == '\0') strcpy(buf, "/");
}

/* ------------------------------------------------------------------ */
/* files                                                              */
/* ------------------------------------------------------------------ */

#define NFILE 16
static struct {
  short ref;                    /* the File Manager's, 0 if free */
  int append;
  int text;                     /* a TEXT file: Mac Roman and CR */
  char pend[3];                 /* read: UTF-8 not yet given */
  int npend;
  unsigned long wc;             /* write: a UTF-8 character so far */
  int wneed;                    /* its bytes still to come */
} files[NFILE];

/* A Mac text file is in Mac Roman, its lines ending in CR; gork's text is
   UTF-8, its lines ending in LF.  A TEXT file is turned from the one to
   the other as it is read and written.  CR and LF change places, so a
   file with both comes back as it was; and each of Mac Roman's 256
   characters is one in UTF-8, so a file read and written again is the
   same.  What Mac Roman has no character for is written as `?`.  Sizes
   and offsets are the Mac's, in bytes of Mac Roman. */
static char swap_crlf(char c)
{
  return c == '\r' ? '\n' : c == '\n' ? '\r' : c;
}

static void text_reset(int fd)
{
  files[fd].npend = 0;
  files[fd].wneed = 0;
}

static long text_read(int fd, char *p, size_t n)
{
  char src[256], u[3];
  long k;
  size_t out = 0;
  int i, j, m;
  OSErr e;

  while (out < n && files[fd].npend > 0) {      /* what was left over */
    p[out++] = files[fd].pend[0];
    memmove(files[fd].pend, files[fd].pend + 1, (size_t) --files[fd].npend);
  }
  if (out > 0) return (long) out;
  /* as many bytes as surely fit, three of UTF-8 each; one at least */
  k = (long) (n / 3 < sizeof src ? n / 3 : sizeof src);
  if (k == 0) k = 1;
  e = FSRead(files[fd].ref, &k, src);
  if (e != noErr && e != eofErr) return mac_err(e);
  for (i = 0; i < k; i++) {
    m = roman_to_utf8((unsigned char) swap_crlf(src[i]), u);
    for (j = 0; j < m; j++)
      if (out < n) p[out++] = u[j];
      else files[fd].pend[files[fd].npend++] = u[j];
  }
  return (long) out;
}

static OSErr put_bytes(int fd, const char *b, long m)
{
  if (files[fd].append) SetFPos(files[fd].ref, fsFromLEOF, 0);
  return FSWrite(files[fd].ref, &m, b);
}

static long text_write(int fd, const char *p, size_t n)
{
  char buf[256];
  unsigned char c;
  long m = 0;
  size_t i;
  OSErr e;

#define EMIT(ch) do { \
    buf[m++] = swap_crlf(ch); \
    if (m == (long) sizeof buf) { \
      if ((e = put_bytes(fd, buf, m)) != noErr) return mac_err(e); \
      m = 0; \
    } \
  } while (0)
  for (i = 0; i < n; i++) {
    c = (unsigned char) p[i];
    if (files[fd].wneed > 0) {
      if ((c & 0xC0) == 0x80) {
        files[fd].wc = files[fd].wc << 6 | (c & 0x3F);
        if (--files[fd].wneed == 0) EMIT(roman_from_unicode(files[fd].wc));
        continue;
      }
      files[fd].wneed = 0;      /* one cut short */
      EMIT('?');
    }
    if (c < 0x80)
      EMIT((char) c);
    else if (c >= 0xC0 && c < 0xF8) {
      files[fd].wneed = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      files[fd].wc = c & (0x3F >> files[fd].wneed);
    } else
      EMIT('?');                /* not UTF-8 */
  }
  if (m > 0 && (e = put_bytes(fd, buf, m)) != noErr) return mac_err(e);
#undef EMIT
  return (long) n;
}

int bb_open(const char *path, int flags, unsigned mode)
{
  CInfoPBRec pb;
  Str255 name;
  short ref;
  char perm;
  OSErr e;
  int i, r;

  (void) mode;
  for (i = 0; i < NFILE && files[i].ref != 0; i++) {}
  if (i == NFILE) return -E_MFILE;
  r = cat_info(path, &pb, name);
  if (r == 0 && is_dir(&pb))
    return -E_ISDIR;            /* opendir() reads folders, not open() */
  if (r == 0 && (flags & O_CRE) && (flags & O_EXC)) return -E_EXIST;
  if (r == -E_NOENT && (flags & O_CRE)) {
    /* a text file, as SimpleText makes them */
    e = HCreate(0, 0, name, 'ttxt', 'TEXT');
    if (e != noErr) return mac_err(e);
    files[i].text = 1;
    text_reset(i);
  } else if (r < 0)
    return r;
  else
    files[i].text = pb.hFileInfo.ioFlFndrInfo.fdType == 'TEXT';
  text_reset(i);
  perm = (flags & O_ACC) == O_RW ? fsRdWrPerm :
         (flags & O_ACC) == O_WR ? fsWrPerm : fsRdPerm;
  e = HOpenDF(0, 0, name, perm, &ref);
  if (e != noErr) return mac_err(e);
  if ((flags & O_TRU) && (flags & O_ACC) != 0) SetEOF(ref, 0);
  files[i].ref = ref;
  files[i].append = (flags & O_APP) != 0;
  return i;
}

static short file_ref(int fd)
{
  return fd >= 0 && fd < NFILE ? files[fd].ref : 0;
}

long bb_read(int fd, void *p, size_t n)
{
  short ref = file_ref(fd);
  long k = (long) n;
  OSErr e;

  if (ref == 0) return -E_BADF;
  if (files[fd].text) return text_read(fd, p, n);
  e = FSRead(ref, &k, p);
  if (e != noErr && e != eofErr) return mac_err(e);
  return k;
}

long bb_write(int fd, const void *p, size_t n)
{
  short ref = file_ref(fd);
  long k = (long) n;
  OSErr e;

  if (ref == 0) return -E_BADF;
  if (files[fd].append) SetFPos(ref, fsFromLEOF, 0);
  if (files[fd].text) return text_write(fd, p, n);
  e = FSWrite(ref, &k, p);
  if (e != noErr) return mac_err(e);
  return k;
}

int bb_close(int fd)
{
  short ref = file_ref(fd), vref = 0;

  if (ref == 0) return -E_BADF;
  if (files[fd].text && files[fd].wneed > 0) {  /* one cut short */
    files[fd].wneed = 0;
    put_bytes(fd, "?", 1);
  }
  files[fd].ref = 0;
  GetVRefNum(ref, &vref);
  FSClose(ref);
  FlushVol(NULL, vref);         /* on the disk, should the Mac crash */
  return 0;
}

long long bb_lseek(int fd, long long off, int whence)
{
  short ref = file_ref(fd);
  long base = 0, eof, pos;
  OSErr e;

  if (ref == 0) return -E_BADF;
  if (whence == 1 && GetFPos(ref, &base) != noErr) return -E_IO;
  if (whence == 2 && GetEOF(ref, &base) != noErr) return -E_IO;
  pos = base + (long) off;
  if (pos < 0) return -E_INVAL;
  if (!(whence == 1 && off == 0)) text_reset(fd);   /* not just asked */
  if (GetEOF(ref, &eof) == noErr && pos > eof) SetEOF(ref, pos);
  e = SetFPos(ref, fsFromStart, pos);
  return e == noErr ? pos : mac_err(e);
}

int bb_stat(const char *path, int follow, int *isdir, long long *size)
{
  char full[PATHMAX];
  CInfoPBRec pb;
  Str255 name;
  int r;

  (void) follow;
  if ((r = abs_path(path, full)) < 0) return r;
  if (strcmp(full, "/") == 0) {         /* the volumes' list */
    *isdir = 1;
    *size = 0;
    return 0;
  }
  if ((r = cat_info(full, &pb, name)) < 0) return r;
  *isdir = is_dir(&pb);
  *size = *isdir ? 0 : pb.hFileInfo.ioFlLgLen;
  return 0;
}

int bb_mkdir(const char *path)
{
  HParamBlockRec pb;
  Str255 name;
  int r = mac_path(path, name);

  if (r < 0) return r == -E_NOENT ? -E_EXIST : r;
  memset(&pb, 0, sizeof pb);
  pb.fileParam.ioNamePtr = name;
  return mac_err(PBDirCreateSyncG(&pb));
}

static int remove_at(const char *path, int want_dir)
{
  CInfoPBRec pb;
  Str255 name;
  int r = cat_info(path, &pb, name);

  if (r < 0) return r;
  if (is_dir(&pb) && !want_dir) return -E_ISDIR;
  if (!is_dir(&pb) && want_dir) return -E_NOTDIR;
  return mac_err(HDelete(0, 0, name));
}

int bb_unlink(const char *path) { return remove_at(path, 0); }
int bb_rmdir(const char *path) { return remove_at(path, 1); }

/* rename(), over what is there as POSIX has it; to another folder by
   CatMove, on the same volume only */
int bb_rename(const char *from, const char *to)
{
  char f[PATHMAX], t[PATHMAX], fdir[PATHMAX], tdir[PATHMAX];
  Str255 fname, tname, newname, oldname, dname;
  CInfoPBRec pb, tpb;
  CMovePBRec mv;
  short vref;
  long parid;
  int r;

  if ((r = abs_path(from, f)) < 0 || (r = abs_path(to, t)) < 0) return r;
  if ((r = cat_info(f, &pb, fname)) < 0) return r;
  if ((r = mac_path(t, tname)) < 0) return r;
  if (vol_ref(fname) != vol_ref(tname)) return -E_XDEV;
  if (strcmp(f, t) == 0) return 0;
  if (strncmp(t, f, strlen(f)) == 0 && t[strlen(f)] == '/') return -E_INVAL;
  if (cat_info(t, &tpb, tname) == 0) {          /* what it replaces */
    if (is_dir(&tpb) != is_dir(&pb)) return is_dir(&tpb) ? -E_ISDIR : -E_NOTDIR;
    if ((r = mac_err(HDelete(0, 0, tname))) < 0) return r;
  }
  strcpy(fdir, f);
  *strrchr(fdir, '/') = '\0';
  strcpy(tdir, t);
  *strrchr(tdir, '/') = '\0';
  base_name(t, newname);
  /* by its folder and name: HRename takes the new name in the folder it
     is given, which a full path name does not change (0 is the program's) */
  base_name(f, oldname);
  vref = vol_ref(fname);
  parid = pb.hFileInfo.ioFlParID;       /* dirInfo.ioDrParID, for a folder */
  if (strcmp(fdir, tdir) == 0) return mac_err(HRename(vref, parid, oldname, newname));
  /* the new name first, where it is, then the move */
  if ((r = mac_err(HRename(vref, parid, oldname, newname))) < 0) return r;
  snprintf(f, sizeof f, "%s/%s", fdir, strrchr(t, '/') + 1);
  if ((r = mac_path(f, fname)) < 0) return r;
  if ((r = mac_path(tdir[0] ? tdir : "/", dname)) < 0) return r;
  memset(&mv, 0, sizeof mv);
  mv.ioNamePtr = fname;
  mv.ioNewName = dname;
  return mac_err(PBCatMoveSyncG(&mv));
}

const char *bb_getcwd(void) { return cwd; }

int bb_chdir(const char *path)
{
  char full[PATHMAX];
  int isdir, r;
  long long size;

  if ((r = abs_path(path, full)) < 0) return r;
  if ((r = bb_stat(full, 1, &isdir, &size)) < 0) return r;
  if (!isdir) return -E_NOTDIR;
  strcpy(cwd, full);
  return 0;
}

struct dirlist {
  char **names;
  int n, at;
};

static int dl_add(struct dirlist *d, ConstStr255Param pn)
{
  char *s, **nn;

  if ((nn = realloc(d->names, (d->n + 1) * sizeof *nn)) == NULL) return -1;
  d->names = nn;
  if ((s = malloc(3 * pn[0] + 1)) == NULL) return -1;
  gork_name(pn, s);
  d->names[d->n++] = s;
  return 0;
}

void *bb_opendir(const char *path)
{
  char full[PATHMAX];
  struct dirlist *d;
  HParamBlockRec vp;
  CInfoPBRec pb;
  Str255 name, pn;
  short vref;
  long dir;
  int i;

  if (abs_path(path, full) < 0 || (d = calloc(1, sizeof *d)) == NULL)
    return NULL;
  if (strcmp(full, "/") == 0) {         /* the volumes */
    for (i = 1;; i++) {
      memset(&vp, 0, sizeof vp);
      vp.volumeParam.ioNamePtr = pn;
      vp.volumeParam.ioVolIndex = i;
      if (PBHGetVInfoSync(&vp) != noErr) break;
      dl_add(d, pn);
    }
    return d;
  }
  if (cat_info(full, &pb, name) < 0 || !is_dir(&pb)) {
    free(d);
    return NULL;
  }
  vref = vol_ref(name);
  dir = pb.dirInfo.ioDrDirID;
  for (i = 1;; i++) {
    memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = pn;
    pb.hFileInfo.ioVRefNum = vref;
    pb.hFileInfo.ioFDirIndex = i;
    pb.hFileInfo.ioDirID = dir;
    if (PBGetCatInfoSync(&pb) != noErr) break;
    dl_add(d, pn);
  }
  return d;
}

const char *bb_readdir(void *p)
{
  struct dirlist *d = p;

  return d->at < d->n ? d->names[d->at++] : NULL;
}

void bb_closedir(void *p)
{
  struct dirlist *d = p;
  int i;

  for (i = 0; i < d->n; i++) free(d->names[i]);
  free(d->names);
  free(d);
}

/* ------------------------------------------------------------------ */
/* the environment: the prompt's `set`                                */
/* ------------------------------------------------------------------ */

#define NENV 32
static char *env[NENV];

static char **env_find(const char *name)
{
  size_t n = strlen(name);
  int i;

  for (i = 0; i < NENV; i++)
    if (env[i] && strncmp(env[i], name, n) == 0 && env[i][n] == '=')
      return &env[i];
  return NULL;
}

const char *bb_getenv(const char *name)
{
  char **e = env_find(name);

  return e ? *e + strlen(name) + 1 : NULL;
}

int bb_setenv(const char *name, const char *value)
{
  char **e = env_find(name), *s;
  int i;

  if (value == NULL) {
    if (e) {
      free(*e);
      *e = NULL;
    }
    return 0;
  }
  if ((s = malloc(strlen(name) + strlen(value) + 2)) == NULL) return -E_NOMEM;
  sprintf(s, "%s=%s", name, value);
  if (e) {
    free(*e);
    *e = s;
    return 0;
  }
  for (i = 0; i < NENV; i++)
    if (env[i] == NULL) {
      env[i] = s;
      return 0;
    }
  free(s);
  return -E_NOMEM;
}

/* gork.cnf beside the program */
const char *bb_home(void) { return app_dir; }
const char *bb_tmpdir(void) { return app_dir; }

/* ------------------------------------------------------------------ */
/* the console                                                        */
/* ------------------------------------------------------------------ */

int bb_console_getc(void) { return -1; }

/* Command-period, the Mac's interrupt */
int bb_ctrlc(void)
{
  EventRecord ev;

  SystemTask();
  while (EventAvail(keyDownMask | autoKeyMask, &ev)) {
    GetNextEvent(keyDownMask | autoKeyMask, &ev);
    if ((ev.modifiers & cmdKey) && (ev.message & charCodeMask) == '.')
      return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */

unsigned long long bb_ms(void)
{
  return (unsigned long long) TickCount() * 1000 / 60;
}

/* the Mac's clock is local time since 1904; the location in parameter
   RAM says how far from UTC */
long long bb_epoch(void)
{
  unsigned long secs;
  Location loc;
  long delta;

  GetDateTime(&secs);
  ReadLocationG(&loc);
  delta = loc.gmtFlags & 0x00FFFFFF;
  if (delta & 0x00800000) delta |= 0xFF000000;
  return (long long) secs - 2082844800LL - delta;
}

void bb_udelay(unsigned us)
{
  unsigned long end = TickCount() + (us + 16666) / 16667;

  while (TickCount() < end) SystemTask();
}

/* no shell to run a command in */
int bb_run(const char *line, char **o, size_t *len)
{
  (void) line;
  (void) o;
  (void) len;
  return -1;
}

void card_pause(void)
{
  SystemTask();
  con_idle();
}

void card_break(void)
{
  if (bb_ctrlc()) {
    outs("\ngork: interrupted\n");
    gork_exit(130);
  }
}

/* ------------------------------------------------------------------ */
/* MacTCP                                                             */
/* ------------------------------------------------------------------ */

static short ipp;               /* the .IPP driver, once open */
static StreamPtr stream;
static Ptr rcvbuf;
#define RCVBUF (16 * 1024)

static int set_errno(int e)
{
  gork_errno = e;
  return -1;
}

int ip_start(void)
{
  if (ipp != 0) return 0;
  if (OpenDriver((ConstStr255Param) "\p.IPP", &ipp) != noErr) {
    ipp = 0;
    outs("gork: no MacTCP (or Open Transport's MacTCP) to talk to\n");
    return -1;
  }
  return 0;
}

void ip_poll(void) { SystemTask(); }

/* a call to the driver, waited for.  At Command-period the connection
   is aborted, which ends the call, and gork after it: the driver must be
   done with the parameter block, on this stack, before gork leaves. */
static OSErr tcp_call(TCPiopb *pb, short code)
{
  TCPiopb ab;

  pb->ioCRefNum = ipp;
  pb->csCode = code;
  pb->tcpStream = stream;
  pb->ioCompletion = NULL;
  PBControlAsync((ParmBlkPtr) pb);
  while (pb->ioResult > 0) {
    if (bb_ctrlc()) {
      memset(&ab, 0, sizeof ab);
      ab.ioCRefNum = ipp;
      ab.csCode = TCPAbort;
      ab.tcpStream = stream;
      PBControlSync((ParmBlkPtr) &ab);
      while (pb->ioResult > 0) SystemTask();
      outs("\ngork: interrupted\n");
      gork_exit(130);
    }
    card_pause();
  }
  return pb->ioResult;
}

static int tcp_err(OSErr e)
{
  switch (e) {
  case openFailed: return E_CONNREFUSED;
  case commandTimeout: return E_TIMEDOUT;
  case connectionTerminated: case connectionDoesntExist: return E_CONNRESET;
  case ipBadAddr: return E_HOSTUNREACH;
  case insufficientResources: case outOfMemory: return E_NOMEM;
  default: return E_NETDOWN;
  }
}

/* waited for without Command-period: it is quick, and run() calls it
   after gork has ended, when gork_exit() must not run again */
void ip_tcp_close(void)
{
  TCPiopb pb;

  if (stream == NULL) return;
  memset(&pb, 0, sizeof pb);
  pb.ioCRefNum = ipp;
  pb.csCode = TCPAbort;
  pb.tcpStream = stream;
  PBControlSync((ParmBlkPtr) &pb);
  memset(&pb, 0, sizeof pb);
  pb.ioCRefNum = ipp;
  pb.csCode = TCPRelease;
  pb.tcpStream = stream;
  PBControlSync((ParmBlkPtr) &pb);
  stream = NULL;
}

int ip_tcp_connect(const unsigned char *addr, unsigned port)
{
  TCPiopb pb;
  OSErr e;

  if (ip_start() != 0) return set_errno(E_NETDOWN);
  ip_tcp_close();
  if (rcvbuf == NULL && (rcvbuf = NewPtr(RCVBUF)) == NULL)
    return set_errno(E_NOMEM);
  memset(&pb, 0, sizeof pb);
  pb.csParam.create.rcvBuff = rcvbuf;
  pb.csParam.create.rcvBuffLen = RCVBUF;
  stream = NULL;
  pb.ioCRefNum = ipp;
  pb.csCode = TCPCreate;
  if ((e = PBControlSync((ParmBlkPtr) &pb)) != noErr)
    return set_errno(tcp_err(e));
  stream = pb.tcpStream;
  memset(&pb, 0, sizeof pb);
  pb.csParam.open.ulpTimeoutValue = 30;
  pb.csParam.open.ulpTimeoutAction = 1;         /* abort */
  pb.csParam.open.validityFlags = timeoutValue | timeoutAction;
  pb.csParam.open.commandTimeoutValue = 30;
  memcpy(&pb.csParam.open.remoteHost, addr, 4); /* both big-endian */
  pb.csParam.open.remotePort = port;
  if ((e = tcp_call(&pb, TCPActiveOpen)) != noErr) {
    ip_tcp_close();
    return set_errno(tcp_err(e));
  }
  return 0;
}

int ip_tcp_write(const void *p, size_t n)
{
  struct { wdsEntry e[2]; } wds;
  TCPiopb pb;
  OSErr e;

  if (stream == NULL) return set_errno(E_CONNRESET);
  if (n > 8192) n = 8192;
  wds.e[0].length = n;
  wds.e[0].ptr = (Ptr) p;
  wds.e[1].length = 0;
  wds.e[1].ptr = NULL;
  memset(&pb, 0, sizeof pb);
  pb.csParam.send.ulpTimeoutValue = 60;
  pb.csParam.send.ulpTimeoutAction = 1;
  pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
  pb.csParam.send.pushFlag = 1;
  pb.csParam.send.wdsPtr = (Ptr) &wds;
  if ((e = tcp_call(&pb, TCPSend)) != noErr) return set_errno(tcp_err(e));
  return (int) n;
}

/* 1 with what is unread and the state, or 0 if the stream is gone */
static int tcp_status(unsigned *unread, int *state)
{
  TCPiopb pb;

  if (stream == NULL) return 0;
  memset(&pb, 0, sizeof pb);
  if (tcp_call(&pb, TCPStatus) != noErr) return 0;
  *unread = pb.csParam.status.amtUnreadData;
  *state = pb.csParam.status.connectionState;
  return 1;
}

int ip_tcp_ready(void)
{
  unsigned unread;
  int state;

  if (!tcp_status(&unread, &state)) return 1;
  return unread > 0 || state != TCPSEstablished;
}

int ip_tcp_read(void *p, size_t n)
{
  unsigned unread;
  int state;
  TCPiopb pb;
  OSErr e;

  for (;;) {
    if (!tcp_status(&unread, &state)) return set_errno(E_CONNRESET);
    if (unread > 0) break;
    /* the other side's FIN is in, or the connection is gone: the end */
    if (state != TCPSEstablished && state != TCPSFinWait1 &&
        state != TCPSFinWait2)
      return 0;
    card_break();
    card_pause();
  }
  if (n > unread) n = unread;
  if (n > 0xFFFF) n = 0xFFFF;
  memset(&pb, 0, sizeof pb);
  pb.csParam.receive.commandTimeoutValue = 1;
  pb.csParam.receive.rcvBuff = p;
  pb.csParam.receive.rcvBuffLen = n;
  e = tcp_call(&pb, TCPRcv);
  if (e == connectionClosing || e == connectionTerminated) return 0;
  if (e != noErr) return set_errno(tcp_err(e));
  return pb.csParam.receive.rcvBuffLen;
}

/* Names, by the resolver MacTCP keeps in a code resource ('dnrp') of its
   control panel; Open Transport's TCP/IP control panel has one too, of
   the same type and creator.  Its calls are C's: a selector first. */
typedef OSErr (*dnr_fn)(long selector, ...);
static Handle dnr;
#define OPENRESOLVER 1
#define CLOSERESOLVER 2
#define STRTOADDR 3
static volatile int dnr_finished;

struct hostinfo {               /* MacTCP's, 68k alignment */
  long rtnCode;
  char cname[255];
  char pad;
  unsigned long addr[4];
};

static pascal void dnr_done(struct hostinfo *h, char *user)
{
  (void) h;
  (void) user;
  dnr_finished = 1;
}

static void dnr_close(void)
{
  if (dnr == NULL) return;
  ((dnr_fn) *dnr)(CLOSERESOLVER);
  DisposeHandle(dnr);
  dnr = NULL;
}

/* the resolver's file, by type and creator, in the System Folder or the
   Control Panels folder */
static short dnr_file(void)
{
  static const OSType where[2] = {kSystemFolderType, kControlPanelFolderType};
  static const OSType creators[2] = {'ztcp', 'mtcp'};
  CInfoPBRec pb;
  Str63 name;
  short vref, ref;
  long dir;
  int w, c, i;

  for (w = 0; w < 2; w++) {
    if (FindFolder(ON_SYSTEM_DISK, where[w], false, &vref, &dir) != noErr)
      continue;
    for (c = 0; c < 2; c++)
      for (i = 1;; i++) {
        memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = vref;
        pb.hFileInfo.ioFDirIndex = i;
        pb.hFileInfo.ioDirID = dir;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        if (pb.hFileInfo.ioFlFndrInfo.fdType == 'cdev' &&
            pb.hFileInfo.ioFlFndrInfo.fdCreator == creators[c]) {
          ref = HOpenResFile(vref, dir, name, fsRdPerm);
          if (ref != -1) return ref;
        }
      }
  }
  return -1;
}

static int dnr_open(void)
{
  short old = CurResFile(), ref;
  OSErr e;

  if (dnr != NULL) return 0;
  if ((ref = dnr_file()) == -1) return -1;
  UseResFile(ref);
  dnr = Get1IndResource('dnrp', 1);
  if (dnr != NULL) {
    DetachResource(dnr);
    HLock(dnr);
  }
  CloseResFile(ref);
  UseResFile(old);
  if (dnr == NULL) return -1;
  e = ((dnr_fn) *dnr)(OPENRESOLVER, (char *) NULL);
  if (e != noErr) {
    DisposeHandle(dnr);
    dnr = NULL;
    return -1;
  }
  return 0;
}

int ip_resolve(const char *name, unsigned char *addr)
{
  static struct hostinfo h;
  unsigned long end;
  OSErr e;

  if (ip_start() != 0) return set_errno(E_NETDOWN);
  if (dnr_open() != 0) {
    outs("gork: no name resolver (MacTCP DNR); give the relay's address\n");
    return set_errno(E_HOSTUNREACH);
  }
  memset(&h, 0, sizeof h);
  dnr_finished = 0;
  e = ((dnr_fn) *dnr)(STRTOADDR, (char *) name, &h, dnr_done, (char *) NULL);
  if (e == cacheFault) {        /* asked of a name server: wait for it */
    end = TickCount() + 60 * 30;
    while (!dnr_finished) {
      if (TickCount() > end) return set_errno(E_TIMEDOUT);
      card_break();
      card_pause();
    }
    e = h.rtnCode;
  }
  if (e != noErr || h.addr[0] == 0) return set_errno(E_HOSTUNREACH);
  memcpy(addr, &h.addr[0], 4);
  return 0;
}

/* ------------------------------------------------------------------ */
/* the command                                                        */
/* ------------------------------------------------------------------ */

/* gork's variables, data and bss together (barebox/gork.lds): saved as
   they are before the first run and put back before each run after it */
extern char __gork_data_start[], __gork_data_end[];
static void *gork_data_saved;

static void gork_reset(void)
{
  size_t n = __gork_data_end - __gork_data_start;

  if (gork_data_saved == NULL) {
    if ((gork_data_saved = malloc(n)) != NULL)
      memcpy(gork_data_saved, __gork_data_start, n);
  } else
    memcpy(__gork_data_start, gork_data_saved, n);
}

static jmp_buf done;
static int status;

void bb_exit(int s)
{
  status = s;
  longjmp(done, 1);
}

static int run(int argc, char **argv)
{
  status = 0;
  if (setjmp(done) == 0) {
    gork_reset();
    status = gork_start(argc, argv);
    gork_exit(status);          /* flushes, frees, comes back here */
  }
  ip_tcp_close();
  return status;
}

/* a line as words: spaces part them, "..." keeps them together, and \"
   and \\ inside quotes are " and \ */
static int split(char *s, char **argv, int max)
{
  int n = 0;
  char *o;

  for (;;) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || n == max) return n;
    argv[n++] = o = s;
    while (*s && *s != ' ' && *s != '\t') {
      if (*s == '"') {
        for (s++; *s && *s != '"'; s++) {
          if (*s == '\\' && (s[1] == '"' || s[1] == '\\')) s++;
          *o++ = *s;
        }
        if (*s) s++;
      } else
        *o++ = *s++;
    }
    if (*s) s++;
    *o = '\0';
  }
}

/* the folder the program is in */
static void find_app_dir(void)
{
  ProcessSerialNumber psn;
  ProcessInfoRec info;
  FSSpec spec;

  memset(&info, 0, sizeof info);
  info.processInfoLength = sizeof info;
  info.processAppSpec = &spec;
  if (GetCurrentProcess(&psn) == noErr &&
      GetProcessInformation(&psn, &info) == noErr)
    dir_path(spec.vRefNum, spec.parID, app_dir);
}

/* `set NAME=value`, `unset NAME`: 1 if the line was one */
static int builtin(char **argv, int argc)
{
  char *eq;

  if (strcmp(argv[0], "set") == 0 && argc == 2 &&
      (eq = strchr(argv[1], '=')) != NULL) {
    *eq = '\0';
    bb_setenv(argv[1], eq + 1);
    return 1;
  }
  if (strcmp(argv[0], "unset") == 0 && argc == 2) {
    bb_setenv(argv[1], NULL);
    return 1;
  }
  return 0;
}

/* gork.in beside the program, whole, or NULL */
static char *read_script(void)
{
  char path[PATHMAX], *buf;
  long n, k;
  int fd;

  snprintf(path, sizeof path, "%s/gork.in", app_dir);
  if ((fd = bb_open(path, 0, 0)) < 0) return NULL;
  if ((buf = malloc(32768)) == NULL) {
    bb_close(fd);
    return NULL;
  }
  /* a text file comes a little at a time */
  for (n = 0; n < 32767 && (k = bb_read(fd, buf + n, 32767 - n)) > 0; n += k) {}
  bb_close(fd);
  buf[n] = '\0';
  return buf;
}

/* the next line of the script (CR or LF ends it), or of the keyboard */
static int next_line(char **script, char *line, size_t size)
{
  size_t n;

  if (*script == NULL)
    return con_readline(line, size) >= 0;
  if (**script == '\0') return 0;
  n = strcspn(*script, "\r\n");
  if (n >= size) n = size - 1;
  memcpy(line, *script, n);
  line[n] = '\0';
  *script += n;
  if (**script == '\r') (*script)++;
  if (**script == '\n') (*script)++;
  return 1;
}

int main(void)
{
  char line[2048], path[PATHMAX], *argv[64], *script, *at;
  int argc, shutdown = 0;

  /* a stack of 512K: gork keeps paths of 4K on it, and a 68k Mac gives
     a program 24K or so */
  SetApplLimit(LMGetApplLimit() - 512 * 1024);
  MaxApplZone();
  if (con_init() != 0) return 1;
  find_app_dir();

  at = script = read_script();
  if (script) {
    snprintf(path, sizeof path, "%s/gork.out", app_dir);
    transcript_fd = bb_open(path, O_WR | O_CRE | O_TRU, 0);
  }

  outs("gork for the Mac: a gork command line at each prompt (--help for\n"
       "the options), set NAME=value, unset NAME, or quit.\n");
  for (;;) {
    outs("gork> ");
    if (!next_line(&at, line, sizeof line)) break;
    line[strcspn(line, "\r\n")] = '\0';
    if (script) {
      outs(line);
      outs("\n");
    }
    argc = split(line, argv + 1, 62);
    if (argc == 0) continue;
    if (strcmp(argv[1], "quit") == 0) break;
    if (strcmp(argv[1], "shutdown") == 0) {
      shutdown = 1;
      break;
    }
    if (builtin(argv + 1, argc)) continue;
    /* the folder to work in: GORK_DIR, or the program's */
    strcpy(cwd, app_dir);
    if (bb_getenv("GORK_DIR") && bb_chdir(bb_getenv("GORK_DIR")) < 0) {
      outs("gork: GORK_DIR: no such folder\n");
      continue;
    }
    if (strcmp(argv[1], "gork") == 0)         /* "gork x" is "x" */
      run(argc, argv + 1);
    else {
      argv[0] = "gork";
      run(argc + 1, argv);
    }
    if (script) {
      sprintf(line, "==status %d\n", status);
      outs(line);
    }
    if (transcript_fd >= 0) {   /* the transcript so far, on the disk */
      bb_close(transcript_fd);
      snprintf(path, sizeof path, "%s/gork.out", app_dir);
      transcript_fd = bb_open(path, O_WR | O_APP, 0);
    }
  }
  if (transcript_fd >= 0) bb_close(transcript_fd);
  dnr_close();
  if (shutdown) ShutDwnPower();
  return 0;
}
