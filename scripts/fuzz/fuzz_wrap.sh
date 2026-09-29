#!/usr/bin/env bash
# fuzz_wrap.sh — the explicit-policy operators (`+% -% *% /%`, `+| -| *| /|`, `+? -? *?`, and the
# casts `as% as| as?`) against an exact Python model, AT the boundaries, under UBSan. See
# fuzz_wrap.py for why: every other
# generator keeps its values inside the type, so none ever executed a wrapping operator at the
# overflow it exists to define — and that is where the backend had compiled it to C signed
# overflow (undefined), an int-promotion overflow, and an unwrapped odd width.
#
#   bash fuzz_wrap.sh [N]        # default 200 programs; LAIN=path/to/lain to test another build
set -u
cd "$(dirname "$0")/../.."
N="${1:-200}"; SEED=${RANDOM_SEED:-$$}
SC="${TMPDIR:-/tmp}/fuzz_wrap.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
LAIN="${LAIN:-./lain}"
[ -x "$LAIN" ] || { echo "build first"; exit 2; }
ran=0; refused=0; ub=0; wrong=0; brokenc=0
for ((k=0; k<N; k++)); do
    p="$SC/w.ln"
    python3 scripts/fuzz/fuzz_wrap.py $((SEED + k)) > "$p" || continue
    if ! "$LAIN" "$p" -o "$SC/w.c" >"$SC/w.err" 2>&1; then
        # Every program here is defined, so a refusal is a finding. Name the refused LINE: a
        # program holds up to a dozen checks and the operator is what the report is about.
        refused=$((refused+1))
        ln=$(grep -m1 -o 'Ln [0-9]*' "$SC/w.err" | grep -o '[0-9]*')
        echo "  REFUSED (seed $((SEED+k))) $(grep -m1 -o '\[E[0-9]*\]' "$SC/w.err") ${ln:+line $ln: $(sed -n "${ln}p" "$p" | sed 's/^ *//')}"
        continue
    fi
    gcc -std=c99 -w -fsanitize=undefined -fno-sanitize-recover=all -o "$SC/w" "$SC/w.c" 2>/dev/null \
        || { brokenc=$((brokenc+1)); echo "  BROKEN-C (seed $((SEED+k)))"; continue; }
    ran=$((ran+1))
    "$SC/w" >/dev/null 2>"$SC/w.ub"; rc=$?
    if grep -q "runtime error" "$SC/w.ub"; then
        ub=$((ub+1)); echo "  ★ UB (seed $((SEED+k))): $(grep -m1 'runtime error' "$SC/w.ub" | cut -c1-120)"
    elif [ $rc -ne 0 ]; then
        wrong=$((wrong+1)); echo "  ★ WRONG RESULT (seed $((SEED+k))): check #$rc"
        grep -n "a$rc \|b$rc \|r$rc \|e$rc " "$p" | sed 's/^/      /'
    fi
done
echo "=============================================================="
echo "fuzz_wrap: gens=$N  executed=$ran  refused=$refused"
[ "$ran" -lt $(( N / 2 )) ] && { echo "  ★ FUZZER DID NOT RUN: only $ran/$N executed"; exit 1; }
echo "  bugs:  UB=$ub  WRONG-RESULT=$wrong  broken-C=$brokenc  refused(defined programs)=$refused"
echo "=============================================================="
[ $ub -eq 0 ] && [ $wrong -eq 0 ] && [ $brokenc -eq 0 ] && [ $refused -eq 0 ] && exit 0 || exit 1
