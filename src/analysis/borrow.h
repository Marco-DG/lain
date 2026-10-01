// src/analysis/borrow.h — Phase 3.1: borrow / region checking as a sovereign IR pass.
//
// The high-value core is the DANGLING REFERENCE: a function must not return a pointer or
// slice whose provenance roots in one of ITS OWN local allocations — that storage dies at
// the return, so the caller would hold a dangling reference (a use-after-return / stack
// use-after-free). This is exactly what the old engine's E-borrow / region check catches.
//
// Method: escape analysis (bor_check_escapes). Every value that can flow into a returned
// reference, a store into storage the caller owns, or an argument a callee keeps is followed
// back; if any roots in a local IR_ALLOCA the reference escapes → flag. A param is a POINTER
// value (a borrow of the caller's storage), not an alloca, so returning it is fine.
//
// Sovereign: reads only ir.h. (NLL region lifetimes and two-phase borrows are follow-ups;
// this cut is the escaping-local-reference core — the memory-safety heart.)
#ifndef LAIN_BORROW_H
#define LAIN_BORROW_H

#include "../ir/ir.h"
#include "../ir/place.h"
#include "vra.h"          // phase D: numeric index disjointness
#include "effects.h"      // C5: the per-parameter write footprint
#include <stdlib.h>
#include <string.h>

// code: 10 = dangling return (E010) ; 4 = conflicting co-argument borrows (E004-class)
typedef struct { isize line, col; int code; } BorrowFinding;
typedef struct { IrFunc *f; IrInstr **def; int nvar; BorrowFinding *finds; int nfinds, cap; } Borrow;

static void bor_add(Borrow *B, isize line, isize col, int code) {
    // A deferred statement is replayed at every exit: one write through a reference is one E009.
    if (code == 9)
        for (int k = 0; k < B->nfinds; k++)
            if (B->finds[k].code == 9 && B->finds[k].line == line && B->finds[k].col == col) return;
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
// The single value ever stored into `slot`, or NULL if it is written zero or many times.
static IrValue *bor_unique_store_value(IrFunc *f, IrValue *slot) {
    if (!f || !slot) return NULL;
    IrValue *found = NULL; int n = 0;
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_STORE && i->n_operands>=2 && i->operands[0]==slot) { found = i->operands[1]; n++; }
    return n==1 ? found : NULL;
}

// The SLOT a place was reached through, when its root is a pointer LOADED from a local slot —
// the shape of every use of a reference binding (`r = 9`, `q.x = 5` after `var q = var p`).
// NULL when the root is anything else.
static IrValue *bor_deref_slot(IrInstr **def, int nvar, const IrPlace *t) {
    if (!t->valid || t->base_kind != IRPB_DEREF || t->base_id < 0 || t->base_id >= nvar) return NULL;
    IrInstr *ld = def[t->base_id];
    if (!ld || ld->op != IR_LOAD || ld->n_operands < 1 || !ld->operands[0]) return NULL;
    IrValue *slot = ld->operands[0];
    if (slot->id < 0 || slot->id >= nvar || !def[slot->id] || def[slot->id]->op != IR_ALLOCA) return NULL;
    return slot;
}

static IrValue *bor_unique_store_value(IrFunc *f, IrValue *slot);   // fwd
// Is an access at place `t` made THROUGH the reference held in `slot` — directly, or through a
// REBORROW of it (`var r2 = var r` stores the pointer loaded from r's slot into r2's)? Such an
// access is the loan being used, not a rival to it: `var r2 = var r; r2 = 6; r = r +% 1` wrote
// through r2 inside r's live region and was refused as a conflict with r itself.
static bool bor_through_carrier(IrFunc *f, IrInstr **def, int nvar, const IrPlace *t, IrValue *slot) {
    if (!slot) return false;
    IrValue *x = bor_deref_slot(def, nvar, t);
    for (int guard = 0; x && guard < 32; guard++) {
        if (x == slot) return true;
        IrValue *held = bor_unique_store_value(f, x);           // what x's binding was given
        IrInstr *hd = (held && held->id >= 0 && held->id < nvar) ? def[held->id] : NULL;
        if (!hd || hd->op != IR_LOAD || hd->n_operands < 1) return false;
        IrValue *y = hd->operands[0];                           // ...the pointer another slot held
        if (!y || y->id < 0 || y->id >= nvar || !def[y->id] || def[y->id]->op != IR_ALLOCA) return false;
        x = y;
    }
    return false;
}

