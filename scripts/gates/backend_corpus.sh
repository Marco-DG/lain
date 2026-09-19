#!/usr/bin/env bash
# backend_corpus.sh — EVERY corpus program the compiler accepts, built with BOTH backends and RUN.
#
# ★ WHY THIS EXISTS BESIDE emit_gate. `emit_gate` drives the new backend through
# `lower_driver`, a test harness, and reaches 399 programs. The corpus is 723. The difference is
# not noise: it is every program the driver's own configuration could not reach, and those are
# exactly the ones nobody had checked.
#
# It also asks the question through the REAL COMPILER (`--backend=ir`), which is what a user
# would run — same module loading, same flags, same everything. A backend that works under a
# driver and not under the compiler is not a backend anybody can use.
#
# The reason this matters more than it sounds: every corpus test compiles through the OLD
# emitter today, so `make gates` cannot see the new one at all. 251 programs once failed to
# build under it while all eight gates stayed green. Until `--backend=ir` is the default, THIS
# is the only thing that looks.
cd "/home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain Compiler/lain"
ok=0; ccfail=0; differ=0; refused=0
: > /tmp/cb_fail.txt
for f in $(find tests -name '*.ln' -type f | sort); do
  case "$f" in *_fail.ln) continue;; esac
  flags=$(grep -o 'LAINFLAGS:.*' "$f" | sed 's/LAINFLAGS: *//' | head -1)
  ./lain $flags "$f" -o /tmp/cb_o.c >/dev/null 2>&1 || { refused=$((refused+1)); continue; }
  gcc -o /tmp/cb_o /tmp/cb_o.c -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null || { refused=$((refused+1)); continue; }
  oout=$(/tmp/cb_o 2>/dev/null); orc=$?
  if ! ./lain $flags --backend=ir "$f" -o /tmp/cb_n.c >/dev/null 2>&1; then
    ccfail=$((ccfail+1)); echo "LOWER  $f" >> /tmp/cb_fail.txt; continue; fi
  if ! gcc -o /tmp/cb_n /tmp/cb_n.c -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null; then
    ccfail=$((ccfail+1)); echo "CC     $f" >> /tmp/cb_fail.txt; continue; fi
  nout=$(/tmp/cb_n 2>/dev/null); nrc=$?
  if [ "$oout" = "$nout" ] && [ "$orc" = "$nrc" ]; then ok=$((ok+1));
  else differ=$((differ+1)); echo "DIFFER $f   old=($orc)'$oout' new=($nrc)'$nout'" >> /tmp/cb_fail.txt; fi
done
echo "=================================================="
echo "WHOLE CORPUS through --backend=ir"
echo "  agree            : $ok"
echo "  behaviour differs: $differ"
echo "  cannot build     : $ccfail"
echo "  (old engine refused / did not build: $refused)"
echo "=================================================="
head -20 /tmp/cb_fail.txt
