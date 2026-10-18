# Building from a fresh clone

Everything the build makes from the game is derived from **your own dump** on **your own PC**, into `build/` (ignored by
git). One script does all of it, in resumable steps:

```bat
.venv\Scripts\python scripts\setup.py --dump <folder with your NSP>
```

## What you need

- Windows 10/11 x64 and a GPU with Vulkan 1.3 (developed on NVIDIA). The renderer uses
  `VK_EXT_graphics_pipeline_library`, `VK_EXT_extended_dynamic_state3`, `VK_EXT_vertex_input_dynamic_state` and
  `VK_EXT_depth_clip_control`.
- **Python 3.13** in a virtual environment in the repository:
  ```bat
  py -3.13 -m venv .venv
  .venv\Scripts\python -m pip install pycryptodome lz4 zstandard capstone unicorn numpy Pillow pyyaml
  ```
- **LLVM/clang 9.0.0** (the lifted code is compiled with it): unpack it to `C:\llvm-9.0.0` or point `LLVM_ROOT` at it.
- **Visual Studio 2019 Build Tools** (16.11) with the Windows SDK 10.0.19041: the C runtime and system libraries clang
  links against.
- The **Vulkan SDK** 1.4: its headers and `vulkan-1.lib`, and its `shaderc_shared.dll`, which `smo_glslang.dll` (built
  from `runtime/vk_shaderc_shim.c`) uses to turn GLSL into SPIR-V. `VULKAN_HEADERS` overrides the header folder.
