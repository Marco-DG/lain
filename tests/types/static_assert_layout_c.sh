#!/usr/bin/env bash
# A module-scope assert that MEASURES a type is decided by the C compiler (DECIDE-O): Lain
# accepts the program, and building the emitted C must fail with the same E134 Lain would print.
# A `_fail` test cannot say this — Lain itself is right to accept — so it is checked here.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
printf 'type Token {\n    kind u8\n    pos u32\n}\nassert @sizeof(Token) == 5\nfunc main() i32 {\n    return 0\n}\n' > "$D/t.ln"
"$LAIN" "$D/t.ln" -o "$D/t.c" >/dev/null 2>&1 || { echo "lain refused a layout assert it cannot decide"; exit 1; }
if gcc -c -o "$D/t.o" "$D/t.c" 2> "$D/err"; then echo "the C compiler accepted a false layout assert"; exit 1; fi
grep -q 'E134' "$D/err" || { echo "the C diagnostic does not carry E134"; exit 1; }
exit 0
