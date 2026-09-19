#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"

#include <unistd.h> /* chdir */

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "ast_print.h"
#include "module.h"
#include "emit.h"
#include "error.h"
#include "args.h"
#include "target.h"
#include "sema.h"
#include "emit_llvm.h"
#include "ir/lower.h"
#include "analysis/linearity.h"
#include "analysis/borrow.h"
#include "analysis/definite_init.h"
#include "analysis/vra.h"
#include "analysis/report.h"
#include "ir/emit_c.h"

void expr_print_ast(Expr *expr, int depth);
void stmt_print_ast(Stmt *stmt, int depth);
void decl_print_ast(Decl *decl, int depth);
void print_ast(DeclList* decl_list, int depth); // Function prototype

// utility: turn "foo/bar/baz.ln" or "./foo/bar.ln" or "/foo/bar.ln"
//          → "foo.bar.baz"
static char *filepath_to_modname(Arena *arena, const char *path) {
    // 1) skip any leading "./", ".\", "/" or "\"
    const char *p = path;
    while ((p[0] == '.' && (p[1] == '/' || p[1] == '\\')) 
        || p[0] == '/' || p[0] == '\\') {
        if (p[0] == '/' || p[0] == '\\') {
            p += 1;
        } else {
            p += 2;  // skip "./" or ".\"
        }
    }

    // 2) determine length without the ".ln" extension
    size_t n = strlen(p);
    size_t end = (n > 3 && strcmp(p + n - 3, ".ln") == 0)
                 ? n - 3
                 : n;

    // 3) allocate exactly end+1 chars in the AST arena
    char *tmp = arena_push_many(arena, char, end + 1);
    for (size_t i = 0; i < end; i++) {
        char c = p[i];
        tmp[i] = (c == '/' || c == '\\') ? '.' : c;
    }
    tmp[end] = '\0';
    return tmp;
}


