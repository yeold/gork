/*
 * gork -- a lightweight AI agent harness for old and obscure operating
 * systems, most of which predate TLS.
 *
 * Runs the Messages API tool-use loop against api.anthropic.com through a
 * plaintext HTTP relay, executing tool calls locally.  Transport is
 * relay.c; JSON is vendored cJSON.
 *
 * Target: gcc 2.4.5+, libc 4.5 or glibc 2.0+, Linux 1.0+, the BSDs,
 * Solaris and illumos; DOS and Windows through port.h.
 * Strict C89.
 *
 * Usage:
 *   ./gork ["your task"]
 *   ./gork --version
 *
 * With a task: run it, print the answer on stdout, exit (batch mode).
 * Without:     interactive TUI (tui.c) that asks before writes and commands.
 *              The first time in a directory it asks whether to trust it,
 *              and remembers the answer in ~/.gork/trusted.
 *
 * Settings come from ~/.gork.conf (or $GORK_CONFIG), `key = value`.  On
 * DOS and Windows the file is gork.cnf and the directory _gork, beside
 * gork.exe or, on Windows, in %USERPROFILE% (see DOT_GORK and port.h):
 *   relay    host:port to connect to                  (required)
 *   host     Host: header value     env AGENT_HOST    (api.anthropic.com)
 *   path     request path           env AGENT_PATH    (/v1/messages)
 *   model    model id               env AGENT_MODEL   (claude-opus-5)
 *   api_key  x-api-key header       env ANTHROPIC_API_KEY
 *   allow    extra directories the file tools may use, `:`-separated
 *   max_turns, retries, context_window, auto_compact, default_effort,
 *   timeout, stream
 *            (see README)
 * A set environment variable beats the file.
 *
 * Other environment:
 *   GORK_TRACE  set: dump response headers and bodies to stderr
 *   GORK_ALLOW_RUN  set: offer run_command in batch mode (the TUI always
 *                    offers it, since it asks before each command)
 *
 * If GORK.md exists in the working directory it is sent as the system
 * prompt, followed by an index of the skills in .gork/skills/ and
 * ~/.gork/skills/.  File tools are confined to the working directory and
 * any `allow` directories (reads may also reach ~/.gork/skills).  MCP
 * servers listed in ~/.gork/mcp.json add their tools (mcp.c).
 *
 * The loop:  user turn -> POST -> if stop_reason is "tool_use", run each
 * tool_use block, send every result back in ONE user turn, repeat.  Anything
 * else ends the turn.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "port.h"
#if !defined(_WIN32) && !defined(GORK_DOS16)
#include <sys/wait.h>
#endif
#if !defined(GORK_DOS16) && !defined(GORK_NO_SELECT)
#include <sys/time.h>
#endif
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "relay.h"
#include "tui.h"
#include "mcp.h"
#include "cJSON/cJSON.h"

/* GNU/Hurd has no path length limit, so no PATH_MAX.
   ponytail: a fixed 4096 there; realpath() into a buffer of that size is
   only safe while paths stay shorter.  Use realpath(p, NULL) if that ever
   matters. */
#ifdef GORK_DOS16               /* DOS paths are short, and so is the stack */
#undef PATH_MAX
#define PATH_MAX 144
#define mkdir(dir, mode) mkdir(dir)
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef GORK_DOS16
#include <malloc.h>           /* _heapmin() */
#include <ctype.h>
#include <dos.h>              /* _dos_getvect(), intdosx() */
/* As DJGPP's realpath(): c:/dir/file, whether it exists or not.  The
   library's _fullpath() adds the drive and the current directory, but it
   takes only backslashes and leaves ".." in the path, which would lead out
   of the working directory.  So "." and ".." are taken out here. */
char *dos16_realpath(const char *path, char *out)
{
    char  in[PATH_MAX], *p, *w, *start;

    if (strlen(path) >= sizeof in) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(in, path);
    for (p = in; *p != '\0'; p++)
        if (*p == '/') *p = '\\';
    if (_fullpath(out, in, PATH_MAX) == NULL || out[1] != ':') {
        errno = ENOENT;
        return NULL;
    }
    for (p = out; *p != '\0'; p++)
        *p = *p == '\\' ? '/' : (char) tolower((unsigned char) *p);

    /* out is "c:/a/./b/../c": copy it over itself, one name at a time */
    start = w = out + 2;
    p = out + 2;
    while (*p != '\0') {
        size_t n;

        while (*p == '/') p++;
        n = strcspn(p, "/");
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            while (w > start && *--w != '/')    /* drop the last name */
                ;
        } else if (n > 0 && !(n == 1 && p[0] == '.')) {
            *w++ = '/';
            memmove(w, p, n);
            w += n;
        }
        p += n;
    }
    if (w == start) *w++ = '/';                 /* the root: "c:/" */
    *w = '\0';
    return out;
}
#endif

#ifdef GORK_EMX
/* EMX's _fullpath() adds the drive and current directory and takes "."
   and ".." out; existence is checked here. */
char *os2_realpath(const char *path, char *out)
{
    struct stat st;
    size_t      n;
    char       *p;

    if (_fullpath(out, path, PATH_MAX) != 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    for (p = out; *p != '\0'; p++)
        if (*p == '\\') *p = '/';
    n = strlen(out);
    if (n > 3 && out[n - 1] == '/') out[n - 1] = '\0';     /* not "c:/" */
    if (stat(out, &st) != 0) {
        errno = ENOENT;
        return NULL;
    }
    return out;
}
#endif

/* configure passes VERSION; this is for builds by hand and Makefile.dj.
   Bumped with configure.ac at each release. */
#ifndef VERSION
#define VERSION "1.2"
#endif

/* DOS (DJGPP) has 8.3 file names: no leading dot, one extension.  Windows
   gets the same names: COMMAND.COM on Windows 98 cannot copy to a name
   that starts with a dot, and Explorer will not create one. */
#ifdef GORK_NO_FORK
#define DOT_GORK  "_gork"
#define CONF_NAME "gork.cnf"
#else
#define DOT_GORK  ".gork"
#define CONF_NAME ".gork.conf"
#endif

#ifdef GORK_NO_FORK
#define ALLOW_SEP ";"           /* ':' belongs to the drive letter */
/* No fork, so no MCP servers: mcp.c is left out of the build. */
#define mcp_start(path)          ((void) (path))
#define mcp_stop()               ((void) 0)
#define mcp_tool_count()         0
#define mcp_add_tools(tools)     0
#define mcp_find(name)           (-1)
#define mcp_always(i)            ((int *) 0)
#define mcp_call(i, input, err)  ((char *) 0)
/* Text mode for the files the model reads and writes: it sees and sends
   bare newlines, the files keep their CR LF. */
#define RB "r"
#define WB "w"
#elif defined(GORK_EMX)        /* OS/2: drive letters, CR LF, but fork */
#define ALLOW_SEP ";"
#define RB "r"
#define WB "w"
#else
#define ALLOW_SEP ":"
#define RB "rb"
#define WB "wb"
#endif

/* Windows, OS/2 and UEFI file names do not tell upper case from lower. */
#if defined(_WIN32) || defined(GORK_EMX) || defined(GORK_EFI)
#define PATHNCMP strnicmp
#else
#define PATHNCMP strncmp
#endif

#define DEFAULT_MODEL "claude-opus-5"

/* max_tokens caps thinking AND response text together.  Thinking is on by
   default on Opus 5 and we deliberately do not send "thinking":{"disabled"}:
   with thinking off the model sometimes writes a tool call into its visible
   text instead of emitting a tool_use block, which this loop would silently
   treat as a finished turn. */
#ifdef GORK_DOS16               /* the reply has to fit in memory: see below */
#define MAX_TOKENS    3000
#else
#define MAX_TOKENS    16000
#endif

/* Defaults for `max_turns` (ceiling on tool round-trips per task, so a model
   that loops on a failing tool burns a bounded number of requests) and
   `retries` (extra attempts for a request that hits a network error, 429 or
   5xx). */
#define MAX_TURNS     "24"
#define RETRIES       "2"

/* Defaults for `context_window` (tokens the model can take) and
   `auto_compact` (compact once the context passes this percentage of the
   window; 0 turns it off). */
#ifdef GORK_DOS16
/* 16-bit DOS: a request is one block of memory, no block passes 64K, and
   cJSON prints none over 32K (INT_MAX).  So the conversation is kept well
   under that: compacting starts at 4000 tokens, some 16K of JSON, and the
   tools return little at a time. */
#define CTX_WINDOW    "8000"
#define AUTO_COMPACT  "50"
#define TOOL_READ_MAX 8192
#define TOOL_RUN_MAX  8192
#define TOOL_EDIT_MAX 30000
#else
#define CTX_WINDOW    "200000"
#define AUTO_COMPACT  "80"

#define TOOL_READ_MAX 65536     /* refuse to inline a file bigger than this */
#define TOOL_RUN_MAX  65536     /* command output beyond this is cut off */
#define TOOL_EDIT_MAX 1048576   /* edit_file works on files up to this size */
#endif
#define MAX_ALLOW     8         /* `allow` directories in the config */
#define LIST_MAX      2000      /* list_dir entries before it stops */
#define LIST_DEPTH    8         /* list_dir recursion limit */

#define RULES_FILE    "GORK.md"

/* realpath of the working directory at startup; file tools stay inside it. */
static char work_root[PATH_MAX];

static int interactive_mode;

/* The user trusted this directory with "e": file edits need no approval. */
static int trust_edits;

/* realpath of ~/.gork/skills, or "" -- readable, never writable. */
static char user_skills[PATH_MAX];

/* realpaths of the config's `allow` directories: readable and writable. */
static char allow_roots[MAX_ALLOW][PATH_MAX];
static int  nallow;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

/* strdup is not C89. */
/* dst = a followed by b, if that fits in `size` bytes: 0, or -1 if not.
   For the paths built from HOME and the configuration: no snprintf here. */
static int join(char *dst, size_t size, const char *a, const char *b)
{
    size_t na = strlen(a), nb;

    /* "C:\" + "/gork.cnf": MS-DOS 2 opens no path with "\/" in it */
    if (na > 0 && (a[na - 1] == '/' || a[na - 1] == '\\') && *b == '/') b++;
    nb = strlen(b);
    if (na >= size || nb >= size - na) return -1;
    memcpy(dst, a, na);
    memcpy(dst + na, b, nb + 1);
    return 0;
}

static char *kstrdup(const char *s)
{
    char  *p;
    size_t n = strlen(s) + 1;

    p = (char *) malloc(n);
    if (p != NULL) memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* settings                                                           */
/* ------------------------------------------------------------------ */

/* The environment beats ~/.gork.conf, which beats the default. */
enum { CFG_RELAY, CFG_MODEL, CFG_HOST, CFG_PATH, CFG_KEY, CFG_ALLOW,
       CFG_TURNS, CFG_RETRIES, CFG_WINDOW, CFG_COMPACT, CFG_EFFORT,
       CFG_TIMEOUT, CFG_PROTOCOL, CFG_STREAM, NCFG };

static const struct {
    const char *name, *env, *fallback;
} CFG[NCFG] = {
    { "relay",   NULL,                NULL                },
    { "model",   "AGENT_MODEL",       DEFAULT_MODEL       },
    { "host",    "AGENT_HOST",        "api.anthropic.com" },
    { "path",    "AGENT_PATH",        "/v1/messages"      },
    { "api_key", "ANTHROPIC_API_KEY", ""                  },
    { "allow",   NULL,                NULL                },
    { "max_turns", "AGENT_MAX_TURNS", MAX_TURNS           },
    { "retries", "AGENT_RETRIES",     RETRIES             },
    { "context_window", "AGENT_CONTEXT_WINDOW", CTX_WINDOW    },
    { "auto_compact", "AGENT_AUTO_COMPACT", AUTO_COMPACT      },
    { "default_effort", "AGENT_DEFAULT_EFFORT", ""            },
    { "timeout", "AGENT_TIMEOUT",     "120"               },
    { "protocol", "AGENT_PROTOCOL",   "anthropic"         },
    { "stream",  "AGENT_STREAM",      "auto"              }
};

/* `protocol = openai`: speak chat completions instead of the Messages API. */
static int openai;

/* The environment variable that overrides setting k.  An Anthropic key in
   the environment must not go to somebody else's server. */
static const char *cfg_env(int k)
{
    return openai && k == CFG_KEY ? "OPENAI_API_KEY" : CFG[k].env;
}

static char *cfg_file[NCFG];

/* Reasoning effort sent with every request; "" sends none (the model's own
   default).  Starts as default_effort, /effort changes it. */
static char  effort[8];

static int valid_effort(const char *e)
{
    return strcmp(e, "low") == 0 || strcmp(e, "medium") == 0 ||
           strcmp(e, "high") == 0 || strcmp(e, "xhigh") == 0 ||
           strcmp(e, "max") == 0;
}
static char  cfg_path[PATH_MAX];        /* the file cfg_file came from */

/* Where messages tell the user to put a setting.  DOS and Windows have no
   `~`, and the file is not where a Unix user would look, so there
   load_config() puts the full path here. */
#ifdef GORK_NO_FORK
static char  conf_hint[PATH_MAX] = CONF_NAME;
#else
static const char conf_hint[] = "~/" CONF_NAME;
#endif

static const char *cfg(int k)
{
    const char *v = cfg_env(k) != NULL ? getenv(cfg_env(k)) : NULL;

    if (v != NULL && *v != '\0') return v;
    if (cfg_file[k] != NULL) return cfg_file[k];
    return openai && k == CFG_PATH ? "/v1/chat/completions" : CFG[k].fallback;
}

/* An environment variable quietly beating the file is confusing -- a stale
   `export AGENT_MODEL` in an old shell looks exactly like a config bug --
   so say so whenever one changes a value the file sets. */
static void report_overrides(void)
{
    const char *v;
    int         k;

    for (k = 0; k < NCFG; k++) {
        v = cfg_env(k) != NULL ? getenv(cfg_env(k)) : NULL;
        if (v == NULL || *v == '\0' || cfg_file[k] == NULL ||
            strcmp(v, cfg_file[k]) == 0)
            continue;
        if (k == CFG_KEY)               /* never print a key */
            fprintf(stderr, "gork: api_key from %s overrides %s\n",
                    cfg_env(k), cfg_path);
        else
            fprintf(stderr, "gork: %s %s from %s overrides %s (%s)\n",
                    CFG[k].name, v, cfg_env(k), cfg_path, cfg_file[k]);
    }
}

static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t') s++;
    for (e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); e--)
        ;
    *e = '\0';
    return s;
}

/* Read `key = value` lines (# starts a comment line) into cfg_file.  No file
   at the default path is fine; a named one must exist.  Returns 0, or -1
   after reporting the problem. */
