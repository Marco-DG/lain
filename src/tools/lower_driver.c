// src/ir/lower_driver.c — standalone driver: parse + typecheck a Lain file with the
// existing frontend, then LOWER each function to IR and dump it (Phase 1.2 harness).
//
// Reuses the real frontend (load_module → sema_resolve_module gives a TYPED AST), so
// this exercises lowering on genuine programs. Not part of the main build.
//
// Build & run:
//   gcc -std=c99 -o /tmp/lowerdrv src/ir/lower_driver.c -I src
//   /tmp/lowerdrv tests/ir/sum_maxi.ln
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"
#include <unistd.h>
#include "frontends/lain/lexer.h"
#include "frontends/lain/parser.h"
#include "frontends/lain/ast.h"
#include "frontends/lain/module.h"
#include "target.h"
#include "frontends/lain/sema.h"
#include "frontends/lain/lower.h"
#include "ir/dump.h"
#include "ir/emit_c.h"

// module name = basename without ".ln" (mirrors main.c's private helper)
static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, end+1);
    memcpy(t, p, end); t[end] = '\0';
    return t;
}

int main(int argc, char **argv) {
    // ── DEFAULT TO THE SHIPPING CONFIGURATION ────────────────────────────────────────────
    // This driver predates the flip, so its default was the OLD split: every legacy check on,
    // the sovereign engine merely observing. The compiler has not been that since 2026-09-17,
    // and the mismatch was not academic — it made `emit_gate` and `annot_gate` attribute a
    // LEGACY refusal to the new BACKEND. Six of the eight programs they reported as "the new
    // emitter cannot build this" were programs the legacy engine refused before lowering ever
    // ran, on grounds (a halving measure, an element range) the shipping compiler PROVES.
    //
    // A meter that blames the wrong component does not produce a wrong number, it produces a
    // wrong TARGET — the same failure `precision_loss.sh` had with its glob. Matching the
    // compiler's configuration is what makes a refusal here mean what it says.
    g_suppress_ownership  = true;
    /* g_vra_suppress_bounds: the legacy bounds pass (src/sema/bounds.h) was deleted
       2026-09-23 — there is nothing left to suppress. */
    g_suppress_termination = true;
    g_suppress_overflow   = true;
    // The flags below are kept so a caller can still ask the OLD question explicitly.
    //
    // --reject: stand the LEGACY ownership checks down so lowering completes on a program
    // the old engine would exit() on. Lets the IR of a fail-test be inspected.
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--reject")) g_suppress_ownership = true;
        // --suppress-bounds: stand the LEGACY bounds checker down too, so a program only the
        // NEW engine proves can still be EMITTED and run. Without it the new VRA's own proofs
        // are unfalsifiable — nothing can execute a program the old engine refuses.
        /* --suppress-bounds: accepted and ignored; the legacy bounds pass is gone. */
        // --suppress-term: same seam for the legacy TERMINATION diagnostics. Needed to
        // EMIT a program the old engine refuses on those grounds, which is the only way
        // to execute one and check that a loop proven terminating actually terminates
        // (scripts/fuzz/fuzz_termination.sh).
        if (!strcmp(argv[i],"--suppress-term")) g_suppress_termination = true;
        if (!strcmp(argv[i],"--suppress-ovf")) g_suppress_overflow = true;
    }
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ln>\n", argv[0]); return 2; }
    Arena file_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ir_arena   = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);

    // chdir to the file's directory so module resolution (std/, siblings) works;
    // use the basename as the module name.
    const char *path = argv[1]; const char *slash = NULL;
    for (const char *q = path; *q; q++) if (*q=='/'||*q=='\\') slash = q;
    if (slash && path[0]=='/') { char dir[4096]; size_t dl=(size_t)(slash-path);
        if (dl<sizeof dir){ memcpy(dir,path,dl); dir[dl]='\0'; if (chdir(dir)!=0){} path=slash+1; } }

    char *modname = drv_modname(&ast_arena, path);
    DeclList *program = load_module(&file_arena, &ast_arena, modname);
    if (!program) { fprintf(stderr, "load failed: %s\n", modname); return 1; }
    sema_resolve_module(program, modname, &sema_arena);

    bool emit_c   = (argc >= 3 && strcmp(argv[2], "--emit-c") == 0);
    // C3: report how much of the program the IR actually MODELS. `incomplete` silently
    // excludes a function from every proof, so every metric is conditioned on this number.
    bool coverage = (argc >= 3 && strcmp(argv[2], "--coverage") == 0);
    int n_total = 0, n_incomplete = 0;
    // Use ir_lower_module, not a hand-rolled loop over bodies: this one dropped every EXTERN
    // declaration, so the emitted C called printf and friends with no prototype in scope —
    // an implicit declaration, and the single largest cause of the new backend failing to
    // build the corpus at all.
    IrFunc *head = ir_lower_module(program, &ir_arena);
    for (IrFunc *f = head; f; f = f->next) {
        if (f->is_extern) continue;
        n_total++; if (f->incomplete) n_incomplete++;
        if (!emit_c && !coverage) { ir_dump_func(f, stdout); fputc('\n', stdout); }
    }
    if (coverage) { printf("coverage %d %d\n", n_incomplete, n_total); return 0; }
    if (emit_c) ir_emit_module_c(head, stdout, &ir_arena);
    return 0;
}
