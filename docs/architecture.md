# Architecture

*Super Mario Odyssey* runs here as a Windows program. Its AArch64 machine code is translated to C ahead of time and
compiled for x64; the parts of the console it talks to (operating system, graphics API, audio, input, files, system
applets) are provided by a host runtime written for this project. Nothing is interpreted at run time.

This page follows the build from your dump to the executable, then the runtime folder by folder.

## From the dump to an executable

`scripts/setup.py` runs these steps in order; each one writes under `build/` and can be repeated alone
(`--only <step>`).

| Step | Tool | What it does |
|---|---|---|
| import | `tools/extract/extract.py`, `nso.py` | Reads your decrypted dump in place. The program's modules (`main`, `sdk`, `subsdk0`, `rtld`) are NSO files: their segments are LZ4-decompressed into ELF images in `build/dump/update/elf`. Each piece is checked by what it contains: the program id in `main.npdm`, the NintendoSDK version `main` carries (7.3.2), the display version in `control.nacp` (1.3.0). `build/romfs` and `build/manual` become directory junctions to your dump, so the 5.7 GB RomFS is read where it is. |
| functions | `tools/extract/functions.py` | Every function of every module with exact boundaries, from the `.eh_frame` unwind tables and the sized symbols, never from guesses: 54,214 functions in `main`. Names come from the module's symbols and, for the game's state classes, from its RTTI (the 1.3.0 `main` keeps almost no symbols). |
| lift | `tools/lifter/recomp.py` | Each AArch64 function becomes one C function working on a register context (`Ctx`, `runtime/core/runtime.h`). Direct calls stay direct C calls; indirect calls go through the runtime's dispatcher; calls into other modules go to the runtime's handlers. Floating point follows the console's rules exactly (`fpexact.h`), and NEON vector instructions have C helpers (`neon.h`). |
| compile | `tools/lifter/build_main.py` | clang 9 compiles the lifted C (`-O2`, `RECOMP_OPT`; with a time limit per file that falls back to unoptimised code for the largest functions) into `smo_game.dll`. The lifted code is a DLL of its own so the runtime, which changes far more often, links in seconds. |
| tables | `tools/lifter/gen_tables.py` | Writes `build/gen/image.h`: the image layout, relocations, dynamic symbols, and the addresses of the few game functions the runtime calls or watches. The 1.3.0 `main` keeps almost no symbols, so each of those functions is found by a rule of its own from what it is and does: a class's virtual function from its vtable (found by the class's RTTI name), a game state's from its nerve's, the strings a function loads or passes to a call, the functions it calls and that call it. A rule must give exactly one function, else the entry is 0 and the runtime leaves that feature off. No game address is written in the runtime's sources. |
| sdk | `tools/lifter/sdklift.py` | Lifts parts of the game's own SDK module the same way (1,609 functions: the audio renderer and its DSP, allocators, the vibration player, ...). `tools/lifter/sdklift_rules.txt` chooses the roots and the points where the host answers instead. |
| link | `tools/lifter/build_exe.py` | Compiles the host runtime and links `build/bin/smo_native.exe`; builds `smo_glslang.dll` (GLSL to SPIR-V through the Vulkan SDK's shaderc). |
| check | `tools/lifter/difftest.py`, `linkcheck.py` | The lifter's differential test: sampled instruction shapes run both as lifted C and in the Unicorn emulator, from random, special floating-point, branch and trap states; registers, flags and memory must match (23,194 of 23,194). `difftest_mutation.py` checks that the test catches planted bugs. |

## The runtime

`runtime/` is C, compiled with clang 9. Each folder is one area of the console; files include each other's headers
by name (every folder is an include directory).

### `core/`: loading and calling

- `boot.c` runs the game's own start-up: `nninitStartup`, the static constructors, `nnMain`.
- `hostrt.c` maps the image at 0x7100000000 (addresses in comments are offsets from there), applies its relocations,
  makes code and constants read-only, and dispatches indirect calls (a hash table from guest address to lifted
  function) and calls to imported functions (by name, to the handler registered for it). On a crash it prints the guest
  function, its registers and the host call stack. `SMO_STRICT=1` lists the imports whose handler does nothing.
- `gamedll.c` and `game_shim.c` join the exe and `smo_game.dll`.
- `sdklift_rt.c` maps the SDK module beside the game, so lifted SDK code finds its own data.
- `hitch.c`: the hitch log (`SMO_HITCH`), which says which thread was late in a slow frame and inside what.

### `libc/`: the C and C++ runtime

The C library, the C++ standard library pieces the game imports, the C++ ABI runtime (exceptions, RTTI, guard
variables) and the unwinder that walks the guest stack with the game's unwind tables.

### `os/`: threads, time, memory

- `os_threads.c`: `nn::os` threads, events, message queues and the console's scheduling. The console has three cores
  and runs only the highest-priority ready thread of each; three processors of the PC stand for them, every guest
  thread keeps its core mask, and the game's priorities keep their order. The game depends on this: with free
  scheduling 4 of 6 boots failed in its heap code while loading, with the model 0 of 6.
- `nntime.c`: clocks, calendar time and time zones (from Windows, or the public tz database when present).
- `smo_nn.c`: the memory heap the game's allocators sit on (a pagefile-backed section, so graphics memory pools can
  show the same pages at a second address) and SDK functions no other table handles.
