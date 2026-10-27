# Contributing

Testers with other GPUs and controllers, people who know the game's internals, and anyone who wants to carry the
scripted runs further through the game are welcome. Three rules apply to every contribution.

## The three rules

### 1. Nothing derived from the game goes into the repository

- No game code, binaries, assets, keys, firmware or save data, and none of what the build makes from them: lifted C,
  function lists, shaders, extracted files, captures, logs. All of it lives in `build/`, which git ignores; the
  `.gitignore` is the firewall, keep it that way.
- Screenshots and captures of the game stay out of the repository too.
- Names are fine: function, class and state names from the game's own executable, used to find places in it
  (interoperability), and coordinates measured in play for the input sequences in `tests/`.

### 2. Clean sources only

- [OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp) has no licence. Read it as a reference; never copy
  its code, its lists or its files into the repository.
- [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) (GPL-3.0) is the base of the runtime and the lifter; its code
  is adapted under its licence, and files adapted from it say so in their header.
- Emulator sources and documentation (yuzu/suyu, Ryujinx) consulted for console behaviour are cited where used; copy
  nothing their licences do not allow.

### 3. Every claim is measured

- A claim in a comment, a document or a pull request comes from a test or a measurement, and says how it was
  measured; what was not checked is said too.
- Never change a test or its reference data to make something pass. If a test is wrong, say why in the pull request
  and fix it as a change of its own.

## Ways to help

- Run the scripted Cap Kingdom test (`tests/run_cap_kingdom.py`) on an AMD or Intel GPU and report its result lines
  and the log.
- Play with a physical controller and report what does not work.
- Carry the scripted runs further: the rest of Cap Kingdom (Topper), then the other kingdoms
  ([`docs/scripted-runs.md`](docs/scripted-runs.md) shows how a route is found).
- Speed: the GPU wait at a conditional render while compute results wait to be copied back (once a frame at Cascade
  Kingdom's first checkpoint, 1.2 ms each); profiling the game side in Top-Hat Tower.
- Check the shader instructions translated from documentation only (`SHFL`, `AL2P`, indexed `IPA`) against hardware.

## Building

See [`docs/building.md`](docs/building.md), and [`docs/architecture.md`](docs/architecture.md) for how the parts fit.

## Reporting a problem

Give the commit, your GPU and driver version, what you did (or the input sequence), and the run's log
(`build\run\<name>.log`): its last lines name the crash, the missing feature or the stuck state. Never attach game
files or your keys.

## Code

- `runtime/` is C (clang 9), one folder per subsystem (`core`, `libc`, `os`, `fs`, `gfx`, `audio`, `input`,
  `services`, `game`). A new file goes into its folder and into `HOST_SOURCES` in `tools/lifter/build_exe.py`.
  Comments say what the code does and why, and cite the source of any behaviour taken from documentation or another
  project.
- `tools/` and `scripts/` are Python 3.13; `tools/<area>/` for build tools, `tools/dev/` for lookups while debugging.
  `tools/target.py` says where the build's files are.
- Runtime switches use the `SMO_` prefix and are described in [`docs/debugging.md`](docs/debugging.md).
- No game address is written into the runtime: what it needs is generated into `build/gen` (for example `GAME_SYMS` in
  `tools/lifter/gen_tables.py` for game functions the runtime calls by name).
- A change to the shader translator's output must bump `MX_VK_VERSION` in `runtime/gfx/nvn_vk_pipelines.c`, so that
  cached stages are translated again.

## Testing

Say in the pull request which of these you ran:

- **Lifter changes:** `tools\lifter\difftest.py all` (0 different) and `tools\lifter\difftest_mutation.py` (the
  harness still catches planted bugs).
- **Shader translator changes:** `tests\build_mx_equiv.py`, then from `build\bin`: `mx_equiv.exe <folder of dumped
  stages>` (0 different); stages are dumped with `SMO_SHADERDUMP=<folder>` and `SMO_VK_DUMP_MINBYTES=<n>`. And
  `tests\check_all_shaders.py`: every stage of the game's shader files must still translate and compile.
- **System call changes:** `scripts\run.py selftest --env SMO_IMPORT_SELFTEST=1` (0 failed checks). A handler belongs
  to a name the game imports: `--env SMO_AUDIT_DUMP=<file>` lists in its `unimported` rows the handlers registered under
  other names, and each of those has a comment in its table saying how it is reached (the lifted SDK code, a C
  wrapper); the start log has no `is not an import` line.
- **Texture layout changes:** `tests\bntx_layout_check.py` (every texture of the RomFS equal).
- **Changes to the largest-index scan (`runtime/gfx/nvn_vk_draw.c`) or the command-list overwrite index
  (`runtime/gfx/nvn_gl.c`):** `tests\check_fast_paths.py` (`PASS`: both equal to the plain code on random input).
- **Anything that can change game behaviour, rendering or timing:** `tests\run_cap_kingdom.py <new run name>` must
  print `RESULT: PASS`.
- **Renderer, threading or anything a stage, a kingdom or a scenario the scripted run does not reach could meet:**
  `tests\stress_stages.py --save <a copy of a save> --scenarios` starts every stage and every scenario of every kingdom
  (hours, two games side by side; [`docs/debugging.md`](docs/debugging.md)), and its `summary.md` should list nothing
  new.
- **Input changes:** `scripts\run.py padtest --env SMO_PAD_SELFTEST=1` (every check passes).
- **Changes to how `scripts\run.py` limits the test games running at once:** `tests\check_game_places.py` (0 failed;
  starts no game).
- **Changes to the amiibo menu or the NFC reader:** the `SMO_IMPORT_SELFTEST=1` run above (its `nfp` and `amiibo menu`
  lines), and a scripted run that opens the menu and scans an amiibo ([`docs/scripted-runs.md`](docs/scripted-runs.md)):
  the log must show the game reading it (`nfp: the game read the amiibo's model information`).

Known: `SMO_SDK_SELFTEST=1`, the self-test of the service families, fails two checks (the `errno` location is not in
the thread block; `NumberLineAllocator` is not lifted) and stops at `nn::image::JpegDecoder`, which the game imports
but no handler implements.

## Commits and licence

Contributions are licensed GPL-3.0-or-later, like the project. A commit message says what changed and what was
measured.

## Legal

This project is not affiliated with Nintendo. Do not contribute anything you do not have the right to share.
