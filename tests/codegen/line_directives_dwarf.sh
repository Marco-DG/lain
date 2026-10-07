#!/usr/bin/env bash
# I.100: the emitted C carries #line directives, so a -g build's line table names the .ln file
# and its lines. The C had none after the IR emitter replaced src/emit/, and --no-line-directives
# was read nowhere. Checked here: the table names the program's file as given and an imported
# module's file (relative to where gcc runs, the source's directory when the path was absolute),
# every line it gives is a line of that file, the statements' own lines are among them (not one
# per C line counted on from a directive), and the flag leaves no directive.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
command -v readelf >/dev/null 2>&1 || { echo "SKIP: no readelf"; exit 0; }
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
mkdir -p "$D/imp"
printf 'func triple(x i32) i32 {\n    var y = x\n    y = y +%% x\n    y = y +%% x\n    return y\n}\n' > "$D/imp/m.ln"
printf 'import imp.m.{triple}\n\nfunc twice(x i32) i32 {\n    var y = x\n    y = y +%% x\n    return y\n}\n\nfunc main() i32 {\n    a = twice(3)\n    b = triple(a)\n    if b != 18 {\n        return 1\n    }\n    return 0\n}\n' > "$D/prog.ln"
( cd "$D" && "$LAIN" "$D/prog.ln" -o out.c > /dev/null 2>&1 ) || { echo "lain refused the program"; exit 1; }
( cd "$D" && gcc -g -O0 -w -o prog out.c ) || { echo "the C does not compile"; exit 1; }
"$D/prog" || { echo "the program returned $?"; fail=1; }
T=$(LC_ALL=C readelf --debug-dump=decodedline "$D/prog" 2>/dev/null)
lines_of() { echo "$T" | awk -v f="$1" '$1 == f && $2 ~ /^[0-9]+$/ {print $2}' | sort -n | uniq; }
pl=$(lines_of prog.ln); ml=$(lines_of m.ln)
[ -n "$pl" ] || { echo "the line table does not name prog.ln"; echo "$T" | head -20; fail=1; }
[ -n "$ml" ] || { echo "the line table does not name imp/m.ln"; fail=1; }
for l in $pl; do [ "$l" -ge 1 ] && [ "$l" -le 16 ] || { echo "prog.ln line $l is not a line of the file"; fail=1; }; done
for l in $ml; do [ "$l" -ge 1 ] && [ "$l" -le 6 ] || { echo "imp/m.ln line $l is not a line of the file"; fail=1; }; done
for want in 4 5 6 10 11 12 13 15; do echo "$pl" | grep -qx "$want" || { echo "prog.ln line $want is not in the table"; fail=1; }; done
for want in 2 3 4 5; do echo "$ml" | grep -qx "$want" || { echo "imp/m.ln line $want is not in the table"; fail=1; }; done
grep -q "^#line [0-9]* \"$D/prog.ln\"$" "$D/out.c" || { echo "prog.ln is not named as given ($D/prog.ln)"; fail=1; }
grep -q "^#line [0-9]* \"$D/imp/m.ln\"$" "$D/out.c" || { echo "imp/m.ln is not named from gcc's directory"; fail=1; }
( cd "$D" && "$LAIN" "$D/prog.ln" -o off.c --no-line-directives > /dev/null 2>&1 )
grep -q '^#line' "$D/off.c" && { echo "--no-line-directives left a #line"; fail=1; }
exit $fail
