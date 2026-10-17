# smo-recomp

A **static recompilation** of *Super Mario Odyssey* (version 1.0.0) from the Nintendo Switch to a native Windows x64
program, with its own runtime and a Vulkan renderer.

It is **not an emulator**: there is no CPU interpreter and no JIT. The game's AArch64 machine code is translated to C
ahead of time and compiled for the PC; the console's system libraries, graphics API, audio, video, input and file
system are re-implemented by this project's runtime.

> **This repository contains no game code, no game assets, no keys and no firmware.**
> You need your own legally obtained copy of the game (1.0.0) and keys and firmware dumped from your own console.
> Everything derived from them is generated on your PC, from your dump, under `build/`, and never leaves it.

## Status (October 2026)

Early, not finished: played by hand up to Cascade Kingdom's first checkpoint, the start checked with scripted input.
What is written here was measured on the development PC (Windows 11, RTX 4090); where something was not checked, it
says so.

| Area | State |
|---|---|
| Boot → title screen → file select | works |
| New game → prologue → Cap Kingdom → Bonneton → into Top-Hat Tower, up to Mario's first capture (a frog) | plays with scripted input: `tests/run_cap_kingdom.py` checks 13 checkpoints against the game's own state, from an empty save, without a crash (5 of 5 runs passed; 42,409 frames in the last) |
| Cap Kingdom → Cascade Kingdom, up to its first checkpoint | played by hand with an Xbox One controller; no scripted check yet |
| The rest of Cascade Kingdom, the other kingdoms | not tested |
| Renderer | Vulkan. Every shader stage the scripted run meets is translated (Maxwell machine code → GLSL → SPIR-V; 0 untranslated or rejected). The structured control flow of the translator was checked against its reference form on the GPU: 1,519 of 1,519 stages equal over 6.2 million invocations. 0 Vulkan call failures and 0 dropped command lists in the scripted run. No single-frame flashes: 0 frames with a flash in captures of every frame while the camera pans in Cascade Kingdom (2 to 9 in 361 before the fix: draws that read vertices past the range the game declared) |
| Audio | the game's own audio renderer runs (lifted from its system library); the final mix plays through the Windows audio device (6 → 2 channel downmix, 48 kHz): 0 dropped samples, 0 underruns in the scripted run |
| Video | H.264 through Windows Media Foundation; the tutorial videos open and close without error, their picture is not checked |
| Controllers | keyboard, XInput, and any pad through SDL3 (hot-plug, 8 players, remapping file, rumble, gyro): passes its automated tests with virtual devices (49 of 49); played by hand with an Xbox One controller; other physical pads not tried |
| Save data | journalled like the console's (two-phase commit); the game's autosave and Resume work |
| Speed | 60 fps, paced to a 60 Hz clock. At Cascade Kingdom's first checkpoint 59.8-60.0 fps in every 300-frame window for two minutes (1% lows 52.7-57.4 fps), also while a web browser keeps most of a CPU core busy and two or four more processes each keep one busy (`tools/dev/cpuload.py`); before, the browser alone brought it to 56.4-59.8 fps per window. Scripted Cap Kingdom run, with a screenshot every 300 frames: 59.5 fps on average once play begins, 59.4-59.7 in Bonneton, 59.0-59.6 in Top-Hat Tower; lowest 48.6 while the tower stage loads (before: 26-42 fps until Cappy joins, then about 13). Measured on the development PC only |
| Online | not available (the network services answer as an offline console) |

## How it works

