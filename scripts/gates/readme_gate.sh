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
# ★ WHAT THAT ORACLE CAN AND CANNOT SEE (Handwriting's framing, worth stating where the marker is
# defined so a green line is not over-read). `--interpret` runs the program on the IR's own
# semantics. So it catches a wrong ANALYSIS — anything the IR does not itself encode, e.g. I.60's
# unsafe-waiver, where the range analysis read an unwrapped value and the running program did not.
# It is BLIND to a wrong IR, because it executes the same mistake it is meant to detect: in I.56
# the policy operators were typed i32, and the emitted C and the interpreter agreed on the wrong
# value. Only Handwriting's census saw it, because its expected values come from Python — outside
# the compiler. A second oracle inside the system is not an independent one.
# The exit status is also 8 bits, so values are compared modulo 256; see the write site.
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
VERBOSE=0; PAGES=(README.md LANGUAGE.md USAGE.md); DEFAULT_PAGES=1
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
TMP=$(mktemp -d)
# ★ THE PROGRAM FILE IS NAMED PER PROCESS. It has to live in the repo root — module paths resolve
# relative to the source file's directory, so an example saying `import std.c.{…}` can only be
# compiled from the directory that holds `std/` (see the note at the write site). But a FIXED name
# there is shared state: two runs of this gate at once — one in a compiler session's chain, one in
# mine — overwrite each other's program, so run A can compile run B's example and report the
# verdict against A's page and line, and whichever exits first deletes the other's file mid-run.
# Moving it into $TMP would fix the collision and break every module example, which is why the
# fix is the NAME and not the directory.
PROG="$ROOT/_readme_gate_tmp.$$.ln"; PROGC="$ROOT/_readme_gate_tmp.$$.c"
trap 'rm -rf "$TMP" "$PROG" "$PROGC"' EXIT

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

