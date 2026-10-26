#!/bin/bash
# The Linux port's checks without a dump (docs/linux-port.md): runtime/core (tests/macos/check_core.sh, which runs on
# Linux too), every runtime file compiled with -Wall -Werror against the stand-in image.h, then the self-tests of
# tests/macos/ that are not about macOS's own frameworks: time zones, the save data journal, controllers with SDL3's
# virtual pads, sockets on loopback, the console core model, virtual memory pools, the vertex layout, glyphs from FreeType.
# Usage: tests/linux/check_runtime.sh   (from anywhere; writes only under build/linux-check)
set -u
cd "$(dirname "$0")/../.."
OUT=build/linux-check
mkdir -p "$OUT"
fail=0
step() { if [ "$1" -eq 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; fail=1; fi; }
PY=python3; [ -x .venv/bin/python ] && PY=.venv/bin/python
"$PY" tools/lifter/gen_sdktest.py >/dev/null     # build/gen/sdktest_data.h: synthetic JPEGs (Pillow)
CC=${CC:-clang}
CFLAGS=(-std=gnu11 -O1 -g -D_GNU_SOURCE -fno-omit-frame-pointer -Wall -Wno-unused-function -Wno-unused-but-set-variable -Wno-unused-but-set-global)
INC=(-Iruntime/compat -Itests/macos/stub -Ibuild/gen -I/usr/include/freetype2 $(pkg-config --cflags sdl3 freetype2 libavformat 2>/dev/null))
for d in runtime/*/; do INC+=("-I${d%/}"); done
LIBS=(-lpthread -ldl -rdynamic)
before=$(git status --porcelain --untracked-files=all | sort)
tests/macos/check_core.sh || fail=1

# gfx/ and audio/ carry style warnings of their own, hidden in the real build (-w); nvn_vk_*.c are part of nvn_vk.c
RELAXED=(-Wno-unused-variable -Wno-logical-op-parentheses -Wno-switch -Wno-misleading-indentation -Wno-unknown-pragmas -Wno-format)
for d in libc os fs game input services gfx audio compat; do
    for f in runtime/$d/*.c; do
        case "$f" in */nvn_vk_draw.c|*/nvn_vk_pipelines.c|*/nvn_vk_vlayout.c|*/nvn_vk_watchdog.c) continue ;; esac
        extra=(); case "$d" in gfx|audio|os|libc|services) extra=("${RELAXED[@]}") ;; esac
        log="$OUT/$(basename "$f" .c).log"
        "$CC" "${CFLAGS[@]}" ${extra[@]+"${extra[@]}"} -Werror "${INC[@]}" -c "$f" -o "$OUT/$(basename "$f" .c).o" >"$log" 2>&1
        r=$?; step $r "compile $f"; [ $r -ne 0 ] && sed 's/^/      /' "$log" | head -20
    done
done

"$CC" "${CFLAGS[@]}" "${INC[@]}" tests/macos/test_nntime.c runtime/compat/win32_posix.c "${LIBS[@]}" -o "$OUT/test_nntime"
step $? "link nntime.c's self-test (test_nntime)"
for z in America/New_York Europe/Berlin Australia/Sydney Asia/Kolkata Pacific/Chatham America/Sao_Paulo Africa/Casablanca UTC; do
    line=$("$OUT/test_nntime" "$z" 2>&1)
    r=$?
    step $r "time zone $z: $(grep -o '[0-9]* conversions.*' <<<"$line" | head -1)"
done

"$CC" "${CFLAGS[@]}" "${INC[@]}" runtime/fs/nnfs.c runtime/fs/nnfsdir.c runtime/compat/win32_posix.c tests/macos/test_nnfs.c "${LIBS[@]}" -o "$OUT/test_nnfs"
step $? "link nnfs.c's and nnfsdir.c's self-tests (test_nnfs)"
"$OUT/test_nnfs" > "$OUT/nnfs.txt" 2>&1
step $? "save data journal, mount table, directory listing: $(tail -1 "$OUT/nnfs.txt")"

"$CC" "${CFLAGS[@]}" "${INC[@]}" runtime/input/nnhid.c runtime/input/nnpad_sdl.c runtime/compat/win32_posix.c tests/macos/test_pad.c \
    -lSDL3 -lm "${LIBS[@]}" -o "$OUT/test_pad"
step $? "link the controller self-test with SDL3 (test_pad)"
SDL_VIDEO_DRIVER=dummy "$OUT/test_pad" > "$OUT/pad.txt" 2>&1
step $? "controllers through SDL3 virtual pads: $(grep -o 'pad self-test: .*' "$OUT/pad.txt" | tail -1)"

