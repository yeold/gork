/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * glue.c -- GRUB's half of gork: the `gork` command (the module gork.mod),
 * the calls of barebox/bb.h on GRUB's own, a file system in RAM for gork
 * to write in, (gork), and the network card for ip.c (the card_* calls of
 * ip.h).  Built as GRUB builds its modules; gork's half is the one object
 * gork.o, whose names are gork_* (barebox/bb.h says why).
 *
 * GRUB reads file systems but writes none, so files are written in
 * (gork), a device of this module's, which GRUB's commands read as any
 * other: `cat (gork)/x`, `configfile (gork)/x.cfg`.  It lasts until GRUB
 * boots something.  Every other device is read-only to gork.
 */
#include <grub/dl.h>
#include <grub/command.h>
#include <grub/datetime.h>
#include <grub/device.h>
#include <grub/disk.h>
#include <grub/env.h>
#include <grub/err.h>
#include <grub/file.h>
#include <grub/fs.h>
#include <grub/i18n.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/net.h>
#include <grub/net/netbuff.h>
#include <grub/script_sh.h>
#include <grub/setjmp.h>
#include <grub/term.h>
#include <grub/time.h>
#include <grub/types.h>
#ifdef GRUB_MACHINE_EFI
#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#endif

GRUB_MOD_LICENSE ("GPLv3+");

/* bb.h's calls, here with GRUB's types (bb.h itself would bring the
   compiler's stddef.h beside GRUB's own types) */
void *bb_malloc (grub_size_t n);
void *bb_realloc (void *p, grub_size_t n);
void bb_free (void *p);
int bb_open (const char *path, int flags, unsigned mode);
long bb_read (int fd, void *p, grub_size_t n);
long bb_write (int fd, const void *p, grub_size_t n);
int bb_close (int fd);
long long bb_lseek (int fd, long long off, int whence);
int bb_stat (const char *path, int follow, int *isdir, long long *size);
int bb_mkdir (const char *path);
int bb_unlink (const char *path);
int bb_rmdir (const char *path);
int bb_rename (const char *from, const char *to);
const char *bb_getcwd (void);
int bb_chdir (const char *path);
void *bb_opendir (const char *path);
const char *bb_readdir (void *d);
void bb_closedir (void *d);
const char *bb_getenv (const char *name);
int bb_setenv (const char *name, const char *value);
const char *bb_home (void);
const char *bb_tmpdir (void);
void bb_console_write (const char *p, grub_size_t n);
int bb_console_getc (void);
int bb_ctrlc (void);
unsigned long long bb_ms (void);
long long bb_epoch (void);
void bb_udelay (unsigned us);
int bb_run (const char *line, char **out, grub_size_t *len);
void bb_exit (int status) __attribute__ ((noreturn));

/* ip.h's card_* */
int card_open (grub_uint8_t mac[6]);
int card_send (const void *frame, grub_size_t len);
int card_recv (void *buf, grub_size_t size);
void card_pause (void);
void card_break (void);
unsigned card_seed (void);
unsigned card_mtu (void);
const char *card_ip_hint (void);

int gork_start (int argc, char **argv);    /* libc.c: then main() */
void gork_exit (int status) __attribute__ ((noreturn));

/* fwinclude/'s numbers, the Linux ones */
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define EROFS 30
#define ENAMETOOLONG 36
#define ENOTEMPTY 39
#define O_ACCMODE 0x0003
#define O_RDONLY 0x0000
#define O_CREAT 0x0040
#define O_EXCL 0x0080
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* gork's variables, data and bss together (barebox/gork.lds): saved as
   they are before the first run and put back before each run after it,
   since a command, unlike a program, keeps them from one run to the next */
extern char __gork_data_start[], __gork_data_end[];
static void *gork_data_saved;

static void
gork_reset (void)
{
  grub_size_t n = __gork_data_end - __gork_data_start;

  if (!gork_data_saved)
    {
      gork_data_saved = grub_malloc (n);
      if (gork_data_saved)
	grub_memcpy (gork_data_saved, __gork_data_start, n);
    }
  else
    grub_memcpy (__gork_data_start, gork_data_saved, n);
}

/* GRUB's error, as an errno, and GRUB's cleared: gork reports its own */
static int
gerr (void)
{
  int e;

  switch (grub_errno)
    {
    case GRUB_ERR_FILE_NOT_FOUND:
    case GRUB_ERR_UNKNOWN_DEVICE:
    case GRUB_ERR_BAD_DEVICE:
    case GRUB_ERR_UNKNOWN_FS:
      e = ENOENT;
      break;
    case GRUB_ERR_BAD_FILE_TYPE:
      e = EISDIR;
      break;
    case GRUB_ERR_OUT_OF_MEMORY:
      e = ENOMEM;
      break;
    case GRUB_ERR_BAD_FILENAME:
      e = EINVAL;
      break;
    default:
      e = EIO;
    }
  grub_errno = GRUB_ERR_NONE;
  return -e;
}

