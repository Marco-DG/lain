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

// ── phase A2: loans that outlive their statement (liveness regions) ──────────
// A call that takes a MUTABLE borrow and RETURNS a reference hands the caller a reference
// derived from that borrow (Rust would say the return borrows from the reference parameter
// — lifetime elision). The loan therefore stays live as long as the returned reference is
// live, not just for the call. Any conflicting borrow of an overlapping place inside that
// window is an error — this is the `r = get_ref(var d); read_data(d); use(r)` family.
//
// Region = the span from the creating call to the carrier's LAST USE. The carrier is the
// result value, plus the slot it is stored into (`var r = get_ref(var d)`), so loads of
// that slot extend the region. Straight-line spans are exact; branches are approximated by
// instruction order, which can only ever be *narrower or equal* here because a use on any
// path still extends the window — and the gate verifies no over-rejection.
#define BOR_MAX_INSTR 4096
typedef struct { IrInstr *ins[BOR_MAX_INSTR]; int n; } BorSeq;

static void bor_linearize(IrFunc *f, BorSeq *s) {
    s->n = 0;
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next)
            if (s->n < BOR_MAX_INSTR) s->ins[s->n++] = i;
}
// last index at which `val` (or a load of `slot`) is used; -1 if never
static int bor_last_use(BorSeq *s, int from, IrValue *val, IrValue *slot) {
    int last = -1;
    for (int k=from+1;k<s->n;k++) {
        IrInstr *i = s->ins[k];
        bool used = false;
        for (int o=0; o<i->n_operands && !used; o++) {
            IrValue *op = i->operands[o]; if (!op) continue;
            // ANY mention keeps the reference alive: a LOAD of the slot, but equally an
            // ADDRESS use — `consume_ref(var ref)` passes the slot itself, which is exactly
            // how these tests keep the borrow live, and only counting loads missed it.
            if ((val && op==val) || (slot && op==slot)) used = true;
        }
        if (used) last = k;
    }
    return last;
}
// the slot a call result is immediately stored into, if any (`var r = call(...)`)
static IrValue *bor_result_slot(BorSeq *s, int at, IrValue *res) {
    if (!res) return NULL;
    for (int k=at+1;k<s->n && k<=at+4;k++) {
        IrInstr *i = s->ins[k];
        if (i->op==IR_STORE && i->n_operands>=2 && i->operands[1]==res) return i->operands[0];
    }
    return NULL;
}

// ── which PARAMETER does a returned reference borrow from? (design §5) ───────────────────
// INFERRED from the body, never guessed from the signature. `pick(var a, var b) var i32` may
// return either parameter; the old syntactic rule ("the first mutable reference param") is
// UNSOUND for `return var b.x` — the loan is charged to `a` and every conflict on `b` goes
// unreported. Rooting each returned reference to its parameter is exact where it succeeds,
// and falls back to *every* reference parameter (sound, over-strict) where it does not.
//
// This is strictly stronger than Rust, which cannot infer a body fact across the signature
// boundary and so rejects the ambiguous signature outright ("missing lifetime specifier").

// provenance walk to the ROOT value: a parameter (no defining instruction) or a local slot.
// Same edge set as bor_roots_local — address-forming ops only, so provenance is not laundered
// through a load or a call.
static IrValue *bor_root_value(IrInstr **def, int nvar, IrValue *v) {
    for (int guard=0; v && v->id>=0 && v->id<nvar && guard<100000; guard++) {
        IrInstr *d = def[v->id];
        if (!d) return v;                                  // no def ⇒ a parameter: the root
        switch (d->op) {
            case IR_ALLOCA: return v;                      // a local stack slot: the root
            case IR_ELEM_PTR: case IR_FIELD_PTR:
            case IR_SLICE_DATA: case IR_MAKE_SLICE:
                v = d->n_operands>=1 ? d->operands[0] : NULL; break;
            default: return NULL;                          // opaque provenance
        }
    }
    return NULL;
}
static int bor_param_index(IrFunc *f, IrValue *v) {
    int idx = 0;
    for (IrParam *p = f->params; p; p = p->next, idx++)
        if (p->value == v) return idx;
    return -1;
}
static uint64_t bor_all_ref_params(IrFunc *f) {
    uint64_t m = 0; int idx = 0;
    for (IrParam *p = f->params; p; p = p->next, idx++) {
        IrType *t = p->value ? p->value->type : NULL;
        if (idx < 64 && t && (t->kind==IRT_PTR || t->kind==IRT_STRUCT || t->kind==IRT_SLICE))
            m |= (1ull<<idx);
    }
    return m;
}
static uint64_t bor_ret_borrow_mask(IrFunc *f) {
    if (!f->ret_borrows) return 0;
    if (f->ret_borrow_mask_done) return f->ret_borrow_mask;
    f->ret_borrow_mask_done = true;
    int nvar = f->next_value_id>0?f->next_value_id:1;
    IrInstr **def = calloc(nvar,sizeof(IrInstr*));
    if (!def) { f->ret_borrow_mask = bor_all_ref_params(f); return f->ret_borrow_mask; }
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next)
            if (i->result && i->result->id>=0 && i->result->id<nvar) def[i->result->id]=i;
    uint64_t mask = 0; bool opaque = false;
    for (IrBlock *b=f->blocks;b;b=b->next) {
        if (b->term.kind!=IR_TERM_RET || !b->term.cond) continue;
        IrValue *root = bor_root_value(def, nvar, b->term.cond);
        if (!root) { opaque = true; continue; }                    // cannot attribute
        int pi = bor_param_index(f, root);
        if (pi >= 0) { if (pi < 64) mask |= (1ull<<pi); else opaque = true; continue; }
        IrInstr *d = (root->id>=0 && root->id<nvar) ? def[root->id] : NULL;
        if (d && d->op==IR_ALLOCA) continue;   // roots in a LOCAL ⇒ the dangling case (code 10),
        opaque = true;                          // reported separately; it lends no parameter
    }
    free(def);
    if (opaque) mask |= bor_all_ref_params(f);   // sound fallback: assume it borrows them all
    f->ret_borrow_mask = mask;
    return mask;
}

