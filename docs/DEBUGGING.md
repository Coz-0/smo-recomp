# Debugging aids

The runtime's switches are environment variables with the `SMO_` prefix; pass them to a run with
`scripts\run.py <run name> --env NAME=VALUE`. Everything a run prints goes to `build\run\<run name>.log`.

## The log at a glance

The end of every log has the counts that tell whether a run was healthy:

| Line | Healthy |
|---|---|
| `nvn_vk: N frames presented, ... 0 Vulkan call failures, ... 0 calls of operations that are not implemented` | both zero. An operation the renderer lacks is named once (`NOT IMPLEMENTED in the Vulkan back end, skipped`), counted here and skipped; `SMO_VK_MISSING=exit` stops the game at the first one instead |
| `nvn_vk: images: ...; draws with a geometry stage: ...; indirect: ...` | how often the less common paths ran (storage images, geometry stages, indirect draws and dispatches) |
| `nvn_gl: ... command lists (0 unknown handles) ...` | zero: a non-zero count means draws were dropped (scenery missing or flickering) |
| `nvn: command lists freed: ...` | lists freed when the game finalizes them or records over them; `by the budget` should be 0 |
| `audio: device sink: ... 0 samples dropped ..., 0 underruns ...` | both zero |
| `HOST EXCEPTION` | absent |
| `imports reached ... unimplemented imports reached:` | system calls the game made that have no implementation yet |

## Pictures

- `SMO_SHOT=<n>` saves frame n; `SMO_SHOT_EVERY=<k>` saves every k-th frame from `SMO_SHOT_FROM=<frame>` on (up to
  `SMO_SHOT_TO=<frame>` when given), as `frame_<n>.bmp` in `SMO_SHOT_DIR=<folder>` (the folder must exist).
- `SMO_VK_PASSSHOT=<frame>` saves colour target 0 at the end of every render pass of that frame
  (`pass_<n>_<format>.bmp`) and logs every draw of it: programs, blend and depth state, viewport, vertex attributes and
  the texture behind every binding. `SMO_VK_PASSSHOT=<first>-<last>/<step>` does it for several frames
  (`pass_<frame>_<n>_<format>.bmp`), for defects that show in some frames only. `SMO_VK_PASSSHOT_RAW=1` adds the
  texels as read, for exact values.
