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
#include "ir/interp.h"   // --interpret: the IR's semantics, executable (DECIDE-W step 1)
#include "static_eval.h"  // DECIDE-W step 2: module constants computed at compile time
#include "analysis/containment.h"   // --check-invariants: the analysis's states against a real run

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

    // 3) one dotted segment per path component, each an identifier. ★ The name was the path
    // with '/' turned into '.', and a dot INSIDE a component survived, so the name could not be
    // turned back into the path: `v1.2/a.ln` became `v1.2.a`, whose file is `v1/2/a.ln`, and the
    // root file was read from that (a different file, if one existed). The root is now read from
    // the path as given (module_root_file); the name only has to be a valid, distinct identifier
    // path, so any other character becomes '_' and a component starting with a digit gets one
    // in front (`1prog.ln` was refused: its constants mangled to C names starting with a digit).
    char *tmp = arena_push_many(arena, char, 2 * end + 1);
    size_t o = 0; bool seg_start = true;
    for (size_t i = 0; i < end; i++) {
        char c = p[i];
        if (c == '/' || c == '\\') { tmp[o++] = '.'; seg_start = true; continue; }
        bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (seg_start && c >= '0' && c <= '9') tmp[o++] = '_';
        tmp[o++] = ident ? c : '_';
        seg_start = false;
    }
    tmp[o] = '\0';
    return tmp;
}


