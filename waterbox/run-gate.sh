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
wiidisc="$root/tests/roms-local/Dragon Ball Z - Budokai Tenkaichi 3 (USA) (Rev 1).iso"
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

# ---- the other cpu cores: each deterministic and flavor-equal --------------
# The default (jit) is what every leg above ran; each alternative is a
# DIFFERENT machine (instruction- vs block-granular timing), so each is
# proven on its own: native == sandbox, and a save+load round trip per
# frame changes nothing.
for core in interpreter cached-interpreter; do
	nat "cc-$core" --cpu-core "$core" --frames 120 --report 1 "$swiss" > "$work/cc-n.txt"
	wbx --settings "{\"cpu_core\":\"$core\"}" --frames 120 --report 1 "$swiss" > "$work/cc-g.txt"
	if cmp -s "$work/cc-n.txt" "$work/cc-g.txt"; then PASS "cpu core '$core' - native == sandbox at 120 frames"
	else FAIL "cpu core '$core' - flavors differ"; fi
	wbx --settings "{\"cpu_core\":\"$core\"}" --frames 60 --report 1 --rerecord "$swiss" > "$work/cc-rr.txt"
	wbx --settings "{\"cpu_core\":\"$core\"}" --frames 60 --report 1 "$swiss" > "$work/cc-pl.txt"
	if [ -s "$work/cc-pl.txt" ] && cmp -s "$work/cc-rr.txt" "$work/cc-pl.txt"; then PASS "cpu core '$core' - rerecord changes nothing"
	else FAIL "cpu core '$core' - rerecord leg"; fi
done

# ---- the GPU bridge: a real driver, the same bytes both flavors ------------
# The GPU is outside the sandbox and different on every machine, so this leg
# proves equality ON THIS DRIVER only - and SKIPs, not fails, where no GL
# context exists at all.
rm -rf "$work/gpu-n"
"$here/obj-native/run-native" --sys "$sys" --user "$work/gpu-n" --renderer opengl \
	--frames 60 --report 1 "$swiss" 2>"$work/gpu-n.err" | grep '^frame' > "$work/gpu-n.txt"
if ! [ -s "$work/gpu-n.txt" ]; then
	SKIP "gpu leg (no GL context: $(grep -m1 'no context' "$work/gpu-n.err" | head -c 60)) - would prove the OGL backend equal across flavors on this driver"
else
	CHIMERA_GPU=1 "$here/bin/run-wbx" "$here/bin/core.wbx" --sys "$sys" \
		--settings '{"renderer":"opengl"}' --frames 60 --report 1 "$swiss" 2>/dev/null | grep '^frame' > "$work/gpu-g.txt"
	if cmp -s "$work/gpu-n.txt" "$work/gpu-g.txt"; then PASS "gpu leg - the OGL backend drew, native == sandbox on this driver"
	else FAIL "gpu leg - flavors differ under the GPU"; fi
fi

# ---- tier 2: a commercial disc --------------------------------------------
if [ -f "$disc" ]; then
	nat d1 --frames "$frames" --report 1 "$disc" > "$work/d1.txt"
	wbx --frames "$frames" --report 1 "$disc" > "$work/dg.txt"
	if cmp -s "$work/d1.txt" "$work/dg.txt"; then PASS "disc leg - native == sandbox at $frames frames"
	else FAIL "disc leg"; fi
else
	SKIP "disc leg (no commercial disc in tests/roms-local) - would prove DiscIO+DVD timing equivalence on a real game"
fi

# ---- tier 2b: a Wii disc ---------------------------------------------------
# The other machine this core is: IOS HLE, the NAND in guest memory, the
# disc decrypted with dolphin's own keys. The rerecord leg doubles as proof
# the RAM NAND is machine state an arena snapshot captures.
if [ -f "$wiidisc" ]; then
	nat w1 --frames 60 --report 1 "$wiidisc" > "$work/w1.txt"
	wbx --frames 60 --report 1 "$wiidisc" > "$work/wg.txt"
	if cmp -s "$work/w1.txt" "$work/wg.txt"; then PASS "wii disc leg - native == sandbox at 60 frames"
	else FAIL "wii disc leg"; fi
	wbx --frames 30 --report 1 --rerecord "$wiidisc" > "$work/wrr.txt"
	wbx --frames 30 --report 1 "$wiidisc" > "$work/wpl.txt"
	if [ -s "$work/wpl.txt" ] && cmp -s "$work/wrr.txt" "$work/wpl.txt"; then PASS "wii rerecord leg - the RAM NAND survives arena restores"
	else FAIL "wii rerecord leg"; fi
else
	SKIP "wii legs (no Wii disc in tests/roms-local) - would prove IOS HLE + the in-memory NAND across flavors"
fi

say ""
say "$pass ok, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
