#!/usr/bin/env bash
# E124 in a TYPE position says where. A Type carries no position, so `func f(g G(7))`, a field
# `g G(7)`, `p Plain(i32)` and `b Buf(n)` printed "[E124] Error: ..." with no line at all, and
# check_build_warnings allowed it. The construct the type is written in now supplies it (the
# parameter, the field, the variant field, the statement, the function for its return type, the
# template's field for an instance). Each shape below must name its line and column.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
check() {   # name, expected text, program body (after the shared header, from line 11)
    printf 'type G(T type) {\n    Has { v T }\n    No\n}\ntype Buf(N usize) {\n    data u8[N]\n}\ntype Plain {\n    x i32\n}\n%b\nfunc main() i32 {\n    return 0\n}\n' "$3" > "$D/$1.ln"
    out=$("$LAIN" "$D/$1.ln" -o "$D/o.c" 2>&1); rc=$?
    if [ $rc -eq 0 ] || ! echo "$out" | grep -qF "$2"; then
        echo "$1: expected '$2'"; echo "$out" | grep -m1 '^\['; fail=1
    fi
}
check param    "[E124] Error Ln 11, Col 8: 'T' of 'G' is a type parameter, and 7 is a value." 'func f(g G(7)) i32 {\n    return 0\n}'
check ret      "[E124] Error Ln 11, Col 6: 'T' of 'G'"  'func f() G(7) {\n    return G(i32).No\n}'
check var      "[E124] Error Ln 12, Col 5: 'T' of 'G'"  'func f() i32 {\n    var g G(7) = G(i32).No\n    return 0\n}'
check field    "[E124] Error Ln 12, Col 5: 'T' of 'G'"  'type S {\n    g G(7)\n}'
check payload  "[E124] Error Ln 12, Col 9: 'T' of 'G'"  'type E {\n    V { g G(7) }\n    N\n}'
check instance "[E124] Error Ln 12, Col 5: 'T' of 'G'"  'type H(T type) {\n    g G(7)\n    t T\n}\nfunc f(h H(i32)) i32 {\n    return 0\n}'
check nested   "[E124] Error Ln 11, Col 8: 'T' of 'G'"  'func f(g G(G(7))[]) i32 {\n    return 0\n}'
check notgen   "[E124] Error Ln 11, Col 8: 'Plain' is not a generic type." 'func f(p Plain(i32)) i32 {\n    return 0\n}'
check constarg "[E124] Error Ln 11, Col 17: the argument for 'N' of 'Buf' must be a non-negative constant" 'func f(n usize, b Buf(n)) i32 {\n    return 0\n}'
exit $fail
