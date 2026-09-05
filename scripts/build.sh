#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_ROOT="${ONYRION_BUILD_DIR:-$ROOT/build}"
BUILD_TYPE="${ONYRION_BUILD_TYPE:-debugoptimized}"
PREFIX="${ONYRION_PREFIX:-/usr}"

setup_one() {
    src="$1"
    build="$2"

    if [ -f "$build/meson-private/coredata.dat" ]; then
        meson setup \
            --reconfigure \
            "$build" \
            "$src" \
            --prefix="$PREFIX" \
            --buildtype="$BUILD_TYPE"
    else
        meson setup \
            "$build" \
            "$src" \
            --prefix="$PREFIX" \
            --buildtype="$BUILD_TYPE"
    fi
}

mkdir -p "$BUILD_ROOT"

setup_one "$ROOT/core" "$BUILD_ROOT/core"
meson compile -C "$BUILD_ROOT/core"

setup_one "$ROOT/shell" "$BUILD_ROOT/shell"
meson compile -C "$BUILD_ROOT/shell"

printf '\nOnyrion build complete.\n'
printf 'Core:  %s\n' "$BUILD_ROOT/core/onyrion"
printf 'Shell: %s\n' "$BUILD_ROOT/shell/onyrion-shell"
