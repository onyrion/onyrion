# Onyrion

**Onyrion** — экспериментальная среда рабочего стола для Wayland, в которой
композитор, семантическая модель окон и системная оболочка разделены на два
основных компонента: **Core** и **Shell**.

Проект находится в активной разработке и публикуется до первого стабильного
релиза, чтобы уже сейчас можно было собрать исходный код, изучить архитектуру и
запустить текущую версию.

> **Статус:** рабочий прототип / development snapshot.  
> Это ещё не стабильный релиз для повседневного использования без резервного
> способа входа в систему.

## Архитектура

```text
Onyrion
├── Core
│   ├── Wayland-композитор
│   ├── модель Output / Workspace / Group / Window
│   ├── раскладка и управление окнами
│   ├── клавиатура, указатель, touchpad и gestures
│   ├── layer-shell
│   ├── session lock
│   └── compositor-side protocols
│
└── Shell
    ├── управление desktop-сессией
    ├── семантический control plane
    ├── launcher
    ├── Quick Settings
    ├── системные интеграции
    └── UI на базе Ewwii
```

### Core

Core написан на **C23** и использует **wlroots 0.20**.

Core является владельцем канонического состояния композитора:

- выходы и рабочие пространства;
- группы и окна;
- tiled / floating / fullscreen режимы;
- focus и layout;
- клавиатурный и pointer input;
- touchpad policy;
- gestures;
- layer-shell;
- session lock;
- screencopy;
- typed Core → Shell protocol.

Базовая модель окон:

```text
Output
  -> visible Workspace
      -> Layout tree
          -> Group
              -> ordered Windows
```

В группе одновременно активным является одно окно.

### Shell

Shell отвечает за desktop/session-level поведение и пользовательский интерфейс.

В текущей реализации присутствуют:

- launcher;
- per-output bar;
- Quick Settings;
- Wi-Fi;
- Bluetooth;
- PipeWire audio;
- battery state;
- power profiles;
- SNI tray;
- notifications;
- session lifecycle;
- semantic actions;
- Ewwii UI integration.

Динамические коллекции Wi-Fi, Bluetooth, audio и SNI tray отрисовываются через
плагин Onyrion для Ewwii, без многократного разбора больших JSON-структур внутри
NBCL.

## Текущее состояние

Onyrion уже запускается как самостоятельная Wayland-сессия и используется для
реального тестирования разработки.

Реализованы, в частности:

- native Wayland session;
- несколько рабочих пространств;
- глобальная модель workspace ID;
- привязка workspace к output;
- группы окон;
- tiled и floating режимы;
- fullscreen;
- XWayland integration;
- launcher;
- per-output bar;
- Quick Settings;
- Wi-Fi и Bluetooth state/actions;
- PipeWire audio controls;
- battery и power profiles;
- SNI tray;
- session lock foundation;
- screencopy protocol support;
- keyboard / pointer / touchpad configuration;
- standalone Super-key lifecycle;
- pointer move / resize / workspace wheel actions;
- базовая gesture infrastructure.

На момент этой публикации Shell dynamic-renderer закрыт по текущему
функциональному объёму, а Core проходит финальную физическую проверку input /
gesture / lock / regression paths перед первой beta-версией.

Проект пока следует запускать с доступным резервным VT или другой рабочей
сессией.

## Source snapshot

- Core HEAD: `9cb37a698b05fd45297177f2d7cd31f297e36668` — tracked worktree in progress (10 status entries)
- Shell HEAD: `cec69f6fc853a0b57cfd4766982caaec1adaa12c` — clean

Публичный snapshot содержит текущие working-tree bytes **tracked-файлов**.
Untracked local files, ignored build/cache output и вложенные Git metadata в
публикацию не входят.

Точная граница исходников сохранена в `SOURCE-SNAPSHOT.md`.

## Структура репозитория

```text
.
├── core/                  Onyrion compositor
├── shell/                 Onyrion Shell и desktop UI
├── scripts/
│   ├── build.sh
│   └── install.sh
├── LICENSE
└── README.md
```

Core и Shell остаются двумя отдельными Meson-проектами. Верхнеуровневый
`build.sh` собирает их в правильном порядке.

## Требования для сборки

Основная среда разработки проекта сейчас — **Arch Linux**.

### Arch Linux

Установить зависимости:

```sh
sudo pacman -S --needed \
  base-devel \
  git \
  meson \
  ninja \
  pkgconf \
  rust \
  wayland \
  wayland-protocols \
  wlr-protocols \
  wlroots0.20 \
  libxkbcommon \
  libinput \
  glib2 \
  json-glib \
  libpipewire \
  libxcb \
  gtk4 \
  gtk4-layer-shell
```

Важно: `wlr-protocols` требуется отдельно от `wayland-protocols`. Core
получает `wlr-layer-shell-unstable-v1.xml` через `wlr-protocols.pc`.