- **SDL3** 3.4.18 (the development package for Visual C++) in `deps\SDL3-3.4.18`, or `SDL3_ROOT` pointing at it.
- The **WebView2 SDK** (Microsoft's NuGet package `Microsoft.Web.WebView2`, developed with 1.0.2903.40; a `.nupkg` is
  a zip file): its `build\native\include` and `build\native\x64` folders in `deps\webview2-<version>\build\native\`, or
  `WEBVIEW2_ROOT` pointing at the unpacked package. The build copies its `WebView2Loader.dll` next to the exe. The game's
  Help (its Action Guide) is shown in the game window with it; the **WebView2 Runtime** it needs is part of Windows 11
  (on Windows 10, install Microsoft's Evergreen WebView2 Runtime). Without the runtime, Help opens in your web browser.

`scripts\setup.py` checks for each of these first and names what is missing.

## What you dump from your own console

| What | Used for |
|---|---|
| *Super Mario Odyssey* 1.0.0, the base package: an `.nsp` with `[0100000000010000]` in its name | the executable (`main`), the system library it ships (`sdk`), the RomFS |
| `prod.keys` from your console | decrypting the package, read in place. Found from `--keys`, `%SMO_KEYS%`, `~\.switch\prod.keys`, or Ryujinx's folder |
| your console's firmware (`.nca` files) | the system's shared fonts (`--fw <folder>`; default: the firmware installed in Ryujinx's folder) |
| an update package `[0100000000010800]` (optional) | `--target 1.4.1` builds that version instead (see below); without it the build is 1.0.0 |

Put the packages in one folder and give it as `--dump`. Nothing is copied out of them except what the build derives,
into `build/`.

**Optional:** a local clone of [OdysseyDecomp](https://github.com/MonsterDruide1/OdysseyDecomp), given as
`--decomp <folder>`. Its function list adds names to `build/functions` (for the debugging tools) and checks that your
`main` is the 1.0.0 executable; the state names the scripted runs wait for also come from the game's own RTTI, without
it. Nothing of it is copied into the repository.

## The steps

```
deps       check Python packages, clang, the Vulkan SDK and SDL3
extract    dump -> ExeFS, main.elf / sdk.elf, NACP, version.json        tools/extract/extract.py
romfs      the game's RomFS -> build/romfs (about 5.1 GB)               tools/extract/romfs.py
manual     the game's electronic manual (Help) -> build/manual          tools/extract/romfs.py --manual
fonts      the system's shared fonts from your firmware                 tools/extract/fw_fonts.py
functions  every function of main, with exact boundaries                tools/extract/functions.py
lift       AArch64 -> C for every function -> build/lift/main          tools/lifter/recomp.py
compile    the lifted C -> objects (clang -O1, incremental)            tools/lifter/build_main.py
tables     generated headers and tables                                 tools/lifter/gen_tables.py, gen_sdktest.py
sdk        the system-library functions that are lifted too             tools/lifter/sdklift.py
link       host runtime + game library -> build/bin/smo_native.exe      tools/lifter/build_exe.py
check      differential test against Unicorn, link check (on demand)   tools/lifter/difftest.py, linkcheck.py
```

A step whose output exists is skipped, so an interrupted setup resumes where it stopped. `compile`, `tables` and `link`
always run and only rebuild what changed. `--status` shows what is done, `--only <step>` runs one step, `--redo <step>`
repeats one, `--from <step>` repeats it and everything after it, `--check` adds the check step (minutes). After a change
to `tools/lifter/sdklift_rules.txt` run `--redo sdk`; after a change to the runtime, `--only link` is enough.

## The update (1.4.1)

With the update package in the dump folder, `--target` builds the game as the update leaves it, beside 1.0.0:

```bat
.venv\Scripts\python scripts\setup.py --dump <folder> --target 1.4.1
.venv\Scripts\python scripts\run.py <run name> --target 1.4.1 --seconds 300
```

- Everything goes to `build\1.4.1\`, laid out as `build\` is; the extracted packages in `build\dump` are shared.
- The update's RomFS is a patch of the base's: the `romfs` step applies it and can check every block against the
  update's own hash tree (`tools\extract\romfs.py --update --verify`). Files the update did not change are hard links
  to `build\romfs` when that exists, so the second copy takes little space; `build\1.4.1\romfs_changes.txt` lists what
  changed.
- The update's `main` has almost no symbols; the function list names the game's states from its RTTI instead, and the
  few functions the runtime replaces with host code are found by matching them with the base executable's.

## Running

```bat
.venv\Scripts\python scripts\run.py <run name> --seconds 300
```

- The game runs from `build\bin` for the given time; everything it prints goes to `build\run\<run name>.log`, and a
  summary (boot stages reached, unimplemented system calls, frames presented, a crash with its host frames named) is
  printed when it ends.
- Only `smo_native.exe` and `smo_user.exe` open a visible window and play sound (`--exe smo_native.exe` to play);
  other names, used for tests, keep the window hidden (`SMO_SHOW=1` shows it) and silent.
- A test run first waits while `SMO_MAX_GAMES` test games started this way run on the PC (default: one per 12 GB of
  memory, 2 on a 32 GB PC; `0` = no limit). A game in a stage takes 4-7 GB; more games than fit make the PC page,
  which slows every one of them and can make graphics memory allocations fail.
- `--userdata <folder>` gives the run its own save folder: a new, empty folder starts a new game, a copy of an earlier
  run's folder continues from there. Without it saves go to `build\userdata\save`.
- `--seq <file>` plays an input sequence (see [`docs/SCRIPTED_RUNS.md`](docs/SCRIPTED_RUNS.md)); `--env NAME=VALUE`
  sets one of the runtime's switches (see [`docs/DEBUGGING.md`](docs/DEBUGGING.md)).
- Graphics and controller settings live in `build\config\graphics.ini` and `build\config\controllers.ini` (written
  with documented defaults on the first run; `controllers.ini` is read again when you edit it while the game runs).
- The window: by default a borderless window of the size in `graphics.ini`, centred (drag it anywhere to move it). F11,
  or a double-click on it, switches between that and borderless fullscreen; `window = borderless_fullscreen` starts that
  way, and `windowed` (with a title bar) and `fullscreen` (on top) are there too.
- Help (the pause menu's Action Guide) opens inside the game window and the game waits while it is open, as on the
  console: stick or d-pad and A to choose and open, L / R to turn pages, B to go back and to close (keyboard: arrows,
  Enter, Q / E, Escape).
- amiibo: press F1, or both stick buttons of a pad, while you play. The game waits and a menu like its pause menu lists
  the amiibo it knows; up / down (L / R: a page) to choose, A to scan, B (or F1 again) to close. When the game is
  waiting for an amiibo, → is held for you until it has read it; otherwise hold → yourself (the game's amiibo mode)
  and it reads the one you chose. The key and the buttons are set in `controllers.ini`, section `[hotkeys]` (a file
  written before this section existed keeps working with these defaults).
- Translated shaders are cached in `build\cache\vk`. The first run compiles them as the game meets them and is slower;
  later runs reuse them.

The scripted Cap Kingdom run, which checks the game from a new save up to Mario's first capture:

```bat
.venv\Scripts\python tests\run_cap_kingdom.py <new run name>
```

It takes about 12 minutes with a warm shader cache (give a first run a larger `--seconds`, e.g. 1500) and exits with
code 0 when every checkpoint was reached and every health counter is zero.

## Troubleshooting

- **A tool is missing:** the `deps` step names it; install it or set the variable it names (`LLVM_ROOT`,
  `VULKAN_HEADERS`, `SDL3_ROOT`).
- **Keys not found:** give `--keys <your prod.keys>`; the extraction prints which file it used.
- **The first minutes are slow, or loading screens pause:** shaders are being translated and compiled; the cache in
  `build\cache\vk` makes the next run faster.
- **"NOT IMPLEMENTED in the Vulkan back end, skipped" in the log:** an NVN feature the renderer does not have yet; the
  game keeps running, but that part of the picture is not the game's. Please report it with the log.
- **A crash:** the log has a `HOST EXCEPTION` block with the guest function, the registers and the host call stack
  (named from the linker map); `SMO_CRASH_DUMP` and `SMO_WATCH` give more (see `docs/DEBUGGING.md`).
- **No new frame for 5 seconds:** a watchdog writes every thread's state to the log and to `<exe name>_freeze.txt`.
