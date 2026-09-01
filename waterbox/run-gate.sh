#!/bin/sh
# The dolphin core gate. Tiered like the pcsx2 gate: every leg that cannot
# run SKIPs with what it would have proven. Usage: ./run-gate.sh [frames]
#
# Tier 1 needs nothing beyond the repo (swiss is committed, GPL).
# Tier 2 needs a commercial GC disc in tests/roms-local (user-supplied).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
frames="${1:-120}"
sys="$root/extern/dolphin/Data/Sys"
swiss="$root/tests/roms/swiss_r2092.dol"
disc="$root/tests/roms-local/Mortal Kombat - Deadly Alliance.iso"
work="$here/tests/work"
pass=0; fail=0; skip=0

say() { printf '%s\n' "$*"; }
PASS() { say "PASS: $*"; pass=$((pass+1)); }
FAIL() { say "FAIL: $*"; fail=$((fail+1)); }
SKIP() { say "SKIP: $*"; skip=$((skip+1)); }

rm -rf "$work"; mkdir -p "$work"

[ -x "$here/obj-native/run-native" ] || { say "run-native missing - make -f native.mk"; exit 2; }
[ -x "$here/bin/run-wbx" ] || { say "run-wbx missing - ./build-core.sh"; exit 2; }

nat() { d="$1"; shift; rm -rf "$work/$d"; "$here/obj-native/run-native" --sys "$sys" --user "$work/$d" "$@" 2>/dev/null | grep '^frame'; }
wbx() { "$here/bin/run-wbx" "$here/bin/core.wbx" --sys "$sys" "$@" 2>/dev/null | grep '^frame'; }

# ---- tier 1: swiss ---------------------------------------------------------
nat n1 --frames "$frames" --report 1 "$swiss" > "$work/n1.txt"
nat n2 --frames "$frames" --report 1 "$swiss" > "$work/n2.txt"
if cmp -s "$work/n1.txt" "$work/n2.txt"; then PASS "native deterministic at $frames frames"
else FAIL "native deterministic at $frames frames"; fi

wbx --frames "$frames" --report 1 "$swiss" > "$work/g1.txt"
if cmp -s "$work/n1.txt" "$work/g1.txt"; then PASS "native == sandbox at $frames frames (ram, video, audio, lag)"
else FAIL "native == sandbox at $frames frames"; fi

if "$here/bin/run-wbx" "$here/bin/core.wbx" --sys "$sys" --frames 60 --rewind "$swiss" 2>/dev/null | grep -q "EQUAL"; then
	PASS "rewind leg - load of a mid-run state replays identically"
else FAIL "rewind leg"; fi

wbx --frames 60 --report 1 --rerecord "$swiss" > "$work/rr.txt"
wbx --frames 60 --report 1 "$swiss" > "$work/pl.txt"
if cmp -s "$work/rr.txt" "$work/pl.txt"; then PASS "rerecord leg - save+load around every frame changes nothing"
else FAIL "rerecord leg"; fi

nat np --frames 80 --report 1 --press 20:30:7 "$swiss" > "$work/np.txt"
head -80 "$work/n1.txt" > "$work/n80.txt"
if cmp -s "$work/np.txt" "$work/n80.txt"; then FAIL "input leg - a press left the machine unchanged"
else
	wbx --frames 80 --report 1 --press 20:30:7 "$swiss" > "$work/gp.txt"
	if cmp -s "$work/np.txt" "$work/gp.txt"; then PASS "input leg - the press reached the machine, native == sandbox"
	else FAIL "input leg - press runs differ between flavors"; fi
fi

if grep -q "lag 0$" "$work/n1.txt"; then PASS "lag leg - the machine polls the pad every frame"
else FAIL "lag leg - unpolled frames counted"; fi

# ---- tier 2: a commercial disc --------------------------------------------
if [ -f "$disc" ]; then
	nat d1 --frames "$frames" --report 1 "$disc" > "$work/d1.txt"
	wbx --frames "$frames" --report 1 "$disc" > "$work/dg.txt"
	if cmp -s "$work/d1.txt" "$work/dg.txt"; then PASS "disc leg - native == sandbox at $frames frames"
	else FAIL "disc leg"; fi
else
	SKIP "disc leg (no commercial disc in tests/roms-local) - would prove DiscIO+DVD timing equivalence on a real game"
fi

say ""
say "$pass ok, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