// ★ A WRITE THROUGH A REFERENCE BINDING HAS A KNOWN TARGET. `r = 7` after `var r = var p.x` stores
// through a pointer LOADED from r's slot, and `ir_place_of` reads any load as a pointer of unknown
// provenance (IRPB_DEREF), which "may alias anything". So with two bindings to DISJOINT fields —
// `var r = var p.x` and `var s = var p.y` — a write through r was reported as conflicting with s's
// loan on p.y, "conflicting borrows of the same value", although the two places cannot overlap.
//
// The binding's slot is written EXACTLY ONCE (its initialiser), so the pointer it holds is the place
// that initialiser named, and a write through it touches that place — extended by whatever field
// path the write adds (`q.x = 5` through `var q = var p` is p.x). Only a UNIQUE store qualifies: a
// slot written twice might point at either referent, and then the conservative DEREF reading
// stands. Sound in the direction that matters — it can only narrow a DEREF to the one place the
// slot provably holds, and only when that place has a named root (a local or a parameter).
static IrPlace bor_place_through_binding(IrFunc *f, IrInstr **def, int nvar, IrValue *ptr) {
    IrPlace t = ir_place_of(def, nvar, ptr);
    IrValue *slot = bor_deref_slot(def, nvar, &t);
    if (!slot) return t;
    IrValue *held = bor_unique_store_value(f, slot);
    if (!held || held->id < 0 || held->id >= nvar) return t;
    IrPlace r = ir_place_of(def, nvar, held);
    if (!r.valid || r.base_kind == IRPB_DEREF) return t;      // a local's or a parameter's place
    if (r.nproj + t.nproj > IR_PLACE_MAX_PROJ) return t;          // too deep to name: stay DEREF
    for (int i = 0; i < t.nproj; i++) r.proj[r.nproj++] = t.proj[i];
    return r;
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
static IrFunc *bor_loan_mod = NULL;   // module for the write-footprint query (C5)

static bool bor_arg_loan_ex(Borrow *B, IrFunc *callee, int k, IrValue *arg, IrPlace *out,
                            bool *is_mut, bool *is_arr, bool *is_mv) {
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
    // ★ IRT_ARRAY BELONGS IN THIS LIST, and its absence was a soundness hole. A fixed-array
    // parameter is passed BY REFERENCE and the backend lowers it to `int32_t* restrict` —
    // `annot.h` says so explicitly ("a DECAYED fixed-array reference" returns true from the
    // restrict predicate). This list enumerated three aggregate kinds and omitted the fourth, so
    // an array argument was not a loan at all, the co-argument check never compared two of them,
    // and `addto(var x, x)` was ACCEPTED — emitting `e087_addto(v0, v0)` against a signature whose
    // two parameters are both `restrict`. Passing one pointer to two `restrict` parameters is
    // undefined behaviour whatever the optimiser happens to do with it today.
    //
    // Two lists had to agree about what a reference is and only one of them was maintained. The
    // legacy front end's E087 covered the gap until its seam stood it down, which is why this was
    // reachable at all — [[seam-only-where-engine-opines]]: the seam is legitimate only where the
    // new engine emits the obligation, and here it did not.
    bool is_borrow = (pt->kind==IRT_PTR || pt->kind==IRT_STRUCT || pt->kind==IRT_SLICE
                   || pt->kind==IRT_ARRAY);
    // A MOVE is an access too, and the strongest one: `conflict(res, mov res)` reads res
    // through one parameter while consuming it through another, leaving the first looking at
    // a moved-from value. Folding moves into the borrow framework as an access (design §2)
    // rather than a separate pass is what makes one conflict rule cover it — a consuming
    // parameter invalidates the place, so it conflicts with any overlapping access, exactly
    // as a mutable borrow does. This also makes a `mov` of a SCALAR a tracked access, which
    // a by-value copy is not.
    bool is_move = (p && p->value && p->value->owns);
    if (!is_borrow && !is_move) return false;
    // C5: mutability comes from the WRITE FOOTPRINT, not from the parameter's type.
    //
    // The type says a parameter MAY be written; the footprint says whether it IS. That
    // matters in BOTH directions and the type alone got both wrong:
    //   • a `var` scalar the callee never writes is not a mutable access — two arguments
    //     naming the same place is only undefined behaviour when one is WRITTEN (C's
    //     `restrict` is violated by a write, not by a reference), so `f(var a[i], var a[i])`
    //     with a callee that writes neither is legal, and was rejected;
    //   • a SLICE or STRUCT parameter never set is_mut at all, because only IRT_PTR carries
    //     `ptr_mut` — so `vadd(n, a, a)` writing `dst[i]` produced NO mutable loan and the
    //     whole-array aliasing case went uncaught. (That gap was invisible until the E087
    //     suppression made these programs reachable by the new pass.)
    //
    // A move always writes. Without a footprint (no module, or an extern) fall back to the
    // type, which is the conservative reading.
    if (is_move) *is_mut = true;
    else if (callee && bor_loan_mod && k < 64) {
        IrWriteFootprint w = ir_param_writes(callee, bor_loan_mod);
        *is_mut = ((w >> k) & 1u) != 0;
    } else {
        *is_mut = (pt->kind==IRT_PTR && pt->ptr_mut);
    }
    IrInstr *d = (arg->id>=0 && arg->id<B->nvar) ? B->def[arg->id] : NULL;
    IrPlace pl = (d && d->op==IR_LOAD && d->n_operands>=1)
               ? ir_place_of(B->def, B->nvar, d->operands[0])   // by-value read of a place
               : ir_place_of(B->def, B->nvar, arg);             // an address argument
    if (!pl.valid || pl.base_kind==IRPB_DEREF) return false;    // unresolved ⇒ no loan tracked
    // An ARRAY or SLICE reaching two parameters is the `restrict` violation the language
    // names E087, not a generic borrow conflict: it is what makes the emitted `restrict`
    // a lie. Reporting it as E004 loses the reason.
    // ...and also when the parameter is a SCALAR but the place is an ELEMENT of one:
    // `mix(var a[i], var a[j])` passes two i32s, yet what reaches the callee twice is the
    // array `a`. The projection says so even when the parameter type does not.
    // A MOVE beside a borrow of the same place is the narrower constraint E008 names,
    // "cannot move X because it is currently borrowed": `conflict(res, mov res)` and
    // `consume_data(var d, mov d)`. Reporting it as a generic conflict loses which of
    // the two arguments is the problem.
    if (is_mv) *is_mv = is_move;
    if (is_arr) {
        bool elem = false;
        for (int q=0; q<pl.nproj; q++) if (pl.proj[q].kind==IRPJ_INDEX) { elem = true; break; }
        *is_arr = (pt->kind==IRT_SLICE || pt->kind==IRT_ARRAY || elem);
    }
    *out = pl; return true;
}

static bool bor_arg_loan(Borrow *B, IrFunc *callee, int k, IrValue *arg,
                         IrPlace *out, bool *is_mut) {
    bool ignored = false;
    bool ignored2 = false;
    return bor_arg_loan_ex(B, callee, k, arg, out, is_mut, &ignored, &ignored2);
}

// Collect the loans a call's arguments create, INCLUDING those made by a nested call that
// computes an argument.
//
// `scale(var v, length(v))` was accepted because the second argument is the RESULT of a call:
// its defining instruction is an IR_CALL, no place resolves from it, and the read of `v` that
// happens inside `length` was therefore invisible at the outer call. In the IR the nested call
// is a separate, EARLIER instruction, so when it runs the outer call's mutable loan does not
// exist yet — the conflict is only visible if the outer call reaches down into it.
//
// Rust rejects the same program (E0502): two-phase borrows cover a method RECEIVER
// (`v.push(v.len())`), not an explicit `&mut` argument sitting beside a shared one. The
// by-value-copy rule in bor_arg_loan still protects the two-phase pattern, because a scalar
// copy creates no loan at any depth.
#define BOR_ARG_DEPTH 3
static void bor_collect_loans(Borrow *B, IrFunc *mod, IrInstr *call, IrFunc *callee,
                              IrPlace *pl, bool *mut, bool *arr, bool *mv,
                              int *n, int cap, int depth) {
    for (int k=0; k<call->n_operands && *n<cap; k++) {
        IrValue *arg = call->operands[k];
        IrPlace p; bool m; bool a = false; bool v = false;
        if (bor_arg_loan_ex(B, callee, k, arg, &p, &m, &a, &v)) {
            pl[*n]=p; mut[*n]=m; arr[*n]=a; mv[*n]=v; (*n)++; continue; }
        if (depth >= BOR_ARG_DEPTH || !arg || arg->id<0 || arg->id>=B->nvar) continue;
        IrInstr *d = B->def[arg->id];
        if (!d || d->op != IR_CALL) continue;
        IrFunc *inner = bor_find_func(mod, d->aux.callee);
        if (inner) bor_collect_loans(B, mod, d, inner, pl, mut, arr, mv, n, cap, depth+1);
    }
}

static void bor_check_call(Borrow *B, IrFunc *mod, IrInstr *call) {
    IrFunc *callee = bor_find_func(mod, call->aux.callee);
    if (!callee || call->n_operands < 2) return;
    IrPlace pl[16]; bool mut[16]; bool arr[16]; bool mv[16]; int n = 0;
    bor_collect_loans(B, mod, call, callee, pl, mut, arr, mv, &n, 16, 0);
    for (int i=0;i<n;i++)
        for (int j=i+1;j<n;j++)
            if ((mut[i] || mut[j]) && ir_place_overlaps(&pl[i], &pl[j])) {
                // 87 when an array/slice reaches two parameters (the restrict violation),
                // 4 otherwise. Two names for two constraints, as Annex B has them.
                // Three constraints, three names: 8 = one side MOVES what the other
                // borrows (E008); 87 = an array reaches two parameters (E087); 4 = a
                // conflict with neither shape. Move is checked first because it names the
                // ACTION, and an array can be moved too.
                int code = (mv[i] != mv[j]) ? 8 : (arr[i] || arr[j]) ? 87 : 4;
                bor_add(B, call->line, call->col, code);
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

// D-37: truncating at BOR_MAX_INSTR was SILENT, so every loan in a larger function was checked
// against a prefix of it. The conflict scan now walks the CFG directly (D-36), so this sequence
// is only used to find a carrier's home slot and to ask whether it is used at all — but a
// truncated answer to either is still a missed conflict. Measured over the corpus and std the
// high-water mark is 0 truncations, so the bound is generous; what was wrong was proceeding
// without saying so. A limit that is not reported is indistinguishable from a proof.
static void bor_linearize(IrFunc *f, BorSeq *s) {
    s->n = 0;
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next) {
            if (s->n >= BOR_MAX_INSTR) {
                fprintf(stderr, "error: '%.*s' exceeds %d instructions, which is more than the "
                        "borrow checker can sequence. Its loans would be checked against a "
                        "truncated function, so no borrow result is reported for it. Split the "
                        "function.\n",
                        f->name ? (int)f->name->length : 1, f->name ? f->name->name : "?",
                        BOR_MAX_INSTR);
                exit(1);
            }
            s->ins[s->n++] = i;
        }
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
// Address-forming ops only, so provenance is not laundered through a load or a call.
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
    if (!ir_ret_is_borrow(f)) return 0;
    if (f->ret_borrow_mask_done) return f->ret_borrow_mask;
    f->ret_borrow_mask_done = true;
    int nvar = f->next_value_id>0?f->next_value_id:1;
    IrInstr **def = calloc(nvar,sizeof(IrInstr*));
    if (!def) { f->ret_borrow_mask = bor_all_ref_params(f); return f->ret_borrow_mask; }
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next)
            if (i->result && i->result->id>=0 && i->result->id<nvar) def[i->result->id]=i;
    uint64_t mask = 0; bool opaque = false;
    // NO BODY (an extern) — nothing to infer from, so the honest answer is "it may borrow any
    // of them". An empty mask would say "it borrows nothing", which is a claim, not an absence
    // of one, and it silently discharged every conflict on an extern's returned reference.
    // Keyed on is_extern, not on `!f->blocks`: ir_func_new always makes an entry block, so an
    // extern has one — empty, but present.
    if (f->is_extern) opaque = true;
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
    // F1. On an EXTERN the annotation REPLACES the fallback: there is no body, so believing it
    // is what `extern` already means. On a function WITH a body the inferred mask is the
    // truth, and an annotation claiming LESS than the body does is a lie — reported, not
    // trusted. That check is only possible because the engine infers the answer independently;
    // it is the same shape as B2, where a caller may LEARN a fact only because the callee is
    // made to PROVE it.
    if (f->ret_borrow_annot) {
        if (f->is_extern) mask = f->ret_borrow_annot_mask;
        else if (mask & ~f->ret_borrow_annot_mask) {
            // Narrower than the truth. Keep the INFERRED mask — soundness is not negotiable —
            // and record it so the function's own analysis reports it.
            f->ret_borrow_annot_wrong = true;
        } else mask = f->ret_borrow_annot_mask;
    }
    f->ret_borrow_mask = mask;
    return mask;
}

static Borrow *borrow_analyze_mod(IrFunc *f, IrFunc *mod);
static Borrow *borrow_analyze(IrFunc *f) { return borrow_analyze_mod(f, NULL); }

// A SCOPED loan: `IR_BORROW place` … `IR_BORROW_END place`. Unlike the liveness-derived
// regions below, the extent is stated by the construct, because nothing's liveness expresses
// it — `case &x { 42: x = 99 }` borrows x for the whole match while no value in the arms
// reads that borrow.
static void bor_check_scoped(Borrow *B, IrFunc *mod, IrFunc *f) {
    for (IrBlock *sb=f->blocks; sb; sb=sb->next)
    for (IrInstr *si=sb->instrs; si; si=si->next) {
        if (si->op != IR_BORROW || si->n_operands < 1) continue;
        IrValue *place = si->operands[0];
        IrPlace src = ir_place_of(B->def, B->nvar, place);
        if (!src.valid) continue;
        // The region is every block REACHABLE from the borrow without passing its END — a
        // CFG question, not a textual one. A linear scan over the flattened instruction list
        // gets this wrong: the match's JOIN block (holding borrow_end) is emitted BEFORE the
        // arms, so the scan ended the region before ever reaching the writes it must catch.
        bool *seen = calloc(f->next_block_id, sizeof(bool));
        IrBlock **work = malloc(sizeof(IrBlock*) * (size_t)f->next_block_id);
        int nw = 0;
        bool reported = false;
        // walk the rest of the borrow's own block, then fan out
        for (IrBlock *cur = sb; cur && !reported; ) {
            IrInstr *from = (cur == sb) ? si->next : cur->instrs;
            bool ended = false;
            for (IrInstr *o = from; o && !reported; o = o->next) {
                if (o->op==IR_BORROW_END && o->n_operands>=1 && o->operands[0]==place) { ended=true; break; }
                if (o->op==IR_STORE && o->n_operands>=1 && o->operands[0]) {
                    IrPlace t = ir_place_of(B->def, B->nvar, o->operands[0]);
                    if (ir_place_overlaps(&src, &t)) { bor_add(B, o->line, o->col, 4); reported=true; }
                } else if (o->op==IR_CALL && mod) {
                    IrFunc *oc = bor_find_func(mod, o->aux.callee);
                    if (oc) for (int a=0;a<o->n_operands;a++) {
                        IrPlace p; bool m;
                        if (!bor_arg_loan(B, oc, a, o->operands[a], &p, &m)) continue;
                        if (m && ir_place_overlaps(&src, &p)) { bor_add(B, o->line, o->col, 4); reported=true; break; }
                    }
                }
            }
            if (!ended && !reported) {                     // fan out to successors
                IrBlock *succ[3]={0,0,0}; int ns=0;
                switch (cur->term.kind) {
                    case IR_TERM_BR:      succ[ns++]=cur->term.a; break;
                    case IR_TERM_BR_COND: succ[ns++]=cur->term.a; succ[ns++]=cur->term.b; break;
                    case IR_TERM_SWITCH:  succ[ns++]=cur->term.a;
                        for (IrSwitchCase *c=cur->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                    default: break;
                }
                for (int k=0;k<ns;k++)
                    if (succ[k] && !seen[succ[k]->id]) { seen[succ[k]->id]=true; work[nw++]=succ[k]; }
            }
            cur = nw ? work[--nw] : NULL;
        }
        free(seen); free(work);
        if (reported) return;
    }
}

// ── D-36: a loan's region is LIVENESS OVER THE CFG, not a window in a flattened list ────────
//
// `bor_linearize` walks f->blocks in LIST order and `bor_last_use` scans that list forward, so
// the region was the index range (creation, last_use]. Inside a loop whose body uses the
// carrier BEFORE it writes the borrowed place, the computed window ends before the write — and
// the back edge, which makes the next iteration's use follow it, is not in the list at all:
//
//     var ref = get_ref(var data)
//     while i < 3 {
//         consume_ref(var ref)    // use
//         data.value = 99         // conflict, at a LATER index than the use
//         i = i + 1
//     }                           // back edge: the next iteration uses an invalidated ref
//
// Accepted; --engine=legacy reports E004. The source comment claimed branch approximation was
// "narrower or equal" — narrower is the unsound direction, and a back edge is where it bites.
//
// The fix is the standard one (Brandner et al., "Computing Liveness Sets for SSA-Form
// Programs", which is what Hylo's last-use stage uses): compute, per block, whether a use of
// the carrier is REACHABLE FROM ITS ENTRY. A back edge is then an ordinary edge and needs no
// special case.
//
//     R[b] = uses(b) OR (OR over successors s of R[s])
//
// A loan created at instruction i0 is live at instruction j iff j is reachable from i0 and a
// use is reachable from j — i.e. a later use inside j's block, or R[s] for some successor.
#define BOR_MAX_BLOCKS 4096
typedef struct { bool reach_use[BOR_MAX_BLOCKS]; bool from_new[BOR_MAX_BLOCKS]; int nb; } BorLive;

static int bor_succs(IrBlock *b, IrBlock **out) {
    int n = 0;
    switch (b->term.kind) {
        case IR_TERM_BR:      if (b->term.a) out[n++] = b->term.a; break;
        case IR_TERM_BR_COND: if (b->term.a) out[n++] = b->term.a;
                              if (b->term.b) out[n++] = b->term.b; break;
        case IR_TERM_SWITCH:  if (b->term.a) out[n++] = b->term.a;
            for (IrSwitchCase *c = b->term.cases; c && n < 3; c = c->next)
                if (c->target) out[n++] = c->target;
            break;
        default: break;
    }
    return n;
}

// A DEFINITION of the carrier ends the old loan's liveness rather than extending it: the store
// that (re)initialises the slot — `var r = var i` inside a loop body runs again on the next
// iteration — or the instruction that defines the carrying value. Counting that store as a USE
// made the loan live across the back edge, so the loop condition's read of `i` sat inside the
// PREVIOUS iteration's region and `while i < 10 { var r = var i; r = 0; i = i + 1 }` drew E004
// on a reference that was already dead.
static bool bor_instr_kills(IrInstr *i, IrValue *val, IrValue *slot) {
    if (slot && i->op == IR_STORE && i->n_operands >= 1 && i->operands[0] == slot) return true;
    return val && i->result == val;
}
static bool bor_instr_uses(IrInstr *i, IrValue *val, IrValue *slot) {
    if (bor_instr_kills(i, val, slot)) return false;
    for (int o = 0; o < i->n_operands; o++) {
        IrValue *op = i->operands[o];
        if (!op) continue;
        if ((val && op == val) || (slot && op == slot)) return true;
    }
    return false;
}

// R[b]: is a use of the carrier reachable from the entry of block b? Backward fixpoint.
// `from_new[b]`: is b reachable from the creating block? Forward fixpoint.
static void bor_live_sets(IrFunc *f, IrBlock *newb, IrValue *val, IrValue *slot, BorLive *L) {
    L->nb = f->next_block_id > 0 ? f->next_block_id : 1;
    if (L->nb > BOR_MAX_BLOCKS) L->nb = BOR_MAX_BLOCKS;
    for (int i = 0; i < L->nb; i++) { L->reach_use[i] = false; L->from_new[i] = false; }

    // A block decides for itself when its first carrier event is a use (live) or a kill (dead
    // on entry, whatever follows); only a block with neither inherits from its successors.
    bool killed[BOR_MAX_BLOCKS];
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (b->id < 0 || b->id >= L->nb) continue;
        killed[b->id] = false;
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (bor_instr_uses(i, val, slot))  { L->reach_use[b->id] = true; break; }
            if (bor_instr_kills(i, val, slot)) { killed[b->id] = true;       break; }
        }
    }
    IrBlock *sc[4];
    for (bool ch = true; ch; ) {                       // backward: use reachable from entry
        ch = false;
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (b->id < 0 || b->id >= L->nb || L->reach_use[b->id] || killed[b->id]) continue;
            int ns = bor_succs(b, sc);
            for (int k = 0; k < ns; k++)
                if (sc[k]->id >= 0 && sc[k]->id < L->nb && L->reach_use[sc[k]->id]) {
                    L->reach_use[b->id] = true; ch = true; break;
                }
        }
    }
    if (newb && newb->id >= 0 && newb->id < L->nb) L->from_new[newb->id] = true;
    for (bool ch = true; ch; ) {                       // forward: reachable from the creation
        ch = false;
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (b->id < 0 || b->id >= L->nb || !L->from_new[b->id]) continue;
            int ns = bor_succs(b, sc);
            for (int k = 0; k < ns; k++)
                if (sc[k]->id >= 0 && sc[k]->id < L->nb && !L->from_new[sc[k]->id]) {
                    L->from_new[sc[k]->id] = true; ch = true;
                }
        }
    }
}

