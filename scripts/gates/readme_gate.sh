#!/usr/bin/env bash
# readme_gate.sh — the README is DOCUMENTATION OF RECORD, so check it the way the corpus is
# checked: by running it. Every ```lain block on BOTH pages — the front page and the language
# reference — is extracted and put through the compiler.
#
# A block is judged by what it claims:
#   · contains `// ERROR` or an `[Exxx]` code  → it ILLUSTRATES a rejection and must FAIL
#   · otherwise                                → it must COMPILE
#
# Blocks that are fragments (no `proc main`) are wrapped: top-level declarations get a `main`
# appended, statement fragments get wrapped in one. A fragment that cannot be made into a
# program is reported as UNCHECKED rather than quietly passed — an unchecked example is a
# claim nobody is testing, which is how the build command in the README's own Quick Start
# came to be missing `-I src` for four months.
#
#   bash readme_gate.sh            # exit 0 = every checkable example does what it says
#   bash readme_gate.sh -v         # ...and print each failure's source
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
LAIN=./lain
VERBOSE=0; [ "${1:-}" = "-v" ] && VERBOSE=1
[ -x "$LAIN" ] || { echo "build first: gcc -std=c99 -o lain src/main.c -I src"; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP" "$ROOT/_readme_gate_tmp.ln" "$ROOT/_readme_gate_tmp.c"' EXIT

python3 - "$TMP" <<'PY'
import re, sys, os
tmp = sys.argv[1]
src = []
for path in ("README.md", "LANGUAGE.md"):
    for i, line in enumerate(open(path).read().split("\n"), 1):
        src.append((path, i, line))
blocks, cur, start, where = [], None, 0, ""
for path, i, line in src:
    if cur is None:
        if line.strip() == "```lain":
            cur, start, where = [], i, path
    else:
        if line.strip() == "```":
            blocks.append((where, start, "\n".join(cur))); cur = None
        else:
            cur.append(line)
for n, (path, ln, body) in enumerate(blocks):
    tag = path.replace("/", "%")
    open(os.path.join(tmp, "b%04d_%s_%d.txt" % (n, tag, ln)), "w").write(body)
print(len(blocks))
PY

ok=0 fail=0 expfail_ok=0 expfail_bad=0 unchecked=0
unchecked_readme=0 unchecked_lang=0 falseclaim=0
for f in "$TMP"/b*.txt; do
    ln=${f##*_}; ln=${ln%.txt}
    page=${f%_*}; page=${page##*_}; page=${page//%//}
    body=$(cat "$f")
    # what does the block CLAIM?
    # A block CLAIMS to be an error only when the marker follows real CODE on that line.
    # `// x = 20   // ERROR: ...` has the offending line COMMENTED OUT — the block is showing
    # you what not to write, and it must still compile.
    expect_fail=0
    echo "$body" | grep -qE '^[[:space:]]*[^/[:space:]].*//.*(ERROR|error:|Compile error|E[0-9]{3})' && expect_fail=1
    # ...and a marker on its OWN first line labels the whole block. That is a different thing
    # from the commented-out-line case above, and missing it cost a real false negative:
    # `// ERROR: -1 does not satisfy >= 0` over a `func bad_abs` block was read as an example
    # that should COMPILE, failed, and was filed under UNVERIFIABLE instead of being counted
    # as the error demonstration it is.
    echo "$body" | grep -m1 -vE '^[[:space:]]*$' | grep -qE '^[[:space:]]*//[[:space:]]*(ERROR|Compile error|E[0-9]{3})' && expect_fail=1
    # make it a program
    # ★ The program is written into the REPOSITORY ROOT, not into $TMP. Module paths resolve
    # relative to the source file's directory, so an example that says `import std.c.{…}` can
    # only be checked from the directory that contains `std/`. Written to a temp dir, every
    # such example failed for a reason that had nothing to do with the example.
    prog="$ROOT/_readme_gate_tmp.ln"
    selfcontained=0
    echo "$body" | grep -qE '^\s*(proc|func) main' && selfcontained=1
    if [ $selfcontained -eq 1 ]; then
        printf '%s\n' "$body" > "$prog"
    elif echo "$body" | grep -qE '^\s*(proc|func|type|extern|import|c_include)\b'; then
        { printf '%s\n' "$body"; printf 'proc main() i32 { return 0 }\n'; } > "$prog"
    elif [ -n "$(echo "$body" | tr -d '[:space:]')" ]; then
        { printf 'proc main() i32 {\n'; printf '%s\n' "$body"; printf '    return 0\n}\n'; } > "$prog"
    else
        unchecked=$((unchecked+1))
        case "$page" in README.md) unchecked_readme=$((unchecked_readme+1)) ;;
                        *)         unchecked_lang=$((unchecked_lang+1))     ;; esac
        continue
    fi
    out=$("$LAIN" "$prog" -o "$ROOT/_readme_gate_tmp.c" 2>&1); rc=$?
    # ★ ONLY A SELF-CONTAINED EXAMPLE IS HELD TO ACCOUNT. A fragment names types and functions
    # it deliberately did not declare (`case color { … }`, `var p Point`), so every diagnostic
    # it draws is the gate's wrapper talking, not the README being wrong. The first version of
    # this script reported 53 "README errors" that were almost all its own.
    #
    # The fragment count is therefore not noise to be suppressed — it is the number to REDUCE.
    # An example nothing can check is a claim nobody is testing.
    if [ $rc -ne 0 ] && [ $expect_fail -eq 0 ] && [ $selfcontained -eq 0 ]; then
        # ★ NOT EVERY FAILURE HERE IS AN UNCHECKABLE FRAGMENT. A fragment that fails on a
        # PARSE or NAME error (E100, E106, E012, ...) really is the wrapper talking. One that
        # fails on a PROOF diagnostic — bounds, overflow, borrows, initialisation — compiled
        # far enough to be judged, and the judgement was that the documented code is unsafe.
        # That is the page making a false claim, and filing it under "backlog" hides it: the
        # `int` alias fix turned five such fragments red and this bucket absorbed all five
        # without the total moving enough to notice.
        if echo "$out" | grep -qE '^\[E(004|005|019|082|085|086|126|130|131)\]'; then
            falseclaim=$((falseclaim+1))
            echo "  ★ $page:$ln — fragment draws a PROOF diagnostic: the documented code is not safe"
            echo "$out" | grep -m1 -E '^\[E' | sed 's/^/      /'
        fi
        unchecked=$((unchecked+1))
        case "$page" in README.md) unchecked_readme=$((unchecked_readme+1)) ;;
                        *)         unchecked_lang=$((unchecked_lang+1))     ;; esac
        [ $VERBOSE -eq 1 ] && { echo "  UNVERIFIABLE $page:$ln"
                                echo "$out" | grep -m1 -E '^\[E' | sed 's/^/      /'; }
        continue
    fi
    if [ $expect_fail -eq 1 ]; then
        if [ $rc -ne 0 ]; then expfail_ok=$((expfail_ok+1))
        else
            expfail_bad=$((expfail_bad+1))
            echo "  ★ $page:$ln — block says it is an ERROR, compiler ACCEPTS it"
            [ $VERBOSE -eq 1 ] && sed 's/^/      /' "$f"
        fi
    else
        if [ $rc -eq 0 ]; then ok=$((ok+1))
        else
            fail=$((fail+1))
            echo "  ★ $page:$ln — documented as valid, compiler REJECTS it"
            echo "$out" | grep -m1 -E '^\[E' | sed 's/^/      /'
            [ $VERBOSE -eq 1 ] && sed 's/^/      /' "$f"
        fi
    fi
