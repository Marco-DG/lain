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

// The front end's source-line printer, set by the driver for a function in the file whose text it
// holds: it prints the `-->` line, the source line and a caret. Front-end errors always showed the
// line and the analyses' never did, so E130 had an excerpt and E085 beside it did not. NULL (a
// function from an imported module, or a tool without a front end) keeps the bare `-->` line.
static void (*ir_diag_excerpt)(isize line, isize col) = NULL;

// The function being reported, for a finding that has no position (I.68): `[E086] Error:` with no
// place at all was printed for `func f(a i32) i8 { return a }`, and the gate that requires every
// coded error to be placed grepped for a LITERAL code, while this helper passes the code as a
// format argument, so no rule saw it. With no line it now names the function, as ir_diag_locus
// does for the emitter's errors.
static const IrFunc *ir_diag_fn = NULL;
static void ir_diag(const char *file, isize line, isize col, const char *code, const char *msg) {
    fprintf(stderr, "[%s] Error", code);
    ir_diag_locus(line, col, ir_diag_fn);
    fprintf(stderr, ": %s\n", msg);
    if (line && ir_diag_excerpt) ir_diag_excerpt(line, col);
    else if (file && line) fprintf(stderr, "  --> %s:%lld:%lld\n", file, (long long)line, (long long)col);
}