static int load_config(void)
{
    char        path[PATH_MAX], line[1024], *k, *v;
    const char *named = getenv("GORK_CONFIG"), *home = getenv("HOME");
    FILE       *f;
    struct stat st;
    int         n = 0, i;

    if (named != NULL && *named != '\0') {
        if (join(path, sizeof path, named, "") != 0) {
            fprintf(stderr, "gork: GORK_CONFIG path too long\n");
            return -1;
        }
    } else if (home != NULL &&
               join(path, sizeof path, home, "/" CONF_NAME) == 0) {
        named = NULL;
#ifdef GORK_NO_FORK
        strcpy(conf_hint, path);
#endif
    } else {
        return 0;
    }

    f = fopen(path, "r");
    if (f == NULL) {
        if (errno == ENOENT && named == NULL) return 0;
        fprintf(stderr, "gork: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    join(cfg_path, sizeof cfg_path, path, "");     /* the same size */

    while (fgets(line, sizeof line, f) != NULL) {
        n++;
        line[strcspn(line, "\r\n")] = '\0';
        k = trim(line);
        if (*k == '\0' || *k == '#') continue;

        v = strchr(k, '=');
        if (v != NULL) {
            *v++ = '\0';
            k = trim(k);
            v = trim(v);
        }
        for (i = 0; v != NULL && i < NCFG; i++)
            if (strcmp(k, CFG[i].name) == 0) break;
        if (v == NULL || i == NCFG) {
            fprintf(stderr, "gork: %s:%d: expected `key = value`, key one of "
                            "relay, model, host, path, api_key, allow, max_turns, retries, "
                            "context_window, auto_compact, default_effort, timeout, "
                            "protocol, stream\n", path, n);
            fclose(f);
            return -1;
        }
        free(cfg_file[i]);
        if ((cfg_file[i] = kstrdup(v)) == NULL) {
            fprintf(stderr, "gork: out of memory\n");
            fclose(f);
            return -1;
        }
    }

    /* The key is a secret: refuse a file others can read, as ssh does.
       DOS, Windows and OS/2 have no such modes; every file reads as 0644.
       Nor under Cygwin: a file made by a Windows program in the user's own
       profile shows group bits, for the access SYSTEM and the
       administrators have, and the packaged build has no chmod. */
#if !defined(GORK_NO_FORK) && !defined(__CYGWIN__) && !defined(GORK_EMX)
    if (cfg_file[CFG_KEY] != NULL && fstat(fileno(f), &st) == 0 &&
        (st.st_mode & 077) != 0) {
        fprintf(stderr, "gork: %s holds api_key but others can read it; "
                        "chmod 600 it\n", path);
        fclose(f);
        return -1;
    }
#else
    (void) st;
#endif

    fclose(f);
    return 0;
}

/* Read f into b until EOF or until b holds more than max bytes.
   Returns 0 at EOF, 1 if the cap was hit, -1 if out of memory. */
static int read_stream(FILE *f, struct buf *b, size_t max)
{
    char   chunk[4096];
    size_t n;

    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (buf_add(b, chunk, n) != 0) return -1;
        if (b->len > max) return 1;
    }
    return 0;
}

/* Is resolved path p inside directory root? */
static int under(const char *p, const char *root)
{
    size_t n = strlen(root);

    return n > 0 && PATHNCMP(p, root, n) == 0 &&
           (root[n - 1] == '/' || p[n] == '/' || p[n] == '\0');
}

/* Resolve the config's `allow` list (`:`-separated) into allow_roots.  A
   directory that does not resolve is an error, not a silent skip. */
static int load_allow(const char *list)
{
    char  copy[MAX_ALLOW * PATH_MAX], *p;

    if (list == NULL) return 0;
    if (join(copy, sizeof copy, list, "") != 0) {
        fprintf(stderr, "gork: allow list too long\n");
        return -1;
    }
    for (p = strtok(copy, ALLOW_SEP); p != NULL;
         p = strtok(NULL, ALLOW_SEP)) {
        p = trim(p);
        if (*p == '\0') continue;
        if (nallow == MAX_ALLOW) {
            fprintf(stderr, "gork: at most %d allow directories\n", MAX_ALLOW);
            return -1;
        }
        if (realpath(p, allow_roots[nallow]) == NULL) {
            fprintf(stderr, "gork: allow: %s: %s\n", p, strerror(errno));
            return -1;
        }
#ifdef __MSDOS__    /* DJGPP's realpath does not check that the path exists */
        {
            DIR *d = opendir(allow_roots[nallow]);

            if (d == NULL) {
                fprintf(stderr, "gork: allow: %s: no such directory\n", p);
                return -1;
            }
            closedir(d);
        }
#endif
        nallow++;
    }
    return 0;
}

/*
 * The model chooses tool paths, so they are untrusted.  Resolve `path`
 * (symlinks and .. included) into `out` and check it lies inside work_root
 * or an allow directory, or for reads, inside the user's skills.  A path that does not exist yet -- a file about to be written -- is checked
 * via its parent directory.  Returns NULL if allowed, else the reason.
 *
 * ponytail: check-then-open, so a symlink swapped in between the two can
 * still escape.  Fine against a model; not against a hostile local user.
 */
static const char *confine(const char *path, char *out, int writing)
{
    char        dir[PATH_MAX];
    const char *base;
    size_t      n;
    struct stat st;
    int         i;
#if defined(_WIN32) || defined(GORK_EMX) || defined(GORK_EFI)
    /* the model may write either slash */
    char        norm[PATH_MAX];

    if (strlen(path) >= sizeof norm) return "path too long";
    for (i = 0; (norm[i] = path[i] == '\\' ? '/' : path[i]) != '\0'; i++)
        ;
    path = norm;
#endif

    if (realpath(path, out) == NULL) {
        if (errno != ENOENT) return strerror(errno);

        base = strrchr(path, '/');
        if (base == NULL) {
            strcpy(dir, ".");
            base = path;
        } else {
            n = (base == path) ? 1 : (size_t) (base - path);   /* "/x" -> "/" */
#if defined(GORK_NO_FORK) || defined(GORK_EMX)
            if (path[n - 1] == ':') n++;    /* "c:/x" -> "c:/", "fs0:/x" too */
#endif
            if (n >= sizeof dir) return "path too long";
            memcpy(dir, path, n);
            dir[n] = '\0';
            base++;
        }
        if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
            return "not a file name";
        if (realpath(dir, out) == NULL) return strerror(errno);
        if (strlen(out) + 1 + strlen(base) >= PATH_MAX) return "path too long";
        if (out[strlen(out) - 1] != '/') strcat(out, "/");
        strcat(out, base);

        /* realpath said ENOENT, yet something is here: a dangling symlink,
           which fopen would follow to wherever it points. */
        if (lstat(out, &st) == 0) return "path is a dangling symlink";
    }

    if (under(out, work_root) || (!writing && under(out, user_skills)))
        return NULL;
    for (i = 0; i < nallow; i++)
        if (under(out, allow_roots[i])) return NULL;
    return "path is outside the working directory and allowed directories";
}

/*
 * Replace the file at `real` (already confined) with data.  Written to a
 * temp file beside it and renamed over, so a failed write never leaves a
 * truncated file; an existing file keeps its permission bits.  Returns NULL
 * on success, else the reason.
 */
static const char *save_file(const char *real, const char *data, size_t n)
{
    static char err[160];
    char        tmp[PATH_MAX + 16];
    struct stat st;
    FILE       *f;
    int         fd, e;

    if (strlen(real) + 11 >= sizeof tmp) return "path too long";
    strcpy(tmp, real);
#if defined(GORK_NO_FORK) || defined(GORK_EMX)
    {   /* DOS has one extension only, and so has a FAT disk under Windows
           NT 3.1 or OS/2: NAME.EXT is written as NAME.g$$ */
        char *dot = strrchr(tmp, '.'), *sl = strrchr(tmp, '/');
        if (dot != NULL && (sl == NULL || dot > sl)) *dot = '\0';
        strcat(tmp, ".g$$");
    }
#elif defined(GORK_MINIX2)
    /* Names stop at 14 characters there, so NAME.gork-tmp could be NAME
       itself: one fixed name beside it instead. */
    strcpy(strrchr(tmp, '/') + 1, ".gork-tmp");
#else
    strcat(tmp, ".gork-tmp");
#endif

    /* O_EXCL after removing any leftover, so a planted symlink at the temp
       name is replaced rather than followed out of the sandbox. */
    remove(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0666);
    f  = fd >= 0 ? fdopen(fd, WB) : NULL;
    if (f == NULL) {
        e = errno;
        if (fd >= 0) close(fd);
        sprintf(err, "cannot create file: %.100s", strerror(e));
        return err;
    }
    if (fwrite(data, 1, n, f) != n) {
        e = errno;
        fclose(f);
        goto fail;
    }
#if !defined(GORK_NO_FORK) && !defined(GORK_EMX) && !defined(GORK_MINIX2) && \
    !defined(GORK_BEOS) && !defined(GORK_PLAN9)
    /* the old file's mode, set on the open file: by name, something else
       could be standing there by now (EMX, Minix 2, BeOS and APE have no
       fchmod) */
    if (stat(real, &st) == 0) fchmod(fd, st.st_mode & 07777);
#endif
    if (fclose(f) != 0) {
        e = errno;
        goto fail;
    }
#if defined(GORK_NO_FORK) || defined(GORK_EMX) || defined(GORK_MINIX2) || \
    defined(GORK_BEOS) || defined(GORK_PLAN9)
    if (stat(real, &st) == 0) chmod(tmp, st.st_mode & 07777);
#endif
#if defined(_WIN32) || defined(GORK_DOS16) || defined(GORK_EMX)
    /* rename() does not replace a file here.  ponytail: not atomic; if the
       second rename fails the old file is gone. */
    if (rename(tmp, real) != 0 && (remove(real) != 0 || rename(tmp, real) != 0)) {
#else
    if (rename(tmp, real) != 0) {
#endif
        e = errno;
        goto fail;
    }
    return NULL;

fail:
    remove(tmp);
    sprintf(err, "write failed: %.100s", strerror(e));
    return err;
}

/* ------------------------------------------------------------------ */
/* skills                                                             */
/* ------------------------------------------------------------------ */

/* A skill is a directory holding SKILL.md, whose YAML frontmatter names it
   and says when to use it.  Only the index goes in the system prompt; the
   model pulls in the full instructions with the skill tool when needed. */
struct skill {
    char *name, *desc, *dir;
};

static struct skill *skills;
static int           nskills;

static char *unquote(char *v)
{
    size_t n = strlen(v);

    if (n >= 2 && (v[0] == '"' || v[0] == '\'') && v[n - 1] == v[0]) {
        v[n - 1] = '\0';
        v++;
    }
    return v;
}

/* Pull name and description out of the frontmatter.  Handles one-line values
   and the folded / literal blocks (`>`, `|`) that long descriptions use;
   anything fancier is not YAML we parse. */
static void parse_frontmatter(FILE *f, char **name, char **desc)
{
    char       line[2048], *k, *v;
    struct buf d;
    int        in_block = 0;

    *name = *desc = NULL;
    if (buf_init(&d) != 0) return;
    if (fgets(line, sizeof line, f) == NULL || strncmp(line, "---", 3) != 0) {
        buf_free(&d);
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "---", 3) == 0) break;

        if (line[0] == ' ' || line[0] == '\t') {        /* continuation */
            if (in_block && *(v = trim(line)) != '\0') {
                if (d.len > 0) buf_add(&d, " ", 1);
                buf_add(&d, v, strlen(v));
            }
            continue;
        }
        in_block = 0;
        v = strchr(line, ':');
        if (v == NULL) continue;
        *v++ = '\0';
        k = trim(line);
        v = unquote(trim(v));
        if (strcmp(k, "name") == 0) {
            free(*name);
            *name = kstrdup(v);
        } else if (strcmp(k, "description") == 0) {
            d.len = 0;
            d.p[0] = '\0';
            if (*v == '\0' || *v == '>' || *v == '|')
                in_block = 1;
            else
                buf_add(&d, v, strlen(v));
        }
    }
    if (d.len > 0)
        *desc = d.p;
    else
        buf_free(&d);
}

/* Add every <root>/<dir>/SKILL.md.  A name already loaded is skipped, so
   scanning the project before the user's directory lets the project win. */
static void scan_skills(const char *root)
{
    DIR           *dp;
    struct dirent *e;
    struct skill  *ns;
    char           path[PATH_MAX], *name, *desc;
    FILE          *f;
    int            i;

    dp = opendir(root);
    if (dp == NULL) return;

    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.' ||
            strlen(root) + strlen(e->d_name) + 16 >= sizeof path)
            continue;
        strcpy(path, root);                     /* length checked above */
        strcat(path, "/");
        strcat(path, e->d_name);
        strcat(path, "/SKILL.md");
        f = fopen(path, "r");
        if (f == NULL) continue;                /* not a skill directory */
        parse_frontmatter(f, &name, &desc);
        fclose(f);

        if (name == NULL) name = kstrdup(e->d_name);
        for (i = 0; name != NULL && i < nskills; i++)
            if (strcmp(skills[i].name, name) == 0) break;
        if (name == NULL || desc == NULL || i < nskills) {
            if (name != NULL && desc == NULL)
                fprintf(stderr, "gork: skipping %s: no description\n", path);
            free(name);
            free(desc);
            continue;
        }

        ns = (struct skill *) realloc(skills, (nskills + 1) * sizeof *skills);
        path[strlen(path) - 9] = '\0';         /* drop "/SKILL.md" */
        if (ns == NULL || (ns[nskills].dir = kstrdup(path)) == NULL) {
            if (ns != NULL) skills = ns;
            free(name);
            free(desc);
            continue;
        }
        skills = ns;
        skills[nskills].name = name;
        skills[nskills].desc = desc;
        nskills++;
    }
    closedir(dp);
}

/* Tell the model where else its file tools reach, or it will never try. */
static int add_allow_note(struct buf *b)
{
    int i;
    static const char a[] = "Besides the working directory (",
                      z[] = "), the file tools can also read and write "
                            "these directories; use absolute paths for them:";

    if (nallow == 0) return 0;
    if (b->len > 0 && buf_add(b, "\n\n", 2) != 0) return -1;
    if (buf_add(b, a, sizeof a - 1) != 0 ||
        buf_add(b, work_root, strlen(work_root)) != 0 ||
        buf_add(b, z, sizeof z - 1) != 0)
        return -1;
    for (i = 0; i < nallow; i++) {
        if (buf_add(b, " ", 1) != 0 ||
            buf_add(b, allow_roots[i], strlen(allow_roots[i])) != 0)
            return -1;
    }
    return buf_add(b, "\n", 1);
}

