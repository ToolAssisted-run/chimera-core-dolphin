#!/bin/sh
# Native reference build: dolphin's own CMake, host toolchain, the canonical
# option set. Artifacts land in build/native.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
. "$here/configure-flags.sh"
"$here/apply-patches.sh"
cmake -B "$root/build/native" $DOLPHIN_OPTS "$root/extern/dolphin"
make -C "$root/build/native" -j"$(nproc)" "$@"