// Is the loan live at instruction `at` in block `b`? A use later in the same block keeps it
// live; otherwise it is live iff a use is reachable from some successor.
static bool bor_live_at(IrBlock *b, IrInstr *at, IrValue *val, IrValue *slot, BorLive *L) {
    if (b->id < 0 || b->id >= L->nb || !L->from_new[b->id]) return false;
    for (IrInstr *i = at->next; i; i = i->next) {
        if (bor_instr_uses(i, val, slot)) return true;
        if (bor_instr_kills(i, val, slot)) return false;   // a new loan starts here
    }
    IrBlock *sc[4]; int ns = bor_succs(b, sc);
    for (int k = 0; k < ns; k++)
        if (sc[k]->id >= 0 && sc[k]->id < L->nb && L->reach_use[sc[k]->id]) return true;
    return false;
}

static IrBlock *bor_block_of(IrFunc *f, IrInstr *target) {
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (i == target) return b;
    return NULL;
}
// A carrier may be used only across a back edge, which the flattened scan cannot see; ask the
// CFG instead before concluding the reference is never used.
static bool bor_carrier_used_anywhere(IrFunc *f, IrInstr *create, IrValue *val, IrValue *slot) {
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (i != create && bor_instr_uses(i, val, slot)) return true;
    return false;
}

