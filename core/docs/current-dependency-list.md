# Current dependency list

Project: Onyrion / WLDE  
Language: C  
Environment: distrobox `onyrion-dev`  
Base image: `docker.io/library/archlinux:latest`

## Goal

Initial C development environment for Onyrion compositor/core prototype:

- compositor skeleton
- workspace model
- layout tree
- tab groups
- input handling
- future desktop shell layer

## Build system

- meson
- ninja
- cmake
- just
- pkgconf

## Compiler / debugging stack

- base-devel
- clang
- llvm
- lld
- gdb
- valgrind

## Version control

- git
- git-lfs

## Wayland / compositor dependencies

- wayland
- wayland-protocols
- wlroots0.20
- libxkbcommon
- libinput
- systemd-libs
- seatd
- mesa
- libglvnd
- libdrm
- pixman
- xorg-xwayland

## Notes

Onyrion currently targets pure C.

The first prototype should focus on:

1. state model
2. workspace model
3. layout tree
4. tab groups
5. compositor skeleton

For exact installed package versions, see:

- `docs/current-arch-packages.lock`

## Tool versions

```text
cc (GCC) 16.1.1 20260625
clang version 22.1.6
1.11.1
1.13.2
git version 2.55.0
2.5.1
```
