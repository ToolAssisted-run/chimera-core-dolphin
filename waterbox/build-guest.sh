#!/bin/sh
# Guest (waterbox) build: the same CMake and options, under the miniBox musl
# toolchain. Artifacts land in build/guest.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -B "$root/build/guest" -DCMAKE_TOOLCHAIN_FILE="$here/guest-toolchain.cmake" $DOLPHIN_OPTS "$root/extern/dolphin"
make -C "$root/build/guest" -j"$(nproc)" "$@"
