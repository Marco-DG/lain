#!/usr/bin/env bash
# Circular imports (A imports B imports A) must not stack-overflow the compiler.
# ★ It wrote `proc` until 2026-09-29, four days after `proc` was removed: all three programs were
# refused at the PARSE, before any import was resolved, and "any non-crash exit" passed — so the
# test tested nothing. `func`, and the program must now COMPILE.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"
printf 'import modB\nfunc fa() i32 { return 1 }\n' > "$D/modA.ln"
printf 'import modA\nfunc fb() i32 { return 2 }\n' > "$D/modB.ln"
printf 'import modA\nfunc main() i32 { return 0 }\n' > "$D/circ.ln"
( cd "$D" && timeout 10 "$LAIN" "circ.ln" -o "circ.c" >/dev/null 2>&1 )
rc=$?
rm -rf "$D"
if [ "$rc" -ne 0 ]; then echo "circular import did not compile (rc=$rc; 139 = crash, 124 = hang)"; exit 1; fi
exit 0
