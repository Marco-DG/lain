#!/usr/bin/env bash
# I.132: `panic(msg)` writes `panic: <msg>` and a newline to the standard error stream, then aborts
# (spec 12). The emitted helper discarded the message and the interpreter printed nothing; both
# aborted with 134. Each message is written by its LENGTH: an empty one is `panic: `, and one
# holding a NUL is written whole, NUL included. Natively and with --interpret, stderr compared
# byte for byte, and the status.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
one() {   # name, message literal as Lain writes it, expected stderr as printf writes it
    printf 'func check(x i32) i32 effects raises {\n    if x > 3 {\n        panic(%s)\n    }\n    return x\n}\n\nfunc main() i32 effects raises {\n    return check(5)\n}\n' "$2" > "$D/$1.ln"
    printf "$3" > "$D/$1.want"
    "$LAIN" "$D/$1.ln" -o "$D/$1.c" > "$D/out" 2>&1 || { echo "$1: lain refused it"; cat "$D/out"; fail=1; return; }
    gcc -w -o "$D/$1" "$D/$1.c" || { echo "$1: the C does not compile"; fail=1; return; }
    "$D/$1" 2> "$D/$1.err"; rc=$?
    [ $rc -eq 134 ] || { echo "$1: native exit $rc, not 134"; fail=1; }
    cmp -s "$D/$1.err" "$D/$1.want" || { echo "$1: native stderr differs:"; od -c "$D/$1.err" | head -3; fail=1; }
    "$LAIN" "$D/$1.ln" --interpret 2> "$D/$1.ierr" > /dev/null; rc=$?
    [ $rc -eq 134 ] || { echo "$1: --interpret exit $rc, not 134"; fail=1; }
    cmp -s "$D/$1.ierr" "$D/$1.want" || { echo "$1: --interpret stderr differs:"; od -c "$D/$1.ierr" | head -3; fail=1; }
}
one plain '"too big"' 'panic: too big\n'
one empty '""'        'panic: \n'
one nul   '"a\0b"'    'panic: a\000b\n'
exit $fail
