Name:           gork
Version:        0.3
Release:        1%{?dist}
Summary:        AI agent harness for machines that predate TLS
# ponytail: placeholder until gork picks a license (cJSON is MIT)
License:        unspecified
URL:            https://github.com/yeold/gork
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc make ncurses-devel

%description
gork runs an agent's tool-use loop in strict C89 with no dependencies beyond
libc, curses and vendored cJSON, talking plaintext HTTP/1.0 to a TLS relay.

%prep
%autosetup

%build
%configure
%make_build

%install
%make_install

%files
%doc README.md
%{_bindir}/gork
%{_datadir}/icons/hicolor/256x256/apps/gork.png
