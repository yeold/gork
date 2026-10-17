/*
 * tui.c -- the interactive front end.
 *
 * While the TUI is up, fds 1 and 2 point at an unlinked temp file, so the
 * agent loop and relay keep using plain printf/fprintf, and tui_drain() moves
 * whatever they wrote into the chat pane.  A temp file rather than a pipe: a
 * pipe holds 4 KiB on Linux 2.2, and GORK_TRACE alone would fill it and
 * deadlock us against ourselves.  The terminal itself is reached through a
 * dup of the original stdout, handed to newterm().
 *
 * Screen layout:   row 0          status bar
 *                  1 .. LINES-3   chat
 *                  LINES-2        hint / approval prompt
 *                  LINES-1        input (more rows, upwards, when the
 *                                 input has several lines)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include "port.h"
#if !defined(_WIN32) && !defined(GORK_DOS16)
#include <termios.h>
#endif
#include <curses.h>
#undef lines                    /* HP-UX: term.h macros */
#undef columns

#include "relay.h"              /* struct buf */
#include "tui.h"

#define HINT_IDLE  "Enter send | Up/Down history | PgUp/PgDn scroll | /new /compact /effort /quit"
#define HINT_BUSY  "waiting for the model ... (Esc to abort)"
#define PREVIEW_LINES 20        /* approval detail shown before eliding */

enum { ST_USER, ST_TEXT, ST_TOOL, ST_NOTE, ST_DETAIL, NSTYLES };

struct line {
    int    style;
    char  *text;
    size_t len;
};

static int          active;
static int          cap_rfd = -1, saved_out = -1, saved_err = -1;
#if defined(GORK_NO_FORK) || defined(GORK_EMX)
static char         cap_name[TMPNAM_MAX];   /* DOS and OS/2 cannot unlink */
#endif                                      /* a file that is open */
#ifdef GORK_NO_FORK
static long         cap_off;                /* how far it has been read */
#endif
static struct buf   partial;            /* captured output not yet a line */
static int          open_line = -1;     /* index of the line that shows it */
static size_t       open_len;           /* and how long it was then */
static struct line *lines;
static size_t       nlines, cap_lines;
static int          scroll_rows;        /* rows scrolled up from the bottom */
#ifdef GORK_DOS16               /* attributes do not fit a 16-bit int */
static chtype       style_attr[NSTYLES];
#else
static int          style_attr[NSTYLES];
#endif
static const char  *status_text = "";
static const char  *hint_text = HINT_IDLE;
static char         input[4096];
static size_t       input_len;
static size_t       input_pos;          /* the cursor: a byte offset */

/* Earlier lines, for the Up and Down keys. */
#ifdef GORK_DOS16
#define NHIST 8
#else
#define NHIST 32
#endif
static char        *hist[NHIST];
static int          nhist;

/* ponytail: history grows without bound and is wrapped by byte count, so
   UTF-8 text wraps a little early.  Fine for a session; cap it if needed. */
static void add_line(int style, const char *text, size_t len)
{
    struct line *nl;
    char        *t;
    size_t       i;

    if (nlines == cap_lines) {
        size_t ncap = cap_lines ? cap_lines * 2 : 256;
        nl = (struct line *) realloc(lines, ncap * sizeof *lines);
        if (nl == NULL) return;
        lines = nl;
        cap_lines = ncap;
    }
    t = (char *) malloc(len + 1);
    if (t == NULL) return;
    /* Control bytes (tabs, ANSI colour from commands) would render as ^X
       and throw off the wrapping, so they become spaces. */
    for (i = 0; i < len; i++)
        t[i] = ((unsigned char) text[i] < 32) ? ' ' : text[i];
    t[len] = '\0';
#ifdef _WIN32
    len = win_utf8_to_console(t, len, 1);
    t[len] = '\0';
#endif

    lines[nlines].style = style;
    lines[nlines].text  = t;
    lines[nlines].len   = len;
    nlines++;
    scroll_rows = 0;            /* new output pulls the view to the bottom */
}

static void add_text(int style, const char *s)
{
    add_line(style, s, strlen(s));
}

