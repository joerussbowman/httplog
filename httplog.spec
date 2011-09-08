Name: httplog
Group: Applications/Internet
Version: 2.1.1
Release: 5
License: FSL
Summary:  A utility to rollover Apache log files automatically
Source0: http://nutbar.chemlab.org/downloads/programs/httplog-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-root
BuildPrereq: zlib-devel

%description
This program makes Apache act similar to Microsofts' IIS webserver in its
logging style (as opposed to every logfile entry in one single huge logfile).
This allows you to easily maintain your webserver logfiles for statistics
packages in an easily organizable manner without user intervention.

%prep

%setup -q

%build
./configure --bindir=$RPM_BUILD_ROOT%{_sbindir} --mandir=$RPM_BUILD_ROOT%{_mandir}
make

%install
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT
make install

%clean
[ "$RPM_BUILD_ROOT" != "/" ] && rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root)
%doc ChangeLog INSTALL LICENSE README
%{_sbindir}/*
%{_mandir}/man8/*

%changelog
* Thu Sep 8 2011 Joseph Bowman <jbowman@tnc.org>
- Removed zlib checks in configure, and added -lz option to CFLAGS
* Sun Nov 25 2001 Eli Sand <nutbar@innocent.com>
- updated to use configure script
* Tue Jul 17 2001 Andrew Anderson <andrew@redhat.com>
- initial packaging

