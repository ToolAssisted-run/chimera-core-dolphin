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

# bridge_answered FILE...: did the GPU bridge have a case for every opcode the
# guest sent it? gl-host.c's default arm logs and returns 0, and 0 is a
# perfectly plausible answer to nearly every question the bridge carries - so a
# guest that was answered and a guest that was shrugged at look the same, and
# two flavours that were both shrugged at compare EQUAL.
#
# That is not a worry, it is a measurement: GL_OP_CONTEXT_ID (chimera issue
# #43, the opcode that lets the renderer notice its GL objects belong to a
# context that is gone) had no case in gl-host.c for as long as the opcode
# existed, and this gate was green over it. Absent was indistinguishable from
# working (~/chimera/docs/gates.md, mode C). So no gpu leg may go green over
# that line: every one runs this first, on each flavour's stderr that went
# through gl-host.c, and the message names the opcodes.
bridge_gap=""
bridge_answered() {
	bridge_gap=""
	for f in "$@"; do
		[ -f "$f" ] || continue
		grep -q 'has no case' "$f" || continue
		bridge_gap="the GPU bridge had no case for $(grep -o 'opcode [0-9]*' "$f" | sort -u | tr '\n' ',' | sed 's/,$//; s/,/, /g') and answered 0 ($(basename "$f"))"
		return 1
	done
	return 0
}

rm -rf "$work"; mkdir -p "$work"

[ -x "$here/obj-native/run-native" ] || { say "run-native missing - make -f native.mk"; exit 2; }
[ -x "$here/bin/run-wbx" ] || { say "run-wbx missing - ./build-core.sh"; exit 2; }

nat() { d="$1"; shift; rm -rf "$work/$d"; "$here/obj-native/run-native" --sys "$sys" --user "$work/$d" "$@" 2>/dev/null | grep '^frame'; }
wbx() { "$here/bin/run-wbx" "$here/bin/core.wbx" --sys "$sys" "$@" 2>/dev/null | grep '^frame'; }

# ---- tier 1: swiss ---------------------------------------------------------
# -s before every cmp. nat() and wbx() pipe through `grep '^frame'` and drop
# stderr, so a runner that dies on its first instruction leaves an EMPTY file -
# and two empty files are byte-identical. Without the -s these two legs read
# "native deterministic" and "native == sandbox" off a core that never ran.
nat n1 --frames "$frames" --report 1 "$swiss" > "$work/n1.txt"
nat n2 --frames "$frames" --report 1 "$swiss" > "$work/n2.txt"
if [ ! -s "$work/n1.txt" ]; then FAIL "native run produced no frames at all"
elif cmp -s "$work/n1.txt" "$work/n2.txt"; then PASS "native deterministic at $frames frames"
else FAIL "native deterministic at $frames frames"; fi

wbx --frames "$frames" --report 1 "$swiss" > "$work/g1.txt"
if [ ! -s "$work/g1.txt" ]; then FAIL "the sandbox run produced no frames at all"
elif cmp -s "$work/n1.txt" "$work/g1.txt"; then PASS "native == sandbox at $frames frames (ram, video, audio, lag)"
else FAIL "native == sandbox at $frames frames"; fi

if "$here/bin/run-wbx" "$here/bin/core.wbx" --sys "$sys" --frames 60 --rewind "$swiss" 2>/dev/null | grep -q "EQUAL"; then
	PASS "rewind leg - load of a mid-run state replays identically"
else FAIL "rewind leg"; fi

