#!/bin/bash
# The build scripts on macOS, without a dump: tools/lifter/build_main.py compiles two small files in the shape of the
# lifter's output in a build folder of their own (SMO_BUILD_DIR, so build/ never holds made-up outputs that setup.py
# would take for done), including the fallback that compiles long functions unoptimised; build_exe.py builds the shader
# shim and runs its self-test (GLSL -> SPIR-V through the Vulkan SDK's shaderc); setup.py finds the tools.
# Usage: tests/macos/check_build.sh
set -u
cd "$(dirname "$0")/../.."
PY=.venv/bin/python
FAKE=build/macos-check/fakebuild
rm -rf "$FAKE"; mkdir -p "$FAKE/lift/main"
fail=0
step() { if [ "$1" -eq 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; fail=1; fi; }

$PY scripts/setup.py --only deps > build/macos-check/deps.txt 2>&1
step $? "setup.py finds Apple clang, the Vulkan SDK and SDL3"

printf '/* stand-in for the lifter funcs.h */\n#include "runtime.h"\n' > "$FAKE/lift/main/funcs.h"
cat > "$FAKE/lift/main/c_0.c" <<'C'
#include "funcs.h"
void f_8100000100(Ctx *c) {
    c->x[0] = c->x[1] + c->x[2];
    c->x[3] = c->x[0] * 3;
    c->x[4] = c->x[3] ^ c->x[1];
    c->x[5] = c->x[4] >> 2;
    c->x[6] = c->x[5] | 1;
    c->x[7] = c->x[6] - c->x[0];
}
C
cat > "$FAKE/lift/main/c_1.c" <<'C'
#include "funcs.h"
void f_8100000200(Ctx *c) {
    c->v[0].f[0] = c->v[1].f[0] * c->v[2].f[0] + c->v[3].f[0];
}
C
SMO_BUILD_DIR=$FAKE RECOMP_OPTNONE_LINES=3 $PY tools/lifter/build_main.py 2 > build/macos-check/build_main.txt 2>&1
r=$?
[ $r -eq 0 ] && grep -q 'objects present: 2 of 2' build/macos-check/build_main.txt && file "$FAKE/obj/main/c_0.o" | grep -q 'arm64' \
    && grep -q 'unoptimised' build/macos-check/build_main.txt
step $? "build_main.py: 2 files to arm64 objects, the 6-statement function compiled unoptimised (limit 3)"
# -ffp-contract=off: a * b + c stays a multiply and an add (clang contracts it into fmadd for AArch64 by default)
otool -tv "$FAKE/obj/main/c_1.o" | grep -qE '\bfmadd\b'
[ $? -ne 0 ]; step $? "no fused multiply-add formed from a * b + c (-ffp-contract=off)"

$PY -c "import sys; sys.path[:0] = ['tools/lifter', 'tools']; import build_exe; sys.exit(build_exe.build_shader_shim())" > build/macos-check/shim.txt 2>&1
r=$?; step $r "$(cat build/macos-check/shim.txt | tail -1 | sed 's|.*shader shim: [^,]*, ||')"
exit $fail
