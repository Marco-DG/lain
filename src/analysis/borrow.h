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
#include "../ir/place.h"
#include <stdlib.h>
#include <string.h>

// code: 10 = dangling return (E010) ; 4 = conflicting co-argument borrows (E004-class)
typedef struct { isize line, col; int code; } BorrowFinding;
typedef struct { IrFunc *f; IrInstr **def; int nvar; BorrowFinding *finds; int nfinds, cap; } Borrow;

static void bor_add(Borrow *B, isize line, isize col, int code) {
    if (B->nfinds==B->cap){ B->cap=B->cap?B->cap*2:4; B->finds=realloc(B->finds,B->cap*sizeof*B->finds); }
    B->finds[B->nfinds++] = (BorrowFinding){line,col,code};
}
static IrFunc *bor_find_func(IrFunc *mod, const IrName *n) {
    if (!n) return NULL;
    for (IrFunc *f=mod; f; f=f->next)
        if (f->name && f->name->length==n->length && memcmp(f->name->name,n->name,(size_t)n->length)==0) return f;
    return NULL;
}

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

// ── loans at a call site (borrow phase A) ────────────────────────────────────
// Every call argument that names storage creates a LOAN on that place for the duration of
// the call. Two loans on OVERLAPPING places conflict when at least one is mutable — that is
// the conflict rule of borrow_checker_design.md §1.4, specialised to one program point
// (co-arguments), which needs no region inference.
//
// The borrow KIND comes from the callee's parameter type: a `var` param lowers to a pointer
// with ptr_mut. The borrowed PLACE comes from the argument: either an address (a `var x`
// argument) or — for a by-value shared borrow — the place the value was LOADED from, which
// is what keeps `f(data, var data)` visible even though the first argument is a copy.
static bool bor_arg_loan(Borrow *B, IrFunc *callee, int k, IrValue *arg, IrPlace *out, bool *is_mut) {
    if (!arg) return false;
    *is_mut = false;
    IrParam *p = callee ? callee->params : NULL;
    for (int i=0; i<k && p; i++) p = p->next;
    IrType *pt = (p && p->value) ? p->value->type : NULL;
    if (!pt) return false;
    // Only a BORROW creates a loan that is live across the call. A by-value COPY (a scalar
    // parameter) is read during argument evaluation and cannot conflict with anything — this
    // is what makes `ms(var a, a)` and `v.push_n(v.cap)` legal (the two-phase pattern: the
    // mutable borrow is merely RESERVED while the copy is read). An aggregate parameter
    // (ptr / struct / slice) IS a live reference for the call's duration.
    bool is_borrow = (pt->kind==IRT_PTR || pt->kind==IRT_STRUCT || pt->kind==IRT_SLICE);
    if (!is_borrow) return false;
    if (pt->kind==IRT_PTR && pt->ptr_mut) *is_mut = true;
    IrInstr *d = (arg->id>=0 && arg->id<B->nvar) ? B->def[arg->id] : NULL;
    IrPlace pl = (d && d->op==IR_LOAD && d->n_operands>=1)
               ? ir_place_of(B->def, B->nvar, d->operands[0])   // by-value read of a place
               : ir_place_of(B->def, B->nvar, arg);             // an address argument
    if (!pl.valid || pl.base_kind==IRPB_DEREF) return false;    // unresolved ⇒ no loan tracked
    *out = pl; return true;
}

static void bor_check_call(Borrow *B, IrFunc *mod, IrInstr *call) {
    IrFunc *callee = bor_find_func(mod, call->aux.callee);
    if (!callee || call->n_operands < 2) return;        // need ≥2 args to conflict
    IrPlace pl[16]; bool mut[16]; int n = 0;
    for (int k=0; k<call->n_operands && n<16; k++) {
        IrPlace p; bool m;
        if (bor_arg_loan(B, callee, k, call->operands[k], &p, &m)) { pl[n]=p; mut[n]=m; n++; }
    }
    for (int i=0;i<n;i++)
        for (int j=i+1;j<n;j++)
            if ((mut[i] || mut[j]) && ir_place_overlaps(&pl[i], &pl[j])) {
                bor_add(B, call->line, call->col, 4);   // conflicting co-argument borrows
                return;                                  // one finding per call is enough
            }
}

static Borrow *borrow_analyze_mod(IrFunc *f, IrFunc *mod);
static Borrow *borrow_analyze(IrFunc *f) { return borrow_analyze_mod(f, NULL); }

static Borrow *borrow_analyze_mod(IrFunc *f, IrFunc *mod) {
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
        if (dangles) bor_add(B, rv->line, rv->col, 10);
    }
    // phase A: conflicting co-argument borrows at each call
    if (mod)
        for (IrBlock *b=f->blocks;b;b=b->next)
            for (IrInstr *i=b->instrs;i;i=i->next)
                if (i->op==IR_CALL) bor_check_call(B, mod, i);
    return B;
}
static void borrow_free(Borrow *B){ if(!B)return; free(B->def); free(B->finds); free(B); }

#endif // LAIN_BORROW_H