/* Append the skill index to the system prompt being built in b. */
static int add_skill_index(struct buf *b)
{
    static const char intro[] =
        "# Skills\n\n"
        "These skills hold instructions for particular kinds of task.  When "
        "a task matches a skill's description, call the skill tool with its "
        "name and follow what it returns before starting.\n\n";
    int i;

    if (nskills == 0) return 0;
    if (b->len > 0 && buf_add(b, "\n\n", 2) != 0) return -1;
    if (buf_add(b, intro, strlen(intro)) != 0) return -1;
    for (i = 0; i < nskills; i++) {
        if (buf_add(b, "- ", 2) != 0 ||
            buf_add(b, skills[i].name, strlen(skills[i].name)) != 0 ||
            buf_add(b, ": ", 2) != 0 ||
            buf_add(b, skills[i].desc, strlen(skills[i].desc)) != 0 ||
            buf_add(b, "\n", 1) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* tools                                                              */
/* ------------------------------------------------------------------ */

/*
 * A tool returns freshly malloc'd result text, or NULL if out of memory.
 * Set *is_err when the result describes a failure: the API wants that flagged
 * on the tool_result block so the model can recover instead of trusting it.
 */
typedef char *(*tool_fn)(cJSON *input, int *is_err);

struct tool {
    const char *name;
    const char *description;
    const char *schema;         /* JSON text, parsed into input_schema */
    tool_fn     fn;
};

static char *tool_read_file(cJSON *input, int *is_err)
{
    cJSON      *path;
    FILE       *f;
    struct buf  b;
    const char *why;
    char        real[PATH_MAX], msg[256];
    int         rc;

    *is_err = 1;

    path = cJSON_GetObjectItem(input, "path");
    if (!cJSON_IsString(path))
        return kstrdup("error: missing or non-string parameter \"path\"");

    if ((why = confine(path->valuestring, real, 0)) != NULL) {
        sprintf(msg, "error: %.200s", why);
        return kstrdup(msg);
    }
    f = fopen(real, RB);
    if (f == NULL) {
        sprintf(msg, "error: cannot open file: %s", strerror(errno));
        return kstrdup(msg);
    }

    if (buf_init(&b) != 0) {
        fclose(f);
        return NULL;
    }
    rc = read_stream(f, &b, TOOL_READ_MAX);
    fclose(f);
    if (rc != 0) {
        buf_free(&b);
        return rc < 0 ? NULL
                      : kstrdup("error: file too large to read in one call");
    }

    *is_err = 0;
    return b.p;                 /* ownership moves to the caller */
}

static char *tool_write_file(cJSON *input, int *is_err)
{
    cJSON      *path, *content;
    const char *why;
    char        real[PATH_MAX], msg[256];
    size_t      n;

    *is_err = 1;

    path    = cJSON_GetObjectItem(input, "path");
    content = cJSON_GetObjectItem(input, "content");
    if (!cJSON_IsString(path) || !cJSON_IsString(content))
        return kstrdup("error: \"path\" and \"content\" must be strings");

    n = strlen(content->valuestring);
    if ((why = confine(path->valuestring, real, 1)) != NULL ||
        (why = save_file(real, content->valuestring, n)) != NULL) {
        sprintf(msg, "error: %.200s", why);
        return kstrdup(msg);
    }

    sprintf(msg, "wrote %lu bytes", (unsigned long) n);
    *is_err = 0;
    return kstrdup(msg);
}

/* Replace exactly one occurrence of old_text.  Zero or several matches are
   refused: guessing which one was meant is how edits go wrong. */
static char *tool_edit_file(cJSON *input, int *is_err)
{
    cJSON      *path, *old, *new_;
    FILE       *f;
    struct buf  b, out;
    const char *why;
    char        real[PATH_MAX], msg[256], *hit, *p;
    size_t      olen;
    int         rc, count = 0;

    *is_err = 1;

    path = cJSON_GetObjectItem(input, "path");
    old  = cJSON_GetObjectItem(input, "old_text");
    new_ = cJSON_GetObjectItem(input, "new_text");
    if (!cJSON_IsString(path) || !cJSON_IsString(old) || !cJSON_IsString(new_))
        return kstrdup("error: \"path\", \"old_text\" and \"new_text\" "
                       "must be strings");
    olen = strlen(old->valuestring);
    if (olen == 0) return kstrdup("error: old_text is empty");

    if ((why = confine(path->valuestring, real, 1)) != NULL) {
        sprintf(msg, "error: %.200s", why);
        return kstrdup(msg);
    }
    f = fopen(real, RB);
    if (f == NULL) {
        sprintf(msg, "error: cannot open file: %s", strerror(errno));
        return kstrdup(msg);
    }
    if (buf_init(&b) != 0) {
        fclose(f);
        return NULL;
    }
    rc = read_stream(f, &b, TOOL_EDIT_MAX);
    fclose(f);
    if (rc != 0) {
        buf_free(&b);
        return rc < 0 ? NULL : kstrdup("error: file too large to edit");
    }

    hit = NULL;
    for (p = b.p; (p = strstr(p, old->valuestring)) != NULL; p += olen) {
        if (count++ == 0) hit = p;
    }
    if (count != 1) {
        buf_free(&b);
        return kstrdup(count == 0
            ? "error: old_text not found; read the file and copy it exactly"
            : "error: old_text matches more than once; include more "
              "surrounding lines so it is unique");
    }

    if (buf_init(&out) != 0 ||
        buf_add(&out, b.p, (size_t) (hit - b.p)) != 0 ||
        buf_add(&out, new_->valuestring, strlen(new_->valuestring)) != 0 ||
        buf_add(&out, hit + olen, b.len - (size_t) (hit - b.p) - olen) != 0) {
        buf_free(&b);
        buf_free(&out);
        return NULL;
    }
    buf_free(&b);

    why = save_file(real, out.p, out.len);
    buf_free(&out);
    if (why != NULL) {
        sprintf(msg, "error: %.200s", why);
        return kstrdup(msg);
    }
    *is_err = 0;
    return kstrdup("edited: replaced 1 occurrence");
}

/* Set when the user presses Esc; the running task stops at the next safe
   point, and every tool call still pending is declined. */
static int aborted;

/*
 * Runs anything the model asks, as the user running gork -- which is why
 * it is only offered when GORK_ALLOW_RUN is set.  No path confinement is
 * possible here; the shell can reach whatever the user can.
 *
 * The command gets its own process group, so a timeout kills everything it
 * started, and becomes the terminal's foreground job: its /dev/tty prompts
 * (ssh, ftp, sudo) still work, and the interrupt key -- Esc in the TUI, see
 * tui_suspend(), Ctrl-C in batch mode -- hits the command, not gork.
 */
#ifdef GORK_NO_FORK
/* DOS runs one program at a time, so there is no pipe to read while the
   command runs: its output goes to a temporary file, and there is no
   timeout and no interrupt.  The command line is COMMAND.COM's.  The
   Windows build does the same, with cmd.exe on the NT line. */
static char *tool_run_command(cJSON *input, int *is_err)
{
    cJSON      *cmd;
    struct buf  b;
    char        chunk[4096], msg[64], out[TMPNAM_MAX];
    FILE       *f;
    size_t      n;
    int         status, trunc = 0, fd, o1, o2;

    *is_err = 1;

    cmd = cJSON_GetObjectItem(input, "command");
    if (!cJSON_IsString(cmd))
        return kstrdup("error: missing or non-string parameter \"command\"");
    if (tmpnam(out) == NULL || buf_init(&b) != 0) return NULL;

    fprintf(stderr, "[run] %s\n", cmd->valuestring);
    tui_drain();
    fflush(stdout);
    fflush(stderr);
    /* Point 1 and 2 at the file ourselves: system() takes one redirection
       at most and has no 2>&1. */
    fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    o1 = dup(1);
    o2 = dup(2);
    if (fd < 0 || o1 < 0 || o2 < 0) {
        if (fd >= 0) close(fd);
        if (o1 >= 0) close(o1);
        if (o2 >= 0) close(o2);
        buf_free(&b);
        return kstrdup("error: cannot start command");
    }
    tui_suspend();
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
#ifdef GORK_DOS16
    /* The command interpreter needs room of its own in the 640K: hand
       back to DOS what the heap holds and does not use. */
    _heapmin();
#endif
    status = system(cmd->valuestring);
    dup2(o1, 1);
    dup2(o2, 2);
    close(o1);
    close(o2);
    tui_resume();

    if ((f = fopen(out, "r")) != NULL) {    /* text mode: no CRs */
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
            if (b.len > TOOL_RUN_MAX) {
                trunc = 1;
                break;
            }
            if (buf_add(&b, chunk, n) != 0) {
                fclose(f);
                remove(out);
                buf_free(&b);
                return NULL;
            }
        }
        fclose(f);
    }
    remove(out);
    if (trunc) {
        b.len = TOOL_RUN_MAX;
        b.p[b.len] = '\0';
    }
#ifdef _WIN32
    {   /* commands write the OEM code page; the API takes only UTF-8 */
        char *u = win_output_to_utf8(b.p);

        if (u != NULL) {
            b.len = 0;
            if (buf_add(&b, u, strlen(u)) != 0) {
                free(u);
                buf_free(&b);
                return NULL;
            }
            free(u);
        }
    }
#endif
    sprintf(msg, "%s[exit %d]", trunc ? "\n[output truncated]\n" : "", status);
    if (buf_add(&b, msg, strlen(msg)) != 0) {
        buf_free(&b);
        return NULL;
    }
    *is_err = status != 0;
    return b.p;
}
#else
static char *tool_run_command(cJSON *input, int *is_err)
{
    cJSON          *cmd;
    struct buf      b;
#ifndef GORK_NO_PIPE_SELECT
    struct timeval  tv;
    fd_set          rs;
#endif
    char            chunk[4096], msg[256];
    int             out[2], tty, fg = 0, status, n, trunc = 0, timed_out = 0;
    long            left;
    time_t          deadline;
    pid_t           pid;
    void          (*old)(int);

    *is_err = 1;

    cmd = cJSON_GetObjectItem(input, "command");
    if (!cJSON_IsString(cmd))
        return kstrdup("error: missing or non-string parameter \"command\"");
    if (buf_init(&b) != 0) return NULL;

    fprintf(stderr, "[run] %s\n", cmd->valuestring);
    tui_drain();
#ifdef GORK_EMX
    tty = -1;                   /* OS/2 has no job control: never fg */
#else
    tty = open("/dev/tty", O_RDWR);
    if (tty >= 0 && tcgetpgrp(tty) == getpgrp()) fg = 1;
#endif
    if (pipe(out) != 0) {
        sprintf(msg, "error: cannot start shell: %s", strerror(errno));
        goto fail;
    }
    tui_suspend();
    pid = fork();
    if (pid == 0) {
        /* stdin is /dev/null so a command that reads stdin fails instead of
           hanging; prompts go through /dev/tty instead. */
        setpgid(0, 0);
        if (fg) {
            signal(SIGTTOU, SIG_IGN);   /* we are background until this */
            tcsetpgrp(tty, getpid());
            signal(SIGTTOU, SIG_DFL);
        }
        if ((n = open("/dev/null", O_RDONLY)) >= 0) dup2(n, 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(out[0]);
        close(out[1]);
#ifdef GORK_EMX
        {
            const char *sh = getenv("COMSPEC");

            if (sh == NULL) sh = "cmd.exe";
            execlp(sh, sh, "/c", cmd->valuestring, (char *) NULL);
        }
#elif defined(GORK_PLAN9)
        /* /bin/sh is there under ape/psh; started from rc, take APE's
           shell directly, since the commands a model writes are sh, not
           rc */
        execl("/bin/sh", "sh", "-c", cmd->valuestring, (char *) NULL);
        execl("/bin/ape/sh", "sh", "-c", cmd->valuestring, (char *) NULL);
#else
        execl("/bin/sh", "sh", "-c", cmd->valuestring, (char *) NULL);
#endif
        _exit(127);
    }
    close(out[1]);
    if (pid < 0) {
        close(out[0]);
        tui_resume();
        sprintf(msg, "error: cannot start shell: %s", strerror(errno));
        goto fail;
    }
    setpgid(pid, pid);                  /* either of us may get there first */
    if (fg) {
        old = signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(tty, pid);
        signal(SIGTTOU, old);
    }

    /* Read to EOF or the deadline.  Past the cap, output is drained and
       dropped, so the command still runs to its end. */
    deadline = time(NULL) + relay_timeout;
    for (;;) {
        left = (long) (deadline - time(NULL));
        if (left <= 0) {
            timed_out = 1;
            break;
        }
#ifdef GORK_NO_PIPE_SELECT
        n = read_timed(out[0], chunk, sizeof chunk, left);
#else
        FD_ZERO(&rs);
        FD_SET(out[0], &rs);
        tv.tv_sec  = left;
        tv.tv_usec = 0;
        n = select(out[0] + 1, &rs, NULL, NULL, &tv);
        if (n < 0 && errno != EINTR) break;
        if (n <= 0) continue;
        n = read(out[0], chunk, sizeof chunk);
#endif
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (!trunc && buf_add(&b, chunk, (size_t) n) != 0) {
            killpg(pid, SIGKILL);
            trunc = -1;
            break;
        }
        if (b.len > TOOL_RUN_MAX) trunc = 1;
    }
    if (timed_out) killpg(pid, SIGKILL);
    close(out[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (fg) {
        old = signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(tty, getpgrp());
        signal(SIGTTOU, old);
    }
    if (tty >= 0) close(tty);
    tui_resume();
    if (trunc < 0) {
        buf_free(&b);
        return NULL;
    }
    if (trunc) {
        b.len = TOOL_RUN_MAX;
        b.p[b.len] = '\0';
    }

    if (timed_out)
        sprintf(msg, "%s[timed out after %d s, killed]",
                trunc ? "\n[output truncated]\n" : "", relay_timeout);
    else if (WIFEXITED(status))
        sprintf(msg, "%s[exit %d]", trunc ? "\n[output truncated]\n" : "",
                WEXITSTATUS(status));
    else if (WTERMSIG(status) == SIGINT && fg) {
        sprintf(msg, "%s[interrupted by the user]",
                trunc ? "\n[output truncated]\n" : "");
        aborted = 1;
    } else
        sprintf(msg, "%s[killed by signal %d]",
                trunc ? "\n[output truncated]\n" : "", WTERMSIG(status));

    if (buf_add(&b, msg, strlen(msg)) != 0) {
        buf_free(&b);
        return NULL;
    }
    *is_err = !(!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return b.p;

fail:
    if (tty >= 0) close(tty);
    buf_free(&b);
    return kstrdup(msg);
}
#endif

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char * const *) a, *(char * const *) b);
}

/* Append dir's entries to b, sorted, as paths relative to the listing's
   start (`rel`, "" at the top).  Dirs end in '/', symlinks in '@' and are
   never followed; hidden dirs are listed but not descended into.  Returns
   -1 only when out of memory. */
static int list_into(struct buf *b, const char *dir, const char *rel,
                     int recursive, int depth, int *count)
{
    DIR           *dp;
    struct dirent *e;
    struct stat    st;
    char         **names = NULL, **nn, full[PATH_MAX], sub[PATH_MAX];
    char           line[PATH_MAX + 64];
    int            n = 0, i, rc = 0;

    dp = opendir(dir);
    if (dp == NULL) {
        sprintf(line, "%.*s[cannot open: %s]\n", PATH_MAX, rel,
                strerror(errno));
        return buf_add(b, line, strlen(line));
    }
    while ((e = readdir(dp)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        nn = (char **) realloc(names, (n + 1) * sizeof *names);
        if (nn == NULL || (nn[n] = kstrdup(e->d_name)) == NULL) {
            if (nn != NULL) names = nn;
            rc = -1;
            break;
        }
        names = nn;
        n++;
    }
    closedir(dp);
    if (n > 0) qsort(names, n, sizeof *names, cmp_names);

    for (i = 0; i < n && rc == 0 && *count < LIST_MAX; i++) {
        if (strlen(dir) + strlen(names[i]) + 2 >= sizeof full ||
            strlen(rel) + strlen(names[i]) + 2 >= sizeof sub)
            continue;                           /* absurdly deep: skip */
        strcpy(full, dir);
        strcat(full, "/");
        strcat(full, names[i]);
        strcpy(sub, rel);
        strcat(sub, names[i]);
        if (lstat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode))
            sprintf(line, "%s/\n", sub);
        else if (S_ISLNK(st.st_mode))
            sprintf(line, "%s@\n", sub);
        else
            sprintf(line, "%s  (%lu bytes)\n", sub, (unsigned long) st.st_size);
        if (buf_add(b, line, strlen(line)) != 0) {
            rc = -1;
            break;
        }
        (*count)++;

        if (recursive && S_ISDIR(st.st_mode) && names[i][0] != '.' &&
            depth < LIST_DEPTH) {
            strcat(sub, "/");
            rc = list_into(b, full, sub, recursive, depth + 1, count);
        }
    }

    for (i = 0; i < n; i++) free(names[i]);
    free(names);
    return rc;
}

static char *tool_list_dir(cJSON *input, int *is_err)
{
    cJSON      *path, *rec;
    struct buf  b;
    const char *why;
    char        real[PATH_MAX], msg[256];
    struct stat st;
    int         count = 0;

    *is_err = 1;

    path = cJSON_GetObjectItem(input, "path");
    rec  = cJSON_GetObjectItem(input, "recursive");
    if (path != NULL && !cJSON_IsString(path))
        return kstrdup("error: \"path\" must be a string");

    if ((why = confine(path != NULL ? path->valuestring : ".", real, 0)) != NULL) {
        sprintf(msg, "error: %.200s", why);
        return kstrdup(msg);
    }
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode))
        return kstrdup("error: not a directory");

    if (buf_init(&b) != 0) return NULL;
    if (list_into(&b, real, "", cJSON_IsTrue(rec), 0, &count) != 0) {
        buf_free(&b);
        return NULL;
    }
    if (count >= LIST_MAX) {
        sprintf(msg, "[stopped after %d entries; list a subdirectory]\n",
                LIST_MAX);
        if (buf_add(&b, msg, strlen(msg)) != 0) {
            buf_free(&b);
            return NULL;
        }
    } else if (count == 0 && buf_add(&b, "(empty)\n", 8) != 0) {
        buf_free(&b);
        return NULL;
    }

    *is_err = 0;
    return b.p;
}

static char *tool_skill(cJSON *input, int *is_err)
{
    cJSON      *name;
    FILE       *f;
    struct buf  b;
    char        path[PATH_MAX + 32];
    int         i, rc;

    *is_err = 1;

    name = cJSON_GetObjectItem(input, "name");
    if (!cJSON_IsString(name))
        return kstrdup("error: missing or non-string parameter \"name\"");
    for (i = 0; i < nskills; i++)
        if (strcmp(skills[i].name, name->valuestring) == 0) break;
    if (i == nskills) return kstrdup("error: no such skill");

    sprintf(path, "%s/SKILL.md", skills[i].dir);
    f = fopen(path, RB);
    if (f == NULL) {
        sprintf(path, "error: cannot open SKILL.md: %s", strerror(errno));
        return kstrdup(path);
    }

    /* Tell the model where the skill lives so read_file can reach any
       files its instructions refer to. */
    sprintf(path, "Skill directory: %.*s\n\n", PATH_MAX - 1, skills[i].dir);
    if (buf_init(&b) != 0 || buf_add(&b, path, strlen(path)) != 0) {
        fclose(f);
        buf_free(&b);
        return NULL;
    }
    rc = read_stream(f, &b, TOOL_READ_MAX);
    fclose(f);
    if (rc != 0) {
        buf_free(&b);
        return rc < 0 ? NULL : kstrdup("error: SKILL.md is too large");
    }

    *is_err = 0;
    return b.p;
}

static const struct tool TOOLS[] = {
    { "list_dir",
      "List a directory: one entry per line, directories ending in '/', "
      "symlinks in '@', files with their size.  recursive also lists "
      "subdirectories (hidden ones are not descended into).",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"path\":{\"type\":\"string\","
                   "\"description\":\"Directory to list; defaults to the "
                                    "working directory.\"},"
         "\"recursive\":{\"type\":\"boolean\","
                        "\"description\":\"Also list subdirectories.\"}}}",
      tool_list_dir },
    { "read_file",
      "Read a file from disk and return its contents.",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"path\":{\"type\":\"string\","
                   "\"description\":\"Path to the file, relative to the "
                                    "working directory.\"}},"
       "\"required\":[\"path\"]}",
      tool_read_file },
    { "write_file",
      "Create or overwrite a file with the given contents.",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"path\":{\"type\":\"string\","
                   "\"description\":\"Path to the file, relative to the "
                                    "working directory.\"},"
         "\"content\":{\"type\":\"string\","
                      "\"description\":\"Full new contents of the file.\"}},"
       "\"required\":[\"path\",\"content\"]}",
      tool_write_file },
    { "edit_file",
      "Replace one exact snippet in a file.  old_text must appear exactly "
      "once, character for character (whitespace included); include enough "
      "surrounding lines to make it unique.  Prefer this to write_file for "
      "changing an existing file.",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"path\":{\"type\":\"string\","
                   "\"description\":\"Path to the file.\"},"
         "\"old_text\":{\"type\":\"string\","
                       "\"description\":\"Exact text to replace.\"},"
         "\"new_text\":{\"type\":\"string\","
                       "\"description\":\"Replacement text.\"}},"
       "\"required\":[\"path\",\"old_text\",\"new_text\"]}",
      tool_edit_file },
    { "run_command",
      "Run a shell command in the working directory and return its combined "
      "stdout and stderr, followed by its exit status.  stdin is empty.",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"command\":{\"type\":\"string\","
#ifdef __MSDOS__
                      "\"description\":\"Command line for DOS "
                      "(COMMAND.COM).\"}},"
#elif defined(GORK_EFI)
                      "\"description\":\"Command line for the UEFI "
                      "shell.\"}},"
#elif defined(_WIN32)
                      "\"description\":\"Command line for Windows "
                      "(cmd.exe; COMMAND.COM on Windows 9x).\"}},"
#elif defined(GORK_EMX)
                      "\"description\":\"Command line for OS/2 "
                      "(cmd.exe).\"}},"
#elif defined(__CYGWIN__)
                      "\"description\":\"Command line for /bin/sh, on "
                      "Windows (Cygwin): Windows programs can be run; the "
                      "Unix tools may be missing.\"}},"
#else
                      "\"description\":\"Command line for /bin/sh.\"}},"
#endif
       "\"required\":[\"command\"]}",
      tool_run_command },
    { "skill",
      "Load the full instructions of one of the skills listed in the system "
      "prompt.",
      "{\"type\":\"object\","
       "\"properties\":{"
         "\"name\":{\"type\":\"string\","
                   "\"description\":\"Skill name, as listed.\"}},"
       "\"required\":[\"name\"]}",
      tool_skill }
};

#define NTOOLS ((int) (sizeof TOOLS / sizeof TOOLS[0]))

/* run_command is opt-in in batch mode: when off it is neither offered nor
   runnable.  Interactive mode asks before every command instead. */
static int tool_enabled(const struct tool *t)
{
    if (t->fn == tool_skill) return nskills > 0;
    return t->fn != tool_run_command || interactive_mode ||
           getenv("GORK_ALLOW_RUN") != NULL;
}

/* "key: value" for each string field of a tool's input -- enough to see
   what a write or command will do before allowing it. */
static char *describe_input(cJSON *input)
{
    struct buf b;
    cJSON     *f;

    if (buf_init(&b) != 0) return NULL;
    cJSON_ArrayForEach(f, input) {
        char *v = cJSON_IsString(f) ? f->valuestring : cJSON_PrintUnformatted(f);

        if (f->string == NULL || v == NULL ||
            buf_add(&b, f->string, strlen(f->string)) != 0 ||
            buf_add(&b, ": ", 2) != 0 ||
            buf_add(&b, v, strlen(v)) != 0 ||
            buf_add(&b, "\n", 1) != 0) {
            if (!cJSON_IsString(f)) free(v);
            buf_free(&b);
            return NULL;
        }
        if (!cJSON_IsString(f)) free(v);
    }
    return b.p;
}

/* Interactive mode asks before anything but a read, Cline-style -- MCP
   tools included, since they can do anything -- and 'a' approves that tool
   for the rest of the session.  Batch mode never asks:
   writes stay confined and run_command is opt-in. */
static int always_ok[NTOOLS];      /* 'a' answers, per builtin tool */

/* Does this builtin need the user's OK in the TUI?  Reads never do; file
   edits don't in a directory trusted with "e". */
static int tool_asks(const struct tool *t)
{
    if (t->fn == tool_read_file || t->fn == tool_list_dir ||
        t->fn == tool_skill)
        return 0;
    return !(trust_edits &&
             (t->fn == tool_write_file || t->fn == tool_edit_file));
}

static int check_abort(void)
{
    if (!aborted && tui_poll_abort()) aborted = 1;
    return aborted;
}

static int approved(const char *name, int ask, int *always, cJSON *input)
{
    int   ans;
    char *detail;

    if (aborted) return 0;
    if (!interactive_mode || !ask || *always) return 1;
    detail = describe_input(input);
    if (detail == NULL) return 0;       /* never ask blind */
    ans = tui_confirm(name, detail);
    free(detail);
    if (ans == 'a') *always = 1;
    if (ans == 27) aborted = 1;
    return ans == 'y' || ans == 'a';
}

static const struct tool *tool_lookup(const char *name)
{
    int i;

    for (i = 0; i < NTOOLS; i++) {
        if (strcmp(TOOLS[i].name, name) == 0 && tool_enabled(&TOOLS[i]))
            return &TOOLS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* request construction                                               */
/* ------------------------------------------------------------------ */

/* Borrows `messages`: the returned object holds a reference, so deleting it
   leaves the conversation intact for the next turn.  `system` may be NULL. */
static cJSON *build_request(cJSON *messages, const char *system)
{
    cJSON *req, *tools, *t, *schema;
    int    i;

    req = cJSON_CreateObject();
    if (req == NULL) return NULL;

    if (cJSON_AddStringToObject(req, "model",
                                cfg(CFG_MODEL)) == NULL ||
        cJSON_AddNumberToObject(req, "max_tokens", MAX_TOKENS) == NULL)
        goto fail;
    if (system != NULL && cJSON_AddStringToObject(req, "system", system) == NULL)
        goto fail;
    /* Unset sends nothing: the model's own default, and local servers never
       see a field they may not know. */
    if (*effort != '\0') {
        t = cJSON_AddObjectToObject(req, "output_config");
        if (t == NULL || cJSON_AddStringToObject(t, "effort", effort) == NULL)
            goto fail;
    }

    tools = cJSON_AddArrayToObject(req, "tools");
    if (tools == NULL) goto fail;

    for (i = 0; i < NTOOLS; i++) {
        if (!tool_enabled(&TOOLS[i])) continue;
        t = cJSON_CreateObject();
        if (t == NULL) goto fail;
        cJSON_AddItemToArray(tools, t);

        schema = cJSON_Parse(TOOLS[i].schema);
        if (schema == NULL ||
            cJSON_AddStringToObject(t, "name", TOOLS[i].name) == NULL ||
            cJSON_AddStringToObject(t, "description",
                                    TOOLS[i].description) == NULL) {
            cJSON_Delete(schema);
            goto fail;
        }
        cJSON_AddItemToObject(t, "input_schema", schema);
    }
    if (mcp_add_tools(tools) != 0) goto fail;

    if (!cJSON_AddItemReferenceToObject(req, "messages", messages))
        goto fail;

    return req;

fail:
    cJSON_Delete(req);
    return NULL;
}

static int append_message(cJSON *messages, const char *role, cJSON *content)
{
    cJSON *msg = cJSON_CreateObject();

    if (msg == NULL) {
        cJSON_Delete(content);
        return -1;
    }
    if (cJSON_AddStringToObject(msg, "role", role) == NULL) {
        cJSON_Delete(msg);
        cJSON_Delete(content);
        return -1;
    }
    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);
    return 0;
}

static int append_text_message(cJSON *messages, const char *role,
                               const char *text)
{
    cJSON *content = cJSON_CreateString(text);

    if (content == NULL) return -1;
    return append_message(messages, role, content);
}

/* ------------------------------------------------------------------ */
/* response handling                                                  */
/* ------------------------------------------------------------------ */

/* Returns how many text blocks were printed. */
static int print_text_blocks(cJSON *content)
{
    cJSON *block, *type, *text;
    int    n = 0;

    cJSON_ArrayForEach(block, content) {
        type = cJSON_GetObjectItem(block, "type");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "text") != 0)
            continue;
        text = cJSON_GetObjectItem(block, "text");
        if (cJSON_IsString(text)) {
#ifdef _WIN32   /* a console shows its code page, not UTF-8; the TUI's
                   capture file and a redirected stdout get it as it is */
            char *shown = isatty(1) ? kstrdup(text->valuestring) : NULL;

            if (shown != NULL) {
                shown[win_utf8_to_console(shown, strlen(shown), 0)] = '\0';
                printf("%s\n", shown);
                free(shown);
            } else
#endif
            printf("%s\n", text->valuestring);
            n++;
        }
    }
    fflush(stdout);
    return n;
}

