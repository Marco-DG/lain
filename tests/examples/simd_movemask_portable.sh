#!/usr/bin/env bash
# @movemask must not tie a program to x86. It emitted `_mm_movemask_epi8` and <immintrin.h>
# unconditionally, so the C of any program using it did not compile for ARM. Now the intrinsic is
# used where the target has it and a portable loop elsewhere. Checked two ways:
#   1. the portable path (forced with LAIN_PORTABLE_SIMD) gives the same answer as the intrinsic;
#   2. where clang is installed, the C type-checks for aarch64 (freestanding: no sysroot needed).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
"$LAIN" "$ROOT/tests/examples/simd_mask_pass.ln" -o "$D/m.c" >/dev/null 2>&1 || { echo "lain refused simd_mask_pass"; exit 1; }
gcc -std=c11 -w -O2 -Dlibc_printf=printf -o "$D/native" "$D/m.c" || { echo "native build failed"; exit 1; }
gcc -std=c11 -w -O2 -DLAIN_PORTABLE_SIMD -Dlibc_printf=printf -o "$D/portable" "$D/m.c" || { echo "portable build failed"; exit 1; }
"$D/native" > "$D/n.out"; rn=$?
"$D/portable" > "$D/p.out"; rp=$?
[ "$rn" = "$rp" ] && cmp -s "$D/n.out" "$D/p.out" || { echo "portable movemask differs: $rn vs $rp"; exit 1; }
if command -v clang >/dev/null 2>&1; then
    clang --target=aarch64-linux-gnu -ffreestanding -std=c11 -w -fsyntax-only "$D/m.c" 2> "$D/clang.err" \
        || { echo "the emitted C does not compile for aarch64: $(grep -m1 error "$D/clang.err")"; exit 1; }
fi
exit 0
