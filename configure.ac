AC_INIT([gork], [1.4])
AC_CONFIG_SRCDIR([main.c])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CC
AM_PROG_CC_C_O          # old automake needs it for cJSON/cJSON.c
# 32-bit Linux: without this, readdir() and stat() fail on file systems
# with 64-bit inode numbers, and list_dir shows an empty directory.
AC_SYS_LARGEFILE

# No system curses.h but a private ncurses in ~/local (see README)?  Use it.
# Explicit CPPFLAGS/LDFLAGS on the configure line still win.
if test ! -f /usr/include/curses.h && test -f "$HOME/local/include/curses.h"; then
  CPPFLAGS="$CPPFLAGS -I$HOME/local/include"
  LDFLAGS="$LDFLAGS -L$HOME/local/lib"
fi

AC_ARG_WITH([max-file-size],
  [AS_HELP_STRING([--with-max-file-size=BYTES],
                  [maximum file size for read_file, write_file and edit_file])],
  [case "$withval" in
     ''|yes|no) AC_MSG_ERROR([--with-max-file-size requires a byte count]) ;;
     *[!0-9]*)  AC_MSG_ERROR([--with-max-file-size=$withval: want a positive integer]) ;;
     *)         if test "$withval" -le 0 2>/dev/null; then
                  AC_MSG_ERROR([--with-max-file-size=$withval: want a positive integer])
                fi ;;
   esac
   CPPFLAGS="$CPPFLAGS -DTOOL_READ_MAX=${withval}UL -DTOOL_WRITE_MAX=${withval}UL -DTOOL_EDIT_MAX=${withval}UL"])

AC_ARG_ENABLE([unicode],
  [AS_HELP_STRING([--enable-unicode],
                  [UTF-8 in the TUI (default: auto, falls back to ASCII)])],
  [enable_unicode=$enableval], [enable_unicode=auto])
case "$enable_unicode" in
  yes|no|auto) ;;
  *) AC_MSG_ERROR([--enable-unicode=$enable_unicode: want yes, no or auto]) ;;
esac

AC_ARG_WITH([tui],
  [AS_HELP_STRING([--without-tui],
                  [build without the curses TUI (batch mode only)])],
  [with_tui=$withval], [with_tui=yes])
case "$with_tui" in
  yes|no) ;;
  *) AC_MSG_ERROR([--with-tui=$with_tui: want yes or no]) ;;
esac

AC_ARG_WITH([mcp],
  [AS_HELP_STRING([--without-mcp],
                  [build without the MCP client])],
  [with_mcp=$withval], [with_mcp=yes])
case "$with_mcp" in
  yes|no) ;;
  *) AC_MSG_ERROR([--with-mcp=$with_mcp: want yes or no]) ;;
esac
if test "$with_mcp" = no; then
  CPPFLAGS="$CPPFLAGS -DGORK_NO_MCP"
fi

AC_ARG_WITH([openssl],
  [AS_HELP_STRING([--with-openssl@<:@=DIR@:>@],
                  [do TLS with OpenSSL (1.0.2 or later; in DIR if given),
                   so that no relay is needed])],
  [with_openssl=$withval], [with_openssl=no])

AC_ARG_WITH([serial],
  [AS_HELP_STRING([--with-serial],
                  [no network: all traffic over a serial line, to
                   gork-relay -S])],
  [with_serial=$withval], [with_serial=no])
case "$with_serial" in
  yes|no) ;;
  *) AC_MSG_ERROR([--with-serial=$with_serial: want yes or no]) ;;
esac
if test "$with_serial" = yes && test "$with_openssl" != no; then
  AC_MSG_ERROR([--with-serial and --with-openssl do not go together: over a serial line the relay does the TLS])
fi

# -W, not -Wextra: the spelling gcc 3.2 (SUSE 8.1) knows.
if test "$GCC" = yes; then
  AM_CFLAGS="-std=gnu89 -pedantic -Wall -W"