Ewwii plugin для Shell собирается через Cargo. Версия API плагина закреплена в
исходниках Shell, поэтому при первой сборке может потребоваться доступ к сети
для получения Cargo-зависимостей.

## Сборка

После публикации репозитория:

```sh
git clone https://github.com/onyrion/onyrion.git
cd onyrion
./scripts/build.sh
```

По умолчанию:

```text
prefix:     /usr
build type: debugoptimized
build dir:  ./build
```

Основные артефакты:

```text
build/core/onyrion
build/shell/onyrion-shell
build/shell/onyrion-control
build/shell/onyrion-ewwii-plugin.so
```

Для другой build-директории или типа сборки:

```sh
ONYRION_BUILD_DIR=/tmp/onyrion-build \
ONYRION_BUILD_TYPE=release \
./scripts/build.sh
```

### Ручная сборка

Core:

```sh
meson setup build/core core \
  --prefix=/usr \
  --buildtype=debugoptimized

meson compile -C build/core
```

Shell:

```sh
meson setup build/shell shell \
  --prefix=/usr \
  --buildtype=debugoptimized

meson compile -C build/shell
```

## Установка

После успешной сборки:

```sh
sudo ./scripts/install.sh
```

Текущая Meson-конфигурация устанавливает runtime в стандартный `/usr` layout,
включая:

```text
/usr/bin/onyrion
/usr/bin/onyrion-shell
/usr/bin/onyrion-control
/usr/bin/onyrionctl

/usr/libexec/onyrion/

/usr/share/onyrion/
/usr/share/wayland-sessions/onyrion.desktop
```

После установки нужно завершить текущую графическую сессию и выбрать
**Onyrion** в Wayland-совместимом display manager.

## Runtime-зависимости

В зависимости от используемых функций текущая сессия ожидает стандартные
Linux desktop services:

- Ewwii;
- NetworkManager;
- BlueZ;
- PipeWire / WirePlumber;
- UPower;
- power profiles service;
- logind;
- XWayland;
- xdg-desktop-portal;
- Polkit authentication agent.

Onyrion не пытается переписывать эти зрелые системные компоненты и использует
их как внешние сервисы.

## Конфигурация

Onyrion использует семантические конфигурационные файлы KDL.

Системные defaults:

```text
/usr/share/onyrion/config/
```

Пользовательские конфигурации:

```text
~/.config/onyrion/
```

Основной compositor config:

```text
~/.config/onyrion/compositor.kdl
```

Shell config:

```text
~/.config/onyrion/shell.kdl
```

Поддерживаются realtime reload и сохранение последней валидной конфигурации.
Контракт конфигурации всё ещё развивается до первого стабильного релиза.

## Сборка пакетов

Первая публичная публикация делается в формате **source-first**:

1. исходный код;
2. README;
3. проверенный build path;
4. install path.

После завершения текущего Core closeout планируется добавить:

- RPM spec;
- сборку RPM;
- проверку install / upgrade / remove;
- публикацию бинарных package artifacts.

## Состояние проекта перед первой beta

Перед beta остаётся закрыть финальную физическую проверку Core:

- 3-finger swipe;
- 3-finger pinch;
- realtime config/LKG regression;
- secure-lock input regression;
- ordinary Wayland pointer regression;
- cumulative physical input gate.

После этого development snapshot будет переведён в первую beta-границу.

## Участие в разработке

Проект пока быстро меняется: внутренние protocols, конфигурация и часть UI
могут быть несовместимо изменены до первого стабильного релиза.

Наиболее полезны сейчас:

- воспроизводимые bug reports;
- runtime traces;
- Wayland/input regression reports;
- тесты на другом оборудовании;
- проверка сборки на других Linux-дистрибутивах.

## Лицензия

Код Onyrion распространяется по лицензии **GNU GPL v3**, если конкретный файл
или сторонний компонент не указывает другую лицензию.

Лицензии сторонних компонентов сохраняются в соответствующих каталогах.

## RPM / Fedora

Для конференционной и тестовой установки добавлена RPM-сборка для
**Fedora 44+**.

Текущая граница публикации: **Onyrion 0.1.0, pre-beta-development-snapshot**.

```sh
sudo dnf install \
  gcc git meson ninja-build pkgconf-pkg-config rust cargo rpm-build \
  wayland-devel wayland-protocols-devel wlr-protocols-devel \
  wlroots-devel libxkbcommon-devel libinput-devel \
  glib2-devel json-glib-devel pipewire-devel libxcb-devel \
  gtk4-devel gtk4-layer-shell-devel

./scripts/build-rpm.sh
sudo dnf install ./dist/onyrion-*.rpm
```

Подробности и текущее ограничение Ewwii:
[`packaging/README.md`](packaging/README.md).
