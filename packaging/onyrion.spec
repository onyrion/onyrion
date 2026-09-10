Name:           onyrion
Version:        0.1.0
Release:        0.1.prebeta%{?dist}
Summary:        Experimental Wayland desktop environment

License:        GPL-3.0-only
URL:            https://github.com/onyrion/onyrion
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  git
BuildRequires:  meson >= 1.8
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  rust
BuildRequires:  cargo
BuildRequires:  wayland-devel
BuildRequires:  wayland-protocols-devel
BuildRequires:  wlr-protocols-devel
BuildRequires:  wlroots-devel >= 0.20
BuildRequires:  libxkbcommon-devel
BuildRequires:  libinput-devel
BuildRequires:  glib2-devel
BuildRequires:  json-glib-devel
BuildRequires:  pipewire-devel >= 1.6
BuildRequires:  libxcb-devel
BuildRequires:  gtk4-devel
BuildRequires:  gtk4-layer-shell-devel

Requires:       NetworkManager
Requires:       bluez
Requires:       pipewire
Requires:       wireplumber
Requires:       upower
Requires:       power-profiles-daemon
Requires:       polkit
Requires:       xdg-desktop-portal
Requires:       xdg-desktop-portal-wlr
Requires:       xwayland-satellite

%description
Onyrion is an experimental Wayland desktop environment consisting of a wlroots
compositor and a separate semantic Shell/UI layer.

This RPM is a pre-beta-development-snapshot package. Ewwii is currently an external runtime
dependency and is not bundled by this RPM.

%prep
%autosetup -n %{name}-%{version}

%build
# Fedora's fortified libc annotates fgets() with warn_unused_result.
# Current Shell deliberately discards one best-effort probe result in
# default_interface(). Keep the project's global -Werror policy intact while
# downgrading only this Fedora-specific category for the RPM build.
export CFLAGS="%{build_cflags} -Wno-error=unused-result"

meson setup build/core core     --prefix=%{_prefix}     --libdir=%{_libdir}     --libexecdir=%{_libexecdir}     --buildtype=release
meson compile -C build/core

meson setup build/shell shell     --prefix=%{_prefix}     --libdir=%{_libdir}     --libexecdir=%{_libexecdir}     --buildtype=release
meson compile -C build/shell

%install
DESTDIR=%{buildroot} meson install -C build/core
DESTDIR=%{buildroot} meson install -C build/shell

%files
%license LICENSE
%{_bindir}/onyrion
%{_bindir}/onyrion-shell
%{_bindir}/onyrion-control
%{_bindir}/onyrionctl
%{_libexecdir}/onyrion/
%{_datadir}/onyrion/
%{_datadir}/wayland-sessions/onyrion.desktop
%{_datadir}/xdg-desktop-portal/onyrion-portals.conf

%changelog
* Thu Sep 10 2026 Onyrion Project <onyrion@users.noreply.github.com> - 0.1.0-0.1.prebeta
- Publish current pre-beta-development-snapshot development snapshot.