/* ------------------------------------------------------------------ */
/* (gork): the file system in RAM                                     */
/* ------------------------------------------------------------------ */

/* Each file and directory by its path below (gork): "/a/b"; the root is
   "" and not kept.  A list: gork writes a few files, not thousands. */
struct node
{
  struct node *next;
  char *path;
  int dir;
  char *data;
  grub_size_t size, cap;
};
static struct node *nodes;

/* (gork)'s path of a whole path, normalized into out (no "//", no "/"
   at the end), or NULL if it is not on (gork) */
static const char *
ram_path (const char *p, char *out, grub_size_t size)
{
  grub_size_t n = 0;

  if (grub_strncmp (p, "(gork)", 6) != 0)
    return NULL;
  for (p += 6; *p; p++)
    {
      if (*p == '/' && n > 0 && out[n - 1] == '/')
	continue;
      if (n + 1 >= size)
	return NULL;
      out[n++] = *p;
    }
  while (n > 0 && out[n - 1] == '/')
    n--;
  out[n] = '\0';
  if (n > 0 && out[0] != '/')
    return NULL;		/* (gork)x */
  return out;
}

/* the same, for a path below (gork) as GRUB gives one to the file system */
static void
ram_norm (const char *p, char *out, grub_size_t size)
{
  char tmp[1024];

  grub_snprintf (tmp, sizeof tmp, "(gork)/%s", p);
  if (!ram_path (tmp, out, size))
    out[0] = '\0';
}

static struct node *
ram_find (const char *path)
{
  struct node *n;

  for (n = nodes; n; n = n->next)
    if (grub_strcmp (n->path, path) == 0)
      return n;
  return NULL;
}

/* 1 if path is a directory: the root, or a node */
static int
ram_isdir (const char *path)
{
  struct node *n;

  if (path[0] == '\0')
    return 1;
  n = ram_find (path);
  return n && n->dir;
}

/* the length of path's parent: "/a/b" -> 2, "/a" -> 0 (the root) */
static grub_size_t
ram_parent_len (const char *path)
{
  const char *s = grub_strrchr (path, '/');

  return s ? (grub_size_t) (s - path) : 0;
}

static int
ram_parent_ok (const char *path)
{
  char parent[1024];
  grub_size_t n = ram_parent_len (path);

  grub_memcpy (parent, path, n);
  parent[n] = '\0';
  return ram_isdir (parent);
}

static struct node *
ram_new (const char *path, int dir)
{
  struct node *n = grub_zalloc (sizeof *n);

  if (!n)
    return NULL;
  n->path = grub_strdup (path);
  if (!n->path)
    {
      grub_free (n);
      return NULL;
    }
  n->dir = dir;
  n->next = nodes;
  nodes = n;
  return n;
}

static void
ram_drop (struct node *gone)
{
  struct node **p;

  for (p = &nodes; *p; p = &(*p)->next)
    if (*p == gone)
      {
	*p = gone->next;
	grub_free (gone->path);
	grub_free (gone->data);
	grub_free (gone);
	return;
      }
}

/* 1 if node n lies directly in directory dir */
static int
ram_in (const struct node *n, const char *dir)
{
  grub_size_t k = ram_parent_len (n->path);

  return grub_strlen (dir) == k && grub_strncmp (n->path, dir, k) == 0;
}

static const char *
ram_base (const struct node *n)
{
  return n->path + ram_parent_len (n->path) + 1;
}

/* GRUB's side: a device "gork" with no sectors, and a file system on it
   that reads the nodes */
static int
gorkdev_iterate (grub_disk_dev_iterate_hook_t hook, void *hook_data,
		 grub_disk_pull_t pull)
{
  if (pull != GRUB_DISK_PULL_NONE)
    return 0;
  return hook ("gork", hook_data);
}

static grub_err_t
gorkdev_open (const char *name, grub_disk_t disk)
{
  if (grub_strcmp (name, "gork"))
    return grub_error (GRUB_ERR_UNKNOWN_DEVICE, "not the gork disk");
  disk->total_sectors = 0;
  disk->id = 0;
  disk->data = 0;
  return GRUB_ERR_NONE;
}

static void
gorkdev_close (grub_disk_t disk __attribute__ ((unused)))
{
}

static grub_err_t
gorkdev_read (grub_disk_t disk __attribute__ ((unused)),
	      grub_disk_addr_t sector __attribute__ ((unused)),
	      grub_size_t size __attribute__ ((unused)),
	      char *buf __attribute__ ((unused)))
{
  return GRUB_ERR_OUT_OF_RANGE;
}

