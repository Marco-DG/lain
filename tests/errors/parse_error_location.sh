#!/usr/bin/env bash
# A parse error names its file and shows its line (spec 04: a diagnostic includes the source file
# name). Parse errors printed only "Ln, Col", so an error in an imported module did not say which
# file it was in; sema's diagnostics already showed `--> file:line:col`, the line and a caret.
# A parse error names the token as the source spells it, never the compiler's TOKEN_ name, and a
# bad top-level line is one error, not one per token.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
fail=0
mkdir -p "$D/imp"
printf 'func helper() i32 {\n    return = 1\n}\n' > "$D/imp/badmod.ln"
printf 'import imp.badmod\n\nfunc main() i32 {\n    return 0\n}\n' > "$D/main.ln"
out=$(cd "$D" && "$LAIN" main.ln -o /dev/null 2>&1); rc=$?
[ $rc -ne 0 ] || { echo "the import's parse error compiled"; fail=1; }
echo "$out" | grep -qF -- "--> imp/badmod.ln:2:12" || { echo "no '--> imp/badmod.ln:2:12' in:"; echo "$out"; fail=1; }
echo "$out" | grep -qF -- " 2 |     return = 1" || { echo "the source line is not shown"; fail=1; }
for prog in 'func main() i32 {\n    x = = 3\n    return 0\n}\n' 'func main() i32 {\n    return 0\n}\n- 3\n' \
            'func main() i32 {\n    return (1\n}\n' 'type T = i32 <= 9 - 1\n'; do
    printf "$prog" > "$D/p.ln"
    o=$("$LAIN" "$D/p.ln" -o /dev/null 2>&1)
    echo "$o" | grep -q 'TOKEN_' && { echo "a TOKEN_ name leaked:"; echo "$o" | grep 'TOKEN_'; fail=1; }
    echo "$o" | grep -q -- "--> p.ln:" || { echo "no location in:"; echo "$o" | head -3; fail=1; }
done
printf 'func main() i32 {\n    return 0\n}\n- 3 4 5\n' > "$D/p.ln"
n=$("$LAIN" "$D/p.ln" -o /dev/null 2>&1 | grep -c '^\[E')
[ "$n" -eq 1 ] || { echo "one bad top-level line gave $n errors"; fail=1; }
exit $fail
