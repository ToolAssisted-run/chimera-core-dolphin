# AGENTS.md - Dolphin core for Chimera

This repository builds Dolphin (GameCube, Wii, Triforce) as a sandboxed guest
for Chimera, a frontend for tool-assisted speedruns. It produces one file,
`dolphin.chimeraCore`, which Chimera loads from its `Cores` folder. Dolphin is
also built natively as a reference, and the gate holds the two byte-identical.
`.github/workflows/chimera.yml` is the authoritative build recipe;
`docs/BUILDING.md` explains it step by step.

## Layout

- `extern/dolphin` - upstream Dolphin, a pinned submodule with its own submodules. Never edited in place.
- `patches/` - 24 numbered patches applied to `extern/dolphin`.
- `waterbox/configure-flags.sh` - the one CMake option set both flavors use (`DOLPHIN_OPTS`); sourced, not run. `guest-toolchain.cmake` - the miniBox guest as a CMake toolchain.
- `waterbox/build-native.sh`, `native.mk` - the native reference: `build/native`, then `waterbox/obj-native/run-native`.
- `waterbox/build-guest.sh`, `guest.mk`, `build-core.sh` - the guest: `build/guest`, then `waterbox/bin/core.wbx` and `waterbox/bin/run-wbx`. `build-package.sh` runs them and writes the package.
- `waterbox/dolphin-driver.cpp`, `dolphin-driver.h` - the adapter both flavors share.
- `waterbox/wbx-entry.cpp` - the guest ABI (guest only); `run-native.cpp` is its native twin; `run-wbx.c` drives `core.wbx` from the host.
- `waterbox/gl-shim.cpp`, `gl-host.c`, `gl-entry-points.txt`, `glad/` - the GPU bridge: guest end, host end, the entry points, the vendored loader.
- `waterbox/ram-nand.cpp`, `guest-syscalls.cpp`, `host-stubs.cpp`, `zip-reader.*` - the Wii NAND in guest memory, and what a sandbox answers differently.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`, `package-licenses.json` - what the package declares to Chimera.
- `waterbox/run-gate.sh`, `waterbox/tests/run-frontend.sh` - the core gate and the frontend gate.
- `tests/roms/` - committed test content (Swiss, GPL). `tests/roms-local/` - your own discs, ignored by git.
- `docs/PLAN.md` - the design log: decisions, measurements, sharp edges.
- `build/`, `waterbox/bin/`, `waterbox/obj-native/`, `waterbox/obj-guest/`, `waterbox/generated-gl/` - build outputs. Ignored by git.

## Set up the build environment

Ubuntu, as CI uses. Set the two paths first; both must be absolute. The
scripts download nothing, and there is no guest Mesa in this core.

```sh
chimera="$HOME/chimera"                        # a Chimera checkout
mb="$chimera/extern/chimera-common-minibox"    # miniBox, a submodule of it

sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential cmake python3 pkg-config libegl-dev libegl1 libgl1-mesa-dri libegl-mesa0

git submodule update --init --recursive

[ -d "$chimera" ] || git clone https://github.com/ToolAssisted-run/chimera.git "$chimera"
git -C "$chimera" submodule update --init extern/chimera-common-minibox