// Is `v` the SLOT of a reference binding — an alloca whose contents are a mutable borrow
// (`var q = var p`)? Lowering types it `*var (*var T)` with the inner pointer marked borrowed,
// which a raw `*var T` local is not.
static bool bor_is_ref_slot(Borrow *B, IrValue *v) {
    if (!v || v->id < 0 || v->id >= B->nvar || !B->def[v->id]) return false;
    IrInstr *d = B->def[v->id];
    IrType  *t = (d->op == IR_ALLOCA) ? d->aux.alloca_ty : NULL;
    return t && t->kind == IRT_PTR && t->ptr_mut && t->borrowed;
}

static void bor_check_regions(Borrow *B, IrFunc *mod, IrFunc *f) {
    BorSeq s; bor_linearize(f, &s);
    for (int k=0;k<s.n;k++) {
        IrInstr *ins = s.ins[k];
        IrPlace src[8]; int nsrc = 0;
        // What keeps the loan alive: uses of `carrier` (the borrowing value) or of `slot`.
        IrValue *carrier = ins->result, *slot = NULL;
        if (ins->op == IR_STORE && ins->n_operands >= 2 && bor_is_ref_slot(B, ins->operands[0])) {
            // ★ A BORROW OF A WHOLE PLACE HAS NO ADDRESS INSTRUCTION OF ITS OWN. `var q = var p`
            // stores p's slot itself, `var r = var x` stores a `var` parameter's pointer, and
            // `var r2 = var r` stores the pointer r holds — none of them is the field/element
            // address the branch below recognises, so none created a loan, and `p = P(3, 4)` or
            // a read of `p.y` while q was still used went unchecked. The loan is created where
            // the binding is: the store into its slot. Field/element addresses and returned
            // borrows keep their own branches (they have a creating instruction to key on).
            IrValue *held = ins->operands[1];
            IrInstr *hd = (held && held->id >= 0 && held->id < B->nvar) ? B->def[held->id] : NULL;
            if (hd && (hd->op == IR_FIELD_PTR || hd->op == IR_ELEM_PTR || hd->op == IR_CALL)) continue;
            IrPlace p = bor_place_through_binding(f, B->def, B->nvar, held);
            if (!p.valid || p.base_kind == IRPB_DEREF) continue;   // an address we cannot name
            src[nsrc++] = p;
            slot = ins->operands[0]; carrier = NULL;
        } else if (!ins->result) {
            continue;
        } else if (ins->op == IR_CALL) {
            IrFunc *callee = bor_find_func(mod, ins->aux.callee);
            if (!callee || !ir_ret_is_borrow(callee)) continue;  // the return must BORROW a parameter
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
            // A PARAMETER's field is as nameable a place as a local's: `var a = var p.x` inside
            // `f(p var P)` holds p.x exactly as it would a local's. Keyed on LOCAL only, two live
            // bindings to the same parameter field created no loan at all and were accepted.
            if (!p.valid || p.base_kind == IRPB_DEREF) continue;
            src[nsrc++] = p;
        } else continue;
        if (!nsrc) continue;
        if (!slot) slot = bor_result_slot(&s, k, ins->result);
        if (bor_last_use(&s, k, carrier, slot) < 0 &&
            !bor_carrier_used_anywhere(f, ins, carrier, slot))
            continue;                                 // reference never used ⇒ no live region

        // D-36: the region is LIVENESS OVER THE CFG, not the index range (k, last]. Walk every
        // block reachable from the creating one and test each instruction for liveness, so an
        // instruction that follows the creation only across a BACK EDGE is inside the region.
        BorLive live;
        bor_live_sets(f, bor_block_of(f, ins), carrier, slot, &live);
        for (IrBlock *ob = f->blocks; ob; ob = ob->next) {
            if (ob->id < 0 || ob->id >= live.nb || !live.from_new[ob->id]) continue;
            bool after_creation = (ob != bor_block_of(f, ins));
            for (IrInstr *other = ob->instrs; other; other = other->next) {
                if (!after_creation) {                // skip up to and including the creation
                    if (other == ins) after_creation = true;
                    continue;
                }
                if (!bor_live_at(ob, other, carrier, slot, &live)) continue;
            // A DIRECT WRITE conflicts with a live loan just as a second borrow does — the
            // conflict rule (design §1.4) is over ACCESSES, not over calls. Loans were only
            // ever created and checked at call arguments, so `r = get_ref(var d); d.value = 99`
            // and whole-owner reassignment `d = D(99)` were both invisible.
            //
            // The store that CREATES the carrier (`store ref, <the borrow>`) targets the
            // carrier slot, not the borrowed place, so it cannot self-conflict; and a write
            // THROUGH the reference roots at the carrier for the same reason.
            //
            // ★ ...and a write through THIS reference is a USE of the loan, not a rival to it.
            // The old exemption compared the target's root with the carrier slot, but a write
            // through the reference stores via a pointer LOADED from that slot, whose place is
            // a DEREF rooted at the load — never the slot — so the exemption could not fire.
            // It went unnoticed because the write was always the reference's LAST use, where it
            // is no longer live; `r = 9; r = r +% 1` refused its own first line.
            if (other->op == IR_STORE && other->n_operands>=1 && other->operands[0]) {
                IrPlace t0 = ir_place_of(B->def, B->nvar, other->operands[0]);
                if (slot && bor_through_carrier(f, B->def, B->nvar, &t0, slot)) continue;
                IrPlace t = bor_place_through_binding(f, B->def, B->nvar, other->operands[0]);
                if (!(slot && t.base_kind==IRPB_LOCAL && t.base_id==slot->id))
                    for (int q=0;q<nsrc;q++)
                        if (ir_place_overlaps(&src[q], &t)) { bor_add(B, other->line, other->col, 4); return; }
                continue;
            }
            // ★ A READ of the borrowed place conflicts too — exclusivity is over ACCESSES, and a
            // load is one. Only stores and calls were checked, so `var r = var p.x; r = 9;
            // var t = p.x; r = 10` read the owner under a live mutable reference and was
            // accepted. (It LOOKED rejected: the E004 came from `r = 9`, which the region then
            // mistook for a rival to its own loan — one bug hiding another.) Loading the carrier
            // slot itself, or through the pointer it holds, is the reference being used.
            if (other->op == IR_LOAD && other->n_operands>=1 && other->operands[0]) {
                IrPlace t0 = ir_place_of(B->def, B->nvar, other->operands[0]);
                if (slot && t0.base_kind==IRPB_LOCAL && t0.base_id==slot->id) continue;
                if (slot && bor_through_carrier(f, B->def, B->nvar, &t0, slot)) continue;
                IrPlace t = bor_place_through_binding(f, B->def, B->nvar, other->operands[0]);
                // A read through an UNRESOLVED pointer is not charged. Every way such a pointer can
                // reach the borrowed place — another reference, a call's returned borrow — was
                // itself a conflicting access when it was created, and was refused there; charging
                // the read again would refuse every load through any pointer while a loan lives.
                if (t.base_kind == IRPB_DEREF) continue;
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
                if (bor_through_carrier(f, B->def, B->nvar, &p, slot)) continue;  // ...or passing it on
                for (int q=0;q<nsrc;q++)
                    if (ir_place_overlaps(&src[q], &p)) { bor_add(B, other->line, other->col, 4); return; }
            }
            }
        }
    }
}