int main(int argc, char **argv) {
    // two arenas:
    Arena file_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena _sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);

    Args args = args_parse(argc, argv);

    // Initialize target config (host auto-detect unless --target= specified).
    target_init_for(args.target_triple);
    sema_w130_silent = args.no_w130;
    sema_dump_niche = args.dump_niche;
    sema_dump_effects = args.dump_effects;

    // C.1 fix: if the user passed an **absolute** path, chdir to its directory
    // so import-based module resolution keeps working. Relative paths are left
    // untouched — the project convention is to invoke lain from the repo root
    // with a relative path so that std/ resolves correctly.
    if (args.filename && args.filename[0] == '/') {
        const char *fname = args.filename;
        const char *last_sep = NULL;
        for (const char *p = fname; *p; p++) {
            if (*p == '/' || *p == '\\') last_sep = p;
        }
        if (last_sep && last_sep != fname) {
            size_t dirlen = (size_t)(last_sep - fname);
            char dirbuf[4096];
            if (dirlen < sizeof(dirbuf)) {
                memcpy(dirbuf, fname, dirlen);
                dirbuf[dirlen] = '\0';
                if (chdir(dirbuf) != 0) {
                    fprintf(stderr, "Error: cannot chdir to '%s' for module resolution.\n", dirbuf);
                    return 1;
                }
                args.filename = (char *)(last_sep + 1);
            }
        }
    }

    // derive a nice “foo.bar” module‑name from the filename
    // (strip “.ln” and turn “/” into “.”)
    char *modname = filepath_to_modname(&ast_arena, args.filename);

    DeclList *program = load_module(&file_arena, &ast_arena, modname);
    if (!program) {
        fprintf(stderr, "Could not load root module %s\n", modname);
        return 1;
    }

    if (args.dump_ast) {
        printf("\n\n#### AST ####\n");
        print_ast(program, 0);
        return 0;
    }
    // Stand the LEGACY ownership and bounds checks down: under --engine=ir the sovereign
    // analyses are the authority for exactly those questions, and leaving both on would let
    // the old engine exit() first — the new one would never get to speak.
    if (args.engine_ir) {
        g_suppress_ownership = true;                              // ownership: the IR decides
        if (args.engine_ir_numeric) {
            g_vra_suppress_bounds = true;                         // numerics: only with -full
            // RECURSION termination. The sovereign engine now answers it — a well-founded
            // ranking over a parameter, or over a DIFFERENCE of two parameters (which is what
            // divide-and-conquer descends on), read from the octagon at each self-call — and
            // raises it as an obligation through analysis/report.h. Leaving the legacy check on
            // would let the old engine exit() first and the new verdict would never be heard.
            //
            // NOT the loop half, and the boundary was measured rather than assumed: the
            // sovereign engine raises a loop obligation only inside a `func`, while a written
            // `decreasing` clause is a claim the language accepts on ANY loop. Standing the
            // loop checks down dropped three such claims in `proc`s — see g_suppress_recursion
            // in sema.h. Also staying with the old engine: E091 (the SHAPE of a `decreasing`
            // clause, front-end policy) and MUTUAL recursion (f -> g -> f), which the sovereign
            // check does not model at all.
            g_suppress_recursion = true;
            // ── D-44 ANSWERED, AND THE LOOP HALF STANDS DOWN WITH IT ─────────────────────
            // The blocker was never porting: it was a language question plus a precision debt,
            // and both are now paid.
            //
            // THE LANGUAGE ANSWER: a written `decreasing` is a CLAIM THE COMPILER DEFENDS,
            // wherever it appears — the same relationship `effects ...` has to the effect row.
            // `IrBlock.has_measure` carries the fact per loop, and `vra_analyze` raises the
            // obligation for every `func` loop AND every `proc` loop that carried a measure. A
            // `proc` loop with no measure raises nothing, which is what lets an event loop be
            // written at all.
            //
            // THE PRECISION DEBT: raising it cost 12 corpus programs, because the sovereign
            // loop rule was weaker than the legacy one in four shapes — a variable step, an
            // offset counter (`i + 1 < n`), a bound that is an expression (`n / 2`), and a
            // two-endpoint measure (`lo < hi`) — and because the back-edge test used
            // REACHABILITY where it needed DOMINANCE, which refused every nested loop in the
            // corpus. All five are closed, each with its violating case checked first.
            //
            // STILL WITH THE OLD ENGINE, deliberately: E091 (the SHAPE of a `decreasing`
            // clause) and MUTUAL recursion (f -> g -> f), which the sovereign check does not
            // model. And one claim is weaker than the legacy's: the engine defends "this loop
            // terminates", not "this expression is the measure".
            g_suppress_termination = true;
            // ── OVERFLOW: THE LEGACY PATH IS NOW REDUNDANT ───────────────────────────────
            // Item 1.6 asked this twice. The first run (2026-09-17) said NO: 8 programs
            // regressed and SEVEN were LOST GUARANTEES. Those named three missing NARROWING
            // SITES — a call argument, a struct field initialiser, an enum payload — plus a
            // wrong predicate at the three sites that already existed (`from->bits > to->bits`
            // is a proxy that misses a SIGN CHANGE) and one measure expression the IR never
            // lowered. All closed (D-47, D-49).
            //
            // Re-measured: **zero**. Corpus 723/723 with this set, all eight gates green, and
            // the four numeric fuzzers — which EXECUTE what the engine proved — at zero:
            // fuzz_overflow (UBSan), fuzz_unsigned (exact integer oracle, since no sanitizer
            // sees unsigned wrap), fuzz_termination, fuzz_vra.
            //
            // So the rebuilt engine is now the SOLE authority for every numeric obligation:
            // bounds, division, overflow and termination. What remains with the old engine is
            // E091 (the SHAPE of a `decreasing` clause) and MUTUAL recursion, both deliberate.
            g_suppress_overflow = true;
            //
            // RE-RUN 2026-09-18, after D-47 wired the three missing narrowing sites and
            // `vra_type_may_lose` replaced the width proxy: **8 regressions → 1**, and the one
            // is `measure_underflow_fail` — a `decreasing n - i` whose own expression
            // underflows. The IR does not carry the measure EXPRESSION (only the bit saying one
            // was written), so the sovereign engine cannot see it; lowering it re-computes
            // subexpressions the octagon cannot relate to the guard's copies. That is D-49, and
            // it is a lowering restructure rather than a patch.
            //
            // One program, one named cause. That is what stands between here and retiring the
            // legacy overflow path — and with it, the old half of src/sema/.
        }
    }
    // The octagon is built on a PLAIN compile too — effects.h runs the numeric analysis to
    // settle totality — so the dump has something to print on every path. What --engine=ir-full
    // changes is whether that state is authoritative for bounds and overflow, not whether it
    // exists (src/analysis/report.h returns before the numeric findings otherwise).
    vra_dump_enabled = args.dump_octagon;


    // sema = resolve identifiers → you’d call:
    sema_resolve_module(program, modname, &_sema_arena);

    // ── STAGE 3.5, THE SPLIT ─────────────────────────────────────────────────────────────
    // `--engine=ir` makes the SOVEREIGN IR analyses authoritative for what they actually
    // cover — ownership/linearity, borrows, definite assignment, and the numeric obligations
    // (bounds, overflow, division) — by standing the legacy checks down and reporting the new
    // engine's findings instead. Resolution and typing still come from sema, and the backend
    // is still the old emitter: this is deliberately a SPLIT, not a switchover. The analyses
    // are ready to be authoritative; the backend has not yet earned the proofs (Stage IV).
    static Arena ir_arena;
    IrFunc *ir_mod = NULL;
    if (args.engine_ir) {
        ir_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
        IrFunc *mod = ir_lower_module(program, &ir_arena);
        ir_mod = mod;
        lin_mod = mod; bor_loan_mod = mod; vra_mod = mod;
        int found = 0;
        for (IrFunc *f = mod; f; f = f->next) {
            if (f->is_extern) continue;
            // An unfaithfully lowered function cannot be judged: say so rather than pretend.
            if (f->incomplete) {
                fprintf(stderr, "note: '%.*s' is not fully modelled (%s); its checks are skipped\n",
                        (int)f->name->length, f->name->name,
                        f->incomplete_why ? f->incomplete_why : "unknown");
                continue;
            }
            found += ir_report_findings(f, mod, args.filename, args.engine_ir_numeric);
        }
        if (found) { sema_destroy(); return 1; }
    }

    // then code-gen: proof-carrying LLVM-IR (Phase 1 seam) or the portable C target.
    if (args.emit_llvm) {
        emit_llvm(program, args.output_file);
        sema_destroy();
        return 0;
    }
    // ── STAGE IV: WHICH BACKEND WRITES THE C ─────────────────────────────────────────────
    // `--backend=ir` emits from the IR (src/ir/emit_c.h) instead of from the AST
    // (src/emit/). Introduced as a FLAG rather than a switchover so the change is one line to
    // revert and, more importantly, so the corpus can be run both ways and the difference
    // measured — `emit_gate` compares the two on 399 programs, but the corpus is 723, and the
    // ones it cannot reach are exactly the ones nobody has checked.
    //
    // ⚠ THE GATES CANNOT SEE THE NEW BACKEND TODAY. Every corpus program compiles through the
    // OLD emitter, which is why 251 programs once failed to build under the new one while all
    // eight gates stayed green. Flipping this default is what makes the corpus the new
    // backend's test — and that, not the deletion of a directory, is the real switchover.
    if (args.backend_ir) {
        if (!ir_mod) {
            ir_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
            ir_mod = ir_lower_module(program, &ir_arena);
        }
        FILE *out = fopen(args.output_file, "w");
        if (!out) { fprintf(stderr, "Error: cannot open %s\n", args.output_file); sema_destroy(); return 1; }
        ir_emit_module_c(ir_mod, out, &ir_arena);
        fclose(out);
        sema_destroy();
        return 0;
    }
    emit_source_filename = args.no_line_directives ? NULL : args.filename;
    emit(program, 0, args.output_file);

    sema_destroy();

    return 0;
}