/*
 * Run every tool_use block in `content` and return the user message content
 * array holding one tool_result per block.  All results must travel in a
 * single user turn -- splitting them across turns teaches the model to stop
 * calling tools in parallel, and omitting one is rejected outright.
 */
static cJSON *run_tools(cJSON *content)
{
    cJSON            *results, *block, *type, *id, *name, *input, *res;
    const struct tool *tool;
    char             *out;
    int               is_err, m;

    results = cJSON_CreateArray();
    if (results == NULL) return NULL;

    cJSON_ArrayForEach(block, content) {
        type = cJSON_GetObjectItem(block, "type");
        if (!cJSON_IsString(type) ||
            strcmp(type->valuestring, "tool_use") != 0)
            continue;

        id    = cJSON_GetObjectItem(block, "id");
        name  = cJSON_GetObjectItem(block, "name");
        input = cJSON_GetObjectItem(block, "input");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) {
            fprintf(stderr, "gork: malformed tool_use block, skipping\n");
            continue;
        }

        fprintf(stderr, "[tool] %s\n", name->valuestring);

        is_err = 1;
        tool   = tool_lookup(name->valuestring);
        m      = tool == NULL ? mcp_find(name->valuestring) : -1;
        if (tool == NULL && m < 0)
            out = kstrdup("error: no such tool");
        else if (tool != NULL
                 ? !approved(tool->name,
                             tool_asks(tool),
                             &always_ok[tool - TOOLS], input)
                 : !approved(name->valuestring, 1, mcp_always(m), input))
            out = kstrdup("error: the user declined this action");
        else if (tool != NULL)
            out = tool->fn(input, &is_err);
        else
            out = mcp_call(m, input, &is_err);
        if (out == NULL) {
            cJSON_Delete(results);
            return NULL;
        }

        res = cJSON_CreateObject();
        if (res == NULL ||
            cJSON_AddStringToObject(res, "type", "tool_result") == NULL ||
            cJSON_AddStringToObject(res, "tool_use_id",
                                    id->valuestring) == NULL ||
            cJSON_AddStringToObject(res, "content", out) == NULL ||
            (is_err && cJSON_AddBoolToObject(res, "is_error", 1) == NULL)) {
            free(out);
            cJSON_Delete(res);
            cJSON_Delete(results);
            return NULL;
        }
        free(out);
        cJSON_AddItemToArray(results, res);
    }

    return results;
}

