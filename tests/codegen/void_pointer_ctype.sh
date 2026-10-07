#!/usr/bin/env bash
# I.150 (Documentation, measuring spec 07's C mapping): `*void` is `void*` in the emitted C. `void`
# lowered as an unknown name, an unnamed struct the emitter prints as `void*`, so a pointer to it was
# `void**`: `func p(q *void) *void` became `void** p(void**)`, and `extern func free(p mov *void)`
# declared `void free(void**)`, which gcc reports as conflicting with the builtin. It ran (one
# register either way), but the C type was wrong, and an extern's prototype is a contract with C.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cat > "$W/vp.ln" <<'LN'
extern func malloc(size usize) mov *void effects io, alloc
extern func free(p mov *void) effects io

func pass(q *void) *void {
    return q
}

func main() i32 effects io, alloc {
    mov p *void = malloc(16)
    q = pass(p)
    free(mov p)
    return 0
}
LN
( cd "$W" && "$LAIN" vp.ln -o vp.c >"$W/err" 2>&1 ) || { echo "lain refused the program"; cat "$W/err"; exit 1; }
# (A local holding a `*void` has a slot whose address is `void**`, which is right; the prototypes
# are what must say `void*`.)
grep -q 'void\* vp_pass(void\*)' "$W/vp.c" || { echo "pass is not declared void* -> void*"; grep -n 'vp_pass' "$W/vp.c"; exit 1; }
grep -q 'void\* malloc(' "$W/vp.c" || { echo "malloc is not declared as returning void*"; grep -n 'malloc' "$W/vp.c"; exit 1; }
grep -q 'void free(void\*)' "$W/vp.c" || { echo "free is not declared as taking void*"; grep -n 'free' "$W/vp.c"; exit 1; }
gcc -std=c99 -Werror=builtin-declaration-mismatch -Werror=incompatible-pointer-types -o "$W/vp" "$W/vp.c" 2>"$W/gcc" || { echo "gcc refused the C:"; head -5 "$W/gcc"; exit 1; }
"$W/vp" || { echo "the program failed"; exit 1; }
echo "void pointer C type: ok"
