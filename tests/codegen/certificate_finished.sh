#!/usr/bin/env bash
# An analysis that has printed and released its certificate is no longer certifying. vra_cert_finish
# freed the certificate and set V->cert to NULL but left V->certifying set, so a range query made
# on the finished analysis (vra.h's accumulator queries test `certifying` and then write through
# V->cert) wrote through NULL. The compiler asks no query after the analysis returns, so no gate
# could see it. The Debugger agent's probe, which replays the octagon on the finished analysis,
# crashed on 2 corpus programs. This drives the analysis directly: certify one function, then
# require that it either is not certifying or still has a certificate.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/drv.c" <<'C'
#define main lain_main
#include "frontends/lain/main.c"
#undef main
int main(void) {
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ir_arena   = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);
    DeclList *program = load_module(&ast_arena, "p");
    if (!program) { printf("could not load p.ln\n"); return 2; }
    sema_resolve_module(program, "p", &sema_arena);
    IrFunc *f = NULL;
    for (IrFunc *g = ir_lower_module(program, &ir_arena); g; g = g->next)
        if (g->name && g->name->length == 7 && !memcmp(g->name->name, "p_total", 7)) f = g;   // module-qualified
    if (!f) { printf("no function p_total\n"); return 2; }
    vra_cert_out = tmpfile(); vra_cert_next = f;
    Vra *V = vra_analyze(f);
    long written = ftell(vra_cert_out);
    printf("certificate: %ld bytes; after the analysis: certifying=%d cert=%s\n",
           written, V->certifying ? 1 : 0, V->cert ? "set" : "NULL");
    if (written <= 0) { printf("nothing was certified\n"); return 2; }
    return (V->certifying && !V->cert) ? 1 : 0;
}
C
cat > "$D/p.ln" <<'LN'
func total(n u32 <= 100) u32 {
    var s u32 = 0
    var i u32 = 0
    while i < n {
        s = s + 7
        i = i + 1
    }
    return s
}
func main() i32 {
    return total(3) as i32
}
LN
gcc -std=c99 -O0 -w -I "$ROOT/src" -o "$D/drv" "$D/drv.c" -lm || { echo "the driver does not build"; exit 1; }
cd "$D" && ./drv