done

# ── FLAGS ────────────────────────────────────────────────────────────────────────────────
# The ```lain blocks were the only thing this gate read, so a flag named in PROSE was checked
# by nothing — and `--dump-octagon` sat in the README for weeks without existing in the binary.
# A flag name is a claim about the compiler like any other, so it gets tested like any other.
flag_bad=0
for flag in $(grep -ohE '\-\-[a-z][a-z-]*(=[a-z-]+)?' README.md LANGUAGE.md 2>/dev/null | sort -u); do
    grep -qF "\"$flag\"" src/args.h && continue
    grep -qE "\"${flag%%=*}=\"" src/args.h && continue          # --target=<triple> style
    grep -qE "strncmp\(argv\[i\], \"${flag%%=*}=\"" src/args.h && continue
    flag_bad=$((flag_bad+1))
    echo "  ★ README/LANGUAGE names $flag — src/args.h does not accept it"
done

echo "=================================================================="
echo "README examples"
echo "  compile as documented   : $ok"
echo "  REJECTED but documented : $fail      ← the README is wrong here"
echo "  illustrate an error, and do fail : $expfail_ok"
echo "  illustrate an error, but COMPILE : $expfail_bad   ← the README is wrong here too"
# Split by PAGE, because the two are held to different standards and the combined number reads
# as a regression on the one that is clean. README.md is the showcase and its count is 0 by
# policy; LANGUAGE.md is the old manual and its count is a backlog. Reading "66" against a
# recorded "README has zero unverifiable fragments" cost a real detour before this split.
echo "  UNVERIFIABLE fragments   : $unchecked   ← not noise: a claim nobody tests"
echo "      README.md   : $unchecked_readme   <- must stay 0"
echo "      LANGUAGE.md : $unchecked_lang   <- the old manual: a backlog, not a regression"
echo "  FLAGS named but not accepted : $flag_bad   ← a claim about the binary, now tested"
echo "  fragments drawing a PROOF diagnostic : $falseclaim   ← a false SAFETY claim, must stay 0"
echo "=================================================================="
[ $fail -eq 0 ] && [ $expfail_bad -eq 0 ] && [ $flag_bad -eq 0 ] && [ $falseclaim -eq 0 ] && exit 0 || exit 1
