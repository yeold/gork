bin_PROGRAMS = gork
gork_SOURCES = main.c relay.c relay.h tui.c tui.h mcp.c mcp.h port.h \
                cJSON/cJSON.c cJSON/cJSON.h
EXTRA_DIST = README.md LICENSE CONTRIBUTING.md docs/BUILD.md docs/build/aix.md docs/build/beos.md docs/build/dos.md docs/build/hpux.md docs/build/minix.md docs/build/nextstep.md docs/build/os2.md docs/build/plan9.md docs/build/qnx.md docs/build/sunos4.md docs/build/svr4.md docs/build/uefi.md docs/build/windows.md docs/HACKING.md docs/MAINTAINING.md Makefile.dj dosbuild.sh dosspx.c win.c \
             dos16build.sh dos16/watcom.err dos16/syserr.c \
             Makefile.w32 winbuild.sh nt3crt.c aixps2.c minix2.c minix2.h nextstep.c sunos4.c beos.c svr40.c plan9.c notui.c cygpack.sh linuxbuild.sh bsdbuild.sh version.sh \
             uefibuild.sh uefi/efi.h uefi/libc.c uefi/net.c uefi/card.c ip.c ip.h \
             uefi/include/arpa/inet.h uefi/include/assert.h \
             uefi/include/ctype.h uefi/include/dirent.h \
             uefi/include/errno.h uefi/include/fcntl.h \
             uefi/include/locale.h uefi/include/math.h \
             uefi/include/netdb.h uefi/include/netinet/in.h \
             uefi/include/signal.h uefi/include/stdio.h \
             uefi/include/stdlib.h uefi/include/string.h \
             uefi/include/sys/select.h uefi/include/sys/socket.h \
             uefi/include/sys/stat.h uefi/include/sys/time.h \
             uefi/include/sys/types.h uefi/include/sys/wait.h \
             uefi/include/time.h uefi/include/unistd.h \
             gork.spec debian/changelog debian/control \
             debian/rules debian/copyright debian/source/format

iconsdir = $(datadir)/icons/hicolor/256x256/apps
dist_icons_DATA = images/gork.png
