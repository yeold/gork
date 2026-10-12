AC_INIT([gork], [0.10~beta])
AC_CONFIG_SRCDIR([main.c])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CC
AM_PROG_CC_C_O          # old automake needs it for cJSON/cJSON.c

# No system curses.h but a private ncurses in ~/local (see README)?  Use it.
# Explicit CPPFLAGS/LDFLAGS on the configure line still win.
if test ! -f /usr/include/curses.h && test -f "$HOME/local/include/curses.h"; then
  CPPFLAGS="$CPPFLAGS -I$HOME/local/include"
  LDFLAGS="$LDFLAGS -L$HOME/local/lib"
fi

# -W, not -Wextra: the spelling gcc 3.2 (SUSE 8.1) knows.
if test "$GCC" = yes; then
  AM_CFLAGS="-std=gnu89 -pedantic -Wall -W"
fi
AC_SUBST([AM_CFLAGS])
# Run the checks as C89 too: new autoconf puts -std=gnu23 in CC, and the
# Solaris curses.h (typedef char bool) does not compile as C23.
save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS $AM_CFLAGS"

AC_CHECK_HEADER([curses.h], [], [AC_MSG_ERROR([curses.h not found; install ncurses headers])])
# ncurses on Linux/FreeBSD/OpenBSD, plain curses on NetBSD and Solaris.
# `./configure LIBS=-lncursesw` for UTF-8 terminals; it is also the last
# resort, for systems that have only the wide library.
# A System V curses.h (it renames initscr) needs libcurses even where a
# libncurses is installed too, as on illumos.
AC_CHECK_DECL([initscr32], [gork_curses="curses ncurses"], [gork_curses="ncurses curses ncursesw"],
              [[#include <curses.h>]])
AC_SEARCH_LIBS([initscr], [$gork_curses], [], [AC_MSG_ERROR([no curses library found])])
AC_SEARCH_LIBS([fabs], [m])
# Solaris keeps sockets and the resolver out of libc, Haiku in libnetwork.
AC_SEARCH_LIBS([gethostbyname], [nsl network])
AC_SEARCH_LIBS([socket], [socket network])

CFLAGS=$save_CFLAGS

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
