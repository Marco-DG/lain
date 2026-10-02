#!/usr/bin/env bash
# A program with no declarations (an empty file, or only comments) compiles, as a program with
# declarations and no `main` does: to C with nothing in it, which a C compiler accepts. It was
# refused, exit 1, with "Could not load root module m": no code, and it blamed loading for a file
# that loaded fine. fuzz_malformed's `truncate-any` found it by cutting programs inside their
# opening comment. `--interpret` says what it says of any program without `main`.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cd "$D"
printf '' > empty.ln
printf '// only a comment\n/* and a block\n   comment */\n' > comments.ln
printf 'x = 1\n' > nomain.ln
for f in empty comments; do
    "$LAIN" "$f.ln" -o "$f.c" > "$f.out" 2>&1; rc=$?
    [ $rc -eq 0 ] || { echo "$f: exit $rc"; cat "$f.out"; exit 1; }
    [ -s "$f.out" ] && { echo "$f: printed something"; cat "$f.out"; exit 1; }
    gcc -std=c99 -c -o "$f.o" "$f.c" || { echo "$f: the C does not compile"; exit 1; }
    "$LAIN" "$f.ln" --interpret > "$f.iout" 2>&1; irc=$?
    "$LAIN" nomain.ln --interpret > nomain.iout 2>&1; nrc=$?
    [ $irc -eq $nrc ] && cmp -s "$f.iout" nomain.iout || {
        echo "$f --interpret: exit $irc, a program without main exits $nrc"; cat "$f.iout"; exit 1; }
done
echo "an empty program compiles, and --interpret treats it as any program without main"