/* The agent loop marks its diagnostics by prefix; style them to match. */
static int style_of(const char *s)
{
    if (strncmp(s, "[tool]", 6) == 0 || strncmp(s, "[run]", 5) == 0)
        return ST_TOOL;
    if (strncmp(s, "gork:", 6) == 0)
        return ST_NOTE;
    return ST_TEXT;
}

static int line_rows(size_t i, int w)
{
    return lines[i].len == 0 ? 1 : (int) ((lines[i].len + w - 1) / w);
}

/* Text at a position, by move + waddnstr only.  Old ncurses lacks addnstr
   (1.8.5) and mvhline (1.9.4), and the mvaddnstr macro of the 1993 one
   does not parse. */
static void put(int y, int x, const char *s, int n)
{
    move(y, x);
    waddnstr(stdscr, (char *) s, n);
}

/* Is `c` a byte inside a UTF-8 character, not its first?  Never on DOS,
   Windows and OS/2, where the keyboard gives one byte for each character. */
static int is_cont(char c)
{
#if defined(GORK_NO_FORK) || defined(GORK_EMX)
    (void) c;
    return 0;
#else
    return ((unsigned char) c & 0xC0) == 0x80;
#endif
}

/* How many screen columns the `n` bytes at `s` take. */
static int columns(const char *s, size_t n)
{
    int    c = 0;
    size_t i;

    for (i = 0; i < n; i++)
        if (!is_cont(s[i])) c++;
    return c;
}

/* The line of the input that holds byte `pos`: where it starts and ends
   (at its newline or the end of the input).  Returns its number, from 0. */
static int input_line(size_t pos, size_t *start, size_t *end)
{
    int    n = 0;
    size_t i;

    *start = 0;
    for (i = 0; i < pos; i++)
        if (input[i] == '\n') {
            n++;
            *start = i + 1;
        }
    for (i = pos; i < input_len && input[i] != '\n'; i++)
        ;
    *end = i;
    return n;
}

static void draw(void)
{
    int    w = COLS, avail, total = 0, first, g, k, rows, off;
    int    in_rows, in_all, cur, top, y;
    size_t i, a, b;

    /* The input takes a row for each of its lines, up to a third of the
       screen; with more, the rows around the cursor show. */
    cur = input_line(input_pos, &a, &b);
    in_all = input_line(input_len, &a, &b) + 1;
    in_rows = in_all;
    if (in_rows > LINES / 3) in_rows = LINES / 3;
    if (in_rows < 1) in_rows = 1;
    top = cur >= in_rows ? cur - in_rows + 1 : 0;
    avail = LINES - 2 - in_rows;

    if (avail < 1 || w < 4) return;     /* terminal too small to bother */
    erase();

    attrset(A_REVERSE);
    move(0, 0);
    hline(' ', w);
    put(0, 0, status_text, w);

    for (i = 0; i < nlines; i++) total += line_rows(i, w);
    if (scroll_rows > total - avail) scroll_rows = total - avail;
    if (scroll_rows < 0) scroll_rows = 0;
    first = total > avail ? total - avail - scroll_rows : 0;

    g = 0;
    for (i = 0; i < nlines && g < first + avail; i++) {
        rows = line_rows(i, w);
        if (g + rows <= first) {
            g += rows;
            continue;
        }
        attrset(style_attr[lines[i].style]);
        for (k = 0; k < rows; k++, g++) {
            if (g < first || g >= first + avail) continue;
            put(1 + g - first, 0, lines[i].text + (size_t) k * w, w);
        }
    }

    attrset(A_DIM);
    put(LINES - 1 - in_rows, 0, hint_text, w);
    if (scroll_rows > 0) mvaddstr(LINES - 1 - in_rows, w - 10, "[scrolled]");

    /* Input: of a line wider than the screen, the part with the cursor
       (or its start, in the lines the cursor is not in).  One column is
       kept free so the cursor never sits in the bottom-right corner. */
    a = 0;
    for (k = 0; k < top + in_rows && k < in_all; k++) {
        for (b = a; b < input_len && input[b] != '\n'; b++)
            ;
        if (k >= top) {
            y = LINES - in_rows + (k - top);
            attrset(A_BOLD);
            mvaddstr(y, 0, k == 0 ? "> " : "  ");
            attrset(A_NORMAL);
            off = 0;
            if (k == cur) {
                off = (int) (input_pos - a) > w - 3
                      ? (int) (input_pos - a) - (w - 3) : 0;
                while (a + off < input_pos && is_cont(input[a + off])) off++;
            }
            rows = (int) (b - a) - off;         /* bytes to show */
            put(y, 2, input + a + off, rows < w - 3 ? rows : w - 3);
        }
        a = b + 1;
    }
    input_line(input_pos, &a, &b);
    off = (int) (input_pos - a) > w - 3 ? (int) (input_pos - a) - (w - 3) : 0;
    while (a + off < input_pos && is_cont(input[a + off])) off++;
    move(LINES - in_rows + (cur - top),
         2 + columns(input + a + off, input_pos - a - (size_t) off));
    refresh();
}