/* Report the API's own error JSON rather than a bare status code. */
static void report_api_error(int status, const char *body)
{
    cJSON *root, *err, *type, *msg;

    fprintf(stderr, "gork: API returned HTTP %d\n", status);

    root = cJSON_Parse(body);
    err  = cJSON_GetObjectItem(root, "error");
    type = cJSON_GetObjectItem(err, "type");
    msg  = cJSON_GetObjectItem(err, "message");

    if (cJSON_IsString(type) && cJSON_IsString(msg))
        fprintf(stderr, "gork: %s: %s\n", type->valuestring,
                msg->valuestring);
    else
        fprintf(stderr, "gork: %s\n", body);

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* trust                                                              */
/* ------------------------------------------------------------------ */

/* ~/.gork/trusted holds "trust <dir>" or "edits <dir>" lines; a trusted
   directory covers everything below it.  Returns 0 (unknown), 1 (trusted)
   or 2 (trusted, edits need no approval). */
static int trust_level(const char *file)
{
    FILE *f = fopen(file, "r");
    char  line[PATH_MAX + 16];
    int   best = 0, lvl;

    if (f == NULL) return 0;
    while (fgets(line, sizeof line, f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "edits ", 6) == 0)
            lvl = 2;
        else if (strncmp(line, "trust ", 6) == 0)
            lvl = 1;
        else
            continue;
        if (under(work_root, line + 6) && lvl > best) best = lvl;
    }
    fclose(f);
    return best;
}

/* Ask on the plain terminal, before the TUI and before anything from this
   directory (GORK.md, .gork/skills) is read into the model's prompt.
   Returns the level chosen, or 0 to quit. */
static int ask_trust(const char *file, const char *home)
{
    char  ans[16], dir[PATH_MAX], real_home[PATH_MAX];
    FILE *f;
    int   lvl, fd;

    fprintf(stderr,
        "gork will work in %s\n"
        "It reads files here, takes GORK.md and " DOT_GORK "/skills as "
        "instructions,\nand -- when you approve -- changes files and runs "
        "commands.\n", work_root);
    if (strcmp(work_root, "/") == 0 ||
        (realpath(home, real_home) != NULL && strcmp(work_root, real_home) == 0))
        fprintf(stderr, "Careful: that is %s, so gork could reach "
                        "everything under it.\n",
                strcmp(work_root, "/") == 0 ? "the root directory"
                                            : "your home directory");
    fprintf(stderr, "\nTrust this directory?\n"
                    "  y  yes\n"
                    "  e  yes, and let gork edit files here without asking\n"
                    "  n  no, quit\n> ");
    if (fgets(ans, sizeof ans, stdin) == NULL) return 0;
    lvl = (ans[0] == 'y' || ans[0] == 'Y') ? 1
        : (ans[0] == 'e' || ans[0] == 'E') ? 2 : 0;
    if (lvl == 0) return 0;

    if (join(dir, sizeof dir, file, "") == 0 && strrchr(dir, '/') != NULL) {
        *strrchr(dir, '/') = '\0';
        mkdir(dir, 0700);                       /* ~/.gork, if missing */
    }
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0600);   /* ours alone */
    f = fd >= 0 ? fdopen(fd, "a") : NULL;
    if (f == NULL && fd >= 0) close(fd);
    if (f == NULL || fprintf(f, "%s %s\n", lvl == 2 ? "edits" : "trust",
                             work_root) < 0 || fclose(f) != 0)
        fprintf(stderr, "gork: could not remember this in %s; you will "
                        "be asked again\n", file);
    return lvl;
}

/* ------------------------------------------------------------------ */
/* the loop                                                           */
/* ------------------------------------------------------------------ */

struct conn {
    const char *relay_host;
    long        relay_port;
    const char *api_host, *path, *key;
    const char *system;                 /* GORK.md, or NULL */
    const char *mcp_config;             /* ~/.gork/mcp.json, or NULL */
    int         have_rules;             /* GORK.md was found and non-empty */
    int         max_turns, retries;
    long        ctx_window;             /* tokens */
    int         auto_compact;           /* percent of ctx_window, 0 = off */
};

/* Tokens in the conversation as of the last reply: what that request sent
   plus what the model wrote back.  0 = not known yet. */
static long ctx_tokens;
static int  compactions;                /* bumped by every compact() */

static char status_line[PATH_MAX + 400];

static void update_status(const struct conn *c)
{
    char ctx[96];

    if (ctx_tokens > 0)
        sprintf(ctx, "ctx %ldk/%ldk (%ld%%)", (ctx_tokens + 500) / 1000,
                c->ctx_window / 1000, ctx_tokens * 100 / c->ctx_window);
    else
        strcpy(ctx, "ctx -");
    /* What matters most first: a narrow terminal cuts the bar on the right. */
    sprintf(status_line, " gork %.16s | %.90s | %.100s @ %.100s | effort %.32s | ",
            VERSION, ctx,
            cfg(CFG_MODEL), c->api_host, *effort ? effort : "default");
    /* the directory last, cut short if it must be */
    strncat(status_line, work_root,
            sizeof status_line - strlen(status_line) - 1);
}

static long usage_of(cJSON *usage, const char *field)
{
    cJSON *v = cJSON_GetObjectItem(usage, field);

    return cJSON_IsNumber(v) ? (long) v->valuedouble : 0;
}

/* ------------------------------------------------------------------ */
/* OpenAI chat completions                                            */
/* ------------------------------------------------------------------ */

/* With `protocol = openai` the conversation is still kept in Messages API
   form; only the request and the reply are translated, here, so the loop,
   the tools and compacting never see the difference. */

static cJSON *oa_message(cJSON *out, const char *role, const char *content)
{
    cJSON *m = cJSON_CreateObject();

    if (m == NULL) return NULL;
    cJSON_AddItemToArray(out, m);
    if (cJSON_AddStringToObject(m, "role", role) == NULL ||
        cJSON_AddStringToObject(m, "content", content) == NULL)
        return NULL;
    return m;
}

/* Append one Messages API message to `out` as chat messages: a tool_result
   becomes a "tool" message of its own, a tool_use a tool_calls entry. */
static int oa_convert(cJSON *out, cJSON *msg)
{
    cJSON     *role = cJSON_GetObjectItem(msg, "role");
    cJSON     *content = cJSON_GetObjectItem(msg, "content");
    cJSON     *b, *type, *v, *id, *m, *call, *fn, *calls = NULL;
    struct buf txt;
    char      *args;
    int        asst;

    if (!cJSON_IsString(role)) return 0;
    if (cJSON_IsString(content))
        return oa_message(out, role->valuestring,
                          content->valuestring) != NULL ? 0 : -1;
    asst = strcmp(role->valuestring, "assistant") == 0;
    if (buf_init(&txt) != 0) return -1;

    cJSON_ArrayForEach(b, content) {
        type = cJSON_GetObjectItem(b, "type");
        if (!cJSON_IsString(type)) continue;
        if (strcmp(type->valuestring, "text") == 0) {
            v = cJSON_GetObjectItem(b, "text");
            if (!cJSON_IsString(v)) continue;
            if ((txt.len > 0 && buf_add(&txt, "\n", 1) != 0) ||
                buf_add(&txt, v->valuestring, strlen(v->valuestring)) != 0)
                goto fail;
        } else if (!asst && strcmp(type->valuestring, "tool_result") == 0) {
            v  = cJSON_GetObjectItem(b, "content");
            id = cJSON_GetObjectItem(b, "tool_use_id");
            m  = oa_message(out, "tool",
                            cJSON_IsString(v) ? v->valuestring : "");
            if (m == NULL ||
                cJSON_AddStringToObject(m, "tool_call_id", cJSON_IsString(id)
                                        ? id->valuestring : "") == NULL)
                goto fail;
        } else if (asst && strcmp(type->valuestring, "tool_use") == 0) {
            id = cJSON_GetObjectItem(b, "id");
            v  = cJSON_GetObjectItem(b, "name");
            if (calls == NULL && (calls = cJSON_CreateArray()) == NULL)
                goto fail;
            call = cJSON_CreateObject();
            if (call == NULL) goto fail;
            cJSON_AddItemToArray(calls, call);
            args = cJSON_PrintUnformatted(cJSON_GetObjectItem(b, "input"));
            fn   = cJSON_AddObjectToObject(call, "function");
            if (fn == NULL ||
                cJSON_AddStringToObject(call, "id", cJSON_IsString(id)
                                        ? id->valuestring : "") == NULL ||
                cJSON_AddStringToObject(call, "type", "function") == NULL ||
                cJSON_AddStringToObject(fn, "name", cJSON_IsString(v)
                                        ? v->valuestring : "") == NULL ||
                cJSON_AddStringToObject(fn, "arguments",
                                        args != NULL ? args : "{}") == NULL) {
                free(args);
                goto fail;
            }
            free(args);
        }
    }

    /* Text of a user turn goes after its tool results: those have to
       follow the assistant's tool_calls directly. */
    if (asst || txt.len > 0) {
        m = oa_message(out, role->valuestring, txt.p);
        if (m == NULL) goto fail;
        if (calls != NULL) cJSON_AddItemToObject(m, "tool_calls", calls);
        calls = NULL;
    }
    buf_free(&txt);
    return 0;

fail:
    cJSON_Delete(calls);
    buf_free(&txt);
    return -1;
}

/* The chat completions request for a Messages API one; NULL if out of
   memory.  No token limit is sent: servers disagree on the name of the
   field (max_tokens, max_completion_tokens), and all have a default. */
static cJSON *to_openai(cJSON *req)
{
    cJSON *out = cJSON_CreateObject(), *msgs, *tools, *t, *w, *fn, *v;

    if (out == NULL) return NULL;
    if (cJSON_AddStringToObject(out, "model", cfg(CFG_MODEL)) == NULL ||
        (*effort != '\0' &&
         cJSON_AddStringToObject(out, "reasoning_effort", effort) == NULL))
        goto fail;

    msgs = cJSON_AddArrayToObject(out, "messages");
    if (msgs == NULL) goto fail;
    v = cJSON_GetObjectItem(req, "system");
    if (cJSON_IsString(v) &&
        oa_message(msgs, "system", v->valuestring) == NULL)
        goto fail;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(req, "messages"))
        if (oa_convert(msgs, t) != 0) goto fail;

    tools = cJSON_CreateArray();
    if (tools == NULL) goto fail;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(req, "tools")) {
        w = cJSON_CreateObject();
        if (w == NULL) break;
        cJSON_AddItemToArray(tools, w);
        fn = cJSON_AddObjectToObject(w, "function");
        v  = cJSON_Duplicate(cJSON_GetObjectItem(t, "input_schema"), 1);
        if (v != NULL) cJSON_AddItemToObject(fn, "parameters", v);
        if (fn == NULL || v == NULL ||
            cJSON_AddStringToObject(w, "type", "function") == NULL ||
            !cJSON_AddItemToObject(fn, "name", cJSON_Duplicate(
                cJSON_GetObjectItem(t, "name"), 1)) ||
            !cJSON_AddItemToObject(fn, "description", cJSON_Duplicate(
                cJSON_GetObjectItem(t, "description"), 1)))
            break;
    }
    if (t != NULL) {                    /* left the loop early */
        cJSON_Delete(tools);
        goto fail;
    }
    if (cJSON_GetArraySize(tools) > 0)
        cJSON_AddItemToObject(out, "tools", tools);
    else
        cJSON_Delete(tools);            /* an empty list is refused */
    return out;

fail:
    cJSON_Delete(out);
    return NULL;
}

/* Turn a chat completions reply into the Messages API reply the loop
   expects.  Takes `oa`; returns NULL after reporting what is wrong. */
