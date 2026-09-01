#!/bin/sh
# Applies the numbered patches to the extern/dolphin submodule. Idempotent:
# a tree that already carries the changes is left alone; anything else is an
# error worth seeing. The driver lives in waterbox/ and is built OUTSIDE the
# dolphin tree, so nothing is copied in.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
dolphin="$root/extern/dolphin"

for p in "$root"/patches/*.patch; do
	if git -C "$dolphin" apply --check "$p" 2>/dev/null; then
		git -C "$dolphin" apply "$p"
		echo "applied: $(basename "$p")"
	elif git -C "$dolphin" apply --reverse --check "$p" 2>/dev/null; then
		echo "already applied: $(basename "$p")"
	else
		echo "NEITHER applies nor reverses: $(basename "$p")" >&2
		exit 1
	fi
done
