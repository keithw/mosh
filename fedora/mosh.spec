Name:		mosh
Version:	1.1
Release:	1%{?dist}
Summary:	Mobile shell that supports roaming and intelligent local echo

License:	GPLv3+
Group:		Applications/Internet
URL:		http://mosh.mit.edu/
Source0:	https://github.com/downloads/keithw/mosh/mosh-%{version}.tar.gz

BuildRequires:	protobuf-compiler
BuildRequires:	protobuf-devel
BuildRequires:	libutempter-devel
BuildRequires:	boost-devel
BuildRequires:	zlib-devel
BuildRequires:	ncurses-devel
Requires:	openssh-clients
Requires:	perl-IO-Tty

%description
Mosh is a remote terminal application that supports:
  - intermittent network connectivity,
  - roaming to different IP address without dropping the connection, and
  - intelligent local echo and line editing to reduce the effects
    of "network lag" on high-latency connections.


%prep
%setup -q


%build
%configure --enable-compile-warnings=error
make %{?_smp_mflags}


%install
make install DESTDIR=$RPM_BUILD_ROOT


%files
%doc README.md COPYING ChangeLog
%{_bindir}/*
%{_mandir}/man1/*


%changelog
* Wed Mar 21 2012  <achernya@mit.edu> - 1.1-1
- Initial packaging for mosh.