static cJSON *from_openai(cJSON *oa)
{
    cJSON      *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(oa, "choices"), 0);
    cJSON      *msg = cJSON_GetObjectItem(choice, "message");
    cJSON      *usage = cJSON_GetObjectItem(oa, "usage");
    cJSON      *root, *content, *b, *v, *call, *fn, *in;
    const char *stop = "end_turn";
    char        idbuf[32];
    int         n = 0;

    if (!cJSON_IsObject(msg)) {
        fprintf(stderr, "gork: reply had no choices[0].message\n");
        cJSON_Delete(oa);
        return NULL;
    }
    root    = cJSON_CreateObject();
    content = root != NULL ? cJSON_AddArrayToObject(root, "content") : NULL;
    if (content == NULL) goto oom;

    v = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(v) && v->valuestring[0] != '\0') {
        b = cJSON_CreateObject();
        if (b == NULL) goto oom;
        cJSON_AddItemToArray(content, b);
        if (cJSON_AddStringToObject(b, "type", "text") == NULL ||
            cJSON_AddStringToObject(b, "text", v->valuestring) == NULL)
            goto oom;
    }

    v = cJSON_GetObjectItem(choice, "finish_reason");
    if (cJSON_IsString(v) && strcmp(v->valuestring, "length") == 0)
        stop = "max_tokens";
    else if (cJSON_IsString(v) && strcmp(v->valuestring, "content_filter") == 0)
        stop = "refusal";

    /* Go by the calls, not by finish_reason: some servers say "stop". */
    cJSON_ArrayForEach(call, cJSON_GetObjectItem(msg, "tool_calls")) {
        fn = cJSON_GetObjectItem(call, "function");
        v  = cJSON_GetObjectItem(fn, "arguments");
        in = cJSON_IsString(v) ? cJSON_Parse(v->valuestring)
                               : cJSON_Duplicate(v, 1);
        if (!cJSON_IsObject(in)) {      /* the tool reports what is missing */
            cJSON_Delete(in);
            in = cJSON_CreateObject();
        }
        b = cJSON_CreateObject();
        if (b == NULL || in == NULL) {
            cJSON_Delete(b);
            cJSON_Delete(in);
            goto oom;
        }
        cJSON_AddItemToArray(content, b);
        cJSON_AddItemToObject(b, "input", in);
        sprintf(idbuf, "call_%d", n++);
        v = cJSON_GetObjectItem(call, "id");
        if (cJSON_AddStringToObject(b, "type", "tool_use") == NULL ||
            cJSON_AddStringToObject(b, "id", cJSON_IsString(v) &&
                                    v->valuestring[0] != '\0'
                                    ? v->valuestring : idbuf) == NULL)
            goto oom;
        v = cJSON_GetObjectItem(fn, "name");
        if (cJSON_AddStringToObject(b, "name", cJSON_IsString(v)
                                    ? v->valuestring : "") == NULL)
            goto oom;
        stop = "tool_use";
    }

    if (cJSON_AddStringToObject(root, "stop_reason", stop) == NULL) goto oom;
    if (usage != NULL) {
        v = cJSON_AddObjectToObject(root, "usage");
        if (v == NULL ||
            cJSON_AddNumberToObject(v, "input_tokens", (double)
                usage_of(usage, "prompt_tokens")) == NULL ||
            cJSON_AddNumberToObject(v, "output_tokens", (double)
                usage_of(usage, "completion_tokens")) == NULL)
            goto oom;
    }
    cJSON_Delete(oa);
    return root;

oom:
    fprintf(stderr, "gork: out of memory reading reply\n");
    cJSON_Delete(root);
    cJSON_Delete(oa);
    return NULL;
}

/* Sleep n seconds, giving up early if the user aborts. */
static void wait_secs(int n)
{
    while (n-- > 0 && !(relay_abort != NULL && relay_abort()))
        sleep(1);
}

/* ------------------------------------------------------------------ */
/* streamed replies                                                   */
/* ------------------------------------------------------------------ */

/*
 * With `"stream": true` the reply comes as server-sent events: lines of
 * `data: {json}`, each a small piece.  The text is printed as it comes,
 * and the pieces are put back together into the message a plain reply
 * would have been, since that is what goes back to the model, tool_use
 * and thinking blocks included.  A server that ignores the request and
 * answers with plain JSON is taken as it is.
 */
static int want_stream;         /* the `stream` setting, resolved */
static int no_stream;           /* this request's text is not for the user */
static int streamed_said = -1;  /* text blocks the last post() printed
                                   itself; -1: none, the caller prints */

#ifndef GORK_BARE
#define STREAM_CALLS 16         /* tool calls in one OpenAI reply */

static struct {
    struct buf line;            /* the unfinished line of the stream */
    struct buf acc;             /* text, thinking or tool input so far */
    struct buf sig;             /* signature of a thinking block */
    struct buf plain;           /* the whole body, when it is no stream */
    struct buf args[STREAM_CALLS];      /* OpenAI: arguments of each call */
    int        sse;             /* -1 not known yet, 0 plain JSON, 1 events */
    cJSON     *root;            /* the message being put together */
    cJSON     *block;           /* its content block that is open */
    cJSON     *calls;           /* OpenAI: the tool calls so far */
    char       finish[32];      /* OpenAI: finish_reason */
    int        said, midline, printed, failed, done, oom;
} st;

static void stream_reset(void)
{
    int i;

    st.oom = 0;
    if (st.line.p == NULL) {            /* first use */
        if (buf_init(&st.line) != 0 || buf_init(&st.acc) != 0 ||
            buf_init(&st.sig) != 0 || buf_init(&st.plain) != 0)
            st.oom = 1;
    }
    st.line.len = st.acc.len = st.sig.len = st.plain.len = 0;
    for (i = 0; i < STREAM_CALLS; i++)  /* these come into being when used */
        if (st.args[i].p != NULL) {
            st.args[i].len = 0;
            st.args[i].p[0] = '\0';
        }
    if (st.acc.p != NULL) st.acc.p[0] = '\0';
    if (st.sig.p != NULL) st.sig.p[0] = '\0';
    cJSON_Delete(st.root);
    cJSON_Delete(st.calls);
    st.root = st.block = st.calls = NULL;
    st.finish[0] = '\0';
    st.sse = -1;
    st.said = st.midline = st.printed = st.failed = st.done = 0;
}

/* A piece of the answer, on the screen at once. */
static void stream_text(const char *t)
{
    if (*t == '\0') return;
#ifdef _WIN32
    if (isatty(1)) {
        char *shown = kstrdup(t);

        if (shown != NULL) {
            shown[win_utf8_to_console(shown, strlen(shown), 0)] = '\0';
            fputs(shown, stdout);
            free(shown);
        }
    } else
#endif
    fputs(t, stdout);
    fflush(stdout);
    st.midline = st.printed = 1;
}

static void stream_text_end(void)
{
    if (!st.midline) return;
    putchar('\n');
    fflush(stdout);
    st.midline = 0;
    st.said++;
}

static const char *str_of(cJSON *obj, const char *name)
{
    cJSON *v = cJSON_GetObjectItem(obj, name);

    return cJSON_IsString(v) ? v->valuestring : "";
}

/* Give object `obj` the string `value` under `name`, replacing what is there. */
static void set_string(cJSON *obj, const char *name, const char *value)
{
    cJSON *v = cJSON_CreateString(value);

    if (v == NULL) {
        st.oom = 1;
        return;
    }
    if (cJSON_GetObjectItem(obj, name) != NULL)
        cJSON_ReplaceItemInObject(obj, name, v);
    else
        cJSON_AddItemToObject(obj, name, v);
}

/* One event of the Messages API. */
static void stream_anthropic(cJSON *ev)
{
    const char *type = str_of(ev, "type"), *dt;
    cJSON      *v, *d, *u;

    if (strcmp(type, "message_start") == 0) {
        cJSON_Delete(st.root);
        st.root = cJSON_CreateObject();
        if (st.root == NULL || cJSON_AddArrayToObject(st.root, "content") == NULL) {
            st.oom = 1;
            return;
        }
        u = cJSON_Duplicate(cJSON_GetObjectItem(
                cJSON_GetObjectItem(ev, "message"), "usage"), 1);
        if (u != NULL) cJSON_AddItemToObject(st.root, "usage", u);
    } else if (st.root == NULL) {
        if (strcmp(type, "error") == 0) goto error;
        return;                         /* nothing to add it to */
    } else if (strcmp(type, "content_block_start") == 0) {
        st.block = cJSON_Duplicate(cJSON_GetObjectItem(ev, "content_block"), 1);
        if (st.block == NULL) {
            st.oom = 1;
            return;
        }
        cJSON_AddItemToArray(cJSON_GetObjectItem(st.root, "content"), st.block);
        st.acc.len = st.sig.len = 0;
        st.acc.p[0] = st.sig.p[0] = '\0';
        if (strcmp(str_of(st.block, "type"), "text") == 0) {
            dt = str_of(st.block, "text");
            if (buf_add(&st.acc, dt, strlen(dt)) != 0) st.oom = 1;
            stream_text(dt);
        }
    } else if (strcmp(type, "content_block_delta") == 0) {
        d = cJSON_GetObjectItem(ev, "delta");
        dt = str_of(d, "type");
        if (st.block == NULL) return;
        if (strcmp(dt, "text_delta") == 0) {
            dt = str_of(d, "text");
            stream_text(dt);
        } else if (strcmp(dt, "input_json_delta") == 0) {
            dt = str_of(d, "partial_json");
        } else if (strcmp(dt, "thinking_delta") == 0) {
            dt = str_of(d, "thinking");
        } else if (strcmp(dt, "signature_delta") == 0) {
            dt = str_of(d, "signature");
            if (buf_add(&st.sig, dt, strlen(dt)) != 0) st.oom = 1;
            return;
        } else {
            return;
        }
        if (buf_add(&st.acc, dt, strlen(dt)) != 0) st.oom = 1;
    } else if (strcmp(type, "content_block_stop") == 0) {
        if (st.block == NULL) return;
        dt = str_of(st.block, "type");
        if (strcmp(dt, "text") == 0) {
            set_string(st.block, "text", st.acc.p);
            stream_text_end();
        } else if (strcmp(dt, "thinking") == 0) {
            set_string(st.block, "thinking", st.acc.p);
            if (st.sig.len > 0) set_string(st.block, "signature", st.sig.p);
        } else if (strcmp(dt, "tool_use") == 0) {
            v = st.acc.len > 0 ? cJSON_Parse(st.acc.p) : NULL;
            if (!cJSON_IsObject(v)) {   /* the tool reports what is missing */
                cJSON_Delete(v);
                v = cJSON_CreateObject();
            }
            if (v == NULL)
                st.oom = 1;
            else if (cJSON_GetObjectItem(st.block, "input") != NULL)
                cJSON_ReplaceItemInObject(st.block, "input", v);
            else
                cJSON_AddItemToObject(st.block, "input", v);
        }
        st.block = NULL;
    } else if (strcmp(type, "message_delta") == 0) {
        dt = str_of(cJSON_GetObjectItem(ev, "delta"), "stop_reason");
        if (*dt != '\0') set_string(st.root, "stop_reason", dt);
        u = cJSON_GetObjectItem(st.root, "usage");
        if (u == NULL) u = cJSON_AddObjectToObject(st.root, "usage");
        cJSON_ArrayForEach(v, cJSON_GetObjectItem(ev, "usage")) {
            if (u == NULL || !cJSON_IsNumber(v) || v->string == NULL) continue;
            d = cJSON_CreateNumber(v->valuedouble);
            if (d == NULL)
                st.oom = 1;
            else if (cJSON_GetObjectItem(u, v->string) != NULL)
                cJSON_ReplaceItemInObject(u, v->string, d);
            else
                cJSON_AddItemToObject(u, v->string, d);
        }
    } else if (strcmp(type, "message_stop") == 0) {
        st.done = 1;
    } else if (strcmp(type, "error") == 0) {
        goto error;
    }
    return;

error:
    fprintf(stderr, "gork: the API reported an error in the reply: %s\n",
            str_of(cJSON_GetObjectItem(ev, "error"), "message"));
    st.failed = 1;
}

/* One chunk of the OpenAI chat completions API. */
static void stream_openai(cJSON *ev)
{
    cJSON      *choice = cJSON_GetArrayItem(cJSON_GetObjectItem(ev, "choices"), 0);
    cJSON      *delta = cJSON_GetObjectItem(choice, "delta");
    cJSON      *v, *call, *fn, *mine;
    const char *t;
    int         i;

    v = cJSON_GetObjectItem(ev, "error");
    if (v != NULL && !cJSON_IsNull(v)) {
        fprintf(stderr, "gork: the API reported an error in the reply: %s\n",
                cJSON_IsString(v) ? v->valuestring : str_of(v, "message"));
        st.failed = 1;
        return;
    }
    v = cJSON_GetObjectItem(ev, "usage");       /* the last chunk has it */
    if (cJSON_IsObject(v)) {
        if (st.root == NULL) st.root = cJSON_CreateObject();
        if (st.root != NULL) {
            cJSON_DeleteItemFromObject(st.root, "usage");
            cJSON_AddItemToObject(st.root, "usage", cJSON_Duplicate(v, 1));
        }
    }
    t = str_of(delta, "content");
    if (*t != '\0') {
        if (buf_add(&st.acc, t, strlen(t)) != 0) st.oom = 1;
        stream_text(t);
    }
    cJSON_ArrayForEach(call, cJSON_GetObjectItem(delta, "tool_calls")) {
        v = cJSON_GetObjectItem(call, "index");
        i = cJSON_IsNumber(v) ? v->valueint : 0;
        if (i < 0 || i >= STREAM_CALLS) continue;
        if (st.calls == NULL) st.calls = cJSON_CreateArray();
        while (st.calls != NULL && cJSON_GetArraySize(st.calls) <= i) {
            mine = cJSON_CreateObject();
            if (mine == NULL || cJSON_AddObjectToObject(mine, "function") == NULL) {
                cJSON_Delete(mine);
                st.oom = 1;
                return;
            }
            cJSON_AddItemToArray(st.calls, mine);
        }
        mine = cJSON_GetArrayItem(st.calls, i);
        if (mine == NULL) {
            st.oom = 1;
            return;
        }
        t = str_of(call, "id");
        if (*t != '\0') set_string(mine, "id", t);
        fn = cJSON_GetObjectItem(call, "function");
        t = str_of(fn, "name");
        if (*t != '\0') set_string(cJSON_GetObjectItem(mine, "function"), "name", t);
        t = str_of(fn, "arguments");
        if ((st.args[i].p == NULL && buf_init(&st.args[i]) != 0) ||
            buf_add(&st.args[i], t, strlen(t)) != 0)
            st.oom = 1;
    }
    t = str_of(choice, "finish_reason");
    if (*t != '\0' && strlen(t) < sizeof st.finish) strcpy(st.finish, t);
}

static void stream_line(char *l)
{
    cJSON *ev;

    if (strncmp(l, "data:", 5) != 0) return;    /* event:, comments, blanks */
    l += 5;
    while (*l == ' ') l++;
    if (getenv("GORK_TRACE") != NULL) fprintf(stderr, "%s\n", l);
    if (strcmp(l, "[DONE]") == 0) {
        st.done = 1;
        return;
    }
    ev = cJSON_Parse(l);
    if (ev == NULL) return;
    if (openai)
        stream_openai(ev);
    else
        stream_anthropic(ev);
    cJSON_Delete(ev);
}

