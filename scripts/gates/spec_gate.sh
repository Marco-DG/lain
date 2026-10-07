#!/usr/bin/env bash
# spec_gate.sh — Annex B is NORMATIVE: "a conforming implementation shall issue a diagnostic
# for every constraint violation in the table below". That is a claim about a set, so check
# the set. Two directions, and both matter:
#
#   MISSING     the compiler emits a code the annex does not define — an implementation
#               diagnosing something the specification never required
#   PHANTOM     the annex defines a code nothing emits — a requirement no implementation meets,
#               including this one
#
# ★ WHAT THIS GATE DOES NOT CHECK — stated here because this comment used to claim it did.
#
# "A code carries ONE meaning" is a RULE OF THE ANNEX, not a check in this script. Nothing below
# counts meanings. What is reported is MISSING, PHANTOM, the build, and the examples, and that is
# all. The rule is real and was earned — E123 once covered both function-pointer arity and
# `assume`-outside-unsafe, E125 both direct ADT access and an understated effect row — but it is
# upheld by whoever edits the annex and by nothing else.
#
# It is drifting now. As of fa8de9f, E009 carries THREE meanings in one row: a field mutated
# through a raw pointer in safe code, a write through a read-only `*T` inside `unsafe`, and a
# write reaching an immutable binding's storage through a reference. Those are one family rather
# than E123's two unrelated rules, so the row is defensible — but it is three, and the only thing
# distinguishing them for a reader who hits one is the message text, which the tests pin with
# EXPECT-TEXT.
#
# A comment claiming a check that does not exist is worse than no comment: it stops people
# looking. Whoever adds a fourth meaning to a row should know that nothing here will object.
#
#   bash spec_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
ANNEX=spec/annexes/B-diagnostics.tex
[ -f "$ANNEX" ] || { echo "no $ANNEX"; exit 2; }

# ★ BOTH SPELLINGS, and the second one is why this line had to change. The old AST engine
# wrote the code INSIDE its format string (`"[E001] Error ..."`), so `\[E###\]` found it. The
# sovereign engine passes the code as a bare argument to `ir_diag`, which adds the brackets —
# `ir_diag(file, line, col, "E001", "...")`. The bracket pattern cannot see that, so for as
# long as both engines existed this gate was measuring the OLD one's diagnostics and calling
# it "the compiler". Deleting src/sema/linearity.h is what exposed it: eight ownership codes
# turned PHANTOM overnight while the compiler went on emitting all eight.
#
# A gate that greps for one implementation's phrasing is scoped to that implementation, and
# says nothing about the other — the same defect as a differential that names only one leg.
emitted=$( { grep -rhoE '\[E[0-9]{3}\]' --include='*.h' --include='*.c' src/ | tr -d '[]'
             grep -rhoE '"E[0-9]{3}"'     --include='*.h' --include='*.c' src/ | tr -d '"'
           } | sort -u)
# Only the codes the annex actually DEFINES — a `\diagcode{E###}` heading a row — not every
# code its prose happens to mention. Scraping bare `E###` made a cross-reference count as a
# definition: folding the retired E006 into E002's entry ("this subsumes E006") left E006
# looking documented-but-never-emitted, i.e. PHANTOM, purely because the explanation named it.
# A spec gate should measure what the spec SPECIFIES, and prose that refers to a retired code
# is exactly the kind of thing a good annex contains.
documented=$(grep -ohE '\\diagcode\{E[0-9]{3}\}' "$ANNEX" | grep -ohE 'E[0-9]{3}' | sort -u)

missing=$(comm -23 <(echo "$emitted") <(echo "$documented"))
phantom=$(comm -13 <(echo "$emitted") <(echo "$documented"))

nm=$(echo "$missing" | grep -c . || true)
np=$(echo "$phantom" | grep -c . || true)

[ "$nm" -gt 0 ] && { echo "MISSING from Annex B (emitted, never specified):"; echo "$missing" | sed 's/^/  /'; }
[ "$np" -gt 0 ] && { echo "PHANTOM in Annex B (specified, never emitted):"; echo "$phantom" | sed 's/^/  /'; }