fi
AC_SUBST([AM_CFLAGS])
# In a git checkout, name the build as version.sh does: v<version>-beta-
# <commits>, -dirty with tracked files modified, and the version alone at
# its release tag.  Evaluated by make's shell so edits after ./configure
# still flip -dirty; dist tarballs have no .git and keep the release
# VERSION from AC_INIT.
if (cd "$srcdir" && test -e .git && git rev-parse --verify HEAD) >/dev/null 2>&1; then
  AM_CPPFLAGS='-DGORK_VERSION=\"`cd $(srcdir) && sh version.sh`\"'
fi
AC_SUBST([AM_CPPFLAGS])
# Run the checks as C89 too: new autoconf puts -std=gnu23 in CC, and the
# Solaris curses.h (typedef char bool) does not compile as C23.
save_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS $AM_CFLAGS"

if test "$with_tui" = no; then
  CPPFLAGS="$CPPFLAGS -DGORK_NO_TUI"
else
  AC_CHECK_HEADER([curses.h], [], [AC_MSG_ERROR([curses.h not found; install ncurses headers])])
  gork_have_locale=no
  if test "$enable_unicode" != no; then
    AC_CHECK_HEADER([locale.h],
      [AC_CHECK_FUNC([setlocale], [gork_have_locale=yes])])
  fi
  # ncursesw first when Unicode is wanted, then plain ncurses/curses as
  # fallback.  A System V curses.h (it renames initscr) needs libcurses
  # even where a libncurses is installed too, as on illumos.
  if test "$enable_unicode" != no && test "$gork_have_locale" = yes; then
    AC_CHECK_DECL([initscr32], [gork_curses="curses ncurses"], [gork_curses="ncursesw ncurses curses"],
                  [[#include <curses.h>]])
  else
    AC_CHECK_DECL([initscr32], [gork_curses="curses ncurses"], [gork_curses="ncurses curses ncursesw"],
                  [[#include <curses.h>]])
  fi
  AC_SEARCH_LIBS([initscr], [$gork_curses], [], [AC_MSG_ERROR([no curses library found])])
  gork_unicode=no
  if test "$enable_unicode" != no && test "$gork_have_locale" = yes; then
    AC_CHECK_FUNC([waddnwstr], [gork_unicode=yes])
    if test "$enable_unicode" = yes; then
      gork_unicode=yes
    fi
  elif test "$enable_unicode" = yes; then
    AC_MSG_ERROR([--enable-unicode requested, but locale.h / setlocale was not found])
  fi
  if test "$gork_unicode" = yes; then
    CPPFLAGS="$CPPFLAGS -DGORK_UNICODE"
  fi
fi
AC_SEARCH_LIBS([fabs], [m])
if test "$with_serial" = yes; then
  # no sockets at all: a system with no network may have no netdb.h
  AC_CHECK_HEADER([termios.h], [],
                  [AC_MSG_ERROR([--with-serial needs termios.h])])
  CPPFLAGS="$CPPFLAGS -DGORK_SERIAL"
else
  # Solaris keeps sockets and the resolver out of libc, Haiku in libnetwork.
  AC_SEARCH_LIBS([gethostbyname], [nsl network])
  AC_SEARCH_LIBS([socket], [socket network])
fi
if test "$with_openssl" != no; then
  if test "$with_openssl" != yes; then
    CPPFLAGS="$CPPFLAGS -I$with_openssl/include"
    LDFLAGS="$LDFLAGS -L$with_openssl/lib"
  fi
  AC_CHECK_HEADER([openssl/ssl.h], [],
                  [AC_MSG_ERROR([--with-openssl: openssl/ssl.h not found; install the OpenSSL headers (libssl-dev, openssl-devel)])])
  AC_SEARCH_LIBS([ERR_get_error], [crypto], [],
                 [AC_MSG_ERROR([--with-openssl: no libcrypto])])
  AC_SEARCH_LIBS([SSL_new], [ssl], [],
                 [AC_MSG_ERROR([--with-openssl: no libssl])])
  AC_CHECK_FUNC([X509_VERIFY_PARAM_set1_host], [],
                [AC_MSG_ERROR([--with-openssl needs OpenSSL 1.0.2 or later, to check the server's name])])
  CPPFLAGS="$CPPFLAGS -DGORK_OPENSSL"
fi

CFLAGS=$save_CFLAGS

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
