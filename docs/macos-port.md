# macOS on Apple silicon: port plan

Status: started. Nothing below has run the game on a Mac yet. What was measured is said; the rest is the plan.

## What was measured (Apple M4, macOS 27, 2026-10-25)

Vulkan drivers on Metal, from `vulkaninfo` (LunarG SDK 1.4.363.0 and Homebrew MoltenVK 1.4.2):

| What the renderer asks for | KosmicKrisp (SDK 1.4.363) | MoltenVK 1.4.2 |
|---|---|---|
| `VK_EXT_vertex_input_dynamic_state` (required on Windows; on macOS the layout is in the pipeline key) | no | no |
| `VK_EXT_graphics_pipeline_library`, `VK_EXT_shader_object` | no | no |
| `VK_EXT_extended_dynamic_state3`: blend enable/equation, write mask, sample mask, alpha to coverage | no (the renderer keys them instead) | no |
| `logicOp` | yes | no |
| `VK_EXT_provoking_vertex`, `VK_EXT_primitive_topology_list_restart`, `VK_EXT_blend_operation_advanced` | yes | no |
| `VK_EXT_depth_clip_control`, `VK_KHR_push_descriptor`, `VK_EXT_index_type_uint8`, `VK_EXT_4444_formats` | yes | yes |
| `VK_EXT_custom_border_color` | no | no |
| `geometryShader`, `wideLines` | no | no |
| BC and ASTC textures | yes | yes |

MoltenVK is the default driver, KosmicKrisp the alternative (`SMO_VK_DRIVER=kosmickrisp`). Neither driver has dynamic
vertex input, so on macOS the vertex layout is part of the pipeline key ("The vertex layout in the pipeline key").

Why MoltenVK (measured 2026-10-26, Cap Kingdom with `tests/cap_kingdom.seq`, the same frames on both): the game ends
70-90 render passes a frame, and a pass has a fixed cost of 25-80 us on MoltenVK against 100-170 us on KosmicKrisp
(`tests/macos/vk_pass_cost.c`).

| Frames 3600-7200 (opening, Top-Hat Tower) | KosmicKrisp | MoltenVK |
|---|---|---|
| fps | 26-43 | 42-60 |
| GPU time per frame | 18-19 ms | 6-8 ms |

What MoltenVK does not have changes nothing the game draws in that run: logic ops are never set to anything but copy
(0 calls of unimplemented operations), and Metal restarts lists at the all-ones index by itself. Two things it needed:
the Vulkan shaders use the KHR subgroup operations (the ARB ballot becomes `OpSubgroupBallotKHR`, which SPIRV-Cross
turns into a compute kernel declaring `gl_SubgroupInvocationID` twice), and its pipeline cache write reports
`VK_INCOMPLETE` ("Failed to write 4 bytes") while the file it writes reads back whole.

## Order of work

Each step builds and is checked before the next one starts.

1. **`runtime/core` builds for arm64 macOS.** `runtime/compat/win32_posix.h` gives the Win32 calls the runtime uses
   (memory, locks, interlocked operations, clocks, threads, module names) on POSIX and Mach, so the Windows sources stay
   as they are. Code that cannot be shimmed is under `#ifdef _WIN32`, with the macOS version beside it: the crash
   report (signals and the frame-pointer chain instead of SEH and `RtlVirtualUnwind`), thread sampling
   (`thread_get_state`), and the `SMO_STRICT` check of do-nothing handlers (AArch64 instead of x64 machine code).
   Checked by `tests/macos/check_core.sh`, which compiles the folder and runs the shim's tests.
   **Done** (Apple M4, Apple clang, 2026-10-25). Every file of `runtime/core` compiles with `-Wall -Werror` against a
   stand-in `image.h`. The shim passes 53 of 53 checks: the four guest ranges map at their fixed addresses, the image
   protection works in 16 KB pages, and locks, condition variables, `CREATE_SUSPENDED` threads, events and thread
   sampling behave as on Windows. The AArch64 handler check passes 16 of 16 against handlers compiled at -O1 and -O2.
   `hostrt.c`, linked with two stand-in lifted functions, maps the image, dispatches an indirect call and reports a
   write into read-only text and a guest trap with the fault address, the guest and host stacks and exit code 4.
   Not checked: that the Windows build still compiles (no Windows toolchain on the Mac). The Windows code is
   unchanged except that the `SMO_CRASH_DUMP` block of the crash report is now a function of its own.
   `SMO_WATCH` is not available on macOS yet: a signal handler cannot single-step the faulting write.