static void scroll_key(int ch)
{
    if (ch == KEY_PPAGE) scroll_rows += LINES / 2;
    if (ch == KEY_NPAGE) scroll_rows -= LINES / 2;
}

/* Default SIGINT would leave the terminal in cbreak/noecho. */
static void on_signal(int sig)
{
    endwin();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* The window title.  PDCurses knows how on DOS and Windows.  Elsewhere it
   is the xterm sequence, sent only to the terminals known to take it: a
   console or a real VT100 would print it.  xterm's title stack (22 and 23)
   puts the old title back in tui_end(). */
static FILE *term;
static int   titled;

static void set_title(const char *title)
{
#ifdef PDCURSES
    PDC_set_title(title);
#else
    static const char *const takes[] = { "xterm", "rxvt", "screen", "tmux",
                                         "alacritty", "foot", "konsole",
                                         "putty" };
    const char *t = getenv("TERM");
    size_t      i;

    for (i = 0; t != NULL && i < sizeof takes / sizeof *takes; i++)
        if (strncmp(t, takes[i], strlen(takes[i])) == 0) {
            fprintf(term, "\033[22;0t\033]0;%s\007", title);
            fflush(term);
            titled = 1;
            return;
        }
#endif
}

int tui_init(const char *status, const char *title)
{
#ifdef GORK_NO_FORK
    char *name = cap_name;
#elif defined(GORK_EMX)
    char *name = cap_name, *dir = getenv("TMP");
#else
    char  name[] = "/tmp/gorkXXXXXX";
#endif
    int   fd;
    FILE *tty;

    status_text = status;
    /* Esc is a key here, not just a prefix: don't wait a second for the
       rest of an escape sequence.  ncurses reads this at init. */
    if (getenv("ESCDELAY") == NULL) putenv("ESCDELAY=50");
    setvbuf(stdout, NULL, _IOLBF, 0);   /* before anything is printed */

    if (buf_init(&partial) != 0) return -1;
#ifdef GORK_NO_FORK
    fd = tmpnam(name) != NULL ? open(name, O_WRONLY | O_CREAT | O_EXCL, 0600)
                              : -1;
#else
#ifdef GORK_EMX                         /* no /tmp: %TMP%, or here */
    if (dir == NULL) dir = getenv("TEMP");
    if (dir == NULL || strlen(dir) + 16 > sizeof cap_name) dir = ".";
    sprintf(name, "%s/gorkXXXXXX", dir);
#endif
    fd = mkstemp(name);
#endif
    if (fd < 0) return -1;
    cap_rfd = open(name, O_RDONLY);     /* own offset, independent of writers */
#if !defined(GORK_NO_FORK) && !defined(GORK_EMX)
    unlink(name);
#endif

    saved_out = dup(1);
    saved_err = dup(2);
    tty = fdopen(dup(1), "w");
#ifdef _WIN32
    /* PDCurses would draw in a second screen buffer, which Windows 9x
       never shows: have it use the console's own. */
    if (getenv("PDC_RESTORE_SCREEN") == NULL) putenv("PDC_RESTORE_SCREEN=0");
    win_own_console();
#endif
    /* $TERM spelled out: AIX PS/2's curses fails on NULL, which elsewhere
       means the same thing. */
    if (cap_rfd < 0 || saved_out < 0 || saved_err < 0 || tty == NULL ||
        newterm(getenv("TERM"), tty, stdin) == NULL) {
        close(fd);
        return -1;
    }

    fflush(stdout);
    fflush(stderr);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);

    cbreak();
    noecho();
    keypad(stdscr, TRUE);

    style_attr[ST_USER]   = A_BOLD;
    style_attr[ST_TEXT]   = A_NORMAL;
    style_attr[ST_TOOL]   = A_DIM;
    style_attr[ST_NOTE]   = A_BOLD;
    style_attr[ST_DETAIL] = A_DIM;
#ifdef COLOR_BLACK                      /* AIX PS/2's curses has no colour */
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);
        style_attr[ST_TOOL] = COLOR_PAIR(1);
        style_attr[ST_NOTE] = COLOR_PAIR(2);
    }