wbx --frames 60 --report 1 --rerecord "$swiss" > "$work/rr.txt"
wbx --frames 60 --report 1 "$swiss" > "$work/pl.txt"
if [ -s "$work/pl.txt" ] && cmp -s "$work/rr.txt" "$work/pl.txt"; then PASS "rerecord leg - save+load around every frame changes nothing"
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
	if [ -s "$work/cc-n.txt" ] && cmp -s "$work/cc-n.txt" "$work/cc-g.txt"; then PASS "cpu core '$core' - native == sandbox at 120 frames"
	else FAIL "cpu core '$core' - flavors differ"; fi
	wbx --settings "{\"cpu_core\":\"$core\"}" --frames 60 --report 1 --rerecord "$swiss" > "$work/cc-rr.txt"
	wbx --settings "{\"cpu_core\":\"$core\"}" --frames 60 --report 1 "$swiss" > "$work/cc-pl.txt"
	if [ -s "$work/cc-pl.txt" ] && cmp -s "$work/cc-rr.txt" "$work/cc-pl.txt"; then PASS "cpu core '$core' - rerecord changes nothing"
	else FAIL "cpu core '$core' - rerecord leg"; fi
done

# ---- the ports are the machine: a second pad changes it, identically -------
nat pp --ports 1100 --frames 60 --report 1 "$swiss" > "$work/pp-n.txt"
wbx --settings '{"port2":"gc-controller"}' --frames 60 --report 1 "$swiss" > "$work/pp-g.txt"
head -60 "$work/n1.txt" > "$work/n1-60.txt"
if cmp -s "$work/pp-n.txt" "$work/pp-g.txt" && ! cmp -s "$work/pp-n.txt" "$work/n1-60.txt"; then
	PASS "ports leg - a second controller is a different machine, equal across flavors"
else FAIL "ports leg"; fi

# ---- the GPU bridge: a real driver, the same bytes both flavors ------------
# The GPU is outside the sandbox and different on every machine, so this leg
# proves equality ON THIS DRIVER only - and SKIPs, not fails, where no GL
# context exists at all. Both flavours dispatch through gl-host.c here (the
# native binary installs the same dispatcher), so both stderrs are held to
# bridge_answered before the frames are compared: two runs shrugged at
# identically compare equal, and did, 60 times a run, for as long as
# GL_OP_CONTEXT_ID had no case.
rm -rf "$work/gpu-n"
"$here/obj-native/run-native" --sys "$sys" --user "$work/gpu-n" --renderer opengl \
	--frames 60 --report 1 "$swiss" 2>"$work/gpu-n.err" | grep '^frame' > "$work/gpu-n.txt"
# A host with no GL is the only thing this leg may SKIP for, and the bridge
# says so in as many words ("gpu bridge: no context (...)", run-native.cpp).
# Keying the SKIP on an EMPTY output instead - which is what this did - made
# every other way the OGL backend can produce no frames read as "this machine
# has no GPU", which is mode C: absent wearing the costume of unsupported.
if grep -q 'gpu bridge: no context' "$work/gpu-n.err"; then
	SKIP "gpu leg ($(grep -m1 'no context' "$work/gpu-n.err" | head -c 60)) - would prove the OGL backend equal across flavors on this driver"
elif ! [ -s "$work/gpu-n.txt" ]; then
	FAIL "gpu leg - the host HAS a GL context and the OGL backend still drew no frames ($(grep -v '^\s*$' "$work/gpu-n.err" | tail -1 | head -c 80))"
else
	CHIMERA_GPU=1 "$here/bin/run-wbx" "$here/bin/core.wbx" --sys "$sys" \
		--settings '{"renderer":"opengl-hw"}' --frames 60 --report 1 "$swiss" 2>"$work/gpu-g.err" | grep '^frame' > "$work/gpu-g.txt"
	if ! bridge_answered "$work/gpu-n.err" "$work/gpu-g.err"; then FAIL "gpu leg - $bridge_gap"
	elif cmp -s "$work/gpu-n.txt" "$work/gpu-g.txt"; then PASS "gpu leg - the OGL backend drew, native == sandbox on this driver, and every opcode the guest sent had a case"
	else FAIL "gpu leg - flavors differ under the GPU"; fi
fi