// ── E010, TOTAL: a reference must not outlive the storage it points into ─────────────────────
// A local's storage dies when the function returns, so a reference into it may reach neither the
// return value nor any place the caller owns. The check used to chase ONE provenance chain
// and answer "not local" wherever it could not follow, so it failed OPEN: a
// struct returned through a binding (`s = St(1, xs[0..3])  return s`), a field store
// (`s.s = xs[0..3]`), a nested constructor, any sum, a variable assigned twice
// (`var p = &xs[0]  if c { p = &ys[0] }  return p`), and a local's slice stored into a `var`
// parameter's field all compiled, and each read a dead frame (ASan stack-use-after-return;
// `lain --interpret` "no longer live"). Only `return St(..)` and `return &xs[0]` were refused.
//
// Now every value that can flow into a reference is followed, and anything unknown counts as
// local unless it is provably not: a φ by all its inputs, a slot by everything stored into it
// (directly, through a reference binding, or by a call that receives its address), a constructor
// by its reference operands, a call by the arguments its callee may hand back. Three sinks:
//   · a returned value that can hold a reference;
//   · a store into storage that outlives the frame (a parameter's, or one reached through it);
//   · an argument the callee RETAINS (ir_param_retains) beside a destination that outlives the
//     frame. An extern is C's contract: it is believed to keep no pointer past the call.
// An ARRAY operand of a constructor is COPIED into the field, so it lends nothing (a struct or
// sum holds its arrays inline): `return St2(1, xs)` with an array field was refused as a borrow.
#define BOR_ESC_MAX_SLOTS 16
typedef struct { int16_t n; int8_t state; bool outliving; int slots[BOR_ESC_MAX_SLOTS]; } BorTargets;
typedef struct { IrValue **v; int n, cap; } BorVals;
typedef struct {
    IrFunc *f, *mod; IrInstr **def; int nvar;
    int *pidx;           // per value: its parameter index, or -1
    BorTargets *tg;      // per value: the local slots a pointer may point into, or "outliving"
    BorVals *direct;     // per slot: values stored straight into it (its address chain, no load)
    BorVals *stored;     // per slot: every value that may be stored into it
} BorEsc;

// What a callee does with its PARAMETERS' references, from the same walk:
//   ret   the parameters whose references may flow into its result;
//   keep  the parameters whose references may outlive the call: returned, stored where its
//         caller owns, or handed to a callee that keeps them beside such a place.
// A by-value aggregate is copied into a local slot before a field is read, so a summary taken
// off address-forming ops alone (bor_ret_borrow_mask, ir_param_retains) lost it:
// `unwrap(b Box) i32[] { return b.s }` handed back nothing, and `return unwrap(Box(xs[0..2]))`
// read a dead frame. An extern has no body: its result borrows what its type says
// (bor_ret_borrow_mask), and it is believed to keep nothing else. Memoized per module; a
// recursive cycle answers "every parameter" (fail-closed).
// and, for E009 (immutable storage), what it WRITES through them:
//   wsh   the parameters whose referent it writes (`d[0] = 9` through a slice parameter);
//   wdp   the parameters through which it writes storage a reference HELD in them points to
//         (`t = b.s  t[0] = 9` for a by-value `b Box`), so a struct passed `var` whose callee
//         writes only its own fields does not count as writing what its slices point into.
typedef struct { IrFunc *f; uint64_t ret, keep, wsh, wdp; int state; } BorSum;
#define BOR_SUM_MAX 4096
static BorSum  bor_sums[BOR_SUM_MAX];
static int     bor_nsums;
static IrFunc *bor_sums_mod;
static void bor_summary(IrFunc *callee, IrFunc *mod, uint64_t *ret, uint64_t *keep);
static void bor_summary_w(IrFunc *callee, IrFunc *mod, uint64_t *wsh, uint64_t *wdp);

static bool bor_holds_ref(const IrType *t, int depth) {
    if (!t || depth > 16) return false;
    switch (t->kind) {
        case IRT_PTR: case IRT_SLICE: return true;
        case IRT_ARRAY: return bor_holds_ref(t->elem, depth + 1);
        case IRT_STRUCT: case IRT_SUM:
            for (int k = 0; k < t->n_fields; k++) if (bor_holds_ref(t->fields[k], depth + 1)) return true;
            return false;
        default: return false;
    }
}
static void bor_vals_add(BorVals *L, IrValue *v) {
    if (!v) return;
    for (int k = 0; k < L->n; k++) if (L->v[k] == v) return;
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 4; L->v = realloc(L->v, (size_t)L->cap * sizeof *L->v); }
    L->v[L->n++] = v;
}
static void bor_tg_union(BorTargets *a, const BorTargets *b) {
    if (b->outliving) a->outliving = true;
    for (int k = 0; k < b->n; k++) {
        bool have = false;
        for (int j = 0; j < a->n; j++) if (a->slots[j] == b->slots[k]) have = true;
        if (have) continue;
        if (a->n == BOR_ESC_MAX_SLOTS) { a->outliving = true; continue; }   // too many: assume the worst
        a->slots[a->n++] = b->slots[k];
    }
}
// Which operand of a call holds argument 0 (an indirect call's op[0] is the callee).
static int bor_call_arg0(IrInstr *call) { return call->aux.callee ? 0 : 1; }
static IrFunc *bor_callee(BorEsc *E, IrInstr *call) {
    return call->aux.callee && E->mod ? ireff_find(E->mod, call->aux.callee) : NULL;
}
// The storage a pointer value may point into: local slots, or "outliving" (a parameter's, a
// call's, anything not followed). A cycle reads as outliving: the conservative answer.
static const BorTargets *bor_targets(BorEsc *E, IrValue *p) {
    static const BorTargets OUT = { 0, 2, true, {0} };
    if (!p || p->id < 0 || p->id >= E->nvar) return &OUT;
    BorTargets *T = &E->tg[p->id];
    if (T->state == 2) return T;
    if (T->state == 1) return &OUT;
    T->state = 1;
    BorTargets R; memset(&R, 0, sizeof R);
    IrInstr *d = E->def[p->id];
    if (!d) R.outliving = true;                                   // a parameter: the caller's
    else switch (d->op) {
        case IR_ALLOCA:
            if (d->data) R.outliving = true;                      // read-only static data
            else { R.n = 1; R.slots[0] = p->id; }
            break;
        case IR_FIELD_PTR: case IR_ELEM_PTR: case IR_SLICE_DATA: case IR_MAKE_SLICE:
        case IR_SUBSLICE: case IR_CAST:
            bor_tg_union(&R, bor_targets(E, d->n_operands >= 1 ? d->operands[0] : NULL));
            break;
        case IR_LOAD: {                                           // the pointers stored there
            const BorTargets *at = bor_targets(E, d->n_operands >= 1 ? d->operands[0] : NULL);
            if (at->outliving) R.outliving = true;
            for (int k = 0; k < at->n; k++) {
                BorVals *L = &E->direct[at->slots[k]];
                for (int j = 0; j < L->n; j++) bor_tg_union(&R, bor_targets(E, L->v[j]));
            }
            break;
        }
        case IR_PHI:
            for (IrPhiArg *a = d->phi_args; a; a = a->next) bor_tg_union(&R, bor_targets(E, a->value));
            break;
        case IR_CALL: {                     // a returned reference points where its arguments do
            IrFunc *callee = bor_callee(E, d);
            uint64_t rm = ~(uint64_t)0, km;
            if (callee) bor_summary(callee, E->mod, &rm, &km);
            int a0 = bor_call_arg0(d);
            R.outliving = true;                                   // ...or somewhere it owns
            for (int k = a0; k < d->n_operands; k++)
                if (k - a0 >= 64 || ((rm >> (k - a0)) & 1u)) bor_tg_union(&R, bor_targets(E, d->operands[k]));
            break;
        }
        default: R.outliving = true; break;
    }
    R.state = 2; *T = R;
    return T;
}
// The address chain of a store, followed without loads: the slot it writes directly, or -1.
static int bor_direct_slot(BorEsc *E, IrValue *a) {
    for (int guard = 0; a && a->id >= 0 && a->id < E->nvar && guard < 1000; guard++) {
        IrInstr *d = E->def[a->id];
        if (!d) return -1;
        if (d->op == IR_ALLOCA) return d->data ? -1 : a->id;
        if (d->op != IR_FIELD_PTR && d->op != IR_ELEM_PTR) return -1;
        a = d->n_operands >= 1 ? d->operands[0] : NULL;
    }
    return -1;
}

