#!/bin/bash
# The frontend half of the gate: load the Dolphin package in Chimera (under
# Mono, on a private Xvfb display), boot swiss for a fixed number of frames
# with nothing pressed, and require the machine's memory to be byte-identical
# to the sandbox reference (which the core gate already holds equal to the
# native build). Then the same over a commercial disc, a machine-shaping
# setting, and the package's keybinds - the disc legs SKIP without content.
#
# Usage: ./run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=200
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/dolphin.chimeraCore"
runwbx="$wb/bin/run-wbx"
wbx="$wb/bin/core.wbx"
sys="$root/extern/dolphin/Data/Sys"
swiss="$root/tests/roms/swiss_r2092.dol"
disc="$root/tests/roms-local/Mortal Kombat - Deadly Alliance.iso"
[ -f "$emu_exe" ] || { echo "Chimera not built: $emu_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$runwbx" ] || { echo "run-wbx not built (../build-core.sh)" >&2; exit 1; }

ok=0
failed=0
skipped=0
report() {
	printf "%-28s %-9s %s\n" "$1" "$2" "$3"
	case "$2" in
		PASS) ok=$((ok + 1)) ;;
		SKIP) skipped=$((skipped + 1)) ;;
		*) failed=$((failed + 1)) ;;
	esac
}
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

work="$here/work"
mkdir -p "$work"

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

SLICE=1048576

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="$4" romarg="$5"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
		echo "bytes=$SLICE"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "$romarg" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# the sandbox reference (the core gate holds run-wbx == the native build)
reference_ram() {
	local tag="$1" game="$2" settings="${3:-}"
	# run-wbx carries its own libminiboxhost via rpath; the chimera
	# LD_LIBRARY_PATH exported above would shadow it
	env -u LD_LIBRARY_PATH timeout 900 "$runwbx" "$wbx" --sys "$sys" \
		--frames "$frames" --report "$frames" --ram-out "$work/ref.$tag.ram.full" \
		${settings:+--settings "$settings"} \
		"$game" > "$work/ref.$tag.log" 2>&1 || return 1
	head -c "$SLICE" "$work/ref.$tag.ram.full" > "$work/ref.$tag.ram.bin"
}

settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2" "{}"; }

# --- 1. swiss through the frontend: RAM == the sandbox reference ------------
settings_config "$work/config.base.ini" '{}'
if ! reference_ram "base" "$swiss"; then
	report "game:frontend" FAIL "reference runner error (see tests/work/ref.base.log)"
elif ! run_frontend "base" "$work/config.base.ini" "$frames" "$work/base.png" "$swiss"; then
	report "game:frontend" FAIL "no OK meta (see tests/work/base.log)"
elif ! cmp -s "$work/ref.base.ram.bin" "$work/base.ram.bin"; then
	report "game:frontend" FAIL "System RAM differs from the sandbox reference"
elif [ "$(sed -n 's/^ramsize=//p' "$work/base.meta.txt")" != "25165824" ]; then
	report "game:frontend" FAIL "System RAM is $(sed -n 's/^ramsize=//p' "$work/base.meta.txt") bytes, want 25165824"
else
	report "game:frontend" PASS "$frames frames of swiss, System RAM identical to the sandbox reference"
fi

# --- 2. a commercial disc through the frontend ------------------------------
if [ -f "$disc" ]; then
	if ! reference_ram "disc" "$disc"; then
		report "disc:frontend" FAIL "reference runner error (see tests/work/ref.disc.log)"
	elif ! run_frontend "disc" "$work/config.base.ini" "$frames" "" "$disc"; then
		report "disc:frontend" FAIL "no OK meta (see tests/work/disc.log)"
	elif ! cmp -s "$work/ref.disc.ram.bin" "$work/disc.ram.bin"; then
		report "disc:frontend" FAIL "System RAM differs from the sandbox reference"
	else
		report "disc:frontend" PASS "$frames frames of $(basename "$disc"), RAM identical"
	fi
else
	report "disc:frontend" SKIP "no commercial disc in tests/roms-local"
fi

# --- 3. a machine-shaping setting arrives through the frontend --------------
# No card in slot A: games notice on their first screen, so the machine's
# memory must (a) match a reference run with the same setting and (b) differ
# from the with-card machine - otherwise the knob turned nothing.
if [ -f "$disc" ]; then
	settings_config "$work/config.nocard.ini" '{"memcard_a": false}'
	if ! reference_ram "nocard" "$disc" '{"memcard_a":false}'; then
		report "settings:memcard" FAIL "reference runner error (see tests/work/ref.nocard.log)"
	elif cmp -s "$work/ref.nocard.ram.bin" "$work/ref.disc.ram.bin"; then
		report "settings:memcard" FAIL "the setting changes nothing in $frames frames"
	elif ! run_frontend "nocard" "$work/config.nocard.ini" "$frames" "" "$disc"; then
		report "settings:memcard" FAIL "no OK meta (see tests/work/nocard.log)"
	elif ! cmp -s "$work/ref.nocard.ram.bin" "$work/nocard.ram.bin"; then
		report "settings:memcard" FAIL "RAM differs from the no-card reference"
	else
		report "settings:memcard" PASS "no-card machine equals its reference and differs from the with-card one"
	fi
else
	report "settings:memcard" SKIP "no commercial disc in tests/roms-local"
fi

# --- 4. the package's bindings became the frontend's defaults ---------------
if out="$(python3 "$here/check-keybinds.py" "$work/config.base.ini" "$wb/default_keybinds.json" "GameCube Controller" 2>&1)"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
