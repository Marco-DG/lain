#!/usr/bin/env bash
# `-o out.c` is relative to the WORKING directory. With an absolute input path the driver chdirs
# to the source's directory (for module resolution), and a relative -o used to follow it: the C
# landed next to the source, in the source tree, and not where the user asked.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
SRC="$(mktemp -d)"; WORK="$(mktemp -d)"; trap 'rm -rf "$SRC" "$WORK"' EXIT
printf 'func main() i32 {\n    return 0\n}\n' > "$SRC/prog.ln"
( cd "$WORK" && "$LAIN" "$SRC/prog.ln" -o out.c >/dev/null 2>&1 ) || { echo "lain failed"; exit 1; }
[ -f "$WORK/out.c" ] || { echo "out.c is not in the working directory"; exit 1; }
[ ! -f "$SRC/out.c" ] || { echo "out.c was written next to the source"; exit 1; }
exit 0
