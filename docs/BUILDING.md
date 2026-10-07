# Building the Dolphin core

This repository builds Dolphin (the GameCube, the Wii and the Triforce arcade
board) as a sandboxed guest for Chimera. The result is one file,
`dolphin.chimeraCore`, which Chimera loads. The steps below are the ones
`.github/workflows/chimera.yml` runs on a fresh clone on a public Ubuntu
runner. Cores are built on Linux; the same package file runs on Linux and on
Windows, because the guest inside it is run by Chimera's sandbox (miniBox) on
either.

Names used below:

- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<minibox>` - `<chimera>/extern/chimera-common-minibox`, the miniBox
  submodule: the sandbox host and the guest toolchain.

The commands use two shell variables for them. Both must be absolute paths
(CI passes `$PWD/chimera-checkout/...`). Commands run from the root of this
repository unless they say otherwise.

```sh
chimera=/absolute/path/to/chimera
mb="$chimera/extern/chimera-common-minibox"
```

## Requirements

**Operating system.** CI builds on GitHub's `ubuntu-latest` runner, x86-64.

**Packages for the core and the core gate** (workflow job `core-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake python3 pkg-config \
  libegl-dev libegl1 libgl1-mesa-dri libegl-mesa0
```

**Packages for the frontend gate** (workflow job `frontend-gate`, which also
builds Chimera itself):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 \
  mono-complete xvfb \
  libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev \
  libegl1 libgl1-mesa-dri libegl-mesa0
```

**Toolchains.**

- C and C++: the gcc and g++ that `build-essential` installs, and CMake. The
  workflow pins no compiler or CMake version.
- EGL. The gate's two runners, `run-native` and `run-wbx`, link `-lEGL`: they
  carry the host half of the GPU bridge (`waterbox/gl-host.c`). The Mesa
  packages above give them a GL context on a machine with no GPU; the
  workflow's GPU leg draws on Mesa's llvmpipe.
- .NET SDK 8.0, for the frontend gate only. CI uses `actions/setup-dotnet@v4`
  with `dotnet-version: '8.0'`. Chimera's README gives the manual equivalent:
  `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`.
  It also says distro-built SDKs omit the WindowsDesktop targets the frontend
  needs.
- Mono and Xvfb (`mono-complete`, `xvfb`), for the frontend gate only.
- No Rust.

**What the scripts fetch or build themselves.**

- The scripts download nothing. Everything is source from git: this
  repository and the `extern/dolphin` submodule with its own submodules.
- Dolphin is configured with `-DUSE_SYSTEM_LIBS=OFF`
  (`waterbox/configure-flags.sh`), so its libraries are built from Dolphin's
  vendored `Externals`, not taken from the system. That is why the checkout
  is recursive.
- No guest Mesa is fetched or built. The `software` renderer is Dolphin's own
  software rasteriser. The `opengl-hw` renderer is Dolphin's OpenGL backend
  drawing on the host's GPU through Chimera's GPU bridge.
- The GL loader (glad) is vendored in `waterbox/glad/`. The bridge's guest
  wrappers are generated at build time into `waterbox/generated-gl/` by
  miniBox's `source/gl/gen-gl-bridge.py`, for the entry points listed in
  `waterbox/gl-entry-points.txt`.
- miniBox builds the guest C and C++ toolchain from the Chimera submodule.

## Get the sources

CI checks this repository out with `actions/checkout@v6` and
`submodules: recursive`. By hand:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-dolphin.git
cd chimera-core-dolphin
git submodule update --init --recursive
```

Chimera, with the miniBox submodule. CI checks out Chimera's `main` branch
(`CHIMERA_REF: main`) into `chimera-checkout/` inside this repository's
workspace:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git "$chimera"
git -C "$chimera" submodule update --init extern/chimera-common-minibox
```

That is enough to build the core, the package and the core gate. The frontend
gate builds Chimera too, and for that CI checks Chimera out with
`submodules: recursive`:

```sh
git -C "$chimera" submodule update --init --recursive
```

Where the scripts look when they are not told:

| Script | Option | Otherwise |
| --- | --- | --- |
| `waterbox/guest-toolchain.cmake` | `$MINIBOX_SYSROOT` (the sysroot itself) | `$MINIBOX_DIR/build/meson-cpp/guest-sysroot`, else under `$HOME/chimera/extern/chimera-common-minibox` |
| `waterbox/native.mk`, `waterbox/guest.mk` | make variable `MB=<minibox>` | `$HOME/chimera/extern/chimera-common-minibox` |
| `waterbox/build-core.sh` | `-m <minibox>` | `$MINIBOX_DIR`, else `$HOME/chimera/extern/chimera-common-minibox` |
| `waterbox/build-package.sh` | `-r <chimera>`, `-m <minibox>` | Chimera: `../chimera`, else `$HOME/chimera`. miniBox: `$MINIBOX_DIR`, else `<chimera>/extern/chimera-common-minibox` |
| `waterbox/tests/run-frontend.sh` | `--chimera-root <chimera>` | `../chimera`, else `$HOME/chimera` |

