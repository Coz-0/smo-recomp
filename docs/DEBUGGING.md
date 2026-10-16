# Debugging aids

The runtime's switches are environment variables with the `SMO_` prefix; pass them to a run with
`scripts\run.py <run name> --env NAME=VALUE`. Everything a run prints goes to `build\run\<run name>.log`.

## The log at a glance

The end of every log has the counts that tell whether a run was healthy:

| Line | Healthy |
|---|---|
| `nvn_vk: N frames presented, ... 0 Vulkan call failures, ... 0 calls of operations that are not implemented` | both zero |
| `nvn_gl: ... command lists (0 unknown handles) ...` | zero: a non-zero count means draws were dropped (scenery missing or flickering) |
| `nvn: command lists freed: ...` | lists freed when the game finalizes them or records over them; `by the budget` should be 0 |
| `audio: device sink: ... 0 samples dropped ..., 0 underruns ...` | both zero |
| `HOST EXCEPTION` | absent |
| `imports reached ... unimplemented imports reached:` | system calls the game made that have no implementation yet |

## Pictures

- `SMO_SHOT=<n>` saves frame n; `SMO_SHOT_EVERY=<k>` saves every k-th frame from `SMO_SHOT_FROM=<frame>` on, as
  `frame_<n>.bmp` in `SMO_SHOT_DIR=<folder>` (the folder must exist).
- `SMO_VK_PASSSHOT=<frame>` saves colour target 0 at the end of every render pass of that frame
  (`pass_<n>_<format>.bmp`) and logs every draw of it: programs, blend and depth state, viewport, vertex attributes and
  the texture behind every binding. `SMO_VK_PASSSHOT=<first>-<last>/<step>` does it for several frames
  (`pass_<frame>_<n>_<format>.bmp`), for defects that show in some frames only. `SMO_VK_PASSSHOT_RAW=1` adds the
  texels as read, for exact values.
- `SMO_VK_DIAG_NODEPTH=1` draws without depth testing (to see whether something is hidden or missing).

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

## Missing or flickering draws

- Check the `unknown handles` count first. `SMO_CMDMEMLOG=1` logs the command memory the game hands to its command
  buffers (the first 400 times), which decides when a recorded list is over.
- `SMO_LIST_BUDGET_MB=<n>` (default 4096) is a last-resort limit on recorded command lists; when it drops lists it says
  so, because the game may still submit them.
- `SMO_TEXLOG=1` logs texture creation, finalization and registration; the per-draw log of `SMO_VK_PASSSHOT` says why
  a texture handle is unknown (slot outside the pool, never registered, or a texture that was finalized).

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

## Other switches

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
