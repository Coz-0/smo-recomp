#!/bin/bash
# The whole program on macOS, without a dump: a build folder of its own (SMO_BUILD_DIR) gets stand-ins for what the
# dump gives (two functions in the shape of the lifted game with their tables, tests/macos/stub/image.h, the empty SDK
# tables gen_tables.py writes when the SDK step has not run, a zero-filled image file), then the real build_main.py and
# build_exe.py make libsmo_game.dylib and smo_native from them and the whole runtime. Then the program runs:
#   1. SMO_VK_SELFTEST=<frames>: start-up, every registration, the window, Vulkan on the Mac's driver, frames presented
#      on the display clock, then the renderer's own statistics and exit code 0;
#   2. without it: the guest's start-up, as with the game: nninitStartup, the static constructors, nnMain, here the
#      stand-in game's two functions (image.h names them), which return: "nnMain returned", exit code 0.
# Usage: tests/macos/check_link.sh [frames, default 180]   SMO_SHOW=1 shows the window (hidden otherwise).
set -u
cd "$(dirname "$0")/../.."
PY=.venv/bin/python
FAKE=$PWD/build/macos-check/linkbuild
FRAMES=${1:-180}
fail=0
step() { if [ "$1" -eq 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; fail=1; fi; }
rm -rf "$FAKE"; mkdir -p "$FAKE/lift/main" "$FAKE/gen"
cp tests/macos/stub/image.h tests/macos/stub/sdklift_install.h "$FAKE/gen/"
cat > "$FAKE/gen/sdkdata.c" <<'C'
/* stand-in: no data symbols of the SDK module */
#include "hostrt.h"
const SdkData sdk_data[] = { { 0, 0, 0 } };
const int sdk_data_count = 0;
C
cat > "$FAKE/gen/sdklift_gen.c" <<'C'
/* stand-in: no lifted SDK functions (as gen_tables.py writes it before the SDK step) */
#include "runtime.h"
#include "sdklift.h"
const char *const sdk_host_names[] = { 0 };
const int sdk_host_name_count = 0;
const SdkFn sdk_fns[] = { {0, 0, 0} };
const int sdk_fn_count = 0;
const uint64_t sdk_ctors[] = { 0 };
const int sdk_ctor_count = 0;
C
SMO_BUILD_DIR=$FAKE $PY tools/lifter/gen_sdktest.py > /dev/null
printf '/* stand-in for the lifter funcs.h */\n#include "runtime.h"\nvoid f_8100000100(Ctx *c);\nvoid f_8100000200(Ctx *c);\n' > "$FAKE/lift/main/funcs.h"
cat > "$FAKE/lift/main/c_0.c" <<'C'
#include "funcs.h"
void f_8100000200(Ctx *c) { c->x[0] = c->x[1] + 1; }
void f_8100000100(Ctx *c) { c->x[30] = 0x8100000104ull; f_8100000200(c); }
C
cat > "$FAKE/lift/main/table.c" <<'C'
#include "funcs.h"
const FnEntry fn_table[] = { {0x8100000100ull, f_8100000100}, {0x8100000200ull, f_8100000200} };
const int fn_table_count = 2;
const DynSym dyn_syms[] = { {"", 0, 0} };
const int dyn_sym_count = 1;
C
# the dump's layout: dump/update/elf/main.elf, and exefs/main.npdm beside it (boot.c reads the main thread's stack size
# there: "META", size at +0x1c; 1 MB as this game's)
mkdir -p "$FAKE/dump/update/elf" "$FAKE/dump/update/exefs"
head -c $((0x11000)) /dev/zero > "$FAKE/dump/update/elf/main.elf"
$PY -c "import struct,sys; h=bytearray(0x80); h[0:4]=b'META'; h[0xe]=44; struct.pack_into('<I', h, 0x1c, 0x100000); open(sys.argv[1],'wb').write(h)" "$FAKE/dump/update/exefs/main.npdm"
ELF=$FAKE/dump/update/elf/main.elf

SMO_BUILD_DIR=$FAKE $PY tools/lifter/build_main.py 4 > "$FAKE/build_main.txt" 2>&1
step $? "build_main.py: the stand-in game's objects"
SMO_BUILD_DIR=$FAKE $PY tools/lifter/build_exe.py > "$FAKE/build_exe.txt" 2>&1
r=$?; step $r "build_exe.py: libsmo_game.dylib + smo_native linked from the whole runtime"
[ $r -ne 0 ] && { grep -E 'error|FAILED|Undefined|undefined' "$FAKE/build_exe.txt" | head -30; exit 1; }

cd "$FAKE/bin"
SMO_SHOW=${SMO_SHOW:-0} SMO_VK_SELFTEST=$FRAMES ./smo_native "$ELF" > ../selftest.txt 2>&1
r=$?
grep -E '^(display:|nvn_vk: (Vulkan driver|self-test: [0-9]+ frames in|swapchain|shutdown)|nvn: renderer)' ../selftest.txt | sed 's/^/      /' | cut -c1-260
step $r "the program starts, opens its window and presents $FRAMES frames through Vulkan (exit code $r)"
./smo_native "$ELF" 2 > ../boot.txt 2>&1
r=$?
grep -q '^\[phase 1\] nninitStartup' ../boot.txt && grep -q '^\[phase 3\] nnMain' ../boot.txt && grep -q '^nnMain returned' ../boot.txt && ! grep -q 'HOST EXCEPTION' ../boot.txt
step $(( ($? != 0) | (r != 0) )) "without the self-test: the guest's start-up (nninitStartup, constructors, nnMain) runs the stand-in game to its end (exit code $r)"
exit $fail
