# macOS on Apple silicon: port plan

Status: started. Nothing below has run the game on a Mac yet. What was measured is said; the rest is the plan.

## What was measured (Apple M4, macOS 27, 2026-10-25)

Vulkan drivers on Metal, from `vulkaninfo` (LunarG SDK 1.4.363.0 and Homebrew MoltenVK 1.4.2):

| What the renderer asks for | KosmicKrisp (SDK 1.4.363) | MoltenVK 1.4.2 |
|---|---|---|
| `VK_EXT_vertex_input_dynamic_state` (required, `nvn_vk_draw.c` stops without it) | no | no |
| `VK_EXT_graphics_pipeline_library`, `VK_EXT_shader_object` | no | no |
| `VK_EXT_extended_dynamic_state3`: blend enable/equation, write mask, sample mask, alpha to coverage | no (the renderer keys them instead) | no |
| `logicOp` | yes | no |
| `VK_EXT_provoking_vertex`, `VK_EXT_primitive_topology_list_restart`, `VK_EXT_blend_operation_advanced` | yes | no |
| `VK_EXT_depth_clip_control`, `VK_KHR_push_descriptor`, `VK_EXT_index_type_uint8`, `VK_EXT_4444_formats` | yes | yes |
| `VK_EXT_custom_border_color` | no | no |
| `geometryShader`, `wideLines` | no | no |
| BC and ASTC textures | yes | yes |

KosmicKrisp is the target driver, MoltenVK the fallback. Neither driver has dynamic vertex input, so the vertex layout
has to become part of the pipeline key on both.

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
3. **The build scripts run on macOS**: `setup.py`, `build_main.py`, `build_exe.py` with Apple clang, a dylib instead
   of `smo_game.dll`, Homebrew SDL3 and the Vulkan SDK.
4. **Boot without a picture**: the game reaches `nnMain` and its threads run (`SMO_GFX` off).
5. **Renderer**: an SDL3 window with a Metal layer, the vertex layout in the pipeline key, a fallback for custom
   border colours, the geometry stage of the Ruined Kingdom's lightning.
6. **Audio, video, Help**: audio through SDL3, H.264 through VideoToolbox, the Action Guide in WKWebView.

## Things the Mac does differently

- **16 KB pages.** The guest image protects text and read-only data up to `GUEST_RO_END`; in 1.4.1 that is
  0x4f16000, which is 4 KB aligned but not 16 KB aligned. Protection stops at the 16 KB page below it, so up to 12 KB
  of read-only data stays writable. Rounding up instead would make the start of `.data` read-only.
- **Fixed addresses.** `mmap` is given the guest bases (0x7100000000, 0x7300000000, 0x7400000000, 0x7500000000) as a
  hint and the result is checked, as `VirtualAlloc`'s is on Windows. `tests/macos/check_core.sh` maps all four.
- **No core affinity.** macOS has no `SetThreadAffinityMask`. The three-core model keeps its own run queue and does
  not need it.
