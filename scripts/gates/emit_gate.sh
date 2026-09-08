#!/usr/bin/env bash
# emit_gate.sh — how far is the NEW backend from replacing the old one?
#
# Deleting `src/sema/` (E0.3, the rebuild's finish line) takes the OLD EMITTER with it, so the
# blocker is no longer the analyses — it is whether `src/ir/emit_c.h` can compile the corpus.
# fuzz_ir_codegen.sh already differentials the two emitters, but only over GENERATED programs,
# which are shaped like the bugs we already know about. This runs the real corpus through both
# and compares observable behaviour: stdout and exit code.
#
#   BUILD-FAIL   the new emitter produced C that does not compile  (or lowering refused)
#   DIFFER       both compiled and ran, and disagreed               ← a MISCOMPILE
#   AGREE        identical stdout and exit code
#
#   bash emit_gate.sh [N]
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
LOWERDRV="${LOWERDRV:-/tmp/lowerdrv}"; CC="${CC:-gcc}"; N="${1:-100000}"
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_realloc=realloc -Dlibc_putchar=putchar -Dlibc_calloc=calloc"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
[ -x ./lain ] || { echo "build first"; exit 2; }
gcc -std=c99 -o "$LOWERDRV" src/ir/lower_driver.c -I src 2>/dev/null || { echo "build lowerdrv failed"; exit 2; }

agree=0; differ=0; buildfail=0; oldskip=0
: > "$TMP/bad"; : > "$TMP/bf"
for f in $(find tests -name '*_pass.ln' -type f | sort | head -"$N"); do
  case "$f" in */_tmp/*) continue;; esac
  ./lain --engine=legacy "$f" -o "$TMP/old.c" >/dev/null 2>&1 || { oldskip=$((oldskip+1)); continue; }
  $CC -std=c99 -w -o "$TMP/old" "$TMP/old.c" $DEFS 2>/dev/null || { oldskip=$((oldskip+1)); continue; }
  oout=$("$TMP/old" 2>/dev/null); orc=$?

  if ! "$LOWERDRV" "$f" --emit-c > "$TMP/new.c" 2>/dev/null; then
    buildfail=$((buildfail+1)); echo "  lower-refused  $f" >> "$TMP/bf"; continue; fi
  if ! $CC -std=c99 -w -o "$TMP/new" "$TMP/new.c" $DEFS 2>"$TMP/cc.err"; then
    buildfail=$((buildfail+1))
    echo "  cc-failed      $f   $(grep -m1 'error:' "$TMP/cc.err" | sed 's/.*error: //')" >> "$TMP/bf"; continue; fi
  nout=$("$TMP/new" 2>/dev/null); nrc=$?

  if [ "$oout" = "$nout" ] && [ "$orc" = "$nrc" ]; then agree=$((agree+1))
  else differ=$((differ+1)); echo "  $f   old=($orc)'$oout'  new=($nrc)'$nout'" >> "$TMP/bad"; fi
done
echo "=================================================================="
echo "Stage IV readiness — can the NEW emitter replace the old one?"
echo "  behaviour AGREES      : $agree"
echo "  behaviour DIFFERS     : $differ    ← miscompiles"
echo "  new emitter FAILED    : $buildfail  ← cannot build it at all"
echo "  (old engine rejected / did not build: $oldskip — not the new backend's fault)"
echo "=================================================================="
[ "$differ" -gt 0 ] && { echo "differences:"; cat "$TMP/bad"; }
[ "$buildfail" -gt 0 ] && { echo "build failures (first 25):"; cat "$TMP/bf"; }
[ "$differ" -eq 0 ] && [ "$buildfail" -eq 0 ]