// The parameters an address that leaves the frame may come from: storage the caller passed in.
// Anything not followed counts as every parameter (fail-closed for the summary).
static uint64_t bor_addr_params(BorEsc *E, IrValue *a, int depth) {
    if (!a || a->id < 0 || a->id >= E->nvar || depth > 64) return ~(uint64_t)0;
    IrInstr *d = E->def[a->id];
    if (!d) { int pi = E->pidx[a->id]; return pi < 0 ? 0 : pi < 64 ? (uint64_t)1 << pi : ~(uint64_t)0; }
    uint64_t m = 0;
    switch (d->op) {
        case IR_ALLOCA: return 0;                                 // the frame's, or static data
        case IR_FIELD_PTR: case IR_ELEM_PTR: case IR_SLICE_DATA: case IR_MAKE_SLICE:
        case IR_SUBSLICE: case IR_CAST:
            return bor_addr_params(E, d->n_operands >= 1 ? d->operands[0] : NULL, depth + 1);
        case IR_PHI:
            for (IrPhiArg *p = d->phi_args; p; p = p->next) m |= bor_addr_params(E, p->value, depth + 1);
            return m;
        case IR_LOAD: {                                           // a pointer held in memory
            const BorTargets *t = bor_targets(E, d->n_operands >= 1 ? d->operands[0] : NULL);
            for (int k = 0; k < t->n; k++) {
                BorVals *L = &E->direct[t->slots[k]];
                for (int j = 0; j < L->n; j++) m |= bor_addr_params(E, L->v[j], depth + 1);
            }
            if (t->outliving) m |= bor_addr_params(E, d->n_operands >= 1 ? d->operands[0] : NULL, depth + 1);
            return m;
        }
        case IR_CALL: {
            IrFunc *callee = bor_callee(E, d);
            uint64_t rm = ~(uint64_t)0, km;
            if (callee) bor_summary(callee, E->mod, &rm, &km);
            int a0 = bor_call_arg0(d);
            for (int k = a0; k < d->n_operands; k++)
                if (k - a0 >= 64 || ((rm >> (k - a0)) & 1u)) m |= bor_addr_params(E, d->operands[k], depth + 1);
            return m;
        }
        case IR_STR_CONST: case IR_CONST: return 0;
        default: return ~(uint64_t)0;
    }
}

// Where can the references that flow into v come from? Returns true if any roots in one of this
// function's locals, and adds the parameters any roots in to *params.
static bool bor_walk(BorEsc *E, IrValue *v0, uint64_t *params) {
    char *seen = calloc((size_t)E->nvar, 1);
    IrValue **wl = malloc((size_t)(E->nvar > 0 ? E->nvar : 1) * sizeof *wl);
    if (!seen || !wl) { free(seen); free(wl); *params = ~(uint64_t)0; return true; }
    int top = 0; bool local = false;
#define BOR_PUSH(x) do { IrValue *x_ = (x); if (x_ && x_->id >= 0 && x_->id < E->nvar && !seen[x_->id]) { seen[x_->id] = 1; wl[top++] = x_; } } while (0)
    BOR_PUSH(v0);
    while (top > 0) {
        IrValue *v = wl[--top];
        IrInstr *d = E->def[v->id];
        if (!d) {                                                 // a parameter: the caller's
            int pi = E->pidx[v->id];
            if (pi >= 0) *params |= pi < 64 ? (uint64_t)1 << pi : ~(uint64_t)0;
            continue;
        }
        switch (d->op) {
            case IR_ALLOCA: if (!d->data) local = true; break;
            case IR_FIELD_PTR: case IR_ELEM_PTR: case IR_SLICE_DATA: case IR_MAKE_SLICE:
            case IR_SUBSLICE: case IR_CAST: case IR_SUM_PAYLOAD:
                if (d->n_operands >= 1) BOR_PUSH(d->operands[0]);
                break;
            case IR_PHI:
                for (IrPhiArg *a = d->phi_args; a; a = a->next) BOR_PUSH(a->value);
                break;
            case IR_LOAD: {                                       // whatever was stored there
                const BorTargets *t = bor_targets(E, d->n_operands >= 1 ? d->operands[0] : NULL);
                for (int k = 0; k < t->n; k++) {
                    BorVals *L = &E->stored[t->slots[k]];
                    for (int j = 0; j < L->n; j++) BOR_PUSH(L->v[j]);
                }
                // storage that outlives the frame holds what the caller put there: its parameters'
                // references (anything local stored there is a sink of its own)
                if (t->outliving) *params |= bor_addr_params(E, d->n_operands >= 1 ? d->operands[0] : NULL, 0);
                break;
            }
            case IR_STRUCT_NEW: case IR_SUM_NEW: {
                IrType *rt = d->result ? d->result->type : NULL;
                IrType *pl = rt && rt->kind == IRT_SUM && d->aux.sum.variant >= 0 && d->aux.sum.variant < rt->n_fields
                           ? rt->fields[d->aux.sum.variant] : rt;
                for (int k = 0; k < d->n_operands; k++) {
                    IrValue *o = d->operands[k];
                    IrType *ft = pl && (pl->kind == IRT_STRUCT || pl->kind == IRT_SUM) && k < pl->n_fields ? pl->fields[k] : NULL;
                    if (ft && ft->kind == IRT_ARRAY) {            // copied in: lends nothing, but
                        if (!bor_holds_ref(ft->elem, 0)) continue;  // the references it holds move
                        const BorTargets *t = bor_targets(E, o);
                        for (int s = 0; s < t->n; s++) {
                            BorVals *L = &E->stored[t->slots[s]];
                            for (int j = 0; j < L->n; j++) BOR_PUSH(L->v[j]);
                        }
                        if (t->outliving) *params |= bor_addr_params(E, o, 0);
                        continue;
                    }
                    if (o && bor_holds_ref(o->type, 0)) BOR_PUSH(o);
                }
                break;
            }
            case IR_CALL: {
                IrFunc *callee = bor_callee(E, d);
                uint64_t rm = ~(uint64_t)0, km;
                if (callee) bor_summary(callee, E->mod, &rm, &km);
                int a0 = bor_call_arg0(d);
                for (int k = a0; k < d->n_operands; k++) {
                    int ai = k - a0;
                    if (ai < 64 && !((rm >> ai) & 1u)) continue;
                    if (d->operands[k] && bor_holds_ref(d->operands[k]->type, 0)) BOR_PUSH(d->operands[k]);
                }
                break;
            }
            case IR_CONST: case IR_STR_CONST: case IR_FUNC_REF: case IR_OPAQUE:
            case IR_SUM_TAG: case IR_SLICE_LEN: case IR_ICMP:
                break;
            default:                                              // anything else: its inputs
                for (int k = 0; k < d->n_operands; k++)
                    if (d->operands[k] && bor_holds_ref(d->operands[k]->type, 0)) BOR_PUSH(d->operands[k]);
                break;
        }
    }
#undef BOR_PUSH
    free(seen); free(wl);
    return local;
}

