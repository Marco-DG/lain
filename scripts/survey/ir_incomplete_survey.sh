#!/usr/bin/env bash
# ir_incomplete_survey.sh — what is the NEW pipeline still unable to lower FAITHFULLY?
#
# `IrFunc.incomplete` suppresses every proof over a function, so an unmeasured one silently
# conditions every survey number in this repo (corrective backlog C3). This reports the rate
# AND the ranked reason distribution, turning one opaque count into a work list.
#
#   bash ir_incomplete_survey.sh [N]      # N = how many *_pass.ln files to scan (default 300)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
N="${1:-300}"; DRV=/tmp/lain_incdrv
gcc -std=c99 -w -o "$DRV" src/tools/incomplete_driver.c -I src || { echo "build failed"; exit 2; }
WHY=$(mktemp); trap 'rm -f "$WHY"' EXIT
T=0; I=0; F=0; unmeasured=0
for f in $(find tests -name "*_pass.ln" | head -"$N"); do
  out=$(timeout 10 "$DRV" "$f" 2>/dev/null) || { unmeasured=$((unmeasured+1)); continue; }
  echo "$out" | grep '^WHY' >> "$WHY"
  line=$(echo "$out" | grep '^TOT'); [ -z "$line" ] && { unmeasured=$((unmeasured+1)); continue; }
  T=$((T + $(echo "$line" | cut -d' ' -f2))); I=$((I + $(echo "$line" | cut -d' ' -f3))); F=$((F+1))
done
echo "=================================================================="
printf 'IR faithfulness: %d/%d functions lowered faithfully (%d%%) over %d files\n' \
       $((T-I)) "$T" $(( T>0 ? (T-I)*100/T : 0 )) "$F"
echo "  incomplete = $I   (every proof over these is suppressed)"
# A file the driver could not process is NOT a faithful one — it is one nobody looked at.
# Reporting it keeps the denominator honest: a chdir bug once dropped every stdlib-importing
# program here in silence, and the percentage above looked better for it.
echo "  UNMEASURED files = $unmeasured   (driver failed / no TOT line — not counted above)"
echo "------------------------------------------------------------------"
sort "$WHY" | uniq -c | sort -rn | sed 's/^/  /'
echo "=================================================================="
