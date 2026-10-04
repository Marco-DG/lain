#!/usr/bin/env bash
# I.121: a stack array larger than the stack's guard gap could skip it. `var a u8[n]` with n = 2^40
# made a 1 TiB alloca, and the write to a[5] landed in whatever was mapped below the stack: the
# program printed 7 and exited 0, a write into memory that was not the array's. With stack-clash
# probing (every page of the new region touched from the top down), the program faults AT the guard
# (SIGSEGV), deterministically, like any stack overflow, before anything beyond it is written.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cat > "$W/huge.ln" <<'LN'
extern func libc_printf(fmt *u8, ...) i32 effects io

func f(n usize) i32 {
    var a u8[n]
    if 5 < a.len {
        a[5] = 7
        return a[5] as i32
    }
    return 1
}

func main() i32 effects io {
    libc_printf("%d\n", f(1099511627776))
    return 0
}
LN
( cd "$W" && "$LAIN" huge.ln -o huge.c >/dev/null 2>&1 ) || { echo "lain refused the program"; exit 1; }
grep -q "volatile char" "$W/huge.c" || { echo "no stack-clash probe in the emitted C"; exit 1; }
gcc -O2 -w -Dlibc_printf=printf -o "$W/huge" "$W/huge.c" || { echo "the C did not compile"; exit 1; }
out="$(timeout 20 "$W/huge" 2>/dev/null)"; rc=$?
[ "$out" = "7" ] && { echo "the 1 TiB stack array was written past the guard (printed 7)"; exit 1; }
[ $rc -eq 139 ] || [ $rc -eq 134 ] || { echo "expected a fault at the guard (139), got rc=$rc"; exit 1; }
exit 0
