#!/bin/bash
# The macOS port's checks beyond runtime/core (docs/macos-port.md, step 2): check_core.sh, then every file of the
# folders ported so far compiled with -Wall -Werror against the stand-in image.h, then the self-tests that run without
# a dump: nntime.c's time zone rule against localtime_r in zones of every kind, nnfs.c's save data journal, the
# controller self-test with SDL3's virtual pads, the network self-test on loopback. Usage: tests/macos/check_runtime.sh
set -u
cd "$(dirname "$0")/../.."
OUT=build/macos-check
mkdir -p "$OUT"
[ -x .venv/bin/python ] && .venv/bin/python tools/lifter/gen_sdktest.py >/dev/null     # build/gen/sdktest_data.h: synthetic JPEGs (Pillow)
CC=${CC:-clang}
FOLDERS=(libc os fs game input services)
INC=(-Iruntime/compat -Itests/macos/stub -Ibuild/gen -I/opt/homebrew/include)
for d in runtime/*/; do INC+=("-I${d%/}"); done
fail=0
step() { if [ "$1" -eq 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; fail=1; fi; }

before=$(git status --porcelain --untracked-files=all | sort)
tests/macos/check_core.sh || fail=1

# gfx/ and audio/ carry style warnings of their own (unused variables, && within ||, switches over part of an enum),
# hidden in the real build (-w); these folders are compiled without them. nvn_vk_draw.c, nvn_vk_pipelines.c and
# nvn_vk_watchdog.c are part of nvn_vk.c.
RELAXED=(-Wno-unused-variable -Wno-logical-op-parentheses -Wno-switch -Wno-misleading-indentation -Wno-unknown-pragmas)
INC+=(-I"$HOME/VulkanSDK/1.4.363.0/macOS/include")
for d in "${FOLDERS[@]}" gfx audio compat; do
    for f in runtime/$d/*.c; do
        case "$f" in */nvn_vk_draw.c|*/nvn_vk_pipelines.c|*/nvn_vk_watchdog.c) continue ;; esac
        extra=(); case "$d" in gfx|audio) extra=("${RELAXED[@]}") ;; esac
        log="$OUT/$(basename "$f" .c).log"
        "$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-but-set-variable ${extra[@]+"${extra[@]}"} -Werror "${INC[@]}" -c "$f" -o "$OUT/$(basename "$f" .c).o" >"$log" 2>&1
        r=$?; step $r "compile $f"; [ $r -ne 0 ] && sed 's/^/      /' "$log" | head -20
    done
done

"$CC" -std=gnu11 -O1 -Wall -Wno-unused-function "${INC[@]}" tests/macos/test_nntime.c runtime/compat/win32_posix.c \
    -framework CoreFoundation -framework AudioToolbox -framework CoreGraphics -o "$OUT/test_nntime"
step $? "link nntime.c's self-test (test_nntime)"
# northern and southern daylight time, a half-hour and a 45-minute offset, daylight time abolished (2019), a zone whose
# rule is in its footer only (Casablanca), UTC
for z in America/New_York Europe/Berlin Australia/Sydney Asia/Kolkata Pacific/Chatham America/Sao_Paulo Africa/Casablanca UTC; do
    line=$("$OUT/test_nntime" "$z" 2>&1)
    r=$?
    step $r "time zone $z: $(grep -o '[0-9]* conversions.*' <<<"$line" | head -1)"
done

"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function "${INC[@]}" runtime/fs/nnfs.c runtime/fs/nnfsdir.c runtime/compat/win32_posix.c \
    tests/macos/test_nnfs.c -framework CoreFoundation -framework AudioToolbox -framework CoreGraphics -o "$OUT/test_nnfs"
step $? "link nnfs.c's and nnfsdir.c's self-tests (test_nnfs)"
"$OUT/test_nnfs" > "$OUT/nnfs.txt" 2>&1
step $? "save data journal, mount table, directory listing: $(tail -1 "$OUT/nnfs.txt")"

"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-but-set-variable "${INC[@]}" runtime/input/nnhid.c runtime/input/nnpad_sdl.c \
    runtime/compat/win32_posix.c tests/macos/test_pad.c -L/opt/homebrew/lib -lSDL3 -framework CoreFoundation -framework AudioToolbox \
    -framework CoreGraphics -o "$OUT/test_pad"
step $? "link the controller self-test with SDL3 (test_pad)"
"$OUT/test_pad" > "$OUT/pad.txt" 2>&1
step $? "controllers through SDL3 virtual pads: $(grep -o 'pad self-test: .*' "$OUT/pad.txt" | tail -1)"

"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function "${INC[@]}" tests/macos/test_net.c runtime/compat/win32_posix.c \
    -framework CoreFoundation -framework AudioToolbox -framework CoreGraphics -o "$OUT/test_net"