int main(int argc, char **argv) {
    Arena ast_arena  = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena _sema_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);

    Args args = args_parse(argc, argv);

    // C.2: read a certificate and print it back. The format's own test: emitting, parsing and
    // re-emitting must give the same text (tests/codegen/certificate_roundtrip.sh), because the
    // checker (C.3) reads what the compiler writes.
    if (args.certificate_roundtrip) {
        FILE *in = fopen(args.certificate_roundtrip, "r");
        if (!in) { fprintf(stderr, "lain: cannot open '%s'\n", args.certificate_roundtrip); return 1; }
        char err[256]; CertFunc *c = cert_parse(in, err, sizeof err); fclose(in);
        if (!c) { fprintf(stderr, "lain: %s: %s\n", args.certificate_roundtrip, err); return 1; }
        cert_print(c, stdout); cert_free(c);
        return 0;
    }
    // C.3a: the analysis checks these certificates instead of searching (vra.h, vra_check_rebuild).
    if (args.check_certificate) {
        FILE *in = fopen(args.check_certificate, "r");
        if (!in) { fprintf(stderr, "lain: cannot open '%s'\n", args.check_certificate); return 1; }
        char err[256]; vra_check_certs = cert_parse(in, err, sizeof err); fclose(in);
        if (!vra_check_certs && err[0]) { fprintf(stderr, "lain: %s: %s\n", args.check_certificate, err); return 1; }
        vra_check_mode = true;
    }
    // Opened before anything changes directory, so a relative path is the caller's.
    if (args.emit_certificate) {
        vra_cert_out = fopen(args.emit_certificate, "w");
        if (!vra_cert_out) { fprintf(stderr, "lain: cannot write '%s'\n", args.emit_certificate); return 1; }
    }

    // Initialize target config (host auto-detect unless --target= specified).
    target_init_for(args.target_triple);
    sema_w130_silent = args.no_w130;
    sema_dump_effects = args.dump_effects;

    // A relative path that CLIMBS (`../m.ln`, `sub/../m.ln`) cannot name a module: the name is
    // the path with `/` turned into `.`, so `../m.ln` became the module `...m`, whose file is
    // `///m.ln`, and the driver could not open the program it was given. Made absolute against
    // the working directory, it takes the branch below exactly as `lain /abs/m.ln` does: the
    // same module name, the same emitted C.
    if (args.filename && args.filename[0] != '/' && args.filename[0] != '\\') {
        bool climbs = false;
        for (const char *p = args.filename; *p && !climbs; ) {
            const char *q = p;
            while (*q && *q != '/' && *q != '\\') q++;
            if (q - p == 2 && p[0] == '.' && p[1] == '.') climbs = true;
            p = *q ? q + 1 : q;
        }
        static char in_abs[4096];
        char cwd[4096];
        if (climbs && getcwd(cwd, sizeof cwd) &&
            (size_t)snprintf(in_abs, sizeof in_abs, "%s/%s", cwd, args.filename) < sizeof in_abs)
            args.filename = in_abs;
    }

    // C.1 fix: if the user passed an **absolute** path, chdir to its directory
    // so import-based module resolution keeps working. Relative paths are left
    // untouched — the project convention is to invoke lain from the repo root
    // with a relative path so that std/ resolves correctly.
    if (args.filename && args.filename[0] == '/') {
        // ★ ...but `-o` names a file relative to where the user IS, not to where the source is.
        // Resolved after the chdir, `lain /abs/src/x.ln -o out.c` wrote /abs/src/out.c — into
        // the source tree (probe files landed in tests/types/ this way, twice). Every other
        // compiler resolves -o against the working directory; so does this one now.
        static char out_abs[4096];
        if (args.output_file && args.output_file[0] != '/') {
            char cwd[4096];
            if (getcwd(cwd, sizeof cwd) &&
                (size_t)snprintf(out_abs, sizeof out_abs, "%s/%s", cwd, args.output_file) < sizeof out_abs)
                args.output_file = out_abs;
        }
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
                    fprintf(stderr, "lain: cannot chdir to '%s' for module resolution.\n", dirbuf);
                    return 1;
                }
                args.filename = (char *)(last_sep + 1);
            }
        }
    }

    // derive a nice “foo.bar” module‑name from the filename
    // (strip “.ln” and turn “/” into “.”)
    char *modname = filepath_to_modname(&ast_arena, args.filename);

    module_root_file = args.filename;   // read the program from the path given, not from its name
    // NULL is the program with no declarations: a file it cannot open has already been reported,
    // and the root module is never one already loaded. It compiles to a module with nothing in it,
    // as a program without `main` compiles. It was refused as "Could not load root module", with
    // no code, which blamed a file that loaded fine.
    DeclList *program = load_module(&ast_arena, modname);

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
    vra_dump_measures_enabled = args.dump_measures;


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
    // The same walk enforces spec 9's rule that control cannot leave a deferred statement (E138).
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
    bool layout_reported = false;
    if (args.engine_ir) {
        ir_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
        // DECIDE-W step 2: a module constant that calls a function is computed now, by the
        // program's own semantics, and becomes a literal before the module is lowered for real.
        { static Arena se_arena; se_arena = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
          if (ir_static_eval_module(program, args.filename, &se_arena)) { sema_destroy(); return 1; } }
        IrFunc *mod = ir_lower_module(program, &ir_arena);
        ir_mod = mod;
        // How each sum is represented, from the one decision (layout.h), before any analysis:
        // a `T | markers` union that would need a tag is refused here (E064).
        if (ir_emit_layout_report(mod, &ir_arena, args.dump_niche, args.filename)) { sema_destroy(); return 1; }
        if (ir_check_readonly_args(mod, args.filename)) { sema_destroy(); return 1; }
        layout_reported = true;
        lin_mod = mod; bor_loan_mod = mod; vra_mod = mod;
        // ★ AN UNMODELLED CONSTRUCT IS REPORTED FIRST. Lowering turns one into an OPAQUE — an
        // unknown value — so the function is still analysed, and every finding that depends on
        // that unknown is reported: an overflow, a loop that cannot be shown to end, an index.
        // The opaque itself was refused only at emission, which a program with findings never
        // reaches, so the cause was never printed. A module-scope `[0 for i in 0..256]` made
        // E011 and E086 appear on correct loops elsewhere in the file and said nothing at its own
        // line. Nothing can be emitted for such a program anyway: name the cause and stop. The
        // same holds for a function lowering marked `incomplete` (unmodelled control flow); it
        // used to be skipped here with a note and then EMITTED, its checks never run.
        if (ir_emit_refuse_opaque(mod, args.filename)) { sema_destroy(); return 1; }
        int found = 0;
        for (IrFunc *f = mod; f; f = f->next) {
            if (f->is_extern) continue;
            // The source excerpt reads the main file's text, so only a function defined there
            // gets one; an imported module's finding keeps the bare `-->` line, not a wrong line.
            Decl *sd = (Decl *)f->src_decl;
            ir_diag_excerpt = (sd && (!sd->defining_module || strcmp(sd->defining_module, modname) == 0))
                              ? diagnostic_show_line : NULL;
            found += ir_report_findings(f, mod, args.filename, args.engine_ir_numeric);
        }
        ir_diag_excerpt = NULL;
        if (found) { sema_destroy(); return 1; }
        if (ir_require_verdicts(mod)) { sema_destroy(); return 70; }
        if (args.interpret) {             // run it instead of emitting it; the status is the program's
            if (args.check_invariants) { vra_mod = mod; ii_on_block = ct_on_block; }
            int st = ir_interpret_module(mod, args.filename);
            if (args.check_invariants && getenv("LAIN_CONTAINMENT_TRACE"))
                fprintf(stderr, "check-invariants: %ld block entries checked against %ld constraints\n", ct_checked, ct_constraints);
            sema_destroy();
            return st;
        }
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
    if (!layout_reported && ir_emit_layout_report(ir_mod, &ir_arena, args.dump_niche, args.filename)) {
        sema_destroy(); return 1;
    }
    // Refuse BEFORE opening the file: a backend that cannot represent a construct says so,
    // rather than leaving a half-written .c that happens to compile (see ir_emit_refuse_opaque
    // — a zero of the right type is not a diagnosable failure).
    if (ir_emit_refuse_opaque(ir_mod, args.filename)) { sema_destroy(); return 1; }
    FILE *out = fopen(args.output_file, "w");
    if (!out) { fprintf(stderr, "lain: cannot open '%s' for writing.\n", args.output_file); sema_destroy(); return 1; }
    ir_emit_module_c(ir_mod, out, &ir_arena);
    fclose(out);
    sema_destroy();

    return 0;
}
