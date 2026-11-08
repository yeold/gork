Name:           gork
Version:        1.6
Release:        2%{?dist}
Summary:        Lightweight AI agent harness for old and obscure operating systems
# cJSON, which is vendored, is MIT
License:        GPL-3.0-or-later AND MIT
URL:            https://github.com/yeold/gork
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc make ncurses-devel
# curses static on Fedora (below); openSUSE's static libtinfo.a segfaults.
%if !0%{?suse_version}
BuildRequires:  ncurses-static
%endif

%description
gork runs an AI agent's tool-use loop on systems from 1987 to today: old
Linux, BSD and Solaris releases, DOS and Windows 95 as well as current ones.
It is small, strict C89 with no dependencies beyond libc, curses and the
vendored cJSON, and talks plaintext HTTP/1.0 to a TLS relay.

%prep
%autosetup

%build
%configure
# Linked against one distribution's shared libncursesw, the package runs
# only on that one: openSUSE builds curses --enable-reentrant, which hides
# stdscr, curscr, LINES and COLS, so the first word gork says there is
# "undefined symbol: curscr".  Static curses, as linuxbuild.sh does it, runs
# on both.  Not on openSUSE, whose static libtinfo.a segfaults (a six-line
# newterm program does it too); there the rpm is for that release anyway.
%if 0%{?suse_version}
%make_build
%else
%make_build LIBS="-lm -Wl,-Bstatic -lncursesw -ltinfo -Wl,-Bdynamic"
%endif

%install
%make_install

%files
%license LICENSE
%doc README.md
%{_bindir}/gork
%{_datadir}/icons/hicolor/256x256/apps/gork.png
