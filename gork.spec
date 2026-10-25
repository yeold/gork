Name:           gork
Version:        1.3
Release:        1%{?dist}
Summary:        Lightweight AI agent harness for old and obscure operating systems
# cJSON, which is vendored, is MIT
License:        GPL-3.0-or-later AND MIT
URL:            https://github.com/yeold/gork
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc make ncurses-devel

%description
gork runs an AI agent's tool-use loop on systems from 1987 to today: old
Linux, BSD and Solaris releases, DOS and Windows 95 as well as current ones.
It is small, strict C89 with no dependencies beyond libc, curses and the
vendored cJSON, and talks plaintext HTTP/1.0 to a TLS relay.

%prep
%autosetup

%build
%configure
%make_build

%install
%make_install

%files
%license LICENSE
%doc README.md
%{_bindir}/gork
%{_datadir}/icons/hicolor/256x256/apps/gork.png
