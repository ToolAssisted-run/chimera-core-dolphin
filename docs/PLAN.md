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
