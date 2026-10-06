#!/usr/bin/env bash
# The interpreter's failure hook, `ii_on_fail` (src/ir/interp.h), is called when a run fails and
# BEFORE anything leaves the frames, so a debugger can stop there with the stack and the locals
# (the Debugger agent's IR stepping). Without it a failure longjmps past every frame and only the
# message survives. Driven here by the step budget: `spin` loops past it, called from `main`, and
# inside the hook the stack must still be spin above main.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/drv.c" <<'C'
#define main lain_main
#include "frontends/lain/main.c"
#undef main
static int calls, got_status = -1, at_line = -1, depth = 0;
static char top[64], below[64], why[64];
static void on_fail(int status, IrInstr *at) {
    calls++; got_status = status; at_line = at ? (int)at->line : -1;
    snprintf(why, sizeof why, "%.40s", ii_why);
    for (IFrame *fr = ii_frame; fr; fr = fr->up) depth++;
    if (ii_frame && ii_frame->f && ii_frame->f->name)
        snprintf(top, sizeof top, "%.*s", (int)ii_frame->f->name->length, ii_frame->f->name->name);
    if (ii_frame && ii_frame->up && ii_frame->up->f && ii_frame->up->f->name)
        snprintf(below, sizeof below, "%.*s", (int)ii_frame->up->f->name->length, ii_frame->up->f->name->name);
}
static void report(void) {
    printf("calls=%d status=%d at_line=%d depth=%d top=%s below=%s why=%s\n",
           calls, got_status, at_line, depth, top, below, why);
    fflush(stdout);
}
int main(int argc, char **argv) { ii_on_fail = on_fail; atexit(report); return lain_main(argc, argv); }
C
cat > "$D/p.ln" <<'LN'
func spin(n u32) u32 {
    var i u32 = 0
    while i < n {
        i = i + 1
    }
    return i
}
func main() i32 {
    return spin(1000000) as i32
}
LN
gcc -std=c99 -O0 -w -I "$ROOT/src" -o "$D/drv" "$D/drv.c" -lm || { echo "the driver does not build"; exit 1; }
cd "$D"
out=$(LAIN_INTERP_STEPS=500 ./drv p.ln --interpret 2>/dev/null); rc=$?
echo "$out"
[ $rc -eq 97 ] || { echo "exit $rc, the step budget is 97"; exit 1; }
echo "$out" | grep -q '^calls=1 status=97 at_line=[1-9]' || { echo "the hook was not called once, with 97 and a positioned instruction"; exit 1; }
echo "$out" | grep -q 'depth=2 top=p_spin below=main ' || { echo "the frames were not live in the hook (want spin above main)"; exit 1; }
echo "$out" | grep -q 'why=STEP BUDGET EXHAUSTED' || { echo "ii_why was not written before the hook"; exit 1; }
echo "ii_on_fail: called once, before the frames are gone"
