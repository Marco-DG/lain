#!/usr/bin/env bash
# bench_gate.sh — the BENCHMARKS are claims too, so compile them the way the corpus is compiled.
#
# `bench/` had no harness of any kind: not the Makefile, not a gate, not a survey. Two of its five
# programs had stopped compiling — at the commit they were written against too, so silently — while
# their READMEs go on quoting throughput figures (13.81 GB/s among them). A number nobody can
# reproduce is the same defect class as a README example that does not compile, and readme_gate has
# enforced that rule for the front page for months.
#
# What this checks, in order of severity:
#   1. BROKEN C  — a program that compiles but whose emitted C gcc rejects. A hard failure: it is
#                  the one class that is unambiguously the compiler's fault.
#   2. REFUSED   — a program the compiler will not accept. Reported per program WITH its diagnostic
#                  code, because the two current cases are opposite in kind and the code says which:
#                  `wsbench` is refused CORRECTLY (nothing relates its `n` to a raw pointer's
#                  extent, so no bounds proof exists and its "no bounds checks emitted" was
#                  fail-open, not proof), while a precision gap is a compiler defect.
#   3. UNSUPPORTED — a directory whose README quotes a measurement AND whose program does not
#                  compile. That pair is the actual claim defect, and it is what this prints last so
#                  it is the line a reader ends on.
#
# It exits non-zero only on BROKEN C. The refusals are a distance to report, not a regression to
# block: deciding whether `wsbench` should be migrated to the provable sized form changes what the
# benchmark measures, and that is the author's call, not a gate's.
#
#   bash scripts/gates/bench_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
LAIN=./lain
[ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

total=0 ok=0 refused=0 brokenc=0 unsupported=0
refused_list="" brokenc_list="" unsupported_list=""

for src in $(find bench -name '*.ln' | sort); do
    total=$((total+1))
    cfile="$TMP/$(echo "$src" | tr '/' '_').c"
    if ! out=$("$LAIN" "$src" -o "$cfile" 2>&1); then
        code=$(printf '%s' "$out" | grep -oE '\[[EW][0-9]+\]' | head -1)
        refused=$((refused+1)); refused_list="$refused_list
    $src  ${code:-[no code]}"
        dir=$(dirname "$src")
        if [ -f "$dir/README.md" ] && grep -qE '[0-9]+\.[0-9]+ ?(GB/s|x|×)|[0-9]+ ?(GB/s|ns/|ms)' "$dir/README.md"; then
            unsupported=$((unsupported+1)); unsupported_list="$unsupported_list
    $dir  (README quotes a measurement its program cannot produce)"
        fi
        continue
    fi
    # The emitted C must build. ★ NOT `-w`: the first version of this check used it, and a teeth
    # test (feed it `return undefined_thing();`) showed gcc ACCEPTING invalid C — an implicit
    # function declaration is only a warning, and `-w` silenced it. That is the single most common
    # broken-C shape in this project's history (102 of one survey's 141 backend build failures).
    # These are run_tests.sh's flags, which were chosen by measuring the emitted C, plus
    # implicit-function-declaration as an error.
    if ! gcc -std=c11 -march=native -c -o /dev/null "$cfile" \
            -Wno-discarded-qualifiers -Wno-format-security \
            -Werror=int-conversion -Werror=implicit-int \
            -Werror=implicit-function-declaration \
            -Werror=incompatible-pointer-types \
            -Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_putchar=putchar \
            >/dev/null 2>&1; then
        brokenc=$((brokenc+1)); brokenc_list="$brokenc_list
    $src"
        continue
    fi
    ok=$((ok+1))
done

echo "=================================================================="
echo "bench/ programs"
echo "  compile + emit buildable C : $ok / $total"
echo "  REFUSED by the compiler    : $refused${refused_list}"
echo "  BROKEN C (compiler's fault): $brokenc${brokenc_list}   <- must stay 0"
echo "  UNSUPPORTED CLAIMS         : $unsupported${unsupported_list}"
echo "=================================================================="
[ "$brokenc" -eq 0 ] || exit 1
exit 0