/* relay.c hands the body here as it arrives. */
static void stream_feed(const char *p, size_t n)
{
    char  *nl;
    size_t used;

    if (st.oom) return;                 /* stream_result() says so */
    if (st.sse < 0) {
        while (n > 0 && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) {
            p++;
            n--;
        }
        if (n == 0) return;
        st.sse = *p != '{';
    }
    if (!st.sse) {
        if (buf_add(&st.plain, p, n) != 0) st.oom = 1;
        return;
    }
    if (buf_add(&st.line, p, n) != 0) {
        st.oom = 1;
        return;
    }
    used = 0;
    while ((nl = (char *) memchr(st.line.p + used, '\n',
                                 st.line.len - used)) != NULL) {
        *nl = '\0';
        if (nl > st.line.p + used && nl[-1] == '\r') nl[-1] = '\0';
        stream_line(st.line.p + used);
        used = (size_t) (nl - st.line.p) + 1;
    }
    memmove(st.line.p, st.line.p + used, st.line.len - used);
    st.line.len -= used;
    st.line.p[st.line.len] = '\0';
}

/* The message, once the reply is over: as a plain reply's root would be.
   NULL, with a message, if it did not arrive whole. */
static cJSON *stream_result(void)
{
    cJSON *root, *msg, *choice, *choices, *call;
    int    i;

    stream_text_end();
    streamed_said = st.said;
    if (st.oom) {
        fprintf(stderr, "gork: out of memory reading reply\n");
        return NULL;
    }
    if (st.failed) return NULL;
    if (st.sse < 0) {
        fprintf(stderr, "gork: the reply was empty\n");
        return NULL;
    }
    if (!st.sse) {                      /* the server did not stream */
        streamed_said = -1;
        if (getenv("GORK_TRACE") != NULL) fprintf(stderr, "%s\n", st.plain.p);
        root = cJSON_Parse(st.plain.p);
        if (root == NULL) {
            fprintf(stderr, "gork: reply was not valid JSON\n");
            return NULL;
        }
        return openai ? from_openai(root) : root;
    }
    if (!openai) {
        if (st.root == NULL || cJSON_GetObjectItem(st.root, "stop_reason") == NULL) {
            fprintf(stderr, "gork: the reply stopped before its end\n");
            return NULL;
        }
        root = st.root;
        st.root = st.block = NULL;
        return root;
    }

    /* OpenAI: build what a whole reply would have been, and convert that. */
    if (!st.done && st.finish[0] == '\0') {
        fprintf(stderr, "gork: the reply stopped before its end\n");
        return NULL;
    }
    root = st.root != NULL ? st.root : cJSON_CreateObject();
    st.root = NULL;
    choices = root != NULL ? cJSON_AddArrayToObject(root, "choices") : NULL;
    choice = cJSON_CreateObject();
    msg = choice != NULL ? cJSON_AddObjectToObject(choice, "message") : NULL;
    if (choices == NULL || msg == NULL ||
        cJSON_AddStringToObject(msg, "content", st.acc.p) == NULL ||
        (st.finish[0] != '\0' &&
         cJSON_AddStringToObject(choice, "finish_reason", st.finish) == NULL)) {
        cJSON_Delete(choice);
        cJSON_Delete(root);
        fprintf(stderr, "gork: out of memory reading reply\n");
        return NULL;
    }
    cJSON_AddItemToArray(choices, choice);
    if (st.calls != NULL) {
        i = 0;
        cJSON_ArrayForEach(call, st.calls)
        {
            set_string(cJSON_GetObjectItem(call, "function"), "arguments",
                       st.args[i].p != NULL ? st.args[i].p : "");
            i++;
        }
        cJSON_AddItemToObject(msg, "tool_calls", st.calls);
        st.calls = NULL;
    }
    return from_openai(root);
}
#else   /* GORK_BARE: want_stream stays 0, and post() never calls these */
static struct { int printed; } st;
static void stream_reset(void) { }
static void stream_feed(const char *p, size_t n) { (void) p; (void) n; }
static void stream_text_end(void) { }
static cJSON *stream_result(void) { return NULL; }
#endif


/* Send the conversation and return the parsed 200 reply, or NULL after
   reporting the failure.  Network errors, rate limits and overload (429,
   5xx, 529) are often gone a few seconds later, so those are retried;
   anything else will fail the same way. */
static cJSON *post(const struct conn *c, cJSON *messages)
{
    cJSON      *req, *root, *usage;
    char       *body;
    struct buf  resp;
    int         status, try, wait, stream = want_stream && !no_stream;

    streamed_said = -1;
    req = build_request(messages, c->system);
    if (req != NULL && openai) {
        root = to_openai(req);
        cJSON_Delete(req);
        req = root;
    }
    if (req != NULL && stream) {
        /* stream_options: or an OpenAI server leaves the token counts out */
        root = openai ? cJSON_AddObjectToObject(req, "stream_options") : req;
        if (cJSON_AddTrueToObject(req, "stream") == NULL || root == NULL ||
            (openai && cJSON_AddTrueToObject(root, "include_usage") == NULL)) {
            cJSON_Delete(req);
            req = NULL;
        }
    }
    if (req == NULL) {
        fprintf(stderr, "gork: out of memory building request\n");
        return NULL;
    }
    body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);                  /* messages was held by reference */
    if (body == NULL) {
#ifdef GORK_DOS16
        fprintf(stderr, "gork: the conversation has grown too large for "
                        "the 16-bit build (a request is at most 32K): "
                        "/compact or /new\n");
#else
        fprintf(stderr, "gork: out of memory serialising request\n");
#endif
        return NULL;
    }

    for (try = 0; ; try++) {
        if (stream) {
            stream_reset();
            relay_stream = stream_feed;
        }
        status = relay_post(c->relay_host, c->relay_port, c->api_host,
                            c->path, c->key, body, strlen(body), &resp);
        relay_stream = NULL;
        /* no second attempt once part of an answer is on the screen */
        if (status == -2 || try >= c->retries || (stream && st.printed) ||
            (status >= 0 && status != 429 && status < 500))
            break;
        if (status > 0) report_api_error(status, resp.p);
        buf_free(&resp);
        wait = try < 5 ? 2 << try : 60;         /* 2, 4, 8, 16, 32, 60... */
        fprintf(stderr, "gork: retrying in %ds (%d of %d)\n",
                wait, try + 1, c->retries);
        wait_secs(wait);    /* ponytail: ignores retry-after; relay_post
                               drops headers */
        if (aborted) {
            free(body);
            return NULL;
        }
    }
    free(body);
    if (status < 0) {
        if (stream) stream_text_end();
        buf_free(&resp);
        return NULL;
    }
    if (status != 200) {
        report_api_error(status, resp.p);
        buf_free(&resp);
        return NULL;
    }
    if (stream) {
        buf_free(&resp);
        root = stream_result();
        if (root == NULL) return NULL;
    } else {
        if (getenv("GORK_TRACE") != NULL)
            fprintf(stderr, "%s\n", resp.p);

        root = cJSON_Parse(resp.p);
        buf_free(&resp);
        if (root == NULL) {
            fprintf(stderr, "gork: reply was not valid JSON\n");
            return NULL;
        }
        if (openai && (root = from_openai(root)) == NULL) return NULL;
    }

    usage = cJSON_GetObjectItem(root, "usage");
    if (usage != NULL) {
        ctx_tokens = usage_of(usage, "input_tokens") +
                     usage_of(usage, "cache_creation_input_tokens") +
                     usage_of(usage, "cache_read_input_tokens") +
                     usage_of(usage, "output_tokens");
        update_status(c);
    }
    return root;
}

static const char COMPACT_ASK[] =
    "Summarize this conversation so far, so the work can carry on from your "
    "summary alone: what the user asked for, decisions made, files read or "
    "changed and how, commands run and what they showed, and what is left "
    "to do.  Be specific: keep names, paths and values.  Do not call any "
    "tools; reply with the summary only.";

/*
 * Replace the conversation with one user message holding the model's own
 * summary of it.  If the conversation ends in a user turn -- a new request
 * or tool results not answered yet -- the ask rides along in that turn, so
 * the summary covers it; `resume` then tells the model to carry on.
 * Tools stay in the request: the API refuses tool_use blocks without them.
 * Returns 0, or -1 (conversation unchanged) after reporting why.
 */
static int compact(const struct conn *c, cJSON *messages, int resume)
{
    static const char head[] = "This conversation was compacted.  Summary "
                               "of it so far:\n\n",
                      tail[] = "\n\nContinue where it left off.";
    cJSON     *tmp, *last, *role, *content, *block, *root, *text, *type;
    struct buf sum;
    int        n = cJSON_GetArraySize(messages);

    if (n < 2) {
        fprintf(stderr, "gork: nothing to compact\n");
        return 0;
    }
    fprintf(stderr, "gork: compacting the conversation (%ld tokens) ...\n",
            ctx_tokens);

    tmp     = cJSON_Duplicate(messages, 1);
    last    = cJSON_GetArrayItem(tmp, n - 1);
    role    = cJSON_GetObjectItem(last, "role");
    content = cJSON_GetObjectItem(last, "content");
    if (tmp == NULL) goto oom;
    if (cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0) {
        if (cJSON_IsString(content)) {          /* "text" -> [{text}] */
            block = cJSON_CreateObject();
            if (block == NULL ||
                cJSON_AddStringToObject(block, "type", "text") == NULL ||
                cJSON_AddStringToObject(block, "text",
                                        content->valuestring) == NULL) {
                cJSON_Delete(block);
                goto oom;
            }
            content = cJSON_CreateArray();
            if (content == NULL) {
                cJSON_Delete(block);
                goto oom;
            }
            cJSON_AddItemToArray(content, block);
            cJSON_ReplaceItemInObject(last, "content", content);
        }
        block = cJSON_CreateObject();
        if (block == NULL ||
            cJSON_AddStringToObject(block, "type", "text") == NULL ||
            cJSON_AddStringToObject(block, "text", COMPACT_ASK) == NULL) {
            cJSON_Delete(block);
            goto oom;
        }
        cJSON_AddItemToArray(content, block);
    } else if (append_text_message(tmp, "user", COMPACT_ASK) != 0) {
        goto oom;
    }

    no_stream = 1;                      /* the summary is not for the screen */
    root = post(c, tmp);
    no_stream = 0;
    cJSON_Delete(tmp);
    if (root == NULL) {
        fprintf(stderr, "gork: compacting failed\n");
        return -1;
    }

    if (buf_init(&sum) != 0 || buf_add(&sum, head, sizeof head - 1) != 0) {
        cJSON_Delete(root);
        buf_free(&sum);
        fprintf(stderr, "gork: out of memory\n");
        return -1;
    }
    cJSON_ArrayForEach(block, cJSON_GetObjectItem(root, "content")) {
        type = cJSON_GetObjectItem(block, "type");
        text = cJSON_GetObjectItem(block, "text");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 &&
            cJSON_IsString(text))
            buf_add(&sum, text->valuestring, strlen(text->valuestring));
    }
    ctx_tokens = usage_of(cJSON_GetObjectItem(root, "usage"), "output_tokens");
    cJSON_Delete(root);
    if (sum.len == sizeof head - 1) {
        fprintf(stderr, "gork: compacting failed: the model wrote no "
                        "summary\n");
        buf_free(&sum);
        return -1;
    }
    if (resume) buf_add(&sum, tail, sizeof tail - 1);

    while (cJSON_GetArraySize(messages) > 0)
        cJSON_DeleteItemFromArray(messages, 0);
    if (append_text_message(messages, "user", sum.p) != 0) {
        buf_free(&sum);
        fprintf(stderr, "gork: out of memory; conversation lost\n");
        return -1;
    }
    buf_free(&sum);
    compactions++;
    update_status(c);
    fprintf(stderr, "gork: compacted to about %ld tokens\n", ctx_tokens);
    return 0;

oom:
    cJSON_Delete(tmp);
    fprintf(stderr, "gork: out of memory\n");
    return -1;
}

/* Run turns until the model stops asking for tools.  Returns 1 if the task
   finished, 0 on any failure (already reported on stderr). */
static int run_task(const struct conn *c, cJSON *messages)
{
    cJSON      *root, *content, *stop, *results;
    int         turn, said;

    for (turn = 0; turn < c->max_turns; turn++) {
        /* The conversation ends in a user turn here, complete, so stopping
           or compacting now leaves nothing dangling. */
        if (aborted) {
            fprintf(stderr, "gork: aborted\n");
            return 0;
        }
        if (c->auto_compact > 0 &&
            ctx_tokens >= c->ctx_window / 100 * c->auto_compact &&
            compact(c, messages, 1) != 0)
            return 0;

        root = post(c, messages);
        if (root == NULL) {
            if (aborted) fprintf(stderr, "gork: aborted\n");
            return 0;
        }

        stop    = cJSON_GetObjectItem(root, "stop_reason");
        content = cJSON_GetObjectItem(root, "content");

        /* Check stop_reason before reading content: on a refusal the content
           array is empty or partial, and a truncated turn is not an answer. */
        if (cJSON_IsString(stop) &&
            strcmp(stop->valuestring, "refusal") == 0) {
            fprintf(stderr, "gork: request was declined by the API\n");
            cJSON_Delete(root);
            return 0;
        }
        if (!cJSON_IsArray(content)) {
            fprintf(stderr, "gork: reply had no content array\n");
            cJSON_Delete(root);
            return 0;
        }

        /* a streamed reply's text is on the screen already */
        said = streamed_said >= 0 ? streamed_said : print_text_blocks(content);

        /* The assistant turn goes back verbatim -- tool_use and thinking
           blocks included, unedited -- or the next request is rejected. */
        if (append_message(messages, "assistant",
                           cJSON_Duplicate(content, 1)) != 0) {
            fprintf(stderr, "gork: out of memory recording reply\n");
            cJSON_Delete(root);
            return 0;
        }

        if (!cJSON_IsString(stop)) {
            fprintf(stderr, "gork: reply had no stop_reason\n");
            cJSON_Delete(root);
            return 0;
        }

        if (strcmp(stop->valuestring, "tool_use") == 0) {
            results = run_tools(content);
            cJSON_Delete(root);
            if (results == NULL ||
                append_message(messages, "user", results) != 0) {
                fprintf(stderr, "gork: out of memory running tools\n");
                return 0;
            }
            continue;                   /* feed results back, keep going */
        }

        if (strcmp(stop->valuestring, "max_tokens") == 0)
            fprintf(stderr, "gork: reply hit max_tokens and is truncated\n");
        else if (said == 0)     /* small local models do this now and then */
            fprintf(stderr, "gork: the model finished without saying "
                            "anything; try asking again\n");

        cJSON_Delete(root);
        return 1;
    }

    fprintf(stderr, "gork: gave up after %d turns; raise max_turns in "
                    "%s to allow more\n", c->max_turns, conf_hint);
    return 0;
}

