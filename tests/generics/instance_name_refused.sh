#!/usr/bin/env bash
# I.172, the stopgap: an instance is found by its NAME, and a name is not its arguments. Each
# shape below gave two instantiations one name, and the second silently was the first: a pointer
# and a `*var` pointer (`ptr_i32`), two unions whose payload markers differ in a field's name, a
# declaration that is itself named like the instance (I.168, a function called the user's
# `pick_i32` for `pick(i32, ...)`), and an argument whose name is longer than the 127 bytes kept
# (cut, so two long names alike in those bytes were one). Each is refused with E124 and the
# limitation it hits. Three long arguments wrote past the name buffer: the arithmetic took
# snprintf's would-be length as written, so the next write was told it had SIZE_MAX bytes (a
# stack overflow on the parent, which `*** buffer overflow detected ***` aborts). The compiler is
# also built with AddressSanitizer here, and the long names must be refused with no report.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
P=$(printf 'Long%.0s' $(seq 1 75)); Q=$(printf 'Q%.0s' $(seq 1 130))
cat > "$D/ptrvar.ln" <<'LN'
type Box(T type) {
    v T
}
func one(b Box(*i32)) i32 {
    return 0
}
func two(b Box(*var i32)) i32 {
    return 0
}
func main() i32 {
    return 0
}
LN
cat > "$D/unionfield.ln" <<'LN'
func f(k i32) i32 | Err(line u32) {
    if k == 1 { return Err(7) }
    return k
}
func g(k i32) i32 | Err(col u32) {
    if k == 1 { return Err(8) }
    return k
}
func main() i32 {
    return 0
}
LN
cat > "$D/usertype.ln" <<'LN'
type Box_i32 {
    w u8
}
type Box(T type) {
    v T
}
func one(b Box(i32)) i32 {
    return b.v
}
func main() i32 {
    return 0
}
LN
cat > "$D/userfunc.ln" <<'LN'
func pick_i32(a i32, b i32) i32 {
    return b
}
func pick(T type, a T, b T) T {
    return a
}
func main() i32 {
    return pick(i32, 0, 5)
}
LN
cat > "$D/long1.ln" <<LN
type ${P}_a {
    x i32
}
type ${P}_b {
    y u8
}
type Box(T type) {
    v T
}
func one(b Box(${P}_a)) i32 {
    return b.v.x
}
func two(b Box(${P}_b)) u8 {
    return b.v.y
}
func main() i32 {
    return 0
}
LN
cat > "$D/long3.ln" <<LN
type ${Q}a {
    x i32
}
type ${Q}b {
    y i32
}
type ${Q}c {
    z i32
}
type Three(A type, B type, C type) {
    a A
    b B
    c C
}
func one(t Three(${Q}a, ${Q}b, ${Q}c)) i32 {
    return t.a.x
}
func main() i32 {
    return 0
}
LN
check() {   # compiler, program, expected position, expected text
    "$1" "$D/$2.ln" -o "$D/$2.c" > "$D/out" 2>&1; rc=$?
    if [ $rc -ne 1 ]; then echo "$2 ($(basename "$1")): exit $rc, not 1"; head -3 "$D/out"; fail=1; return; fi
    grep -q "^\[E124\] Error Ln $3:" "$D/out" || { echo "$2 ($(basename "$1")): not E124 at Ln $3:"; head -2 "$D/out"; fail=1; }
    grep -qF "$4" "$D/out" || { echo "$2 ($(basename "$1")): no '$4':"; head -2 "$D/out"; fail=1; }
    if grep -q 'AddressSanitizer' "$D/out"; then echo "$2 ($(basename "$1")): AddressSanitizer reported:"; grep -m2 'ERROR\|SUMMARY' "$D/out"; fail=1; fi
}
check "$LAIN" ptrvar     "7, Col 10"  "have the same name 'Box_ptr_i32'; this is a known limitation (I.172)"
check "$LAIN" unionfield "5, Col 6"   "two different unions have the same name '__U_i32_Err_u32'; this is a known limitation (I.172)"
check "$LAIN" usertype   "7, Col 10"  "is named 'Box_i32', and a declaration has that name; this is a known limitation (I.168)"
check "$LAIN" userfunc   "8, Col 12"  "is named 'pick_i32', and a declaration has that name; this is a known limitation (I.168)"
check "$LAIN" long1      "10, Col 10" "the name of this instantiation of 'Box' is longer than the compiler keeps"
check "$LAIN" long3      "15, Col 10" "the name of this instantiation of 'Three' is longer than the compiler keeps"
if gcc -std=c99 -O0 -g -fsanitize=address -w -o "$D/lain_asan" "$ROOT/src/frontends/lain/main.c" -I "$ROOT/src" 2> "$D/asan_build"; then
    export ASAN_OPTIONS=detect_leaks=0
    check "$D/lain_asan" long1 "10, Col 10" "the name of this instantiation of 'Box' is longer than the compiler keeps"
    check "$D/lain_asan" long3 "15, Col 10" "the name of this instantiation of 'Three' is longer than the compiler keeps"
else
    echo "the compiler did not build with -fsanitize=address:"; head -3 "$D/asan_build"; fail=1
fi
exit $fail
