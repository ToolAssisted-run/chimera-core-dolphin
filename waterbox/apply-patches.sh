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
		echo "ERROR: patch neither applies cleanly nor reverses: $(basename "$p")" >&2
		echo "  The dolphin tree at $dolphin is in a partially-applied or" >&2
		echo "  hand-edited state (e.g. an edit sitting atop an already-applied" >&2
		echo "  patch). Refusing to build - a stale or broken guest must never be" >&2
		echo "  packaged. Reset the tree, then re-run the build:" >&2
		echo "    git -C \"$dolphin\" checkout -- . && git -C \"$dolphin\" clean -fd" >&2
		exit 1
	fi
done