static grub_err_t
gorkdev_write (grub_disk_t disk __attribute__ ((unused)),
	       grub_disk_addr_t sector __attribute__ ((unused)),
	       grub_size_t size __attribute__ ((unused)),
	       const char *buf __attribute__ ((unused)))
{
  return GRUB_ERR_OUT_OF_RANGE;
}

static grub_err_t
gorkfs_dir (grub_device_t device, const char *path,
	    grub_fs_dir_hook_t hook, void *hook_data)
{
  char p[1024];
  struct node *n;
  struct grub_dirhook_info info;

  if (!device->disk || grub_strcmp (device->disk->name, "gork"))
    return grub_error (GRUB_ERR_BAD_FS, "not the gork file system");
  ram_norm (path, p, sizeof p);
  if (!ram_isdir (p))
    return ram_find (p) ? grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not a directory"))
			: grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("file `%s' not found"), path);
  for (n = nodes; n; n = n->next)
    if (ram_in (n, p))
      {
	grub_memset (&info, 0, sizeof info);
	info.dir = n->dir;
	if (hook (ram_base (n), &info, hook_data))
	  break;
      }
  return GRUB_ERR_NONE;
}

static grub_err_t
gorkfs_open (struct grub_file *file, const char *name)
{
  char p[1024];
  struct node *n;

  ram_norm (name, p, sizeof p);
  n = ram_find (p);
  if (!n)
    return grub_error (GRUB_ERR_FILE_NOT_FOUND, N_("file `%s' not found"), name);
  if (n->dir)
    return grub_error (GRUB_ERR_BAD_FILE_TYPE, N_("not a regular file"));
  /* a copy: gork may write the file while GRUB reads it */
  file->data = grub_malloc (n->size ? n->size : 1);
  if (!file->data)
    return grub_errno;
  grub_memcpy (file->data, n->data, n->size);
  file->size = n->size;
  return GRUB_ERR_NONE;
}

static grub_ssize_t
gorkfs_read (grub_file_t file, char *buf, grub_size_t len)
{
  grub_memcpy (buf, (char *) file->data + file->offset, len);
  return len;
}

static grub_err_t
gorkfs_close (grub_file_t file)
{
  grub_free (file->data);
  return GRUB_ERR_NONE;
}

static struct grub_disk_dev gorkdev = {
  .name = "gork",
  .id = GRUB_DISK_DEVICE_PROCFS_ID + 0x100,	/* no sectors: nothing cached */
  .disk_iterate = gorkdev_iterate,
  .disk_open = gorkdev_open,
  .disk_close = gorkdev_close,
  .disk_read = gorkdev_read,
  .disk_write = gorkdev_write,
  .next = 0
};

static struct grub_fs gorkfs = {
  .name = "gork",
  .fs_dir = gorkfs_dir,
  .fs_open = gorkfs_open,
  .fs_read = gorkfs_read,
  .fs_close = gorkfs_close,
  .next = 0
};

/* ------------------------------------------------------------------ */
/* memory, files, the environment                                     */
/* ------------------------------------------------------------------ */

/* gork's working directory, for GRUB has none: see cwd_init */
static char cwd[1024];

/* path as GRUB takes it, with a device: relative ones against cwd, and
   "/x" on cwd's device; into buf, or path itself if it has one */
static const char *
abs_path (const char *path, char *buf, grub_size_t size)
{
  const char *e;
  grub_size_t dev = 0;

  if (path[0] == '(')
    return path;
  if (cwd[0] == '(' && (e = grub_strchr (cwd, ')')) != NULL)
    dev = e + 1 - cwd;
  if (path[0] == '/')
    grub_snprintf (buf, size, "%.*s%s", (int) dev, cwd, path);
  else
    grub_snprintf (buf, size, "%s/%s", cwd, path);
  return buf;
}

void *bb_malloc (grub_size_t n) { return grub_malloc (n); }
void *bb_realloc (void *p, grub_size_t n) { return grub_realloc (p, n); }
void bb_free (void *p) { grub_free (p); }

#define NF 16
static struct ofile
{
  int used;
  grub_file_t file;		/* on a disk: GRUB's, read-only */
  struct node *node;		/* or in (gork) */
  grub_size_t at;
  int writing, append;
} files[NF];

static struct ofile *
get_f (int fd)
{
  return fd >= 0 && fd < NF && files[fd].used ? &files[fd] : NULL;
}

