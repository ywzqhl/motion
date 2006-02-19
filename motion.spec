Name:           motion
Version:        3.2.5
Release:        1
Summary:        MOTION, a Video-surveilance-system

Group:          Applications/Multimedia
License:        GPL
URL:            http://motion.sourceforge.net/
Source0:        http://prdownloads.sourceforge.net/%{name}/%{name}-%{version}.tar.gz
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires:  libjpeg-devel ffmpeg-devel
BuildRequires:  postgresql-devel mysql-devel

%description
Motion is a software motion detector. It grabs images from video4linux
devices and/or from webcams (such as the axis network cameras). Motion
is the perfect tool for keeping an eye on your property keeping only
those images that are interesting. Motion is strictly command line
driven and can run as a daemon with a rather small footprint. It is
built with MySQL and PostgreSQL support and mpegs generated by ffmpeg
and http remote control.

%prep

%setup -q

%build

CFLAGS=`echo "$CFLAGS" | sed -e 's/D_FORTIFY_SOURCE=2/D_FORTIFY_SOURCE=1/'`

%configure --sysconfdir=%{_sysconfdir}/%{name} \
	--without-optimizecpu \
	--without-libjpeg-mmx

make %{?_smp_mflags}

%install
rm -rf $RPM_BUILD_ROOT motion.init
make install DESTDIR=$RPM_BUILD_ROOT

(cd $RPM_BUILD_ROOT%{_sysconfdir}/%{name} ; mv motion-dist.conf motion.conf)

sed  -e 's#MOTION-/usr/#MOTION-#g' motion.init-RH > motion.init
install -D -m 0755 motion.init $RPM_BUILD_ROOT%{_initrddir}/%{name}

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr (-,root,root,-)
%doc CHANGELOG COPYING CREDITS INSTALL README motion_guide.html
%doc motion-dist.conf thread1.conf thread2.conf thread3.conf thread4.conf
%dir %{_sysconfdir}/%{name}
%config %{_sysconfdir}/%{name}/motion.conf
%{_bindir}/motion
%{_mandir}/man1/motion.1*
%{_initrddir}/%{name}


%changelog
* Sun Sep 18 2005 Kenneth Lavrsen <kenneth@lavrsen.dk> - 3.2.4-1
- Generic version of livna spec file replacing the old less optimal specfile.

* Thu Sep 15 2005 Dams <anvil[AT]livna.org> - 3.2.3-0.lvn.1
- Initial released based upon upstream spec file