#ifndef LAIN_IR_REPORT_H
#define LAIN_IR_REPORT_H
// report.h — render the sovereign IR analyses' findings as user-facing diagnostics.
//
// Stage 3.5's split needs one thing the differential harnesses never did: MESSAGES. A gate
// compares verdicts, so it never noticed that every finding pointed at line 0 and named a
// slot number rather than anything a person could act on. An engine that cannot say WHERE
// and WHAT is not one a user can run, whatever it can prove.
//
// The codes are the same E-numbers the old engine uses, so a program's diagnosis does not
// change identity when the authority behind it does.
#include "../ir/ir.h"
#include "linearity.h"
#include "borrow.h"
#include "definite_init.h"
#include "vra.h"
#include <stdio.h>

static void ir_diag(const char *file, isize line, isize col, const char *code, const char *msg) {
    fprintf(stderr, "[%s] Error", code);
    if (line) fprintf(stderr, " Ln %lld, Col %lld", (long long)line, (long long)col);
    fprintf(stderr, ": %s\n", msg);
    if (file && line) fprintf(stderr, "  --> %s:%lld:%lld\n", file, (long long)line, (long long)col);
}

// Run every sovereign analysis over one function and print what it finds.
// Returns the number of ERRORS (warnings and notes do not count).
// `mod` is passed EXPLICITLY rather than read from bor_loan_mod: borrow_analyze_mod sets that
// global on entry and NULLS IT ON EXIT, so reading it here gave the first function a module
// and every one after it nothing — the co-argument aliasing check silently stopped running
// after the first callee, which is exactly where `mix(var x, var x)` lives.
static int ir_report_findings(IrFunc *f, IrFunc *mod, const char *file, bool numeric) {
    int n = 0;

    // The borrow pass runs FIRST so the linearity pass can defer to it. `f(var d, mov d)`
    // produces two findings at one position: linearity sees `var d` as a use of a value the
    // same call moved (E001), and the borrow pass sees the real constraint — you may not
    // borrow and move the same place in one call (E008). Both are true; only the second says
    // what the programmer did wrong, and the first is an artefact of `mov` being lowered as a
    // use. Emission order is unchanged; only the analysis order and this suppression are new.
    Borrow *B = borrow_analyze_mod(f, mod);

    Lin *L = lin_analyze(f);
    for (int i = 0; i < L->nfinds; i++) {
        LinFinding *fi = &L->finds[i];
        if (fi->code == 1) {
            bool superseded = false;
            for (int q = 0; q < B->nfinds && !superseded; q++)
                superseded = (B->finds[q].code == 8 &&
                              B->finds[q].line == fi->line && B->finds[q].col == fi->col);
            if (superseded) continue;
        }
        const char *code = fi->code==1 ? "E001" : fi->code==2 ? "E002"
                         : fi->code==16 ? "E016" : "E003";
        const char *msg  = fi->code==1 ? "use of a value that was already moved"
                         : fi->code==2 ? "this value is moved twice"
                         : fi->code==16 ? "consumed on some paths but not others"
                         : "a linear value is not consumed before it goes out of scope";
        ir_diag(file, fi->line, fi->col, code, msg);
        n++;
    }
    lin_free(L);

    di_mod = mod;                 // the shared-borrow read rule needs callee signatures
    Di *D = di_analyze(f);
    for (int i = 0; i < D->nfinds; i++) {
        DiFinding *fi = &D->finds[i];
        ir_diag(file, fi->line, fi->col, fi->code==19 ? "E019" : "E005",
                fi->code==19 ? "this value is only partially initialised"
                             : "read of an uninitialised value");
        n++;
    }
    di_free(D);

    for (int i = 0; i < B->nfinds; i++) {
        BorrowFinding *fi = &B->finds[i];
        // borrow.h emits 4 (conflicting co-argument borrows), 10 (dangling return) and
        // 11 (`in` clause understates the body). 10 had NO branch here and fell through to
        // E004, so every dangling return was reported as a borrow conflict — the analysis
        // knew, and the last step threw it away.
        const char *bcode = fi->code==10 ? "E010" : fi->code==11 ? "E124"
                          : fi->code==87 ? "E087" : fi->code==8 ? "E008" : "E004";
        const char *bmsg  = fi->code==10 ? "this reference would outlive the value it borrows"
                          : fi->code==11 ? "the `in` clause claims this result borrows less "
                                           "than the body actually does"
                          : fi->code==87 ? "this array reaches two parameters of the same call"
                          : fi->code==8  ? "cannot move this value because it is also borrowed here"
                          :                "conflicting borrows of the same value";
        ir_diag(file, fi->line, fi->col, bcode, bmsg);
        n++;
    }
    borrow_free(B);

    if (!numeric) return n;   // the old engine still owns bounds/overflow/division
    Vra *V = vra_analyze(f);
    for (int i = 0; i < V->nchecks; i++) {
        VraCheck *c = &V->checks[i];
        if (c->ok) continue;                       // proved check-free: nothing to say
        switch (c->kind) {
            case VRA_BOUNDS:
                ir_diag(file, c->line, c->col, "E085",
                        c->has_len ? "index is not provably within bounds"
                                   : "index is not provably within bounds (no length is known here)");
                n++; break;
            case VRA_OVERFLOW:
                if (c->accum) {
                    // B1 computed all of this in order to fail. Saying only "not provably free
                    // of overflow" leaves the user to guess which of three things to change,
                    // on a loop they wrote in the obvious way.
                    ir_diag(file, c->line, c->col, "E086",
                            "this running total can overflow the type it is stored in");
                    fprintf(stderr, "       it starts in [%lld, %lld] and each iteration adds "
                                    "between %lld and %lld\n",
                            (long long)c->accum_s0lo, (long long)c->accum_s0hi,
                            (long long)c->accum_dlo, (long long)c->accum_dhi);
                    if (c->accum_T < 0)
                        fprintf(stderr, "       and the loop has no bounded trip count, so the "
                                        "total has no bound at all\n");
                    else
                        fprintf(stderr, "       over up to %lld iterations\n",
                                (long long)c->accum_T);
                    fprintf(stderr,
                        "       bound any ONE of the three and this proves:\n"
                        "         the count    a length with a refinement, e.g. "
                        "`f(a i32[n], n usize < 4096)`\n"
                        "         the element  a narrower element type, or a refinement on it\n"
                        "         the total    a wider accumulator, widening the addend too\n"
                        "       or say which arithmetic you meant: `+%%` wraps, `+|` saturates\n");
                } else {
                    ir_diag(file, c->line, c->col, "E086",
                            "arithmetic is not provably free of overflow");
                }
                n++; break;
            case VRA_DIVZERO:
                ir_diag(file, c->line, c->col, "E015", "divisor is not provably non-zero");
                n++; break;
            case VRA_PRECOND:
                ir_diag(file, c->line, c->col, "E012", "a required precondition is not established here");
                n++; break;
            case VRA_TERMINATION:
                // One obligation, two arguments, two codes — the identity a user already knows.
                // E082 is a loop whose measure does not decrease; E011 is a `func` whose
                // recursion has no well-founded ranking. Both come from the sovereign engine
                // now; both used to come from src/sema/.
                if (c->recursion) {
                    // Annex B, normatively: E011 is a self-recursion for which NO measure can
                    // be inferred; a measure that is present and fails is E082. The engine
                    // carries the source fact (`IrFunc.has_decreasing`) so it can tell them
                    // apart rather than picking one and being wrong about half the programs.
                    ir_diag(file, c->line, c->col, c->had_measure ? "E082" : "E011",
                            c->mutual
                              ? "this call closes a mutual-recursion cycle, and no ranking over the "
                                "cycle can be inferred"
                              : c->had_measure
                              ? "the `decreasing` measure is not provably well-founded here"
                              : "this recursion is not provably terminating");
                    if (c->mutual)
                        fprintf(stderr,
                            "       the engine ranks a 2-cycle when a parameter does not grow on "
                            "either edge and falls on one\n"
                            "       give the cycle a measure that shrinks per lap and is bounded "
                            "below, or write `effects diverge`\n");
                    else
                    fprintf(stderr, "       a function is total by default, so some parameter has "
                                    "to shrink toward a base case on every self-call\n"
                                    "       the engine looks for one that strictly decreases "
                                    "AND is bounded below (`>= 0`, or an unsigned type)\n"
                                    "       name it with `decreasing <param>`, guard the base "
                                    "case, or write `effects diverge` to say it may not "
                                    "terminate\n");
                } else {
                    // Annex B again, and the same rule as the recursive case above: E011 is a
                    // loop "whose measure is neither given nor inferable", E082 one whose
                    // measure is PRESENT and fails. `IrBlock.has_measure` carries whether the
                    // programmer wrote `decreasing` — recorded at PARSE time, because sema's
                    // inference installs its candidate in the same AST field and by lowering
                    // the two are indistinguishable.
                    ir_diag(file, c->line, c->col, c->had_measure ? "E082" : "E011",
                            c->had_measure
                              ? "the `decreasing` measure is not provably well-founded here"
                              : "this loop is not provably terminating, and no measure could "
                                "be inferred");
                }
                n++; break;
        }
    }
    vra_free(V);
    return n;
}

#endif // LAIN_IR_REPORT_H