"$CC" "${CFLAGS[@]}" "${INC[@]}" tests/macos/test_net.c runtime/compat/win32_posix.c "${LIBS[@]}" -o "$OUT/test_net"
step $? "link the network self-test (test_net)"
"$OUT/test_net" > "$OUT/net.txt" 2>&1
nf=$(grep -c 'FAILED' "$OUT/net.txt"); ndump=$(grep 'FAILED' "$OUT/net.txt" | grep -c 'read from the SDK image')
[ "$nf" -eq "$ndump" ]
step $? "sockets, resolver, ssl, nsd on BSD sockets: $(grep -o '[0-9]* cases, [0-9]* failed' "$OUT/net.txt") ($ndump of them needs sdk.elf from a dump)"

"$CC" "${CFLAGS[@]}" "${INC[@]}" runtime/os/core_model.c runtime/compat/win32_posix.c tests/macos/test_core_model.c "${LIBS[@]}" -o "$OUT/test_core_model"
step $? "link the console core model's self-test (test_core_model)"
"$OUT/test_core_model" > "$OUT/core_model.txt" 2>&1
step $? "console core model (core tokens): $(grep -o 'core model: .*' "$OUT/core_model.txt" | tail -1); $(grep -o 'every 20 us: .*' "$OUT/core_model.txt" | sed 's/.*ready to running: //')"

"$CC" "${CFLAGS[@]}" "${INC[@]}" runtime/gfx/nvn_stub.c runtime/compat/win32_posix.c tests/macos/test_vpool.c "${LIBS[@]}" -o "$OUT/test_vpool"
step $? "link the virtual memory pool test (test_vpool)"
"$OUT/test_vpool" > "$OUT/vpool.txt" 2>&1
step $? "virtual memory pools on a memfd (mmap views of heap pages): $(grep -o 'vpool: .*' "$OUT/vpool.txt" | tail -1)"

"$CC" "${CFLAGS[@]}" "${INC[@]}" tests/macos/test_vlayout.c -o "$OUT/test_vlayout"
step $? "link the vertex layout test (test_vlayout)"
"$OUT/test_vlayout" > "$OUT/vlayout.txt" 2>&1
step $? "vertex layout in the pipeline key equals the dynamic vertex input: $(grep -o 'vlayout: .*' "$OUT/vlayout.txt" | tail -1)"

"$CC" "${CFLAGS[@]}" -Wno-unused-variable -DFONTLL_TEST "${INC[@]}" runtime/services/nnfontll.c runtime/compat/win32_posix.c -lfreetype -lm "${LIBS[@]}" -o "$OUT/fonttest"
step $? "link nnfontll.c's font test (fonttest)"
# the outlines come from FreeType: a TrueType glyph and a CFF glyph of a collection's first face, plain and outlined; the
# glyph the font's own cmap gives must be the one FreeType gives, and every map must have ink
latin=$(fc-match -f '%{file}' 'Noto Sans:style=Regular'); cjk=$(fc-match -f '%{file}' 'Noto Sans CJK SC:style=Regular')
"$OUT/fonttest" "$latin" 20 A > "$OUT/font_a.txt" 2>&1
"$OUT/fonttest" "$cjk" 16 U+4E2D 1 > "$OUT/font_zh.txt" 2>&1
grep -Eq 'glyph ([0-9]+) \(the system says \1\)' "$OUT/font_a.txt" && grep -Eq 'plain: 1[0-9]x1[0-9] ' "$OUT/font_a.txt" &&
    grep -q 'CFF outlines' "$OUT/font_zh.txt" && grep -Eq 'glyph ([0-9]+) \(the system says \1\)' "$OUT/font_zh.txt" &&
    grep -Eq 'plain: 1[0-9]x1[0-9] ' "$OUT/font_zh.txt" && grep -q 'outlined filled' "$OUT/font_zh.txt"
step $? "glyph maps from FreeType outlines: $(basename "$latin") \"A\" $(grep -o 'plain: [0-9x]*' "$OUT/font_a.txt" | head -1), $(basename "$cjk") U+4E2D $(grep -o 'plain: [0-9x]*' "$OUT/font_zh.txt" | head -1)"

after=$(git status --porcelain --untracked-files=all | sort)
[ "$before" == "$after" ]
step $? "the tests left no files in the repository (build/ aside)$( [ "$before" != "$after" ] && comm -13 <(echo "$before") <(echo "$after") | head -3 | tr '\n' ' ')"

[ $fail -eq 0 ] && echo "check_runtime: all passed" || echo "check_runtime: FAILED (logs in $OUT)"
exit $fail
