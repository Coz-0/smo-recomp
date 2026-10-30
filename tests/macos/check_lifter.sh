#!/bin/bash
# The lifter on Apple silicon, without a dump: tools/lifter/difftest.py with instruction words sampled from arm64
# macOS libraries (Homebrew's SDL3: C; ffmpeg's libavcodec: hand-written NEON; MoltenVK: C++), the lifted C built with
# Apple clang and run on this Mac, Unicorn as the reference; then difftest_mutation.py, which checks that the harness
# notices planted bugs. Usage: tests/macos/check_lifter.sh   (writes only under build/difftest)
set -u
cd "$(dirname "$0")/../.."
PY=.venv/bin/python
[ -x "$PY" ] || { echo "FAIL  no .venv (python3 -m venv .venv; .venv/bin/pip install lz4 capstone unicorn numpy Pillow)"; exit 1; }
libs=()
for f in sdl3/lib/libSDL3.dylib ffmpeg/lib/libavcodec.dylib molten-vk/lib/libMoltenVK.dylib; do
    p=/opt/homebrew/opt/$f
    [ -e "$p" ] && libs+=("macho:$(realpath "$p")") || echo "note  $p not installed: sampled without it"
done
[ ${#libs[@]} -gt 0 ] || { echo "FAIL  none of the sample libraries is installed (brew install sdl3 ffmpeg molten-vk)"; exit 1; }
export RECOMP_ELF=none DIFFTEST_MODULES=$(IFS=,; echo "${libs[*]}")
fail=0
$PY tools/lifter/difftest.py all > build/difftest-macos.txt 2>&1
r=$?; grep -E '^(shapes|tests|  (branch|special|straight|trap) )' build/difftest-macos.txt | sed 's/^/      /'
[ $r -eq 0 ] && echo "ok    lifted C (Apple clang, arm64) equals Unicorn" || { echo "FAIL  lifted C differs from Unicorn (build/difftest-macos.txt)"; fail=1; }
$PY tools/lifter/difftest_mutation.py > build/difftest-mutation-macos.txt 2>&1
r=$?; [ $r -eq 0 ] && echo "ok    $(grep -o 'harness DETECTS.*' build/difftest-mutation-macos.txt)" || { echo "FAIL  mutation check (build/difftest-mutation-macos.txt)"; fail=1; }
exit $fail
