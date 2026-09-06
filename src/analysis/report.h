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

    Lin *L = lin_analyze(f);
    for (int i = 0; i < L->nfinds; i++) {
        LinFinding *fi = &L->finds[i];
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

    Di *D = di_analyze(f);
    for (int i = 0; i < D->nfinds; i++) {
        DiFinding *fi = &D->finds[i];
        ir_diag(file, fi->line, fi->col, fi->code==19 ? "E019" : "E005",
                fi->code==19 ? "this value is only partially initialised"
                             : "read of an uninitialised value");
        n++;
    }
    di_free(D);

    Borrow *B = borrow_analyze_mod(f, mod);
    for (int i = 0; i < B->nfinds; i++) {
        BorrowFinding *fi = &B->finds[i];
        ir_diag(file, fi->line, fi->col, fi->code==2 ? "E002" : "E004",
                fi->code==2 ? "this reference outlives the value it borrows"
                            : "conflicting borrows of the same value");
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
                ir_diag(file, c->line, c->col, "E086", "arithmetic is not provably free of overflow");
                n++; break;
            case VRA_DIVZERO:
                ir_diag(file, c->line, c->col, "E087", "divisor is not provably non-zero");
                n++; break;
            case VRA_PRECOND:
                ir_diag(file, c->line, c->col, "E088", "a required precondition is not established here");
                n++; break;
            case VRA_TERMINATION:
                ir_diag(file, c->line, c->col, "E082", "this loop is not provably terminating");
                n++; break;
        }
    }
    vra_free(V);
    return n;
}

#endif // LAIN_IR_REPORT_H
