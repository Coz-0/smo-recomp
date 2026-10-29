# Building

Everything the build makes from the game is derived from **your own dump** on **your own PC**, into `build/` (ignored
by git). One script does all of it, in steps that resume where they stopped:

```bat
.venv\Scripts\python scripts\setup.py --dump "<folder of your dump>"
```

No console keys and no firmware are needed: the dump's files are already decrypted by your console, and the build only
converts and reads them.

## What you need

- Windows 10/11 x64 and a GPU with Vulkan 1.3 (developed on NVIDIA). The renderer uses
  `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state` and
  `VK_EXT_depth_clip_control`.
- **Python 3.13** in a virtual environment in the repository:
  ```bat
  py -3.13 -m venv .venv
  .venv\Scripts\python -m pip install lz4 capstone unicorn numpy Pillow
  ```
- **LLVM/clang 9.0.0** (the lifted code is compiled with it): unpack it to `C:\llvm-9.0.0` or point `LLVM_ROOT` at it.
- **Visual Studio 2019 Build Tools** (16.11) with the Windows SDK 10.0.19041: the C runtime and system libraries clang
  links against.
- The **Vulkan SDK** 1.4: its headers and `vulkan-1.lib`, and its `shaderc_shared.dll`, which `smo_glslang.dll` (built
  from `runtime/gfx/vk_shaderc_shim.c`) uses to turn GLSL into SPIR-V. `VULKAN_HEADERS` overrides the header folder.
- **SDL3** 3.4.18 (the development package for Visual C++) in `deps\SDL3-3.4.18`, or `SDL3_ROOT` pointing at it.
- The **WebView2 SDK** (Microsoft's NuGet package `Microsoft.Web.WebView2`, developed with 1.0.2903.40; a `.nupkg` is
  a zip file): its `build\native\include` and `build\native\x64` folders in `deps\webview2-<version>\build\native\`, or
  `WEBVIEW2_ROOT` pointing at the unpacked package. The game's Help is shown in the game window with it; the **WebView2
  Runtime** it needs is part of Windows 11 (on Windows 10, install Microsoft's Evergreen WebView2 Runtime). Without the
  runtime, Help opens in your web browser.

`scripts\setup.py` checks for each of these first and names what is missing.

## What to dump

Dump the game from your own console with a homebrew dumping tool that writes decrypted files, such as nxdumptool. The
build needs the game as the 1.4.1 update leaves it, in one folder laid out like this:

```
<your dump folder>\
  exefs\          the program's ExeFS with the 1.4.1 update: main, main.npdm, sdk, rtld, subsdk0
  romfs\          the game's RomFS with the 1.4.1 update applied
  control.nacp    the update's control data
  manual\         optional: the Help pages (the RomFS of the game's HTML document content: html-document\...)
  fonts\          optional: the console's shared fonts, as the console stores them (nintendo_udsg-r_std_003.bfttf
                  and the others) or as .ttf files of the same names
