#!/usr/bin/env bash
# readme_gate.sh — the README is DOCUMENTATION OF RECORD, so check it the way the corpus is
# checked: by running it. Every ```lain block on BOTH pages — the front page and the language
# reference — is extracted and put through the compiler.
#
# A block is judged by what it claims:
#   · contains `// ERROR` or an `[Exxx]` code  → it ILLUSTRATES a rejection and must FAIL
#   · otherwise                                → it must COMPILE
#   · plus `// VERIFY: exit N`                 → ...and `--interpret` must exit N
#
# That third marker exists because COMPILING IS NOT COMPUTING. A page may state what a program
# produces — "both spellings give the same value", "this wraps to 54" — and no amount of
# compiling can check that sentence. An example written as its own assertion is worse than
# unchecked: `if a != b { return 1 }` reads like a test and is inert until something runs it.
#
# Blocks that are fragments (no `func main`) are wrapped: top-level declarations get a `main`
# appended, statement fragments get wrapped in one. The wrapper's row is `io, raises, alloc` and
# deliberately NOT `diverge`: since E.6 every effect must be acknowledged, so a wrapper with a
# bare row would fail fragments for the wrapper's own reasons — but `diverge` is the one effect
# that carries a PROOF OBLIGATION, and granting it would quietly stop the gate demanding that a
# documented loop terminates. A fragment that cannot be made into a
# program is reported as UNCHECKED rather than quietly passed — an unchecked example is a
# claim nobody is testing, which is how the build command in the README's own Quick Start
# came to be missing `-I src` for four months.
#
#   bash readme_gate.sh            # exit 0 = every checkable example does what it says
#   bash readme_gate.sh -v         # ...and print each failure's source
#   bash readme_gate.sh [-v] --pages F...   # the same judgement over other pages (spec_gate
#                                  # uses it for the specification's examples); a line
#                                  # `<!-- file:line -->` before a block names its origin
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
LAIN=./lain
VERBOSE=0; PAGES=(README.md LANGUAGE.md); DEFAULT_PAGES=1
while [ $# -gt 0 ]; do
    case "$1" in
        -v) VERBOSE=1 ;;
        --pages) shift; PAGES=("$@"); DEFAULT_PAGES=0; break ;;
    esac
    shift
done
[ -x "$LAIN" ] || { echo "build first: gcc -std=c99 -o lain src/frontends/lain/main.c -I src"; exit 2; }
# ★ A STALE BINARY MAKES THIS GATE LIE IN BOTH DIRECTIONS. The check above only ever asked whether
# ./lain EXISTS, so a green here meant "the documentation agrees with whatever binary is lying in
# the tree", not "with HEAD". On 2026-10-01 a binary six hours behind HEAD reported `--interpret`
# and `--check-invariants` as unaccepted — and the stale binary's own usage string omitted them
# too, so the false verdict looked confirmed from two independent directions. The converse is
# worse: a stale binary silently PASSES a documented claim the current compiler would reject.
# A refusal rather than an implicit `make`, deliberately. A gate that builds in the shared tree
# swaps the binary under any other session's gate running at that moment, and a warning is a
# thing people learn to scroll past.
if stale=$(find src -type f -newer "$LAIN" -print -quit 2>/dev/null); [ -n "$stale" ]; then
    echo "REFUSING TO RUN: ./lain is older than $stale"
    echo "  This gate would be checking the documentation against a stale compiler."
    echo "  Run: make"
    exit 2
fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP" "$ROOT/_readme_gate_tmp.ln" "$ROOT/_readme_gate_tmp.c"' EXIT

python3 - "$TMP" "${PAGES[@]}" <<'PY'
import re, sys, os
tmp = sys.argv[1]
src = []
for path in sys.argv[2:]:
    for i, line in enumerate(open(path).read().split("\n"), 1):
        src.append((path, i, line))
blocks, cur, start, where, origin = [], None, 0, "", None
for path, i, line in src:
    if cur is None:
        m = re.match(r"\s*<!-- (\S+):(\d+) -->\s*$", line)
        if m: origin = (m.group(1), int(m.group(2))); continue
        if line.strip() == "```lain":
            cur = []
            if origin: (where, start), origin = origin, None
            else:      where, start = path, i
    else:
        if line.strip() == "```":
            blocks.append((where, start, "\n".join(cur))); cur = None
        else:
            cur.append(line)
for n, (path, ln, body) in enumerate(blocks):
    tag = path.replace("/", "%").replace("_", "~")      # `_` separates the name's fields
    open(os.path.join(tmp, "b%04d_%s_%d.txt" % (n, tag, ln)), "w").write(body)
open(os.path.join(tmp, "EXTRACTED"), "w").write(str(len(blocks)))
PY

