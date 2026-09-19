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
    // ── THE DEFAULT BACKEND, 2026-09-19 ──────────────────────────────────────────────────
    // The C is now emitted FROM THE IR. `--backend=legacy` restores the AST emitter.
    //
    // What had to be true first, each measured rather than argued:
    //   emit_gate      399 agree / 0 differ / 0 build-fail
    //   annot_gate     0 rows short — the new backend tells the C compiler at least as much
    //                  on every row, and more on four of five (restrict 200 -> 488,
    //                  access 38 -> 164); the one divergence (`const T*`) is stated
    //   backend_corpus 421 programs through the REAL compiler: 0 differ, 0 cannot build
    //   D-51/54/55     0 programs it cannot emit · module-qualified function identity ·
    //                  no corpus test depends on the old emitter's spellings
    //
    // ★ AND THE POINT OF FLIPPING IT, beyond tidiness: until now every corpus test compiled
    // through the OLD emitter, so `make gates` could not see the new one — 251 programs once
    // failed to build under it while all eight gates stayed green. THE FLIP IS WHAT MAKES THE
    // CORPUS THE NEW BACKEND'S TEST. That, not deleting a directory, is the switchover.
    //
    // ⚠ AND FLIPPING IT IS WHAT FOUND OUT WHAT THE DIFFERENTIALS COULD NOT. Running the
    // corpus through the new backend surfaced twelve failures, and then the trust harness —
    // which EXECUTES what was proven — surfaced a thirteenth the corpus could not see:
    //
    //   nine  codegen defects: a null pointer stored from an integer, a function-pointer type
    //         with no signature, a borrow binding addressed instead of loaded
    //   two   E106 "use of undeclared identifier", REPORTED BY src/emit/expr.h — so with this
    //         backend the program was accepted and emitted `void v0;`. Name resolution is the
    //         front end's job; it is now src/sema/undeclared.h (D-57). (The D-57 entry said
    //         all THREE remaining failures were E106. Two were; the third was the one below,
    //         and it was the more serious of the two kinds.)
    //   one   IR_OPAQUE emitted as `v0 = 0`. Right for the ANALYSES (a declared unknown they
    //         havoc around); for codegen it passed a NULL to a function that dereferences it.
    //         The backend now REFUSES what it cannot model rather than emitting a placeholder,
    //         because a zero of the right type compiles and a segfault is not a diagnostic
    //   plus  `uint16_t * uint16_t` into a `uint32_t`: UB in C, because both operands promote
    //         to `int`. The IR said u32; the C computed in int. A widening operation is now
    //         SPELLED as one. No gate saw it — run_trust did, by running the program
    //
    // Verified after all of it: corpus 724/724 with every gate at exit 0, trust 43/0, and
    // backend_corpus 422 agree / 0 behaviour differs / 0 cannot build.
    //
    // ══ AND THEN REVERTED, THE SAME DAY, FOR A REASON NONE OF THOSE TEN GATES COULD SEE ══
    //
    // ★ D-62 — THE NEW BACKEND DOES NOT NICHE-PACK. For every `T | markers` in the corpus it
    // emits a tag+union struct where the old backend emits the payload itself:
    //
    //     old:  typedef const uint8_t * __U_ptr_u8_none;                    // 8 bytes
    //     new:  struct __U_ptr_u8_none { int32_t tag; union {...} data; };  // 16 bytes
    //
    // Both are CORRECT and the programs print the same thing, which is precisely why
    // `backend_corpus` — behaviour, 422/0/0 — is blind to it, and why `emit_gate` and the
    // corpus and trust and the nineteen fuzzers are too. The measurement is `layout_gate.sh`,
    // written after the fact: **29 sums lost their packing, 85 agree**.
    //
    // The niche optimization is not a nicety here. It is P2 — "zero-cost" — it is what the
    // README showcases, and `T | markers` is the ONE error-handling construct in the language,
    // so this silently doubles the width of every optional and every result in every program
    // that uses them. A default that abandons the project's headline claim without saying so
    // is worse than a flag.
    //
    // plan item 2.2 predicted this exactly: "if two backends chose differently, the same
    // program would have different runtime semantics under each, and nothing would catch it —
    // there is no cross-backend layout test because there has never been a second backend."
    // It was written as an argument for the layout pass. It was also a defect report.
    //
    // What stands: everything the flip FOUND (D-57 E106 in the front end, D-58 the backend
    // refusing what it cannot model, D-59 the widening-arithmetic UB, D-60/61 the unplugged
    // harnesses) — those were real and are fixed. And `backend_corpus` is now a GATE, so the
    // corpus still tests the new backend on every run even though it is not the default. The
    // flip's value did not depend on the flip staying.
    //
    // THE CONDITION FOR FLIPPING IT AGAIN: `layout_gate.sh` at 0 niche lost — i.e. plan item
    // 2.2, layout decided once below the IR.
    args.backend_ir = false;
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