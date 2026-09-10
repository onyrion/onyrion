#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: build.sh CARGO MANIFEST TARGET_DIR OUTPUT" >&2
    exit 64
fi

cargo_bin=$1
manifest=$2
requested_target_dir=$3
output=$4

# Meson supplies an isolated build-local target by default. For development,
# allow the standard Cargo target-dir environment contract to opt into a
# persistent cache without hardcoding a user-specific path in project source.
target_dir=${CARGO_TARGET_DIR:-$requested_target_dir}
printf 'ONYRION_PLUGIN_CARGO_TARGET_DIR=%s\n' "$target_dir" >&2

# Cargo/libgit2 can fail against the same repository that system git fetches
# correctly. Keep the checked-in git dependency exact, but use the verified
# system-git transport for Cargo fetches.
export CARGO_NET_GIT_FETCH_WITH_CLI=true

"$cargo_bin" build \
    --locked \
    --release \
    --manifest-path "$manifest" \
    --target-dir "$target_dir"

built="$target_dir/release/libonyrion_ewwii_plugin.so"
if [ ! -f "$built" ]; then
    echo "FAIL: expected plugin artifact missing: $built" >&2
    exit 1
fi

cp -- "$built" "$output"
