bin_PROGRAMS = gork
gork_SOURCES = main.c relay.c relay.h tui.c tui.h mcp.c mcp.h port.h \
                cJSON/cJSON.c cJSON/cJSON.h
EXTRA_DIST = README.md LICENSE CONTRIBUTING.md docs/BUILD.md docs/HACKING.md docs/MAINTAINING.md Makefile.dj dosbuild.sh dosspx.c win.c \
             dos16build.sh dos16/watcom.err dos16/syserr.c \
             Makefile.w32 winbuild.sh nt3crt.c aixps2.c minix2.c minix2.h nextstep.c sunos4.c beos.c cygpack.sh linuxbuild.sh gork.spec debian/changelog debian/control \
             debian/rules debian/copyright debian/source/format

iconsdir = $(datadir)/icons/hicolor/256x256/apps
dist_icons_DATA = images/gork.png
