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

## The gate's own GL host had no case for the context id (2026-09-21)

Found on rpcs3 (its b1b88fe) and checked here the same morning, with the same
result. `waterbox/gl-host.c` is the host half of the GPU bridge that THIS
repository's harness hands a guest - `run-wbx` under `CHIMERA_GPU=1`, and, in
this core alone, `run-native --renderer opengl` as well, since the single
binary installs the same dispatcher. It had no case for `GL_OP_CONTEXT_ID`,
the opcode that exists so `chimera_dolphin_gl_frame_start` can tell that the
GL names it holds belong to a context that is gone (chimera issue #43), for as
long as the opcode has existed. The default arm printed `opcode 4 has no case`
and returned 0, and 0 is the contract's "cannot tell": the backend concluded
nothing had moved and kept the names. Chimera's real host (gl_bridge.cpp)
answers the opcode, so the frontend was never affected; the harness that exists
to stand in for it was.

**Measured before anything was changed.** The gate's gpu leg, 60 frames of
swiss on llvmpipe: the line printed 60 times per run - once per frame, at the
top of every advance - in EACH flavour, and the leg PASSED, because two runs
shrugged at identically produce identical frame lines. With the case present
the frame lines of both flavours are byte-identical to the runs without it:
nothing was lost from the command stream, only the answer to the one question
that makes a restore safe. Absent was indistinguishable from working (chimera
docs/gates.md, mode C).

**The fix is rpcs3's, all three parts.** The case answers an id minted the way
the engine mints it (pid and a high-resolution counter carry the per-process
entropy; `time()` alone would hand two runs in the same second the SAME id);
the host mints again on every state load through `chimera_gl_host_state_loaded`,
which run-wbx calls at both of its load sites, because chimera's host does
(`ce_gl_state_loaded`; this core declares no `video.rebuildOnStateLoad`, so it
rebuilds) and a load in the runner was otherwise an easier test than a load in
Chimera. The default arm COUNTS as well as logs, caps its own chatter at eight
lines, and `chimera_gl_host_unhandled` hands the count to both runners, which
print it at the end. And `bridge_answered` in run-gate.sh fails the gpu leg
when either flavour's stderr carries the line - the sandbox's stderr used to go
to /dev/null there, so it is now kept.

**Proved by breaking it.** With the case label changed to a number nothing
sends and both runners rebuilt, the full gate said:

    FAIL: gpu leg - the GPU bridge had no case for opcode 4 and answered 0 (gpu-n.err)

With the case back: `PASS: gpu leg - the OGL backend drew, native == sandbox
on this driver, and every opcode the guest sent had a case`. Gate at the commit: 13 ok, 0 failed, 2 skipped (the two need discs); the
negative control was 12 ok, 1 failed, 2 skipped, the one failure being the
leg under test.

**Found on the way.** `obj-native/run-native` did not relink: `dolphin-driver.cpp`
references `chimera_dolphin_gl_state_loaded`, added to OGLGfx.cpp by 9498b50
this morning, and `build/native`'s archives predated it. An incremental
`make -C build/native` rebuilt `libvideoogl.a` and the link went through. The
native reference the 9498b50 gate ran was the Sep 20 binary, which is fine for
what that gate measured (the sandbox against the engine) but worth knowing.

**What this does not establish** (gates.md, E): swiss on llvmpipe is neither a
game nor a driver. The leg proves the dispatcher answered every opcode the
guest sent, not that a picture is right on real hardware.

## Emulation options a project pins (chimera#149, 2026-09-26)

The issue asked for dolphin's picture and accuracy options. Each was measured
before it was declared: native, OpenGL on the GTX 1060 box (WSL), each option
at a non-default value against the default, the same fields. "RAM" is the
console's 24 MiB; "picture" is what Chimera shows.

| Option | Pro Rally 2002 (GC, 600 fields) | MK Armageddon (Wii, 600) | Virtua Striker 2002 (Triforce, 3000, 3D) |
|---|---|---|---|
| internal_resolution 2x | RAM + picture from field 100 | RAM + picture from 100 | RAM + picture; lag 547 vs 585 at 3000 |
| msaa 4x, ssaa | RAM + picture from 100 | RAM + picture from 100 | (not run past 600) |
| anisotropy 16x | RAM + picture from 100 | RAM + picture from 100 | (not run past 600) |
| texture_filtering nearest / linear | RAM + picture from 100 / 400 | from 100 / none | (not run past 600) |
| widescreen_hack | none (no 3D in the fields run) | none | wider view, see below |
| mmu, texture_cache safe, gpu_texture_decoding | none | none | none (3000 fields) |
| any of them on the software renderer | none | none | none |

What that decided:

- **The picture options are part of the machine.** EFB and XFB copies land in
  the console's memory, and they are drawn with the option applied, so the
  game reads back a different picture. They are project settings, chosen
  once, like xemu's internal resolution in #122. All of them are
  `exposedWhen renderer = opengl-hw`: the software rasterizer has none of them.
- **Internal Resolution did not raise the picture's resolution** - Chimera
  showed the console's 640-wide framebuffer decoded from RAM (patch 0021). By
  the user's decision, above 1x the picture is now the crisp XFB copy the
  presenter fetched from VRAM (patch 0023 hands it to the driver, which reads
  it back): 1280x896 at 2x on a 640x448 game. The RAM at field 3000 is the
  same whether the picture comes from VRAM or not. The known cost, measured
  when patch 0021 was made: the crisp copy is not in a savestate, so after a
  state load the picture can differ from a run that never stopped. The
  machine does not, and a movie played from power-on is crisp throughout.
  Capacity grew to 2880x2304 (4x a 720x576 field).
- **The widescreen hack did nothing headless.** dolphin widens the projection
  by the picture's aspect over the aspect it is drawn at, and works that out
  only when presenting to a window, which this core never does. The driver
  now sets the two factors itself each field, from the VI's aspect (machine
  state) against a stated 16:9 screen - dolphin's own arithmetic on a
  constant, so the same view on every host. On Virtua Striker's attract demo
  the view is visibly wider. The picture stays 640 wide, squeezed.
- **MMU, texture cache accuracy and GPU texture decoding measured inert** on
  this content. Declared anyway, by the user's decision: MMU is known to
  matter for some games (the TASVideos thread in the issue); Safe texture
  cache matters for games that change textures in place. Their defaults are
  dolphin's (off, Fast, off), so an existing project is the same machine.
  Hard-coding Safe, as the issue suggested, would have changed every existing
  opengl-hw project with no measured fix.

Gate: `options:config` (native: every name reaches its knob in dolphin's own
config after boot, and unset they read as dolphin's defaults),
`options:names` (the sandbox refuses a bogus value for each declared name,
naming it), `options:crisp` (GPU: 2x doubles the picture, native ==
sandbox on this driver).
