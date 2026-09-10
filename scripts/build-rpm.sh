#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
VERSION="$(
    sed -nE 's/^Version:[[:space:]]*//p' \
        "$ROOT/packaging/onyrion.spec" |
        head -n1
)"
TOP="${ONYRION_RPM_TOPDIR:-$ROOT/.rpmbuild}"

test -n "$VERSION"

rm -rf "$TOP"
mkdir -p \
    "$TOP/BUILD" \
    "$TOP/BUILDROOT" \
    "$TOP/RPMS" \
    "$TOP/SOURCES" \
    "$TOP/SPECS" \
    "$TOP/SRPMS"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

mkdir -p "$TMP/onyrion-$VERSION"

tar \
    -C "$ROOT" \
    --exclude=.git \
    --exclude=.rpmbuild \
    --exclude=dist \
    -cf - . |
    tar -C "$TMP/onyrion-$VERSION" -xf -

tar -C "$TMP" \
    -czf "$TOP/SOURCES/onyrion-$VERSION.tar.gz" \
    "onyrion-$VERSION"

cp "$ROOT/packaging/onyrion.spec" "$TOP/SPECS/onyrion.spec"

rpmbuild \
    --define "_topdir $TOP" \
    -ba "$TOP/SPECS/onyrion.spec"

mkdir -p "$ROOT/dist"

find "$TOP/RPMS" "$TOP/SRPMS" \
    -type f \
    -name '*.rpm' \
    -exec cp -f {} "$ROOT/dist/" \;

printf '\nRPM artifacts:\n'
find "$ROOT/dist" -maxdepth 1 -type f -name '*.rpm' -print