int
bb_open (const char *path, int flags, unsigned mode __attribute__ ((unused)))
{
  char full_[1024];
  char p[1024];
  int fd, writing = (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC));
  struct node *n = NULL;
  grub_file_t f = NULL;
  path = abs_path (path, full_, sizeof full_);

  for (fd = 0; fd < NF && files[fd].used; fd++)
    ;
  if (fd == NF)
    return -EMFILE;
  if (ram_path (path, p, sizeof p))
    {
      if (p[0] == '\0')
	return -EISDIR;
      n = ram_find (p);
      if (n && n->dir)
	return -EISDIR;
      if (n && (flags & O_CREAT) && (flags & O_EXCL))
	return -EEXIST;
      if (!n)
	{
	  if (!(flags & O_CREAT) || !ram_parent_ok (p))
	    return -ENOENT;
	  if (!(n = ram_new (p, 0)))
	    return -ENOMEM;
	}
      if (flags & O_TRUNC)
	n->size = 0;
    }
  else
    {
      if (writing)
	return -EROFS;
      f = grub_file_open (path, GRUB_FILE_TYPE_CAT | GRUB_FILE_TYPE_NO_DECOMPRESS);
      if (!f)
	return gerr ();
    }
  files[fd].used = 1;
  files[fd].file = f;
  files[fd].node = n;
  files[fd].at = 0;
  files[fd].writing = writing;
  files[fd].append = (flags & O_APPEND) != 0;
  return fd;
}

long
bb_read (int fd, void *p, grub_size_t n)
{
  struct ofile *o = get_f (fd);
  grub_ssize_t r;

  if (!o)
    return -EBADF;
  if (o->file)
    {
      r = grub_file_read (o->file, p, n);
      return r < 0 ? gerr () : r;
    }
  if (o->at >= o->node->size)
    return 0;
  if (n > o->node->size - o->at)
    n = o->node->size - o->at;
  grub_memcpy (p, o->node->data + o->at, n);
  o->at += n;
  return n;
}

long
bb_write (int fd, const void *p, grub_size_t n)
{
  struct ofile *o = get_f (fd);
  struct node *d;
  grub_size_t end, cap;
  char *q;

  if (!o || !o->node || !o->writing)
    return -EBADF;
  d = o->node;
  if (o->append)
    o->at = d->size;
  end = o->at + n;
  if (end > d->cap)
    {
      cap = d->cap ? d->cap : 4096;
      while (cap < end)
	cap *= 2;
      q = grub_realloc (d->data, cap);
      if (!q)
	return -ENOMEM;
      d->data = q;
      d->cap = cap;
    }
  if (o->at > d->size)
    grub_memset (d->data + d->size, 0, o->at - d->size);
  grub_memcpy (d->data + o->at, p, n);
  o->at = end;
  if (end > d->size)
    d->size = end;
  return n;
}

int
bb_close (int fd)
{
  struct ofile *o = get_f (fd);

  if (!o)
    return -EBADF;
  if (o->file)
    grub_file_close (o->file);
  grub_errno = GRUB_ERR_NONE;
  o->used = 0;
  return 0;
}

long long
bb_lseek (int fd, long long off, int whence)
{
  struct ofile *o = get_f (fd);
  long long at, size;

  if (!o)
    return -EBADF;
  at = o->file ? (long long) grub_file_tell (o->file) : (long long) o->at;
  size = o->file ? (long long) grub_file_size (o->file) : (long long) o->node->size;
  if (whence == SEEK_CUR)
    off += at;
  else if (whence == SEEK_END)
    off += size;
  else if (whence != SEEK_SET)
    return -EINVAL;
  if (off < 0)
    return -EINVAL;
  if (o->file)
    {
      if (off > size)
	off = size;		/* GRUB's files end where they end */
      if (grub_file_seek (o->file, off) == (grub_off_t) -1)
	return gerr ();
    }
  else
    o->at = off;
  return off;
}

static int
stop_hook (const char *name __attribute__ ((unused)),
	   const struct grub_dirhook_info *info __attribute__ ((unused)),
	   void *data __attribute__ ((unused)))
{
  return 1;
}

/* the device and file system of a path on a disk, and the path on it */
struct where
{
  grub_device_t dev;
  grub_fs_t fs;
  const char *path;
};

static int
find_fs (const char *full, struct where *w)
{
  char *name = grub_file_get_device_name (full);
  const char *s = grub_strchr (full, ')');

  w->dev = grub_device_open (name);
  grub_free (name);
  w->fs = NULL;
  if (!w->dev)
    return gerr ();
  w->fs = grub_fs_probe (w->dev);
  if (!w->fs)
    {
      grub_device_close (w->dev);
      return gerr ();
    }
  w->path = s ? s + 1 : full;
  if (!*w->path)
    w->path = "/";
  return 0;
}

/* 0 if path on a disk is a directory, else -errno */
static int
disk_dir (const char *path)
{
  struct where w;
  int r;

  if ((r = find_fs (path, &w)) < 0)
    return r;
  w.fs->fs_dir (w.dev, w.path, stop_hook, NULL);
  r = grub_errno == GRUB_ERR_NONE ? 0 : gerr ();
  grub_device_close (w.dev);
  grub_errno = GRUB_ERR_NONE;
  return r == -EISDIR ? -ENOTDIR : r;
}

