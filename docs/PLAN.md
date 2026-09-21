# Dolphin -> Chimera waterbox core: the plan

Written 2026-09-01 at the start of the effort; update as milestones land. The goal is a
working, deterministic, waterboxed Dolphin core package: GameCube first, Wii later.
Software renderer and PowerPC interpreter first, GPU bridge and JIT as later
optimisations, no unnecessary subsystems (no netplay, no achievements, no analytics,
no UI, no real audio device).

## What the survey found (2026-09-01, upstream @ a1e636d72c)

- **Scale**: the components in scope are ~400k lines (Core 204k, VideoCommon 61k,
  Common 57k, VideoBackends/Software+Null 34k total across backends, DiscIO 18k,
  AudioCommon 4k). PCSX2-sized, not bigger.
- **Language**: C++23 on paper, but upstream's own floor is GCC 12 - so it is the
  GCC-12 subset of C++23. The guest toolchain (host g++ 13.3 against the musl
  sysroot) clears it.
- **No copyrighted firmware needed for GameCube.** Dolphin HLEs the IPL, and
  `Data/Sys/GC/` ships free `dsp_rom.bin`/`dsp_coef.bin` and the IPL fonts under
  their own licences. A real IPL dump can become an optional firmware channel later.
- **Deterministic configuration exists upstream**: PowerPC interpreter (and cached
  interpreter), dual core OFF (no GPU thread), the Software video backend,
  DSP-HLE (no ROM needed), NullSoundStream + Mixer. Netplay forced Dolphin to take
  determinism seriously years ago; the knobs are real.
- **Thread model**: `Core::Init` spawns EmuThread; single-core mode keeps video and
  DSP on it. miniBox green threads (futex-backed, cooperative) carried QEMU's
  threads in xemu, so `std::thread` + condition_variable here is proven ground.
- **Homebrew boots directly**: BootParameters accepts `.dol`/`.elf` with no disc and
  no IPL - that is the gate content story (freely-licensed homebrew, or our own
  devkitPPC-built test dol, like padtest.elf on PS2).
- **Achievements** are already a compile-time option (just never define
  `USE_RETRO_ACHIEVEMENTS`). **NetPlay is not**: `NetPlayClient.cpp` (2.8k lines,
  wants enet) is referenced from Core.cpp/Movie.cpp. M0 decides between a stub TU
  for the query surface (`NetPlay::IsNetPlayRunning` and friends) and compiling it
  against stubbed sockets. Start with the stub TU; it keeps enet out entirely.
- **DolphinTool proves headlessness**: it links discio+uicommon with a trivial
  `ToolHeadlessPlatform.cpp`, and the `Host_*` interface is ~15 small functions.
  Our driver implements Host_* directly, the way headless PPSSPP implemented its
  System_* stubs.

## Architecture decisions

- **Upstream pin**: `extern/dolphin` = dolphin-emu/dolphin @ `a1e636d72c`
  (master, 2026-08; describe: 2606-344). Unmodified; local changes live in
  `patches/` (numbered, applied by `apply-patches.sh`), each a build option or a
  weak hook rather than a deletion, per the house rule. Prefer solving problems in
  the adapter over patching.
- **Own build, no CMake**: one curated source list (`waterbox/sources.mk`), compiled
  twice - natively (the reference and debugging build) and for the guest (musl
  toolchain, `-mcmodel=large -fno-pic`). Same sources, same defines. The PPSSPP
  pattern verbatim.
- **CPU**: interpreter first (deterministic, no codegen), cached interpreter once
  the gate is green, Jit64 later still - miniBox hosts RWX pages (PCSX2's VIF
  generator and the scanline JIT already run there), so the JIT is an
  optimisation, not an architectural question.
- **GPU**: VideoBackends/Software for every equivalence gate. Rendered output that
  the game reads back (EFB copies) is machine state either way. The OGL backend
  through the glad bridge (xemu/pcsx2 pattern) is its own milestone, and softgpu
  stays the reference forever.
- **Audio**: DSP-HLE + Mixer into a buffer we drain per frame; NullSoundStream
  shape, no cubeb, no time stretcher. DSP-LLE on the free ROM is a later option if
  HLE accuracy disappoints.
- **Disc**: mounted host-side (`wbx_mount_file`, read-only, hash-bound), read lazily
  through the guest VFS - never slurped (discs are 1.4GB). Plain ISO/GCM first;
  RVZ works through the same DiscIO path if the compression externals earn their
  place. GC memory cards through the save-data channel, the PCSX2 fmemopen trick
  if GCMemcardRaw insists on a FILE.
