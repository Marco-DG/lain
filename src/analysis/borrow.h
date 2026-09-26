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
#include "vra.h"          // phase D: numeric index disjointness
#include "effects.h"      // C5: the per-parameter write footprint
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
// The single value ever stored into `slot`, or NULL if it is written zero or many times.
static IrValue *bor_unique_store_value(IrFunc *f, IrValue *slot) {
    if (!f || !slot) return NULL;
    IrValue *found = NULL; int n = 0;
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_STORE && i->n_operands>=2 && i->operands[0]==slot) { found = i->operands[1]; n++; }
    return n==1 ? found : NULL;
}

static bool bor_roots_local(IrFunc *f, IrInstr **def, int nvar, IrValue *v) {
    for (int guard=0; v && v->id>=0 && v->id<nvar && guard<100000; guard++) {
        IrInstr *d = def[v->id];
        if (!d) return false;                             // param / φ with no single def
        switch (d->op) {
            case IR_ALLOCA:                                return true;    // a local stack slot
            case IR_ELEM_PTR: case IR_FIELD_PTR:
            case IR_SLICE_DATA: case IR_MAKE_SLICE:
                v = d->n_operands>=1 ? d->operands[0] : NULL; break;       // provenance = base/data
            case IR_LOAD: {
                // A load launders provenance in general — but when the slot it reads is
                // written EXACTLY ONCE, the loaded value is that stored value and provenance
                // survives. This is what makes the escaping-local case visible at all:
                // `var local = "hello"; return Lexer(local, 0)` LOADS the slice out of
                // local's slot before it enters the struct, and stopping at the load reported
                // nothing. (Reading a slot written from a PARAMETER still roots at the param,
                // so this adds no false positives — it follows the value, not the slot.)
                IrValue *addr = d->n_operands>=1 ? d->operands[0] : NULL;
                IrValue *stored = bor_unique_store_value(f, addr);
                if (!stored) return false;
                v = stored; break;
            }
            default: return false;                        // call/… — not a known local
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

static bool bor_instr_uses(IrInstr *i, IrValue *val, IrValue *slot) {
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

    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (b->id < 0 || b->id >= L->nb) continue;
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (bor_instr_uses(i, val, slot)) { L->reach_use[b->id] = true; break; }
    }
    IrBlock *sc[4];
    for (bool ch = true; ch; ) {                       // backward: use reachable from entry
        ch = false;
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (b->id < 0 || b->id >= L->nb || L->reach_use[b->id]) continue;
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
    for (IrInstr *i = at->next; i; i = i->next)
        if (bor_instr_uses(i, val, slot)) return true;
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

static void bor_check_regions(Borrow *B, IrFunc *mod, IrFunc *f) {
    BorSeq s; bor_linearize(f, &s);
    for (int k=0;k<s.n;k++) {
        IrInstr *ins = s.ins[k];
        if (!ins->result) continue;
        IrPlace src[8]; int nsrc = 0;
        if (ins->op == IR_CALL) {
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
            if (!p.valid || p.base_kind != IRPB_LOCAL) continue;
            src[nsrc++] = p;
        } else continue;
        if (!nsrc) continue;
        IrValue *slot = bor_result_slot(&s, k, ins->result);
        if (bor_last_use(&s, k, ins->result, slot) < 0 &&
            !bor_carrier_used_anywhere(f, ins, ins->result, slot))
            continue;                                 // reference never used ⇒ no live region

        // D-36: the region is LIVENESS OVER THE CFG, not the index range (k, last]. Walk every
        // block reachable from the creating one and test each instruction for liveness, so an
        // instruction that follows the creation only across a BACK EDGE is inside the region.
        BorLive live;
        bor_live_sets(f, bor_block_of(f, ins), ins->result, slot, &live);
        for (IrBlock *ob = f->blocks; ob; ob = ob->next) {
            if (ob->id < 0 || ob->id >= live.nb || !live.from_new[ob->id]) continue;
            bool after_creation = (ob != bor_block_of(f, ins));
            for (IrInstr *other = ob->instrs; other; other = other->next) {
                if (!after_creation) {                // skip up to and including the creation
                    if (other == ins) after_creation = true;
                    continue;
                }
                if (!bor_live_at(ob, other, ins->result, slot, &live)) continue;
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
            dangles = bor_roots_local(f, B->def, B->nvar, rv);         // return a local reference
        else if (rv->type && rv->type->kind==IRT_STRUCT) {         // return a struct that BORROWS a
            IrInstr *d = B->def[rv->id];                            // local through a pointer/slice field
            if (d && d->op==IR_STRUCT_NEW)
                for (int k=0;k<d->n_operands && !dangles;k++) {
                    IrValue *fv = d->operands[k];
                    if (fv && fv->type && (fv->type->kind==IRT_PTR || fv->type->kind==IRT_SLICE))
                        dangles = bor_roots_local(f, B->def, B->nvar, fv);
                }
        }
        if (dangles) bor_add(B, rv->line, rv->col, 10);
    }
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