static Borrow *borrow_analyze_mod(IrFunc *f, IrFunc *mod);
static Borrow *borrow_analyze(IrFunc *f) { return borrow_analyze_mod(f, NULL); }

static void bor_check_regions(Borrow *B, IrFunc *mod, IrFunc *f) {
    BorSeq s; bor_linearize(f, &s);
    for (int k=0;k<s.n;k++) {
        IrInstr *ins = s.ins[k];
        if (!ins->result) continue;
        IrPlace src[8]; int nsrc = 0;
        if (ins->op == IR_CALL) {
            IrFunc *callee = bor_find_func(mod, ins->aux.callee);
            if (!callee || !callee->ret_borrows) continue;  // the return must BORROW a parameter
            // Loans on every place passed to a parameter the RETURN may borrow from. The mask
            // is inferred from the callee's body, so `pick(var a, var b) var i32` charges the
            // loan to the parameter actually returned — and to both only if it returns either.
            uint64_t want = bor_ret_borrow_mask(callee);
            for (int a=0;a<ins->n_operands && nsrc<8;a++) {
                if (a < 64 && !((want>>a)&1u)) continue;
                IrPlace p; bool m;
                if (bor_arg_loan(B, callee, a, ins->operands[a], &p, &m)) src[nsrc++] = p;
            }
        } else if (ins->op == IR_FIELD_PTR || ins->op == IR_ELEM_PTR) {
            // A DIRECT borrow: `var ref = var d.value` takes an address and PARKS IT IN A
            // SLOT, so the loan outlives the statement exactly like a returned reference.
            // Requiring the address to be stored into a slot is what separates a borrow from
            // ordinary field/element access: `a[i] = v` and `x = d.value` feed the address
            // straight to a store/load and create no lasting loan (treating every elem_ptr as
            // a loan would make `a[i] = v` conflict with itself).
            if (!bor_result_slot(&s, k, ins->result)) continue;
            IrPlace p = ir_place_of(B->def, B->nvar, ins->result);
            if (!p.valid || p.base_kind != IRPB_LOCAL) continue;
            src[nsrc++] = p;
        } else continue;
        if (!nsrc) continue;
        IrValue *slot = bor_result_slot(&s, k, ins->result);
        int last = bor_last_use(&s, k, ins->result, slot);
        if (last < 0) continue;                       // reference never used ⇒ no live region
        // any conflicting ACCESS of an overlapping place inside (k, last]
        for (int j=k+1;j<=last;j++) {
            IrInstr *other = s.ins[j];
            // A DIRECT WRITE conflicts with a live loan just as a second borrow does — the
            // conflict rule (design §1.4) is over ACCESSES, not over calls. Loans were only
            // ever created and checked at call arguments, so `r = get_ref(var d); d.value = 99`
            // and whole-owner reassignment `d = D(99)` were both invisible.
            //
            // The store that CREATES the carrier (`store ref, <the borrow>`) targets the
            // carrier slot, not the borrowed place, so it cannot self-conflict; and a write
            // THROUGH the reference roots at the carrier for the same reason.
            if (other->op == IR_STORE && other->n_operands>=1 && other->operands[0]) {
                IrPlace t = ir_place_of(B->def, B->nvar, other->operands[0]);
                if (!(slot && t.base_kind==IRPB_LOCAL && t.base_id==slot->id))
                    for (int q=0;q<nsrc;q++)
                        if (ir_place_overlaps(&src[q], &t)) { bor_add(B, other->line, other->col, 4); return; }
                continue;
            }
            if (other->op != IR_CALL) continue;
            IrFunc *oc = bor_find_func(mod, other->aux.callee);
            if (!oc) continue;
            for (int a=0;a<other->n_operands;a++) {
                IrPlace p; bool m;
                if (!bor_arg_loan(B, oc, a, other->operands[a], &p, &m)) continue;
                if (slot && other->op==IR_CALL && p.base_kind==IRPB_LOCAL && p.base_id==slot->id) continue; // using the ref itself
                for (int q=0;q<nsrc;q++)
                    if (ir_place_overlaps(&src[q], &p)) { bor_add(B, other->line, other->col, 4); return; }
            }
        }
    }
}

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
    // phase A2: loans that outlive their statement
    if (mod) bor_check_regions(B, mod, f);
    // phase A: conflicting co-argument borrows at each call
    if (mod)
        for (IrBlock *b=f->blocks;b;b=b->next)
            for (IrInstr *i=b->instrs;i;i=i->next)
                if (i->op==IR_CALL) bor_check_call(B, mod, i);
    return B;
}
static void borrow_free(Borrow *B){ if(!B)return; free(B->def); free(B->finds); free(B); }

#endif // LAIN_BORROW_H
