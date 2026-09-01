# chimera-core-dolphin

The Nintendo GameCube (and, later, Wii) as a
[Chimera](https://github.com/ToolAssisted-run/chimera) waterbox core, built from
[Dolphin](https://dolphin-emu.org). The machine runs deterministically inside
the miniBox sandbox: byte-for-byte reproducible boots, savestates that are
arena snapshots, PowerPC interpreter and software renderer first.

- `extern/dolphin` - upstream, pinned, unmodified
- `patches/` - the local patch series (numbered, applied by `apply-patches.sh`;
  each a build option or a weak hook, never a deletion)
- `waterbox/` - the adapter, the curated source list, the build and the gate
- `docs/PLAN.md` - milestones and the decisions behind them

Upstream is GPL-2.0-or-later; the glue in this repository is MIT. Test content
(discs, romsets) lives in `tests/roms-local/`, which is gitignored and must
never be committed.
