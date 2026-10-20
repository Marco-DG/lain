#!/usr/bin/env bash
# too_large_function.sh — a large legal program stops with a message, never a panic and never by
# taking the machine's memory. One function of 4,096 calls, each of six casts: its IR passed the
# compiler's fixed 16 MB arena and lowering PANICKED (exit 134); with the arena growing, the range
# analysis' dense octagon, (2n)^2 entries for n values, then took 9.6 GB and the machine's OOM
# killer (2026-10-20). Now the arena grows (src/utils/arena.h, ARENA_MAX_TOTAL bounding it) and
# vra_analyze refuses a function of more than VRA_MAX_DIM values before making any matrix: exit 70,
# "too large for the range analysis", in well under a second and under a 1 GB address-space cap.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
{
    echo 'func task(a i32, b i32, c i32, d i32, e i32, f i32) i32 {'
    echo '    return 0'
    echo '}'
    echo 'func main() i32 {'
    for i in $(seq 1 4096); do echo '    task((-7 as i32), (-6 as i32), (-5 as i32), (-4 as i32), (-3 as i32), (-2 as i32))'; done
    echo '    return 0'
    echo '}'
} > "$T/big.ln"
out=$(cd "$T" && timeout -s KILL 60 prlimit --as=1000000000 --core=1:unlimited "$ROOT/lain" big.ln -o big.c 2>&1); rc=$?
[ $rc -eq 70 ] || { echo "expected exit 70, got $rc:"; echo "$out" | grep -v '^\s*$' | head -5; exit 1; }
echo "$out" | grep -q "function 'main' is too large for the range analysis" \
    || { echo "exit 70 without the size message:"; echo "$out" | head -5; exit 1; }
exit 0