#endif

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    term = tty;
    set_title(title);
    active = 1;
    draw();
    return 0;
}

void tui_end(void)
{
    if (!active) return;
    active = 0;
#if defined(GORK_NO_FORK) || defined(GORK_EMX)   /* PDCurses: no alternate
                                                    screen to switch back from */
    clear();
    refresh();
#endif
    if (titled) {
        fputs("\033[23;0t", term);
        fflush(term);
    }
    endwin();
    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, 1);
    dup2(saved_err, 2);
#if defined(GORK_NO_FORK) || defined(GORK_EMX)
    close(cap_rfd);
    remove(cap_name);
#endif
}

void tui_drain(void)
{
    char   chunk[4096];
    char  *start, *nl;
    int    n, was = scroll_rows;
    size_t rest;

    if (!active) return;
    fflush(stdout);

#ifdef GORK_NO_FORK
    /* A second handle does not see what the first appends until the file
       is committed and opened again. */
    fsync(1);
#ifdef __MSDOS__
    {                           /* MS-DOS 2 has no commit: closing a */
        int d = dup(1);         /* duplicate does the same */

        if (d >= 0) close(d);
    }
#endif
    close(cap_rfd);
    cap_rfd = open(cap_name, O_RDONLY);
    lseek(cap_rfd, cap_off, SEEK_SET);
#endif
    while ((n = read(cap_rfd, chunk, sizeof chunk)) > 0)
        if (buf_add(&partial, chunk, (size_t) n) != 0) break;
#ifdef GORK_NO_FORK
    cap_off = lseek(cap_rfd, 0L, SEEK_CUR);
#endif

    /* The unfinished line was shown as it stood (a streamed reply grows
       word by word): take that out again, it is about to be redone. */
    if (open_line >= 0 && (size_t) open_line < nlines) {
        free(lines[open_line].text);
        memmove(lines + open_line, lines + open_line + 1,
                (nlines - (size_t) open_line - 1) * sizeof *lines);
        nlines--;
    }
    open_line = -1;

    start = partial.p;
    while ((nl = (char *) memchr(start, '\n',
                                 partial.len - (start - partial.p))) != NULL) {
        *nl = '\0';
        add_line(style_of(start), start, (size_t) (nl - start));
        start = nl + 1;
    }
    rest = partial.len - (start - partial.p);   /* unfinished line: keep */
    memmove(partial.p, start, rest);
    partial.len = rest;
    partial.p[rest] = '\0';
    if (rest > 0) {
        size_t before = nlines;

        add_line(style_of(partial.p), partial.p, rest);
        if (nlines > before) {
            /* nothing new: leave the view where the user scrolled it */
            if (open_line >= 0 && (size_t) open_line == before &&
                rest == open_len)
                scroll_rows = was;
            open_line = (int) before;
            open_len = rest;
        }
    }

    draw();
}

static void set_input(const char *text)
{
    input_len = strlen(text);
    if (input_len > sizeof input - 1) input_len = sizeof input - 1;
    memcpy(input, text, input_len);
    input[input_len] = '\0';
    input_pos = input_len;
}

/* Remember a line that was sent; the oldest goes when the list is full. */
static void remember(const char *text)
{
    char *copy;

    if (nhist > 0 && strcmp(hist[nhist - 1], text) == 0) return;
    copy = (char *) malloc(strlen(text) + 1);
    if (copy == NULL) return;
    strcpy(copy, text);
    if (nhist == NHIST) {
        free(hist[0]);
        memmove(hist, hist + 1, (NHIST - 1) * sizeof *hist);
        nhist--;
    }
    hist[nhist++] = copy;
}