/* Chat until the user quits.  One conversation, so each message builds on
   the last; /new starts over, /compact squeezes it into a summary, and Esc
   aborts the running task. */
static int interactive(const struct conn *c)
{
    cJSON *messages;
    char  *line, *arg;
    int    n, before;

    update_status(c);

    messages = cJSON_CreateArray();
    if (messages == NULL) {
        fprintf(stderr, "gork: out of memory\n");
        return 1;
    }
    interactive_mode = 1;
    if (tui_init(status_line, "gork " VERSION) != 0) {
        fprintf(stderr, "gork: cannot start the TUI (is this a terminal?)\n");
        cJSON_Delete(messages);
        return 1;
    }
    report_overrides();                 /* inside the TUI, or it's wiped */
    if (c->have_rules)
        fprintf(stderr, "gork: rules loaded from %s\n", RULES_FILE);
    if (nskills > 0)
        fprintf(stderr, "gork: %d skill%s available\n", nskills,
                nskills == 1 ? "" : "s");
    /* After tui_init, so server problems show up in the chat. */
    if (c->mcp_config != NULL) {
        mcp_start(c->mcp_config);
        if (mcp_tool_count() > 0)
            fprintf(stderr, "gork: %d MCP tools ready\n", mcp_tool_count());
    }

    while ((line = tui_read_line()) != NULL) {
        if (strcmp(line, "/new") == 0) {
            cJSON_Delete(messages);
            messages = cJSON_CreateArray();
            if (messages == NULL) break;
            ctx_tokens = 0;
            update_status(c);
            fprintf(stderr, "gork: new task, conversation cleared\n");
            continue;
        }
        if (strncmp(line, "/effort", 7) == 0 &&
            (line[7] == '\0' || line[7] == ' ')) {
            arg = line + 7 + strspn(line + 7, " ");
            if (strcmp(arg, "default") == 0)
                *effort = '\0';
            else if (valid_effort(arg))
                strcpy(effort, arg);
            else if (*arg != '\0')
                fprintf(stderr, "gork: want /effort low, medium, high, "
                                "xhigh, max or default\n");
            fprintf(stderr, "gork: effort %s\n", *effort ? effort : "default");
            update_status(c);
            continue;
        }
        aborted = 0;
        relay_abort = check_abort;
        if (strcmp(line, "/compact") == 0) {
            compact(c, messages, 0);
            relay_abort = NULL;
            continue;
        }

        n = cJSON_GetArraySize(messages);
        before = compactions;
        if (append_text_message(messages, "user", line) != 0) break;

        if (run_task(c, messages)) {
            /* done */
        } else if (aborted) {
            /* Aborts land where the conversation ends in a complete user
               turn, so it is kept; this reply restores the alternation. */
            if (append_text_message(messages, "assistant",
                                    "(aborted by the user)") != 0)
                break;
        } else {
            /* A failed task can leave a tool_use with no tool_result, which
               the API rejects on every later request -- so drop the whole
               exchange (all but the summary, if it was compacted meanwhile).
               ponytail: also drops the work of a task that hit max_turns. */
            if (compactions != before) n = 1;
            while (cJSON_GetArraySize(messages) > n)
                cJSON_DeleteItemFromArray(messages, n);
            fprintf(stderr, "gork: task failed; it was dropped from the "
                            "conversation\n");
        }
        relay_abort = NULL;
    }

    tui_end();
    cJSON_Delete(messages);
    return 0;
}

#ifdef __MSDOS__
/* MS-DOS 2 tells a program nothing of where it was found.  Look for
   gork.exe where COMMAND.COM did: the current directory, then each
   directory of PATH.  Its directory into `dir`; 0, or -1 if not found. */
static int dos_self_dir(char *dir, size_t size)
{
    const char *p, *e;
    char        f[PATH_MAX];
    size_t      n;

    if (access("GORK.EXE", 0) == 0)
        return getcwd(dir, (int) size) != NULL ? 0 : -1;
    for (p = getenv("PATH"); p != NULL && *p != '\0'; p = *e ? e + 1 : e) {
        e = strchr(p, ';');
        if (e == NULL) e = p + strlen(p);
        n = (size_t) (e - p);
        if (n == 0 || n + 10 > sizeof f || n >= size) continue;
        sprintf(f, "%.*s%sGORK.EXE", (int) n, p,
                p[n - 1] == '\\' || p[n - 1] == '/' ? "" : "\\");
        if (access(f, 0) == 0) {
            sprintf(dir, "%.*s", (int) n, p);
            return 0;
        }
    }
    return -1;
}
#endif

int main(int argc, char **argv)
{
    struct conn c;
    cJSON      *messages;
    struct buf  rules;
    FILE       *f;
    const char *relay, *colon, *home;
    char        relay_host[256], skills_path[PATH_MAX], mcp_path[PATH_MAX];
    char        trust_path[PATH_MAX];
    int         done, lvl;

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("gork %s\n", VERSION);
        return 0;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: %s [\"task\" | --version]\n", argv[0]);
        return 2;
    }
#ifdef __MSDOS__
#ifdef __DJGPP__
    /* Only the mode and the size are used.  For the rest DJGPP's stat()
       reads tables inside DOS, and on MS-DOS 4.01 it page-faults there. */
    _djstat_flags = _STAT_INODE | _STAT_EXEC_EXT | _STAT_EXEC_MAGIC |
                    _STAT_DIRSIZE | _STAT_ROOT_TIME | _STAT_WRITEBIT;
    /* Time zone first.  If DJGPP's first time conversion is the mktime()
       inside a stat(), the zone is left broken: time() returns the same
       wrong second from then on, Watt-32's timeouts never expire, and a
       request can be neither waited out nor aborted with Esc. */
    tzset();
#endif
#ifdef GORK_DOS16
    /* MS-DOS 2 leaves INT 2Fh, the multiplex interrupt of DOS 3 and later,
       at 0000:0000, and PDCurses calls it while it waits for a key (so
       does the SPX code, looking for IPX): aim it at the BIOS's dummy
       handler, an IRET at F000:FF53.  It may stay there after gork. */
    if (_dos_getvect(0x2f) == NULL &&
        *(unsigned char __far *) MK_FP(0xf000, 0xff53) == 0xcf) {
        union REGS   r;         /* INT 21h AH=25h: a cast to a pointer to */
        struct SREGS sr;        /* function loses the segment */

        segread(&sr);
        r.x.ax = 0x252f;
        r.x.dx = 0xff53;
        sr.ds = 0xf000;
        intdosx(&r, &r, &sr);
    }
#endif
    /* DOS sets no HOME: use the directory gork.exe is in. */
    if (getenv("HOME") == NULL && strlen(argv[0]) < PATH_MAX) {
        static char h[PATH_MAX + 8];
        char       *p;

        sprintf(h, "HOME=%s", argv[0]);
        for (p = h + strlen(h); p > h + 5 && *p != '/' && *p != '\\'; p--)
            ;
        if (p > h + 5) {
            p[p[-1] == ':'] = '\0';    /* "c:" is that drive's cwd: keep the / */
            putenv(h);
        } else if (dos_self_dir(h + 5, PATH_MAX) == 0) {
            putenv(h);
        }
    }
#endif
#ifdef _WIN32
    if (getenv("HOME") == NULL) {
        static char h[PATH_MAX + 8] = "HOME=";

        if (win_home(h + 5, PATH_MAX) == 0) putenv(h);
    }
#endif
    if (load_config() != 0) return 2;
    openai = strcmp(cfg(CFG_PROTOCOL), "openai") == 0;
    if (!openai && strcmp(cfg(CFG_PROTOCOL), "anthropic") != 0) {
        fprintf(stderr, "gork: want protocol anthropic or openai\n");
        return 2;
    }
    relay_bearer = openai;
    /* auto: in the TUI, where someone is watching; not in batch mode */
    if (strcmp(cfg(CFG_STREAM), "auto") == 0) {
#ifndef GORK_BARE
        want_stream = argc == 1;
#endif
    } else if (strcmp(cfg(CFG_STREAM), "yes") == 0) {
#ifdef GORK_BARE
        fprintf(stderr, "gork: stream: the DOS builds take replies whole\n");
#else
        want_stream = 1;
#endif
    } else if (strcmp(cfg(CFG_STREAM), "no") != 0) {
        fprintf(stderr, "gork: want stream auto, yes or no\n");
        return 2;
    }

    relay = cfg(CFG_RELAY);
    if (relay == NULL) {
        fprintf(stderr, "gork: no relay set; put `relay = host:port` in "
                        "%s (or the file GORK_CONFIG names)\n", conf_hint);
        return 2;
    }
    colon = strrchr(relay, ':');
    /* spx:NET.NODE:SOCKET, an IPX address: its socket is in hex */
    c.relay_port = colon == NULL ? 0 :
        strtol(colon + 1, NULL, strncmp(relay, "spx:", 4) == 0 ? 16 : 10);
    if (colon == NULL || colon == relay ||
        (size_t) (colon - relay) >= sizeof relay_host ||
        c.relay_port <= 0 || c.relay_port > 65535) {
        fprintf(stderr, "gork: bad relay '%s', want host:port or "
                        "spx:NET.NODE:SOCKET\n", relay);
        return 2;
    }
    memcpy(relay_host, relay, colon - relay);
    relay_host[colon - relay] = '\0';
    c.relay_host = relay_host;

    c.api_host = cfg(CFG_HOST);
    c.path     = cfg(CFG_PATH);
    c.key      = cfg(CFG_KEY);
    c.max_turns = atoi(cfg(CFG_TURNS));
    c.retries   = atoi(cfg(CFG_RETRIES));
    c.ctx_window   = atol(cfg(CFG_WINDOW));
    c.auto_compact = atoi(cfg(CFG_COMPACT));
    if (c.max_turns < 1 || c.retries < 0 || c.retries > 10) {
        fprintf(stderr, "gork: want max_turns >= 1 and retries 0..10\n");
        return 2;
    }
    if (*cfg(CFG_EFFORT) != '\0' && !valid_effort(cfg(CFG_EFFORT))) {
        fprintf(stderr, "gork: want default_effort low, medium, high, "
                        "xhigh or max\n");
        return 2;
    }
    join(effort, sizeof effort, cfg(CFG_EFFORT), "");  /* checked above */
    relay_timeout = atoi(cfg(CFG_TIMEOUT));
    if (relay_timeout < 1) {
        fprintf(stderr, "gork: want timeout >= 1 (seconds)\n");
        return 2;
    }
    if (c.ctx_window < 1000 || c.auto_compact < 0 || c.auto_compact > 99) {
        fprintf(stderr, "gork: want context_window >= 1000 and "
                        "auto_compact 0..99\n");
        return 2;
    }

    /* Local servers (LM Studio, llama.cpp, Ollama) serve the same APIs
       without auth, so an empty key is only fatal when we are actually
       talking to Anthropic or OpenAI.  Catching it here beats a round-trip
       to find out. */
    if (*c.key == '\0' && (strstr(c.api_host, "anthropic.com") != NULL ||
                           strstr(c.api_host, "openai.com") != NULL)) {
        fprintf(stderr, "gork: no API key; set api_key in %s "
                        "or %s\n", conf_hint, cfg_env(CFG_KEY));
        return 2;
    }

    if (realpath(".", work_root) == NULL) {
        fprintf(stderr, "gork: cannot resolve working directory: %s\n",
                strerror(errno));
        return 1;
    }
    if (load_allow(cfg(CFG_ALLOW)) != 0) return 2;
    home = getenv("HOME");

#ifdef GORK_NO_TUI
    if (argc == 1) {
        fprintf(stderr, "gork: this build has no TUI: give the task on the "
                        "command line\n");
        return 2;
    }
#endif
    /* Interactive only: a task on the command line is consent already. */
    if (argc == 1 && home != NULL &&
        join(trust_path, sizeof trust_path, home, "/" DOT_GORK "/trusted") == 0) {
        lvl = trust_level(trust_path);
        if (lvl == 0) lvl = ask_trust(trust_path, home);
        if (lvl == 0) return 1;
        trust_edits = lvl == 2;
    }

    /* System prompt = GORK.md, then the skill index.  A missing GORK.md
       is normal, an unreadable one is not -- silently running without the
       operator's rules would be worse. */
    if (buf_init(&rules) != 0) {
        fprintf(stderr, "gork: out of memory\n");
        return 1;
    }
    f = fopen(RULES_FILE, RB);
    if (f == NULL && errno != ENOENT) {
        fprintf(stderr, "gork: cannot read %s: %s\n", RULES_FILE,
                strerror(errno));
        return 1;
    }
    if (f != NULL) {
        if (read_stream(f, &rules, TOOL_READ_MAX) != 0) {
            fprintf(stderr, "gork: %s is too large or out of memory\n",
                    RULES_FILE);
            fclose(f);
            return 1;
        }
        fclose(f);
    }
    c.have_rules = rules.len > 0;

    scan_skills(DOT_GORK "/skills");
    c.mcp_config = NULL;
    if (home != NULL &&
        join(mcp_path, sizeof mcp_path, home, "/" DOT_GORK "/mcp.json") == 0) {
        c.mcp_config = mcp_path;
    }
    if (home != NULL &&
        join(skills_path, sizeof skills_path, home, "/" DOT_GORK "/skills") == 0) {
        if (realpath(skills_path, user_skills) != NULL)
            scan_skills(user_skills);
        else
            user_skills[0] = '\0';
    }
    if (add_allow_note(&rules) != 0 || add_skill_index(&rules) != 0) {
        fprintf(stderr, "gork: out of memory\n");
        return 1;
    }
    c.system = rules.len > 0 ? rules.p : NULL;

    if (argc == 1) {
        done = interactive(&c) == 0;
    } else {
        const char *task = argv[1];
#ifdef _WIN32   /* the command line is in the ANSI code page */
        char       *utf8 = win_input_to_utf8(task, 0);

        if (utf8 != NULL) task = utf8;  /* ponytail: never freed */
#endif
        messages = cJSON_CreateArray();
        if (messages == NULL ||
            append_text_message(messages, "user", task) != 0) {
            fprintf(stderr, "gork: out of memory\n");
            cJSON_Delete(messages);
            free(rules.p);
            return 1;
        }
        report_overrides();
        if (c.mcp_config != NULL) mcp_start(c.mcp_config);
        done = run_task(&c, messages);
        cJSON_Delete(messages);
    }

    mcp_stop();
    free(rules.p);
    return done ? 0 : 1;
}