int
bb_stat (const char *path, int follow __attribute__ ((unused)), int *isdir,
	 long long *size)
{
  char full_[1024];
  char p[1024];
  struct node *n;
  grub_file_t f;
  path = abs_path (path, full_, sizeof full_);

  if (ram_path (path, p, sizeof p))
    {
      if (p[0] == '\0')
	{
	  *isdir = 1;
	  *size = 0;
	  return 0;
	}
      if (!(n = ram_find (p)))
	return -ENOENT;
      *isdir = n->dir;
      *size = n->size;
      return 0;
    }
  f = grub_file_open (path, GRUB_FILE_TYPE_GET_SIZE | GRUB_FILE_TYPE_NO_DECOMPRESS);
  if (f)
    {
      *isdir = 0;
      *size = grub_file_size (f);
      grub_file_close (f);
      return 0;
    }
  grub_errno = GRUB_ERR_NONE;
  if (disk_dir (path) == 0)
    {
      *isdir = 1;
      *size = 0;
      return 0;
    }
  return -ENOENT;
}

int
bb_mkdir (const char *path)
{
  char full_[1024];
  char p[1024];
  path = abs_path (path, full_, sizeof full_);

  if (!ram_path (path, p, sizeof p))
    return -EROFS;
  if (p[0] == '\0' || ram_find (p))
    return -EEXIST;
  if (!ram_parent_ok (p))
    return -ENOENT;
  return ram_new (p, 1) ? 0 : -ENOMEM;
}

int
bb_unlink (const char *path)
{
  char full_[1024];
  char p[1024];
  struct node *n;
  int i;
  path = abs_path (path, full_, sizeof full_);

  if (!ram_path (path, p, sizeof p))
    return -EROFS;
  if (!(n = ram_find (p)))
    return -ENOENT;
  if (n->dir)
    return -EISDIR;
  for (i = 0; i < NF; i++)
    if (files[i].used && files[i].node == n)
      return -EBUSY;
  ram_drop (n);
  return 0;
}

int
bb_rmdir (const char *path)
{
  char full_[1024];
  char p[1024];
  struct node *n, *c;
  path = abs_path (path, full_, sizeof full_);

  if (!ram_path (path, p, sizeof p))
    return -EROFS;
  if (p[0] == '\0')
    return -EBUSY;
  if (!(n = ram_find (p)))
    return -ENOENT;
  if (!n->dir)
    return -ENOTDIR;
  for (c = nodes; c; c = c->next)
    if (ram_in (c, p))
      return -ENOTEMPTY;
  ram_drop (n);
  return 0;
}

/* within (gork) only; a directory takes what is in it along */
int
bb_rename (const char *from, const char *to)
{
  char ff_[1024], ft_[1024];
  char f[1024], t[1024], *np;
  struct node *n, *c;
  grub_size_t fl;
  from = abs_path (from, ff_, sizeof ff_);
  to = abs_path (to, ft_, sizeof ft_);

  if (!ram_path (from, f, sizeof f))
    return -EROFS;
  if (!ram_path (to, t, sizeof t))
    return -EXDEV;
  if (!(n = ram_find (f)))
    return -ENOENT;
  if (t[0] == '\0' || ram_find (t))
    return -EEXIST;
  if (!ram_parent_ok (t))
    return -ENOENT;
  fl = grub_strlen (f);
  if (n->dir && grub_strncmp (t, f, fl) == 0 && t[fl] == '/')
    return -EINVAL;
  for (c = nodes; c; c = c->next)
    if (c == n || (n->dir && grub_strncmp (c->path, f, fl) == 0 && c->path[fl] == '/'))
      {
	np = grub_xasprintf ("%s%s", t, c->path + fl);
	if (!np)
	  return -ENOMEM;
	grub_free (c->path);
	c->path = np;
      }
  return 0;
}

/* The working directory: $GORK_DIR, or (gork)/, set at each run; one in
   (gork) that is not there yet is made, with its parents. */

static void
cwd_init (void)
{
  const char *d = grub_env_get ("GORK_DIR");
  char p[1024], *s;

  if (!d || !*d)
    d = "(gork)/";
  grub_strncpy (cwd, d, sizeof cwd - 1);
  cwd[sizeof cwd - 1] = '\0';
  if (!ram_path (cwd, p, sizeof p) || p[0] == '\0')
    return;
  for (s = p + 1;; s++)
    if (*s == '/' || *s == '\0')
      {
	char c = *s;

	*s = '\0';
	if (!ram_find (p))
	  ram_new (p, 1);
	*s = c;
	if (c == '\0')
	  break;
      }
}

const char *bb_getcwd (void) { return cwd; }