ok=0 fail=0 expfail_ok=0 expfail_bad=0 expfail_code=0 unchecked=0
unchecked_readme=0 unchecked_lang=0 unchecked_other=0 falseclaim=0 synopsis=0
verify_n=0 verify_bad=0 accounting_bad=0 unchecked_usage=0
for f in "$TMP"/b*.txt; do
    # ★ With no blocks extracted the glob does not match and bash passes the PATTERN through,
    # so the body came back empty and was counted as one unverifiable fragment. A page with
    # zero examples then reported 1, and the accounting line said -1 fell through.
    [ -e "$f" ] || continue
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
    # ★ AN EXIT STATUS IS 8 BITS, so this comparison aliases modulo 256 and the gate must say so
    # rather than let a page look checked. Handwriting measured it on 479e320: a `main` returning
    # 256 exits 0 and one returning 300 exits 44, in the emitted C and under --interpret alike.
    # Two ways that bites, the second much worse than the first:
    #   · `// VERIFY: exit 300` can never hold, and the failure reads as a compiler bug rather
    #     than as an impossible expectation;
    #   · `// VERIFY: exit 0` PASSES for a program that computes 256 — the gate then CONFIRMS a
    #     wrong value, and 0 is the value an author writes most often.
    # So an out-of-range claim is refused as a malformed annotation, AND the claimed value must
    # appear in the block as a literal `return N`. The second rule is what actually closes the
    # aliasing hazard: it forces the interesting comparison INSIDE the program
    # (`if f(x) != 300 { return 1 }`), where it happens at full width, and leaves the exit status
    # carrying only which assertion failed. A computed return can alias; a literal the author
    # wrote cannot. It costs a little verbosity in the examples and buys the one thing a
    # mod-256 comparison cannot give: a value the gate checks is the value the page claims.
    if [ -n "$verify_exp" ]; then
        if [ "$verify_exp" -lt 0 ] || [ "$verify_exp" -gt 255 ]; then
            verify_bad=$((verify_bad+1))
            echo "  ★ $page:$ln — VERIFY: exit $verify_exp is outside 0..255; an exit status cannot carry it"
            echo "      Compare it inside the program instead: \`if f(x) != $verify_exp { return 1 }\` with VERIFY: exit 0"
        elif ! echo "$body" | grep -qE "^[[:space:]]*return[[:space:]]+$verify_exp([[:space:]]+//.*)?[[:space:]]*$"; then
            verify_bad=$((verify_bad+1))
            echo "  ★ $page:$ln — VERIFY claims exit $verify_exp but the block has no literal \`return $verify_exp\`"
            echo "      An exit status is 8 bits, so a COMPUTED return aliases mod 256 and could"
            echo "      confirm a wrong value. Assert it inside the program and return a literal."
        fi
    fi
    # make it a program
    # ★ The program is written into the REPOSITORY ROOT, not into $TMP. Module paths resolve
    # relative to the source file's directory, so an example that says `import std.c.{…}` can
    # only be checked from the directory that contains `std/`. Written to a temp dir, every
    # such example failed for a reason that had nothing to do with the example.
    prog="$PROG"
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
                        USAGE.md)    unchecked_usage=$((unchecked_usage+1))   ;;
                        *)           unchecked_other=$((unchecked_other+1))   ;; esac
        continue
    fi
    out=$("$LAIN" "$prog" -o "$PROGC" 2>&1); rc=$?
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
        #
        # ★ THE LIST WAS INCOMPLETE and that is how it fails: a code missing from it files a
        # false SAFETY claim under "backlog". E121 was missing, so §8.3.2's documented lexer
        # loop — which breaks a struct field's invariant — counted as an unverifiable fragment
        # rather than as unsafe documented code. So were E001-E003 (linearity), E011 (a
        # documented loop that is not provably terminating, which the wrapper's row withholds
        # `diverge` precisely in order to catch), E015, E016, E010 and E125.
        # That is the page making a false claim, and filing it under "backlog" hides it: the
        # `int` alias fix turned five such fragments red and this bucket absorbed all five
        # without the total moving enough to notice.
        if echo "$out" | grep -qE '^\[E(001|002|003|004|005|007|010|011|015|016|019|082|085|086|121|125|126|130|131)\]'; then
            falseclaim=$((falseclaim+1))
            echo "  ★ $page:$ln — fragment draws a PROOF diagnostic: the documented code is not safe"
            echo "$out" | grep -m1 -E '^\[E' | sed 's/^/      /'
        fi
        unchecked=$((unchecked+1))
        case "$page" in README.md)   unchecked_readme=$((unchecked_readme+1)) ;;
                        LANGUAGE.md) unchecked_lang=$((unchecked_lang+1))     ;;
                        USAGE.md)    unchecked_usage=$((unchecked_usage+1))   ;;
                        *)           unchecked_other=$((unchecked_other+1))   ;; esac
        [ $VERBOSE -eq 1 ] && { echo "  UNVERIFIABLE $page:$ln"
                                echo "$out" | grep -m1 -E '^\[E' | sed 's/^/      /'; }
        continue
    fi
    if [ $expect_fail -eq 1 ]; then
        if [ $rc -ne 0 ]; then
            # ★ FAILING IS NOT ENOUGH: THE BLOCK MUST FAIL FOR THE REASON IT NAMES. A block marked
            # `// ERROR [E012]` passed on any non-zero exit, and five did on an unrelated one: a
            # parse error, or a name the fragment never declared (2026-10-03). So a block names its
            # code, and the compiler's output must carry that code.
            named=$(echo "$body" | grep -oE '//.*' | grep -oE 'E[0-9]{3}' | sort -u)
            hit=0
            for c in $named; do echo "$out" | grep -q "\[$c\]" && hit=1; done
            if [ -z "$named" ]; then
                expfail_code=$((expfail_code+1))
                echo "  ★ $page:$ln — an ERROR block that names no code; it fails with $(echo "$out" | grep -m1 -oE '^\[E[0-9]{3}\]'): name it"
            elif [ $hit -eq 0 ]; then
                expfail_code=$((expfail_code+1))
                echo "  ★ $page:$ln — names $(echo $named) but fails with: $(echo "$out" | grep -m1 -E '^\[E' | cut -c1-110)"
            else
                expfail_ok=$((expfail_ok+1))
            fi
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

