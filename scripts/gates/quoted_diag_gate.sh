#!/usr/bin/env bash
# quoted_diag_gate.sh — a diagnostic QUOTED in a document must still be what the compiler prints.
#
# readme_gate compiles every ```lain block, so it proves a documented program is still accepted or
# still refused. It compares no OUTPUT TEXT at all. So a page could quote a message the compiler
# stopped printing years ago and every gate would stay green — which is not hypothetical:
#
#   - README quoted "[E085] bounds error: cannot prove index is within bounds for dynamic-length
#     array" with three hint lines and a caret. The real text is "index is not provably within
#     bounds", there are TWO errors not one, and E085 printed no excerpt at all at the time.
#   - LANGUAGE quoted "[E004] cannot mutate 'x' because it is borrowed by 'x'". The real text is
#     "conflicting borrows of the same value".
#
# Both were found by hand, on the day someone happened to look. This is the instrument.
#
# HOW IT PAIRS A QUOTE WITH A PROGRAM. A ```lain block followed, within a few lines, by a plain
# fenced block whose first line starts with [Exxx] or [Wxxx]. The program is compiled and the
# quoted text must appear in its output.
#
# Whitespace is normalised on BOTH sides before comparing, because a quote is reflowed to the
# page width and the compiler's is not. Everything else is compared literally: a changed word is
# a failure, which is the entire point.
#
#   bash scripts/gates/quoted_diag_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT" || exit 2
LAIN=./lain
[ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
if stale=$(find src -type f -newer "$LAIN" -print -quit 2>/dev/null); [ -n "$stale" ]; then
    echo "REFUSING TO RUN: ./lain is older than $stale"; echo "  Run: make"; exit 2
fi

python3 - "$LAIN" README.md LANGUAGE.md <<'PY'
import re, subprocess, sys, tempfile, os

lain, pages = sys.argv[1], sys.argv[2:]
checked = failed = 0
problems = []
all_quotes = []
checked_at = set()

# Count EVERY quoted diagnostic first, so the gate reports coverage rather than only successes.
# "2 checked, all pass" reads as safety; "2 of 5 checked" says where the risk still is.
total_quoted = 0
for page in pages:
    ls = open(page, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(ls):
        t = ls[i].strip()
        # A ```lain block is skipped WHOLE. Walking into one makes its closing fence look like an
        # opening one, which silently swallowed the next quoted block and made the totals disagree.
        if t.startswith("```") and t != "```":
            j = i + 1
            while j < len(ls) and ls[j].strip() != "```":
                j += 1
            i = j + 1
            continue
        if t == "```":
            j = i + 1
            if j < len(ls) and re.match(r'\[[EW]\d+\]', ls[j].strip()):
                total_quoted += 1
                all_quotes.append((page, j + 1, ls[j].strip()[:60]))
            while j < len(ls) and ls[j].strip() != "```":
                j += 1
            i = j + 1
            continue
        i += 1

for page in pages:
    lines = open(page, encoding="utf-8").read().split("\n")
    i = 0
    while i < len(lines):
        if lines[i].strip() != "```lain":
            i += 1; continue
        # collect the program
        j = i + 1; prog = []
        while j < len(lines) and lines[j].strip() != "```":
            prog.append(lines[j]); j += 1
        # look for a quoted diagnostic within the next few lines
        k = j + 1
        while k < len(lines) and k < j + 6 and lines[k].strip() == "":
            k += 1
        if k >= len(lines) or lines[k].strip() != "```":
            i = j + 1; continue
        m = k + 1; quote = []
        while m < len(lines) and lines[m].strip() != "```":
            quote.append(lines[m]); m += 1
        if not quote or not re.match(r'\[[EW]\d+\]', quote[0].strip()):
            i = j + 1; continue

        checked += 1
        checked_at.add((page, k + 2))
        src = "\n".join(prog) + "\n"
        with tempfile.NamedTemporaryFile("w", suffix=".ln", delete=False, dir=".") as f:
            f.write(src); path = f.name
        try:
            r = subprocess.run([lain, path, "-o", os.devnull],
                               capture_output=True, text=True, timeout=60)
            got = (r.stderr or "") + (r.stdout or "")
        finally:
            os.unlink(path)

        norm = lambda s: re.sub(r"\s+", " ", s).strip()
        got_n = norm(got)
        # Compare each quoted LINE, so a failure names the line that drifted rather than the block.
        base = os.path.basename(path)
        for q in quote:
            qn = norm(q)
            if not qn:
                continue
            qn = qn.replace(base, "").replace(base.replace(".ln", ""), "")
            qn = re.sub(r"-->.*", "", qn).strip()     # the file path differs by construction
            qn = re.sub(r"^\d+ \|", "", qn).strip()   # excerpt line numbers differ
            if not qn or qn in ("|", "^"):
                continue
            if qn not in got_n:
                failed += 1
                problems.append((page, k + 1, qn, norm(got)[:150]))
        i = m + 1

print("==================================================================")
print("Diagnostics quoted in README.md / LANGUAGE.md")
print(f"  quoted diagnostics found   : {total_quoted}")
print(f"  checked against a program  : {checked}")
print(f"  lines that no longer match : {failed}")
unpaired = [q for q in all_quotes if (q[0], q[1]) not in checked_at]
if unpaired:
    print(f"  NOT CHECKED (no ```lain program beside them) : {len(unpaired)}")
    for page, line, txt in unpaired:
        print(f"      {page}:{line}  {txt}")
    print("    Give one a program in a ```lain block directly above it and it becomes checked.")
for page, line, q, got in problems:
    print(f"  ✗ {page}:{line}")
    print(f"      quoted : {q}")
    print(f"      actual : {got}")
if failed:
    print("A quoted diagnostic the compiler no longer prints is a claim about the compiler")
    print("that nobody was running. Re-take it from a build; never edit it to match.")
    sys.exit(1)
print("Every quoted diagnostic is still what the compiler prints.")
PY
