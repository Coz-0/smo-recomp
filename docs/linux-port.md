# Linux (x86-64)

Status: in progress. The runtime builds on Linux on top of the macOS port's POSIX layer (docs/macos-port.md); what was
measured is said, the rest is the plan. Developed on Arch Linux (CachyOS), Ryzen 5 7535HS, Radeon 660M (RADV).

## What Linux shares with each port

- **With Windows:** the CPU. The lifted code is x86-64 as on Windows, so the lifter, its floating-point rules
  (`fpexact.h`) and the SSE paths (`nvn_vk_draw.c`'s index scan) are the same. The crash report reads x86-64 registers;
  `SMO_STRICT`'s do-nothing handler check reads x86-64 code with the System V argument register (rdi, not rcx).
- **With macOS:** the operating system. `runtime/compat/win32_posix.c` gives the runtime's Win32 calls on POSIX, with
  the Mach parts done the Linux way:

| macOS | Linux |
|---|---|
| `os_unfair_lock`, `os_sync_wait_on_address` | futexes (`runtime/compat/posix_sync.h`), same names |
| `mach_vm_region` (`VirtualQuery`) | `/proc/self/maps` |
| `mach_vm_read_overwrite` (`host_try_read`) | `process_vm_readv` on the process itself |
| Mach thread ports, `thread_suspend` / `thread_get_state` | kernel thread ids; a signal (`SIGRTMIN+3`) stops a thread in its handler, which keeps its registers until `ResumeThread` |
| `task_threads`, `thread_info` | `/proc/self/task/*/stat` |
| `mach_vm_remap` (virtual memory pools) | none by default: the heap is private memory, which RADV can import (the game's one virtual pool is a direct alias and needs no view); with `SMO_HEAP_MEMFD=1` the heap is a shared mapping of a memfd and a view is the same file range mapped again |
| CoreText (glyph outlines) | FreeType |
| fonts in /System/Library/Fonts | what fontconfig picks: Noto Sans, Noto Sans CJK |
| AVFoundation (movies) | FFmpeg (`runtime/services/nnmovie_ff.c`) |
| WKWebView (Help) | the web browser (`xdg-open`): not in the game window yet |
| CoreFoundation alert (software keyboard) | kdialog or zenity |
| CommonCrypto (MD5, SHA-1) | OpenSSL's libcrypto |
| Metal layer, KosmicKrisp / MoltenVK | SDL3's Vulkan surface (Wayland or X11), the system's loader |

The guest is mapped at the console's own addresses (0x7100000000...), as on Windows: Linux places nothing there.

## Building

Packages (Arch names): `clang lld python sdl3 vulkan-headers vulkan-icd-loader shaderc freetype2 ffmpeg openssl
fontconfig noto-fonts noto-fonts-cjk`, and a Vulkan driver (`vulkan-radeon`, `vulkan-intel` or NVIDIA's).

```sh
python3.13 -m venv .venv
.venv/bin/python -m pip install lz4 capstone unicorn numpy Pillow
.venv/bin/python scripts/setup.py --dump <folder of your dump>
```

The build makes `build/bin/libsmo_game.so` (the lifted game, `-fPIC`, linked with `-Bsymbolic` so its calls stay inside
it as in a DLL) and `build/bin/smo_native` (the runtime, `-rdynamic`, frame pointers kept for the crash report).

## Checks without a dump

`tests/linux/check_runtime.sh`: `tests/macos/check_core.sh` (which runs on Linux too), every runtime file with
`-Wall -Werror`, and the self-tests of `tests/macos/` that are not about macOS's frameworks.
