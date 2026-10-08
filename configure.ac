AC_INIT([gork], [0.2.2])
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

AC_CHECK_HEADER([curses.h], [], [AC_MSG_ERROR([curses.h not found; install ncurses headers])])
# ncurses on Linux/FreeBSD/OpenBSD, plain curses on NetBSD and Solaris.
# `./configure LIBS=-lncursesw` for UTF-8 terminals.
AC_SEARCH_LIBS([initscr], [ncurses curses], [], [AC_MSG_ERROR([no curses library found])])
AC_SEARCH_LIBS([fabs], [m])
# Solaris keeps sockets and the resolver out of libc.
AC_SEARCH_LIBS([gethostbyname], [nsl])
AC_SEARCH_LIBS([socket], [socket])

# -W, not -Wextra: the spelling gcc 3.2 (SUSE 8.1) knows.
if test "$GCC" = yes; then
  AM_CFLAGS="-std=gnu89 -pedantic -Wall -W"
fi
AC_SUBST([AM_CFLAGS])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