- `SMO_VK_DIAG_NODEPTH=1` draws without depth testing (to see whether something is hidden or missing).
- `SMO_CAPTURE_AT=<frame>[,<frame>...]` presses the Capture button at those frames (the picture goes to the save
  folder's `album`, as when you press F12); `SMO_CAPTURE_GS=<n>` presses it at n frames that drew with a geometry stage
  (the lightning), at least 60 frames apart.
- `SMO_AMIIBO_SHOT_DIR=<folder>` saves the first three different pictures of each opening of the amiibo menu as
  `amiibo_menu_<opening>_<n>.png` (the menu's frames are not game frames, so `SMO_SHOT` does not see them).

## Shaders

- Translated stages are cached in `build\cache\vk` (`SMO_VK_CACHE=<folder>` uses another one; an empty folder forces
  every stage to be translated again).
- `SMO_VK_GLSLDUMP=<folder>` writes the GLSL of every stage translated in the run.
- `SMO_SHADERDUMP=<folder>` writes the machine code of stages that could not be translated (`vk_fail_*.bin`); with
  `SMO_VK_DUMP_MINBYTES=<n>` (stages whose GLSL is at least n bytes) or `SMO_VK_DUMP_SERIAL=<n>` (one program; serials
  change from run to run) it writes `vk_prog_*.bin` for the equivalence harness. `tools\dev\mxdis.py <file.bin>` names
  the instructions of a dump.
- `SMO_MX_NOSTRUCT=1` translates with the old `switch(pc)` control flow instead of structured blocks;
  `SMO_MX_MUTATE=1` plants a known bug, to check that the equivalence harness catches it.
- Equivalence harness: `tests\build_mx_equiv.py`, then from `build\bin`: `mx_equiv.exe <folder of vk_prog_*.bin> [max]`.
- `tests\check_all_shaders.py` takes every shader stage out of the game's shader archives (agl `.sharcb`, g3d `.bfsha`,
  the effect `.ptcl`, nested archives, `main` and `sdk`) into `build\shaders_all`, translates each with the Vulkan back
  end's settings and compiles it with the renderer's compiler; `--list` adds a line per stage, `--extract-only` stops
  after the extraction. A geometry stage is checked for point, line and triangle input.
- `SMO_VK_GS_DEBUG=<mode>` for draws with a geometry stage: `1` measures them (`nvn_vk_gs:` lines: the GPU's pipeline
  statistics of 16 of them, and the colour target read back before and after every 20th, which says how many pixels
  each draw brightened and by how much), `2` leaves them out (to compare pictures), `3` draws them in bright magenta
  without the depth test and measures them as 1 does.

## Missing or flickering draws

- Check the `unknown handles` count first. `SMO_CMDMEMLOG=1` logs the command memory the game hands to its command
  buffers (the first 400 times), which decides when a recorded list is over.
- `SMO_LIST_BUDGET_MB=<n>` (default 4096) is a last-resort limit on recorded command lists; when it drops lists it says
  so, because the game may still submit them.
- `SMO_TEXLOG=1` logs texture creation, finalization and registration; the per-draw log of `SMO_VK_PASSSHOT` says why
  a texture handle is unknown (slot outside the pool, never registered, or a texture that was finalized).
- Flicker that shows in single frames: save every frame of a stretch (`SMO_SHOT_EVERY=1 SMO_SHOT_FROM=<a> SMO_SHOT_TO=<b>
  SMO_SHOT_DIR=<folder>`), then `tools\dev\flicker.py <folder>` lists the frames where a region of the picture is brighter
  or darker than in both neighbours, and saves them side by side. To see what such a frame drew differently, run the same
  stretch again with `SMO_VK_PASSSHOT=<a>-<b> SMO_VK_PASSSHOT_LOGONLY=1 SMO_VK_FRAMELOG=<a>-<b>` (every draw logged,
  no pass pictures; a lot of log) and `tools\dev\drawdiff.py build\run\<name>.log <frame>`: the draws the frame has that
  neither neighbour has, and the ones it lacks.
- `SMO_VK_FRAMELOG=<first>-<last>`: one `nvn_vk_frame:` line per frame (draws, draws left out and why, conditional
  renders, render passes, texture barriers, stream ring wraps and kilobytes streamed).

## Speed

- Every 300 frames the log has `nvn_perf: frames A..B frame time ms: ...` (frame times, fps, 1% low), `nvn_perf: cpu
  ms/frame by thread` and `nvn_vk_perf:` (where the render thread's time went: batch replay, draw preparation, waits for
  the GPU, present). `nvn_texhash:` says how many textures were checked for changes by hashing their memory, and why.
- `SMO_HOSTPROF=<start s>,<seconds>[,<threads>][;<start s>,<seconds>[,<threads>] ...]` samples the stacks of the
  busiest threads (default 4) about every millisecond in each window and prints the functions met most often, what
  each thread was blocked in, and under which function of the game. `tools\dev\profnames.py build\run\<name>.log` adds
  the names of the lifted game's functions (`guest f_...`) from the function list.
- `SMO_FRAMETL=<first frame>,<frames>` records a timeline of those frames (every job the game's threads queue for the
  render thread and when it ran, the submissions to the GPU and the waits for it, the presents, the game's waits for the
  refresh and the refreshes, each with its thread and processor) into `SMO_FRAMETL_OUT=<file>` (default `frametl.txt`
  next to the exe). `tools\dev\frametl.py <file>` makes a table of it per frame: when the batch was submitted, how long
  the replay and the present took and how much of it was waiting for the GPU, how long the main thread drew after the
  present, and how far ahead of the next refresh it was done; the slow frames are listed.
- `nvn_vk_ring:` (every 300 frames) says what the stream ring copied per frame, how often its range caches were dropped
  (`new generations`) and which writes of the host into guest memory were checked against them instead.
- `tools\dev\cpuload.py --busy <n> --seconds <s> --log <file>` keeps n processes busy (arithmetic and memory copies) and
  logs the CPU use of the system, the game and the busiest other programs every 5 seconds: frame rates measured while
  other programs use the PC, the same way each time.
- `SMO_PRIORITY_CLASS=normal` runs the process in the NORMAL priority class (default ABOVE_NORMAL: the game's threads
  are tied to three processors and must come before other programs' threads there; `runtime/os_threads.c`).
- `SMO_FAST=0` leaves the game's own code where `runtime/smo_fast.c` puts host code in its place (the sort of the
  sampler list of `agl::driver::NVNMgr`); `SMO_FAST_CHECK=1` runs the game's code as well and stops the game if the
  results ever differ.
- Switches that bring back an older behaviour, for comparisons: `SMO_VK_OCC_SYNC=1` (every occlusion report waits for the
  GPU), `SMO_VK_OCC_PRESENT=0` (occlusion results are not waited for at the present), `SMO_VK_RING_DRAIN=1` (the stream
  ring waits for the GPU at every wrap), `SMO_TEXHASH_ALL=1` (textures are hashed whatever their memory pool),
  `SMO_VK_NO_TEXBARRIER=1` (the game's fragment-order barriers are ignored), `SMO_SYNC_SUBMIT=1` (every submit waits for
  the render thread), `SMO_VK_VTXGROW=0` (vertex streams are copied only as far as the range the game bound, even when
  the draw reads further; `SMO_VK_VTXCHECK=1` logs each kind of draw that does). `SMO_PRESENT_ASYNC=1` lets the present not wait for the render thread (one frame in flight);
  `SMO_VK_OCC_LOG=1` logs the conditional renders that had to wait for an occlusion result.

## Crashes and stalls

- A crash prints `HOST EXCEPTION` with the last guest function, its registers and the host call stack, named from the
  linker map next to the exe. `SMO_CRASH_DUMP=<bytes>` (0x60 to 0x1000) adds memory at every register, the strings it
  finds there and the guest's frame-pointer chain (the real guest call stack).
- `SMO_WATCH=<heap offset>[,<length>]` logs every write that changes that range of the game's heap, with the writing
  function (heap offsets repeat from run to run).
- No presented frame for 5 seconds writes every thread's state (locks, waits, call stacks) to the log and to
  `<exe name>_freeze.txt` (`SMO_VK_WATCHDOG=<seconds>`, 0 = off). `SMO_VK_BARRIER_DUMP=1` prints every thread's stack
  when a loading barrier times out.

## The game's state

- `SMO_NERVELOG=<substrings>` prints every change of state of the game's state machines whose state name contains one of
  them (`*` = all): `nerve: frame F keeper K: OpeningStageStartDemoNrvCameraAuto -> OpeningStageStartDemoNrvCameraTutorial`.
- `SMO_ACTORPOS=<n>[:<state name prefixes>]` prints actor positions every n frames (default: the player).
- `SMO_ICALLS=<start second>,<seconds>` counts the targets of indirect calls in that window and prints them at the time
  limit; map the `f_71...` addresses with `build\functions\main_functions.tsv` (address - 0x7100000000). Every state
  machine runs its current state once a frame through such a call, so the active one stands out.
- See [`SCRIPTED_RUNS.md`](SCRIPTED_RUNS.md) for waiting on states and steering in input sequences.

## Audio and input

- `SMO_AUDIO=0` turns the audio output off; `SMO_AUDIO_WAV=<file>` also writes the final mix to a WAV file.
- `SMO_HIDLOG=1` logs the pad values the game reads, when they change.
- `SMO_PAD_SELFTEST=1` runs the controller self-test with SDL3 virtual devices and exits.
- amiibo: the log says when the menu opens and closes (`amiibo menu:`), what the NFC reader does (`nfp:` an amiibo held to
  it, found when the game searches, its model information read by the game, taken away) and whether → is held for the
  game (`nn::hid:`). `SMO_NERVELOG=HelpAmiibo` shows the game's amiibo mode (Wait, CountHold, Active, IconClose).

## Starting elsewhere in the game

- `tools\dev\save_warp.py <save folder> --out <new folder> --stage <StageName> [--world <n>]` copies a save folder so
  that its game file continues in another stage (at the stage's default entrance; the world number comes from
  `SystemData\WorldList.szs` for a kingdom's home stage, e.g. `BossRaidWorldHomeStage` = 11). Run it with
  `scripts\run.py <name> --userdata <new folder>`: a late kingdom can be tried without playing to it. The source folder is
  only read; never give your own save folder as `--out`.

## Other switches

- `SMO_IMPORT_SELFTEST=1` checks the system calls implemented from the list of the game's imports rather than met in
  play (C library, threads and message queues, files, C++ library pieces, `nn::util`, diagnostics and time, the web
  applet, the NFC reader with an amiibo held to it, the amiibo menu's list and picture): run when every handler is
  registered, before the game starts; one line per group, then it exits with the number of failed checks as exit code.
  `SMO_AMIIBO_SHOT=<file.png>` also saves the menu picture it composes.
- `SMO_MANUAL=<folder>` takes the game's manual from that folder instead of `build\manual`; `SMO_WEB_NOOPEN=1` prepares
  the Help pages without opening the browser.
- `SMO_STRICT=1` stops at the first call to a system call that only has a placeholder handler; `SMO_STRICT=report`
  lists them without stopping.
- `SMO_VK_CSLOG=1` logs compute dispatches with large work groups.
- `SMO_NACP=<file>` reads the version string from another `control.nacp`.
- `SMO_USERDATA=<folder>` is the save folder (`scripts\run.py --userdata` sets it).
- `SMO_SDK_SELFTEST=1` runs the system-library self-test inherited from ssbu-recomp (it stops at `log2f`, which this
  game does not import).

## Looking things up in the executable

- `tools\dev\disasm.py <symbol or address> [--module main|sdk]` disassembles one function, naming call targets.
- `tools\dev\callers.py <name substring>` lists the callers of a function or import.
- `tools\dev\sarc.py <romfs path>` lists the files in a SARC archive of the extracted RomFS;
  `tools\dev\byml.py StageData\<Stage>Map.szs --scenario 0 --grep <name>` lists the objects placed in a stage, with
  their positions (prints to the console only).
- `tools\lifter\mapaddr.py` names host addresses from the linker map.
