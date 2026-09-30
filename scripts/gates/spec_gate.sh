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
# It also checks that a code carries ONE meaning. A diagnostic code identifies a constraint;
# two unrelated constraints under one code make the annex's table unusable for the reader who
# actually hit the error. (E123 covered both function-pointer arity and `assume`-outside-unsafe
# until this gate was written; E125 covered both direct ADT access and an understated effect row.)
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
bs="SKIPPED (no latexmk/pdflatex)"; bfail=0
if command -v latexmk >/dev/null 2>&1 || command -v pdflatex >/dev/null 2>&1; then
  STMP=$(mktemp -d); trap 'rm -rf "$STMP"' EXIT
  cp -r spec "$STMP/spec"; make -s -C "$STMP/spec" clean >/dev/null 2>&1; rm -f "$STMP/spec/lain-spec.pdf"
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
    else
      bfail=1; bs="STALE — spec/lain-spec.pdf is not the build of the sources: run make -C spec"
      diff "$STMP/committed.txt" "$STMP/built.txt" | head -8 | sed 's/^/  /'
    fi
  else
    bs="builds clean (0 errors, 0 undefined references); PDF comparison SKIPPED (no pdftotext)"
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
  if [ $exrc -eq 0 ]; then
    ex="hold ($nok compile, $nfail illustrate an error and fail, $nun unverifiable fragments)"
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
