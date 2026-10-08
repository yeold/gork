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
 *                  LINES-1        input
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <curses.h>

#include "relay.h"              /* struct buf */
#include "tui.h"

#define HINT_IDLE  "Enter send | PgUp/PgDn scroll | /new fresh task | /compact | /effort | /quit"
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
static struct buf   partial;            /* captured output not yet a line */
static struct line *lines;
static size_t       nlines, cap_lines;
static int          scroll_rows;        /* rows scrolled up from the bottom */
static int          style_attr[NSTYLES];
static const char  *status_text = "";
static const char  *hint_text = HINT_IDLE;
static char         input[4096];
static size_t       input_len;

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

static void draw(void)
{
    int    w = COLS, avail = LINES - 3, total = 0, first, g, k, rows, off;
    size_t i;

    if (avail < 1 || w < 4) return;     /* terminal too small to bother */
    erase();

    attrset(A_REVERSE);
    mvhline(0, 0, ' ', w);
    mvaddnstr(0, 0, status_text, w);

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
            mvaddnstr(1 + g - first, 0, lines[i].text + (size_t) k * w, w);
        }
    }

    attrset(A_DIM);
    mvaddnstr(LINES - 2, 0, hint_text, w);
    if (scroll_rows > 0) mvaddstr(LINES - 2, w - 10, "[scrolled]");

    /* Input: show the tail if it is wider than the screen.  One column is
       kept free so the cursor never sits in the bottom-right corner. */
    attrset(A_BOLD);
    mvaddstr(LINES - 1, 0, "> ");
    attrset(A_NORMAL);
    off = (int) input_len > w - 3 ? (int) input_len - (w - 3) : 0;
    addnstr(input + off, w - 3);
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

int tui_init(const char *status)
{
    char  name[] = "/tmp/gorkXXXXXX";
    int   fd;
    FILE *tty;

    status_text = status;
    /* Esc is a key here, not just a prefix: don't wait a second for the
       rest of an escape sequence.  ncurses reads this at init. */
    if (getenv("ESCDELAY") == NULL) putenv("ESCDELAY=50");
    setvbuf(stdout, NULL, _IOLBF, 0);   /* before anything is printed */

    if (buf_init(&partial) != 0) return -1;
    fd = mkstemp(name);
    if (fd < 0) return -1;
    cap_rfd = open(name, O_RDONLY);     /* own offset, independent of writers */
    unlink(name);

    saved_out = dup(1);
    saved_err = dup(2);
    tty = fdopen(dup(1), "w");
    if (cap_rfd < 0 || saved_out < 0 || saved_err < 0 || tty == NULL ||
        newterm(NULL, tty, stdin) == NULL) {
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
    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);
        init_pair(2, COLOR_YELLOW, COLOR_BLACK);
        style_attr[ST_TOOL] = COLOR_PAIR(1);
        style_attr[ST_NOTE] = COLOR_PAIR(2);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    active = 1;
    draw();
    return 0;
}

void tui_end(void)
{
    if (!active) return;
    active = 0;
    endwin();
    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, 1);
    dup2(saved_err, 2);
}

void tui_drain(void)
{
    char   chunk[4096];
    char  *start, *nl;
    int    n;
    size_t rest;

    if (!active) return;
    fflush(stdout);

    while ((n = read(cap_rfd, chunk, sizeof chunk)) > 0)
        if (buf_add(&partial, chunk, (size_t) n) != 0) break;

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

    draw();
}

char *tui_read_line(void)
{
    static char echo[sizeof input + 2];
    int         ch;

    if (!active) return NULL;
    tui_drain();
    hint_text = HINT_IDLE;
    input_len = 0;
    input[0]  = '\0';

    for (;;) {
        draw();
        ch = getch();
        switch (ch) {
        case '\n': case '\r': case KEY_ENTER:
            if (input_len == 0) break;
            if (strcmp(input, "/quit") == 0) return NULL;
            sprintf(echo, "> %s", input);
            add_text(ST_USER, echo);
            input_len = 0;
            input[0]  = '\0';
            hint_text = HINT_BUSY;
            draw();
            return echo + 2;
        case KEY_BACKSPACE: case 127: case 8:
            /* drop a whole UTF-8 sequence, not half of one */
            while (input_len > 0 &&
                   ((unsigned char) input[input_len - 1] & 0xC0) == 0x80)
                input_len--;
            if (input_len > 0) input_len--;
            input[input_len] = '\0';
            break;
        case 21:                        /* ^U */
            input_len = 0;
            input[0]  = '\0';
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
                input[input_len++] = (char) ch;
                input[input_len]   = '\0';
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
    struct termios t;

    if (!active) return;
    hint_text = "running command ... (answer any prompt it shows; Esc aborts)";
    draw();
    reset_shell_mode();
    if (tcgetattr(0, &t) == 0) {
        t.c_cc[VINTR] = 27;
        tcsetattr(0, TCSANOW, &t);
    }
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
        if (ch == 27) esc = 1;
        scroll_key(ch);         /* anything else typed now is dropped */
    }
    nodelay(stdscr, FALSE);
    if (esc) {
        add_text(ST_NOTE, "  aborting ...");
        draw();
    }
    return esc;
}
