#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_ROOT="${ONYRION_BUILD_DIR:-$ROOT/build}"

if [ ! -f "$BUILD_ROOT/core/meson-private/coredata.dat" ] ||
   [ ! -f "$BUILD_ROOT/shell/meson-private/coredata.dat" ]; then
    echo "Build directories are missing. Run ./scripts/build.sh first." >&2
    exit 2
fi

meson install -C "$BUILD_ROOT/core"
meson install -C "$BUILD_ROOT/shell"

printf '\nOnyrion install complete.\n'