// One pass over f: what each slot may hold, then the three sinks. With B it reports E010 for
// every reference to a local that reaches one; it always fills f's summary (*ret, *keep).
static bool bor_is_ref(const IrType *t) {
    return t && (t->kind == IRT_PTR || t->kind == IRT_SLICE || t->kind == IRT_ARRAY);
}
// The parameters a write through `a` changes: *sh those whose referent it writes, *dp those
// through which it reaches storage by a reference held inside them (a by-value aggregate's
// references, or one read out of a parameter's referent).
static void bor_write_params(BorEsc *E, IrValue *a, bool deep0, uint64_t *sh, uint64_t *dp) {
    char *seen = calloc((size_t)E->nvar * 2, 1);
    IrValue **wv = malloc((size_t)E->nvar * 2 * sizeof *wv); char *wd = malloc((size_t)E->nvar * 2);
    if (!seen || !wv || !wd) { free(seen); free(wv); free(wd); *sh = *dp = ~(uint64_t)0; return; }
    int top = 0;
#define BOR_WPUSH(x, dd) do { IrValue *x_ = (x); bool d_ = (dd); if (x_ && x_->id >= 0 && x_->id < E->nvar && !seen[x_->id * 2 + d_]) { seen[x_->id * 2 + d_] = 1; wv[top] = x_; wd[top++] = d_; } } while (0)
    BOR_WPUSH(a, deep0);
    while (top > 0) {
        --top; IrValue *v = wv[top]; bool deep = wd[top];
        IrInstr *d = E->def[v->id];
        if (!d) {
            int pi = E->pidx[v->id];
            if (pi < 0) continue;
            uint64_t bit = pi < 64 ? (uint64_t)1 << pi : ~(uint64_t)0;
            if (deep || !bor_is_ref(v->type)) *dp |= bit; else *sh |= bit;
            continue;
        }
        switch (d->op) {
            case IR_ALLOCA: break;                                // the frame's own storage
            case IR_FIELD_PTR: case IR_ELEM_PTR: case IR_SLICE_DATA: case IR_MAKE_SLICE:
            case IR_SUBSLICE: case IR_CAST: case IR_SUM_PAYLOAD:
                if (d->n_operands >= 1) BOR_WPUSH(d->operands[0], deep);
                break;
            case IR_PHI:
                for (IrPhiArg *p = d->phi_args; p; p = p->next) BOR_WPUSH(p->value, deep);
                break;
            case IR_LOAD: {                    // a copy of what was stored, or a reference read
                const BorTargets *t = bor_targets(E, d->n_operands >= 1 ? d->operands[0] : NULL);
                for (int k = 0; k < t->n; k++) {               // out of the caller's storage
                    BorVals *L = &E->stored[t->slots[k]];
                    for (int j = 0; j < L->n; j++) BOR_WPUSH(L->v[j], deep);
                }
                if (t->outliving && d->n_operands >= 1) BOR_WPUSH(d->operands[0], true);
                break;
            }
            case IR_CALL: {
                IrFunc *callee = bor_callee(E, d);
                uint64_t rm = ~(uint64_t)0, km;
                if (callee) bor_summary(callee, E->mod, &rm, &km);
                int a0 = bor_call_arg0(d);
                for (int k = a0; k < d->n_operands; k++)
                    if (k - a0 >= 64 || ((rm >> (k - a0)) & 1u)) BOR_WPUSH(d->operands[k], deep);
                break;
            }
            case IR_CONST: case IR_STR_CONST: case IR_FUNC_REF: case IR_OPAQUE:
            case IR_SUM_TAG: case IR_SLICE_LEN: case IR_ICMP:
                break;
            default:
                for (int k = 0; k < d->n_operands; k++)
                    if (d->operands[k] && bor_holds_ref(d->operands[k]->type, 0)) BOR_WPUSH(d->operands[k], deep);
                break;
        }
    }
#undef BOR_WPUSH
    free(seen); free(wv); free(wd);
}
static int32_t bor_slot_seal(BorEsc *E, int slot) {
    IrInstr *d = (slot >= 0 && slot < E->nvar) ? E->def[slot] : NULL;
    return d && d->op == IR_ALLOCA ? d->seal : 0;
}
// Does a write through a reference HELD in or by v (not v's own referent) reach immutable
// storage whose seal is not `own`?
static bool bor_deep_hits_seal(BorEsc *E, IrValue *v, int32_t own) {
    char *seen = calloc((size_t)E->nvar, 1);
    IrValue **wl = malloc((size_t)(E->nvar > 0 ? E->nvar : 1) * sizeof *wl);
    if (!seen || !wl) { free(seen); free(wl); return true; }
    int top = 0; bool hit = false;
#define BOR_DPUSH(x) do { IrValue *x_ = (x); if (x_ && x_->id >= 0 && x_->id < E->nvar && !seen[x_->id] && bor_holds_ref(x_->type, 0)) { seen[x_->id] = 1; wl[top++] = x_; } } while (0)
    if (bor_is_ref(v->type)) {                                   // what its referent holds
        const BorTargets *t = bor_targets(E, v);
        for (int s = 0; s < t->n; s++) { BorVals *L = &E->stored[t->slots[s]]; for (int j = 0; j < L->n; j++) BOR_DPUSH(L->v[j]); }
    } else BOR_DPUSH(v);
    while (top > 0 && !hit) {
        IrValue *x = wl[--top];
        if (bor_is_ref(x->type)) {                               // a reference: where it points,
            const BorTargets *t = bor_targets(E, x);             // and what is held there
            for (int s = 0; s < t->n; s++) {
                int32_t sl = bor_slot_seal(E, t->slots[s]);
                if (sl && sl != own) { hit = true; break; }
                BorVals *L = &E->stored[t->slots[s]]; for (int j = 0; j < L->n; j++) BOR_DPUSH(L->v[j]);
            }
            continue;
        }
        IrInstr *d = E->def[x->id];                              // an aggregate: its references
        if (!d) continue;
        switch (d->op) {
            case IR_LOAD: {
                const BorTargets *t = bor_targets(E, d->n_operands >= 1 ? d->operands[0] : NULL);
                for (int s = 0; s < t->n; s++) { BorVals *L = &E->stored[t->slots[s]]; for (int j = 0; j < L->n; j++) BOR_DPUSH(L->v[j]); }
                break;
            }
            case IR_PHI: for (IrPhiArg *p = d->phi_args; p; p = p->next) BOR_DPUSH(p->value); break;
            case IR_CALL: {
                IrFunc *callee = bor_callee(E, d);
                uint64_t rm = ~(uint64_t)0, km;
                if (callee) bor_summary(callee, E->mod, &rm, &km);
                int a0 = bor_call_arg0(d);
                for (int k = a0; k < d->n_operands; k++)
                    if (k - a0 >= 64 || ((rm >> (k - a0)) & 1u)) BOR_DPUSH(d->operands[k]);
                break;
            }
            default: for (int k = 0; k < d->n_operands; k++) BOR_DPUSH(d->operands[k]); break;
        }
    }
#undef BOR_DPUSH
    free(seen); free(wl);
    return hit;
}

static void bor_escape_pass(Borrow *B, IrFunc *f, IrFunc *mod, uint64_t *ret, uint64_t *keep,
                            uint64_t *wsh, uint64_t *wdp) {
    *ret = 0; *keep = 0; *wsh = 0; *wdp = 0;
    BorEsc E; memset(&E, 0, sizeof E);
    E.f = f; E.mod = mod; E.nvar = f->next_value_id > 0 ? f->next_value_id : 1;
    E.def = calloc((size_t)E.nvar, sizeof *E.def);
    E.pidx = malloc((size_t)E.nvar * sizeof *E.pidx);
    E.tg = calloc((size_t)E.nvar, sizeof *E.tg);
    E.direct = calloc((size_t)E.nvar, sizeof *E.direct);
    E.stored = calloc((size_t)E.nvar, sizeof *E.stored);
    if (!E.def || !E.pidx || !E.tg || !E.direct || !E.stored) { *ret = *keep = *wsh = *wdp = ~(uint64_t)0; goto done; }
    for (int k = 0; k < E.nvar; k++) E.pidx[k] = -1;
    { int pi = 0;
      for (IrParam *p = f->params; p; p = p->next, pi++)
          if (p->value && p->value->id >= 0 && p->value->id < E.nvar) E.pidx[p->value->id] = pi; }
    for (IrBlock *b = f->blocks; b; b = b->next) {
        for (IrInstr *i = b->phis; i; i = i->next) if (i->result && i->result->id < E.nvar) E.def[i->result->id] = i;
        for (IrInstr *i = b->instrs; i; i = i->next) if (i->result && i->result->id < E.nvar) E.def[i->result->id] = i;
    }
    // what each slot holds: direct stores first (bor_targets reads them), then the rest
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (i->op == IR_STORE && i->n_operands >= 2) {
                int s = bor_direct_slot(&E, i->operands[0]);
                if (s >= 0) bor_vals_add(&E.direct[s], i->operands[1]);
            }
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (i->op == IR_STORE && i->n_operands >= 2) {
                IrValue *val = i->operands[1];
                if (!val || !bor_holds_ref(val->type, 0)) continue;
                const BorTargets *t = bor_targets(&E, i->operands[0]);
                for (int k = 0; k < t->n; k++) bor_vals_add(&E.stored[t->slots[k]], val);
            } else if (i->op == IR_CALL) {
                // A callee may store any reference it keeps into storage another argument
                // reaches: a slot passed by address may come back holding it.
                IrFunc *callee = bor_callee(&E, i);
                uint64_t rm, km = ~(uint64_t)0;
                if (callee) bor_summary(callee, mod, &rm, &km);
                int a0 = bor_call_arg0(i);
                for (int k = a0; k < i->n_operands; k++) {
                    IrValue *dst = i->operands[k];
                    if (!dst || !dst->type || (dst->type->kind != IRT_PTR && dst->type->kind != IRT_SLICE && dst->type->kind != IRT_ARRAY)) continue;
                    const BorTargets *t = bor_targets(&E, dst);
                    for (int s = 0; s < t->n; s++)
                        for (int j = a0; j < i->n_operands; j++) {
                            int aj = j - a0;
                            if (j == k || (aj < 64 && !((km >> aj) & 1u))) continue;
                            if (i->operands[j] && bor_holds_ref(i->operands[j]->type, 0)) bor_vals_add(&E.stored[t->slots[s]], i->operands[j]);
                        }
                }
            }
        }
    // the sinks
    for (IrBlock *b = f->blocks; b; b = b->next) {
        for (IrInstr *i = b->instrs; i; i = i->next) {
            // E009: what this instruction writes, for the summary, and whether it reaches
            // immutable storage from outside the seal that initialises it
            if (i->op == IR_STORE && i->n_operands >= 2) {
                bor_write_params(&E, i->operands[0], false, wsh, wdp);
                const BorTargets *t = bor_targets(&E, i->operands[0]);
                bool hit = false;
                for (int s = 0; !hit && s < t->n; s++) {
                    int32_t sl = bor_slot_seal(&E, t->slots[s]);
                    if (sl && sl != i->seal) hit = true;
                }
                if (B && hit) bor_add(B, i->line, i->col, 9);
            } else if (i->op == IR_CALL) {
                IrFunc *callee = bor_callee(&E, i);
                uint64_t ws = ~(uint64_t)0, wd = ~(uint64_t)0;
                if (callee) bor_summary_w(callee, mod, &ws, &wd);
                int a0 = bor_call_arg0(i);
                bool reported = false;
                for (int k = a0; k < i->n_operands; k++) {
                    IrValue *a = i->operands[k]; int ak = k - a0;
                    if (!a) continue;
                    bool sh = ak >= 64 || ((ws >> ak) & 1u), dp = ak >= 64 || ((wd >> ak) & 1u);
                    if (sh) bor_write_params(&E, a, false, wsh, wdp);
                    if (dp) bor_write_params(&E, a, true, wsh, wdp);
                    // ee32cee reports an immutable array passed straight to a writing parameter
                    if (!B || reported || (ak < 64 && ((i->ro_args >> ak) & 1u))) continue;
                    if (sh && bor_is_ref(a->type)) {
                        const BorTargets *t = bor_targets(&E, a);
                        for (int s = 0; s < t->n && !reported; s++) {
                            int32_t sl = bor_slot_seal(&E, t->slots[s]);
                            if (sl && sl != i->seal) { bor_add(B, i->line, i->col, 9); reported = true; }
                        }
                    }
                    if (dp && !reported && bor_holds_ref(a->type, 0) && bor_deep_hits_seal(&E, a, i->seal)) {
                        bor_add(B, i->line, i->col, 9); reported = true;
                    }
                }
            }
            if (i->op == IR_STORE && i->n_operands >= 2) {        // into storage the caller owns
                IrValue *val = i->operands[1];
                if (!val || !bor_holds_ref(val->type, 0)) continue;
                if (!bor_targets(&E, i->operands[0])->outliving) continue;
                uint64_t pm = 0;
                if (bor_walk(&E, val, &pm) && B) bor_add(B, i->line, i->col, 10);
                *keep |= pm;
            } else if (i->op == IR_CALL) {                        // kept by a callee beside it
                IrFunc *callee = bor_callee(&E, i);
                uint64_t rm, km = ~(uint64_t)0;
                if (callee) bor_summary(callee, mod, &rm, &km);
                int a0 = bor_call_arg0(i);
                bool outliving_dst = false;
                for (int k = a0; k < i->n_operands && !outliving_dst; k++) {
                    IrValue *dst = i->operands[k];
                    if (dst && dst->type && dst->type->kind == IRT_PTR && bor_targets(&E, dst)->outliving)
                        outliving_dst = true;
                }
                if (!outliving_dst) continue;
                bool reported = false;
                for (int j = a0; j < i->n_operands; j++) {
                    int aj = j - a0;
                    if (aj < 64 && !((km >> aj) & 1u)) continue;
                    IrValue *a = i->operands[j];
                    if (!a || !bor_holds_ref(a->type, 0)) continue;
                    uint64_t pm = 0;
                    if (bor_walk(&E, a, &pm) && B && !reported) { bor_add(B, i->line, i->col, 10); reported = true; }
                    *keep |= pm;
                }
            }
        }
        if (b->term.kind == IR_TERM_RET && b->term.cond) {        // returned
            IrValue *rv = b->term.cond;
            if (!bor_holds_ref(rv->type, 0)) continue;
            uint64_t pm = 0;
            if (bor_walk(&E, rv, &pm) && B)                       // reported at the `return`
                bor_add(B, b->term.line ? b->term.line : rv->line, b->term.line ? b->term.col : rv->col, 10);
            *ret |= pm; *keep |= pm;
        }
    }