char *tui_read_line(void)
{
    static char echo[sizeof input + 2];
    static char draft[sizeof input];    /* what was typed before Up */
    static char row[sizeof input + 2];
    int         ch, at = nhist;         /* at: the history line shown */
    size_t      n, e, a, b;

    if (!active) return NULL;
    tui_drain();
    hint_text = HINT_IDLE;
    set_input("");

    for (;;) {
        draw();
        ch = getch();
        switch (ch) {
        case '\n': case '\r': case KEY_ENTER:
            if (input_len == 0) break;
#ifndef GORK_BARE
            /* a backslash before the cursor: not sent, a new line */
            if (input_pos > 0 && input[input_pos - 1] == '\\') {
                input[input_pos - 1] = '\n';
                break;
            }
#endif
            if (strcmp(input, "/quit") == 0) return NULL;
            remember(input);
            sprintf(echo, "> %s", input);
            for (n = 0; ; n = e + 1) {          /* a line of chat for each */
                for (e = n; echo[e] != '\0' && echo[e] != '\n'; e++)
                    ;
                if (n == 0) {
                    add_line(ST_USER, echo, e);
                } else {                        /* under the first: indented */
                    sprintf(row, "  %.*s", (int) (e - n), echo + n);
                    add_line(ST_USER, row, e - n + 2);
                }
                if (echo[e] == '\0') break;
            }
            set_input("");
            hint_text = HINT_BUSY;
            draw();
#ifdef _WIN32
            {   /* typed in the console's code page; the API wants UTF-8 */
                static char utf8[3 * sizeof input];
                char       *u = win_input_to_utf8(echo + 2, 1);

                if (u != NULL && strlen(u) < sizeof utf8) {
                    strcpy(utf8, u);
                    free(u);
                    return utf8;
                }
                free(u);
            }
#endif
            return echo + 2;
        case KEY_BACKSPACE: case 127: case 8:
            /* drop a whole UTF-8 sequence, not half of one */
            n = input_pos;
            while (n > 0 && is_cont(input[n - 1])) n--;
            if (n > 0) n--;
            memmove(input + n, input + input_pos, input_len - input_pos + 1);
            input_len -= input_pos - n;
            input_pos = n;
            break;
        case KEY_DC:
            n = input_pos;
            if (n < input_len) n++;
            while (n < input_len && is_cont(input[n])) n++;
            memmove(input + input_pos, input + n, input_len - n + 1);
            input_len -= n - input_pos;
            break;
        case KEY_LEFT:
            if (input_pos > 0) input_pos--;
            while (input_pos > 0 && is_cont(input[input_pos])) input_pos--;
            break;
        case KEY_RIGHT:
            if (input_pos < input_len) input_pos++;
            while (input_pos < input_len && is_cont(input[input_pos]))
                input_pos++;
            break;
        case KEY_HOME: case 1:          /* ^A */
            input_line(input_pos, &a, &b);
            input_pos = a;
            break;
        case KEY_END: case 5:           /* ^E */
            input_line(input_pos, &a, &b);
            input_pos = b;
            break;
        case KEY_UP:
            /* within an input of several lines: to the line above */
            if (input_line(input_pos, &a, &b) > 0) {
                n = input_pos - a;
                input_line(a - 1, &a, &b);
                input_pos = a + n < b ? a + n : b;
                while (input_pos > a && is_cont(input[input_pos])) input_pos--;
                break;
            }
            if (at == 0) break;
            if (at == nhist) strcpy(draft, input);
            set_input(hist[--at]);
            break;
        case KEY_DOWN:
            input_line(input_pos, &a, &b);
            if (b < input_len) {                /* to the line below */
                n = input_pos - a;
                input_line(b + 1, &a, &b);
                input_pos = a + n < b ? a + n : b;
                while (input_pos > a && is_cont(input[input_pos])) input_pos--;
                break;
            }
            if (at == nhist) break;
            at++;
            set_input(at == nhist ? draft : hist[at]);
            break;
        case 21:                        /* ^U */
            set_input("");
            break;
        case 4:                         /* ^D on an empty line quits */
            if (input_len == 0) return NULL;
            break;
        case KEY_PPAGE: case KEY_NPAGE:
            scroll_key(ch);
            break;
        default:                        /* KEY_RESIZE lands here: redraw */
            if (ch >= 32 && ch < 256 && ch != 127 &&
                input_len < sizeof input - 1) {
                memmove(input + input_pos + 1, input + input_pos,
                        input_len - input_pos + 1);
                input[input_pos++] = (char) ch;
                input_len++;
            }
        }
    }
}