# ── FLAGS, THE OTHER DIRECTION ───────────────────────────────────────────────────────────
# The check below tests every flag a DOCUMENT names against args.h. It cannot see a flag the
# documents never mention, which is the more likely failure: a flag lands with a feature and
# nobody goes back to the manual.
#
# It happened the day this was written. `--emit-certificate` and `--certificate-roundtrip` landed
# in ea9b479 and Appendix E's own header says "Every flag the compiler accepts, verified against
# the binary" — a sentence that had quietly become false. Nothing in the project could see it.
#
# A flag must therefore be documented OR listed here with a reason. The list is the point: it
# turns "nobody noticed" into "someone decided", and it is short enough to read.
undoc_bad=0
flag_exempt() {
    case "$1" in
        # Accepted and silently ignored: there is one engine and one backend. Appendix E explains
        # in prose why they are deliberately NOT in its table — a no-op documented as a feature is
        # worse than an absent one — so they must not be required to appear in it.
        --engine|--backend) return 0 ;;
    esac
    return 1
}
# Only meaningful against the default pages: asked about an arbitrary page, every flag is
# "undocumented" because that page is not the manual. The forward check is guarded the same way.
for flag in $([ $DEFAULT_PAGES -eq 1 ] && grep -ohE '"--[a-z][a-z0-9-]*' src/frontends/lain/args.h | tr -d '"' | sort -u); do
    flag_exempt "$flag" && continue
    grep -qF -- "$flag" "${PAGES[@]}" 2>/dev/null && continue
    undoc_bad=$((undoc_bad+1))
    echo "  ★ args.h accepts $flag — no document names it, and it is not on the exempt list"
done

# ── FLAGS, THE THIRD DIRECTION ───────────────────────────────────────────────────────────
# A flag must be in args.h, in a document, AND in the compiler's own usage text. The third is
# the one nobody was watching: before I.90 the no-arg screen listed 6 of 15 flags and the
# unknown-option message 13, with `--interpret` in NEITHER — so the only way a user could learn
# about the flag that runs their program was to read args.h. Two lists that must agree and
# nothing making them agree.
#
# I.90 made all three outputs come from one table, which is the real fix; this guards against the
# next flag being added to the parser and not to that table.
usage_bad=0
if [ $DEFAULT_PAGES -eq 1 ] && usage_text=$("$LAIN" --help 2>&1); then
    for flag in $(grep -ohE '"--[a-z][a-z0-9-]*' src/frontends/lain/args.h | tr -d '"' | sort -u); do
        printf '%s' "$usage_text" | grep -qF -- "$flag" && continue
        usage_bad=$((usage_bad+1))
        echo "  ★ args.h accepts $flag — the compiler's own --help does not list it"
    done
fi

# ── DIAGNOSTIC CODES ─────────────────────────────────────────────────────────────────────
# A code named in a document is a claim about the compiler, exactly as a flag name is, and it
# rots the same way: silently, because prose is not compiled.
#
# This exists because LANGUAGE.md's §4.3 — the section explaining the language's defining feature —
# named `[E006]` for "moved inside a loop". E006 was RETIRED and folded into E002, and nothing in
# the project could see that a document still taught it. Four of the five codes in that one table
# were wrong (b5b5a21), and a swap between two live codes is beyond this check's reach; a code that
# does not exist at all is not, and it is one grep.
#
# `in` is deliberately NOT inferred from the annex: the annex is a document too, so checking a
# document against a document proves only that they agree. The authority is the SOURCE.
code_bad=0
emitted_codes=$( { grep -rhoE '\[E[0-9]{3}\]' --include='*.h' --include='*.c' src/ | tr -d '[]'
                   grep -rhoE '"E[0-9]{3}"'   --include='*.h' --include='*.c' src/ | tr -d '"'
                   grep -rhoE '\[W[0-9]{3}\]' --include='*.h' --include='*.c' src/ | tr -d '[]'
                   grep -rhoE '"W[0-9]{3}"'   --include='*.h' --include='*.c' src/ | tr -d '"'
                 } | sort -u)