# ── The specification BUILDS, and the committed PDF IS that build ─────────────────────────────
# ★ Nothing built the spec until 2026-09-30. From 4f46b35 (09-28) on, `\(\lain{pos} < ...\)` put
# \lstinline in math mode, a LaTeX error; spec/Makefile said so and exited 1, and the PDF it had
# written anyway was committed nine times. Every one had NO table of contents and 205-212
# section references printed as "§ ??". The Annex B check above passed throughout: it reads the
# .tex, never the document. So: build a clean copy, require zero errors and zero undefined or
# duplicate references, and require the committed PDF to say what that build says (text
# compared, date lines ignored; a different TeX Live may break lines differently).
#
# ★ A VERDICT ALREADY REACHED ON THESE EXACT BYTES IS NOT RECOMPUTED. The build is most of this
# gate's minute, and a chain gated link by link rebuilt the same spec once per link. The key is the
# sha256 of every file the build can read (the copy, after `make clean`, committed PDF included),
# of this script, and of the TeX and pdftotext versions; only "holds" is stored, so a failure is
# always rebuilt and printed in full. The entry is written to a temporary name and renamed into
# place, so a gate running at the same moment sees a whole entry or none. ONLY THE BUILD is
# cached: the examples below depend on ./lain, which is not in the key, and run every time
# (pinned: a lain emitting another code for E016 failed this gate with the cache warm).
# LAIN_GATE_NOCACHE=1 forces the build; LAIN_GATE_CACHE moves the store (default
# ~/.cache/lain-gates).
bs="SKIPPED (no latexmk/pdflatex)"; bfail=0
if command -v latexmk >/dev/null 2>&1 || command -v pdflatex >/dev/null 2>&1; then
  STMP=$(mktemp -d); trap 'rm -rf "$STMP"' EXIT
  cp -r spec "$STMP/spec"; make -s -C "$STMP/spec" clean >/dev/null 2>&1
  BCACHE="${LAIN_GATE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/lain-gates}/spec-build"
  bkey=$( { (cd "$STMP/spec" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum)
            sha256sum < "$ROOT/scripts/gates/spec_gate.sh"
            latexmk -v 2>&1 | head -3; pdflatex --version 2>&1 | head -1; pdftotext -v 2>&1 | head -1
          } | sha256sum | cut -c1-64)
  if [ "${LAIN_GATE_NOCACHE:-0}" != 1 ] && [ -s "$BCACHE/$bkey" ]; then
    bs="$(cat "$BCACHE/$bkey") [cached: these exact sources were built and compared before]"
  else
    rm -f "$STMP/spec/lain-spec.pdf"
    make -C "$STMP/spec" > "$STMP/build.out" 2>&1; brc=$?
    LOG="$STMP/spec/lain-spec.log"
    nerr=$(grep -ac '^! ' "$LOG" 2>/dev/null); nerr=${nerr:-0}
    nref=$(grep -aE "[Rr]eference .* undefined|Citation .* undefined|multiply.defined" "$LOG" 2>/dev/null | grep -vc 'There were'); nref=${nref:-0}
    if [ "$brc" -ne 0 ] || [ "$nerr" -gt 0 ] || [ "$nref" -gt 0 ] || [ ! -f "$STMP/spec/lain-spec.pdf" ]; then
      bfail=1; bs="FAILS (make rc=$brc, $nerr LaTeX errors, $nref undefined/duplicate references)"
      grep -aE -A3 '^! ' "$LOG" 2>/dev/null | head -12 | sed 's/^/  /'
      grep -aE "[Rr]eference .* undefined|multiply.defined" "$LOG" 2>/dev/null | grep -v 'There were' | head -6 | sed 's/^/  /'
    elif command -v pdftotext >/dev/null 2>&1; then
      D='^(January|February|March|April|May|June|July|August|September|October|November|December) [0-9]{1,2}, [0-9]{4}$'
      pdftotext -q "$STMP/spec/lain-spec.pdf" - | grep -vE "$D" > "$STMP/built.txt"
      pdftotext -q spec/lain-spec.pdf - 2>/dev/null | grep -vE "$D" > "$STMP/committed.txt"
      if cmp -s "$STMP/built.txt" "$STMP/committed.txt"; then
        bs="holds ($(grep -c . "$STMP/built.txt") text lines; the committed PDF is the build of these sources)"
        mkdir -p "$BCACHE" && printf '%s\n' "$bs" > "$BCACHE/$bkey.$$" && mv -f "$BCACHE/$bkey.$$" "$BCACHE/$bkey"
      else
        bfail=1; bs="STALE — spec/lain-spec.pdf is not the build of the sources: run make -C spec"
        diff "$STMP/committed.txt" "$STMP/built.txt" | head -8 | sed 's/^/  /'
      fi
    else
      bs="builds clean (0 errors, 0 undefined references); PDF comparison SKIPPED (no pdftotext)"
    fi
  fi
