#!/usr/bin/env bash
# The interpreter's per-instruction hook, `ii_on_instr` (src/ir/interp.h), is what a stepping
# debugger stops on (the Debugger's IR stepping embeds main.c this way, `main` renamed). It must
# see every instruction the interpreter runs, in its own frame, and every block's terminator once
# (`i` NULL), the `return` with its source line. Run on a loop that sums 0 + 1 + 2.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/drv.c" <<'C'
#define main lain_main
#include "frontends/lain/main.c"
#undef main
static long n_instr, n_term, n_block, wrong_frame; static long long steps = -1; static int seen[16], ret_line = -1;
static void on_block(IrFunc *f, IrBlock *b, IVal *v, int nv) { (void)f; (void)b; (void)v; (void)nv; n_block++; }
static void on_instr(IrFunc *f, IrBlock *b, IrInstr *i, IVal *v, int nv) {
    (void)v; (void)nv;
    if (!ii_frame || ii_frame->f != f) wrong_frame++;
    if (i) { n_instr++; steps = ist->steps; if (i->line > 0 && i->line < 16) seen[i->line]++; }
    else   { n_term++; if (b->term.kind == IR_TERM_RET) ret_line = (int)b->term.line; }
}
static void report(void) {
    printf("instr=%ld steps=%lld term=%ld blocks=%ld wrong_frame=%ld ret_line=%d lines=",
           n_instr, steps, n_term, n_block, wrong_frame, ret_line);
    for (int l = 1; l < 16; l++) if (seen[l]) printf("%d:%d ", l, seen[l]);
    printf("\n"); fflush(stdout);
}
int main(int argc, char **argv) {
    ii_on_instr = on_instr; ii_on_block = on_block; atexit(report);
    return lain_main(argc, argv);
}
C
cat > "$D/p.ln" <<'LN'
func main() i32 {
    var s i32 = 0
    var i i32 = 0
    while i < 3 {
        s = s + i
        i = i + 1
    }
    return s
}
LN
gcc -std=c99 -O0 -w -I "$ROOT/src" -o "$D/drv" "$D/drv.c" -lm || { echo "the driver does not build"; exit 1; }
cd "$D"
out=$(./drv p.ln --interpret); rc=$?
echo "$out"
[ $rc -eq 3 ] || { echo "exit $rc, the program returns 3"; exit 1; }
# The interpreter's own count of instructions run, as it stood at the last one.
set -- $(echo "$out" | grep -m1 '^instr=' | sed -E 's/instr=(-?[0-9]+) steps=(-?[0-9]+) term=(-?[0-9]+) blocks=(-?[0-9]+) wrong_frame=(-?[0-9]+) ret_line=(-?[0-9]+).*/\1 \2 \3 \4 \5 \6/')
[ $# -eq 6 ] || { echo "the driver's report is not in the expected shape"; exit 1; }
[ "$1" -gt 0 ] && [ "$1" = "$2" ] || { echo "the hook saw $1 instructions, the interpreter ran $2"; exit 1; }
[ "$3" = "$4" ] || { echo "the hook saw $3 terminators for $4 blocks entered"; exit 1; }
[ "$5" = 0 ]    || { echo "$5 calls not in the running function's frame"; exit 1; }
[ "$6" = 8 ]    || { echo "the return's terminator is at line $6, not 8"; exit 1; }
for l in 2 3 4 5 6; do echo "$out" | grep -qE "[ =]$l:[0-9]+" || { echo "no instruction seen on line $l"; exit 1; }; done
n5=$(echo "$out" | grep -oE ' 5:[0-9]+' | cut -d: -f2)
[ "$n5" -ge 3 ] || { echo "line 5 runs three times; the hook saw $n5 instructions there"; exit 1; }
echo "ii_on_instr: every instruction and every terminator, in its frame"