```

- **One version:** nothing of the base game (1.0.0) is read. The 1.4.1 executable keeps almost no symbols; the few
  game functions the runtime needs are found in it by what they are and do (`tools/lifter/gen_tables.py`). A dump
  folder laid out for older builds may still hold the base game's ExeFS as `exefs-1.0.0\`: it is ignored.
- **Checks:** the import reads the program id from `main.npdm`, the NintendoSDK version `main` was built with (7.3.2)
  and the display version in `control.nacp` (1.4.1), and looks for the RomFS's own folders. A missing or wrong piece is
  named.
- **Read in place:** `build\romfs` and `build\manual` become directory junctions to your dump's folders (the RomFS is
  about 5.7 GB), so keep the dump where it is. Only the executable modules are converted, into `build\dump`.
- **Fonts:** the game draws with the console's shared fonts in one place met so far, the map screen's legend and its
  travel-guide pages. Without `fonts\`, free fonts stand in: put Noto Sans (and Noto Sans SC, TC and KR for Chinese
  and Korean) as `.ttf` or `.otf` in `deps\fonts`; without those, Windows' own fonts (Segoe UI, Microsoft YaHei,
  Microsoft JhengHei, Malgun Gothic) are used. The log says which file each font type got.

## The steps

```
deps       check the Python packages, clang, the Vulkan SDK, SDL3 and the WebView2 SDK
import     your dump -> build/dump (modules as ELF, versions); RomFS and Help read in place   tools/extract/extract.py
functions  every function of each module, with exact boundaries                             tools/extract/functions.py
lift       AArch64 -> C for every function -> build/lift/main                               tools/lifter/recomp.py
compile    the lifted C -> objects (clang -O2, incremental)                                 tools/lifter/build_main.py
tables     generated headers and tables                                       tools/lifter/gen_tables.py, gen_sdktest.py
sdk        the system-library functions that are lifted too                                  tools/lifter/sdklift.py
link       host runtime + game library -> build/bin/smo_native.exe                           tools/lifter/build_exe.py
shaders    every shader stage of the game to SPIR-V, into build/cache/vk      tools/lifter/precompile_shaders.py
check      differential test against Unicorn, link check (on demand)        tools/lifter/difftest.py, linkcheck.py
```

A step whose output exists is skipped, so an interrupted setup resumes where it stopped. `compile`, `tables` and `link`
always run and only rebuild what changed. `--status` shows what is done, `--only <step>` runs one step, `--redo <step>`
repeats one, `--from <step>` repeats it and everything after it, `--check` adds the check step (minutes). After a change
to `tools/lifter/sdklift_rules.txt` run `--redo sdk`; after a change to the runtime, `--only link` is enough.

## Running

```bat
.venv\Scripts\python scripts\run.py play --exe smo_native.exe --seconds 0
```

- `smo_native.exe` (and `smo_user.exe`) open a visible window and play sound; `--seconds 0` means no time limit.
- Without `--exe` (the name is then `smo_test.exe`), or with any other name, the run is a test: `scripts\run.py`
  copies `smo_native.exe` to that name, keeps the window hidden (`SMO_SHOW=1` shows it) and silent, and stops the game
  after `--seconds` (default 30). Everything the game prints goes
  to `build\run\<run name>.log`, and a summary (boot stages reached, unimplemented system calls, frames presented, a
  crash with its host frames named) is printed when it ends.
- A test copy first waits while `SMO_MAX_GAMES` test games started this way run on the PC (default: one per 12 GB of
  memory, 2 on a 32 GB PC; `0` = no limit). A game in a stage takes 4-7 GB; more games than fit make the PC page,
  which slows every one of them and can make graphics memory allocations fail.
- `--userdata <folder>` gives the run its own save folder: a new, empty folder starts a new game, a copy of an earlier
  run's folder continues from there. Without it, saves go to `build\userdata\save`.
- `--seq <file>` plays an input sequence (see [`scripted-runs.md`](scripted-runs.md)); `--env NAME=VALUE` sets one of
  the runtime's switches (see [`debugging.md`](debugging.md)).
- Graphics and controller settings live in `build\config\graphics.ini` and `build\config\controllers.ini` (written
  with documented defaults on the first run; `controllers.ini` is read again when you edit it while the game runs).
- The window: by default a borderless window of the size in `graphics.ini`, centred (drag it anywhere to move it). F11,
  or a double-click on it, switches between that and borderless fullscreen; `window = borderless_fullscreen` starts that
  way, and `windowed` (with a title bar) and `fullscreen` (on top) are there too. The game keeps running while its
  window is not the active one or is minimised (pads and keyboard count only while it is active); `unfocused = pause`
  in `graphics.ini` stops it then instead, as the console stops a game behind its HOME menu.
- Help (the pause menu's Action Guide) opens inside the game window, scaled to it, and the game waits while it is open,
  as on the console: stick or d-pad and A to choose and open, L / R to turn pages, B to go back and to close (keyboard:
  arrows, Enter, Q / E, Escape). Its Choose Mode item leads to the game's Options, and back to the page when you
  leave them, as on the console.
- amiibo: press F1, or both stick buttons of a pad, while you play. The game waits and a menu like its pause menu lists
  the amiibo it knows; up / down (L / R: a page) to choose, A to scan, B (or F1 again) to close. When the game is
  waiting for an amiibo, → is held for you until it has read it; otherwise hold → yourself (the game's amiibo mode)
  and it reads the one you chose. The key and the buttons are set in `controllers.ini`, section `[hotkeys]`.
- The Capture button: F12, or a pad's share button, saves the frame as a BMP in the save folder's `album`.
- Translated shaders are cached in `build\cache\vk`. The `shaders` step compiles every stage of the game to SPIR-V
  there before the first run (compiling is nearly all of a stage's cost); what the game still meets for the first time
  (other variants, pipelines, the driver's own compilation) is done in play and reused by later runs.

The scripted Cap Kingdom run checks the game from a new save up to Mario's first capture:

```bat
.venv\Scripts\python tests\run_cap_kingdom.py <new run name>
```

It takes about 5 minutes with a warm shader cache and ends by itself once the capture's message is closed (give a
first run a larger `--seconds`, e.g. 1500); it exits with code 0 when every checkpoint was reached and every health
counter is zero.

## Troubleshooting

- **A tool is missing:** the `deps` step names it; install it or set the variable it names (`LLVM_ROOT`,
  `VULKAN_HEADERS`, `SDL3_ROOT`, `WEBVIEW2_ROOT`).
- **The import stops:** it names the piece it could not find or that is not what it should be (another game, another
  version, an encrypted file). Check the folder layout above.
- **The first minutes are slow, or loading screens pause:** shaders are being translated and compiled; the cache in
  `build\cache\vk` makes the next run faster.
- **"NOT IMPLEMENTED in the Vulkan back end, skipped" in the log:** an NVN feature the renderer does not have yet; the
  game keeps running, but that part of the picture is not the game's. Please report it with the log.
- **A crash:** the log has a `HOST EXCEPTION` block with the guest function, the registers and the host call stack
  (named from the linker map); `SMO_CRASH_DUMP` and `SMO_WATCH` give more (see [`debugging.md`](debugging.md)).
- **No new frame for 5 seconds:** a watchdog writes every thread's state to the log and to `<exe name>_freeze.txt`.
