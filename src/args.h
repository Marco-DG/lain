#ifndef ARGS_H
#define ARGS_H

/*
                   Copyright Marco De Groskovskaja 2023 - 2024
            Distributed under the Boost Software License Version 1.0
                      https://www.boost.org/LICENSE_1_0.txt
*/

#include "utils/common/libc.h"

typedef struct
{
    char*       filename;
    char*       output_file;  // -o flag, defaults to "out.c"
    bool        dump_ast;
    bool        no_w130;           // --no-w130: suppress proc-could-be-func warning
    bool        no_line_directives; // --no-line-directives: suppress #line in emitted C
    bool        dump_niche;         // --dump-niche: print enum niche layout decisions
    bool        dump_effects;       // --dump-effects: print each function's inferred effect row
    bool        dump_octagon;       // --dump-octagon: print the converged octagon state per block
    bool        emit_llvm;          // --emit-llvm: lower to proof-carrying LLVM-IR (Phase 1 seam)
    bool        engine_ir_numeric;  // --engine=ir-full: ALSO make the IR authoritative for the
                                    // NUMERIC obligations (bounds/overflow/division). Measured
                                    // separately because that is where the gap is: the
                                    // ownership analyses are ready to take over, the numeric
                                    // ones still raise obligations the old engine discharges.
    // STAGE IV: emit the C from the IR (src/ir/emit_c.h) rather than from the AST
    // (src/emit/). A flag first, so the corpus can be run both ways and the difference
    // MEASURED — the gates cannot see the new backend while every test compiles through the
    // old one.
    bool        backend_ir;
    bool        engine_ir;          // DEFAULT since 2026-09-08: the sovereign IR analyses
                                    // are authoritative for ownership, borrows and definite
                                    // assignment. `--engine=legacy` gets the old AST engine
                                    // back; `--engine=ir-full` adds the numeric obligations.
                                    // for ownership, borrows, definite assignment and bounds.
                                    // The old sema still resolves and types, and the old
                                    // backend still emits — this is the SPLIT at Stage 3.5,
                                    // not a wholesale switchover: the analyses are ready to be
                                    // authoritative, the backend is not.
    const char* target_triple;      // --target=<triple>, NULL = host
} Args;

static void _args_help(void)
{
    printf("### Lain Compiler ###\n");
    printf("Usage: <path_to_file_to_compile> [options]\n");
    printf("Options:\n");
    printf("  --dump-ast            Print the AST after parsing\n");
    printf("  --no-line-directives  Suppress #line directives in emitted C\n");
    printf("  --dump-niche          Print niche layout decision for every enum\n");
    printf("  --dump-effects        Print each function's inferred effect row (F3.3)\n");
    printf("  --dump-octagon        Print the converged octagon state per block\n");
    printf("  -o <file>             Set output C file (default: out.c)\n");
    printf("  --target=<triple>     Cross-compile target. Supported:\n");
    printf("                          x86_64-linux-gnu, aarch64-linux-gnu,\n");
    printf("                          x86_64-windows-msvc, cortex-m4-bare, host\n");
    printf("                        Default: host auto-detect.\n");
}

static Args args_parse(int argc, char** argv)
{
    if (argc <= 0) { exit(EXIT_SUCCESS); }
    if (argc == 1) { _args_help(); exit(EXIT_SUCCESS); }

    Args args = {0};
    // ── THE DEFAULT ENGINE ───────────────────────────────────────────────────────────────
    // 2026-09-08: the sovereign IR analyses answer for ownership, borrows and definite
    // assignment on a plain compile.
    //
    // 2026-09-17: ...and for the NUMERIC obligations too — bounds, overflow, division,
    // termination. The rebuilt engine is now authoritative for everything it covers, and the
    // AST engine answers only under `--engine=legacy`.
    //
    // What the flip closes: 17 corpus programs the old engine COMPILED and should have
    // refused, each with a test already asserting it must fail — struct fields overflowing,
    // loop accumulators, slice element ranges, `int` arithmetic, a guard evaluated on a
    // wrapped value. What it costs: two corpus programs pinned to `--engine=legacy`, where a
    // loop or comprehension writes values the element seed cannot see because it admits only
    // syntactic constants and runs before the fixpoint.
    //
    // Verified by applying it and running every gate rather than by surveying: 720/720 corpus
    // on both engines, trust 42/0, spec 56/56, readme gate green, nine fuzzers at zero. The
    // survey that preceded it was wrong — it globbed `*_pass.ln` and missed 23 files.
    args.engine_ir = true;
    args.engine_ir_numeric = true;
    args.output_file = "out.c";  // default

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-ast") == 0) {
            args.dump_ast = true;
        } else if (strcmp(argv[i], "--no-w130") == 0) {
            args.no_w130 = true;
        } else if (strcmp(argv[i], "--no-line-directives") == 0) {
            args.no_line_directives = true;
        } else if (strcmp(argv[i], "--dump-niche") == 0) {
            args.dump_niche = true;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            args.dump_effects = true;
        } else if (strcmp(argv[i], "--dump-octagon") == 0) {
            args.dump_octagon = true;
        } else if (strcmp(argv[i], "--emit-llvm") == 0) {
            args.emit_llvm = true;
        } else if (strcmp(argv[i], "--backend=ir") == 0) {
            args.backend_ir = true;
        } else if (strcmp(argv[i], "--backend=legacy") == 0) {
            args.backend_ir = false;
        } else if (strcmp(argv[i], "--engine=legacy") == 0) {
            // The pre-rebuild AST engine. Kept so the differential harnesses can still ask
            // the old question, and so a user hitting a regression has somewhere to stand.
            args.engine_ir = false; args.engine_ir_numeric = false;
        } else if (strcmp(argv[i], "--engine=ir") == 0) {
            // ★ THE SPLIT, AND IT HAD STOPPED EXISTING. This case set `engine_ir` and left
            // `engine_ir_numeric` alone — which was right while the numeric default was false
            // and became a silent no-op the day the default flipped to true (2026-09-17). So
            // `--engine=ir` and `--engine=ir-full` named the same configuration, and every
            // harness passing the former to mean "sovereign ownership, LEGACY numerics" was
            // measuring the full engine instead: engine_ir_gate.sh, and reverse_differential.sh
            // — which is MEASURE-0, the instrument that reports lost guarantees.
            //
            // A flag whose name states a configuration it no longer selects is the same defect
            // as a survey that globs the wrong sample: it does not produce a wrong number, it
            // produces a number about a different question.
            args.engine_ir = true; args.engine_ir_numeric = false;
        } else if (strcmp(argv[i], "--engine=ir-full") == 0) {
            args.engine_ir = true; args.engine_ir_numeric = true;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            args.output_file = argv[++i];
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            args.target_triple = argv[i] + 9;
        } else {
            args.filename = argv[i];
        }
    }
    
    if (!args.filename) {
        printf("Error: No input file specified.\n");
        exit(1);
    }
    
    return args;
}

#endif /* ARGS_H */