int
bb_chdir (const char *path)
{
  char full_[1024];
  int isdir, r;
  long long size;
  path = abs_path (path, full_, sizeof full_);

  if ((r = bb_stat (path, 1, &isdir, &size)) < 0)
    return r;
  if (!isdir)
    return -ENOTDIR;
  if (grub_strlen (path) >= sizeof cwd)
    return -ENAMETOOLONG;
  grub_strcpy (cwd, path);
  return 0;
}

struct dirlist
{
  char **names;
  int n, cap, at;
};

static int
dl_add (struct dirlist *d, const char *name)
{
  char **p;

  if (d->n == d->cap)
    {
      p = grub_realloc (d->names, (d->cap ? d->cap * 2 : 32) * sizeof *p);
      if (!p)
	return 1;
      d->names = p;
      d->cap = d->cap ? d->cap * 2 : 32;
    }
  if (!(d->names[d->n] = grub_strdup (name)))
    return 1;
  d->n++;
  return 0;
}

static int
dl_hook (const char *name, const struct grub_dirhook_info *info __attribute__ ((unused)),
	 void *data)
{
  if (grub_strcmp (name, ".") == 0 || grub_strcmp (name, "..") == 0)
    return 0;
  return dl_add (data, name);
}

void *
bb_opendir (const char *path)
{
  char full_[1024];
  struct dirlist *d = grub_zalloc (sizeof *d);
  struct where w;
  struct node *n;
  char p[1024];
  path = abs_path (path, full_, sizeof full_);

  if (!d)
    return NULL;
  if (ram_path (path, p, sizeof p))
    {
      if (!ram_isdir (p))
	goto fail;
      for (n = nodes; n; n = n->next)
	if (ram_in (n, p))
	  dl_add (d, ram_base (n));
      return d;
    }
  if (find_fs (path, &w) < 0)
    goto fail;
  w.fs->fs_dir (w.dev, w.path, dl_hook, d);
  grub_device_close (w.dev);
  if (grub_errno == GRUB_ERR_NONE)
    return d;
  grub_errno = GRUB_ERR_NONE;
fail:
  bb_closedir (d);
  return NULL;
}

const char *
bb_readdir (void *p)
{
  struct dirlist *d = p;

  return d->at < d->n ? d->names[d->at++] : NULL;
}

void
bb_closedir (void *p)
{
  struct dirlist *d = p;
  int i;

  for (i = 0; i < d->n; i++)
    grub_free (d->names[i]);
  grub_free (d->names);
  grub_free (d);
}

const char *bb_getenv (const char *name) { return grub_env_get (name); }

int
bb_setenv (const char *name, const char *value)
{
  return grub_env_set (name, value) == GRUB_ERR_NONE ? 0 : gerr ();
}

/* HOME, unless set: $prefix, where grub.cfg is */
const char *
bb_home (void)
{
  const char *p = grub_env_get ("prefix");

  return p && *p ? p : "(gork)";
}

const char *bb_tmpdir (void) { return "(gork)"; }

/* ------------------------------------------------------------------ */
/* the console, time, the shell                                       */
/* ------------------------------------------------------------------ */

void
bb_console_write (const char *p, grub_size_t n)
{
  char buf[257];
  grub_size_t k;

  while (n > 0)
    {
      k = n < sizeof buf - 1 ? n : sizeof buf - 1;
      grub_memcpy (buf, p, k);
      buf[k] = '\0';
      grub_xputs (buf);
      p += k;
      n -= k;
    }
  grub_refresh ();
}

/* a key, or -1; keys that are not characters are let go */
int
bb_console_getc (void)
{
  int k = grub_getkey_noblock ();

  if (k == GRUB_TERM_NO_KEY)
    return -1;
  if (k == (GRUB_TERM_CTRL | 'c'))
    return 3;
  return k >= 0 && k < 256 ? k : -1;
}

int
bb_ctrlc (void)
{
  return bb_console_getc () == 3;
}

unsigned long long bb_ms (void) { return grub_get_time_ms (); }

void
bb_udelay (unsigned us)
{
  grub_millisleep ((us + 999) / 1000);
}

long long
bb_epoch (void)
{
  struct grub_datetime dt;
  grub_int64_t t;

  if (grub_get_datetime (&dt) == GRUB_ERR_NONE && grub_datetime2unixtime (&dt, &t))
    return t;
  grub_errno = GRUB_ERR_NONE;
  return 0;
}

/* Output, while a command runs: taken instead of shown. */
static char *cap;
static grub_size_t cap_len, cap_size;

static void
cap_xputs (const char *s)
{
  grub_size_t n = grub_strlen (s), want;
  char *p;

  if (cap_len + n + 1 > cap_size)
    {
      want = cap_size ? cap_size : 4096;
      while (want < cap_len + n + 1)
	want *= 2;
      p = grub_realloc (cap, want);
      if (!p)
	return;
      cap = p;
      cap_size = want;
    }
  grub_memcpy (cap + cap_len, s, n);
  cap_len += n;
}