# ---- tier 2: a commercial disc --------------------------------------------
if [ -f "$disc" ]; then
	nat d1 --frames "$frames" --report 1 "$disc" > "$work/d1.txt"
	wbx --frames "$frames" --report 1 "$disc" > "$work/dg.txt"
	if [ -s "$work/d1.txt" ] && cmp -s "$work/d1.txt" "$work/dg.txt"; then PASS "disc leg - native == sandbox at $frames frames"
	else FAIL "disc leg"; fi
else
	SKIP "disc leg (no commercial disc in tests/roms-local) - would prove DiscIO+DVD timing equivalence on a real game"
fi

# ---- tier 2b: a Wii disc ---------------------------------------------------
# The other machine this core is: IOS HLE, the NAND in guest memory, the
# disc decrypted with dolphin's own keys. The rerecord leg doubles as proof
# the RAM NAND is machine state an arena snapshot captures.
if [ -f "$wiidisc" ]; then
	nat w1 --frames 60 --report 1 --machine wii "$wiidisc" > "$work/w1.txt"
	wbx --frames 60 --report 1 --settings '{"machine":"wii"}' "$wiidisc" > "$work/wg.txt"
	if [ -s "$work/w1.txt" ] && cmp -s "$work/w1.txt" "$work/wg.txt"; then PASS "wii disc leg - native == sandbox at 60 frames"
	else FAIL "wii disc leg"; fi
	wbx --frames 30 --report 1 --rerecord --settings '{"machine":"wii"}' "$wiidisc" > "$work/wrr.txt"
	wbx --frames 30 --report 1 --settings '{"machine":"wii"}' "$wiidisc" > "$work/wpl.txt"
	if [ -s "$work/wpl.txt" ] && cmp -s "$work/wrr.txt" "$work/wpl.txt"; then PASS "wii rerecord leg - the RAM NAND survives arena restores"
	else FAIL "wii rerecord leg"; fi
	# the declared machine is a gate, not a guess: a Wii image in a project
	# that says GameCube is a refusal, in both flavors
	if nat wm --frames 1 --machine gamecube "$wiidisc" > "$work/wm.txt" 2>/dev/null && [ -s "$work/wm.txt" ]; then
		FAIL "machine leg - native booted a Wii image as a GameCube"
	elif wbx --frames 1 --settings '{"machine":"gamecube"}' "$wiidisc" > "$work/wmg.txt" 2>/dev/null && [ -s "$work/wmg.txt" ]; then
		FAIL "machine leg - the sandbox booted a Wii image as a GameCube"
	else
		PASS "machine leg - a Wii image in a GameCube project is a load error"
	fi
else
	SKIP "wii legs (no Wii disc in tests/roms-local) - would prove IOS HLE + the in-memory NAND across flavors"
fi

# --- the greenzone's frame-0 anchor rebuilds like any other state (#126) ----
#
# On the GPU bridge the OGL backend's objects live in the driver and a savestate
# carries only their NAMES, so the engine mints a fresh context id on every
# state load and this core rebuilds when the id it stored beside those objects
# no longer matches (chimera_dolphin_gl_frame_start, OGLGfx.cpp).
#
# One state used to slip through: the greenzone's FRAME-0 ANCHOR, taken right
# after Init and before the first frame advance. Init boots the machine to a
# pause without running a frame, so the backend is already up and holding real
# GL names while the stored id is still its initial ZERO - and zero was read as
# "nothing to rebuild". The renderer then kept whatever objects the frames after
# the anchor had left in the driver. It reaches a person because TAStudio goes
# to a frame by loading the state BEFORE it and emulating one forward, so frames
# 0 and 1 both load that anchor and frame 2 is the first that does not.
#
# What it measures: the calls that cross the bridge on the frame after the
# restore. A rebuild is over two thousand here against 664 without one, and an
# idle frame of swiss is under 200, so 1200 is a wide margin rather than a tuned
# threshold. A restore to frame 0 and a restore to frame 2 must BOTH rebuild -
# the difference between them was the bug.
#
# This is the only leg here that goes through the ENGINE rather than run-wbx,
# and it has to be: run-wbx's own --rerecord calls wbx_load_state directly, so
# it never calls StateLoaded and never mints a new context id. It is also the
# only leg that needs a built and installed package.
#
# WHAT IT DOES NOT STAND IN FOR (chimera docs/gates.md, E): swiss is a homebrew
# file manager, not a game - it draws a menu, so its texture cache never holds
# much - and llvmpipe is not a driver. What this proves is that the rebuild
# RUNS, not that a real game's picture is right on real hardware.
chimera_root=""
for c in "${CHIMERA_ROOT:-}" "$root/../chimera" "$HOME/chimera"; do
	if [ -n "$c" ] && [ -x "$c/build/meson-linux/chimera-run" ] &&
		[ -f "$c/build/Cores/dolphin.chimeraCore" ]; then
		chimera_root="$c"
		break
	fi
