# chimera-core-dolphin

The Nintendo GameCube and Wii as a
[Chimera](https://github.com/ToolAssisted-run/chimera) waterbox core, built from
[Dolphin](https://dolphin-emu.org). The machine runs deterministically inside
the miniBox sandbox: byte-for-byte reproducible boots, savestates that are
arena snapshots, JIT and interpreter as citable machines, software and
bridged-OpenGL renderers, and the Wii's NAND held in guest memory.

- `extern/dolphin` - upstream, pinned, unmodified
- `patches/` - the local patch series (numbered, applied by `apply-patches.sh`;
  each a build option or a weak hook, never a deletion)
- `waterbox/` - the adapter, the curated source list, the build and the gate
- `docs/PLAN.md` - milestones and the decisions behind them

Upstream is GPL-2.0-or-later; the glue in this repository is MIT. Test content
(discs, romsets) lives in `tests/roms-local/`, which is gitignored and must
never be committed.

## Using it in Chimera, and building it

Chimera ships no cores and downloads nothing. Download the `.chimeraCore`
package from this repository's
[Releases](https://github.com/ToolAssisted-run/chimera-core-dolphin/releases)
page, or build it, and put it in the `Cores` folder beside `Chimera.exe`;
File > Core Manager lists what is there. The same file runs on Linux and on
Windows.

To build it you need a Chimera checkout with miniBox built; then
`./waterbox/build-package.sh -r <chimera checkout>` builds the guest and
writes `<chimera checkout>/build/Cores/dolphin.chimeraCore`.
[`docs/BUILDING.md`](docs/BUILDING.md) has every step, option and requirement,
and the gates. [`AGENTS.md`](AGENTS.md) is the operating guide for an AI
coding agent.