/* A line for GRUB's shell, as typed at its prompt: its status, $?.
   With out, what it printed (its errors too) comes back in *out. */
int
bb_run (const char *line, char **out, grub_size_t *len)
{
  void (*saved) (const char *) = grub_xputs;
  const char *q;
  int st;

  if (out)
    {
      cap = NULL;
      cap_len = cap_size = 0;
      grub_xputs = cap_xputs;
    }
  grub_script_execute_sourcecode (line);
  st = grub_errno != GRUB_ERR_NONE;
  if (grub_errno != GRUB_ERR_NONE)
    grub_print_error ();
  grub_errno = GRUB_ERR_NONE;
  if ((q = grub_env_get ("?")) != NULL && *q)
    st = grub_strtol (q, NULL, 10);
  grub_errno = GRUB_ERR_NONE;
  if (out)
    {
      grub_xputs = saved;
      *out = cap;
      *len = cap_len;
    }
  return st;
}

/* ------------------------------------------------------------------ */
/* the network card                                                   */
/* ------------------------------------------------------------------ */

/* GRUB's own stack polls the card only when it uses the network, which
   it does not while gork runs: every frame the card has is gork's. */
static struct grub_net_card *card;

#ifdef GRUB_MACHINE_EFI
/* Firmware that boots from a disk may leave the network card's driver
   unstarted (OVMF does), and efinet looks for cards once, when it loads:
   so every controller gets its drivers, and efinet looks again. */
static void
start_cards (void)
{
  grub_efi_boot_services_t *b = grub_efi_system_table->boot_services;
  grub_efi_handle_t *h;
  grub_efi_uintn_t n, i;
  grub_dl_t m;

  h = grub_efi_locate_handle (GRUB_EFI_ALL_HANDLES, NULL, NULL, &n);
  if (!h)
    {
      grub_errno = GRUB_ERR_NONE;
      return;
    }
  for (i = 0; i < n; i++)
    b->connect_controller (h[i], NULL, NULL, 1);
  grub_free (h);
  m = grub_dl_get ("efinet");
  if (m && m->fini && m->init)
    {
      m->fini ();
      m->init (m);
    }
  grub_errno = GRUB_ERR_NONE;
}
#endif

int
card_open (grub_uint8_t mac[6])
{
  const char *name = grub_env_get ("GORK_ETH");
  struct grub_net_card *c;

  card = NULL;
#ifdef GRUB_MACHINE_EFI
  if (!grub_net_cards)
    start_cards ();
#endif
  FOR_NET_CARDS (c)
    if (!name || !*name || grub_strcmp (c->name, name) == 0)
      {
	card = c;
	break;
      }
  if (!card)
    {
      grub_printf ("gork: no network card%s%s\n", name && *name ? " " : "",
		   name && *name ? name : "");
      return -1;
    }
  if (card->default_address.type != GRUB_NET_LINK_LEVEL_PROTOCOL_ETHERNET)
    {
      grub_printf ("gork: %s is not Ethernet\n", card->name);
      card = NULL;
      return -1;
    }
  if (!card->opened)
    {
      if (card->driver->open && card->driver->open (card) != GRUB_ERR_NONE)
	{
	  grub_errno = GRUB_ERR_NONE;
	  grub_printf ("gork: %s does not come up\n", card->name);
	  card = NULL;
	  return -1;
	}
      card->opened = 1;
    }
  grub_memcpy (mac, card->default_address.mac, 6);
  return 0;
}

int
card_send (const void *frame, grub_size_t len)
{
  struct grub_net_buff *nb;
  grub_err_t e;

  if (!card || !(nb = grub_netbuff_alloc (len)))
    return -1;
  if (grub_netbuff_put (nb, len) != GRUB_ERR_NONE)
    {
      grub_netbuff_free (nb);
      grub_errno = GRUB_ERR_NONE;
      return -1;
    }
  grub_memcpy (nb->data, frame, len);
  e = card->driver->send (card, nb);
  grub_netbuff_free (nb);
  grub_errno = GRUB_ERR_NONE;
  return e == GRUB_ERR_NONE ? 0 : -1;
}

int
card_recv (void *buf, grub_size_t size)
{
  struct grub_net_buff *nb;
  grub_size_t n;

  if (!card || !(nb = card->driver->recv (card)))
    {
      grub_errno = GRUB_ERR_NONE;
      return 0;
    }
  n = nb->tail - nb->data;
  if (n > size)
    n = size;
  grub_memcpy (buf, nb->data, n);
  grub_netbuff_free (nb);
  return n;
}

void card_pause (void) { grub_millisleep (1); }

