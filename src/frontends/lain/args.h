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
    bool        no_line_directives; // --no-line-directives: suppress #line in emitted C
    bool        dump_niche;         // --dump-niche: print enum niche layout decisions
    bool        dump_effects;       // --dump-effects: print each function's inferred effect row
    bool        dump_octagon;       // --dump-octagon: print the converged octagon state per block
    bool        dump_measures;      // --dump-measures: each loop's and recursion's termination measure
    char*       emit_certificate;   // --emit-certificate F: each function's proof certificate (C.2)
    char*       certificate_roundtrip; // --certificate-roundtrip F: parse a certificate, print it back
    char*       check_certificate;  // --check-certificate F: check F's certificates instead of searching (C.3a)
    bool        emit_llvm;          // --emit-llvm: lower to proof-carrying LLVM-IR (Phase 1 seam)
    bool        interpret;          // --interpret: run the accepted program on the IR's own semantics
                                    // (src/ir/interp.h) instead of emitting C; every discharged proof
                                    // is checked as it is used
    bool        check_invariants;   // --check-invariants (with --interpret): the range analysis's
                                    // state at each block must contain the running program's
                                    // (src/analysis/containment.h)
    bool        engine_ir_numeric;  // --engine=ir-full: ALSO make the IR authoritative for the
                                    // NUMERIC obligations (bounds/overflow/division). Measured
                                    // separately because that is where the gap is: the
                                    // ownership analyses are ready to take over, the numeric
                                    // ones still raise obligations the old engine discharges.
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

// ★ ONE TABLE OF FLAGS (I.90). The usage screen and the unknown-option message were two lists
// kept by hand, and they had drifted from the parser and from each other: the usage screen named
// 6 of the 15 flags accepted, the unknown-option message 13, and `--interpret`, the flag that runs
// a program on the IR's own semantics, was in neither (Documentation). Both now print this table.
// A flag the parser below accepts must have a row here; readme_gate checks that every flag in this
// file is documented, and the row is where the text it documents comes from.
typedef struct { const char *flag, *arg, *help; } LainFlag;
static const LainFlag lain_flags[] = {
    { "-o",                      "<file>",   "write the C to <file> (default: out.c)" },
    { "--target=",               "<triple>", "cross-compile target: x86_64-linux-gnu, aarch64-linux-gnu, "
                                             "x86_64-windows-msvc, cortex-m4-bare, host (default: host)" },
    { "--interpret",             NULL,       "run the program on the IR's own semantics instead of emitting C;"
                                             " every discharged proof is checked as it is used" },
    { "--check-invariants",      NULL,       "with --interpret: also check that the range analysis's state at"
                                             " each block contains the running program's" },
    { "--dump-ast",              NULL,       "print the AST after parsing" },
    { "--dump-niche",            NULL,       "print the niche layout decision for every enum" },
    { "--dump-effects",          NULL,       "print each function's inferred effect row" },
    { "--dump-octagon",          NULL,       "print the converged octagon state per block" },
    { "--dump-measures",         NULL,       "print each loop's and recursion's termination measure" },
    { "--emit-certificate",      "<file>",   "write each function's proof certificate to <file>" },
    { "--certificate-roundtrip", "<file>",   "parse a certificate and print it back" },
    { "--check-certificate",     "<file>",   "check each function's proof certificate from <file> instead of searching" },
    { "--no-line-directives",    NULL,       "omit #line directives from the emitted C" },
    { "--emit-llvm",             NULL,       "refused: the C backend is the complete one" },
    { "--help",                  NULL,       "print this text and exit" },
    { "--no-w130",               NULL,       "accepted and ignored: the warning it suppressed was removed" },
    { "--engine=",               "...",      "accepted and ignored: there is one engine" },
    { "--backend=",              "...",      "accepted and ignored: there is one backend" },
};

static void _args_print_flags(FILE *o)
{
    for (size_t k = 0; k < sizeof lain_flags / sizeof lain_flags[0]; k++) {
        char head[64];
        snprintf(head, sizeof head, "%s%s%s", lain_flags[k].flag,
                 lain_flags[k].arg && lain_flags[k].flag[strlen(lain_flags[k].flag) - 1] != '=' ? " " : "",
                 lain_flags[k].arg ? lain_flags[k].arg : "");
        fprintf(o, "  %-30s %s\n", head, lain_flags[k].help);
    }
}

