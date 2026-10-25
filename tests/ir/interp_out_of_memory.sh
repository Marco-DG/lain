#!/usr/bin/env bash
# interp_out_of_memory.sh — the interpreter out of memory stops its run with NOT MODELLED (exit 98), as
# it does for an object it cannot make, never with a crash. ii_tmp, which holds a frame's temporaries,
# did not check its allocations: a long run under prlimit --as=4G ended in SIGSEGV at 4.1 GB (TCW,
# 2026-10-25), and a non-termination witness's run is the compiler's own. Here a loop of calls that
# never ends fills a 160 MB address space in well under a second; --judge=vra keeps the compile small.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/fill.ln" <<'LN'
func g(x i32) i32 {
    var a i32 = x
    return a
}
func main() i32 effects diverge {
    var s i32 = 0
    while true {
        s = g(s)
    }
    return s
}
LN
out=$(cd "$D" && LAIN_INTERP_STEPS=4000000000 timeout -s KILL 120 prlimit --as=$((160 * 1048576)) --core=0 -- \
      "$ROOT/lain" fill.ln --judge=vra --interpret 2>&1); rc=$?
[ $rc -eq 98 ] || { echo "fill: exit $rc, not 98 (NOT MODELLED):"; echo "$out" | tail -3; exit 1; }
echo "$out" | grep -q 'NOT MODELLED.*out of memory' || { echo "fill: no out-of-memory NOT MODELLED:"; echo "$out" | tail -3; exit 1; }
exit 0
