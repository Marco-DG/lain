#!/usr/bin/env bash
# The program is read from the path it is given. The driver rebuilt the path from the module name
# (the path with '/' turned into '.', then every '.' back into '/'), so a dot inside a component
# changed the file: `v1.2/a.ln` was read as `v1/2/a.ln` and `x.test.ln` as `x/test.ln`, and when
# those existed lain compiled them instead, silently. `.hid/a.ln` was read as `/hid/a.ln`, at the
# filesystem root. A name starting with a digit (`1prog.ln`) was refused (I.45, Handwriting).
# Each program below exits 0; each decoy exits 7.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
SRC="$(mktemp -d)"; trap 'rm -rf "$SRC"' EXIT
PROG='func helper(x i32) i32 {\n    return x +%% 1\n}\nK i32 = 3\nfunc main() i32 {\n    return helper(K) - 4\n}\n'
DECOY='func main() i32 {\n    return 7\n}\n'
mkdir -p "$SRC/.hid" "$SRC/v1.2" "$SRC/v1/2" "$SRC/x"
printf "$DECOY" > "$SRC/v1/2/a.ln"; printf "$DECOY" > "$SRC/x/test.ln"
fail=0
for f in .hid/a.ln v1.2/a.ln x.test.ln my-prog.ln 1prog.ln; do
    printf "$PROG" > "$SRC/$f"
    ( cd "$SRC" && "$LAIN" "$f" -o out.c > err.txt 2>&1 ) || { echo "$f: refused: $(head -1 "$SRC/err.txt")"; fail=1; continue; }
    gcc -o "$SRC/out" "$SRC/out.c" -w || { echo "$f: the C does not compile"; fail=1; continue; }
    "$SRC/out"; rc=$?
    [ $rc -eq 0 ] || { echo "$f: exit $rc (7 = the decoy was compiled)"; fail=1; }
done
exit $fail
