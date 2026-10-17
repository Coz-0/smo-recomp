# smo-recomp

A **static recompilation** of *Super Mario Odyssey* (version 1.0.0, and its 1.4.1 update) from the Nintendo Switch to a
native Windows x64 program, with its own runtime and a Vulkan renderer.

It is **not an emulator**: there is no CPU interpreter and no JIT. The game's AArch64 machine code is translated to C
ahead of time and compiled for the PC; the console's system libraries, graphics API, audio, video, input and file
system are re-implemented by this project's runtime.

> **This repository contains no game code, no game assets, no keys and no firmware.**
> You need your own legally obtained copy of the game (1.0.0; for 1.4.1 also its update) and keys and firmware dumped
> from your own console.
> Everything derived from them is generated on your PC, from your dump, under `build/`, and never leaves it.

## Status (October 2026)

Early, not finished: played by hand up to Cascade Kingdom's first checkpoint, the start checked with scripted input.
What is written here was measured on the development PC (Windows 11, RTX 4090); where something was not checked, it
says so. The table is version 1.0.0; the 1.4.1 update builds and runs too, and what was checked on it is listed
[below](#the-141-update).

| Area | State |
|---|---|
| Boot → title screen → file select | works |
| New game → prologue → Cap Kingdom → Bonneton → into Top-Hat Tower, up to Mario's first capture (a frog) | plays with scripted input: `tests/run_cap_kingdom.py` checks 13 checkpoints against the game's own state, from an empty save, without a crash (5 of 5 runs passed; 42,409 frames in the last) |
| Cap Kingdom → Cascade Kingdom, up to its first checkpoint | played by hand with an Xbox One controller; no scripted check yet |
| Other kingdoms | not played. Loaded from a copy of a save made to start there (`tools/dev/save_warp.py`): Metro Kingdom (daytime) and Ruined Kingdom (arrival, with its lightning) load and run at 60 fps once loaded, with 0 Vulkan call failures and 0 operations not implemented |
| Renderer | Vulkan. **Every shader stage in the game's shader archives translates and compiles** (Maxwell machine code → GLSL → SPIR-V), checked offline by `tests/check_all_shaders.py`: 27,226 of 27,226 distinct stages (7,291 vertex, 19,864 fragment, 3 geometry, 68 compute). The structured control flow of the translator was checked against its reference form on the GPU: 1,519 of 1,519 stages equal over 6.2 million invocations. 0 Vulkan call failures and 0 dropped command lists in the scripted run. No single-frame flashes: 0 frames with a flash in captures of every frame while the camera pans in Cascade Kingdom (2 to 9 in 361 before the fix: draws that read vertices past the range the game declared) |
| Geometry stages | the Ruined Kingdom's lightning: 657 draws in a run. Measured on the GPU: each draw's 31 line segments become 392-507 line primitives; read back from the scene buffer, a bolt adds 3 to 85 (peaks up to 752) to the brightness of about 3,000 pixels, over 1.7 behind it, before the game draws its clouds over it |
| Compute images, indirect draws and dispatches | implemented (image loads, stores and atomics, `nvnCommandBufferBindImage`; `DrawElementsIndirect`, `DispatchComputeIndirect`); no scene tried so far uses them, so they are not checked in play |
| Operations the renderer lacks | reported once in the log, counted and skipped: the game keeps running (`SMO_VK_MISSING=exit` stops at the first instead). None met in any run so far |
| Audio | the game's own audio renderer runs (lifted from its system library); the final mix plays through the Windows audio device (6 → 2 channel downmix, 48 kHz): 0 dropped samples, 0 underruns in the scripted run |
| Video | H.264 through Windows Media Foundation; the tutorial videos play with their picture (checked in the scripted run's captures) |
| Help (the game's manual) | opens in your web browser from the game's menu, from the manual in your dump; pages turn with the keyboard or a pad (checked in a browser) |
| Capture button | F12 or a pad's share button saves the frame as a BMP in the save folder's `album` (the saving checked through `SMO_CAPTURE_AT`, which presses the same button; the turn for the game's portrait setting is not checked against a console) |
| System calls | every system call the game made in the tested scenes is implemented (321 of 321 in the scripted Cap Kingdom run, 294 of 294 in the Ruined Kingdom); `SMO_IMPORT_SELFTEST=1` checks the implementations added from a list of the game's imports rather than from play (0 failures) |
| Controllers | keyboard, XInput, and any pad through SDL3 (hot-plug, 8 players, remapping file, rumble, gyro): passes its automated tests with virtual devices (51 of 51); played by hand with an Xbox One controller; other physical pads not tried |
| Save data | journalled like the console's (two-phase commit); the game's autosave and Resume work |
| Speed | 60 fps, paced to a 60 Hz clock. At Cascade Kingdom's first checkpoint 59.8-60.0 fps in every 300-frame window for two minutes (1% lows 52.7-57.4 fps), also while a web browser keeps most of a CPU core busy and two or four more processes each keep one busy (`tools/dev/cpuload.py`); before, the browser alone brought it to 56.4-59.8 fps per window. Scripted Cap Kingdom run, with a screenshot every 300 frames: 59.5 fps on average once play begins, 59.4-59.7 in Bonneton, 59.0-59.6 in Top-Hat Tower; lowest 48.6 while the tower stage loads (before: 26-42 fps until Cappy joins, then about 13). Measured on the development PC only |
| Online | not available (the network services answer as an offline console) |
| amiibo | F1, or both stick buttons on a pad (`controllers.ini` `[hotkeys]`), opens a menu drawn like the game's pause menu, from the game's own layout textures, fonts and texts: the 91 amiibo the game has names for, in its language. The game waits while it is open; A puts the chosen amiibo on a virtual NFC reader, and if the game is waiting for one (its amiibo mode, the amiibo NPC's prompt) → is held for you until it has read it. Checked in Cascade Kingdom with scripted input: Mario (the game runs its invincibility help), Peach (a Life-Up Heart), Bowser (its coin help), Luigi (coins); chosen in the game's own pause menu, the amiibo waits on the reader until → is held. The amiibo NPC's prompt was not tried |

### The 1.4.1 update

With your update package, the same sources build the game as the update leaves it (`--target 1.4.1`, beside 1.0.0).
Checked on 1.4.1 so far:

| Area | State |
|---|---|
| Game files | the update's RomFS is a patch of the base's; the extraction applies it (336 files added, 246 changed), and `tools/extract/romfs.py --update --verify` checks every block against the update's own hash tree: 346,568 blocks and the levels above them, 0 wrong |
| Boot, new game, a 1.0.0 save | a new game starts at the mode select; a 1.0.0 save made at Cascade Kingdom's first checkpoint loads after the game's update notifications, and play resumes there |
| Scripted Cap Kingdom run | `tests/run_cap_kingdom.py` passes: 13 of 13 checkpoints from an empty save to the frog capture, 0 host exceptions, 0 Vulkan call failures, 0 operations not implemented (3 of 3 runs passed; 40,472 frames in the last) |
| Speed | Cascade Kingdom's first checkpoint: 60.0 fps in all 24 windows of 300 frames (1% lows 52.7-57.0), with the fast path for the game's sampler list that 1.0.0 uses too (without it 59.88 fps on average, 1% lows down to 43.6). Scripted Cap Kingdom run, with a screenshot every 300 frames: 59.4 fps on average once play begins, lowest 48.3 while the tower stage loads |
| Shaders | every stage in the game's shader archives translates and compiles, the update's new ones included: 27,886 of 27,886 distinct stages (7,380 vertex, 20,435 fragment, 3 geometry, 68 compute). One small fragment shader the game creates at start-up, from outside those archives, is not translated yet (its texture handle is read from a uniform buffer): no draw used it from start-up to the mode select (70,752 draws, 0 skipped); later scenes not checked |
| Sound | the game's audio renderer runs on the update's newer system library, whose commands are laid out differently; both layouts are read: 141,972 audio frames in the scripted run, 0 dropped samples, 0 underruns |
| System calls | the ones 1.4.1 adds are implemented: 333 of 333 reached in the scripted run; `SMO_IMPORT_SELFTEST=1` 0 failures |
| Not tried yet | what the update adds (Balloon World, the new Snapshot Mode filters and the rest its notification screens list, the Labo VR mode), the amiibo menu, play by hand |

## How it works

- **Extraction** (`tools/extract`): reads your NSP in place (NCA/PFS0/RomFS readers, keys from your own `prod.keys`) and writes the ELF images, the RomFS and the system fonts to `build/`.
- **Function boundaries** come from the executable's `.eh_frame` unwind data and its own symbol table, never from guesses: 74,822 functions in `main`.
- **Lifter** (`tools/lifter/recomp.py`): translates each AArch64 function into one C function operating on a register context. The output is compiled with clang into a game DLL; nothing is interpreted at run time.
- **Differential testing** (`tools/lifter/difftest.py`): sampled instruction shapes run both as lifted C and in the Unicorn CPU emulator, over random, special floating-point, branch and trap inputs; any difference is a failure (1.0.0: 23,069 of 23,069 equal; 1.4.1: 23,194 of 23,194). A mutation test checks that the harness catches planted bugs.
- **System-library lifting** (`tools/lifter/sdklift.py`): parts of the game's own SDK, such as the audio renderer and its DSP, are lifted the same way, so the host only provides the bottom layer (timers, events, memory, the audio device).
- **Game versions** (`tools/target.py`): 1.0.0, or with the update package 1.4.1, each built in a folder of its own (`build\` and `build\1.4.1\`). The update's RomFS is a patch of the base's (a relocation table and the counter generations of its encryption) that the extraction applies. Its executable keeps almost no symbols: the game's state names come from its RTTI, and the functions the runtime replaces with host code are found by matching them with 1.0.0's. Where its newer system library lays data out differently (the audio commands), the runtime reads it per version.
- **Host runtime** (`runtime/`), adapted from [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp):
  - Horizon OS services: `nn::os` threads and events, `nn::fs` with save-data journalling, hid, settings, an offline network stack.
  - An NVN → Vulkan graphics back end. Command lists are recorded on the game's threads and replayed on a render thread; a recorded list lives until the game finalizes it or records over its memory, as on the console.
  - A Maxwell shader → GLSL → SPIR-V translator (`mx_translate.c`) with structured control flow.
  - Texture de-swizzling, checked against all 19,325 textures in the RomFS.
  - Movie playback on Windows Media Foundation.
  - Menus of its own (the amiibo menu) drawn in software from the game's layout textures (BNTX), fonts (BFFNT) and
    message files (MSBT), read from your RomFS at run time.
- **Scripted runs** (`runtime/smo_nerve.c`, `runtime/nnhid.c`, `tests/`): the runtime can report which state each of the game's state machines is in. Input sequences wait for those states and steer Mario to map coordinates through the game's camera, so a run does not depend on frame rate or loading times. See [`docs/SCRIPTED_RUNS.md`](docs/SCRIPTED_RUNS.md).
- **Strict audit:** every unimplemented system call is logged with its name and call count; `SMO_STRICT=report` lists every call to a placeholder handler.

## Plans

- Speed on slower CPUs and GPUs; shorter stalls while a stage loads (the first seconds in a kingdom never visited before
  run at 40-55 fps while its textures and shaders are made).
- Scripted runs further through the game: the rest of Cap Kingdom, then kingdom by kingdom.
- Checks on real hardware for the few shader instructions translated from documentation only (`SHFL`, `AL2P`, indexed `IPA`).
- Play with other physical controllers (PlayStation, Switch Pro), other GPUs (AMD, Intel).
- 1.4.1: the one shader not translated yet; checks of what the update adds (Balloon World, the Snapshot Mode filters,
  the Labo VR mode) and of the amiibo menu.

## Requirements

- Windows 10/11 x64 and a Vulkan 1.3 GPU (developed on NVIDIA; `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state` and `VK_EXT_depth_clip_control` are used).
- Your own *Super Mario Odyssey* 1.0.0 dump (the base NSP; for 1.4.1 also its update NSP), and `prod.keys` and firmware dumped from your own Switch.
- [LLVM/clang 9.0.0](https://releases.llvm.org/), Visual Studio 2019 Build Tools (16.11) with Windows SDK 10.0.19041, the [Vulkan SDK](https://vulkan.lunarg.com/) 1.4, [SDL3](https://github.com/libsdl-org/SDL) 3.4.18, Python 3.13.

## Building and running

[`BUILDING.md`](BUILDING.md) has the details. In short, from a fresh clone:

```bat
py -3.13 -m venv .venv
.venv\Scripts\python -m pip install pycryptodome lz4 zstandard capstone unicorn numpy Pillow pyyaml
.venv\Scripts\python scripts\setup.py --dump <folder with your NSP>
.venv\Scripts\python scripts\run.py first --seconds 300
```

For 1.4.1, put the update NSP in the same folder and add `--target 1.4.1` to the last two commands. That build lives in
`build\1.4.1\`, beside the 1.0.0 one, with saves of its own in `build\1.4.1\userdata`; copy `build\userdata\save` there
to continue a 1.0.0 save.

Debugging aids (frame and per-pass captures, shader dumps, guest call stacks on crashes, heap write watches, state
logs) are described in [`docs/DEBUGGING.md`](docs/DEBUGGING.md).

## Repository layout

```
runtime/         host runtime: OS services, NVN -> Vulkan, shader translator, audio, input, movies, state tracker
scripts/         setup.py (from your dump to a build) and run.py (start the game, play an input sequence)
tools/target.py  which game version a build is for (1.0.0, or the 1.4.1 update) and where its files go
tools/extract/   NSP/NCA/RomFS readers (the update's RomFS patch too), function list, RomFS and font extraction
tools/lifter/    AArch64 -> C lifter, system-library lifter, differential test, build steps
tools/dev/       disassembler, caller finder, SARC and stage-placement listers, Maxwell instruction namer
tools/port/      idempotent adaptation of the ssbu-recomp runtime sources
tests/           scripted Cap Kingdom run, every shader stage of the game offline, shader translator equivalence harness,
                 texture layout check, fast-path checks
docs/            debugging aids, scripted runs
```

## Related work

- [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp): the static recompilation of *Super Smash Bros. Ultimate* this project is built on; its design, lifter and runtime were adapted here (GPL-3.0).
- [OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp): the decompilation project of *Super Mario Odyssey*, read as a reference only; none of its code is included.
- yuzu/suyu and Ryujinx: documentation and sources consulted for console behaviour (cited where used).

## How this was made

Most of the code was written by AI coding agents (Claude) working under the maintainer's direction, starting from
ssbu-recomp's design. Two rules kept it honest: every claim comes from a test or a measurement (this README says what
was measured and what was not), and nothing derived from the game goes into the repository. Expect rough edges.

## Contributing

Testers with other GPUs and controllers, and people who know the game's internals, are welcome. Read
[CONTRIBUTING.md](CONTRIBUTING.md) first: three rules (nothing derived from the game in the repository, clean sources
only, every claim measured) apply to every contribution.

## Legal

This project is not affiliated with, endorsed or sponsored by Nintendo. *Super Mario Odyssey* and *Nintendo Switch*
are trademarks of Nintendo and are named only to say what the software works with. The repository contains only
original source code and documentation (GPL-3.0-or-later, see [LICENSE](LICENSE)): no game code, binaries, assets,
console keys or firmware, and it does not help anyone obtain them. Function names and addresses in comments identify
locations in the game's own executable, for interoperability; they are not game code. You must own the game and dump
it yourself.

## Credits

- [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) by Denkishi: the design and runtime this project builds on.
- [Unicorn](https://www.unicorn-engine.org/) and [Capstone](https://www.capstone-engine.org/): reference CPU emulation and disassembly.
- [SDL3](https://github.com/libsdl-org/SDL), [glslang/shaderc](https://github.com/KhronosGroup/glslang), the Vulkan SDK.
