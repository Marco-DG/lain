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
#   bash scripts/survey/constraint_code_review.sh all      # ...and the generic-code citations too
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT" || exit 2
LIMIT=${1:-0}
[ "${1:-}" = "all" ] && LIMIT=-1

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
# ★ THE GENERIC CODES ARE LISTED SEPARATELY, not merely annotated. E100 is "Miscellaneous" and
# E012 is "type or constraint violation"; their annex wording cannot echo any particular rule, so
# their overlap is zero BY CONSTRUCTION. Left in the main list they were forty of the first forty
# rows, and a reader learns to scroll past the top of the report — the same failure as a heuristic
# wired to a red build, only slower. Separating them leaves the signal section containing signal.
GENERIC = {"E100", "E012"}

signal  = [r for r in rows if r[3] not in GENERIC]
generic = [r for r in rows if r[3] in GENERIC]

def show(label, rs, cap):
    print("=" * 70)
    print(label)
    print("=" * 70)
    for score, f, line, code, shared, body in (rs if cap <= 0 else rs[:cap]):
        flat = " ".join(body.split())
        flat = re.sub(r'\\lain\{(.*?)\}', r'`\1`', flat)
        flat = re.sub(r'\\[a-zA-Z]+\{?\}?', '', flat)
        sc = "undefined" if score < 0 else f"{score:.2f}"
        print(f"  {sc:>9}  {f}:{line}  {code}")
        print(f"             shared: {shared}")
        print(f"             rule:   {flat[:120]}")
    if not rs:
        print("  (none)")

show("SIGNAL — a SPECIFIC code cited by a rule whose words do not echo Annex B's meaning."
     "\n  A list to read, not a verdict. Both real defects found so far looked like this:"
     "\n  E003 cited for an overwrite (spec 09), E006 cited for a loop consume (spec 11)."
     "\n"
     "\n  Still-expected zeroes, so read PAST these rather than stopping at them:"
     "\n   · a constraint that ENUMERATES several codes (spec 04 lists E001, E008, E016 for"
     "\n     'ownership is not suspended inside unsafe') cannot echo any one of their meanings;"
     "\n   · a rule written in symbols or cross-references, whose words this strips."
     "\n  What to look for instead: the cited code's annex meaning describes a DIFFERENT ACT"
     "\n  from the one the rule forbids. That is what both real defects were.",
     signal, limit)
print()
print(f"  {len(generic)} further citations are of GENERIC codes ({', '.join(sorted(GENERIC))}),")
print("  whose annex wording cannot echo a particular rule. Their overlap is zero by")
print("  construction, so they carry no signal and are not listed. Run with `all` to see them.")
if limit < 0:
    print()
    show("GENERIC-CODE CITATIONS — expected noise, shown on request", generic, 0)
print(f"\n  {len(rows)} citations across {len({r[1] for r in rows})} chapters:"
      f" {len(signal)} with signal, {len(generic)} generic.")
print("  Exit is always 0: this is a heuristic, and a heuristic that fails a build gets ignored.")
PY
exit 0