2. **The rest of the runtime builds**: `os/` (threads, the three-core scheduling, the heap that shows the same pages
   at two addresses: `mach_vm_remap` instead of a pagefile-backed section), `fs/`, `libc/`, `services/`, `game/`
   (`__try`/`__except` probes become a guarded read), `input/` (SDL3 already).
   **Done for `libc/ os/ fs/ game/ input/ services/`** (2026-10-25; `gfx/` is step 5, `audio/` step 6). Every file
   compiles with `-Wall -Werror`; `tests/macos/check_runtime.sh` runs, without a dump, the runtime's own self-tests that
   do not need one:
   - time zones: the macOS rule equals `localtime_r` in 199,798 conversions of 1970..2037 in each of 8 zones;
   - save data: mount table, journal, two-phase commit, roll-forward, directory listing: 0 failures;
   - controllers with SDL3's virtual pads: 51 of 51, as on Windows;
   - network: 16 of 17 (UDP and TCP on loopback, Poll, the console's error numbers, resolver, ssl, nsd); the 17th
     reads a table of `sdk.elf` and needs a dump;
   - glyphs from CoreText outlines (TrueType and CFF), plain and outlined.
   On macOS for now: the software keyboard is a CoreFoundation alert with a text field (its self-test answers it, the
   alert itself is not driven); the Help page and movies are in the game window (step 6); the
   shared fonts fall back to macOS's own (Arial Unicode, Hiragino Sans GB, STHeiti, Apple SD Gothic Neo), not yet seen
   in the game.
3. **The build scripts run on macOS**: `setup.py`, `build_main.py`, `build_exe.py` with Apple clang, a dylib instead
   of `smo_game.dll`, Homebrew SDL3 and the Vulkan SDK.
   **Done as far as it goes without a dump** (2026-10-25). `tools/target.py` has the toolchain and file names of each
   system; `setup.py --only deps` finds Apple clang, the Vulkan SDK and SDL3; `extract.py` links the RomFS with a
   symbolic link instead of a junction. `tests/macos/check_build.sh`: `build_main.py` compiles lifter-shaped files to
   arm64 objects (its unoptimised fallback included); the shader shim builds and compiles GLSL to SPIR-V through the
   SDK's shaderc. `-ffp-contract=off` is checked: Apple clang fuses `a * b + c` into `fmadd` for AArch64 by default
   (measured), which would change float results against the console; x86-64's baseline has no FMA to fuse into.
   `tests/macos/check_lifter.sh`: the lifter's differential test on words sampled from arm64 macOS libraries (SDL3,
   libavcodec's NEON, MoltenVK): 30,176 of 30,176 equal to Unicorn, and the mutation check flags its 4 planted bugs.
   It found two lifter bugs, fixed: XTN2 wrote the lower half, LDAPR (ARMv8.3) was lifted instead of trapping.
   Not done: the link of the whole program (needs `gfx/` and `audio/`, steps 5 and 6); `scripts/run.py` (Windows
   process and memory calls; checked only against a running game).
4. **Boot without a picture**: the game reaches `nnMain` and its threads run (`SMO_GFX` off).
   **The program links and runs** (milestone 1, 2026-10-25; `tests/macos/check_link.sh`). With stand-ins for what the
   dump gives (two lifted-style functions and their tables, the stub `image.h`, empty SDK tables, a zero image and a
   `main.npdm`), the real `build_main.py` and `build_exe.py` make `libsmo_game.dylib` and `smo_native` from the whole
   runtime. `SMO_VK_SELFTEST=180`: start-up, every registration, the window, Vulkan on KosmicKrisp, 180 frames on the
   display clock in 3.000 s (60.00 fps, median 16.66 ms, worst 17.24 ms), 0 Vulkan failures, clean shutdown. Seen on
   screen (`SMO_SHOW=1`): the window shows the self-test's cleared frames. Without the self-test the guest's start-up
   runs the stand-in game through nninitStartup and nnMain to "nnMain returned".
   On macOS a program or library linked over an existing file is killed at its next launch (the kernel keeps the old
   code signature): `build_exe.py` removes them first.
5. **Renderer**: an SDL3 window with a Metal layer, the vertex layout in the pipeline key, a fallback for custom
   border colours, the geometry stage of the Ruined Kingdom's lightning.
   Done so far: the window (`display.c`: SDL3 on the main thread, which pumps its events while it waits for the game;
   events are taken as they are queued, since `nnpad_sdl.c` flushes SDL's queue), the Metal surface, the Vulkan loader
   and driver choice (MoltenVK by default, `SMO_VK_DRIVER=kosmickrisp`), the freeze watchdog's thread report (Mach
   threads, `dladdr` names), the index scan in NEON (200,000 random buffers equal to the scalar rule). The OpenGL
   renderer cannot run on macOS (OpenGL 4.6 and NVIDIA extensions): its file compiles against `compat/GL/gl.h` and
   the renderer is always Vulkan. Graphics memory pools that show heap pages at a second address (virtual pools,
   `nvnMemoryPoolMapVirtual`): `nvn_stub.c` makes its views with `mach_vm_remap` without copying where Windows uses
   `MapViewOfFile3`, with the same 64 KB granularity and the same refusals; `tests/macos/test_vpool.c` (20 checks, in
   `check_runtime.sh`) drives the real entry points. The vertex layout is in the pipeline key (below, "The vertex layout
   in the pipeline key"). To do: a fallback for custom border colours, the geometry stage.
6. **Audio, video, Help**: audio through SDL3, H.264 through VideoToolbox, the Action Guide in WKWebView.
   Audio done (not heard yet: it needs the game's renderer running): `smo_audio.c` sends the device sink to an SDL3
   audio stream with the waveOut rules (16 blocks of 40 ms at most, start at 120 ms queued, drops and underruns
   counted).
   Movies done (not seen in the game yet): `nnmovie_mf.c`'s `movie::` protocol is shared, and the reader under it is
   `nnmovie_src.h`: Media Foundation's source reader on Windows, an `AVAssetReader` on macOS (`nnmovie_av.m`): NV12
   (video range, as Media Foundation's H.264 decoder gives) and 16-bit PCM in blocks of at most 1024 frames. The movie
   goes to a temporary file (an `AVURLAsset` reads a URL), removed when the extractor is destroyed. A seek, or a track
   selected or unselected while reading, makes a new reader; each track drops what it already delivered.
   `tests/macos/test_movie.m` (24 checks, in `check_runtime.sh`) writes an H.264 + AAC movie whose every frame says
   its own number and reads it through the reader and through the real `movie::Extractor` / `Decoder` objects as the
   game drives them: every frame once and in order, the whole sound, the seek, the track changes, the events.
   Help done (not seen with the game's manual yet): `smo_guide_mac.m` shows the page in a `WKWebView` over the game
   window, as `smo_guide.c` does with WebView2 on Windows: the same script (`nnapplet.c`'s window.nx, which finds its
   host as `window.chrome.webview`: given over WebKit's message handler), the largest 16:9 area zoomed to the manual's
   1280-pixel layout and following the window's size, pads through `window.__smo_pad`, the game held while it is open,
   endApplet and callback addresses ending it. `tests/macos/test_guide.m` (9 checks, in `check_runtime.sh`) runs it in
   a hidden SDL window with a page laid out as the manual is, and reads back its `SMO_GUIDE_SHOT` pictures.

## The console core model on macOS

**What it must give** (what the Windows model gives through affinity and priorities, `os_threads.c`):
1. On each of the console's three cores, at most one guest thread runs at a time. A thread runs only on a core of its
   nn::os core mask.
2. A core runs its highest-priority ready thread. A thread that becomes ready takes the core from a running thread of
   lower priority (on Windows within microseconds, by the OS).
3. A thread that blocks (mutex, condition variable, event, message queue, sleep, a wait for the renderer) leaves its core
   to the next ready thread.

**How** (`runtime/os/core_model.c`, macOS only; Windows keeps its model):
- **Tokens.** A token per core. A guest thread runs lifted code only while it holds the token of a core in its mask. It
  takes one when it starts and gives it back when it ends.
- **Blocking.** Every host wait goes through the Win32 shim (critical sections, condition variables,
  `WaitForSingleObject`, `Sleep`), plus `SleepThread`'s `nanosleep`. The shim's slow paths call a hook before and after
  they block. Around the wait the thread gives its token back and takes one again: the same three-core rule as the
  console, applied where the console's kernel switches threads. A wait that does not block (an uncontended lock, a set
  event) costs nothing.
- **Handover.** A freed core goes to the highest-priority waiting thread whose mask has it. Equal priorities go in the
  order they came.
- **Preemption** at safe points. When a thread becomes ready and every core of its mask is held, the lowest-priority
  holder below it is asked to yield. The holder checks a flag of its own at every SDK call (`host_import`), every
  run-time host function and every indirect call (`call_indirect`), the points where lifted code leaves straight-line
  code, then gives the core up and queues again. Lifted code between two such points runs on. The wait this adds is
  measured, and the report prints its distribution. Preemption anywhere in lifted code (a signal) is the next step if
  the measurements call for it: the Windows failure was a wait of a whole Windows time slice (tens of ms).
- **`YieldThread`** gives the core to a ready thread of the same priority, as the console's yield does.
- **Host threads** (render, audio, vsync, the window's main thread) take no token. They are the PC's, as on Windows.
- **No time slicing among equal priorities**: an assumption that the console's scheduler does not either, NOT
  VERIFIED. `SMO_OS_TIMESLICE=<ms>` can turn it on to compare.

**Checked by** `tests/macos/test_core_model.c` (threads standing in for guest threads, no game):
- never two holders of a core, and never more than three at once;
- a single-core thread never overlaps another of its core;
- a thread that blocks lets another of its core run;
- priority order on handover;
- preemption latency with a busy lower-priority thread hitting safe points at a given rate;
- the 17-against-18 scenario of the Windows notes.

**Measured** (M4, `tests/macos/check_runtime.sh`): 9 of 9 checks pass. With a safe point every 20 µs, the time from
ready to running for a priority-17 thread taking core 2 from a busy priority-18 thread was a median of 16–17 µs and a
p99 of 25–76 µs over runs. With a safe point every 500 µs the median was 472 µs, so the wait follows the gap between
safe points. Under the 9-thread load test, a single outlier of 10 ms happened (the holder was descheduled by macOS).

**Wired in:** os_threads.c (start, priority, core mask, current core, name, YieldThread, SleepThread, the main thread
on core 0), the shim's blocking waits (`recomp_set_block_hooks`), and safe points at `host_import` and `call_indirect`
(hostrt.c). The real program runs with it on (`check_link.sh`). **Still open, to be measured with the game:** how often
the game reaches a safe point, and so whether the preemption latency is good enough or signals are needed; host-side
blocking outside the shim (file reads) still holds a core.

## The vertex layout in the pipeline key

Neither Mac driver has `VK_EXT_vertex_input_dynamic_state`, so a pipeline must be made with its vertex input state, and a
draw must find the pipeline of its own layout. On macOS (`#ifndef _WIN32`: the Windows key, records and behaviour stay
byte for byte as they are) the pipeline key `PKey` gains `vl`, the layout as the draw derives it today:

- per attribute location 0..15: not supplied (a constant (0, 0, 0, 1), float, unsigned or signed: three codes), or
  supplied: the NVN vertex format, the stream and the offset;
- per stream 0..15 an attribute reads: the stride and the instance divisor (0: per vertex). The stride is in the key, not
  dynamic: a dynamic stride (`vkCmdBindVertexBuffers2`) must be 0 or cover every attribute of its binding, a rule the
  extension's path does not have and the game's meshes are not known to keep.

The Vulkan bindings are numbered from `vl` alone, in attribute order, as the draw numbers them now. One function makes
the binding and attribute descriptions from `vl` (`nvn_vk_vlayout.c`), used both where the pipeline is made and at the
draw, so the two can't disagree. Where the key is built:

- at a draw (`draw_prepare`): from the bound attributes, streams and buffers, before `pipe_get`;
- in the look-ahead (`scan_draw`): the same, from the stream and buffer state the scan follows (`OP_VBUF`,
  `OP_VSTREAM`, as it already follows `OP_VATTR`);
- in `pipes.bin`: the record holds the key, so a pipeline made from a record has its layout.

Not covered: prediction (`SMO_VK_PREDICT`, which needs extended dynamic state 3 that neither Mac driver has) and pipeline
libraries (`D.has_gpl` needs dynamic vertex input). Both are already off on the Mac. **To be measured with the game:** how
many more pipelines there are per vertex stage, and how many are made at a draw.

**Checked by** `tests/macos/test_vlayout.c`: for random attribute, stream and buffer states, the descriptions made from
the key equal those the dynamic path (the Windows one) gives for the same state, and states that differ only in stride
give different keys only through the stride.

## Things the Mac does differently

- **16 KB pages.** The guest image protects text and read-only data up to `GUEST_RO_END`; in 1.3.0 that is
  0x4f16000, which is 4 KB aligned but not 16 KB aligned. Protection stops at the 16 KB page below it, so up to 12 KB
  of read-only data stays writable. Rounding up instead would make the start of `.data` read-only.
- **Fixed addresses: the guest layout is 0x10_0000_0000 higher.** macOS keeps 0x70_0000_0000..0x80_0000_0000 for
  malloc's heap range and reserves 24 GB of it around its small-allocation region at a random place on every launch;
  below it, 0x10_0000_0000..0x70_0000_0000 is a guard reservation. Measured over 60 launches of a test program (every
  region of the process listed with `mach_vm_region_recurse`): the console's addresses 0x71/0x73/0x74/0x75 × 2³² were
  taken in 10 of 20 launches of the shim's test. Above 0x80_0000_0000 nothing was mapped in any launch. So on macOS
  the game image is at 0x8100000000, the import placeholders at 0x8300000000, the host functions at 0x8400000000 and
  the SDK module at 0x8500000000 (`hostrt.h`, `tools/target.py`; the lifted code is made for these bases, and
  `image.h` says which bases it was made for, so a mismatch stops the build). `hostrt.c` reserves the whole window
  when the program loads. `tests/macos/check_core.sh` launches the mapping test 20 times.
- **No code patching.** `smo_fast.c` makes one hot game function (the sampler list sort, 1-2 ms of the main thread a
  frame at Cascade Kingdom on Windows) call a host version by rewriting a `call` in the loaded game code. The game's
  code on macOS is a signed library, and a written code page fails its signature check, so on macOS the sort runs as
  lifted until the lifter itself can emit the call to the host version.
- **`__try` probes.** The runtime reads memory that may not be readable inside `__try { } __except (1) { }`. On macOS
  the shim gives that form with `sigsetjmp` and the SIGSEGV/SIGBUS handler. As with `setjmp`, a local changed in the
  block and read after a fault must be `volatile`; none of the runtime's blocks does that.
- **No core affinity: the console core model must be rebuilt.** On Windows `os_threads.c` keeps the game's threads to
  the console's three cores by pinning each to one of three host processors (`SetThreadAffinityMask`) with Windows
  priorities in six levels, so that, as on the console, only the highest-priority ready thread of a core runs. Its
  comments record what free scheduling costs: 4 of 6 boots failed in the game's heap code, and a stage-entry crash in 2
  of 91 stress runs. macOS has no processor affinity (thread affinity tags are hints, and Apple silicon ignores them).
  Until a model of its own exists (a token per console core that a guest thread holds while it runs and gives up when
  it blocks in an SDK call, handed over in priority order), macOS prints that the model is off. This is the first thing
  step 4 has to settle.