done:
    for (int k = 0; k < E.nvar; k++) {
        if (E.direct) free(E.direct[k].v);
        if (E.stored) free(E.stored[k].v);
    }
    free(E.def); free(E.pidx); free(E.tg); free(E.direct); free(E.stored);
}

static void bor_summary(IrFunc *callee, IrFunc *mod, uint64_t *ret, uint64_t *keep) {
    *ret = *keep = ~(uint64_t)0;
    if (!callee) return;
    if (callee->is_extern) {
        *ret = *keep = ir_ret_is_borrow(callee) ? bor_ret_borrow_mask(callee) : 0;
        return;
    }
    if (mod != bor_sums_mod) { bor_nsums = 0; bor_sums_mod = mod; }
    for (int k = 0; k < bor_nsums; k++)
        if (bor_sums[k].f == callee) {
            if (bor_sums[k].state == 2) { *ret = bor_sums[k].ret; *keep = bor_sums[k].keep; }
            return;                                               // in progress: everything
        }
    if (bor_nsums == BOR_SUM_MAX) return;
    int me = bor_nsums++;
    bor_sums[me].f = callee; bor_sums[me].state = 1;
    bor_sums[me].wsh = bor_sums[me].wdp = ~(uint64_t)0;            // in progress: everything
    uint64_t r, kp, ws, wd;
    bor_escape_pass(NULL, callee, mod, &r, &kp, &ws, &wd);
    bor_sums[me].ret = r; bor_sums[me].keep = kp; bor_sums[me].wsh = ws; bor_sums[me].wdp = wd;
    bor_sums[me].state = 2;
    *ret = r; *keep = kp;
}
// What a callee writes through its parameters. An extern is believed by its TYPES: a `*var T`,
// an array or a slice parameter (an output reference) may be written, through anything it
// reaches; a `*T` may not.
static void bor_summary_w(IrFunc *callee, IrFunc *mod, uint64_t *wsh, uint64_t *wdp) {
    *wsh = *wdp = ~(uint64_t)0;
    if (!callee) return;
    if (callee->is_extern) {
        uint64_t m = 0; int k = 0;
        for (IrParam *p = callee->params; p; p = p->next, k++) {
            IrType *t = p->value ? p->value->type : NULL;
            if (k < 64 && t && ((t->kind == IRT_PTR && t->ptr_mut) || t->kind == IRT_ARRAY || t->kind == IRT_SLICE))
                m |= (uint64_t)1 << k;
        }
        *wsh = *wdp = m;
        return;
    }
    uint64_t r, kp;
    bor_summary(callee, mod, &r, &kp);                            // fills the memo entry
    for (int k = 0; k < bor_nsums; k++)
        if (bor_sums[k].f == callee && bor_sums[k].state == 2) { *wsh = bor_sums[k].wsh; *wdp = bor_sums[k].wdp; return; }
}

static void bor_check_escapes(Borrow *B, IrFunc *f, IrFunc *mod) {
    uint64_t r, k, ws, wd;
    bor_escape_pass(B, f, mod, &r, &k, &ws, &wd);
}

static Borrow *borrow_analyze_mod(IrFunc *f, IrFunc *mod) {
    Borrow *B = calloc(1,sizeof *B); B->f=f;
    B->nvar = f->next_value_id>0?f->next_value_id:1;
    B->def = calloc(B->nvar,sizeof(IrInstr*));
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next) if (i->result) B->def[i->result->id]=i;
    bor_check_escapes(B, f, mod);                             // E010: returns, stores, calls
    // phase A2: loans that outlive their statement
    if (mod) bor_check_regions(B, mod, f);
    // Scoped loans stated by a construct (`case &x`). Unlike the other phases this does NOT
    // require a module: a write conflicting with a scoped borrow is visible in one function.
    bor_check_scoped(B, mod, f);
    // phase A: conflicting co-argument borrows at each call.
    //
    // ★ Phase D is active here: the numeric domain answers `a[i]` vs `a[j]`. The conflict
    // rule stays conservative by DEFAULT (unknown indices ⇒ assume overlap, which is what
    // catches `f(var a[i], var a[i])`), and the octagon only ever REMOVES a conflict it can
    // prove is not one. The query needs a POINT — the octagon state is per-block — so the
    // block and instruction are set before each call is examined.
    // F1: an `in <param>` that claims less than the body actually borrows. Asking for the mask
    // is what computes this, so the query has to happen before the report.
    if (ir_ret_is_borrow(f)) { (void)bor_ret_borrow_mask(f);
        if (f->ret_borrow_annot_wrong) bor_add(B, 0, 0, 11); }
    if (mod) {
        bor_loan_mod = mod;
        VraDisjoint *D = vra_disjoint_open(f);
        for (IrBlock *b=f->blocks;b;b=b->next)
            for (IrInstr *i=b->instrs;i;i=i->next)
                if (i->op==IR_CALL) {
                    if (D) { D->blk = b; D->at = i; }
                    bor_check_call(B, mod, i);
                }
        vra_disjoint_close(D);
        bor_loan_mod = NULL;
    }
    return B;
}
static void borrow_free(Borrow *B){ if(!B)return; free(B->def); free(B->finds); free(B); }

#endif // LAIN_BORROW_H