Pass the paths, as CI does.

## Build miniBox

The host library, and the C++ guest toolchain:

```sh
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

`build/meson-cpp/guest-sysroot` is the sysroot the guest build requires. CI
keeps both build directories in an `actions/cache@v4` cache; on your machine
they simply stay.

## Build the core

Both flavors are built by Dolphin's own CMake with one option set,
`DOLPHIN_OPTS` in `waterbox/configure-flags.sh`. They differ only in
toolchain. The workflow exports the guest sysroot before both:

```sh
export MINIBOX_SYSROOT="$mb/build/meson-cpp/guest-sysroot"
```

Only `waterbox/guest-toolchain.cmake` reads it.

### Patches

`extern/dolphin` is pinned to pristine upstream. The 24 numbered patches in
`patches/` are applied to it by `waterbox/apply-patches.sh`. There is no
separate step: `build-native.sh` and `build-guest.sh` both run it first.

The script judges the series as a whole:

- It first applies every patch, in order, to a scratch copy of the touched
  files as the submodule's HEAD has them. If that fails it stops and names the
  patch: the submodule was moved without rebasing the patches.
- If the working tree is pristine it applies the series and prints
  `applied: <patch>` for each.
- If the working tree is exactly what the series leaves behind it prints
  `already applied: all 24 patches` and changes nothing.
- Anything in between is an error that names the files and prints the reset
  command.

### The native reference

Dolphin built for the host, with a small runner. It is what the core gate
compares the sandboxed core against.

```sh
./waterbox/build-native.sh
make -C waterbox -f native.mk -j"$(nproc)" MB="$mb"
```

- `build-native.sh` applies the patches, runs
  `cmake -B build/native $DOLPHIN_OPTS extern/dolphin` and then
  `make -C build/native`. Extra arguments go to that `make`.
- `native.mk` builds `waterbox/obj-native/run-native`. It compiles the adapter
  with exactly the flags CMake used (`waterbox/extract-tu-flags.py` reads them
  from `build/native/compile_commands.json`) and links every archive under
  `build/native`.

### The guest core

```sh
./waterbox/build-guest.sh
MINIBOX_DIR="$mb" ./waterbox/build-core.sh -m "$mb"
```

- `build-guest.sh` applies the patches, runs the same CMake configure with
  `-DCMAKE_TOOLCHAIN_FILE=waterbox/guest-toolchain.cmake` into `build/guest`,
  and then `make -C build/guest`. Extra arguments go to that `make`.
- `build-core.sh` takes `-m <miniBox dir>`, `-o <output dir>` (default
  `waterbox/bin`) and `-j N`. It generates the bridge sources, builds the
  adapter objects (`make -f guest.mk`, into `waterbox/obj-guest/`), links
  `core.wbx` from them and every archive under `build/guest`, and runs
  miniBox's `source/guest/check-wbx.sh` on it. It then builds `run-wbx`, the
  host driver the gates use, beside it.

The results are `waterbox/bin/core.wbx` and `waterbox/bin/run-wbx`. The gates
look for them there.

CI keeps `build/native` and `build/guest` in an `actions/cache@v4` cache keyed
on `.gitmodules`, `patches/**`, `waterbox/configure-flags.sh` and
`waterbox/guest-toolchain.cmake`.

## Build the package

```sh
MINIBOX_SYSROOT="$mb/build/meson-cpp/guest-sysroot" \
  ./waterbox/build-package.sh -m "$mb" -r "$chimera"
```

Options: `-m <miniBox dir>` and `-r <chimera root>`. There is no output
directory option.

What it does, in order:

1. Runs `build-guest.sh` and then `build-core.sh`, every time. It never
   reuses `build/guest` unchecked: the patches are re-applied and the make is
   incremental, so a stale guest cannot be packaged. Any failure stops the
   script before anything is copied.
2. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json` and
   `file_slots.json`.
3. Stages `assets/sys/GameSettings` and `assets/sys/GC`, copied from
   `extern/dolphin/Data/Sys`: Dolphin's per-game settings, and the GameCube
   fonts and DSP ROMs Dolphin ships. Chimera mounts them in the guest as
   `/sys/...`.
4. Adds the licence texts (`waterbox/package-licenses.json`) and a
   `build.json` that records the toolchain and the pins.
5. Stamps the version into the staged `waterbox.config`. CI passes
   `CORE_VERSION` (the commit). Without it the stamp is `<commit>+local`,
   with `-dirty` after the commit when `git diff --quiet HEAD` reports
   changes. The applied patch series counts as a change, so a hand build
   normally reads `<commit>-dirty+local`. `versionDate` is the commit's date
   in UTC.
6. Writes `<chimera>/build/Cores/dolphin.chimeraCore`, replacing the one
   there. The zip is written twice and the two SHA-1s must match; it prints
   `package sha1 ...` and `packaged -> ...`.
7. Removes `<chimera>/build/CoreCache/dolphin-*`.

The script does not need the native reference. From a built miniBox it is the
whole path to a package, which is what the workflow's `frontend-gate` job
does.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
is a file a person puts in Chimera's `Cores` folder.

- **A Chimera source checkout.** The cores folder is `<chimera>/build/Cores/`,
  and `build-package.sh -r <chimera>` has already written the package there.
- **A release bundle.** Copy `dolphin.chimeraCore` into the `Cores` folder
  beside `Chimera.exe`, or into the folder chosen with Change folder... in
  File > Core Manager.
- **Without building.** Download the package from this repository's Releases
  page, https://github.com/ToolAssisted-run/chimera-core-dolphin/releases, and
  put it in the same folder. CI publishes a rolling `dev` release on every
  green push to `main` and a dated `nightly-YYYY-MM-DD` release from the
  scheduled run, when `main` moved since the last one.

File > Core Manager lists what is in the folder; Refresh List rescans it.

A package's version is the commit it was built from. A hand-built package
carries `+local` and is for testing; Chimera's publishing script refuses to
publish one. A published package is named `dolphin-<version>.chimeraCore` and
the one built here `dolphin.chimeraCore`; Chimera identifies a package by its
content, not by its file name.

## Run the gates

### The core gate

```sh
./waterbox/run-gate.sh
```

One optional argument: the frame count of the main legs (default 120). It
needs `waterbox/obj-native/run-native`, `waterbox/bin/run-wbx` and
`waterbox/bin/core.wbx`. It empties `waterbox/tests/work/` when it starts,
prints one `PASS:`, `FAIL:` or `SKIP:` line per leg, and ends with
`N ok, N failed, N skipped`; it fails when any leg failed.

A GameCube boots a homebrew executable with no firmware, and Swiss (GPL) is
committed as `tests/roms/swiss_r2092.dol`. So tier 1 runs from a fresh clone:

| Legs | What they hold |
| --- | --- |
| native deterministic | two native runs agree |
| native == sandbox | RAM, video, audio and lag digests are identical in the sandbox |
| rewind, rerecord | a state loaded mid-run replays identically; save and load around every frame changes nothing |
| input, lag | a press reaches the machine, in both flavors; the pad is polled every frame |
| cpu core `interpreter`, `cached-interpreter` | each is flavor-equal and survives rerecording (the default, `jit`, is what the other legs run) |
| ports | a second controller is a different machine, equal across flavors |
| gpu | the OpenGL backend through the GPU bridge: native == sandbox on this driver, and every opcode the guest sent was answered |
| options:config, options:names, options:crisp | the nine emulation options reach Dolphin's config, the sandbox reads every one, and Internal Resolution above 1x hands out the larger picture |

The `gpu` and `options:crisp` legs are skipped only on a host with no GL
context (`gpu bridge: no context`). CI has one through Mesa.

Skipped without content or without Chimera:

| Legs | Needs |
| --- | --- |
| disc | a GameCube disc image at the path the `disc=` line at the top of `run-gate.sh` names, under `tests/roms-local/` |
| wii disc, wii rerecord, machine, widescreen, wii save refusal, wii save round-trip | a Wii disc image: `DOLPHIN_WII_DISC`, or the path the `wiidisc=` line names |
| triforce, triforce rerecord, triforce coin, triforce machine | `DOLPHIN_TRIFORCE`, a Triforce image |
| fzero still-screen, fzero trigger | `DOLPHIN_FZERO`, a Triforce image whose game sits on a still screen and reads the analog triggers |
| wad | `DOLPHIN_WAD`, a Wii channel `.wad` |
| gl:rebuild-at-zero | `chimera-run` and the installed package: `<chimera>/build/meson-linux/chimera-run` and `<chimera>/build/Cores/dolphin.chimeraCore` |

`gl:rebuild-at-zero` looks for Chimera in `$CHIMERA_ROOT`, then `../chimera`,
then `$HOME/chimera`. CI's `core-gate` job builds neither Chimera nor the
package, so it is skipped there. It is also skipped when the machine gives
the bridge no GL context.

### The frontend gate

It runs the package inside Chimera itself, headless under Mono. Build Chimera
first, as the workflow does:

```sh
cd "$chimera"
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from this repository, with the package installed:

```sh
./waterbox/tests/run-frontend.sh --chimera-root "$chimera"
```

Options: `--chimera-root <path>` and `--frames N` (default 200). It needs
`waterbox/bin/run-wbx` and `waterbox/bin/core.wbx`, which `build-package.sh`
has built. Its reference is `run-wbx`, not the native build: the core gate
already holds those two equal. When `DISPLAY` is not set it starts its own
Xvfb. Logs and dumps go to `waterbox/tests/work/`.

| Check | What it holds | Skipped without |
| --- | --- | --- |
| `game:frontend` | Swiss through Chimera: System RAM equals the sandbox reference | - |
| `keybinds` | the package's key bindings become the frontend's defaults | - |
| `disc:frontend` | the same over a disc | the GameCube disc image in `tests/roms-local/` |
| `settings:memcard` | a machine-shaping setting reaches the guest through the frontend | the same disc image |

## Files the core needs at run time

Game files are never in this repository or in the package. The user provides
them. `waterbox/file_slots.json` and `waterbox/waterbox.config` are the
declarations; this is what they say.

- **Game**: exactly one file. A disc image (`.iso`, `.gcm`, or Dolphin's
  `.rvz`), a homebrew executable (`.dol`, `.elf`), or a Wii channel (`.wad`),
  which is installed into the console's NAND before the machine starts.
- **Machine**: the `machine` setting says which console the project is:
  `gamecube` (the default), `wii` or `triforce`. An image of another machine
  is a load error.
- **Firmware**: none. The package declares no firmware. A GameCube boots
  without a dump: the IPL is emulated in software, and the free DSP ROMs ship
  inside the package (step 3 above).
- **Save data**, optional, up to 2 files: on a GameCube the raw memory card
  image under its own name (`MemoryCardA.USA.raw`, `.EUR.` or `.JAP.`); on a
  Wii the `.zip` that Emulator > Export Save Data... writes.

The `renderer` setting has two values. `opengl-hw` (the default) draws on the
host's GPU through Chimera's GPU bridge; the GPU is outside the sandbox, and a
movie recorded this way replays only on the same driver. `software` is
Dolphin's software rasteriser, entirely inside the sandbox and byte-identical
everywhere. Without a GPU the machine falls back to `software`.

## Troubleshooting

- **`miniBox guest sysroot not found at ...`** (CMake, from
  `guest-toolchain.cmake`). Build miniBox's `meson-cpp` with
  `-Dguest_cpp=true`, and set `MINIBOX_SYSROOT` or `MINIBOX_DIR`. With
  neither set the toolchain file looks under `$HOME/chimera`.
- **`miniBox C++ guest toolchain missing at ...`** (`build-core.sh`). The
  same cause.
- **`build/guest missing - run build-guest.sh first.`** (`build-core.sh`).
  Run it.
- **`run-native missing - make -f native.mk`** or **`run-wbx missing -
  ./build-core.sh`** (`run-gate.sh`). The gate needs both runners.
- **`run-native` or `run-wbx` does not link.** Both link `-lEGL`; CI installs
  `libegl-dev` for it.
- **The gpu leg says SKIP, `no context`.** The host gave the bridge no GL
  context. CI installs `libegl1 libgl1-mesa-dri libegl-mesa0` and draws on
  Mesa's llvmpipe.
- **The gpu leg fails with `the GPU bridge had no case for opcode ...`.** The
  guest sent an opcode `waterbox/gl-host.c` does not answer. Add the case; do
  not relax the leg.
- **`extern/dolphin is partly patched`** (`apply-patches.sh`). A patched file
  was edited or reverted by hand. Turn any edits you want into a patch, then
  run the reset command the script prints.
- **`the series does not apply to the submodule's HEAD`.** The submodule pin
  was moved without rebasing the patches.
- **`extern/dolphin is not checked out`.** Run
  `git submodule update --init --recursive`.
- **The frontend gate stops with `Chimera not built`, `package not installed`
  or `run-wbx not built`.** Build Chimera, or run `build-package.sh`, which
  builds `run-wbx` too.
- **`Xvfb not found (apt install xvfb)`.** The frontend gate starts its own X
  display when `DISPLAY` is not set, and needs `xvfb` for it.
- **`docs/PLAN.md` describes a build without CMake, from a curated
  `waterbox/sources.mk`.** The scripts in this repository build with
  Dolphin's CMake, as described here. Follow the scripts.
- **`minibox-diag.log` appears.** The sandbox writes it when a guest faults.
  It is ignored by git.
