#!/usr/bin/env bash
# certificate_gate.sh: every corpus program's proof certificate checks, and checking it decides
# exactly what the search decided (local/internal/design/certificates.md, C.3a).
#
# For each tests/**/*.ln: compile once writing its certificate (--emit-certificate), then compile
# again CHECKING it (--check-certificate). The check replaces the analysis's fixpoint with one pass
# from the certificate's loop-header states and checks every side fact the certificate states
# (element ranges, return ranges, call-site ranges with their nested certificates, measures,
# mutual-recursion pairs, accumulator bounds) instead of searching for it. Then:
#   - no certificate may be refused (exit 70: the search and the check disagree, a compiler bug);
#   - the second compile's diagnostics, exit status and emitted C must equal the first's.
# M5c measured that a header-only certificate loses none of the corpus's proofs once its side
# facts are present, so ANY difference here is a defect in the search or in the check.
#
# The count of certificates checked is printed and must be large: a gate that checks nothing
# would also report no difference.
#
# WHAT IT SHARES, and so cannot see. The check runs the analysis's own transfer functions
# (vra_transfer_instr, vra_refine_guard) and the same IR. A certificate attests the DERIVATION, not
# the transfers: if `idx in arr` were modelled one element too wide, the certificate would carry the
# wrong fact faithfully and the check would re-derive it and agree. Transfers are covered elsewhere,
# by the per-operation soundness harness (src/tools/soundness_driver.c) and by `lain --interpret`.
# The exact refusal messages are asserted in tests/codegen/certificate_check.sh.
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"
[ -x ./lain ] || { echo "build first: make"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
n=0; same=0; nfun=0; : > "$T/diff"; : > "$T/refused"
for f in $(find tests -name '*.ln' | sort); do
    n=$((n+1))
    a=$(timeout 60 ./lain "$f" -o "$T/a.c" --emit-certificate "$T/c.cert" 2>&1); ra=$?
    k=0; [ -s "$T/c.cert" ] && k=$(grep -c '^certificate ' "$T/c.cert"); nfun=$((nfun + k))
    b=$(timeout 60 ./lain "$f" -o "$T/b.c" --check-certificate "$T/c.cert" 2>&1); rb=$?
    if [ $rb -eq 70 ]; then echo "$f: $(echo "$b" | grep -m1 'internal error')" >> "$T/refused"; rm -f "$T/a.c" "$T/b.c"; continue; fi
    ca=$(echo "$a" | grep -oE '^\[[EW][0-9]{3}\]' | sort | uniq -c | tr -s ' ' | tr '\n' ' ')
    cb=$(echo "$b" | grep -oE '^\[[EW][0-9]{3}\]' | sort | uniq -c | tr -s ' ' | tr '\n' ' ')
    ma=-; mb=-; [ -f "$T/a.c" ] && ma=$(md5sum < "$T/a.c"); [ -f "$T/b.c" ] && mb=$(md5sum < "$T/b.c")
    if [ $ra -eq $rb ] && [ "$ca" = "$cb" ] && [ "$ma" = "$mb" ]; then same=$((same+1))
    else echo "$f: searched rc=$ra [$ca] checked rc=$rb [$cb]$([ "$ma" = "$mb" ] || echo ' (the C differs)')" >> "$T/diff"; fi
    rm -f "$T/a.c" "$T/b.c" "$T/c.cert"
done
nd=$(wc -l < "$T/diff"); nr=$(wc -l < "$T/refused")
echo "=================================================================="
echo "certificate gate: $n programs, $nfun function certificates checked"
echo "  same verdict and C, searched and checked : $same"
echo "  a certificate refused (exit 70)          : $nr   <- must stay 0"
echo "  a verdict or the C differs               : $nd   <- must stay 0"
[ $nr -gt 0 ] && { echo "── refused:"; head -10 "$T/refused"; }
[ $nd -gt 0 ] && { echo "── differs:"; head -10 "$T/diff"; }
echo "=================================================================="
[ $nfun -ge 1000 ] || { echo "only $nfun certificates checked: the gate is not judging the corpus"; exit 1; }
[ $nr -eq 0 ] && [ $nd -eq 0 ]
