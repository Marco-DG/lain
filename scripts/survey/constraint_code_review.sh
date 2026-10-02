#!/usr/bin/env bash
# constraint_code_review.sh — a REVIEW LIST, never a gate.
#
# WHY THIS IS NOT A GATE. It pairs each code cited inside a spec `constraint` with Annex B's meaning
# for that code and reports where the two share few words. That is a heuristic: a constraint can
# legitimately cite a code whose annex wording it does not echo, and a wrong citation can happen to
# share vocabulary. A heuristic wired to an exit status trains people to ignore a red build, which
# costs more than the defects it finds. So this prints a ranked list for a human to read and always
# exits 0.
#
# WHAT IT IS FOR. On 2026-10-02, spec 09's constraint "Overwriting a linear variable whose current
# value has not been consumed would leak that value. Such an assignment is ill-formed (E003)" cited
# the wrong code: it is E021, and Annex B defines E021 as "assignment overwrites a linear value that
# still holds a resource". EVERY GATE WAS GREEN, because:
#   · spec_gate checks that each code Annex B DEFINES is one the compiler emits — E003 is;
#   · readme_gate checks that each code README/LANGUAGE NAMES exists — E003 does.
# Neither asks whether a constraint cites the right code FOR THE RULE IT STATES. That is a third
# unchecked surface, and it cannot be gated without knowing which rule a code belongs to.
#
# This would have flagged that one: the constraint's words (overwrite, linear, consumed, assignment,
# leak) overlap E021's wording heavily and E003's ("not consumed before it goes out of scope") only
# on "consumed".
#
#   bash scripts/survey/constraint_code_review.sh          # every citation, worst overlap first
#   bash scripts/survey/constraint_code_review.sh 12       # only the 12 most suspect
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT" || exit 2
LIMIT=${1:-0}

python3 - "$LIMIT" <<'PY'
import re, sys, glob, os

limit = int(sys.argv[1])

# Annex B's meaning per code: the \textit{...} beside each \diagcode{E###} row, plus the
# "additional diagnostics" table, whose rows are `\diagcode{E###} & text \\`.
annex = {}
src = open("spec/annexes/B-diagnostics.tex", encoding="utf-8").read()
for m in re.finditer(r'\\diagcode\{([EW]\d{3})\}\s*\n\s*&[^&]*&\s*\\textit\{(.*?)\}', src, re.S):
    annex.setdefault(m.group(1), m.group(2))
# The "additional diagnostics" rows are `\diagcode{E###} & text \\[2pt]`, and the text CONTAINS
# LaTeX macros — `\lain{...}`, `\S\,\ref{...}`. An earlier version excluded backslashes from the
# text and so silently dropped E106, E121 and E132, then reported them as "not defined in Annex B".
# A survey that mis-parses its own authority invents findings, which is worse than finding none.
for m in re.finditer(r'\\diagcode\{([EW]\d{3})\}\s*&\s*(.+?)\s*\\\\(?:\[|\s*\n)', src, re.S):
    annex.setdefault(m.group(1), m.group(2))

def words(t):
    t = re.sub(r'\\lain\{(.*?)\}', r' \1 ', t)
    t = re.sub(r'\\[a-zA-Z]+\**', ' ', t)
    t = re.sub(r'[^a-zA-Z ]', ' ', t)
    stop = {"the","a","an","is","of","to","that","it","in","and","or","shall","not","be","its",
            "this","for","as","by","with","on","at","from","which","than","then","are","was",
            "issued","when","expression","program","ill","formed","value","type","types"}
    return {w.lower() for w in t.split() if len(w) > 2 and w.lower() not in stop}

rows = []
for path in sorted(glob.glob("spec/chapters/*.tex")):
    text = open(path, encoding="utf-8").read()
    for m in re.finditer(r'\\begin\{constraint\}(.*?)\\end\{constraint\}', text, re.S):
        body = m.group(1)
        line = text[:m.start()].count("\n") + 1
        cited = sorted(set(re.findall(r'\\diagcode\{([EW]\d{3})\}', body)))
        if not cited:
            continue
        cw = words(body)
        for code in cited:
            if code not in annex:
                rows.append((-1.0, os.path.basename(path), line, code, "NOT DEFINED IN ANNEX B", body))
                continue
            aw = words(annex[code])
            shared = cw & aw
            denom = len(aw) or 1
            rows.append((len(shared) / denom, os.path.basename(path), line, code,
                         ",".join(sorted(shared)) or "(none)", body))

rows.sort(key=lambda r: r[0])
print("=" * 70)
print("Codes cited inside spec constraints, weakest word-overlap with Annex B first")
print("  A LIST TO READ, NOT A VERDICT. Low overlap is a reason to look, not a defect.")
print("  KNOWN NOISE FLOOR: E100 (\"Miscellaneous\") and E012 (\"type or constraint violation\")")
print("  have generic annex wording, so zero overlap for them is EXPECTED, not suspicious.")
print("  The signal is a code with SPECIFIC wording cited by a rule that does not echo it.")
print("=" * 70)
shown = rows if limit <= 0 else rows[:limit]
for score, f, line, code, shared, body in shown:
    flat = " ".join(body.split())
    flat = re.sub(r'\\lain\{(.*?)\}', r'`\1`', flat)
    flat = re.sub(r'\\[a-zA-Z]+\{?\}?', '', flat)
    s = "undefined" if score < 0 else f"{score:.2f}"
    print(f"  {s:>9}  {f}:{line}  {code}")
    print(f"             shared: {shared}")
    print(f"             rule:   {flat[:120]}")
print(f"\n  {len(rows)} citations across {len({r[1] for r in rows})} chapters."
      f" Showing {len(shown)}.")
print("  Exit is always 0: this is a heuristic, and a heuristic that fails a build gets ignored.")
PY
exit 0