- `nnsdk.c`, `nnmisc.c`: smaller SDK families.

### `fs/`: files and saves

`nn::fs` on host folders. The RomFS is read-only from `build/romfs`; save data lives in the user folder and is
journalled like the console's: a commit writes the new files beside the old ones, then switches over, so a process
killed in either phase leaves a whole save.

### `gfx/`: graphics

- `nvn_stub.c` hands out an entry point for every NVN function the game asks for; `nvn_gl.c` is the front end:
  command lists are recorded on the game's threads and replayed by one render thread, and a recorded list lives until
  the game finalizes it or records over its memory, as on the console.
- The Vulkan back end, the default: `nvn_vk.c` (device, swapchain, present), `nvn_vk_draw.c` (draws, textures, copies,
  the stream ring that copies guest memory to the GPU), `nvn_vk_pipelines.c` (shader stages and pipelines, made ahead
  of the draw by worker threads), `nvn_vk_watchdog.c` (a report of every thread when no frame comes for 5 seconds).
  The OpenGL back end in `nvn_gl.c` is older and kept as a fallback (`renderer = opengl`, NVIDIA only).
- `mx_translate.c` translates Maxwell shader machine code into GLSL with structured control flow; `smo_glslang.dll`
  (`vk_shaderc_shim.c`) compiles it to SPIR-V. Translated stages are cached in `build/cache/vk`.
- `tex_decode.c`: NVN texture formats, the Tegra block-linear layout, BCn and ASTC decoding. `nvn_layout.c` answers the
  driver's texture size and alignment queries from the same layout rules.
- `display.c`: the window, and the one 60 Hz clock that paces both the game's vsync event and the presents.
  `gfx_options.c` reads `graphics.ini`.

### `audio/`

The game's audio renderer, including its DSP command processing, is lifted SDK code; `smo_audio.c` supplies what lies
below the SDK on a console (the renderer's clock and the output device, with a 6 to 2 channel downmix). `nnaudio.c`:
the answers to two audio-device imports, for a run in which the lifted SDK code does not replace them.

### `input/`

`nnhid.c` is `nn::hid`: controllers, motion, vibration, and the input sequences of scripted runs (`SMO_SEQ`).
`nnpad_sdl.c` reads the keyboard, XInput and every pad SDL3 knows, with hot-plug and a remapping file
(`controllers.ini`).

### `services/`: system services and applets

The offline console: web pages and Help (`nnapplet.c`), the NFC reader that amiibo are held to, the Capture button's
pictures in the album folder (`nnalbum.c`), sockets on Winsock without TLS (`nnnet.c`, `nnsocket.inc`), the online services answering
as a console without a network (`nnonline.inc`), the shared fonts (`nnpl.c`) and the font engine that draws them
(`nnfontll.c`), the movie player on Windows Media Foundation (`nnmovie_mf.c`). `nnservices.c` registers these families
and runs their self-test (`SMO_SDK_SELFTEST=1`).

### `game/`: features of this game

- `smo_nerve.c`: names the state each of the game's state machines is in (its "nerves", from RTTI), so scripted runs
  can wait for states and the log can show them; also starts stages and lists of stages for tests.
- `smo_amiibo.c`, `smo_ui.c`: the amiibo menu, drawn in software from the game's own layout textures, fonts and
  messages.
- `smo_guide.c`: the game's Help shown inside the game window (Microsoft Edge WebView2).
- `smo_fast.c`: host code in place of a hot game function (the sort of the sampler list), with a check mode that
  compares both.
- `smo_selftest.c`: `SMO_IMPORT_SELFTEST=1`, a check of the handlers written from the game's import list rather than
  met in play.

## Files at run time

The game runs from `build/bin`; its paths are relative to it:

| Path | What |
|---|---|
| `build/romfs`, `build/manual` | the game's files and its Help pages (junctions to your dump) |
| `build/sysdata/fonts` | the console's shared fonts, when you dumped them; otherwise free fonts from `deps/fonts`, then Windows' own |
| `build/config` | `graphics.ini` and `controllers.ini`, written with documented defaults on the first start |
| `build/userdata` | save data and the album; `scripts/run.py --userdata` points a run at another folder |
| `build/cache/vk` | translated shader stages and the driver's pipeline cache |
| `build/run` | logs of runs started with `scripts/run.py` |

## Reading the sources

Comments say where a behaviour comes from:

- `[game 0x...]`: an offset in the game's `main`; `[sdk 7.3.2 0x...]`: in the game's SDK module.
- `[sdk]` with NintendoSDK 3.5.1, where the file says so: the base game's SDK module.
- `[doc]`, `[suyu]`: public documentation and emulator sources, by file.
- Files adapted from [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) keep the addresses that project read in
  *Super Smash Bros. Ultimate* 13.0.4's modules, and their headers say so. Addresses of the form 0x71xxxxxxxx above
  0x7102000000 are always from there: this game's `main` is smaller.
- A number in a comment comes from a measurement or a test, which the comment names.