# Prose names a code as `W130` rather than `[W130]`, so both spellings count. That matters:
# `--no-w130`'s row said "Suppress the `W130` warning" and W130 has not been emitted since
# `proc` was removed — the bracketed-only pattern could not see it.
for code in $( { grep -ohE '\[[EW][0-9]{3}\]' "${PAGES[@]}" 2>/dev/null | tr -d '[]'
                 grep -ohE '`[EW][0-9]{3}`'   "${PAGES[@]}" 2>/dev/null | tr -d '`'
               } | sort -u); do
    echo "$emitted_codes" | grep -qxF "$code" && continue
    # ★ A RETIRED code may be NAMED, provided the text says it is retired. spec_gate settled the
    # same question by counting only Annex B's definitions and not its cross-references, on the
    # grounds that "prose that refers to a retired code is exactly the kind of thing a good annex
    # contains". The markdown analogue: a mention whose own line says removed / retired / no longer
    # is describing history, not claiming a live diagnostic. Every OTHER mention is a claim.
    #
    # Without this the only way to satisfy the gate is to stop naming the code, which would make
    # `--no-w130`'s row unable to explain what the flag was ever for — a gate pushing a document
    # toward saying less.
    if ! grep -hE "(\[$code\]|\`$code\`)" "${PAGES[@]}" 2>/dev/null \
         | grep -qvE 'removed|retired|no longer'; then
        continue
    fi
    code_bad=$((code_bad+1))
    echo "  ★ README/LANGUAGE names [$code] — no source file in src/ emits it"
    grep -nH "\[$code\]" "${PAGES[@]}" 2>/dev/null | head -3 | sed 's/^/      /'
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
echo "  fail, but not with the code named : $expfail_code   ← verified by accident"
# Split by PAGE, because the two are held to different standards and the combined number reads
# as a regression on the one that is clean. README.md is the showcase and its count is 0 by
# policy; LANGUAGE.md is the old manual and its count is a backlog. Reading "66" against a
# recorded "README has zero unverifiable fragments" cost a real detour before this split.
echo "  UNVERIFIABLE fragments   : $unchecked   ← not noise: a claim nobody tests"
[ $synopsis -gt 0 ] && echo "  SYNOPSIS blocks (a form, not a program): $synopsis"
echo "      README.md   : $unchecked_readme   <- must stay 0"
echo "      LANGUAGE.md : $unchecked_lang   <- the old manual: a backlog, not a regression"
echo "      USAGE.md    : $unchecked_usage   <- must stay 0"
  # ★ PRINT IT WHENEVER IT IS NON-ZERO, not only under --pages. The split below the total must
  # ACCOUNT FOR the total: a count this gate computes and then hides is a backlog nobody sees, and
  # on a default run the "other" line was printed by nothing. It is 0 today because the default
  # pages are exactly README and LANGUAGE — but the next page added to that list would land here
  # silently, and the two numbers would stop adding up with no line saying so.
  { [ $DEFAULT_PAGES -eq 0 ] || [ $unchecked_other -gt 0 ]; } && \
      echo "      other pages : $unchecked_other   <- a backlog, not a regression"
  split_sum=$((unchecked_readme + unchecked_lang + unchecked_usage + unchecked_other))
  [ $split_sum -ne $unchecked ] && \
      echo "  ★ the split above sums to $split_sum but the total is $unchecked — a page is uncounted"
echo "  FLAGS named but not accepted : $flag_bad   ← a claim about the binary, now tested"
echo "  CODES named but not emitted  : $code_bad   ← a retired or misspelt code"
echo "  FLAGS accepted but undocumented : $undoc_bad   ← a flag landed, the manual did not"
echo "  FLAGS absent from --help        : $usage_bad   ← the compiler's own help, now tested"
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
bucket_sum=$((ok + fail + expfail_ok + expfail_bad + expfail_code + unchecked + synopsis))
if [ "$bucket_sum" -ne "$extracted" ]; then
    echo "  ★ $extracted blocks extracted but $bucket_sum judged — $((extracted - bucket_sum)) fell through"
    accounting_bad=1
fi
echo "  blocks extracted, all accounted for : $extracted"
echo "=================================================================="
[ $fail -eq 0 ] && [ $expfail_bad -eq 0 ] && [ $expfail_code -eq 0 ] && [ $flag_bad -eq 0 ] && [ $falseclaim -eq 0 ] \
    && [ $verify_bad -eq 0 ] && [ $accounting_bad -eq 0 ] && [ $code_bad -eq 0 ] \
    && [ $undoc_bad -eq 0 ] && [ $usage_bad -eq 0 ] && exit 0 || exit 1
