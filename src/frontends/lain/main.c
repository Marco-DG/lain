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
#include "error.h"
#include "args.h"
#include "target.h"
#include "sema.h"
#include "emit_llvm.h"
#include "frontends/lain/lower.h"
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
    // ── THE LEGACY SEAMS, NOW PERMANENT ──────────────────────────────────────────────────
    // These used to be conditional on `--engine=ir`, standing the old AST checks down so the
    // sovereign engine could speak (the old one exit()s first, so leaving both on meant the
    // new verdict was never heard). The old checks are DELETED as of 2026-09-23 — ownership,
    // borrows, definite assignment, bounds, overflow and termination are the IR's, and have
    // been the default since 2026-09-08.
    //
    // The flags remain only because a few blocks inside sema.h and typecheck.h still read
    // them; they are set unconditionally, so those blocks are now unreachable. They are the
    // last of the old engine and are marked for removal as a unit.
    //
    // ★ `--engine=legacy` IS GONE, and removing it was not tidiness. The flag selected passes
    // that no longer exist, so it would have silently compiled with NO ownership, bounds or
    // overflow checking at all — a fail-open with a friendly name. A flag whose implementation
    // has been deleted must be deleted with it.

    vra_dump_enabled = args.dump_octagon;


    // sema = resolve identifiers → you’d call:
    sema_resolve_module(program, modname, &_sema_arena);

    // ── E106, AND IT USED TO LIVE IN THE BACKEND ─────────────────────────────────────────
    // "use of undeclared identifier" was raised by src/emit/expr.h, which was fine only while
    // the AST emitter was the one backend: with the C emitted from the IR, that code never
    // runs, the program is ACCEPTED, and `void v0;` goes to the C compiler. A guarantee that
    // holds in one backend and not the other is not a guarantee of the language.
    //
    // It runs HERE — after resolution, monomorphization and UFCS, before any analysis — for
    // the reason its old comment gave: earlier than this, an unbound name is not yet evidence.
    // Before the analyses, because an undeclared name makes a function unjudgeable, and the
    // sovereign engine was reporting `return x + missing` as "arithmetic is not provably free
    // of overflow" — a confusing message about the wrong thing (src/ir/lower.h marks the
    // function incomplete for exactly this, and that seam stays as the fail-closed backstop).
    if (sema_check_undeclared(program, args.filename)) { sema_destroy(); return 1; }

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
        // Refuse rather than ship a plausible-looking wrong answer — the same rule the C
        // backend follows (ir_emit_refuse_opaque). This path models a small integer subset;
        // anything else used to become a comment, and the file still looked like LLVM IR.
        int unmodelled = emit_llvm(program, args.output_file);
        if (unmodelled > 0) {
            fprintf(stderr, "[E100] Error: the LLVM path cannot model %d construct(s) in this "
                            "program, so it has not been translated.\n", unmodelled);
            fprintf(stderr, "       It covers integer functions with + - * and comparisons, "
                            "`if`, and `return` — see the note in frontends/lain/emit_llvm.h.\n"
                            "       The C backend is the complete one; drop --emit-llvm.\n");
            sema_destroy();
            return 1;
        }
        sema_destroy();
        return 0;
    }
    // ── THE BACKEND ──────────────────────────────────────────────────────────────────────
    // The C is emitted from the IR (src/ir/emit_c.h). `src/emit/`, the AST emitter, was
    // DELETED on 2026-09-23 after its answers were recorded: tests/BASELINE.txt holds, for
    // every corpus program, what it printed, its exit code, and how every type it declared was
    // REPRESENTED — generated from that backend while it still existed, then verified against
    // this one (behaviour identical; two programs this backend compiles and that one could
    // not). `baseline_gate.sh` keeps asking those questions with the implementation gone.
    if (!ir_mod) {
        ir_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
        ir_mod = ir_lower_module(program, &ir_arena);
    }
    // Refuse BEFORE opening the file: a backend that cannot represent a construct says so,
    // rather than leaving a half-written .c that happens to compile (see ir_emit_refuse_opaque
    // — a zero of the right type is not a diagnosable failure).
    if (ir_emit_refuse_opaque(ir_mod, args.filename)) { sema_destroy(); return 1; }
    FILE *out = fopen(args.output_file, "w");
    if (!out) { fprintf(stderr, "Error: cannot open %s\n", args.output_file); sema_destroy(); return 1; }
    ir_emit_module_c(ir_mod, out, &ir_arena);
    fclose(out);
    sema_destroy();

    return 0;
}