[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"

export MINIBOX_SYSROOT="$mb/build/meson-cpp/guest-sysroot"
```

## Build

Shortest path to a package (what the workflow's `frontend-gate` job runs).
It runs `build-guest.sh` and `build-core.sh` itself, every time, so it never
packages a stale guest:

```sh
./waterbox/build-package.sh -m "$mb" -r "$chimera"
```

For the core gate you also need the native reference (what the `core-gate`
job runs):

```sh
./waterbox/build-native.sh
make -C waterbox -f native.mk -j"$(nproc)" MB="$mb"
./waterbox/build-guest.sh
MINIBOX_DIR="$mb" ./waterbox/build-core.sh -m "$mb"
```

- The patches are applied by `build-native.sh` and `build-guest.sh`.
- Change CMake options only in `waterbox/configure-flags.sh`, so the two
  flavors stay the same build.
- A hand-built package is stamped `<commit>+local` (`-dirty` with changes in
  the tree, which includes the applied patches). CI stamps the commit.

## Install the core into Chimera

`build-package.sh -r "$chimera"` writes
`$chimera/build/Cores/dolphin.chimeraCore`: the cores folder of a Chimera
source checkout, so nothing else is needed there. For a release bundle, copy
the file into the `Cores` folder beside `Chimera.exe` (or the folder set in
File > Core Manager > Change folder...). File > Core Manager lists the folder;
Refresh List rescans it. Chimera downloads nothing. The same file runs on
Linux and on Windows.

## Test before you commit

```sh
./waterbox/run-gate.sh                                        # the core gate
./waterbox/tests/run-frontend.sh --chimera-root "$chimera"    # the frontend gate
```

- The core gate must end `N ok, 0 failed, M skipped`. It needs `run-native`,
  `run-wbx` and `core.wbx`, and no content: tier 1 runs Swiss from
  `tests/roms/`.
- Expected SKIPs without content: the disc leg, the Wii legs
  (`DOLPHIN_WII_DISC`), the Triforce legs (`DOLPHIN_TRIFORCE`,
  `DOLPHIN_FZERO`) and the wad leg (`DOLPHIN_WAD`). The gpu legs SKIP only
  on a host with no GL context; the Mesa packages above provide one.
- `gl:rebuild-at-zero` runs only when `$CHIMERA_ROOT` (or `../chimera`,
  `$HOME/chimera`) has `build/meson-linux/chimera-run` and the installed
  package. CI skips it. Run it when you touch the OpenGL path.
- The frontend gate needs Chimera built (natives and the .NET solution), the
  package installed, Mono and Xvfb; see `docs/BUILDING.md`. Run it when you
  touch `waterbox.config`, `default_keybinds.json` or `file_slots.json`.
- The core gate empties `waterbox/tests/work/`, which the frontend gate also
  uses. Do not run the two at once.
- A leg that was skipped has proven nothing about your change.

## Rules of this repository

- **Upstream is patched, not edited.** `extern/dolphin` stays at its pin.
  Changes to it are numbered patches in `patches/`
  (`NNNN-chimera-<what it does>.patch`, `git diff` format, paths relative to
  the submodule), each a build option or a weak hook, never a deletion.
  `waterbox/apply-patches.sh` applies the whole series or none. `git status`
  shows `extern/dolphin` as modified once it is applied; that is expected.
  Never commit inside the submodule. After adding or changing a patch,
  `waterbox/apply-patches.sh` must print `already applied: all N patches`.
  Prefer solving a problem in the adapter over patching (`docs/PLAN.md`).
- **Determinism is the product.** The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate
  must round-trip. The gate checks both; a change that breaks either is a
  bug. The `opengl-hw` renderer is the declared exception
  (`waterbox.config`): the host's GPU draws, and a movie recorded on it
  replays only on the same driver.
- **Run the gate before committing.** A new check needs a negative control:
  show that it fails when the thing it checks is broken.
- **Never commit game files, BIOS or firmware.** Discs and rom sets go in
  `tests/roms-local/`, which is ignored. `tests/roms/` holds only
  redistributable content, with its terms in `tests/roms/README.md`.
- **Never add network access** to the core. A sandbox has no sockets.
- **Scripts stay executable.** Every script that is run is git mode 100755.
  `waterbox/configure-flags.sh` is sourced and is 100644.
- **Documentation prose is plain ASCII.**
- **Commit messages** follow the log: `type(scope): a sentence that says what
  is now true`, for example
  `fix(patches): the series is judged as a whole, not a patch at a time`. The
  body is prose: the cause, the fix and what was measured. Issues live in the
  chimera repository and are cited as `chimera#N`. Assisted commits end with
  a `Co-Authored-By:` trailer.
- **Do not edit `.github/workflows`** unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every build step, option and gate leg.
- `docs/PLAN.md` - why each decision was made. Its "Own build, no CMake"
  paragraph does not match the scripts; the scripts are what is built.
- `.github/workflows/chimera.yml` - the recipe CI runs.
- `tests/roms/README.md`, `LICENSE`, `waterbox/package-licenses.json` - terms of the test content and the package.
- In the Chimera checkout: `docs/porting-a-core.md` (how a core is put together, patch traps), `docs/gates.md` (how a green gate can be wrong), `docs/core-manager.md` (packages, versions, the cores folder).