// Run every sovereign analysis over one function and print what it finds.
// Returns the number of ERRORS (warnings and notes do not count).
// `mod` is passed EXPLICITLY rather than read from bor_loan_mod: borrow_analyze_mod sets that
// global on entry and NULLS IT ON EXIT, so reading it here gave the first function a module
// and every one after it nothing — the co-argument aliasing check silently stopped running
// after the first callee, which is exactly where `mix(var x, var x)` lives.
static int ir_report_findings(IrFunc *f, IrFunc *mod, const char *file, bool numeric) {
    ir_diag_fn = f;
    int n = 0;

    // The borrow pass runs FIRST so the linearity pass can defer to it. `f(var d, mov d)`
    // produces two findings at one position: linearity sees `var d` as a use of a value the
    // same call moved (E001), and the borrow pass sees the real constraint — you may not
    // borrow and move the same place in one call (E008). Both are true; only the second says
    // what the programmer did wrong, and the first is an artefact of `mov` being lowered as a
    // use. Emission order is unchanged; only the analysis order and this suppression are new.
    // One range analysis serves the borrow pass's disjointness queries and the report below.
    if (numeric) vra_cert_next = f;          // this is the analysis a certificate states (C.2)
    Vra *V = numeric ? vra_analyze(f) : NULL;
    vra_shared_f = V ? f : NULL; vra_shared_V = V;
    Borrow *B = borrow_analyze_mod(f, mod);
    vra_shared_f = NULL; vra_shared_V = NULL;

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
                         : fi->code==16 ? "E016" : fi->code==20 ? "E020"
                         : (fi->code==21 || fi->code==22) ? "E021" : "E003";
        const char *msg  = fi->code==1 ? "use of a value that was already moved"
                         : fi->code==2 ? "this value is moved twice"
                         : fi->code==20 ? "cannot move out of a borrow — this place is lent to the function, not owned by it; its owner still holds it"
                         : fi->code==21 ? "this assignment overwrites a linear value that still holds a resource, which is then lost — consume it first (`mov`)"
                         : fi->code==22 ? "this assignment overwrites a linear value through a borrow — the resource there is the owner's, it cannot be consumed here, and it would be lost"
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
                          : fi->code==87 ? "E087" : fi->code==8 ? "E008" : fi->code==9 ? "E009" : "E004";
        const char *bmsg  = fi->code==10 ? "this reference would outlive the value it borrows"
                          : fi->code==9  ? "this writes storage declared immutable (a binding without "
                                           "`var`, or a module constant), reached through a reference"
                          : fi->code==11 ? "the `in` clause claims this result borrows less "
                                           "than the body actually does"
                          : fi->code==87 ? "this array reaches two parameters of the same call"
                          : fi->code==8  ? "cannot move this value because it is also borrowed here"
                          :                "conflicting borrows of the same value";
        ir_diag(file, fi->line, fi->col, bcode, bmsg);
        n++;
    }
    borrow_free(B);

    if (!numeric) return n;   // NOT judged: the numeric obligations were never reported
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
                } else if (c->shift == 1) {
                    ir_diag(file, c->line, c->col, "E086",
                            "shift amount is not provably within the operand's width (valid 0..width-1)");
                } else if (c->shift == 3) {
                    ir_diag(file, c->line, c->col, "E086",
                            "signed division may overflow — TYPE_MIN / -1 is undefined at this width; "
                            "prove the dividend is not TYPE_MIN or the divisor is not -1, or say what "
                            "you mean: `/%` wraps (MIN), `/|` saturates (MAX)");
                } else if (c->shift == 5) {
                    ir_diag(file, c->line, c->col, "E086",
                            "the quotient TYPE_MIN / -1 does not fit here (a divisor that is not 0 "
                            "may still be -1); prove the dividend is not TYPE_MIN or the divisor is "
                            "not -1 (e.g. `b > 0`), or say what you mean: `/%` wraps (MIN), `/|` "
                            "saturates (MAX)");
                } else if (c->shift == 4) {
                    ir_diag(file, c->line, c->col, "E086",
                            "unsigned left shift may lose bits — the result may not fit the left "
                            "operand's type; prove it fits, or say you mean to discard them: `<<%` wraps");
                } else if (c->shift == 2) {
                    ir_diag(file, c->line, c->col, "E086",
                            "signed left shift may overflow — a bit can reach the sign (`1 << 31` on an i32 is UB)");
                } else if (c->refine) {
                    char msg[200];
                    if (c->refine == 2)
                        snprintf(msg, sizeof msg, "this value is not provably different from %lld, "
                                 "which the refined type it lands in excludes", (long long)c->ref_ne);
                    else
                        snprintf(msg, sizeof msg, "this value is not provably within [%lld, %lld]%s, "
                                 "the refinement of the type it lands in",
                                 (long long)c->ref_lo, (long long)c->ref_hi,
                                 c->refine == 3 ? " (and not the excluded value)" : "");
                    ir_diag(file, c->line, c->col, "E086", msg);
                } else {
                    ir_diag(file, c->line, c->col, "E086",
                            "arithmetic is not provably free of overflow");
                }
                n++; break;
            case VRA_DIVZERO:
                if (c->bitcount)
                    ir_diag(file, c->line, c->col, "E015",
                            "the argument of @ctz/@clz is not provably non-zero (counting the zero bits of 0 is undefined)");
                else
                    ir_diag(file, c->line, c->col, "E015", "divisor is not provably non-zero");
                n++; break;
            case VRA_PRECOND:
                if (c->diag == 85)
                    ir_diag(file, c->line, c->col, "E085",
                            "index argument is not provably within the bounds of the array it is `in`");
                else if (c->diag == 86)
                    ir_diag(file, c->line, c->col, "E086",
                            "return value cannot be proven to satisfy the function's return refinement");
                else if (c->diag == 121)
                    ir_diag(file, c->line, c->col, "E121",
                            "a struct field invariant is not proven here — an `in` index must stay below its container's length, and a relation between fields (`pos <= src.len`, `len <= cap`) must hold; both at construction and after every write to either field. A cursor that may rest at the END of its container, or start on an empty one, is `pos usize <= src.len`, not `in`");
                else if (c->diag == 87)
                    ir_diag(file, c->line, c->col, "E087",
                            "argument does not satisfy the parameter's sized-slice constraint");
                else if (c->diag == 137)
                    ir_diag(file, c->line, c->col, "E137",
                            "this call cannot be checked against the callee's parameter refinement: its right-hand side is something the call site cannot evaluate (a call, a cast), so the callee would assume a fact nobody established. Write it with literals, constants and parameters");
                else if (c->diag == 135)
                    ir_diag(file, c->line, c->col, "E135",
                            "a [noreturn] function can return here — every path must end in a panic, a call that does not return, or a loop that does not end (with `effects diverge`)");
                else
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
                    if (c->measure_mismatch) {
                        // I.74: the loop DOES end, by a measure the engine found; the one written
                        // is the claim that is wrong, and was accepted unread until now.
                        ir_diag(file, c->line, c->col, "E082",
                                "this loop ends, but not by the `decreasing` measure written: it "
                                "does not fall on every iteration");
                        fprintf(stderr, "       write the quantity that falls (for a counter rising "
                                        "toward a bound, `bound - counter`), or drop the clause\n");
                    } else
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
    f->judged = true;
    return n;
}

// ★ EMISSION REQUIRES A VERDICT, BY CONSTRUCTION. Three fail-open paths in two days had one shape:
// something skipped the analyses (a contract the resolver could not read, a borrow root it could
// not name, an `incomplete` function whose "checks are skipped") and the program was emitted
// anyway. Each was fixed where it was found; this refuses the class. Every function that is
// emitted or interpreted must have been judged by ir_report_findings.
static int ir_require_verdicts(IrFunc *mod) {
    int n = 0;
    for (IrFunc *f = mod; f; f = f->next) {
        if (f->is_extern || f->judged) continue;
        fprintf(stderr, "internal error: '%.*s' would be emitted without being analysed; refusing "
                "the program (a path skipped the analyses)\n", (int)f->name->length, f->name->name);
        n++;
    }
    return n;
}

#endif // LAIN_IR_REPORT_H
