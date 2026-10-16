# smo-recomp

**A static recompilation of _Super Mario Odyssey_ (Nintendo Switch, version 1.0.0) into a native Windows x64 executable.**

> **No game code, assets, keys or firmware are included in this repository, and none ever will be.**
> You need your own legally obtained copy of the game, and keys and firmware dumped from your own console.
> Everything derived from the game is generated locally on your machine at build time, under `build/` (git-ignored).

This is an unofficial, non-commercial research and preservation project. It is not affiliated with or endorsed by Nintendo.

---

## Status

Work in progress. It boots, plays the opening cutscene, shows the title screen and the save-file screens, and loads into
Cap Kingdom. It is **not playable end to end yet**. Every claim below was measured.

**Measured (October 2026):**
- **System calls reached** (title → file select): 261 SDK imports, every one with a host implementation (0 stubs).
- **Differential test** against Unicorn: 23,068 / 23,068 sampled instructions identical, 0 untranslated (main executable plus the lifted SDK functions).
- **Function boundaries** for `main`: 74,822 functions, from `.eh_frame` and the binary's own symbol table.
- **Stability:** 9,224 frames in a 200-second run (title → Resume → Cap Kingdom), 0 Vulkan call failures.
- **Speed:** 28–31 fps in Cap Kingdom on an RTX 4090; not optimized yet.

## How it works

- **Extraction** (`tools/extract`): reads your NSP in place (NCA/PFS0/RomFS readers, keys from your own `prod.keys`) and writes the ELF images, RomFS and system fonts to `build/`.
- **Function boundaries** come from the executable's `.eh_frame` unwind data and its own symbol table, never from guesses. That yields 74,822 functions in `main`.
- **Lifter** (`tools/lifter/recomp.py`): translates each AArch64 function into one C function operating on a register context. The output is compiled with clang into a game DLL; nothing is interpreted at run time.
- **Differential testing** (`tools/lifter/difftest.py`):
  - Sampled instruction shapes are executed both by the lifted C and by the Unicorn CPU emulator, over random, special floating-point, branch and trap inputs.
  - Any difference is a failure. A mutation test checks that the harness catches planted bugs.
- **SDK lifting** (`tools/lifter/sdklift.py`): parts of the game's own SDK, such as the audio renderer and DSP, are lifted the same way. The host then only provides the bottom layer: timers, events, memory.
- **Host runtime** (`runtime/`), adapted from ssbu-recomp:
  - Horizon OS services: `nn::os` threads and events, `nn::fs` with save-data journaling, hid, settings, an offline network stack.
  - An NVN → Vulkan graphics backend.
  - A Maxwell shader → GLSL → SPIR-V translator (`mx_translate.c`).
  - Texture de-swizzling, verified against all 19,325 textures in the RomFS.
  - Movie playback on Windows Media Foundation.
- **Strict audit:** every unimplemented system call is logged with its name and call count; `SMO_STRICT=report` lists every call to a placeholder handler.

## Requirements

- Windows 10/11 x64, a Vulkan 1.3 GPU (developed on NVIDIA; `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state` and `VK_EXT_depth_clip_control` are used).
- Your own *Super Mario Odyssey* 1.0.0 dump (base NSP), and `prod.keys` plus firmware dumped from your own Switch.
- [LLVM/clang 9.0.0](https://releases.llvm.org/) (the lifted code is compiled with it), Visual Studio 2019 Build Tools (16.11) and Windows SDK 10.0.19041.
- [Vulkan SDK](https://vulkan.lunarg.com/) 1.4 (headers and `shaderc_shared.dll`), [SDL3](https://github.com/libsdl-org/SDL) 3.4.18 in `deps/` (or `SDL3_ROOT`).
- Python 3.13.

## Building

Everything is generated under `build/`. `<dump>` is the folder holding your NSP.

```bat
py -3.13 -m venv .venv
.venv\Scripts\python -m pip install pycryptodome lz4 zstandard capstone unicorn numpy Pillow pyyaml

rem 1. extraction (keys: --keys, %SMO_KEYS%, ~/.switch/prod.keys, or Ryujinx's folder)
.venv\Scripts\python tools\extract\extract.py --dir <dump>
.venv\Scripts\python tools\extract\functions.py
.venv\Scripts\python tools\extract\romfs.py --dir <dump>
.venv\Scripts\python tools\extract\fw_fonts.py --fw <folder with your firmware .nca files>

rem 2. lifting, verification, compilation
.venv\Scripts\python tools\lifter\difftest.py all          & rem must report 0 different
.venv\Scripts\python tools\lifter\recomp.py full
.venv\Scripts\python tools\lifter\build_main.py 28
.venv\Scripts\python tools\lifter\gen_tables.py
.venv\Scripts\python tools\lifter\gen_sdktest.py
.venv\Scripts\python tools\lifter\sdklift.py gen
.venv\Scripts\python tools\lifter\build_exe.py           & rem -> build\bin\smo_game.dll + smo_native.exe

rem 3. run (from build\bin; log in build\run\<name>.log)
.venv\Scripts\python tools\run.py first --seconds 120
```

If loading stalls on a very large shader, add `--env SMO_VK_NOHOLD=1 --env SMO_VK_BARRIER=0` (draws whose shader is not
ready yet are skipped instead of waited for). Settings live in `build/config/graphics.ini` and
`build/config/controllers.ini` (created on first run). Saves go to `build/userdata/save`. The runtime's switches
(`SMO_*`) are described in the comments of the runtime files that read them.

## Repository layout

```
tools/extract/   NSP/NCA/RomFS readers, function list, RomFS and font extraction
tools/lifter/    AArch64 -> C lifter, SDK lifter, differential test, build scripts
tools/dev/       disassembler, caller finder, SARC lister, Maxwell instruction namer
tools/port/      idempotent adaptation of the ssbu-recomp runtime sources
tools/test/      texture layout verification
tools/run.py     runs the built game and summarises the log
runtime/         host runtime: OS services, NVN -> Vulkan, shader translator, audio, input, movies
```

## Legal

- This repository contains only original source code and documentation, licensed GPL-3.0.
- It contains no Nintendo code, binaries, assets, keys or firmware.
- Function names and addresses in comments identify locations in the game's own executable, for interoperability; they are not game code.
- `.gitignore` excludes every file derived from a game dump. Please keep it that way in contributions.
- *Super Mario Odyssey* and Nintendo Switch are trademarks of Nintendo.

## Credits

- [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) by Denkishi: the design and runtime this project builds on (GPL-3.0).
- [OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp): used as a reading reference only; none of its code is included.
- [Unicorn](https://www.unicorn-engine.org/) and [Capstone](https://www.capstone-engine.org/): reference CPU emulation and disassembly.
- [SDL3](https://github.com/libsdl-org/SDL), [glslang/shaderc](https://github.com/KhronosGroup/glslang), the Vulkan SDK.
- yuzu/suyu and Ryujinx documentation and sources, consulted as references for console behaviour (cited where used).

## License

[GPL-3.0-or-later](LICENSE).
