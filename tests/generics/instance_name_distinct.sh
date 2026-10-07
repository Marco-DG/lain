#!/usr/bin/env bash
# I.172, I.168: an instance is found by its template and its arguments, and its name is only
# unique. Chain 4 refused what a name could not tell apart; each shape here now compiles and runs:
#   long      two arguments alike in the 127 bytes a name keeps: each instance is its first 96
#             bytes and a hash (`_h<8 hex>`), and each reads its own field;
#   long3     three 131-byte arguments, which once wrote past the name buffer: compiled, and again
#             with the compiler built under AddressSanitizer, which must report nothing;
#   collide   LAIN_TEST_NAME_HASH_MASK=0 makes both long names hash alike: uniqueness comes from the
#             registry, not the hash, so the second is `..._h00000000_2`, and each still reads its own;
#   stable    `Pair(My, Type_X)` beside `Pair(My_Type, X)` is `Pair_My_Type_X_2`; with an unrelated
#             `Pair(i32, u8)` instantiated above them, both keep their C names and their fields;
#   diag      a renamed instance is shown as its template spelling: 'Box(i32)', not 'Box_i32_2'.
# On the parent each is E124 (refused), and long3 under ASan is the refusal, cleanly.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"
P=$(printf 'Long%.0s' $(seq 1 35)); Q=$(printf 'Q%.0s' $(seq 1 130))
cat > "$D/long.ln" <<LN
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
    if one(Box(${P}_a(100000))) != 100000 { return 1 }
    if two(Box(${P}_b(9))) != 9 { return 2 }
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
    return t.c.z
}
func main() i32 {
    if one(Three(${Q}a(1), ${Q}b(2), ${Q}c(3))) != 3 { return 1 }
    return 0
}
LN
pair_prog() {   # $1: an extra instantiation above the colliding pair, or nothing
cat <<LN
type My_Type {
    a i32
}
type My {
    a i32
}
type X {
    c u8
}
type Type_X {
    c i64
}
type Pair(A type, B type) {
    l A
    r B
}
$1
func one(p Pair(My_Type, X)) u8 {
    return p.r.c
}
func two(p Pair(My, Type_X)) i64 {
    return p.r.c
}
func main() i32 {
    if one(Pair(My_Type(1), X(7))) != 7 { return 1 }
    if two(Pair(My(2), Type_X(5000000000))) != 5000000000 { return 2 }
    return 0
}
LN
}
pair_prog "" > "$D/stable.ln"
pair_prog "func zero(p Pair(i32, u8)) i32 {
    return p.l
}" > "$D/stable_above.ln"
cat > "$D/diag.ln" <<'LN'
type Box_i32 {
    w u8
}
type Box(T type) {
    v T
}
func f(b Box(i32)) i32 {
    return b.v
}
func main() i32 {
    var u = Box_i32(3)
    return f(u)
}
LN
run() {   # compiler, program, label: compile, build and run natively (0 expected), and --interpret
    "$1" "$D/$2.ln" -o "$D/$2.c" > "$D/out" 2>&1 || { echo "$3: lain refused it:"; head -2 "$D/out"; fail=1; return 1; }
    grep -q 'AddressSanitizer' "$D/out" && { echo "$3: AddressSanitizer reported:"; grep -m2 'ERROR\|SUMMARY' "$D/out"; fail=1; }
    gcc -w -o "$D/$2" "$D/$2.c" $DEFS || { echo "$3: the C does not compile"; fail=1; return 1; }
    "$D/$2"; rc=$?; [ $rc -eq 0 ] || { echo "$3: native exit $rc"; fail=1; }
    "$1" "$D/$2.ln" --interpret > /dev/null 2>&1; rc=$?; [ $rc -eq 0 ] || { echo "$3: --interpret exit $rc"; fail=1; }
    return 0
}
names() { grep -oE '^struct [A-Za-z0-9_]+ \{[^}]*\}' "$1" | grep -E "$2" | LC_ALL=C sort; }
# long: two hashed names, distinct
if run "$LAIN" long long; then
    n=$(names "$D/long.c" '^struct Box_' | grep -cE '_h[0-9a-f]{8} \{'); [ "$n" = 2 ] || { echo "long: $n hashed Box instances, not 2"; fail=1; }
fi
# long3: the three-argument overflow, compiled
run "$LAIN" long3 long3
# collide: the hash masked to 0, uniqueness from the registry
if LAIN_TEST_NAME_HASH_MASK=0 run "$LAIN" long collide; then
    names "$D/long.c" '^struct Box_' | grep -qE '_h00000000 \{' || { echo "collide: no _h00000000 instance"; fail=1; }
    names "$D/long.c" '^struct Box_' | grep -qE '_h00000000_2 \{' || { echo "collide: no _h00000000_2 instance"; fail=1; }
fi
# stable: the renamed instance keeps its C name, and its fields, when another is added above
if run "$LAIN" stable stable && run "$LAIN" stable_above stable_above; then
    a=$(names "$D/stable.c" '^struct Pair_My'); b=$(names "$D/stable_above.c" '^struct Pair_My')
    [ "$a" = "$b" ] || { echo "stable: the colliding pair's C names or fields moved:"; echo "$a"; echo "--"; echo "$b"; fail=1; }
    want='struct Pair_My_Type_X { My_Type l; X r; }
struct Pair_My_Type_X_2 { Type_X r; My l; }'
    [ "$a" = "$want" ] || { echo "stable: not Pair(My_Type, X) then Pair(My, Type_X) _2:"; echo "$a"; fail=1; }
fi
# diag: the template spelling of a renamed instance
"$LAIN" "$D/diag.ln" -o "$D/diag.c" > "$D/out" 2>&1
grep -qF "cannot implicitly convert 'Box_i32' to 'Box(i32)'" "$D/out" || { echo "diag: not the template spelling:"; head -1 "$D/out"; fail=1; }
# under AddressSanitizer: the long names again
if gcc -std=c99 -O0 -g -fsanitize=address -w -o "$D/lain_asan" "$ROOT/src/frontends/lain/main.c" -I "$ROOT/src" 2> "$D/asan_build"; then
    export ASAN_OPTIONS=detect_leaks=0
    run "$D/lain_asan" long long-asan
    run "$D/lain_asan" long3 long3-asan
else
    echo "the compiler did not build with -fsanitize=address:"; head -3 "$D/asan_build"; fail=1
fi
exit $fail