step $? "link the network self-test (test_net)"
"$OUT/test_net" > "$OUT/net.txt" 2>&1
# one case reads a table of the SDK module (sdk.elf, from a dump): the only failure allowed here
nf=$(grep -c 'FAILED' "$OUT/net.txt"); ndump=$(grep 'FAILED' "$OUT/net.txt" | grep -c 'read from the SDK image')
[ "$nf" -eq "$ndump" ]
step $? "sockets, resolver, ssl, nsd on BSD sockets: $(grep -o '[0-9]* cases, [0-9]* failed' "$OUT/net.txt") ($ndump of them needs sdk.elf from a dump)"

"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function "${INC[@]}" runtime/os/core_model.c runtime/compat/win32_posix.c tests/macos/test_core_model.c \
    -framework CoreFoundation -framework AudioToolbox -framework CoreGraphics -o "$OUT/test_core_model"
step $? "link the console core model's self-test (test_core_model)"
"$OUT/test_core_model" > "$OUT/core_model.txt" 2>&1
step $? "console core model (core tokens): $(grep -o 'core model: .*' "$OUT/core_model.txt" | tail -1); $(grep -o 'every 20 us: .*' "$OUT/core_model.txt" | sed 's/.*ready to running: //')"

# movies: AVFoundation under nnmovie_mf.c's movie:: protocol, on a movie the test writes itself (H.264 + AAC)
"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function "${INC[@]}" -c runtime/services/nnmovie_mf.c -o "$OUT/movie_mf.o" &&
"$CC" -std=gnu11 -O1 -g -w "${INC[@]}" -c runtime/compat/win32_posix.c -o "$OUT/movie_w32.o" &&
"$CC" -fobjc-arc -O1 -g -Wall -Wno-unused-function -Werror "${INC[@]}" runtime/services/nnmovie_av.m tests/macos/test_movie.m "$OUT/movie_mf.o" "$OUT/movie_w32.o" \
    -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Foundation -framework CoreFoundation -framework AudioToolbox \
    -framework CoreGraphics -o "$OUT/test_movie"
step $? "link the movie test with AVFoundation (test_movie)"
"$OUT/test_movie" > "$OUT/movie.txt" 2>&1
step $? "movies on AVFoundation through movie::Extractor / Decoder: $(grep -o 'movie test: .*' "$OUT/movie.txt" | tail -1)"

# the Help in the game window: WKWebView over a hidden SDL window, a page laid out as the manual is, pads from a thread
"$CC" -fobjc-arc -O1 -g -Wall -Werror "${INC[@]}" runtime/game/smo_guide_mac.m tests/macos/test_guide.m -L/opt/homebrew/lib -lSDL3 \
    -framework AppKit -framework WebKit -o "$OUT/test_guide"
step $? "link the Help page test with WebKit (test_guide)"
"$OUT/test_guide" > "$OUT/guide.txt" 2>&1
step $? "the Help in the game window (WKWebView): $(grep -o 'guide test: .*' "$OUT/guide.txt" | tail -1)"

"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -DFONTLL_TEST "${INC[@]}" runtime/services/nnfontll.c runtime/compat/win32_posix.c \
    -framework CoreText -framework CoreFoundation -framework CoreGraphics -framework AudioToolbox -o "$OUT/fonttest"
step $? "link nnfontll.c's font test (fonttest)"
# the outlines come from CoreText: a TrueType glyph and a CFF glyph of a collection's first face, plain and outlined
"$OUT/fonttest" "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" 20 A > "$OUT/font_a.txt" 2>&1
"$OUT/fonttest" "/System/Library/Fonts/Hiragino Sans GB.ttc" 16 U+4E2D 1 > "$OUT/font_zh.txt" 2>&1
grep -q 'glyph 36 (the system says 36)' "$OUT/font_a.txt" && grep -q 'plain: 15x15 lo_x -1 hi_y 14' "$OUT/font_a.txt" &&
    grep -q 'CFF outlines' "$OUT/font_zh.txt" && grep -q 'plain: 14x16 lo_x 1 hi_y 13' "$OUT/font_zh.txt" && grep -q 'outlined filled: 18x20' "$OUT/font_zh.txt"
step $? "glyph maps from CoreText outlines: Arial Unicode \"A\" 15x15 at 20 px, Hiragino Sans GB U+4E2D 14x16 at 16 px, outlined 18x20"

after=$(git status --porcelain --untracked-files=all | sort)
[ "$before" == "$after" ]
step $? "the tests left no files in the repository (build/ aside)$( [ "$before" != "$after" ] && comm -13 <(echo "$before") <(echo "$after") | head -3 | tr '\n' ' ')"

[ $fail -eq 0 ] && echo "check_runtime: all passed" || echo "check_runtime: FAILED (logs in $OUT)"
exit $fail
