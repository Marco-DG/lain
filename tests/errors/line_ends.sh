#!/usr/bin/env bash
# Line ends are normalised before anything reads the source (spec 05): CR LF is one line feed, and
# so is a lone CR. So a program means the same, and its diagnostics name the same line, column
# and excerpt, whichever line ends its file was saved with (I.102). Before: the lexer counted CR
# and LF as two newlines and the other line counters counted LF only. A raw line end inside a
# string went uncounted in any file. So this file's error, on line 7, said Ln 6 with LF line ends
# (showing `cd"`), Ln 10 with CR LF (showing line 10's empty text) and Ln 5, Col 86 with CR (no
# excerpt). And a string literal spanning two lines kept the CR, so it was one byte longer.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cd "$D"
# Positions: a line comment, a block comment over two lines, a string over two lines, an error.
printf 'func main() i32 {\n    // a note\n    /* a block\n       comment */\n    s = "ab\ncd"\n    y u8 = 300\n    return 0\n}\n' > pos_lf.ln
# Values: the two-line string's length is the exit status.
printf 'func main() i32 {\n    s = "ab\ncd"\n    return s.len as i32\n}\n' > val_lf.ln
for p in pos val; do
    sed 's/$/\r/' ${p}_lf.ln > ${p}_crlf.ln
    tr '\n' '\r' < ${p}_lf.ln > ${p}_cr.ln
done
for e in lf crlf cr; do "$LAIN" pos_$e.ln -o /dev/null > /dev/null 2> pos_$e.err; done
# The LF answer is the right one, so the three cannot agree on a wrong one.
grep -qF '[E086] Error Ln 7, Col 5:' pos_lf.err || { echo "LF: wrong position"; cat pos_lf.err; exit 1; }
grep -qF ' 7 |     y u8 = 300' pos_lf.err || { echo "LF: wrong excerpt"; cat pos_lf.err; exit 1; }
for e in crlf cr; do
    if ! diff <(sed 's/pos_lf\.ln/F/' pos_lf.err) <(sed "s/pos_$e\.ln/F/" pos_$e.err) > /dev/null; then
        echo "$e: the diagnostic differs from LF's:"
        diff <(sed 's/pos_lf\.ln/F/' pos_lf.err) <(sed "s/pos_$e\.ln/F/" pos_$e.err) | cat -A; exit 1
    fi
done
for e in lf crlf cr; do
    "$LAIN" val_$e.ln --interpret > /dev/null 2>&1; rc=$?
    [ $rc -eq 5 ] || { echo "$e: the two-line string \"ab<line end>cd\" has length $rc, not 5"; exit 1; }
done
echo "line ends: LF, CR LF and CR give the same diagnostics and the same string"