int tui_confirm(const char *title, const char *detail)
{
    char        hint[256];
    const char *p, *nl;
    int         ch, shown = 0, more = 0;

    if (!active) return 'n';
    tui_drain();                /* the model's text first, then the ask */

    for (p = detail; *p; p = nl + 1) {
        nl = strchr(p, '\n');
        if (shown < PREVIEW_LINES) {
            add_line(ST_DETAIL, p, nl ? (size_t) (nl - p) : strlen(p));
            shown++;
        } else {
            more++;
        }
        if (nl == NULL) break;
    }
    if (more > 0) {
        sprintf(hint, "  ... %d more lines", more);
        add_text(ST_DETAIL, hint);
    }

    sprintf(hint, "Allow %.100s?   y = yes   n = no   a = always this session",
            title);
    hint_text = hint;
    for (;;) {
        draw();
        ch = getch();
        if (ch == 'y' || ch == 'Y' || ch == 'a' || ch == 'A' ||
            ch == 'n' || ch == 'N' || ch == 27)
            break;
        scroll_key(ch);
    }
    ch = (ch == 'y' || ch == 'Y') ? 'y' : (ch == 'a' || ch == 'A') ? 'a'
       : ch == 27 ? 27 : 'n';
    add_text(ST_NOTE, ch == 'y' || ch == 'a' ? "  approved"
                    : ch == 27 ? "  declined, aborting" : "  declined");
    hint_text = HINT_BUSY;
    draw();
    return ch;
}

/* Hand the terminal back in shell modes (canonical, echo, CR -> NL) so a
   command that prompts on /dev/tty -- ssh, ftp, sudo -- can read a line.
   Esc becomes the interrupt key, so the terminal itself sends SIGINT to the
   command and gork never reads keys a prompt is waiting for.  The TUI stays
   on screen; tui_resume() repaints over whatever was typed and restores the
   modes, the interrupt key included.
   ponytail: arrow keys start with Esc too, so they interrupt here; a key
   reader that tells them apart would have to steal the prompts' input. */
void tui_suspend(void)
{
#if !defined(_WIN32) && !defined(GORK_DOS16) && !defined(GORK_EMX)
    struct termios t;
#endif

    if (!active) return;
#ifdef __MSDOS__
    hint_text = "running command ... (DOS cannot interrupt it)";
#elif defined(_WIN32) || defined(GORK_EMX)
    hint_text = "running command ... (it cannot be interrupted)";
#else
    hint_text = "running command ... (answer any prompt it shows; Esc aborts)";
#endif
    draw();
    reset_shell_mode();
    /* Not on OS/2: termios on the console makes emx.dll read the keyboard
       itself, and it goes on taking every other key from PDCurses. */
#if !defined(_WIN32) && !defined(GORK_DOS16) && !defined(GORK_EMX)
    if (tcgetattr(0, &t) == 0) {
        t.c_cc[VINTR] = 27;
        tcsetattr(0, TCSANOW, &t);
    }
#endif
}

void tui_resume(void)
{
    if (!active) return;
    reset_prog_mode();
    hint_text = HINT_BUSY;
    clearok(curscr, TRUE);
    draw();
}

int tui_poll_abort(void)
{
    int ch, esc = 0;

    if (!active) return 0;
    tui_drain();
    nodelay(stdscr, TRUE);
    while ((ch = getch()) != ERR) {
        if (ch == 27) {
            /* Stop here and empty the queue: 64-bit Solaris curses leaves
               a -1 in it after a lone Esc and crashes on the next getch. */
            esc = 1;
            flushinp();
            break;
        }
        scroll_key(ch);         /* anything else typed now is dropped */
    }
    nodelay(stdscr, FALSE);
    if (esc) {
        add_text(ST_NOTE, "  aborting ...");
        draw();
    }
    return esc;
}
