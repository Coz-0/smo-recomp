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
  `SMO_SHOT_PLAY=<n>` saves the n-th play frame (a frame with compute dispatches and more than 120 draws) as
  `play_<n>.bmp`: the same moment of play whatever the loading took.
- Recording a video: `SMO_SHOT_RANGES=<first>-<last>[/<every>][,...]` saves the frames of several ranges and logs each
  as `shot: frame <n> audio <sample>`, where that frame falls in the `SMO_AUDIO_WAV` capture, so picture and sound can
  be put together. A saved frame normally waits for the GPU (about 32 ms); with `SMO_SHOT_ASYNC=1` it is copied on the
  GPU and written by a thread, and the game keeps its pace (measured: 35 ms per two frames while saving every second
  one, against 65 ms); a frame that finds no free copy buffer is logged as left out.
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
- `SMO_GUIDE_SHOT=<file.png>` saves the Help page as shown in the game window (`runtime/game/smo_guide.c`, WebView2) 1.5 s
  after it loaded and 0.8 s after each pad action, as `<file>_2.png`, `_3.png`, ... (at most 8); `SMO_SHOT` does not see
  it either. The log says what the guide does (`guide:`).

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
- Graphics memory: the `nvn_vk: totals` line (every 600 frames) ends with the device memory the renderer has allocated
  and the images of textures nothing names any more that were freed (agl gave up every texture pool slot of the
  texture and the object that made it holds another one; `runtime/gfx/nvn_gl.c`, above `pool_ent_release`). Such an image
  goes after `SMO_VK_TEXFREE=<frames>` unused frames (default 8; `0` keeps every image, as before). `made again` counts
  textures used after their image went (what the GPU had drawn into one is lost then; it should stay 0, and
  `nvn_vk_tex:` lines name them). With `SMO_TEXLOG=1` every release of a slot and every freed image is logged too.
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
  the GPU, present), `nvn_vk_hostmem:` (vertex and index data read in place from the game's heap or streamed, waits
  before a write of the host into it), `nvn_vk_ring_kinds:` (what the stream ring copied a frame: uniform, storage,
  vertex, index, other), `nvn_vk_vstage:` (stage lookups that missed the stage's last variant) and, when there were,
  `nvn_vk_sync:` (fence waits that found compute results not yet in guest memory and landed them first); every 600
  frames `copy-backs forced by` counts each reason a copy-back of compute results was forced, and the GPU waits it cost.
  `nvn_texhash:` says how many textures were checked for changes by hashing their memory, and why.
- Speed switches, each bringing back the way before (for comparisons): `SMO_VK_HOSTMEM=0` (vertex and index ranges
  copied into the stream ring instead of read in place from the game's heap, imported into Vulkan with
  VK_EXT_external_memory_host), `SMO_VK_TSET_CACHE=0` (texture descriptor sets per command buffer instead of kept
  across them), `SMO_VK_OCC_LATE=0` (a conditional render waits for the occlusion count it compares instead of using the
  last frame's), `SMO_VK_RT_UPLOAD=1` (a render target the game makes again is read from guest memory),
  `SMO_VK_TEXAHEAD_LOAD=0` (no larger texture budget during loading screens), `SMO_VK_DYN_ALL=1` (every dynamic state
  sent at every draw), `SMO_FAST_SKIP=0` (registerSampler's free-slot walk one lifted call per slot), `SMO_HOST_PIN=0`
  (the render thread, the pipeline workers and the renderer's helper threads anywhere instead of away from the console
  cores; `SMO_HOST_CPUS=<hex mask>` chooses their processors, `SMO_HOST_LAYOUT=1` gives the render thread a core of its
  own). `SMO_VK_MT=1` (off by default) runs the look-ahead over a batch on a thread of its own beside the replay. The
  build: `RECOMP_OPT` (lifted code) and `RECOMP_HOST_OPT` (runtime), both `-O2` by default; `RECOMP_ARCH` adds target
  flags (e.g. `-march=znver2`: the objects then run on that CPU family and later ones only). A change of them recompiles
  nothing by itself: `tools\lifter\build_main.py --force`, or delete `build\obj\host`.
- `nvn_vk_frames:` at exit counts the frames that drew a scene, i.e. had compute dispatches (in the Cap Kingdom run 40,164
  of 42,589; the others are boot and loading frames). Within 600 frames of such a frame the renderer does not hold the
  game's threads for its workers and makes only cheap textures ahead of their use; in a frame that draws a scene its
  workers run below normal priority.
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
  are tied to three processors and must come before other programs' threads there; `runtime/os/os_threads.c`).
- `SMO_FAST=0` leaves the game's own code where `runtime/game/smo_fast.c` puts host code in its place (the sort of the
  sampler list of `agl::driver::NVNMgr`); `SMO_FAST_CHECK=1` runs the game's code as well and stops the game if the
  results ever differ. The game's `main` has no symbols for these functions: `tools/lifter/gen_tables.py` finds them
  by what they do (`find_sampler_sort`), and the log says so when the fast path stays off.
- Switches that bring back an older behaviour, for comparisons: `SMO_VK_OCC_SYNC=1` (every occlusion report waits for the
  GPU), `SMO_VK_OCC_PRESENT=0` (occlusion results are not waited for at the present), `SMO_VK_RING_DRAIN=1` (the stream
  ring waits for the GPU at every wrap), `SMO_TEXHASH_ALL=1` (textures are hashed whatever their memory pool),
  `SMO_VK_NO_TEXBARRIER=1` (the game's fragment-order barriers are ignored), `SMO_SYNC_SUBMIT=1` (every submit waits for
  the render thread), `SMO_VK_VTXGROW=0` (vertex streams are copied only as far as the range the game bound, even when
  the draw reads further; `SMO_VK_VTXCHECK=1` logs each kind of draw that does). `SMO_PRESENT_ASYNC=0` makes the present wait for the render thread again (by default it does not: one frame in flight);
  `SMO_VK_OCC_LOG=1` logs the conditional renders that had to wait for an occlusion result.

## Memory

- Every 300 frames the log has `nvn_vk_mem: frame F:` the process's working set (its private part and peak), private
  bytes, the commit of pagefile-backed sections (the game's memory heap), the Vulkan memory the renderer allocated (the
  host-visible part is system memory the driver keeps resident) and the shader modules made, released and made again.
- `tools\dev\memmap.py <pid or exe name> [--log build\run\<name>.log] [--zero 64] [--json <file>]` reads a
  running game from outside (nothing is written into it): every allocation with its reserved, committed and resident
  size, named where it can be (images and mapped files, thread stacks with their start function from the exe's map, NT
  heap segments and large blocks, the guest memory heap and NVN virtual pools from the log, Vulkan memory blocks with
  `SMO_VK_MEMLOG=1`). `--zero` adds, for the large allocations, how much of what is resident holds only zeros and where
  in the allocation it lies.
- Where a game's memory goes (Sand Kingdom home stage, frame 6000 of a scripted run): the game's own memory heap
  (2.2 GB resident of the 3.05 GB the game asks for; 92% of it non-zero data), the graphics driver's heaps (stage
  libraries and pipelines, the pipeline cache object: 640 MB), the stream ring (256 MB of resident system memory), the C
  runtime heap (250 MB), executables and DLLs (100 MB). Graphics memory adds commit, not resident memory: about as much
  as the Vulkan memory, for the driver's eviction backing.
- `SMO_VK_MODREL=0` keeps every shader module (by default a stage's module is released once its pipeline library or
  compute pipeline is made, and made again from the stage cache's SPIR-V file if needed: 1.1 GB less).
- `SMO_VK_PCACHE=0` makes pipelines without a pipeline cache object (nothing read or saved): 640 MB less, as fast for an
  exe the driver has run before, but the NVIDIA driver keeps its own shader cache per executable, so an exe under a new
  name compiles everything again.

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
- See [`scripted-runs.md`](scripted-runs.md) for waiting on states and steering in input sequences.

## Audio and input

- `SMO_AUDIO=0` turns the audio output off; `SMO_AUDIO_WAV=<file>` also writes the final mix to a WAV file.
- Test copies (any exe name but `smo_native.exe` and `smo_user.exe`) are silent: their volume in the Windows mixer is
  0. The sound is still made and fed to the device as with sound, so the timing, the dropped-sample and underrun counts
  and an `SMO_AUDIO_WAV` capture are the same. `SMO_VOLUME=<0..100>` sets the volume of any copy (the log says
  `host volume N%`).
- `SMO_HIDLOG=1` logs the pad values the game reads, when they change.
- `SMO_PAD_SELFTEST=1` runs the controller self-test with SDL3 virtual devices and exits.
- amiibo: the log says when the menu opens and closes (`amiibo menu:`), what the NFC reader does (`nfp:` an amiibo held to
  it, found when the game searches, its model information read by the game, taken away) and whether → is held for the
  game (`nn::hid:`). `SMO_NERVELOG=HelpAmiibo` shows the game's amiibo mode (Wait, CountHold, Active, IconClose).

## Starting elsewhere in the game

- `tools\dev\save_warp.py <save folder> --out <new folder> --stage <StageName> [--world <n>] [--scenario <n>]
  [--checkpoint <object id> --visited]` copies a save folder so that its game file continues in another stage (at the
  stage's default entrance, for a kingdom the Odyssey; the world number comes from `SystemData\WorldList.szs` for a
  kingdom's home stage, e.g. `BossRaidWorldHomeStage` = 11). `--scenario` sets the kingdom's scenario (which objects
  its stages place); `--checkpoint` starts at one of the stage's checkpoint flags (`tools\dev\byml.py
  StageData\<Stage>Map.szs --grep CheckpointFlag` lists them; the flag is marked as reached in the copy, a flag in a
  zone is named `<id>(<zone>[<the zone's id in the stage>])`), `--reached <object id>` marks more flags as reached
  without starting at them, and `--visited` lets it apply in a kingdom the save has
  not been to (a first visit starts at the Odyssey). Run it with `scripts\run.py <name> --userdata <new folder>`: a late
  kingdom can be tried without playing to it. The source folder is only read; never give your own save folder as
  `--out`.
- A save always resumes in a kingdom's home stage. `SMO_START_STAGE=<stage>[:<scenario>]` (TEST ONLY) makes the first
  stage scene the game starts that stage instead, at its default start point, as when Mario goes through its entrance:
  any sub-area, room or 2D section (the log says `start stage: frame F: the game starts <home>, scenario N;
  SMO_START_STAGE makes it <stage>, scenario M (<whose>)`). Give it a save warped to the stage's kingdom. The runtime
  watches for `StageScene::init` (found in the game's own vtable, `GAME_VSYM_StageScene_init` in `build\gen\image.h`)
  and changes the stage name it is called with, once; later stage scenes are only logged (`the game starts <stage>,
  scenario <n> itself`, e.g. the restart after a miss). The scenario: the one given (`asked for`), else the stage's own
  when the game has one for it (`the stage's own, as through its door`: what the game's own `ChangeStageInfo` gives a
  stage when no scenario is asked for, as the doors to sub-areas do; 1 for a kingdom's challenge rooms), else the
  kingdom's as the game chose it (`the kingdom's`). A stage that follows the kingdom's scenario can still be started in one whose home map has no door to it (the Mushroom Kingdom shop in
  scenario 1 shows the game's unbaked placeholder lighting): choosing such a scenario is not done yet. Starting a
  cutscene stage this way, out of its order, can meet game code that expects the story to be where that cutscene
  belongs.
- `SMO_STAGE_LIST=<stage>[:<scenario>[:<start>]],...` (TEST ONLY) plays several stages one after another in one run, so
  that a test of many stages boots the game once. The first is the stage the run starts in (the save's, or
  `SMO_START_STAGE`'s). Each time the input sequence (`SMO_SEQ`) is done, the game changes to the next stage of the list
  through its own stage change, the one a door makes: at the scene's next `StageSceneNrvPlay` (while it waits, B is
  pressed every 45 polls: a map, the pause menu or a message the sequence left open would hold it) the runtime has the
  game make a `ChangeStageInfo` (its own constructor) for the stage, the scenario (default: the game's choice, as for a door)
  and the start point (the id of a placed start, e.g. a checkpoint flag's `PlayerRestartPos` link; default: the stage's
  default start), and hands it to `GameDataFunction::tryChangeNextStage`; the scene then ends and the game loads the
  stage, a kingdom's home stage with its kingdom. The game's `main` has no symbols for the two functions: they are
  found by what they do (`GAME_FN_*` in `build\gen\image.h`, see `find_game_functions` in
  `tools\lifter\gen_tables.py`). When the
  stage's `StageScene::init` runs, the input sequence starts again: `SMO_SEQ_<n>` for the n-th stage of the list
  (counted from 1), else `SMO_STAGE_SEQ`, else `SMO_SEQ`. The log marks each stage: `stage list: frame F: stage n of N
  begins: the game starts <stage>, scenario <s>`, then `stage list: stage n of N done at frame F (poll P): run totals:
  ...`, the counters of the end-of-run summary so far (frames presented, Vulkan call failures, operations not
  implemented, imports and stubs, audio dropped and underruns, loading-screen holds, process memory, run time), so a test
  can take them apart per stage. After the last stage the run ends as with `SMO_SEQ_EXIT=1`. The run's time limit (the
  exe's second argument) counts from the start of the stage being played. Stay within one kingdom: the game changes
  kingdom for a home stage only, so another kingdom's sub-area would load with the old kingdom's scenario.
- `tests\stress_stages.py --save <save folder> [--scenarios] [--jobs 2] [--speed 4] [--cache <folder>]`
  starts every stage of the game from a warped copy of the save (a kingdom's home stage directly, any other stage with
  `SMO_START_STAGE`; with `--scenarios` also every scenario of every kingdom, at its checkpoint flags), plays a few
  seconds of each at a raised speed and lists what the logs show: host exceptions, the game's aborts, operations not
  implemented, shader stages not translated, Vulkan failures, unimplemented system calls, freezes, dummy textures,
  stages that never reach play (`build\stress\<name>\summary.md`, with four small pictures a run). It also
  uses two test switches: `SMO_SPEED=<n>` (TEST ONLY) runs the refresh clock n times faster, 60 x n refreshes a second
  (0 = 960): the game steps once per refresh, so it runs up to n times faster where the PC keeps up, and input sequences,
  counted in frames, play the same; `SMO_SEQ_EXIT=1` ends the run when its input sequence is done. A game takes about
  6 GB of memory: two jobs fit a 32 GB PC with other programs open, and `scripts\run.py` makes a job wait while
  `SMO_MAX_GAMES` test games run on the PC ([`building.md`](building.md)). About three quarters of a run's frames come before play
  (the boot to the title menu alone is about half), so short runs spend most of their time starting. `--chain N` plays
  up to N stages of one kingdom and one kind (stages in the save's scenario; scenarios started at a checkpoint flag;
  scenarios at their default start) in one run with `SMO_STAGE_LIST`, splits the log per stage
  (`build\run\<name>_<stage>.log`, as a run of its own is named) and records each stage as its own run would,
  its counters from the run's totals after each stage; after a crash or a time limit a new run plays the stages left.
  `--exe-prefix` names the jobs' exe copies. A stage that only a painting leads to (the Mushroom Kingdom's six boss
  rematches, the Darker Side's last room; found in the maps: its `MissRestartArea` names the room, whose
  `PictureStageChange` leads to it) is entered as in play: its run starts in the painting's room and changes to the
  stage with the painting's `ChangeStageName` and `ChangeStageId`. Started directly (`--direct`, for comparisons), the
  restart after a miss goes to that room before the kingdom's resource loader has its archives, and they overflow the
  restart's 512 KB scene resource heap (`nn::diag abort` on FileLoadThread); in play the room is entered first and its
  archives are already in the kingdom's heap. Restarts the game makes itself are logged (`the game starts <stage>,
  scenario <n> itself`) and listed per stage (`restarts` in `results.jsonl`).

## Other switches

- Threads: the game's three console cores run on three processors of the PC, one per physical core, from a slot of six
  processors the game takes for itself: counted from the top of the process's affinity, the first slot no other
  running game holds (a named mutex per slot, `Local\smo_console_cores_<k>`; a 32-processor PC has five). A game alone
  takes slot 0; games started while it runs take the next ones, so games side by side never share these processors
  (`console cores in processor slot K of N` in the log). `SMO_OS_HOST_CPUS=<hexadecimal mask>` chooses the processors
  instead (the stress test's `--cpus split`). TEST ONLY, for comparisons: `SMO_OS_PRIO17=0` puts the game's priority 17 back on one level with
  18 to 27 (by default the model draw buffer update, 17, runs before the archive decompression, 18, as on the console).
- `SMO_IMPORT_SELFTEST=1` checks the system calls implemented from the list of the game's imports rather than met in
  play (C library, threads and message queues, files, C++ library pieces, `nn::util`, diagnostics and time, the web
  applet, the NFC reader with an amiibo held to it, the amiibo menu's list and picture): run when every handler is
  registered, before the game starts; one line per group, then it exits with the number of failed checks as exit code.
  `SMO_AMIIBO_SHOT=<file.png>` also saves the menu picture it composes.
- `SMO_MANUAL=<folder>` takes the game's manual from that folder instead of `build\manual`; `SMO_WEB_NOOPEN=1` prepares
  the Help pages without showing them (neither in the game window nor in the browser). Nothing is written next to the
  manual: the game window adds the console browser's `window.nx` to the page, a web browser gets a copy in
  `%TEMP%\smo-recomp-help`.
- `SMO_VK_RTLOG=1` logs every new size of image that can be a render target, once: the game's size, format, target,
  layers, samples and mip levels, and the image's size; at exit, `nvn_vk_rt:` lines give every render-target size a
  pass began with, its formats and how many passes used it.
- Internal resolution scale (`resolution_scale` in `graphics.ini`, 0.25 to 1.0, Vulkan only; `SMO_VK_SCALE=<scale>`
  wins for a run): the game's screen-sized targets (about 16:9, `rs_screen_class` in `nvn_vk_draw.c`) are made at that
  scale. The log's `nvn_vk_scale:` lines give the scale and the window picture's size at start, and at exit the images
  made scaled, the fragment stages translated for it and the uploads, read-backs and copies that touched a scaled
  image. `SMO_VK_UI_NATIVE=1` (experiment) leaves the window textures at the game's own size.
- `SMO_FOCUS_TEST=<start s>,<seconds>` (test): from `<start>` the window counts as not active (another program has the
  focus) for `<seconds>`, then it is minimised for `<seconds>` and restored; `nn::oe: focus test:` lines give how often
  the game's main loop ran in each period (once per frame). A test copy needs `SMO_SHOW=1` for the minimised part.
  `graphics.ini` `unfocused = run` (the default) keeps the game running then; `pause` stops it as the console does.
- `SMO_STRICT=1` lists, at start, every import bound to a handler that does nothing and every function import without a
  handler, and stops (exit code 5); `SMO_STRICT=report` lists them and goes on. The `strict check:` line of every log
  counts them.
- `SMO_VK_CSLOG=1` logs compute dispatches with large work groups.
- `SMO_USERDATA=<folder>` is the save folder (`scripts\run.py --userdata` sets it).
- `SMO_SDK_SELFTEST=1` runs the self-test of the service families (`runtime/services/nnservices.c`); it fails two
  checks (the `errno` location, `NumberLineAllocator` not lifted) and stops at `nn::image::JpegDecoder`, a game import
  without a handler.

## Looking things up in the executable

- `tools\dev\disasm.py <symbol or address> [--module main|sdk]` disassembles one function, naming call targets.
- `tools\dev\callers.py <name substring>` lists the callers of a function or import.
- `tools\dev\sarc.py <romfs path>` lists the files in a SARC archive of the extracted RomFS;
  `tools\dev\byml.py StageData\<Stage>Map.szs --scenario 0 --grep <name>` lists the objects placed in a stage, with
  their positions (prints to the console only).
- `tools\lifter\mapaddr.py` names host addresses from the linker map.
- The function list (`build\functions\<module>_functions.tsv`) names functions by the module's own symbols (column
  `symbol`) and, for `main`, the `execute` function of every state class from its RTTI (column `rtti_name`): the
  game's `main` keeps almost no symbols, so these are most of its names. `tools\target.py` says where the build's
  files are.
