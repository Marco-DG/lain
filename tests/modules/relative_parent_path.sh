#!/usr/bin/env bash
# An input path that climbs (`../prog.ln`, `sub/../prog.ln`) compiles, and to the same C as the
# absolute path. The module name is derived from the path, and `../prog.ln` used to become the
# module `...prog`, whose file is `///prog.ln`: "Cannot open module file".
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
SRC="$(mktemp -d)"; trap 'rm -rf "$SRC"' EXIT
mkdir -p "$SRC/sub"
printf 'func twice(x i32) i32 {\n    return x +%% x\n}\nfunc main() i32 {\n    return twice(0)\n}\n' > "$SRC/prog.ln"
# The C is compared without #line directives (I.100), which name the file as each command line gave
# it; with them, each names its own spelling.
( cd "$SRC" && "$LAIN" "$SRC/prog.ln" -o abs.c --no-line-directives >/dev/null 2>&1 ) || { echo "absolute path failed"; exit 1; }
( cd "$SRC/sub" && "$LAIN" ../prog.ln -o up.c --no-line-directives >/dev/null 2>&1 ) || { echo "../prog.ln failed"; exit 1; }
( cd "$SRC" && "$LAIN" sub/../prog.ln -o mid.c --no-line-directives >/dev/null 2>&1 ) || { echo "sub/../prog.ln failed"; exit 1; }
cmp -s "$SRC/abs.c" "$SRC/sub/up.c" || { echo "../prog.ln emits different C"; exit 1; }
cmp -s "$SRC/abs.c" "$SRC/mid.c" || { echo "sub/../prog.ln emits different C"; exit 1; }
( cd "$SRC/sub" && "$LAIN" ../prog.ln -o upl.c >/dev/null 2>&1 ) || { echo "../prog.ln with #line failed"; exit 1; }
grep -q '^#line [0-9]* "\.\./prog\.ln"$' "$SRC/sub/upl.c" || { echo "#line does not name ../prog.ln as given"; exit 1; }
exit 0