fi

# ── The specification's EXAMPLES do what they say ────────────────────────────────────────────
# ★ The spec's 69 `laincode` examples were checked by NOTHING (readme_gate reads README.md and
# LANGUAGE.md) until 2026-09-30, when running them found 11 false claims in the normative text:
# a stale `import std.fs`, `defer { … }` that does not parse, examples refused for arithmetic
# that can overflow, and two SECTIONS (the L1–L4 "VRA levels" and the "Omega Test") that
# described the analysis engine deleted on 09-26. Each example is judged exactly as the README's
# are (readme_gate --pages): an `// ERROR`/`[Exxx]` block must fail, any other must compile, and
# a fragment drawing a proof diagnostic is a false safety claim.
ex="SKIPPED (no ./lain; build first)"; exfail=0
# ★ A STALE BINARY MAKES THIS CHECK LIE, and silently. Running the spec's own examples against a
# compiler older than src/ can PASS a normative example the current compiler would reject, which
# is the exact failure this gate exists to prevent. Refuse rather than build: a gate that runs
# `make` in the shared tree swaps the binary under another session's gate mid-run.
if [ -x ./lain ]; then
  if stale=$(find src -type f -newer ./lain -print -quit 2>/dev/null); [ -n "$stale" ]; then
      echo "REFUSING TO RUN: ./lain is older than $stale"
      echo "  The spec's examples would be checked against a stale compiler."
      echo "  Run: make"
      exit 2
  fi
fi
if [ -x ./lain ]; then
  ETMP=$(mktemp -d)
  python3 - "$ETMP/spec.md" spec/chapters/*.tex spec/annexes/*.tex <<'PY'
import sys, os
out, md = sys.argv[1], []
for path in sys.argv[2:]:
    cur = None
    for i, l in enumerate(open(path).read().split("\n"), 1):
        if cur is None and l.strip().startswith("\\begin{laincode}"): cur, start = [], i
        elif cur is not None and l.strip().startswith("\\end{laincode}"):
            md += ["<!-- %s:%d -->" % (os.path.basename(path), start), "```lain"] + cur + ["```", ""]
            cur = None
        elif cur is not None: cur.append(l)
open(out, "w").write("\n".join(md))
PY
  bash scripts/gates/readme_gate.sh --pages "$ETMP/spec.md" > "$ETMP/ex.out" 2>&1; exrc=$?
  nok=$(grep -o 'compile as documented *: *[0-9]*' "$ETMP/ex.out" | grep -o '[0-9]*$')
  nfail=$(grep -o 'illustrate an error, and do fail *: *[0-9]*' "$ETMP/ex.out" | grep -o '[0-9]*$')
  nun=$(grep -o 'UNVERIFIABLE fragments *: *[0-9]*' "$ETMP/ex.out" | grep -o '[0-9]*$')
  # An example carrying `// VERIFY: exit N` is RUN, not merely compiled, so a normative sentence
  # about what a program computes gets the same treatment as one about what it accepts. Surfaced
  # here because a count this gate has and does not print is a check nobody knows exists.
  nver=$(grep -o 'claiming an exit value *: *[0-9]*' "$ETMP/ex.out" | grep -o '[0-9]*$')
  if [ $exrc -eq 0 ]; then
    ex="hold ($nok compile, $nfail illustrate an error and fail, ${nver:-0} run for their value, $nun unverifiable fragments)"
  else
    exfail=1; ex="FAIL — an example does not do what the spec says:"
    grep '★' "$ETMP/ex.out" | sed 's/^/  /'
  fi
  rm -rf "$ETMP"
fi

echo "=================================================================="
echo "Annex B vs the compiler"
echo "  codes emitted        : $(echo "$emitted" | grep -c .)"
echo "  codes documented     : $(echo "$documented" | grep -c .)"
echo "  MISSING from the annex : $nm"
echo "  PHANTOM in the annex   : $np"
echo "The specification build: $bs"
echo "The specification's examples: $ex"
echo "=================================================================="
[ "$nm" -eq 0 ] && [ "$np" -eq 0 ] && [ "$bfail" -eq 0 ] && [ "$exfail" -eq 0 ] && exit 0 || exit 1