- **Assets**: the needed slice of `Data/Sys` (GC fonts, dsp roms, GameSettings for
  the titles that need them) packed into one blob served by a File::IOFile-level
  hook or a VFS backend, like PPSSPP's memory-assets. No opendir in the box.
- **Sandbox time**: the machine advances the clock, per frame, never per read - the
  PCSX2 rule, non-negotiable. Throttling/speed-limit code is bypassed; the frame
  boundary is the VI vertical blank (the flycast lesson: pick the machine's
  boundary, not the renderer's present).
- **Wii is deferred by design**: NAND, IOS/ES, crypto keys, Wiimote emulation - all
  real work, none of it needed to prove the core. The source list should simply
  not exclude it gratuitously, so the door stays open.

## Externals policy

In: fmt, zlib-ng, xxhash, lz4, zstd, LZO, bzip2, liblzma (DiscIO's format zoo),
picojson, pugixml (if GameSettings/SYSCONF paths demand it), mbedtls (DiscIO
hashes; Wii later), ed25519 (Wii, deferred), FatFs (Wii NAND, deferred).
Out: Qt, SDL, curl, enet, cubeb, OpenAL, discord-rpc, miniupnpc, imgui, implot,
glslang, spirv_cross, Vulkan*, MoltenVK, libusb, hidapi, sfml, cpp-ipc, mGBA,
rcheevos, watcher, gettext, Bochs_disasm (debugger only).
Each removal must be an exclusion in the source list or a config default, not a
patch, wherever upstream allows.

## Milestones

- **M0 native reference**: curated source list + `native.mk`; `run-native` boots a
  homebrew .dol, software renderer, DSP-HLE; deterministic video/audio/RAM hashes
  across two runs. Decides the NetPlay stub and the Sys-assets packing.
- **M1 guest**: same list under the musl toolchain -> `core.wbx`; native == sandbox
  byte-for-byte over N frames. The syscall stub inventory happens here.
- **M2 savestates**: save+load around every frame changes nothing (miniBox arena
  snapshot; every byte of mutable state must live in guest memory - no host-side
  caches with machine state in them).
- **M3 input + audio legs**: GC pad wired through the SI poll, lag = a frame nobody
  polled, scripted `--press` reaches the machine, audio leg native == sandbox.
- **M4 a real disc**: ISO mount, a commercial GC game (user-supplied, local only),
  memory card through the save-data channel, gate legs SKIP-with-reason when
  content is absent (the PCSX2 tiered gate).
- **M5 GPU bridge**: the OGL backend through glad + the miniBox master GL list,
  gl-host on the frontend side, native == sandbox on the same driver.
- **M6 package + frontend**: `dolphin.chimeraCore`, waterbox.config (buttons in
  packed-bit order, axes in SetAxis order), default_keybinds.json, file_slots,
  firmware channel (optional real IPL), licences manifest, frontend gate 3/3.
- **M7+ (explicitly deferred)**: Wii, Jit64/cached-interpreter speed work, DSP-LLE,
  dual core, RVZ/other disc formats as content appears.

## Risks, ranked

1. **MemArena/fastmem**: Dolphin reserves address space and maps guest RAM through
   shared memory views. The box has no shm; the non-fastmem MMU path exists (other
   platforms use it) but the memory map setup itself may need the PCSX2 patch-5
   treatment ("memory without shared memory").
2. **Hidden host state**: texture/vertex caches, DSP HLE ucode objects, the frame
   dumping path - anything cached outside guest memory that survives a savestate
   load breaks M2. The PPSSPP lesson says trust the arena, not DoState.
3. **NetPlay entanglement** beyond the query surface (Movie.cpp reaches into it).
4. **Timing code** with wall-clock reads sprinkled outside the throttle (Common::Timer
   users need an audit; a sandbox clock advanced per frame answers all of them).
5. **The Sys directory's breadth** - GameSettings inis change machine behavior per
   title; the blob must carry them or determinism differs from stock Dolphin.

## The frame-0 anchor was the one state that did not rebuild (2026-09-21)

Chimera issue #126, reported and fixed on PCSX2 first: a bridged core stores the
host's GL context id beside its GL objects and rebuilds when the stored id no
longer matches the live one, and the guard `if (stored != 0) rebuild` skips
exactly one state. This core had the same code and the same hole.

`chimera_dolphin_gl_frame_start` runs at the top of a frame advance, and
`s_saved_context` is a function-local static that starts at 0 and is first
written there. But `Init` boots the machine to a PAUSE without running a frame
(`bBootToPause`), and the video backend is up before `WaitForState(Paused)` can
return - so at the moment the engine takes the greenzone's **frame-0 anchor**,
right after Init, the OGL backend already holds real GL names and the stored id
is still 0. Loading that anchor read the 0 as "this machine has never held any
GL objects", and the backend went on using whatever the frames after the anchor
had left in the driver.

It reaches a person because TAStudio goes to a frame by loading the state BEFORE
it and emulating one forward, so frames 0 and 1 both load the anchor and frame 2
is the first that does not - which is exactly the boundary the reporter of #126
described (corrupt from frame 0 or 1, clean from frame 2).

The fix keeps the host's word instead of inferring it from the number: the
engine already tells every core when the machine's memory has been replaced, and
this core now implements that export. `StateLoaded` (wbx-entry.cpp) calls
`chimera_dolphin_state_loaded` (dolphin-driver.cpp), which sets the flag in
OGLGfx.cpp, and the guard became
`if (live != s_saved_context && (s_saved_context != 0 || after_load))`. The flag
is set AFTER the load, so the load cannot wipe it; a fresh boot has had no load
and still does not rebuild. Recording the id during Init instead would put it in
the SEALED baseline, where no state carries it as a delta, and the cross-session
rebuild would stop happening; a non-zero "never seen" sentinel fails
identically, because the anchor carries whatever the static's initial value is.

**Measured, not reasoned.** `chimera-run --gpu --greenzone 4096 --rewind-loop
N,1` on `swiss_r2092.dol` under `CHIMERA_GL_TRACE=1 CHIMERA_GL_STATEAUDIT=1`,
counting the calls that cross the bridge on the frame after the restore (an idle
frame of swiss is 167 to 200):

| restore to | before | after |
|---|---|---|
| frame 0 (the anchor) | **664 - no rebuild** | 2222 |
| frame 2 (an ordinary state) | 1624 | 1624 |

The rebuild also announces itself in the log, twice, as
`Failed to create shared context for shader compiling` - the shader cache coming
up again - and those two lines are present after the restore in every column
above except the first. `WARN_LOG_FMT(VIDEO, ...)`'s own "rebuilding" line does
not reach stderr in this build, which is why the leg counts calls rather than
grepping for it.

The leg is `gl:rebuild-at-zero` in `waterbox/run-gate.sh`, and it is the first
leg there that goes through the ENGINE rather than `run-wbx`. It had to be:
`run-wbx --rerecord` calls `wbx_load_state` directly, so it never calls
`StateLoaded` and never mints a new context id, and the native reference answers
context id 0. Neither could ever have witnessed this - which is why a gate with
a rewind leg, a rerecord leg and a GPU leg was green over it for as long as it
existed. NEGATIVE CONTROL: run against the package built before this change the
leg FAILS by name - "restoring the frame-0 anchor made 664 GL calls on the next
frame, against 1624 restoring frame 2: the backend was not rebuilt" - and passes
after. Whole gate: 13 ok, 0 failed, 2 skipped.

### A second hazard, closed on the way

`chimera_dolphin_frame` called the GL hook whenever the BRIDGE was present, not
when the OGL backend was actually chosen - and a project may ask for the
software renderer with a GPU bridge live, in which case `g_gfx` is an `SWGfx`
and `OGL::GetOGLGfx()`'s unchecked `static_cast` is a lie. Any state load in
that configuration therefore called `ChimeraRebuildGLObjects` on an SWGfx.
Nothing crashed here when it was tried (`--settings '{"renderer":"software"}'
--gpu --rewind-loop 0,1` survives both before and after), so this is a hazard
closed rather than a crash fixed, but it was reachable today and the `||
after_load` above would only have made it more so. The driver now gates on the
backend it actually selected (`s_gl_backend`), which is also what the comment in
OGLGfx.cpp had claimed all along.

### What the leg does not stand in for

chimera `docs/gates.md`, mode E. `swiss_r2092.dol` is a homebrew file manager
drawing a menu, not a game: its texture cache never holds much, and it never
reads its own rendered pixels back. llvmpipe is not a driver. What the leg
establishes is that the rebuild RUNS after every restore, not that a real game's
picture is right on real hardware - no wrong picture was reproduced here on any
core. And it is the only leg that needs an installed package, so it SKIPs when
there is none, naming what it would have proven.
