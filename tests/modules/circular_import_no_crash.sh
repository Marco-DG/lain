#!/usr/bin/env bash
# Circular imports (A imports B imports A) must not stack-overflow the compiler.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"
printf 'import modB\nproc fa() i32 { return 1 }\n' > "$D/modA.ln"
printf 'import modA\nproc fb() i32 { return 2 }\n' > "$D/modB.ln"
printf 'import modA\nproc main() i32 { return 0 }\n' > "$D/circ.ln"
( cd "$D" && timeout 10 "$LAIN" "circ.ln" -o "circ.c" >/dev/null 2>&1 )
rc=$?
rm -rf "$D"
# Accept success (0) or a clean non-crash error; reject SIGSEGV (139) / timeout (124).
if [ "$rc" -eq 139 ] || [ "$rc" -eq 124 ]; then echo "circular import crashed/hung (rc=$rc)"; exit 1; fi
exit 0