# ★ A FENCE INSIDE A BLOCKQUOTE IS INVISIBLE TO THE EXTRACTOR, which matches a line whose stripped
# form is exactly ```` ```lain ````. Inside a `>` block the line reads `> ```lain` and is skipped — so an
# example written there is an UNVERIFIABLE claim that does not even show up in the unverifiable count.
# There were zero of them until one was added by accident on 2026-09-26, which is exactly when to make
# the shape impossible rather than to remember not to use it.
blockquoted=$(cat "${PAGES[@]}" | grep -c '^> *```lain')
if [ "$blockquoted" -ne 0 ]; then
    echo "FAIL: $blockquoted \`\`\`lain fence(s) inside a blockquote — the extractor cannot see them."
    grep -n '^> *```lain' "${PAGES[@]}"
    echo "      Move the example out of the '>' block so it is checked."
    exit 1
fi

ok=0 fail=0 expfail_ok=0 expfail_bad=0 unchecked=0
unchecked_readme=0 unchecked_lang=0 unchecked_other=0 falseclaim=0 synopsis=0
verify_n=0 verify_bad=0 accounting_bad=0
for f in "$TMP"/b*.txt; do
    ln=${f##*_}; ln=${ln%.txt}
    page=${f%_*}; page=${page##*_}; page=${page//%//}; page=${page//\~/_}
    body=$(cat "$f")
    # A block whose first line is `// SYNOPSIS` shows a FORM (a grammar schema, a token list),
    # not a program. It is counted and reported, never silently passed.
    if echo "$body" | grep -m1 -vE '^[[:space:]]*$' | grep -qE '^[[:space:]]*//[[:space:]]*SYNOPSIS'; then
        synopsis=$((synopsis+1)); continue
    fi
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
    # `// VERIFY: exit N` — what the block says it COMPUTES. Checked below, after it compiles.
    verify_exp=$(echo "$body" | grep -m1 -oE '//[[:space:]]*VERIFY:[[:space:]]*exit[[:space:]]+-?[0-9]+' \
                 | grep -oE -- '-?[0-9]+$')
    if [ -n "$verify_exp" ] && [ $expect_fail -eq 1 ]; then
        verify_bad=$((verify_bad+1))
        echo "  ★ $page:$ln — block claims both a rejection and an exit value"
    fi
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
        { printf '%s\n' "$body"; printf 'func main() i32 effects io, raises, alloc { return 0 }\n'; } > "$prog"
    elif [ -n "$(echo "$body" | tr -d '[:space:]')" ]; then
        { printf 'func main() i32 effects io, raises, alloc {\n'; printf '%s\n' "$body"; printf '    return 0\n}\n'; } > "$prog"
    else
        unchecked=$((unchecked+1))
        case "$page" in README.md)   unchecked_readme=$((unchecked_readme+1)) ;;
                        LANGUAGE.md) unchecked_lang=$((unchecked_lang+1))     ;;
                        *)           unchecked_other=$((unchecked_other+1))   ;; esac
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
        case "$page" in README.md)   unchecked_readme=$((unchecked_readme+1)) ;;
                        LANGUAGE.md) unchecked_lang=$((unchecked_lang+1))     ;;
                        *)           unchecked_other=$((unchecked_other+1))   ;; esac
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
        if [ $rc -eq 0 ]; then
            ok=$((ok+1))
            # ── A COMPUTED VALUE IS A CLAIM TOO ──────────────────────────────────────────
            # Compiling proves a program is ACCEPTED. It says nothing about what the program
            # computes, so a manual sentence like "both spellings give -9223372036854775729"
            # was checked by nothing — and an example written as its own assertion
            # (`if a != b { return 1 }`) is pure decoration while no gate runs it.
            # `// VERIFY: exit N` makes the block state its result; `--interpret` exits with
            # main's value, so the claim is then checked on the IR's own semantics.
            if [ -n "$verify_exp" ]; then
                verify_n=$((verify_n+1))
                if [ $selfcontained -eq 0 ]; then
                    verify_bad=$((verify_bad+1))
                    echo "  ★ $page:$ln — VERIFY on a block with no \`main\`: nothing can run it"
                else
                    "$LAIN" "$prog" --interpret >/dev/null 2>&1; vrc=$?
                    if [ "$vrc" != "$verify_exp" ]; then
                        verify_bad=$((verify_bad+1))
                        echo "  ★ $page:$ln — block claims exit $verify_exp, the program computes $vrc"
                    fi
                fi
            fi
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
[ $DEFAULT_PAGES -eq 1 ] && \
# A flag name may contain DIGITS (`--no-w130`). The class was [a-z-] only, so that flag was
# extracted as `--no-w`, reported as not accepted, and the real one went unchecked — the gate
# could not express the very flag it was meant to test.
for flag in $(grep -ohE '\-\-[a-z][a-z0-9-]*(=[a-z0-9-]+)?' README.md LANGUAGE.md 2>/dev/null | sort -u); do
    # ★ ACCEPTED IS NOT HONOURED. `--engine=` and `--backend=` are accepted and IGNORED (one
    # engine, one backend remain), so a page saying `--engine=legacy` restores the old checker
    # passed this check for weeks while describing a flag that does nothing. A flag whose
    # accepting branch in args.h is marked IGNORED-FLAG may not be documented as doing anything.
    if grep -E "\"${flag%%=*}=?\"" src/frontends/lain/args.h | grep -q 'IGNORED-FLAG'; then
        flag_bad=$((flag_bad+1))
        echo "  ★ README/LANGUAGE names $flag — the compiler accepts it and IGNORES it"
        continue
    fi
    grep -qF "\"$flag\"" src/frontends/lain/args.h && continue
    grep -qE "\"${flag%%=*}=\"" src/frontends/lain/args.h && continue          # --target=<triple> style
    grep -qE "strncmp\(argv\[i\], \"${flag%%=*}=\"" src/frontends/lain/args.h && continue
    flag_bad=$((flag_bad+1))
    echo "  ★ README/LANGUAGE names $flag — src/frontends/lain/args.h does not accept it"
