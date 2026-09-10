# RPM / Fedora

Текущая цель RPM — **Fedora 44+**. Fedora 44 предоставляет wlroots 0.20.x,
PipeWire 1.6.x и необходимые devel-пакеты.

Статус пакета: **pre-beta-development-snapshot**, версия **0.1.0**.

Установить build dependencies:

```sh
sudo dnf install \
  gcc git meson ninja-build pkgconf-pkg-config rust cargo rpm-build \
  wayland-devel wayland-protocols-devel wlr-protocols-devel \
  wlroots-devel libxkbcommon-devel libinput-devel \
  glib2-devel json-glib-devel pipewire-devel libxcb-devel \
  gtk4-devel gtk4-layer-shell-devel
```

Собрать:

```sh
./scripts/build-rpm.sh
```

Установить полученный пакет:

```sh
sudo dnf install ./dist/onyrion-*.rpm
```

RPM устанавливает Onyrion Core/Shell/session/UI files и системные runtime
dependencies, доступные в Fedora. **Ewwii пока не входит в Fedora RPM и должен
быть установлен отдельно**; поэтому успешная RPM transaction сама по себе не
равна полной functional-V Onyrion session.
