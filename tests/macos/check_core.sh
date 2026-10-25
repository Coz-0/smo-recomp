#!/bin/bash
# runtime/core on macOS (Apple silicon), without a dump: compiles every file of the folder with the stand-in image.h,
# then runs the tests of the Win32 shim, of the AArch64 do-nothing handler check and of hostrt.c's loader, dispatch
# and crash report. Usage: tests/macos/check_core.sh   (from anywhere; writes only under build/macos-check)
set -u
cd "$(dirname "$0")/../.."
OUT=build/macos-check
mkdir -p "$OUT"
CC=${CC:-clang}
CFLAGS=(-std=gnu11 -O1 -g -Wall -Wno-unused-function)
INC=(-Iruntime/compat -Itests/macos/stub)
for d in runtime/*/; do INC+=("-I${d%/}"); done
fail=0
step() { if [ "$1" -eq 0 ]; then echo "ok    $2"; else echo "FAIL  $2"; fail=1; fi; }

for f in runtime/compat/win32_posix.c runtime/core/*.c; do
    log="$OUT/$(basename "$f" .c).log"
    "$CC" "${CFLAGS[@]}" -Werror "${INC[@]}" -c "$f" -o "$OUT/$(basename "$f" .c).o" >"$log" 2>&1
    r=$?; step $r "compile $f"; [ $r -ne 0 ] && sed 's/^/      /' "$log" | head -20
done

"$CC" "${CFLAGS[@]}" "${INC[@]}" runtime/compat/win32_posix.c tests/macos/test_win32_posix.c -o "$OUT/test_win32_posix" &&
    "$OUT/test_win32_posix" | sed 's/^/      /'
step "${PIPESTATUS[0]}" "Win32 shim (test_win32_posix)"

for o in -O1 -O2; do
    "$CC" -std=gnu11 $o -Wall -Iruntime/core tests/macos/audit_handlers.c tests/macos/test_audit_a64.c -o "$OUT/test_audit_a64" &&
        "$OUT/test_audit_a64" | sed 's/^/      /'
    step "${PIPESTATUS[0]}" "do-nothing handler check on AArch64, handlers at $o (test_audit_a64)"
done

"$CC" "${CFLAGS[@]}" "${INC[@]}" runtime/core/hostrt.c runtime/compat/win32_posix.c tests/macos/test_hostrt.c -o "$OUT/test_hostrt"
step $? "link hostrt.c with stand-ins (test_hostrt)"
head -c $((0x11000)) /dev/zero > "$OUT/image.bin"     # file offset 0x1000 + GUEST_FILE_IMAGE of the stand-in

"$OUT/test_hostrt" "$OUT/image.bin" dispatch > "$OUT/dispatch.txt" 2>&1
r=$?; grep -q 'dispatch: x0=42 x1=43' "$OUT/dispatch.txt" && grep -q 'image protection: 0x7100000000..0x7100008000 read-only' "$OUT/dispatch.txt"
step $(( r | $? )) "map, protect whole 16 KB pages, dispatch an indirect call"

"$OUT/test_hostrt" "$OUT/image.bin" write-ro > "$OUT/write-ro.txt" 2>&1
r=$?
grep -q '^HOST EXCEPTION signal' "$OUT/write-ro.txt" && grep -q 'writing address 0x7100001000' "$OUT/write-ro.txt" &&
    grep -q 'last guest function entered: f_7100000200' "$OUT/write-ro.txt" &&
    grep -A3 'guest call stack' "$OUT/write-ro.txt" | grep -q 'f_7100000200' && grep -A3 'guest call stack' "$OUT/write-ro.txt" | grep -q 'f_7100000100' &&
    grep -A2 'host call stack' "$OUT/write-ro.txt" | grep -q 'test_hostrt+0x'
step $(( ($? != 0) | (r != 4) )) "crash report of a write into read-only text: fault, address, guest and host stacks, exit code 4 (got $r)"

"$OUT/test_hostrt" "$OUT/image.bin" trap > "$OUT/trap.txt" 2>&1
r=$?
grep -q '^GUEST TRAP: test trap (0x7100000104), on thread test' "$OUT/trap.txt" && grep -q '^HOST EXCEPTION signal 5' "$OUT/trap.txt" &&
    grep -A3 'guest call stack' "$OUT/trap.txt" | grep -q 'f_7100000100'
step $(( ($? != 0) | (r != 4) )) "guest trap: the message, then the report, exit code 4 (got $r)"

[ $fail -eq 0 ] && echo "check_core: all passed" || echo "check_core: FAILED (logs in $OUT)"
exit $fail
