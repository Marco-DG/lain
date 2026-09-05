// src/analysis/borrow.h — Phase 3.1: borrow / region checking as a sovereign IR pass.
//
// The high-value core is the DANGLING REFERENCE: a function must not return a pointer or
// slice whose provenance roots in one of ITS OWN local allocations — that storage dies at
// the return, so the caller would hold a dangling reference (a use-after-return / stack
// use-after-free). This is exactly what the old engine's E-borrow / region check catches.
//
// Method: escape analysis. Trace the returned value's provenance backward through the
// address-forming ops (elem_ptr / field_ptr / slice_data / make_slice); if the root is a
// local IR_ALLOCA the reference escapes → flag. A param is a POINTER value (a borrow of the
// caller's storage), not an alloca, so returning it is fine; heap/global roots likewise.
//
// Sovereign: reads only ir.h. (NLL region lifetimes and two-phase borrows are follow-ups;
// this cut is the escaping-local-reference core — the memory-safety heart.)
#ifndef LAIN_BORROW_H
#define LAIN_BORROW_H

#include "../ir/ir.h"
#include <stdlib.h>

typedef struct { isize line, col; } BorrowFinding;
typedef struct { IrFunc *f; IrInstr **def; int nvar; BorrowFinding *finds; int nfinds, cap; } Borrow;

// Does v's provenance root in a LOCAL alloca? (⇒ returning it dangles.) Follows only the
// address-forming ops; a load/call/param/global root stops the walk (conservatively safe).
static bool bor_roots_local(IrInstr **def, int nvar, IrValue *v) {
    for (int guard=0; v && v->id>=0 && v->id<nvar && guard<100000; guard++) {
        IrInstr *d = def[v->id];
        if (!d) return false;                             // param / φ with no single def
        switch (d->op) {
            case IR_ALLOCA:                                return true;    // a local stack slot
            case IR_ELEM_PTR: case IR_FIELD_PTR:
            case IR_SLICE_DATA: case IR_MAKE_SLICE:
                v = d->n_operands>=1 ? d->operands[0] : NULL; break;       // provenance = base/data
            default: return false;                        // load/call/… — not a known local
        }
    }
    return false;
}

static Borrow *borrow_analyze(IrFunc *f) {
    Borrow *B = calloc(1,sizeof *B); B->f=f;
    B->nvar = f->next_value_id>0?f->next_value_id:1;
    B->def = calloc(B->nvar,sizeof(IrInstr*));
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next) if (i->result) B->def[i->result->id]=i;
    for (IrBlock *b=f->blocks;b;b=b->next) {
        if (b->term.kind!=IR_TERM_RET || !b->term.cond) continue;
        IrValue *rv = b->term.cond;
        bool dangles = false;
        if (rv->type && (rv->type->kind==IRT_PTR || rv->type->kind==IRT_SLICE))
            dangles = bor_roots_local(B->def, B->nvar, rv);         // return a local reference
        else if (rv->type && rv->type->kind==IRT_STRUCT) {         // return a struct that BORROWS a
            IrInstr *d = B->def[rv->id];                            // local through a pointer/slice field
            if (d && d->op==IR_STRUCT_NEW)
                for (int k=0;k<d->n_operands && !dangles;k++) {
                    IrValue *fv = d->operands[k];
                    if (fv && fv->type && (fv->type->kind==IRT_PTR || fv->type->kind==IRT_SLICE))
                        dangles = bor_roots_local(B->def, B->nvar, fv);
                }
        }
        if (dangles) {
            if (B->nfinds==B->cap){ B->cap=B->cap?B->cap*2:4; B->finds=realloc(B->finds,B->cap*sizeof*B->finds); }
            B->finds[B->nfinds++] = (BorrowFinding){rv->line, rv->col};
        }
    }
    return B;
}
static void borrow_free(Borrow *B){ if(!B)return; free(B->def); free(B->finds); free(B); }

#endif // LAIN_BORROW_H