done

echo "=================================================================="
if [ $DEFAULT_PAGES -eq 1 ]; then echo "README examples"; else echo "Examples in ${PAGES[*]}"; fi
echo "  compile as documented   : $ok"
echo "  REJECTED but documented : $fail      ← the README is wrong here"
echo "  illustrate an error, and do fail : $expfail_ok"
echo "  illustrate an error, but COMPILE : $expfail_bad   ← the README is wrong here too"
# Split by PAGE, because the two are held to different standards and the combined number reads
# as a regression on the one that is clean. README.md is the showcase and its count is 0 by
# policy; LANGUAGE.md is the old manual and its count is a backlog. Reading "66" against a
# recorded "README has zero unverifiable fragments" cost a real detour before this split.
echo "  UNVERIFIABLE fragments   : $unchecked   ← not noise: a claim nobody tests"
[ $synopsis -gt 0 ] && echo "  SYNOPSIS blocks (a form, not a program): $synopsis"
echo "      README.md   : $unchecked_readme   <- must stay 0"
echo "      LANGUAGE.md : $unchecked_lang   <- the old manual: a backlog, not a regression"
  # ★ PRINT IT WHENEVER IT IS NON-ZERO, not only under --pages. The split below the total must
  # ACCOUNT FOR the total: a count this gate computes and then hides is a backlog nobody sees, and
  # on a default run the "other" line was printed by nothing. It is 0 today because the default
  # pages are exactly README and LANGUAGE — but the next page added to that list would land here
  # silently, and the two numbers would stop adding up with no line saying so.
  { [ $DEFAULT_PAGES -eq 0 ] || [ $unchecked_other -gt 0 ]; } && \
      echo "      other pages : $unchecked_other   <- a backlog, not a regression"
  split_sum=$((unchecked_readme + unchecked_lang + unchecked_other))
  [ $split_sum -ne $unchecked ] && \
      echo "  ★ the split above sums to $split_sum but the total is $unchecked — a page is uncounted"
echo "  FLAGS named but not accepted : $flag_bad   ← a claim about the binary, now tested"
echo "  fragments drawing a PROOF diagnostic : $falseclaim   ← a false SAFETY claim, must stay 0"
# A documented VALUE, run rather than compiled. The count is small on purpose: every block that
# earns one is a sentence about what the language computes that used to rest on trust alone.
echo "  blocks claiming an exit value : $verify_n, wrong : $verify_bad   ← run under --interpret"
# EVERY EXTRACTED BLOCK MUST LAND IN EXACTLY ONE BUCKET. This total used to be printed as a bare
# number above the report, by a stray `print()` in the extractor, where it looked like debug noise —
# and it is the one line that can catch a block falling through every branch of the judgement. The
# loop has several `continue` paths, so a block silently counted nowhere would show up as a smaller
# "compile as documented" and nothing else: a claim that stopped being checked, reported as calm.
extracted=$(cat "$TMP/EXTRACTED" 2>/dev/null || echo 0)
bucket_sum=$((ok + fail + expfail_ok + expfail_bad + unchecked + synopsis))
if [ "$bucket_sum" -ne "$extracted" ]; then
    echo "  ★ $extracted blocks extracted but $bucket_sum judged — $((extracted - bucket_sum)) fell through"
    accounting_bad=1
fi
echo "  blocks extracted, all accounted for : $extracted"
echo "=================================================================="
[ $fail -eq 0 ] && [ $expfail_bad -eq 0 ] && [ $flag_bad -eq 0 ] && [ $falseclaim -eq 0 ] \
    && [ $verify_bad -eq 0 ] && [ $accounting_bad -eq 0 ] && exit 0 || exit 1
