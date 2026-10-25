#!/bin/bash
# The macOS port's checks beyond runtime/core (docs/macos-port.md, step 2): check_core.sh, then every file of the
# folders ported so far compiled with -Wall -Werror against the stand-in image.h, then the self-tests that run without
# a dump: nntime.c's time zone rule against localtime_r in zones of every kind, nnfs.c's save data journal. Usage: tests/macos/check_runtime.sh
set -u
cd "$(dirname "$0")/../.."
OUT=build/macos-check
mkdir -p "$OUT"
CC=${CC:-clang}
FOLDERS=(libc os fs)
INC=(-Iruntime/compat -Itests/macos/stub -I/opt/homebrew/include)
for d in runtime/*/; do INC+=("-I${d%/}"); done
fail=0
step() { if [ "$1" -eq 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; fail=1; fi; }

tests/macos/check_core.sh || fail=1

for d in "${FOLDERS[@]}"; do
    for f in runtime/$d/*.c; do
        log="$OUT/$(basename "$f" .c).log"
        "$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function -Werror "${INC[@]}" -c "$f" -o "$OUT/$(basename "$f" .c).o" >"$log" 2>&1
        r=$?; step $r "compile $f"; [ $r -ne 0 ] && sed 's/^/      /' "$log" | head -20
    done
done

"$CC" -std=gnu11 -O1 -Wall -Wno-unused-function "${INC[@]}" tests/macos/test_nntime.c runtime/compat/win32_posix.c \
    -framework CoreFoundation -framework AudioToolbox -o "$OUT/test_nntime"
step $? "link nntime.c's self-test (test_nntime)"
# northern and southern daylight time, a half-hour and a 45-minute offset, daylight time abolished (2019), a zone whose
# rule is in its footer only (Casablanca), UTC
for z in America/New_York Europe/Berlin Australia/Sydney Asia/Kolkata Pacific/Chatham America/Sao_Paulo Africa/Casablanca UTC; do
    line=$("$OUT/test_nntime" "$z" 2>&1)
    r=$?
    step $r "time zone $z: $(grep -o '[0-9]* conversions.*' <<<"$line" | head -1)"
done

"$CC" -std=gnu11 -O1 -g -Wall -Wno-unused-function "${INC[@]}" runtime/fs/nnfs.c runtime/fs/nnfsdir.c runtime/compat/win32_posix.c \
    tests/macos/test_nnfs.c -framework CoreFoundation -framework AudioToolbox -o "$OUT/test_nnfs"
step $? "link nnfs.c's and nnfsdir.c's self-tests (test_nnfs)"
"$OUT/test_nnfs" > "$OUT/nnfs.txt" 2>&1
step $? "save data journal, mount table, directory listing: $(tail -1 "$OUT/nnfs.txt")"

[ $fail -eq 0 ] && echo "check_runtime: all passed" || echo "check_runtime: FAILED (logs in $OUT)"
exit $fail
