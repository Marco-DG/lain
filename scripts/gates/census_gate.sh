#!/usr/bin/env bash
# census_gate.sh — the construct census as a gate: does the compiler still mean what the language says?
#
# WHY THIS IS A GATE AND NOT A FUZZER. It is the only instrument whose oracle is OUTSIDE the
# compiler. Lowering can DROP a construct: the function is marked `incomplete`, a note goes to
# stderr, its checks are skipped, and it is EMITTED anyway. The emitted C and `lain --interpret`
# then run the same mutilated IR, so they agree with each other and disagree only with the truth,
# and a sanitizer sees nothing because the code is absent rather than undefined. Measured at
# fb04c22: a `case` on an f64 compiled to `uint8_t f(double) { return; }` and the compiler exited 0.
# Each cell in census_cells.py states the stdout it must produce, computed in Python. That string
# is the only independent leg, so it must never be replaced by what the compiler prints.
#
# VERDICT: a DIFF against census_baseline.txt, not a threshold. A threshold hides compensating
# changes — one cell regressing OK -> MISMATCH while another is fixed MISMATCH -> OK leaves a count
# level, and a diff names both. The gate therefore fails on ANY verdict change in EITHER direction,
# including an improvement, so a fix shows up in review and the commit that makes it blesses the
# baseline.
#
#   bash scripts/gates/census_gate.sh           # check against the committed baseline
#   bash scripts/gates/census_gate.sh --bless   # rewrite the baseline (a deliberate act)
#   LAIN=path/to/lain bash scripts/gates/census_gate.sh
#
# BASELINE RULES. A line may carry OK or REFUSED <code>. It may carry MISMATCH or
# ACCEPTED-ILLFORMED only with a `# <plan row>` reference naming the open item (the row IDs in
# local/internal/design/plan_2026-09-17.md), so a known hole is recorded rather than making the
# gate un-greenable. The gate REFUSES a non-OK, non-REFUSED line without one.
# The refusal CODE is part of the verdict, so E100 -> E012 is a visible change, not a silent one.
#
# DETERMINISM. A timeout is part of the verdict, not a skip: a cell that hangs is MISMATCH. The
# budgets are fixed here rather than taken from the environment, so a loaded machine cannot change
# the answer: 6 s per compiled run, 30 s per interpretation, 20M interpreter steps.
#
# COST. Each cell is 1 lain compile + 2 gcc builds + 2 runs + 1 interpretation. MEASURED at 14 s
# for 146 cells on an idle machine at a224709 (153 cells since the assert-spelling axis). It is that fast only because no cell hangs at the current
# baseline: before the `for`/`continue` fix twelve cells timed out and the same run took minutes.
# So the cost is a function of how many cells are broken, and a sudden slowdown is itself a signal.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
# CELLDIR and the UNJUDGED existence test below are both resolved against cwd, which must be the
# tree root: a cell compiled from an absolute path cannot import anything, and the existence test
# is what separates a genuinely missing module from a harness run in the wrong place.
ROOT="$PWD"
[ -d "$ROOT/tests" ] && [ -d "$ROOT/std" ] || { echo "not at the tree root: $ROOT"; exit 2; }
LAIN="${LAIN:-$(pwd)/lain}"; [ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
CELLS="$HERE/census_cells.py"; BASE="$HERE/census_baseline.txt"
[ -f "$CELLS" ] || { echo "missing $CELLS"; exit 2; }
BLESS=0; [ "${1:-}" = "--bless" ] && BLESS=1
SC="$(mktemp -d)"
# Cells are written HERE, under the tree root and by a RELATIVE path, not in $SC. A cell that
# imports a module cannot be compiled from an absolute path: lain chdirs to the file's directory
# and `std/...` stops resolving (measured: absolute rc=1 "Cannot open module file", tree-relative
# rc=0). No component may begin with a dot, or the path is rebuilt from the module name and a
# DIFFERENT file is read.
CELLDIR="local/censusrun"; mkdir -p "$CELLDIR"
trap 'rm -rf "$SC" "$CELLDIR"' EXIT

LAIN="$LAIN" CELLS="$CELLS" SC="$SC" CELLDIR="$CELLDIR" python3 - > "$SC/now.txt" <<'PY'
import json, os, re, subprocess, sys, tempfile
LAIN, CELLS = os.environ["LAIN"], os.environ["CELLS"]
DEFS = ["-Dlibc_printf=printf", "-Dlibc_puts=puts"]
RUN_T, INT_T, STEPS = 6, 30, "20000000"
cells = json.loads(subprocess.run([sys.executable, CELLS], capture_output=True, text=True).stdout)
d = os.environ["CELLDIR"]          # tree-relative, so an importing cell resolves std/
assert not any(part.startswith(".") for part in d.split(os.sep) if part), \
    "CELLDIR must have no dotted component: the root path is rebuilt from the module name"
def code_of(t):
    m = re.search(r'\[E\d+\]', t)
    return m.group(0) if m else "uncoded"
for x in cells:
    f, cf = os.path.join(d, "c.ln"), os.path.join(d, "c.c")
    open(f, "w").write(x["prog"])
    if os.path.exists(cf): os.remove(cf)
    r = subprocess.run([LAIN, f, "-o", cf], capture_output=True, text=True, timeout=120)
    ill = x["want"] == "__ILLFORMED__"
    key = "%s\t%s" % (x["axis"], x["cell"])
    # A cell the compiler could not LOAD is not a verdict. The cell file lives in a temp
    # directory and lain chdirs there, so a cell that ever gains an `import` would fail to
    # resolve it; without this that failure reads as REFUSED and blesses into the baseline.
    # Both spellings: pre-P "Cannot open module file", post-P "lain: cannot open".
    _blob = r.stdout + r.stderr
    # Three spellings, and an EXISTENCE test for the module one: after P a legitimately missing
    # module and a harness run from the wrong directory print the same message, and only whether
    # the named file is actually there tells them apart.
    _unjudged = ("Cannot open module file" in _blob) or ("lain: cannot open" in _blob)
    if not _unjudged:
        _m = re.search(r"\(no file '([^']*)'\)", _blob)
        _unjudged = bool(_m) and os.path.isfile(_m.group(1))
    if _unjudged:
        print("%s\tUNJUDGED-could-not-load" % key); continue
    if r.returncode != 0 or not os.path.exists(cf):
        v = ("ok-REFUSED " if ill else "REFUSED ") + code_of(r.stdout + r.stderr)
        print("%s\t%s" % (key, v)); continue
    if ill:
        print("%s\tACCEPTED-ILLFORMED" % key); continue
    outs = {}
    for opt in ("-O0", "-O2"):
        b = os.path.join(d, "b" + opt)
        if subprocess.run(["gcc", opt, "-o", b, cf] + DEFS + ["-w"], capture_output=True).returncode:
            outs[opt] = "<C FAILED>"; continue
        try: outs[opt] = subprocess.run([b], capture_output=True, text=True, timeout=RUN_T).stdout
        except subprocess.TimeoutExpired: outs[opt] = "<HUNG>"
    try:
        outs["interp"] = subprocess.run([LAIN, f, "--interpret"], capture_output=True, text=True,
            env={**os.environ, "LAIN_INTERP_STEPS": STEPS}, timeout=INT_T).stdout
    except subprocess.TimeoutExpired: outs["interp"] = "<HUNG>"
    print("%s\t%s" % (key, "OK" if all(v == x["want"] for v in outs.values()) else "MISMATCH"))
PY
PY_RC=$?

if [ "$PY_RC" -ne 0 ]; then
    echo "REFUSING to proceed: the cell runner itself failed (rc=$PY_RC). This is a harness bug,"
    echo "not a verdict about the language. Nothing is compared and nothing is blessed."
    exit 2
fi
want_n=$(python3 "$CELLS" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
got_n=$(wc -l < "$SC/now.txt")
if [ "$want_n" != "$got_n" ]; then
    echo "REFUSING to proceed: $want_n cells declared but $got_n judged — the runner stopped early."
    exit 2
fi
if grep -q 'UNJUDGED-could-not-load' "$SC/now.txt"; then
    echo "REFUSING to proceed: a cell could not be LOADED (harness bug):"
    grep 'UNJUDGED-could-not-load' "$SC/now.txt" | sed 's/^/  /'
    exit 2
fi
if [ $BLESS -eq 1 ]; then
    { echo "# census baseline — regenerated by census_gate.sh --bless"
      echo "# A non-OK, non-REFUSED verdict needs a trailing '# <plan row>' naming the open item."
      cat "$SC/now.txt"; } > "$BASE"
    echo "blessed: $(grep -vc '^#' "$BASE") cells written to $BASE"
    exit 0
fi
if grep -q 'UNJUDGED-could-not-load' "$SC/now.txt"; then
    echo "FAIL: a cell could not be LOADED by the compiler — the harness is broken, not the language:"
    grep 'UNJUDGED-could-not-load' "$SC/now.txt" | sed 's/^/  /'
    exit 2
fi
[ -f "$BASE" ] || { echo "no baseline; run with --bless first"; exit 2; }

# a non-OK, non-REFUSED baseline line must cite a plan row
bad=$(grep -vE '^#' "$BASE" | grep -E 'MISMATCH|ACCEPTED-ILLFORMED' | grep -v '#' || true)
if [ -n "$bad" ]; then
    echo "FAIL: a baseline hole with no plan-row reference:"; echo "$bad" | sed 's/^/  /'; exit 1
fi
grep -vE '^#' "$BASE" | sed 's/[[:space:]]*#.*//' | sed 's/[[:space:]]*$//' > "$SC/base.txt"
sed 's/[[:space:]]*$//' "$SC/now.txt" > "$SC/cur.txt"
if diff -q "$SC/base.txt" "$SC/cur.txt" >/dev/null; then
    echo "census_gate: hold — $(wc -l < "$SC/cur.txt") cells, every verdict as blessed"
    exit 0
fi
echo "census_gate: FAIL — a cell's verdict changed. Any change fails, including an improvement;"
echo "             fix it or bless it in the commit that intends it."
join -t'	' -j1 -o 0,1.2,2.2 \
  <(awk -F'\t' '{print $1"\t"$2"\t"$3}' "$SC/base.txt" | awk -F'\t' '{print $1"|"$2"\t"$3}' | sort) \
  <(awk -F'\t' '{print $1"|"$2"\t"$3}' "$SC/cur.txt" | sort) 2>/dev/null \
  | awk -F'\t' '$2 != $3 {printf "  %-52s baseline %-22s now %s\n", $1, $2, $3}'
# The `join` above reports every cell present in BOTH files, so a changed verdict is never capped.
# A cell that APPEARS or VANISHES is not in that join, and the raw diff below is capped, so list
# those separately and in full: a renamed or deleted cell must not be able to hide past line 20.
awk -F'\t' '{print $1"|"$2}' "$SC/base.txt" | sort > "$SC/kb.txt"
awk -F'\t' '{print $1"|"$2}' "$SC/cur.txt"  | sort > "$SC/kc.txt"
comm -23 "$SC/kb.txt" "$SC/kc.txt" | sed 's/^/  GONE from the census:  /'
comm -13 "$SC/kb.txt" "$SC/kc.txt" | sed 's/^/  NEW, needs blessing:   /'
diff "$SC/base.txt" "$SC/cur.txt" | head -20 | sed 's/^/  | /'
exit 1
