/*
 * console.c -- gork's console window on the classic Mac OS: a grid of
 * Monaco 9 the size of the screen, text written at the bottom and the
 * lines scrolled up, and one line typed at a time.  No scrollback, no
 * menus; the close box and Command-Q end the program.
 *
 * In place of Retro68's console (C++, and most of libstdc++ with it,
 * a megabyte), so that gork fits a Mac of its time.  gork's text is
 * UTF-8, the Mac's Mac Roman: what is written is shown in Mac Roman (a
 * `?` for what it lacks), and what is typed comes back as UTF-8.
 */
#include <Multiverse.h>
#include <string.h>

#include "console.h"
#include "roman.h"

#define MAXCOLS 132
#define MAXROWS 60

static WindowPtr win;
static short cols, rows, cw, lh, ascent;
static char grid[MAXROWS][MAXCOLS];
static short len[MAXROWS];             /* chars in each row */
static short row, col;                 /* where the next one goes */
static unsigned long uc;               /* a UTF-8 character so far */
static int utf8_left;                  /* its bytes still to come */

static void cell(short r, short c, Rect *rc)
{
  SetRect(rc, 4 + c * cw, 2 + r * lh, 4 + (c + 1) * cw, 2 + (r + 1) * lh);
}

static void draw_row(short r)
{
  Rect rc;

  SetRect(&rc, 0, 2 + r * lh, win->portRect.right, 2 + (r + 1) * lh);
  EraseRect(&rc);
  MoveTo(4, 2 + r * lh + ascent);
  DrawText(grid[r], 0, len[r]);
}

static void redraw(void)
{
  short r;

  EraseRect(&win->portRect);
  for (r = 0; r < rows; r++) draw_row(r);
}

int con_init(void)
{
  FontInfo fi;
  Rect b;

  InitGraf(&qd.thePort);
  InitFonts();
  InitWindows();
  InitMenus();
  TEInit();
  InitDialogs(NULL);
  InitCursor();
  FlushEvents(everyEvent, 0);

  b = qd.screenBits.bounds;
  b.top += 40;                          /* the menu bar and a title bar */
  InsetRect(&b, 6, 6);
  b.top -= 6 - 2;
  win = NewWindow(NULL, &b, (ConstStringPtr) "\pgork", true, 4 /* noGrow */,
                  (WindowPtr) -1, true, 0);
  if (win == NULL) return -1;
  SetPort(win);
  TextFont(4);                          /* Monaco */
  TextSize(9);
  GetFontInfo(&fi);
  cw = fi.widMax;
  lh = fi.ascent + fi.descent + fi.leading;
  ascent = fi.ascent;
  cols = (win->portRect.right - 8) / cw;
  rows = (win->portRect.bottom - 4) / lh;
  if (cols > MAXCOLS) cols = MAXCOLS;
  if (rows > MAXROWS) rows = MAXROWS;
  return 0;
}

static void newline(void)
{
  RgnHandle gone;
  Rect area;

  col = 0;
  if (row < rows - 1) {
    row++;
    return;
  }
  memmove(grid[0], grid[1], (size_t) (rows - 1) * MAXCOLS);
  memmove(len, len + 1, (size_t) (rows - 1) * sizeof len[0]);
  len[rows - 1] = 0;
  SetRect(&area, 0, 2, win->portRect.right, 2 + rows * lh);
  gone = NewRgn();
  ScrollRect(&area, 0, -lh, gone);      /* and erases what it leaves */
  DisposeRgn(gone);
}

static void put(char c)
{
  if (c == '\n') {
    newline();
    return;
  }
  if (c == '\r') return;
  if (c == '\t') {
    do put(' '); while (col % 8 != 0 && col < cols);
    return;
  }
  if (c == '\b') {
    if (col > 0) {
      Rect rc;
      col--;
      len[row] = col;
      cell(row, col, &rc);
      EraseRect(&rc);
    }
    return;
  }
  if (col == cols) newline();
  grid[row][col] = c;
  MoveTo(4 + col * cw, 2 + row * lh + ascent);
  DrawChar(c);
  col++;
  if (len[row] < col) len[row] = col;
}

void con_write(const char *p, unsigned long n)
{
  GrafPtr old;
  unsigned char c;

  if (win == NULL) return;
  GetPort(&old);
  SetPort(win);
  for (; n > 0; n--, p++) {
    c = (unsigned char) *p;
    if (utf8_left > 0 && (c & 0xC0) == 0x80) {  /* the rest of one */
      uc = uc << 6 | (c & 0x3F);
      if (--utf8_left == 0) put(roman_from_unicode(uc));
      continue;
    }
    if (utf8_left > 0) put('?');        /* one cut short */
    utf8_left = 0;
    if (c >= 0xC0 && c < 0xF8) {        /* the first byte of one */
      utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      uc = c & (0x3F >> utf8_left);
    } else if (c >= 0x80)               /* not UTF-8 */
      put('?');
    else if (c >= 32 || c == '\n' || c == '\t' || c == '\b')
      put((char) c);
  }
  SetPort(old);
}

static void cursor(void)
{
  Rect rc;

  if (col == cols) return;
  cell(row, col, &rc);
  rc.top = rc.bottom - 2;
  InvertRect(&rc);
}

/* One event: 1 if it asks the program to end. */
static int event(EventRecord *ev)
{
  WindowPtr w;

  switch (ev->what) {
  case updateEvt:
    w = (WindowPtr) ev->message;
    SetPort(w);
    BeginUpdate(w);
    if (w == win) redraw();
    EndUpdate(w);
    SetPort(win);
    break;
  case mouseDown:
    switch (FindWindow(ev->where, &w)) {
    case inSysWindow: SystemClick(ev, w); break;
    case inDrag: DragWindow(w, ev->where, &qd.screenBits.bounds); break;
    case inGoAway: if (TrackGoAway(w, ev->where)) return 1; break;
    case inContent: if (w != FrontWindow()) SelectWindow(w); break;
    }
    break;
  }
  return 0;
}

void con_idle(void)
{
  EventRecord ev;

  if (win != NULL &&
      WaitNextEvent(updateMask | mDownMask, &ev, 0, NULL))
    event(&ev);
}

int con_readline(char *buf, unsigned long size)
{
  EventRecord ev;
  unsigned long n = 0;
  char c;

  if (win == NULL) return -1;
  SetPort(win);
  cursor();
  for (;;) {
    if (!WaitNextEvent(everyEvent, &ev, 30, NULL)) continue;
    if (ev.what != keyDown && ev.what != autoKey) {
      if (event(&ev)) goto quit;
      continue;
    }
    c = (char) (ev.message & charCodeMask);
    if (ev.modifiers & cmdKey) {
      if (c == 'q' || c == 'Q') goto quit;
      continue;
    }
    cursor();
    if (c == '\r' || c == 3) {          /* Return, Enter */
      put('\n');
      buf[n] = '\0';
      return (int) n;
    }
    if (c == '\b') {
      if (n > 0) {                      /* a whole UTF-8 character */
        while (n > 0 && ((unsigned char) buf[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        put('\b');
      }
    } else if ((unsigned char) c >= 32 && c != 127 &&
               n + 3 < size && col < cols - 1) {
      n += roman_to_utf8((unsigned char) c, buf + n);
      put(c);
    }
    cursor();
  }
quit:
  cursor();
  return -1;
}