done
if [ -z "$chimera_root" ]; then
	SKIP "gl:rebuild-at-zero leg - needs chimera-run and an installed dolphin.chimeraCore (set CHIMERA_ROOT); would prove a greenzone restore rebuilds the GL objects, the frame-0 anchor included"
else
	gz="$work/glzero"
	mkdir -p "$gz"
	crun="$chimera_root/build/meson-linux/chimera-run"
	cpkg="$chimera_root/build/Cores/dolphin.chimeraCore"
	printf '[Input]\nLogKey:#\n' > "$gz/none.txt"
	glrun() { # <movie> <out> <extra args...>
		glmovie="$1"; glout="$2"; shift 2
		CHIMERA_GL_TRACE=1 CHIMERA_GL_STATEAUDIT=1 timeout 600 "$crun" "$cpkg" \
			"$swiss" "$glmovie" --settings '{"renderer":"opengl-hw"}' \
			--frames 60 --gpu "$@" > "$glout" 2>&1 || true
	}
	# the calls on the first traced frame after the restore
	afterRestore() {
		awk '/ce-gl-audit\] restore/ { seen = 1 }
		     seen && match($0, /\[ce-gl\] frame [0-9]+: [0-9]+ calls/) {
			s = substr($0, RSTART, RLENGTH); split(s, f, " "); print f[4]; exit }' "$1"
	}
	glrun "$gz/none.txt" "$gz/record.log" --record "$gz/movie.txt"
	if [ ! -s "$gz/movie.txt" ]; then
		FAIL "gl:rebuild-at-zero leg - could not record a movie to rewind through (see $gz/record.log)"
	else
		glrun "$gz/movie.txt" "$gz/rewind0.log" --greenzone 4096 --rewind-loop 0,1
		glrun "$gz/movie.txt" "$gz/rewind2.log" --greenzone 4096 --rewind-loop 2,1
		zero="$(afterRestore "$gz/rewind0.log")"
		two="$(afterRestore "$gz/rewind2.log")"
		if grep -q "^chimera gl: no context" "$gz/rewind0.log"; then
			SKIP "gl:rebuild-at-zero leg - this build or this machine gives the bridge no GL context"
		elif [ -z "$zero" ] || [ -z "$two" ]; then
			FAIL "gl:rebuild-at-zero leg - no restore was traced (see $gz/rewind0.log and $gz/rewind2.log)"
		elif [ "$zero" -lt 1200 ]; then
			FAIL "gl:rebuild-at-zero leg - restoring the frame-0 anchor made $zero GL calls on the next frame, against $two restoring frame 2: the backend was not rebuilt"
		elif [ "$two" -lt 1200 ]; then
			FAIL "gl:rebuild-at-zero leg - restoring frame 2 made only $two GL calls on the next frame: the backend was not rebuilt"
		else
			PASS "gl:rebuild-at-zero leg - a restore rebuilds the GL objects wherever it lands - $zero calls after frame 0, $two after frame 2"
		fi
	fi
fi

say ""
say "$pass ok, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
