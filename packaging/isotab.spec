Name:           isotab
Version:        %{isotab_version}
Release:        1%{?dist}
Summary:        Manage separate browser profiles
License:        GPL-3.0-only
URL:            https://github.com/niko3x/isotab
Source0:        isotab-%{version}.tar.gz
BuildRequires:  gcc, make, pkgconfig(gtk+-3.0), pkgconfig(glib-2.0)
Requires:       librsvg2

%description
A native GTK3 session manager for installed browsers.

%prep
%autosetup

%build
%make_build

%install
%make_install PREFIX=%{_prefix} DATADIR=%{_datadir}

%check
# Tests refuse root; build this package as a regular user.
make test

%files
%{_bindir}/isotab
%{_datadir}/applications/isotab.desktop
%{_datadir}/icons/hicolor/scalable/apps/isotab.svg
%license %{_datadir}/licenses/isotab/LICENSE