void
card_break (void)
{
  if (bb_ctrlc ())
    {
      grub_printf ("\ngork: interrupted\n");
      gork_exit (130);
    }
}

unsigned
card_seed (void)
{
  grub_uint32_t lo, hi;

#ifdef __aarch64__
  grub_uint64_t t;

  asm volatile ("mrs %0, cntvct_el0" : "=r" (t));
  lo = t;
  hi = t >> 32;
#else
  asm volatile ("rdtsc" : "=a" (lo), "=d" (hi));
#endif
  return lo ^ hi ^ (unsigned) grub_get_time_ms ();
}

/* GRUB's mtu is the largest IP packet, but efinet cuts whole frames,
   their 14 bytes of Ethernet header too, at that length */
unsigned
card_mtu (void)
{
  unsigned m = card && card->mtu ? card->mtu : 1500;

  return (m > 1500 ? 1500 : m) - 14;
}

const char *
card_ip_hint (void)
{
  return "set GORK_IP=\"192.168.1.50/24 192.168.1.1\"";
}

/* ------------------------------------------------------------------ */
/* the command                                                        */
/* ------------------------------------------------------------------ */

static grub_jmp_buf done;
static int status;

/* gork's own stack: GRUB runs on the firmware's, and gork keeps a few
   paths of 4K each on it.  exit() comes back by longjmp, which puts
   GRUB's stack back; a return does too, below. */
#define GORK_STACK (1024 * 1024)
static char *gork_stack;

#ifdef __aarch64__
static int __attribute__ ((noinline))
run_on_own_stack (int argc, char **argv)
{
  register long r asm ("x0") = argc;
  register char **v asm ("x1") = argv;
  register char *top asm ("x2") = gork_stack + GORK_STACK;

  asm volatile ("mov x19, sp\n\t"	/* callee-saved: GRUB's stack */
		"mov sp, x2\n\t"
		"bl gork_start\n\t"
		"mov sp, x19"
		: "+r" (r), "+r" (v), "+r" (top)
		:
		: "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
		  "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x19",
		  "x30", "memory", "cc");
  return r;
}
#else
static int __attribute__ ((noinline))
run_on_own_stack (int argc, char **argv)
{
  register long a asm ("rdi") = argc;
  register char **v asm ("rsi") = argv;
  char *top = gork_stack + GORK_STACK;
  long r;

  asm volatile ("mov %%rsp, %%rbx\n\t"	/* callee-saved: GRUB's stack */
		"mov %[top], %%rsp\n\t"
		"movabs $gork_start, %%rax\n\t"
		"call *%%rax\n\t"
		"mov %%rbx, %%rsp"
		: "=a" (r), "+r" (a), "+r" (v)
		: [top] "r" (top)
		: "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory", "cc");
  return r;
}
#endif

void
bb_exit (int s)
{
  status = s;
  grub_longjmp (done, 1);
}

static grub_err_t
grub_cmd_gork (grub_command_t c __attribute__ ((unused)), int argc, char **args)
{
  static char **argv;
  int i;

  if (!gork_stack && !(gork_stack = grub_memalign (16, GORK_STACK)))
    return grub_errno;
  grub_free (argv);
  argv = grub_calloc (argc + 2, sizeof *argv);
  if (!argv)
    return grub_errno;
  argv[0] = (char *) "gork";
  for (i = 0; i < argc; i++)
    argv[i + 1] = args[i];
  status = 0;
  if (grub_setjmp (done) == 0)
    {
      gork_reset ();		/* its variables as at the first run */
      cwd_init ();
      status = run_on_own_stack (argc + 1, argv);
      gork_exit (status);	/* flushes, frees, comes back here */
    }
  card = NULL;
  grub_errno = GRUB_ERR_NONE;
  /* as `false`: $? is 1, and GRUB prints nothing */
  return status ? grub_error (GRUB_ERR_TEST_FAILURE, "gork") : GRUB_ERR_NONE;
}

static grub_command_t cmd;

GRUB_MOD_INIT (gork)
{
  gorkfs.mod = mod;
  grub_disk_dev_register (&gorkdev);
  grub_fs_register (&gorkfs);
  cmd = grub_register_command ("gork", grub_cmd_gork, N_("\"TASK\" | --version"),
			       N_("An AI agent: a task to the Messages API, with "
				  "tools on GRUB's files and shell. It writes "
				  "in (gork)/, in RAM. Settings in "
				  "$prefix/gork.cnf; network on gork's own "
				  "TCP/IP ($GORK_ETH, $GORK_IP, else DHCP)."));
}

GRUB_MOD_FINI (gork)
{
  grub_unregister_command (cmd);
  grub_fs_unregister (&gorkfs);
  grub_disk_dev_unregister (&gorkdev);
  while (nodes)
    ram_drop (nodes);
  grub_free (gork_stack);
  grub_free (gork_data_saved);
}