- **Extraction** (`tools/extract`): reads your NSP in place (NCA/PFS0/RomFS readers, keys from your own `prod.keys`) and writes the ELF images, the RomFS and the system fonts to `build/`.
- **Function boundaries** come from the executable's `.eh_frame` unwind data and its own symbol table, never from guesses: 74,822 functions in `main`.
- **Lifter** (`tools/lifter/recomp.py`): translates each AArch64 function into one C function operating on a register context. The output is compiled with clang into a game DLL; nothing is interpreted at run time.
- **Differential testing** (`tools/lifter/difftest.py`): sampled instruction shapes run both as lifted C and in the Unicorn CPU emulator, over random, special floating-point, branch and trap inputs; any difference is a failure (23,068 of 23,068 equal). A mutation test checks that the harness catches planted bugs.
- **System-library lifting** (`tools/lifter/sdklift.py`): parts of the game's own SDK, such as the audio renderer and its DSP, are lifted the same way, so the host only provides the bottom layer (timers, events, memory, the audio device).
- **Host runtime** (`runtime/`), adapted from [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp):
  - Horizon OS services: `nn::os` threads and events, `nn::fs` with save-data journalling, hid, settings, an offline network stack.
  - An NVN → Vulkan graphics back end. Command lists are recorded on the game's threads and replayed on a render thread; a recorded list lives until the game finalizes it or records over its memory, as on the console.
  - A Maxwell shader → GLSL → SPIR-V translator (`mx_translate.c`) with structured control flow.
  - Texture de-swizzling, checked against all 19,325 textures in the RomFS.
  - Movie playback on Windows Media Foundation.
- **Scripted runs** (`runtime/smo_nerve.c`, `runtime/nnhid.c`, `tests/`): the runtime can report which state each of the game's state machines is in. Input sequences wait for those states and steer Mario to map coordinates through the game's camera, so a run does not depend on frame rate or loading times. See [`docs/SCRIPTED_RUNS.md`](docs/SCRIPTED_RUNS.md).
- **Strict audit:** every unimplemented system call is logged with its name and call count; `SMO_STRICT=report` lists every call to a placeholder handler.

## Plans

- Speed on slower CPUs and GPUs; shorter stalls while a stage loads.
- Scripted runs further through the game: the rest of Cap Kingdom, then kingdom by kingdom.
- Checks on real hardware for the few shader instructions translated from documentation only (`SHFL`, `AL2P`, indexed `IPA`).
- Play with other physical controllers (PlayStation, Switch Pro), other GPUs (AMD, Intel).

## Requirements

- Windows 10/11 x64 and a Vulkan 1.3 GPU (developed on NVIDIA; `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state` and `VK_EXT_depth_clip_control` are used).
- Your own *Super Mario Odyssey* 1.0.0 dump (the base NSP), and `prod.keys` and firmware dumped from your own Switch.
- [LLVM/clang 9.0.0](https://releases.llvm.org/), Visual Studio 2019 Build Tools (16.11) with Windows SDK 10.0.19041, the [Vulkan SDK](https://vulkan.lunarg.com/) 1.4, [SDL3](https://github.com/libsdl-org/SDL) 3.4.18, Python 3.13.

## Building and running

[`BUILDING.md`](BUILDING.md) has the details. In short, from a fresh clone:

```bat
py -3.13 -m venv .venv
.venv\Scripts\python -m pip install pycryptodome lz4 zstandard capstone unicorn numpy Pillow pyyaml
.venv\Scripts\python scripts\setup.py --dump <folder with your NSP>
.venv\Scripts\python scripts\run.py first --seconds 300
```

Debugging aids (frame and per-pass captures, shader dumps, guest call stacks on crashes, heap write watches, state
logs) are described in [`docs/DEBUGGING.md`](docs/DEBUGGING.md).

## Repository layout

```
runtime/         host runtime: OS services, NVN -> Vulkan, shader translator, audio, input, movies, state tracker
scripts/         setup.py (from your dump to a build) and run.py (start the game, play an input sequence)
tools/extract/   NSP/NCA/RomFS readers, function list, RomFS and font extraction
tools/lifter/    AArch64 -> C lifter, system-library lifter, differential test, build steps
tools/dev/       disassembler, caller finder, SARC and stage-placement listers, Maxwell instruction namer
tools/port/      idempotent adaptation of the ssbu-recomp runtime sources
tests/           scripted Cap Kingdom run, shader translator equivalence harness, texture layout check, fast-path checks
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