static void _args_help(void)
{
    printf("Lain compiler: lain <file.ln> [options]\n");
    printf("Writes C (out.c unless -o says otherwise), or with --interpret runs the program.\n");
    printf("Options:\n");
    _args_print_flags(stdout);
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
    // wrapped value. What it cost: three corpus programs pinned to `--engine=legacy`, where a
    // loop or comprehension writes values the element seed could not see because it admitted
    // only syntactic constants and ran before the fixpoint.
    //
    // ✅ THAT COST IS NOW ZERO (C14, 2026-09-20). The fixpoint runs twice: the seeding re-runs
    // against pass 0's converged state, and pass 1 uses the result. Twice and no more, so
    // nothing justifies itself in a circle. **No corpus program carries `--engine=legacy` any
    // more** — the sovereign engine judges every one of them.
    //
    // Verified by applying it and running every gate rather than by surveying: 720/720 corpus
    // on both engines, trust 42/0, spec 56/56, readme gate green, nine fuzzers at zero. The
    // survey that preceded it was wrong — it globbed `*_pass.ln` and missed 23 files.
    args.engine_ir = true;
    args.engine_ir_numeric = true;
    // ── THE BACKEND, AND THERE IS ONLY ONE NOW ───────────────────────────────────────────
    // The C is emitted from the IR. `src/emit/`, the AST emitter, was deleted on 2026-09-23;
    // `--backend=ir` / `--backend=legacy` went with it, because a flag that selects between
    // one thing is not a choice.
    //
    // ★ WHAT THE FLAG WAS FOR, AND WHY IT EARNED ITS KEEP. It existed so the corpus could be
    // run BOTH ways and the difference MEASURED, and that measurement found four defects no
    // other instrument could — two of which were not backend bugs at all but checks the OLD
    // BACKEND was enforcing on the language's behalf (E106 undeclared identifiers; an
    // unmodelled construct emitted as `v0 = 0`). Flipping the default was the switchover;
    // deleting the directory is the bookkeeping.
    //
    // ★★ AND THE REFERENCE OUTLIVED THE IMPLEMENTATION. tests/BASELINE.txt records, for every
    // corpus program, what it prints, its exit code, and how every type it declares is
    // REPRESENTED — generated from the AST emitter before it was removed, then verified
    // against this backend: behaviour IDENTICAL, plus two programs this one compiles and that
    // one could not. `baseline_gate.sh` still asks the question that caught D-62 (29 sums
    // silently losing their niche packing while ten gates stayed green), which is the one
    // question no behavioural instrument can ask.
    args.output_file = "out.c";  // default

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            _args_help();
            exit(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--dump-ast") == 0) {
            args.dump_ast = true;
        } else if (strcmp(argv[i], "--no-w130") == 0) {
            // accepted and ignored: W130 and the walker behind it are deleted
        } else if (strcmp(argv[i], "--no-line-directives") == 0) {
            args.no_line_directives = true;
        } else if (strcmp(argv[i], "--dump-niche") == 0) {
            args.dump_niche = true;
        } else if (strcmp(argv[i], "--dump-effects") == 0) {
            args.dump_effects = true;
        } else if (strcmp(argv[i], "--dump-octagon") == 0) {
            args.dump_octagon = true;
        } else if (strcmp(argv[i], "--dump-measures") == 0) {
            args.dump_measures = true;
        } else if (strcmp(argv[i], "--emit-certificate") == 0 && i + 1 < argc) {
            args.emit_certificate = argv[++i];
        } else if (strcmp(argv[i], "--certificate-roundtrip") == 0 && i + 1 < argc) {
            args.certificate_roundtrip = argv[++i];
        } else if (strcmp(argv[i], "--check-certificate") == 0 && i + 1 < argc) {
            args.check_certificate = argv[++i];
        } else if (strcmp(argv[i], "--emit-llvm") == 0) {
            args.emit_llvm = true;
        } else if (strcmp(argv[i], "--interpret") == 0) {
            args.interpret = true;
        } else if (strcmp(argv[i], "--check-invariants") == 0) {
            args.interpret = true; args.check_invariants = true;
        } else if (strncmp(argv[i], "--backend=", 10) == 0) {   // IGNORED-FLAG (readme_gate reads this)
            // Accepted and ignored: there is one backend. Kept as a no-op rather than an
            // error so a script pinned to `--backend=ir` still runs.
            (void)0;
        } else if (strncmp(argv[i], "--engine=", 9) == 0) {     // IGNORED-FLAG (readme_gate reads this)
            // Accepted and ignored: there is one engine. `--engine=legacy` selected the AST
            // analyses, which were deleted on 2026-09-23 — honouring it would mean compiling
            // with no ownership, bounds or overflow checking at all, which is a fail-open
            // wearing a flag's name. A no-op rather than an error so old scripts still run.
            (void)0;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            args.output_file = argv[++i];
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            args.target_triple = argv[i] + 9;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            // ★ AN UNKNOWN FLAG IS AN ERROR, not a filename. This branch used to be the
            // catch-all `args.filename = argv[i]`, so `lain --dump-effcts prog.ln` set the
            // filename to the typo, then to `prog.ln`, and compiled in SILENCE with the dump
            // never happening — the user concludes the feature is missing. The same typo AFTER
            // the filename produced "Cannot open module file '--dump-effcts.ln'", which is
            // confusing rather than wrong. readme_gate tests the opposite direction (a flag the
            // docs name that the binary rejects) and cannot see this one.
            fprintf(stderr, "lain: unknown option '%s'. The options are:\n", argv[i]);
            _args_print_flags(stderr);
            exit(1);
        } else {
            args.filename = argv[i];
        }
    }
    
    if (!args.filename && !args.certificate_roundtrip) {
        printf("Error: No input file specified.\n");
        exit(1);
    }
    
    return args;
}

#endif /* ARGS_H */