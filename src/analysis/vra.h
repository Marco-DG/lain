// src/analysis/vra.h — the value-range analysis: an octagon abstract interpreter
// run to fixpoint over an IrFunc's CFG (Phase 2.3–2.5 of the rebuild).
//
// Variable model: one octagon variable per IR value id. Locals are still in memory
// (alloca/load/store) at this phase, so an ALLOCA's own value id doubles as its
// *scalar cell*: a LOAD copies the cell into the loaded SSA value, a STORE copies
// a value back into the cell. That is what lets the loop counter relate to itself
// across iterations without a separate mem2reg pass — the store `i = i+1` becomes
// the octagon fact cell_i' = cell_i + 1.
//
// The whole engine's soundness rests on octagon.h's γ-anchor: every transfer
// over-approximates, so the converged state contains every reachable concrete
// state, and a bounds/overflow obligation proven against it is proven for real.
#ifndef LAIN_VRA_H
#define LAIN_VRA_H

#include "analysis/octagon.h"
#include "ir/ir.h"
#include "analysis/footprint.h"   // the alias oracle: who a call can write / retain
#include "ir/place.h"   // phase D: the index-disjointness seam we fill
#include <stdlib.h>
#include <string.h>

static IrFunc *vra_mod = NULL;   // module for callee lookup; NULL disables the query
static IrFunc *vra_find_func(const IrName *n) {
    if (!n || !vra_mod) return NULL;
    // by CONTENT — ir_intern allocates a fresh IrName per call despite its name
    for (IrFunc *g=vra_mod; g; g=g->next)
        if (g->name && g->name->length==n->length && memcmp(g->name->name,n->name,(size_t)n->length)==0)
            return g;
    return NULL;
}

// One discharged (or not) proof obligation.
#define VRA_MAX_RANK 4

typedef enum { VRA_BOUNDS, VRA_OVERFLOW, VRA_DIVZERO, VRA_TERMINATION, VRA_PRECOND } VraCheckKind;

// ── A1: WHERE THE FACT DIED ───────────────────────────────────────────────────────────────
// An unproven obligation is not evidence for anything until you know WHICH capability was
// missing. Three precision questions in a row were answered by bisecting programs instead of
// reading the abstract state; this records the answer at the point of failure so the next one
// is answered by a tally. The categories map 1:1 onto the Stage B items in REBUILD.md, so the
// survey ORDERS that stage rather than confirming a guess about it.
typedef enum {
    VLOSS_NONE = 0,   // discharged
    VLOSS_PRODUCT,    // loop-carried with a NON-constant step: `s = s + a[i]`. Needs a bound
                      // of the form s0 + T*delta, which is a PRODUCT and unstatable in any
                      // relational domain. Stage B1 (loop summarisation).
    VLOSS_WIDEN,      // loop-carried with a CONSTANT step in a 64-bit slot: nothing wider
                      // can be bounding it, so the domain SHOULD reach this and widening
                      // threw it away. A precision bug, not a missing capability.
    VLOSS_NARROW,     // ★ loop-carried, constant step, but the slot is NARROWER than 64 bits
                      // while the thing bounding it (a slice length) is usize. `var i = 0`
                      // is an i32; `while i in data` compares it unsigned against a usize
                      // length, so `i + 1` really can leave i32 and the engine is RIGHT to
                      // refuse. Split out because lumping it with WIDEN said "18 precision
                      // bugs" when most of them are the language letting a counter be
                      // narrower than what bounds it — an L1 problem, not a domain one.
    VLOSS_CALL,       // an operand comes from a call: needs a postcondition summary. B4.
    VLOSS_ARITY,      // three or more distinct symbolic values: octagons are binary. B5.
    VLOSS_JOIN,       // the value is defined at a merge point: the join is a hull, so a
                      // disjunction was flattened. B2 (partitioning).
    // ── the two below are NOT capability gaps. The engine is refusing correctly. ──────────
    VLOSS_UNBOUNDED,  // every symbolic operand is an unrefined parameter, so the operation
                      // GENUINELY can overflow for some input. Prove-or-reject working as
                      // designed: the program needs a guard, a wider type or `+%`. Counting
                      // these as precision loss would inflate every number in this survey.
    VLOSS_NOLEN,      // no length is known for the array at all. Not a domain weakness
                      // either: nothing in scope says how long it is.
    VLOSS_OTHER
} VraLoss;

static const char *vra_loss_name(VraLoss l) {
    switch (l) {
        case VLOSS_NONE:    return "discharged";
        case VLOSS_PRODUCT: return "product";
        case VLOSS_WIDEN:   return "widen";
        case VLOSS_NARROW:  return "narrow-counter";
        case VLOSS_CALL:    return "call";
        case VLOSS_ARITY:   return "arity";
        case VLOSS_JOIN:    return "join";
        case VLOSS_UNBOUNDED: return "unbounded";
        case VLOSS_NOLEN:   return "no-length";
        default:            return "other";
    }
}
typedef struct {
    VraCheckKind kind;
    IrInstr *at;
    bool     ok;        // discharged: the access/op is provably safe (check-free)
    // bounds detail
    bool     lo_ok;     // proved idx ≥ 0
    bool     hi_ok;     // proved idx < len
    bool     has_len;   // a length was found at all
    VraLoss  loss;      // when !ok: which capability was missing (A1)
    // ── why a running total could not be bounded, for the diagnostic ─────────────────────
    // B1 computes all of this in order to FAIL. A user told only "arithmetic is not provably
    // free of overflow" has no way to know the problem is the trip count, or which of three
    // things to change. 42% of unproven obligations are the engine refusing correctly, so the
    // quality of the refusal IS the user experience.
    bool     accum;                       // the numbers below are meaningful
    int64_t  accum_T, accum_dlo, accum_dhi, accum_s0lo, accum_s0hi;
    int64_t  line, col;
} VraCheck;

typedef struct {
    IrFunc  *f;
    int      nvar;      // = next_value_id
    int     *odim;      // VARIABLE PACKING (2.2): value id → octagon slot, or −1 (untracked)
    int      noct;      // number of packed slots — the octagon's real variable count
    int      dsz;       // octagon storage per block = dim*dim
    int64_t **in;       // in[bid] : entry octagon storage (NULL = unreached)
    bool    *reached;
    IrInstr **def;      // def[val id] = producing instruction (NULL for params)
    IrValue **val;      // val[val id] = the value itself (for its TYPE — see vra_range)
    int     *defblk;    // defblk[val id] = id of the block defining it (-1 = param)
    int64_t *cval; bool *cknown;   // constant values (from IR_CONST)
    int     *slicelen;  // slice value id → its canonical length var (−1 = none)
    int     *cellcanon; // value id → canonical id of the PLACE it names (field_ptr aliasing)
    bool    *subslice_gep;  // elem_ptr result feeding a make_slice (a subslice start,
                            // not an element access — checked by the make_slice instead)
    // ESCAPED cells: an alloca whose ADDRESS leaves this instruction's control — passed to a
    // call, stored as a value, or returned. The octagon models a scalar alloca as a stable
    // memory cell, and nothing was invalidating that cell when someone else could write it:
    //     proc bump(var i usize) { i = i +% 100 }
    //     var i usize = 0 ;  bump(var i) ;  a[i]        // a is i32[4]
    // left the octagon believing i == 0, so `a[i]` was PROVEN check-free while the program
    // actually reads a[100]. A FALSE PROOF is a removed bounds check, so any call must havoc
    // every cell whose address could have reached it.
    bool    *escaped;   // address left this instruction's control at some point
    bool    *persist;   // ...and OUTLIVED the call — stored into memory, returned, or handed
                        // to a callee that RETAINS it. Only these must be havoced at EVERY
                        // call; the rest can be havoced precisely (see the alias oracle).
    // S2: rank-N region shapes, read off the IR_SHAPE instructions. Indexed by the BASE
    // value id; shape_rank[b] > 0 means b has extents shape_ext[b][0..rank-1].
    int     *shape_rank;
    int    (*shape_ext)[VRA_MAX_RANK];
    VraCheck *checks; int nchecks, cap_checks;
} Vra;

// ── small helpers ────────────────────────────────────────────────────────────
static int vra_var(IrValue *v) { return v ? v->id : -1; }
static bool vra_is_int(IrValue *v){ return v && v->type &&
        (v->type->kind==IRT_INT || v->type->kind==IRT_BOOL); }

// is value id `v` a slice-typed alloca cell?
// Follow an address back to the alloca it roots in and mark that cell escaped.
// A `var x i32` parameter is a POINTER to the caller's storage, and the domain modelled it
// as nothing at all: every read of it was ⊤, so `pos = pos + 1` under `pos < toks.len` could
// not be shown not to overflow, and no guard on it ever survived. It is a stable numeric cell
// for the duration of the call — Lain's mutable borrows are EXCLUSIVE, which is the same
// guarantee the backend already relies on when it emits these parameters `restrict`, and the
// borrow checker is what enforces it. Treating it as a cell is reading that guarantee, not
// assuming one; a call it is passed on to still havocs it, exactly like an escaped alloca.
static bool vra_is_param_cell(Vra *V, int id) {
    if (id<0 || id>=V->nvar || V->def[id]) return false;      // must be a parameter
    IrValue *pv = V->val[id];
    return pv && pv->type && pv->type->kind==IRT_PTR && pv->type->ptr_mut
        && pv->type->elem && (pv->type->elem->kind==IRT_INT || pv->type->elem->kind==IRT_BOOL);
}
// Mark every CELL whose address is reachable from `v` as having escaped — and, when
// `persist`, as OUTLIVING the call (stored into memory, returned, or handed to a callee that
// keeps it). A persisting cell can be written by a call that never received it, so it is the
// one case the alias oracle must stay conservative about.
//
// This is a TREE walk, not a chain walk. `Holder(var i)` carries &i inside an IR_STRUCT_NEW
// and the store that follows stores the STRUCT — a walk that stopped at the constructor saw
// no address at all and marked nothing, which was a fail-open predating the oracle. Casts are
// followed for the same reason: a laundered pointer is still the address.
static void vra_mark_addr(Vra *V, IrValue *v, bool persist, int depth) {
    if (!v || v->id<0 || v->id>=V->nvar || depth>64) return;
    IrInstr *d = V->def[v->id];
    if (!d) return;                                   // a parameter: not our cell
    if (d->op == IR_ALLOCA) {
        V->escaped[v->id] = true;
        if (persist) V->persist[v->id] = true;
        return;
    }
    switch (d->op) {
        case IR_ELEM_PTR: case IR_FIELD_PTR:
        case IR_SLICE_DATA: case IR_MAKE_SLICE: case IR_SUBSLICE: case IR_CAST:
            vra_mark_addr(V, d->n_operands>=1?d->operands[0]:NULL, persist, depth+1); return;
        case IR_STRUCT_NEW: case IR_ARRAY_NEW: case IR_SUM_NEW:
            for (int i=0;i<d->n_operands;i++) vra_mark_addr(V, d->operands[i], persist, depth+1);
            return;
        default: return;                              // not an address we can attribute
    }
}
static void vra_mark_persist(Vra *V, IrValue *v) { vra_mark_addr(V, v, true,  0); }
static void vra_mark_escape (Vra *V, IrValue *v) { vra_mark_addr(V, v, false, 0); }

// The alias oracle's CALLER side: which cell does this argument name?
//   >= 0  that cell — a local alloca, or a parameter's own pointer value
//   VRA_ARG_VALUE   not an address at all (a by-value scalar): it names no cell
//   VRA_ARG_UNKNOWN an address we cannot attribute (loaded from memory, cast, arithmetic)
// The last one is why this returns three answers rather than two: an unattributable address
// may be ANY escaped cell, so a call given one has to fall back to the blanket havoc. Getting
// that wrong is not imprecision, it is a miscompile.
#define VRA_ARG_VALUE   (-1)
#define VRA_ARG_UNKNOWN (-2)
static int vra_arg_cell(Vra *V, IrValue *v) {
    for (int guard=0; v && v->id>=0 && v->id<V->nvar && guard<10000; guard++) {
        IrInstr *d = V->def[v->id];
        if (!d) {   // no defining instruction ⇒ a parameter; its own value IS the cell
            return vra_is_param_cell(V, v->id) ? v->id
                 : (v->type && (v->type->kind==IRT_PTR || v->type->kind==IRT_SLICE))
                   ? VRA_ARG_UNKNOWN : VRA_ARG_VALUE;
        }
        if (d->op == IR_ALLOCA) return v->id;
        switch (d->op) {
            case IR_ELEM_PTR: case IR_FIELD_PTR:
            case IR_SLICE_DATA: case IR_MAKE_SLICE:
                v = d->n_operands>=1 ? d->operands[0] : NULL; break;
            default:
                return (v->type && (v->type->kind==IRT_PTR || v->type->kind==IRT_SLICE))
                       ? VRA_ARG_UNKNOWN : VRA_ARG_VALUE;
        }
    }
    return VRA_ARG_UNKNOWN;
}

static bool vra_is_slice_cell(Vra *V, int v) {
    IrInstr *d = (v>=0 && v<V->nvar) ? V->def[v] : NULL;
    if (!d) return false;
    if (d->op==IR_ALLOCA) return d->aux.alloca_ty && d->aux.alloca_ty->kind==IRT_SLICE;
    // A slice-typed FIELD is a cell too. It was not one, so nothing about a struct's slice
    // field ever propagated — `l.text[l.pos]` had no length for `l.text` at all.
    if (d->op==IR_FIELD_PTR)
        return d->result && d->result->type && d->result->type->elem
            && d->result->type->elem->kind==IRT_SLICE;
    return false;
}
// The same PLACE reached twice is two different SSA values: `l.text` lowers to a fresh
// field_ptr at each mention. Keyed by value id, the length learned at one mention was
// invisible at the next — which is exactly what made the struct `in` invariant useless, since
// the assume names one load of `text` and the index check another. Canonicalise a field
// access to the first field_ptr with the same (base, field); everything else is its own.
static int vra_canon_cell(Vra *V, int v) { return (v>=0 && v<V->nvar && V->cellcanon) ? V->cellcanon[v] : v; }
// pre-pass: def sites, constants, and canonical slice-length vars. A slice's length
// var is its make_slice length operand or its first slice_len read; it is propagated
// through a slice local's store/load so `s = a[lo..hi]; s[k]` knows len(s).
static void vra_prepass(Vra *V) {
    for (IrParam *p=V->f->params; p; p=p->next)
        if (p->value && p->value->id>=0 && p->value->id<V->nvar) V->val[p->value->id]=p->value;
    // (the escape flag is set after the prepass has classified them — see below)
    for (int i=0;i<V->nvar;i++){ V->def[i]=NULL; V->defblk[i]=-1; V->cknown[i]=false; V->slicelen[i]=-1; V->subslice_gep[i]=false; }
    // first: def sites + constants (needed to classify slice cells below)
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->result){ V->def[ins->result->id]=ins; V->defblk[ins->result->id]=b->id;
                              V->val[ins->result->id]=ins->result; }
            if (ins->op==IR_CONST && ins->result){ V->cknown[ins->result->id]=true; V->cval[ins->result->id]=ins->aux.imm; }
        }
    V->cellcanon = malloc((size_t)V->nvar*sizeof(int));
    for (int i=0;i<V->nvar;i++) V->cellcanon[i]=i;
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op!=IR_FIELD_PTR || !ins->result || ins->n_operands<1) continue;
            int me = ins->result->id, base = ins->operands[0]->id;
            for (IrBlock *b2=V->f->blocks; b2; b2=b2->next)
                for (IrInstr *o=b2->instrs; o; o=o->next) {
                    if (o==ins) goto done;
                    if (o->op==IR_FIELD_PTR && o->result && o->n_operands>=1
                        && o->operands[0]->id==base && o->aux.field_idx==ins->aux.field_idx) {
                        V->cellcanon[me] = V->cellcanon[o->result->id]; goto done;
                    }
                }
            done: ;
        }
    int *cell_len = malloc(V->nvar*sizeof(int));
    for (int i=0;i<V->nvar;i++) cell_len[i]=-1;
    // How many times is each slice cell STORED? A cell's canonical length is only meaningful
    // while the cell holds one slice for its whole life. `s = borrow(big); if i < s.len { s =
    // borrow(small); return s[i] }` reassigns to a SHORTER slice, and a length cached from the
    // first must not survive it — that is a false proof and an out-of-bounds read. The count
    // is what makes both directions safe: exactly one store ⇒ the stored slice's length is the
    // cell's; ZERO stores ⇒ a stack VLA, whose length comes from the slice_len seeding below;
    // anything else ⇒ no canonical length at all.
    int *cell_stores = calloc((size_t)V->nvar, sizeof(int));
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next)
            if (ins->op==IR_STORE && ins->n_operands>=2 && vra_is_slice_cell(V, ins->operands[0]->id))
                cell_stores[vra_canon_cell(V, ins->operands[0]->id)]++;
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op==IR_MAKE_SLICE && ins->result && ins->n_operands>=2) {
                V->slicelen[ins->result->id] = ins->operands[1]->id;      // {data,len}: len is the length var
                IrInstr *dd = V->def[ins->operands[0]->id];               // subslice start (vs array→slice decay)
                if (dd && dd->op==IR_ELEM_PTR) V->subslice_gep[ins->operands[0]->id] = true;
            }
            else if (ins->op==IR_SLICE_LEN && ins->result && ins->n_operands>=1) {
                // A cell only learned its length from a STORE, so a stack VLA — allocated and
                // never assigned — had none anywhere. Seed it from a `.len` read on a load out
                // of the cell, but ONLY for a cell nothing stores to: a stored slice's own
                // length var is the better representative, because it is the one the rest of
                // the octagon already relates to the buffer.
                int s=ins->operands[0]->id;
                IrInstr *sd = V->def[s];
                if (sd && sd->op==IR_LOAD && sd->n_operands>=1) {
                    int cell = vra_canon_cell(V, sd->operands[0]->id);
                    if (vra_is_slice_cell(V,cell) && cell_stores[cell]==0 && cell_len[cell]<0)
                        cell_len[cell]=ins->result->id;
                }
            }
            else if (ins->op==IR_STORE && ins->n_operands>=2) {
                int cell=vra_canon_cell(V, ins->operands[0]->id), v=ins->operands[1]->id;
                if (vra_is_slice_cell(V,cell))
                    cell_len[cell] = (cell_stores[cell]==1) ? V->slicelen[v] : -1;
            }
        }
    // A SECOND pass for the loads. The scan is linear, but a cell's length is not necessarily
    // learned before the first load out of it — `l.text[l.pos]` loads `text` to index it and
    // only then reads `.len` for the invariant, so a one-pass propagation saw nothing. The
    // length of a cell is a property of the whole function, not of a program point (that is
    // exactly why the single-store rule above has to be enforced), so collecting first and
    // propagating second is the honest order.
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op!=IR_LOAD || !ins->result || ins->n_operands<1) continue;
            int cell=vra_canon_cell(V, ins->operands[0]->id);
            if (vra_is_slice_cell(V,cell) && cell_len[cell]>=0 && V->slicelen[ins->result->id]<0)
                V->slicelen[ins->result->id]=cell_len[cell];
        }
    // THIRD, and only now: a bare `.len` read names the length of a slice that still has no
    // canonical one. This has to come LAST. Run before the propagation above, it claimed the
    // loaded value for a fresh slice_len result and locked out the store's length var — the
    // one every other constraint is stated against — which silently un-proved `buf[1..4]`.
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next)
            if (ins->op==IR_SLICE_LEN && ins->result && ins->n_operands>=1) {
                int s=ins->operands[0]->id;
                if (V->slicelen[s]<0) V->slicelen[s]=ins->result->id;
            }
    free(cell_len); free(cell_stores);

    // A `var` scalar parameter points at storage the CALLER owns, so anything we hand the
    // pointer to may write it: it havocs at a call exactly like an escaped alloca.
    for (IrParam *p=V->f->params; p; p=p->next)
        if (p->value && vra_is_param_cell(V, p->value->id)) {
            V->escaped[p->value->id] = true;
            // ...and PERSISTS: the storage is the caller's, and we cannot see whether the
            // caller stashed its address somewhere a callee of ours can reach. (Recovering
            // these needs a call-site summary — the one precision the oracle leaves on the
            // table. It costs nothing today: this is exactly the old blanket behaviour.)
            V->persist[p->value->id] = true;
        }
    // Mark every alloca whose ADDRESS escapes. Provenance is followed through the
    // address-forming ops, so `f(&s.field)` escapes `s` too. A store's TARGET (operand 0) is
    // an ordinary write and does NOT escape; a store's VALUE (operand 1) does.
    for (IrBlock *b=V->f->blocks; b; b=b->next) {
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op == IR_SHAPE && ins->n_operands >= 3) {
                int b = ins->operands[0]->id;
                int rank = ins->n_operands - 1;
                if (rank > VRA_MAX_RANK) rank = VRA_MAX_RANK;
                if (b>=0 && b<V->nvar) {
                    V->shape_rank[b] = rank;
                    for (int k=0;k<rank;k++) V->shape_ext[b][k] = ins->operands[1+k]->id;
                }
                continue;
            }
            if (ins->op == IR_CALL || ins->op == IR_OPAQUE) {
                // ★ THE ALIAS ORACLE, first half. Handing an address to a call is not the same
                // as losing it: the callee can write it DURING the call, and afterwards only if
                // it RETAINED it. An unresolvable callee or an opaque retains everything.
                IrFunc *callee = (ins->op==IR_CALL) ? vra_find_func(ins->aux.callee) : NULL;
                IrRetainFootprint cr = (ins->op==IR_OPAQUE) ? ~(IrRetainFootprint)0
                                     : (callee && vra_mod) ? ir_param_retains(callee, vra_mod)
                                                           : ~(IrRetainFootprint)0;
                for (int k=0;k<ins->n_operands;k++) {
                    vra_mark_escape(V, ins->operands[k]);
                    if (k>=64 || ((cr>>k)&1u)) vra_mark_persist(V, ins->operands[k]);
                }
            } else if (ins->op == IR_STORE && ins->n_operands>=2) {
                vra_mark_persist(V, ins->operands[1]);   // an address stored into memory PERSISTS
            }
        }
        if (b->term.kind == IR_TERM_RET) vra_mark_persist(V, b->term.cond);   // outlives us
    }
}

// copy `src == dst` (equal values) into octagon o
// c*x, but only if it stays within the octagon's usable range (oct_add_* doubles the
// bound internally, so keep well inside OCT_INF). Returns false ⇒ leave that side unbounded.
static bool vra_safe_scale(int64_t c, int64_t x, int64_t *out) {
    __int128 p = (__int128)c * (__int128)x;
    if (p > (__int128)(OCT_INF/2) || p < -(__int128)(OCT_INF/2)) return false;
    *out = (int64_t)p; return true;
}
static void vra_interval(Vra *V, const Octagon *W, int id, int64_t *lo, bool *hl, int64_t *hi, bool *hh); // fwd
static void vra_add_diff_le(Vra *V, Octagon *W, int a, int b, int64_t c);                                 // fwd
static int64_t vra_diff_ub(Vra *V, const Octagon *W, int a, int b);                                       // fwd
static void vra_range(Vra *V, Octagon *W, IrValue *v, int64_t *lo, int64_t *hi);                          // fwd

static void vra_assign_copy(Vra *V, Octagon *o, int dst, int src) {
    // A CONSTANT source has no dimension to copy from — it lives in the constant table — so
    // the copy must state its value directly. Without this `var i usize = 0` reached its slot
    // carrying nothing at all, and with it every loop counter in the corpus: this one line is
    // ten of the fifty obligations in the calculator program.
    if (src>=0 && src<V->nvar && V->cknown[src]) {
        oct_forget(o, dst); oct_add_const(o, dst, V->cval[src]); return;
    }
    oct_close(o);                      // materialize src's transitive bounds BEFORE the copy
    oct_forget(o, dst);                // (so dst inherits them; this is where a loop invariant
    vra_add_diff_le(V, o, dst, src, 0);//  is carried through a memory-cell load). Copies are
    vra_add_diff_le(V, o, src, dst, 0);//  infrequent (loads/casts/lengths), so O(dim^3) here
}                                      //  is fine — unlike per-instruction forget-closes.

// A constant has no octagon dimension (see the packing note), so every interval read must
// consult the constant table first. This is not a workaround: the exact value is strictly
// better information than any interval the domain could hold.
static void vra_interval(Vra *V, const Octagon *W, int id,
                         int64_t *lo, bool *hl, int64_t *hi, bool *hh) {
    if (id>=0 && id<V->nvar && V->cknown[id]) { *lo=*hi=V->cval[id]; *hl=*hh=true; return; }
    oct_interval(W, id, lo, hl, hi, hh);
}

// ── the constant table IS the domain for constants ───────────────────────────────────────
// With constants packed out of the octagon, every RELATIONAL site has to consult the table
// too, or the relation is silently lost: `idx − len ≤ −1` against a fixed array's constant
// length is the bounds check itself. These two are the only way the rest of the file should
// state or read a difference.
//
// Both are strictly more precise than the octagon form they replace — an exact value beats
// any interval, and an absolute bound needs no closure step to become usable.
static int64_t vra_diff_ub(Vra *V, const Octagon *W, int a, int b) {   // upper bound on a − b
    bool ac = (a>=0 && a<V->nvar && V->cknown[a]), bc = (b>=0 && b<V->nvar && V->cknown[b]);
    if (ac && bc) return V->cval[a] - V->cval[b];
    int64_t lo,hi; bool hl,hh;
    if (ac) { vra_interval(V,W,b,&lo,&hl,&hi,&hh); return hl ? V->cval[a]-lo : OCT_INF; }
    if (bc) { vra_interval(V,W,a,&lo,&hl,&hi,&hh); return hh ? hi-V->cval[b] : OCT_INF; }
    return oct_get(W, oct_pos(b), oct_pos(a));
}
static void vra_add_diff_le(Vra *V, Octagon *W, int a, int b, int64_t c) {   // a − b ≤ c
    if (c >= OCT_INF) return;
    bool ac = (a>=0 && a<V->nvar && V->cknown[a]), bc = (b>=0 && b<V->nvar && V->cknown[b]);
    if (ac && bc) return;                                  // both known: nothing to record
    if (bc)      oct_add_ub(W, a, V->cval[b] + c);         // a ≤ const + c
    else if (ac) oct_add_lb(W, b, V->cval[a] - c);         // b ≥ const − c
    else         oct_add_diff_le(W, a, b, c);
}

static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir);  // fwd
static void vra_free(Vra *V);                                                    // fwd (phase D)
static bool vra_dump_enabled = false;   // --dump-octagon: print the converged state
static void vra_dump_state(Vra *V, FILE *o);   // fwd

// ── INFERRED RETURN RANGES ───────────────────────────────────────────────────────────────
// A call's result was simply FORGOTTEN, so `LUT[nib(c)]` could not be proven even though
// `func nib(c u8) u8 { return c & 0x0F }` can only return 0..15. The range is read off the
// callee's BODY, exactly as the borrow mask is: analyse the callee, union the interval of
// every returned value, memoize on the IrFunc.
//
// Soundness. The callee is analysed with its parameters at their declared intervals and its
// entry assumes in force, so the interval holds for every legal call — which is the same
// contract the caller is separately required to satisfy. A recursive query returns nothing
// rather than a fixpoint over itself: `state == 1` falls back to the type interval.
static bool vra_ret_range(IrFunc *g, int64_t *lo, int64_t *hi);
static void vra_check_narrow(Vra *V, Octagon *W, IrValue *val, IrType *target,
                             IrInstr *at, int64_t line, int64_t col);   // fwd (Path-F's other half)
static int  vra_shape_len_for_stride(Vra *V, int stride_id, int *e0_out);        // fwd (S2)
static bool vra_factor_shape(Vra *V, Octagon *W, int sbase, int idx);            // fwd (S2)

// ── transfer of one instruction over working octagon W ───────────────────────
// Everything integer division tells us about `r = x / D` for a constant D ≥ 1 and x ≥ 0.
// The relational half is what the octagon cannot derive on its own, and it is what the
// binary-search family needs:
//
//   r ≥ 0, r ≤ x            always
//   r ≤ x − 1               when D ≥ 2 and x ≥ 1   — STRICTLY smaller, the midpoint's engine
//   x − r ≤ xhi − xhi/D     when D ≥ 2             — bounds `x − x/D` (i.e. ceil((D−1)x/D)),
//                                                    sound because x − x/D is nondecreasing
//                                                    (a step of 1 in x moves it by 0 or 1)
//   xlo/D ≤ r ≤ xhi/D       the interval, which `r ≤ x` alone does not give
static void vra_div_facts(Vra *V, Octagon *W, int r, int a, int64_t D, int64_t alo, bool hl, int64_t ahi, bool hh) {
    if (D < 1) return;
    oct_add_lb(W, r, 0);
    vra_add_diff_le(V,W, r, a, 0);                        // r ≤ x
    if (hl && alo >= 0) oct_add_lb(W, r, alo / D);
    if (hh && ahi >= 0) oct_add_ub(W, r, ahi / D);
    if (D >= 2) {
        if (hl && alo >= 1) vra_add_diff_le(V,W, r, a, -1);          // r ≤ x − 1
        if (hh && ahi >= 0) vra_add_diff_le(V,W, a, r, ahi - ahi/D); // x − r ≤ max(x − x/D)
    }
}

// ★ The MIDPOINT identity, derived rather than pattern-matched. `mid = lo + (hi−lo)/D` is
// the canonical binary-search index, and no octagon can prove `mid < hi`: that needs
// `q < hi − lo`, a THREE-variable relation, and eliminating `lo` between `mid − lo = q` and
// `hi − lo = d` is outside a domain of two-variable differences.
//
// But the octagon already holds `q − d ≤ c` (from vra_div_facts, with c = −1), and `d`'s
// DEFINITION says `d = B − A`. Substituting one into the other is a single symbolic step:
//     q − (B − A) ≤ c   ⟺   (A + q) − B ≤ c   ⟺   r − B ≤ c
// which IS an octagon fact. So the rule is general — any A, B, divisor and any c the domain
// happens to know — and reads no syntax beyond "this value was defined as a subtraction".
// ★ Matched on VALUE EQUALITY, not on value IDENTITY. `mid = lo + (hi − lo)/2` loads `lo`
// TWICE — once for the addition and once for the subtraction — so the two are different SSA
// values and an identity test never fired. The whole midpoint identity was therefore dead on
// the shape it exists for: the binary search's `mid < hi` was never derived, `lo = mid + 1`
// lost its bound, and the loop's `lo` went unbounded through the widening.
//
// The octagon already KNOWS the two loads are equal (both copy the same cell). Asking it is
// both the fix and the general form — anything the domain proves equal to `a` will do.
static bool vra_same_value(Vra *V, const Octagon *W, int x, int y) {
    if (x == y) return true;
    if (x<0 || y<0 || x>=V->nvar || y>=V->nvar) return false;
    return vra_diff_ub(V,W,x,y) <= 0 && vra_diff_ub(V,W,y,x) <= 0;
}
static void vra_add_via_diff(Vra *V, Octagon *W, int r, int a, int q) {
    for (int d=0; d<V->nvar; d++) {
        IrInstr *dd = V->def[d];
        if (!dd || dd->op != IR_SUB || dd->n_operands < 2) continue;
        if (!dd->operands[0] || !dd->operands[1]) continue;
        if (!vra_same_value(V, W, dd->operands[1]->id, a)) continue;   // d = B − a′ with a′ = a
        int B = dd->operands[0]->id;
        if (B == r || B < 0 || B >= V->nvar) continue;
        int64_t c = vra_diff_ub(V, W, q, d); // q − d ≤ c   ⇒   r − B ≤ c
        if (c < OCT_INF) vra_add_diff_le(V,W, r, B, c);
        int64_t c2 = vra_diff_ub(V, W, d, q); // d − q ≤ c2  ⇒   B − r ≤ c2
        if (c2 < OCT_INF) vra_add_diff_le(V,W, B, r, c2);
    }
}

static void vra_transfer_instr(Vra *V, Octagon *W, IrInstr *ins) {
    int r = ins->result ? ins->result->id : -1;
    switch (ins->op) {
        case IR_CONST:
            if (r>=0){ oct_forget(W,r); oct_add_const(W,r,ins->aux.imm); }
            break;
        case IR_LOAD: {
            if (r<0) break;
            int cell = ins->n_operands ? ins->operands[0]->id : -1;
            IrInstr *d = cell>=0 ? V->def[cell] : NULL;
            if ((d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind!=IRT_ARRAY)
                || vra_is_param_cell(V, cell))
                vra_assign_copy(V, W, r, cell);                // scalar cell → value
            else oct_forget(W, r);                             // array elem / unknown
            break;
        }
        case IR_STORE: {
            if (ins->n_operands<2) break;
            int cell = ins->operands[0]->id;
            IrInstr *d = V->def[cell];
            if ((d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind!=IRT_ARRAY)
                || vra_is_param_cell(V, cell))
                vra_assign_copy(V, W, cell, ins->operands[1]->id);  // value → cell
            break;
        }
        case IR_ADD: case IR_SUB: {
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            bool ac=V->cknown[a], bc=V->cknown[b], isadd=(ins->op==IR_ADD);
            oct_forget(W, r);
            // ★ AN UNSIGNED SUBTRACTION THAT MAY UNDERFLOW HAS NO ℤ RELATION TO STATE.
            // The transfers below record `r = a − b` exactly, which is true over ℤ and FALSE
            // in u64 the moment `a < b`: `hi = mid − 1` with mid = 0 is SIZE_MAX, not −1. The
            // corpus's own soundness lock for this shape (binary_search_underflow_fail) caught
            // it the minute the midpoint identity started firing and something downstream
            // finally depended on the relation. Where `a ≥ b` is provable — `hi − lo` under a
            // live `lo < hi` guard — the relation is exact and is kept; where it is not, the
            // result is unknown except for what its TYPE guarantees.
            if (!isadd && ins->result->type && ins->result->type->kind==IRT_INT
                && !ins->result->type->is_signed) {
                oct_close(W);
                int64_t ba = vra_diff_ub(V, W, b, a);      // b − a ≤ ba
                if (!(ba <= 0)) {                          // a ≥ b NOT provable ⇒ may wrap
                    oct_add_lb(W, r, 0);
                    break;
                }
            }
            if (isadd && bc)      { vra_add_diff_le(V,W,r,a,V->cval[b]); vra_add_diff_le(V,W,a,r,-V->cval[b]); }   // r=a+c (exact)
            else if (isadd && ac) { vra_add_diff_le(V,W,r,b,V->cval[a]); vra_add_diff_le(V,W,b,r,-V->cval[a]); }
            else if (!isadd && bc){ vra_add_diff_le(V,W,r,a,-V->cval[b]); vra_add_diff_le(V,W,a,r,V->cval[b]); }   // r=a-c (exact)
            else if (!isadd && ac){ int64_t c=V->cval[a]; oct_add_sum_le(W,r,b,c); oct_add_negsum_le(W,r,b,-c); } // r=c-b ⇒ r+b=c
            else {
                // two-variable: sound difference bounds from the second operand's interval
                //   r=a+b, b∈[blo,bhi] ⇒ a+blo ≤ r ≤ a+bhi ;  r=a-b ⇒ a-bhi ≤ r ≤ a-blo
                oct_close(W);   // materialize the a↔b (and q↔d) relations before reading them
                int64_t blo,bhi; bool hl,hh; vra_interval(V, W,b,&blo,&hl,&bhi,&hh);
                if (isadd) { if (hh) vra_add_diff_le(V,W,r,a,bhi); if (hl) vra_add_diff_le(V,W,a,r,-blo);
                             vra_add_via_diff(V,W,r,a,b);      // r = a + b where b is bounded vs (B − a)
                             vra_add_via_diff(V,W,r,b,a); }    // ...and symmetrically
                else {
                    if (hl) vra_add_diff_le(V,W,r,a,-blo); if (hh) vra_add_diff_le(V,W,a,r,bhi);
                    // RELATIONAL r = a − b: transfer the octagon's OWN a↔b difference to r
                    // exactly (intervals miss it when the bound is symbolic). This proves the
                    // reverse index `a[L−i−1]`: from `i ≤ L` the octagon already holds, r=L−i
                    // gets r ≥ 0 (no underflow) and r < L.  a−b ≤ ub ⇒ r ≤ ub ; b−a ≤ lbe ⇒ r ≥ −lbe.
                    int64_t ub  = vra_diff_ub(V, W, a, b);   // bound on a − b
                    int64_t lbe = vra_diff_ub(V, W, b, a);   // bound on b − a
                    if (ub  < OCT_INF) oct_add_ub(W, r, ub);
                    if (lbe < OCT_INF) oct_add_lb(W, r, -lbe);
                }
            }
            break;
        }
        case IR_MUL: {   // r = x * c  (one operand constant) — sound interval scaling.
            if (r<0) break;                              // enables the const-stride 2D index
            int a=ins->operands[0]->id, b=ins->operands[1]->id;   // a[i*W + j], W constant
            bool ac=V->cknown[a], bc=V->cknown[b];
            oct_forget(W, r);
            if (ac && bc) { int64_t v; if (vra_safe_scale(V->cval[a],V->cval[b],&v)) oct_add_const(W,r,v); break; }
            // S2: `i * e1` where e1 is a region's innermost EXTENT — the row-major stride.
            // Given 0 ≤ i < e0 and e1 ≥ 0 and len == e0*e1 (true by construction, the shape
            // came from the declared `i32[h*w]`):
            //     i*e1 ≤ (e0−1)*e1 = len − e1 ≤ len
            // so the product is bounded BY THE LENGTH — an octagon difference, even though
            // len == e0*e1 itself is nonlinear and unrepresentable. That is what discharges
            // the intermediate `i*w` overflow: it is below a valid usize.
            for (int side=0; side<2 && r>=0; side++) {
                int stride = side ? a : b, iv = side ? b : a;
                int e0=-1, lenv = vra_shape_len_for_stride(V, stride, &e0);
                if (lenv < 0 || e0 < 0) continue;
                int64_t ilo,ihi,slo,shi; bool ihl,ihh,shl,shh;
                vra_interval(V, W, iv, &ilo,&ihl,&ihi,&ihh);
                vra_interval(V, W, stride, &slo,&shl,&shi,&shh);
                bool i_lt_e0 = vra_diff_ub(V, W, iv, e0) <= -1;
                if (i_lt_e0 && ihl && ilo>=0 && shl && slo>=0) vra_add_diff_le(V,W, r, lenv, 0);
            }
            int xv=-1; int64_t c=0;
            if (bc) { c=V->cval[b]; xv=a; } else if (ac) { c=V->cval[a]; xv=b; }
            if (xv>=0) {
                int64_t xlo,xhi,v; bool hl,hh; vra_interval(V, W, xv, &xlo,&hl,&xhi,&hh);
                if (c==0) oct_add_const(W,r,0);
                else if (c>0) { if (hl && vra_safe_scale(c,xlo,&v)) oct_add_lb(W,r,v);   // monotone
                                if (hh && vra_safe_scale(c,xhi,&v)) oct_add_ub(W,r,v); }
                else          { if (hh && vra_safe_scale(c,xhi,&v)) oct_add_lb(W,r,v);   // c<0: flip
                                if (hl && vra_safe_scale(c,xlo,&v)) oct_add_ub(W,r,v); }
            }
            break;
        }
        case IR_CTZ: case IR_CLZ: case IR_POPCOUNT: {
            // A bit intrinsic lands in [0, W] where W is the OPERAND's width — exactly the
            // fact that makes `a[@popcount(mask)]` provable without a runtime check. Modelled
            // as an op rather than an opaque call precisely so this range is free.
            if (r<0) break;
            oct_forget(W, r);
            const IrType *at = ins->operands[0] ? ins->operands[0]->type : NULL;
            int width = (at && at->kind==IRT_INT && at->bits>0 && at->bits<=64) ? at->bits : 32;
            oct_add_lb(W, r, 0); oct_add_ub(W, r, width);
            break;
        }
        case IR_AND: {   // x & c  with c ≥ 0 constant  ⇒  0 ≤ r ≤ c   (mask idiom c=N−1)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_forget(W, r);
            if      (V->cknown[b] && V->cval[b]>=0){ oct_add_lb(W,r,0); oct_add_ub(W,r,V->cval[b]); }
            else if (V->cknown[a] && V->cval[a]>=0){ oct_add_lb(W,r,0); oct_add_ub(W,r,V->cval[a]); }
            break;
        }
        case IR_UDIV: {  // x / b  — in any defined exec (b > 0, x ≥ 0)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_close(W);                                  // the dividend's interval, relationally
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            oct_forget(W, r);
            vra_div_facts(V, W, r, a, (V->cknown[b] && V->cval[b]>0) ? V->cval[b] : 1, alo,hl,ahi,hh);
            break;
        }
        case IR_SDIV: {  // signed x / c — for x ≥ 0 and c > 0 (the common index idiom
            if (r<0) break;                                 // `i / 2`) it is exactly udiv.
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_close(W);
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            oct_forget(W, r);
            if (hl && alo>=0 && V->cknown[b] && V->cval[b]>0)
                vra_div_facts(V, W, r, a, V->cval[b], alo,hl,ahi,hh);
            break;
        }
        case IR_UREM: {  // x % b  (unsigned)  ⇒  0 ≤ r < b   (b > 0 in any defined exec;
            if (r<0) break;                                 // b = 0 is a separate div-by-zero)
            int b=ins->operands[1]->id; oct_forget(W, r);
            oct_add_lb(W, r, 0);
            if (V->cknown[b] && V->cval[b]>0) oct_add_ub(W, r, V->cval[b]-1);  // absolute ≤ c−1
            else vra_add_diff_le(V,W, r, b, -1);                                 // relative r < b
            break;
        }
        case IR_SREM: {  // signed a % c  ⇒  −(c−1) ≤ r ≤ c−1 (tighter to [0,c−1] if a≥0)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id; oct_forget(W, r);
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            if (V->cknown[b] && V->cval[b]>0){
                int64_t c=V->cval[b];
                oct_add_lb(W,r, (hl&&alo>=0)?0:-(c-1)); oct_add_ub(W,r,c-1);
            } else if (hl && alo>=0) {                     // non-const divisor, a ≥ 0, b > 0 in
                oct_add_lb(W,r,0); vra_add_diff_le(V,W,r,b,-1);   // any defined exec ⇒ 0 ≤ r < b
            }
            break;
        }
        case IR_LSHR: {  // x >> k  (logical) of a non-negative x is in [0, x]
            if (r<0) break;
            int a=ins->operands[0]->id; oct_forget(W,r);
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            if (hl&&alo>=0){ oct_add_lb(W,r,0); if(hh) oct_add_ub(W,r,ahi); }  // 0 ≤ r ≤ a
            break;
        }
        case IR_SLICE_LEN: {
            if (r<0) break;
            oct_forget(W, r); oct_add_lb(W, r, 0);                 // a length is ≥ 0
            int s = ins->operands[0]->id, canon = V->slicelen[s];
            if (canon>=0 && canon!=r) vra_assign_copy(V, W, r, canon); // all len reads agree
            break;
        }
        case IR_SUM_TAG: {
            // A discriminant is an ORDINARY INTEGER in [0, variants−1] — ir.h says so as the
            // reason discrimination is exact — and the domain was forgetting it. Stating it
            // is what lets a tag comparison refine like any other guard.
            if (r<0) break;
            oct_forget(W, r);
            IrType *st = ins->n_operands>=1 && ins->operands[0] ? ins->operands[0]->type : NULL;
            if (st && st->kind==IRT_SUM && st->n_fields > 0) {
                oct_add_lb(W, r, 0); oct_add_ub(W, r, st->n_fields - 1);
            }
            break;
        }
        case IR_VEC_MOVEMASK: {
            // One bit per lane: the result is in [0, 2^lanes − 1], exactly. Without it the
            // `@ctz(@movemask(v))` idiom — the whole point of a SIMD scan — indexed with a
            // value the domain knew nothing about.
            if (r<0) break;
            oct_forget(W, r);
            IrType *vt = ins->n_operands>=1 && ins->operands[0] ? ins->operands[0]->type : NULL;
            int64_t lanes = (vt && vt->kind==IRT_VECTOR) ? vt->array_len : 0;
            if (lanes > 0 && lanes < 63) { oct_add_lb(W, r, 0); oct_add_ub(W, r, (1LL<<lanes) - 1); }
            break;
        }
        case IR_SEQ_EQ: case IR_ICMP: {
            if (r<0) break;                       // a BOOL is 0 or 1, and nothing said so
            oct_forget(W, r); oct_add_lb(W, r, 0); oct_add_ub(W, r, 1);
            break;
        }
        case IR_NEG: {
            // `r = −x` — EXACT, and it is the octagon's own shape (`r + x ≤ 0` with
            // `−r − x ≤ 0`), not an interval approximation. There was no case at all, so a
            // negated value was FORGOTTEN: `var y i16 = -1000` could not be shown to fit i16,
            // because the domain had lost the 1000 the moment it was negated.
            if (r<0) break;
            oct_forget(W, r);
            if (ins->n_operands < 1) break;
            int x = ins->operands[0]->id;
            if (x>=0 && x<V->nvar && V->cknown[x]) { oct_add_const(W, r, -V->cval[x]); break; }
            oct_add_sum_le(W, r, x, 0);
            oct_add_negsum_le(W, r, x, 0);
            break;
        }
        case IR_CAST:
            if (r>=0){ // treat as a copy (widenings preserve value; a narrowing that
                       // changes it would be a separate proven-safe obligation)
                if (vra_is_int(ins->result) && ins->n_operands) vra_assign_copy(V, W, r, ins->operands[0]->id);
                else oct_forget(W, r);
            }
            break;
        case IR_ASSUME:   // the asserted fact holds from here — refine the octagon
            if (ins->n_operands>=1) vra_refine_guard(V, W, ins->operands[0], true);
            break;
        case IR_OPAQUE:
            // B3: an unmodelled construct. Its RESULT is unknown, and if its declared
            // footprint includes a write it may have changed any escaped cell — the same
            // havoc a call needs, for the same reason. Handling it here is what lets the
            // REST of the function stay analysed instead of the whole function being
            // written off as `incomplete`.
            if (ins->aux.opaque.writes)
                for (int cell=0; cell<V->nvar; cell++) if (V->escaped[cell]) oct_forget(W, cell);
            if (r>=0) oct_forget(W, r);
            break;
        case IR_CALL: {
            // ★ THE ALIAS ORACLE. A call may write through any address it was GIVEN, and the
            // octagon's memory cells are exactly the scalar allocas — so the safe answer is to
            // forget every escaped cell, and that is what this did. But it is far too much:
            //
            //     bump(var i)          // i escapes, and is now unknown — fair enough
            //     if i < 16 {
            //         bump(var j)      // touches j ONLY...
            //         a[i]             // ...yet `i < 16` was thrown away here. Not proven.
            //
            // The call cannot reach `i` at all: it was handed `&j`, and `i`'s address was
            // never stored, returned, or given to anything that keeps it. So havoc exactly
            //   (a) every cell that PERSISTS — its address outlived some earlier call, so a
            //       stash-holder may write it now, and
            //   (b) the cells this call was handed in positions the callee actually WRITES.
            // Anything the callee is invisible about (extern, opaque, an unattributable
            // address) falls back to the blanket havoc. Ownership pays for precision here:
            // the same facts that make the emitted `restrict` legal make this legal.
            {
                IrFunc *cal = vra_find_func(ins->aux.callee);
                IrWriteFootprint cw = (cal && vra_mod) ? ir_param_writes(cal, vra_mod)
                                                       : ~(IrWriteFootprint)0;
                bool blanket = (cw == ~(IrWriteFootprint)0);
                if (!blanket)
                    for (int k=0; k<ins->n_operands && !blanket; k++)
                        if (k>=64 || ((cw>>k)&1u))
                            if (vra_arg_cell(V, ins->operands[k]) == VRA_ARG_UNKNOWN) blanket = true;
                if (blanket) {
                    for (int cell=0; cell<V->nvar; cell++) if (V->escaped[cell]) oct_forget(W, cell);
                } else {
                    for (int cell=0; cell<V->nvar; cell++) if (V->persist[cell]) oct_forget(W, cell);
                    for (int k=0; k<ins->n_operands; k++) {
                        if (k<64 && !((cw>>k)&1u)) continue;
                        int c = vra_arg_cell(V, ins->operands[k]);
                        if (c>=0) oct_forget(W, c);
                    }
                }
            }
            if (r>=0) {
                oct_forget(W, r);
                // ...but the RESULT is not unknown: the callee's body bounds it.
                int64_t rlo, rhi;
                if (ins->result && ins->result->type
                    && vra_ret_range(vra_find_func(ins->aux.callee), &rlo, &rhi)) {
                    if (rlo > -OCT_INF/2) oct_add_lb(W, r, rlo);
                    if (rhi <  OCT_INF/2) oct_add_ub(W, r, rhi);
                }
            }
            break;
        }
        default:
            if (r>=0) oct_forget(W, r);   // conservative: result becomes unknown
            break;
    }
}

// refine W along a branch edge from `br` taken in direction `then_dir`
static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir) {
    if (!cond) return;
    IrInstr *ic = V->def[cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return;
    int a=ic->operands[0]->id, b=ic->operands[1]->id;
    IrCmp p = ic->aux.cmp;
    // normalize: on the else edge, the predicate negates
    // LT: a<b  LE: a≤b  GT: a>b  GE: a≥b  EQ  NE
    bool lt=(p==IR_CMP_SLT||p==IR_CMP_ULT), le=(p==IR_CMP_SLE||p==IR_CMP_ULE);
    bool gt=(p==IR_CMP_SGT||p==IR_CMP_UGT), ge=(p==IR_CMP_SGE||p==IR_CMP_UGE);
    bool eq=(p==IR_CMP_EQ), ne=(p==IR_CMP_NE);
    bool uns=(p==IR_CMP_ULT||p==IR_CMP_ULE||p==IR_CMP_UGT||p==IR_CMP_UGE);
    if (!then_dir) { // negate
        bool nl=ge, nle=gt, ng=le, nge=lt, neq=ne, nne=eq;
        lt=nl; le=nle; gt=ng; ge=nge; eq=neq; ne=nne;
    }
    // ★ AN UNSIGNED COMPARISON PROVES NON-NEGATIVITY. `(unsigned)i < n` is the C idiom for
    // "i is a valid index" precisely because a negative i becomes enormous and fails — and it
    // is what `i in arr` lowers to. The refinement read it as an ordinary `<` and got only the
    // upper half, so `func get(arr i32[10], i i32 in arr) { arr[i] }` stayed unproven on the
    // lower one: the annotation's whole meaning was being half-believed.
    //
    // Sound within this domain's model: every value is carried as an int64, and an unsigned
    // type's value is assumed to fit (that is what seeding a u64 with `lb 0` and no upper
    // bound already asserts). So on the true edge of `a <u b`, a negative `a` would compare
    // as ≥ 2^63 and could not be below a `b` that fits — hence a ≥ 0. The bound side must
    // itself be trustworthy, so require its type to be unsigned or its upper bound known and
    // well inside the range.
    if (uns) {
        int small = (lt||le) ? a : (gt||ge) ? b : -1;
        int bound = (lt||le) ? b : (gt||ge) ? a : -1;
        if (small>=0 && bound>=0) {
            IrValue *bv = (bound<V->nvar) ? V->val[bound] : NULL;
            bool trustworthy = bv && bv->type && bv->type->kind==IRT_INT && !bv->type->is_signed;
            if (!trustworthy) {
                int64_t blo,bhi; bool hl,hh;
                vra_interval(V, W, bound, &blo,&hl,&bhi,&hh);
                trustworthy = hh && bhi >= 0 && bhi < ((int64_t)1<<62);
            }
            if (trustworthy) oct_add_lb(W, small, 0);
        }
    }
    // When one side is a CONSTANT, state an ABSOLUTE bound rather than a difference against
    // its dimension — a constant has none (it lives in the constant table), so the relational
    // form would silently drop the guard entirely and `i < 100` would constrain nothing.
    // The absolute form is also the stronger fact: it needs no closure step to be usable.
    bool ac = (a>=0 && a<V->nvar && V->cknown[a]), bc = (b>=0 && b<V->nvar && V->cknown[b]);
    if (bc && !ac) { int64_t c=V->cval[b];
        if (lt) oct_add_ub(W,a,c-1); else if (le) oct_add_ub(W,a,c);
        else if (gt) oct_add_lb(W,a,c+1); else if (ge) oct_add_lb(W,a,c);
        else if (eq) oct_add_const(W,a,c);
    } else if (ac && !bc) { int64_t c=V->cval[a];
        if (lt) oct_add_lb(W,b,c+1); else if (le) oct_add_lb(W,b,c);
        else if (gt) oct_add_ub(W,b,c-1); else if (ge) oct_add_ub(W,b,c);
        else if (eq) oct_add_const(W,b,c);
    }
    else if (lt) vra_add_diff_le(V,W,a,b,-1);   // a − b ≤ −1
    else if (le) vra_add_diff_le(V,W,a,b,0);    // a − b ≤ 0
    else if (gt) vra_add_diff_le(V,W,b,a,-1);   // b − a ≤ −1
    else if (ge) vra_add_diff_le(V,W,b,a,0);    // b − a ≤ 0
    else if (eq){ vra_add_diff_le(V,W,a,b,0); vra_add_diff_le(V,W,b,a,0); }
    // ★ `a ≠ c` IS representable when it cuts an ENDPOINT. A hole in the middle of an interval
    // is not an interval, but `n ≠ 0` on a value already known to be ≥ 0 is exactly `n ≥ 1` —
    // and that is the false edge of `if n == 0 { return }`, the guard every length-1
    // subtraction in the corpus is written behind. Without it `n − 1` could underflow for all
    // the domain knew, and the whole post-loop-negation family lost its bound.
    if (ne) {
        int x = -1; int64_t cv = 0;
        if (bc && !ac)      { x = a; cv = V->cval[b]; }
        else if (ac && !bc) { x = b; cv = V->cval[a]; }
        if (x >= 0) {
            int64_t lo,hi; bool hl,hh;
            vra_interval(V, W, x, &lo,&hl,&hi,&hh);
            if (hl && lo == cv) oct_add_lb(W, x, cv + 1);        // the low end was the hole
            if (hh && hi == cv) oct_add_ub(W, x, cv - 1);        // ...or the high end
        }
    }
}

// ── bounds consumer: discharge 0 ≤ idx < len at an IR_ELEM_PTR ───────────────
// Walk an operand tree to a bounded depth, counting distinct SYMBOLIC leaves and noting
// whether a call result or a loop-carried load appears. Bounded because an SSA chain can be
// long and this runs per failed obligation, not per instruction.
static void vra_loss_walk(Vra *V, IrValue *v, int depth,
                          int *nsym, bool *saw_call, bool *all_param, int *sym_ids, int cap) {
    if (!v || depth > 4 || v->id < 0 || v->id >= V->nvar) return;
    IrInstr *d = V->def[v->id];
    if (!d) {                                    // a parameter: a symbolic leaf
        for (int i=0;i<*nsym;i++) if (sym_ids[i]==v->id) return;
        if (*nsym < cap) sym_ids[(*nsym)++] = v->id;
        return;
    }
    if (d->op == IR_CALL) { *saw_call = true; return; }
    if (V->cknown[v->id]) return;                // a constant is not a symbolic leaf
    if (d->op == IR_LOAD || d->op == IR_SLICE_LEN || d->op == IR_ALLOCA) {
        // Whether a LOAD counts as "an unrefined parameter" depends on where its ADDRESS
        // roots. `p.x` with `p` a parameter is a parameter value and really is unbounded;
        // a load from a local alloca is not. Following the field_ptr/elem_ptr chain is the
        // difference between "the engine is right to refuse" and "the engine lost a fact",
        // and getting it wrong put every `p.x = p.x + 1` into the unclassified bucket.
        if (d->op == IR_LOAD) {
            IrValue *addr = d->n_operands >= 1 ? d->operands[0] : NULL;
            for (int hop = 0; hop < 6 && addr && addr->id >= 0 && addr->id < V->nvar; hop++) {
                IrInstr *ad = V->def[addr->id];
                if (!ad) break;                                   // rooted at a parameter
                if (ad->op == IR_ALLOCA) { *all_param = false; break; }   // a local
                if (ad->n_operands < 1) { *all_param = false; break; }
                addr = ad->operands[0];                           // field_ptr / elem_ptr / cast
            }
        }
        for (int i=0;i<*nsym;i++) if (sym_ids[i]==v->id) return;
        if (*nsym < cap) sym_ids[(*nsym)++] = v->id;
        return;
    }
    for (int k=0;k<d->n_operands;k++)
        vra_loss_walk(V, d->operands[k], depth+1, nsym, saw_call, all_param, sym_ids, cap);
}

// Is this instruction's result stored back into a slot it also READ? That is the shape of a
// loop-carried update, `s = s <op> x`, and the step tells the two apart: a CONSTANT step is
// something the domain should reach (so failing is a widening loss), a symbolic one needs a
// trip-count times delta bound, which is a product.
static bool vra_loss_selfupdate(Vra *V, IrInstr *ins, bool *const_step, int *slot_bits) {
    if (!ins || !ins->result || ins->result->id < 0) return false;
    int blk = V->defblk[ins->result->id];
    if (blk < 0) return false;
    IrValue *slot = NULL;
    for (IrBlock *b = V->f->blocks; b; b = b->next) {
        if (b->id != blk) continue;
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (i->op == IR_STORE && i->n_operands >= 2 && i->operands[1] == ins->result)
                slot = i->operands[0];
        break;
    }
    if (!slot) return false;
    // width of the slot being stored back into: an alloca's type is *var T, so take the elem
    *slot_bits = 64;
    if (slot->type && slot->type->elem && slot->type->elem->kind == IRT_INT)
        *slot_bits = slot->type->elem->bits;
    bool found = false; *const_step = true;
    for (int k=0;k<ins->n_operands;k++) {
        IrValue *o = ins->operands[k];
        if (!o || o->id < 0 || o->id >= V->nvar) continue;
        IrInstr *d = V->def[o->id];
        if (d && d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0] == slot) { found = true; continue; }
        if (!V->cknown[o->id]) *const_step = false;      // the OTHER operand is symbolic
    }
    return found;
}

static VraLoss vra_classify_loss(Vra *V, IrInstr *ins) {
    if (!ins) return VLOSS_OTHER;
    // Path-F puts the overflow obligation on the NARROWING, so the failing instruction is the
    // STORE and not the arithmetic. Look through it, or every accumulator classifies as
    // whatever its operand tree happens to look like — which is how `s = s + i` came out as
    // "arity" on the first run of this survey.
    if (ins->op == IR_STORE && ins->n_operands >= 2 && ins->operands[1] &&
        ins->operands[1]->id >= 0 && ins->operands[1]->id < V->nvar) {
        IrInstr *src = V->def[ins->operands[1]->id];
        if (src) ins = src;
    }
    bool const_step = true; int slot_bits = 64;
    if (vra_loss_selfupdate(V, ins, &const_step, &slot_bits)) {
        if (!const_step) return VLOSS_PRODUCT;
        return slot_bits < 64 ? VLOSS_NARROW : VLOSS_WIDEN;
    }

    int sym_ids[16]; int nsym = 0; bool saw_call = false, all_param = true;
    for (int k=0;k<ins->n_operands;k++)
        vra_loss_walk(V, ins->operands[k], 0, &nsym, &saw_call, &all_param, sym_ids, 16);
    if (saw_call) return VLOSS_CALL;
    if (nsym >= 3)  return VLOSS_ARITY;
    // Every operand is an unrefined parameter: the operation really can overflow, and saying
    // so is the language working. Not a precision loss.
    if (all_param && nsym >= 1) return VLOSS_UNBOUNDED;

    // Defined at a merge point: more than one block branches here, so whatever held on each
    // path separately was hulled on the way in.
    if (ins->result && ins->result->id >= 0 && ins->result->id < V->nvar) {
        int blk = V->defblk[ins->result->id];
        int preds = 0;
        for (IrBlock *b = V->f->blocks; b && preds < 2; b = b->next) {
            if (b->term.kind == IR_TERM_BR) {
                if (b->term.a && b->term.a->id == blk) preds++;
            } else if (b->term.kind == IR_TERM_BR_COND) {
                if (b->term.a && b->term.a->id == blk) preds++;
                if (b->term.b && b->term.b->id == blk) preds++;
            }
        }
        if (preds >= 2) return VLOSS_JOIN;
    }
    return VLOSS_OTHER;
}

static void vra_add_check(Vra *V, VraCheck c) {
    if (!c.ok && c.loss == VLOSS_NONE) {
        // A bounds obligation with NO length in scope is not a domain weakness: nothing
        // available says how long the array is.
        c.loss = (c.kind == VRA_BOUNDS && !c.has_len) ? VLOSS_NOLEN
                                                      : vra_classify_loss(V, c.at);
    }
    if (V->nchecks==V->cap_checks){ V->cap_checks=V->cap_checks?V->cap_checks*2:8;
        V->checks=realloc(V->checks, V->cap_checks*sizeof(VraCheck)); }
    V->checks[V->nchecks++]=c;
}
// S2: the region whose innermost extent is `e1` and whose length variable is known.
// Returns the length var, or -1. (Rank-2 only for now, matching vra_factor_shape.)
static int vra_shape_len_for_stride(Vra *V, int stride_id, int *e0_out) {
    for (int b=0;b<V->nvar;b++) {
        if (V->shape_rank[b] != 2) continue;
        if (V->shape_ext[b][1] != stride_id) continue;
        if (V->slicelen[b] < 0) continue;
        if (e0_out) *e0_out = V->shape_ext[b][0];
        return V->slicelen[b];
    }
    return -1;
}

// S2: can the flat index `idx` be FACTORED against the shape of `sbase`?
//
// For a rank-2 region with extents (e0, e1) the row-major strides are (e1, 1), so the
// coordinate access (i, j) is the flat index `i*e1 + j`. If the index matches that form
// structurally and the octagon knows `i < e0` and `j < e1`, the access is in bounds:
//
//     idx = i*e1 + j  ≤  (e0−1)*e1 + (e1−1)  =  e0*e1 − 1  <  e0*e1  =  len
//
// which is sound because the region's LENGTH is e0*e1 by construction — the shape was
// emitted from the declared type `i32[h*w]`, whose call sites are checked separately.
//
// This is the whole point of the memory model. The flat obligation `i*e1 + j < e0*e1` is
// NONLINEAR (two runtime values multiplied) and no octagon can express it; factored, both
// halves are facts the loop guards already established.
static bool vra_factor_shape(Vra *V, Octagon *W, int sbase, int idx) {
    if (sbase<0 || sbase>=V->nvar || V->shape_rank[sbase] != 2) return false;   // rank-2 for now
    IrInstr *d = (idx>=0 && idx<V->nvar) ? V->def[idx] : NULL;
    if (!d || d->op != IR_ADD || d->n_operands < 2) return false;
    int e0 = V->shape_ext[sbase][0], e1 = V->shape_ext[sbase][1];
    // match ADD(MUL(i, e1), j)  — and the commuted forms
    for (int side=0; side<2; side++) {
        IrValue *mulv = d->operands[side], *jv = d->operands[1-side];
        if (!mulv || !jv) continue;
        IrInstr *m = V->def[mulv->id];
        if (!m || m->op != IR_MUL || m->n_operands < 2) continue;
        int i_id = -1;
        if      (m->operands[1]->id == e1) i_id = m->operands[0]->id;
        else if (m->operands[0]->id == e1) i_id = m->operands[1]->id;
        if (i_id < 0) continue;
        int j_id = jv->id;
        // i < e0  and  j < e1, both as octagon differences (x − y ≤ −1)
        bool i_ok = vra_diff_ub(V, W, i_id, e0) <= -1;
        bool j_ok = vra_diff_ub(V, W, j_id, e1) <= -1;
        // and both non-negative (usize gives this, but check the octagon too)
        int64_t ilo,ihi,jlo,jhi; bool ihl,ihh,jhl,jhh;
        vra_interval(V, W, i_id, &ilo,&ihl,&ihi,&ihh);
        vra_interval(V, W, j_id, &jlo,&jhl,&jhi,&jhh);
        bool nonneg = (ihl && ilo>=0) && (jhl && jlo>=0);
        if (i_ok && j_ok && nonneg) return true;
    }
    return false;
}

static void vra_check_elem(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->n_operands<2) return;
    if (ins->result && V->subslice_gep[ins->result->id]) return;  // a subslice start — the make_slice checks it
    int idx = ins->operands[1]->id;
    IrValue *base = ins->operands[0];
    IrInstr *bd = V->def[base->id];
    int64_t clen=-1; int lenvar=-1;
    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty && bd->aux.alloca_ty->kind==IRT_ARRAY)
        clen = bd->aux.alloca_ty->array_len;                          // local fixed array
    else if (base->type && base->type->kind==IRT_ARRAY)
        clen = base->type->array_len;                                // fixed-array value (e.g. a param)
    int shape_base = -1;
    if (bd && bd->op==IR_SLICE_DATA && bd->n_operands>=1) {
        int s = bd->operands[0]->id; if (V->slicelen[s]>=0) lenvar=V->slicelen[s];
        shape_base = s;                                    // the slice value carries the shape
    }
    int64_t lo,hi; bool hl,hh;
    if (V->cknown[idx]) { lo=hi=V->cval[idx]; hl=hh=true; }   // constant index — no octagon needed
    else {
        // The index's TYPE bounds it too, and reading only the octagon threw that away: a
        // `usize` loaded out of a struct field has no octagon history at all, so `0 ≤ idx` —
        // the easy half of the obligation — could not be discharged even when the hard half
        // could.
        //
        // ★ But ONLY for a value that cannot have wrapped. The octagon models arithmetic in ℤ,
        // so `i - 1` at i = 0 is −1 there, while the unsigned TYPE says ≥ 0 — and that is the
        // underflow, not a fact about it. Intersecting the two would assert the bug away and
        // prove `a[i-1]` under an unguarded `i < n`, which is a removed bounds check. A load,
        // a parameter or a call result holds a value that really does satisfy its type; an
        // arithmetic result does not until its own overflow obligation is discharged.
        IrInstr *idef = V->def[idx];
        bool may_wrap = idef && (idef->op==IR_ADD || idef->op==IR_SUB || idef->op==IR_MUL
                             || idef->op==IR_SHL || idef->op==IR_NEG);
        if (may_wrap) vra_interval(V, W, idx, &lo,&hl,&hi,&hh);
        else { vra_range(V, W, ins->operands[1], &lo, &hi);
               hl = (lo > INT64_MIN); hh = (hi < INT64_MAX); }
    }
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_BOUNDS; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.lo_ok = hl && lo>=0;
    c.has_len = (clen>=0 || lenvar>=0);
    // A WIDE access spans `w` elements from idx, so the obligation is `idx + w <= len` —
    // `idx < len` would pass a 32-lane load starting one element from the end.
    int64_t w = ins->aux.elem_width > 0 ? ins->aux.elem_width : 1;
    if (clen>=0)        c.hi_ok = hh && hi <= clen - w;
    else if (lenvar>=0) c.hi_ok = vra_diff_ub(V, W, idx, lenvar) <= -w;  // idx − len ≤ −w
    else                c.hi_ok = false;
    // S2: if the flat check failed, try FACTORING the index against the region's shape.
    if (!c.hi_ok && shape_base>=0 && vra_factor_shape(V, W, shape_base, idx)) {
        c.hi_ok = true; c.lo_ok = true; c.has_len = true;   // both halves come from the factors
    }
    c.ok = c.lo_ok && c.hi_ok;
    vra_add_check(V, c);
}

// The ℤ range of a value = its type interval, tightened by the octagon.
static void vra_range(Vra *V, Octagon *W, IrValue *v, int64_t *lo, int64_t *hi) {
    // A CONSTANT's value is exact in the constant table, so it needs no octagon dimension at
    // all — and it is the single biggest consumer of them: a 128-element array literal is 128
    // constants, every one of them a fully constrained (hence "active") dimension driving the
    // cubic closure. Reading it here instead is both faster and more precise than an interval.
    if (v && v->id>=0 && v->id<V->nvar && V->cknown[v->id]) { *lo=*hi=V->cval[v->id]; return; }
    int64_t tlo=INT64_MIN, thi=INT64_MAX; (void)V;
    irtype_int_range(v->type, &tlo, &thi);
    // ★ A WIDENING CAST CARRIES ITS SOURCE'S TYPE BOUND. `(x as i64)` on an i32 holds the same
    // number, so it is in i32's range — but the value's own type is i64 and that is all the
    // interval says. The bound lives one step back along the cast chain, and following it is
    // always sound because sext/zext are value-preserving. (A TRUNC is not, so it stops here.)
    //
    // Two things were failing for want of this. The saturating `+|` expands to a widened add
    // whose operands are casts of the destination type; inside a loop the octagon has widened
    // those away, so `i64 + i64` could not be shown to fit i64 and a TOTAL operation was
    // rejected (D-22). And B1's per-iteration delta read `(a[i] as i32)` on a u8 element as
    // [-2^31, 255], which bounds no sum at all.
    for (IrValue *src = v; src && src->id>=0 && src->id<V->nvar; ) {
        IrInstr *d = V->def[src->id];
        if (!d || d->op != IR_CAST || d->n_operands < 1) break;
        // Decided from the TYPES, not from aux.cast_kind: the lowering labels a u8 -> i32
        // widening IR_CAST_BITCAST, so trusting the label follows nothing. A cast preserves
        // the value when it widens AND the source cannot become negative under it — an
        // unsigned source always survives, and a same-signedness widening survives. i8 -> u32
        // does NOT: zext turns -1 into 4294967295, so intersecting with [-128,127] would be
        // unsound. That case stops here.
        IrType *st = d->operands[0] ? d->operands[0]->type : NULL;
        IrType *dt = d->result ? d->result->type : NULL;
        if (!st || !dt || st->kind != IRT_INT || dt->kind != IRT_INT) break;
        if (st->bits > dt->bits) break;                       // a narrowing changes the value
        if (st->is_signed && !dt->is_signed) break;           // signed -> unsigned may not
        src = d->operands[0];
        int64_t slo, shi;
        if (!irtype_int_range(st, &slo, &shi)) break;
        if (slo > tlo) tlo = slo;
        if (shi < thi) thi = shi;
    }
    int64_t olo,ohi; bool hl,hh; vra_interval(V, W, v->id, &olo,&hl,&ohi,&hh);
    if (hl && olo>tlo) tlo=olo;
    if (hh && ohi<thi) thi=ohi;
    // ★ RELATIONAL refinement. A `usize` upper bound is INT64_MAX, which the entry seeding
    // skips (doubling it would overflow the DBM), so `while i < n` leaves `i` with no absolute
    // bound and `i = i + 1` — the commonest statement in the corpus — cannot be shown not to
    // overflow. But the octagon HOLDS `i − n ≤ −1`, and n is bounded by its own TYPE: together
    // those give `i ≤ typemax(n) − 1`, hence `i + 1 ≤ typemax(n)`. The bound is symbolic in
    // the partner rather than absolute, which is exactly what a relational domain is for.
    if (!hh && v->id>=0 && v->id<V->nvar) {
        for (int y=0; y<V->nvar; y++) {
            if (y==v->id || !V->val[y] || !V->val[y]->type) continue;
            int64_t c = oct_get(W, oct_pos(y), oct_pos(v->id));   // v − y ≤ c
            if (c >= OCT_INF) continue;
            int64_t ylo, yhi;
            if (!irtype_int_range(V->val[y]->type, &ylo, &yhi)) continue;
            if (c > 0 && yhi > INT64_MAX - c) continue;           // no wrap in the checker
            int64_t cand = yhi + c;
            if (cand < thi) thi = cand;
        }
    }
    *lo=tlo; *hi=thi;
}
// 128-bit range combine so i64/usize arithmetic can't wrap the checker itself.
static void vra_arith_range(IrOp op, int64_t alo,int64_t ahi, int64_t blo,int64_t bhi,
                            __int128 *rlo, __int128 *rhi) {
    __int128 al=alo,ah=ahi,bl=blo,bh=bhi;
    if (op==IR_ADD){ *rlo=al+bl; *rhi=ah+bh; }
    else if (op==IR_SUB){ *rlo=al-bh; *rhi=ah-bl; }
    else { // MUL: min/max over the four corners
        __int128 c1=al*bl,c2=al*bh,c3=ah*bl,c4=ah*bh;
        __int128 lo=c1,hi=c1;
        if(c2<lo)lo=c2; if(c3<lo)lo=c3; if(c4<lo)lo=c4;
        if(c2>hi)hi=c2; if(c3>hi)hi=c3; if(c4>hi)hi=c4;
        *rlo=lo; *rhi=hi;
    }
}
// ★ THE NARROWING OBLIGATION — the other half of Path-F. `+` WIDENS (i32 + i32 : i33), so
// the addition itself cannot overflow and the obligation moves HERE: to the point where the
// widened value meets a narrower slot. `var s i32 = a + b` on two unconstrained i32 is
// exactly that, and it must be refused.
//
// Confined to a value that is genuinely WIDER than its target, which is what keeps it free of
// noise (a same-type store fits by construction) and away from the 64-bit edges where a type
// interval no longer fits in the domain's own int64.
static bool vra_accum_info(Vra *V, Octagon *W, IrValue *val, VraCheck *c,
                           int64_t tlo, int64_t thi); // B1, defined below

static void vra_check_narrow(Vra *V, Octagon *W, IrValue *val, IrType *target,
                             IrInstr *at, int64_t line, int64_t col) {
    if (at && at->unchecked) return;                        // inside `unsafe`
    if (!val || !val->type || !target) return;
    if (val->type->kind != IRT_INT || target->kind != IRT_INT) return;
    if (val->type->bits <= target->bits) return;            // not a narrowing
    int64_t tlo, thi, vlo, vhi;
    if (!irtype_int_range(target, &tlo, &thi)) return;
    vra_range(V, W, val, &vlo, &vhi);
    VraCheck c; memset(&c,0,sizeof c);
    c.kind = VRA_OVERFLOW; c.at = at; c.line = line; c.col = col;
    c.ok = (vlo >= tlo) && (vhi <= thi);
    // B1: the domain cannot bound a running total, because the bound is a PRODUCT of the trip
    // count and the step. Derive it outside the domain and hand back the interval.
    if (!c.ok) c.ok = vra_accum_info(V, W, val, &c, tlo, thi);
    vra_add_check(V, c);
}

// Overflow obligation: a CHECK-mode +,−,× on two same-typed integers must land
// back inside that type. The octagon reasons in ℤ; here we compare the ℤ result
// range against the operand type's interval (design §2.6).
static void vra_check_overflow(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->wrap != IR_WRAP_CHECK) return;                 // .wrap/.sat skip the obligation
    if (ins->unchecked) return;                             // ...and so does `unsafe`
    if (ins->n_operands<2) return;
    IrValue *a=ins->operands[0], *b=ins->operands[1];
    int64_t tlo,thi;
    // ★ THE TARGET IS THE RESULT TYPE, not the operand type. Lain's `+` is Path-F: it WIDENS,
    // so `a + b` on two i32 has result type i33 and CANNOT overflow — the obligation moves to
    // the NARROWING, where the widened value meets a narrower slot (see vra_check_narrow).
    // Checking the operand type instead demanded that an i32+i32 fit in i32, which refused
    // `func pure_add(a i32, b i32) i64 { return a + b }` — a function that provably cannot
    // overflow, and by far the largest single cause of the numeric engine's false positives.
    // Nothing is lost at 64 bits: there is no u65, so a u64 add's result type IS u64 and the
    // obligation stays exactly where it was.
    IrType *tt = (ins->result && ins->result->type) ? ins->result->type : a->type;
    if (!irtype_int_range(tt, &tlo, &thi)) return;
    int64_t alo,ahi,blo,bhi; vra_range(V,W,a,&alo,&ahi); vra_range(V,W,b,&blo,&bhi);
    __int128 rlo,rhi; vra_arith_range(ins->op, alo,ahi, blo,bhi, &rlo,&rhi);
    // for SUB, refine with the octagon's OWN a−b relation (W is closed here) — this proves
    // `L − i ≥ 0` (no underflow) from `i ≤ L`, which operand intervals miss when symbolic.
    if (ins->op==IR_SUB) {
        int64_t abu = vra_diff_ub(V, W, a->id, b->id);   // a − b ≤ abu
        int64_t bau = vra_diff_ub(V, W, b->id, a->id);   // b − a ≤ bau ⇒ a − b ≥ −bau
        if (abu < OCT_INF && (__int128)abu < rhi) rhi = abu;
        if (bau < OCT_INF && -(__int128)bau > rlo) rlo = -(__int128)bau;
    }
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_OVERFLOW; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = (rlo >= (__int128)tlo) && (rhi <= (__int128)thi);
    // S2: the intermediate arithmetic of a shaped access cannot overflow, because the region
    // LENGTH bounds it and the length is itself a valid value of the index type:
    //     i*e1     ≤ (e0−1)*e1 = len − e1 ≤ len          (given 0 ≤ i < e0, e1 ≥ 0)
    //     i*e1 + j ≤ len − 1                              (given also 0 ≤ j < e1)
    // Operand INTERVALS cannot see this — i and e1 are both unbounded runtime values, so
    // their product's interval is the whole type — which is why `i*w` was reported as a
    // possible overflow even though it indexes a region that provably contains it.
    if (!c.ok && (ins->op==IR_MUL || ins->op==IR_ADD)) {
        if (ins->op==IR_MUL) {
            for (int side=0; side<2 && !c.ok; side++) {
                int stride = side ? a->id : b->id, iv = side ? b->id : a->id;
                int e0=-1, lenv = vra_shape_len_for_stride(V, stride, &e0);
                if (lenv < 0 || e0 < 0) continue;
                int64_t ilo2,ihi2,slo2,shi2; bool ihl2,ihh2,shl2,shh2;
                vra_interval(V, W, iv, &ilo2,&ihl2,&ihi2,&ihh2);
                vra_interval(V, W, stride, &slo2,&shl2,&shi2,&shh2);
                if (vra_diff_ub(V, W, iv, e0) <= -1 && ihl2 && ilo2>=0
                    && shl2 && slo2>=0) c.ok = true;      // ≤ len, a valid value of the type
            }
        } else if (V->def[ins->result ? ins->result->id : 0]) {
            // ADD: the whole `i*e1 + j` form — reuse the index factoring, which proves
            // exactly `i*e1 + j < len`.
            for (int bse=0; bse<V->nvar && !c.ok; bse++)
                if (V->shape_rank[bse]==2 && ins->result
                    && vra_factor_shape(V, W, bse, ins->result->id)) c.ok = true;
        }
    }
    // B1: a 64-bit accumulator has no wider type to widen INTO, so its obligation lands
    // here rather than at a narrowing. Same product bound, same place to ask for it.
    if (!c.ok && ins->result) c.ok = vra_accum_info(V, W, ins->result, &c, tlo, thi);
    vra_add_check(V, c);
}
// Division/remainder: the divisor must be provably non-zero.
// ★ `d != 0` IS A FACT, and an interval cannot hold it: excluding a point from the middle of
// a range is not an interval, and it is not an octagon constraint either — which is why
// `vra_refine_guard` writes `(void)ne`. The old engine carries a dedicated nonzero marker for
// exactly this, and without one every guarded division in the corpus was refused.
//
// So ask the CFG instead of the domain. A block reached only through the true edge of
// `d != 0` — or only through the false edge of `d == 0`, which is the early-return spelling —
// has a nonzero `d`, and that is a dominance question the IR can already answer. Walking the
// single-predecessor chain is the cheap, sound fragment of it: it proves guardedness where it
// says yes and simply declines otherwise.
static bool vra_guarded_nonzero(Vra *V, IrBlock *b, int vid) {
    // An entry ASSUME says the same thing a guard does, from a parameter refinement
    // (`func divide(a i32, b i32 != 0)`) rather than from an edge.
    if (V->f->entry)
        for (IrInstr *ins=V->f->entry->instrs; ins; ins=ins->next) {
            if (ins->op != IR_ASSUME || ins->n_operands < 1) continue;
            IrInstr *ic = V->def[ins->operands[0]->id];
            if (!ic || ic->op!=IR_ICMP || ic->aux.cmp!=IR_CMP_NE || ic->n_operands<2) continue;
            int z = ic->operands[1]->id;
            if (ic->operands[0]->id==vid && z>=0 && z<V->nvar && V->cknown[z] && V->cval[z]==0)
                return true;
        }
    for (int depth=0; b && depth<64; depth++) {
        IrEdge *e = b->preds;
        if (!e || e->next) return false;                     // not a single-predecessor chain
        IrBlock *p = e->block;
        if (!p) return false;
        if (p->term.kind == IR_TERM_BR_COND && p->term.cond) {
            IrInstr *ic = V->def[p->term.cond->id];
            if (ic && ic->op==IR_ICMP && ic->n_operands>=2) {
                int a = ic->operands[0]->id, z = ic->operands[1]->id;
                bool zero_is_const = (z>=0 && z<V->nvar && V->cknown[z] && V->cval[z]==0);
                if (a == vid && zero_is_const) {
                    if (ic->aux.cmp==IR_CMP_NE && p->term.a == b) return true;   // `if d != 0 {`
                    if (ic->aux.cmp==IR_CMP_EQ && p->term.b == b) return true;   // `if d == 0 { return }`
                }
            }
        }
        b = p;
    }
    return false;
}
static void vra_check_divzero(Vra *V, Octagon *W, IrInstr *ins, IrBlock *at) {
    if (ins->n_operands<2 || ins->unchecked) return;         // `unsafe` waives it, as for bounds
    int64_t lo,hi; vra_range(V,W,ins->operands[1],&lo,&hi);
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_DIVZERO; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = (lo>0) || (hi<0);                                // 0 ∉ [lo,hi]
    if (!c.ok) c.ok = vra_guarded_nonzero(V, at, ins->operands[1]->id);
    vra_add_check(V, c);
}

// A subslice `src[lo..hi]` lowered to make_slice(elem_ptr(src_data,lo), hi−lo) is in
// bounds iff 0 ≤ lo AND hi ≤ len(src). (The start elem_ptr is NOT an element access,
// so it is skipped in vra_check_elem; the real obligation is checked here.)
static void vra_check_subslice(Vra *V, Octagon *W, IrInstr *ms) {
    if (ms->n_operands<2) return;
    IrInstr *ndd = V->def[ms->operands[0]->id];
    if (!ndd || ndd->op!=IR_ELEM_PTR || ndd->n_operands<2) return;    // array→slice decay, not a subslice
    int lo = ndd->operands[1]->id;
    IrInstr *bd = V->def[ndd->operands[0]->id];
    int64_t clen=-1; int lenvar=-1;
    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty && bd->aux.alloca_ty->kind==IRT_ARRAY) clen=bd->aux.alloca_ty->array_len;
    else if (bd && bd->op==IR_SLICE_DATA && bd->n_operands>=1){ int s=bd->operands[0]->id; if(V->slicelen[s]>=0) lenvar=V->slicelen[s]; }
    int hi=-1;
    IrInstr *lend = V->def[ms->operands[1]->id];
    if (lend && lend->op==IR_SUB && lend->n_operands>=2 && lend->operands[1]->id==lo) hi=lend->operands[0]->id; // len = hi − lo
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_BOUNDS; c.at=ms; c.line=ms->line; c.col=ms->col;
    int64_t llo,lhi; bool lhl,lhh; vra_interval(V, W,lo,&llo,&lhl,&lhi,&lhh);
    c.lo_ok = lhl && llo>=0;                                          // 0 ≤ lo
    c.has_len = (clen>=0 || lenvar>=0);
    if (hi<0) c.hi_ok=false;                                          // couldn't recover hi ⇒ conservative
    else if (clen>=0){ int64_t hl,hh_; bool a,bb; vra_interval(V, W,hi,&hl,&a,&hh_,&bb); c.hi_ok = bb && hh_<=clen; } // hi ≤ N
    else if (lenvar>=0) c.hi_ok = vra_diff_ub(V, W, hi, lenvar) <= 0;   // hi − len ≤ 0
    else c.hi_ok=false;
    c.ok = c.lo_ok && c.hi_ok;
    vra_add_check(V, c);
}

// Does `a cmp b` hold in the (closed) octagon? The discharge dual of refine.
static bool vra_icmp_holds(Vra *V, Octagon *W, int a, int b, IrCmp cmp) {
    int64_t ab = vra_diff_ub(V, W, a, b);   // bound on a − b
    int64_t ba = vra_diff_ub(V, W, b, a);   // bound on b − a
    switch (cmp) {
        case IR_CMP_SLT: case IR_CMP_ULT: return ab <= -1;
        case IR_CMP_SLE: case IR_CMP_ULE: return ab <= 0;
        case IR_CMP_SGT: case IR_CMP_UGT: return ba <= -1;
        case IR_CMP_SGE: case IR_CMP_UGE: return ba <= 0;
        case IR_CMP_EQ:  return ab <= 0 && ba <= 0;
        case IR_CMP_NE:  return ab <= -1 || ba <= -1;
        default: return false;
    }
}
// Discharge an IR_ASSERT (its operand is a bool; when an icmp, check it holds).
static void vra_check_assert(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->n_operands<1) return;
    IrInstr *ic = V->def[ins->operands[0]->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return;
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_PRECOND; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = vra_icmp_holds(V, W, ic->operands[0]->id, ic->operands[1]->id, ic->aux.cmp);
    vra_add_check(V, c);
}

// ── termination consumer (a func must have only terminating loops) ───────────
static bool vra_is_scalar_cell(Vra *V, int v) {
    IrInstr *d=(v>=0&&v<V->nvar)?V->def[v]:NULL;
    return d && d->op==IR_ALLOCA && d->aux.alloca_ty &&
           d->aux.alloca_ty->kind!=IRT_ARRAY && d->aux.alloca_ty->kind!=IRT_SLICE;
}
// A guard bound is loop-invariant if it is a parameter, a constant, a slice length,
// or defined in a block strictly above the loop header (structured CFG order).
static bool vra_loop_invariant(Vra *V, IrValue *val, int Hid) {
    IrInstr *d=V->def[val->id];
    if (!d) return true;
    if (d->op==IR_CONST || d->op==IR_SLICE_LEN) return true;
    return V->defblk[val->id]>=0 && V->defblk[val->id] < Hid;
}
// ── THE NATURAL LOOP OF A HEADER ─────────────────────────────────────────────────────────
// {H} ∪ {b : b reaches a back-edge source of H without passing through H}. The textbook set,
// and it replaces an id-RANGE heuristic ("a natural loop is a contiguous id range, because
// Lain's CFG is structured") that stopped being true when `case` and `try` began allocating
// their join block before their arms. Two consumers depended on it and both were wrong:
//
//   · the WIDENING selector — a variable modified in the loop but outside the id range was
//     never widened, so its bound grew forever and the fixpoint DID NOT CONVERGE. Five corpus
//     programs, including the flagship binary-search and tokenizer, were exiting through the
//     sweep cap with a PARTIAL fixpoint, i.e. an under-approximation every proof then rested
//     on. (One of them predates the join-block change: the heuristic was already wrong.)
//   · the termination step recogniser, which counts stores "in the loop region".
//
// Fails CONSERVATIVE: if anything cannot be computed, every block is in the loop, which
// widens more and proves less but cannot be unsound.
static void vra_natural_loop(Vra *V, IrBlock *H, int nb, char *inloop) {
    memset(inloop, 0, (size_t)nb);
    char *fwd = calloc((size_t)nb, 1);
    IrBlock **st = malloc((size_t)nb * sizeof(IrBlock*));
    if (!fwd || !st) { memset(inloop, 1, (size_t)nb); free(fwd); free(st); return; }
    // forward reachability from H — a pred of H that H reaches is a BACK-EDGE source
    int sp = 0; st[sp++] = H; fwd[H->id] = 1;
    while (sp > 0) {
        IrBlock *u = st[--sp];
        IrBlock *sv[2]; int ns = 0;
        if (u->term.kind==IR_TERM_BR)      { if (u->term.a) sv[ns++]=u->term.a; }
        else if (u->term.kind==IR_TERM_BR_COND) { if (u->term.a) sv[ns++]=u->term.a;
                                                  if (u->term.b) sv[ns++]=u->term.b; }
        else if (u->term.kind==IR_TERM_SWITCH) {
            if (u->term.a && u->term.a->id>=0 && u->term.a->id<nb && !fwd[u->term.a->id])
                { fwd[u->term.a->id]=1; st[sp++]=u->term.a; }
            for (IrSwitchCase *c=u->term.cases; c; c=c->next)
                if (c->target && c->target->id>=0 && c->target->id<nb && !fwd[c->target->id])
                    { fwd[c->target->id]=1; st[sp++]=c->target; }
        }
        for (int k=0;k<ns;k++)
            if (sv[k]->id>=0 && sv[k]->id<nb && !fwd[sv[k]->id]) { fwd[sv[k]->id]=1; st[sp++]=sv[k]; }
    }
    // backward closure from the back-edge sources, stopping at H
    inloop[H->id] = 1; sp = 0;
    for (IrEdge *e=H->preds; e; e=e->next)
        if (e->block && e->block->id>=0 && e->block->id<nb && fwd[e->block->id] && !inloop[e->block->id])
            { inloop[e->block->id]=1; st[sp++]=e->block; }
    while (sp > 0) {
        IrBlock *u = st[--sp];
        for (IrEdge *e=u->preds; e; e=e->next)
            if (e->block && e->block->id>=0 && e->block->id<nb && !inloop[e->block->id])
                { inloop[e->block->id]=1; st[sp++]=e->block; }
    }
    free(fwd); free(st);
}

// A structured while-loop terminates if its guard variable is a memory cell updated
// by EXACTLY ONE well-formed step `cell = load(cell) ± c` whose direction drains the
// loop-invariant bound (rise toward an upper bound / fall toward a lower one). The
// "exactly one store" rule is conservative: any other write to the cell ⇒ not proven.
// Does the value stored back into `cell` provably lie STRICTLY BELOW the cell's old value,
// given that the loop guard keeps the cell positive? The step recogniser used to demand
// `cell ± constant`, which is one shape out of several that every real loop uses:
//
//   b = a % b       the remainder is in [0, b−1] — Euclid's GCD, and the divisor IS the cell
//   n = n / k       k ≥ 2, so n ≥ 1 ⇒ n/k < n — the halving loop
//   n = n >> k      k ≥ 1, same argument
//   x = x & (x − 1) clears the lowest set bit — the popcount loop
//
// Each is a FACT THE DOMAIN ALREADY HAS (vra_div_facts and the mask/modulo transfers state
// exactly these intervals); the termination check simply was not asking. Recognising the
// shape rather than querying the octagon at the store keeps this a syntactic, cheap test —
// but every case is one the numeric domain independently justifies.
static bool vra_step_decreases(Vra *V, int cell, IrInstr *vd) {
    if (!vd) return false;
    int nops = vd->n_operands;
    IrInstr *ld0 = (nops>=1) ? V->def[vd->operands[0]->id] : NULL;
    bool op0_is_cell = ld0 && ld0->op==IR_LOAD && ld0->n_operands>=1 && ld0->operands[0]->id==cell;
    switch (vd->op) {
        case IR_UREM: case IR_SREM: {
            // `a % b` with b THE CELL: the remainder is below b, so the cell falls.
            if (nops < 2) return false;
            IrInstr *ld1 = V->def[vd->operands[1]->id];
            return ld1 && ld1->op==IR_LOAD && ld1->n_operands>=1 && ld1->operands[0]->id==cell;
        }
        case IR_UDIV: case IR_SDIV: {
            if (!op0_is_cell || nops < 2) return false;
            int d = vd->operands[1]->id;
            return d>=0 && d<V->nvar && V->cknown[d] && V->cval[d] >= 2;
        }
        case IR_LSHR: case IR_ASHR: {
            if (!op0_is_cell || nops < 2) return false;
            int k = vd->operands[1]->id;
            return k>=0 && k<V->nvar && V->cknown[k] && V->cval[k] >= 1;
        }
        case IR_AND: {
            // `x & (x − 1)` — clears the lowest set bit, so it is strictly below x for x ≥ 1.
            if (!op0_is_cell || nops < 2) return false;
            IrInstr *sub = V->def[vd->operands[1]->id];
            if (!sub || sub->op!=IR_SUB || sub->n_operands<2) return false;
            IrInstr *ls = V->def[sub->operands[0]->id];
            int one = sub->operands[1]->id;
            return ls && ls->op==IR_LOAD && ls->n_operands>=1 && ls->operands[0]->id==cell
                && one>=0 && one<V->nvar && V->cknown[one] && V->cval[one]==1;
        }
        default: return false;
    }
}
static bool vra_loop_terminates(Vra *V, IrBlock *H) {
    if (H->term.kind != IR_TERM_BR_COND) return false;
    IrInstr *ic = V->def[H->term.cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return false;
    IrCmp p = ic->aux.cmp;
    for (int side=0; side<2; side++) {
        IrValue *ivv=ic->operands[side], *bnd=ic->operands[side^1];
        IrInstr *ivd=V->def[ivv->id];
        if (!ivd || ivd->op!=IR_LOAD || ivd->n_operands<1) continue;
        int cell=ivd->operands[0]->id;
        if (!vra_is_scalar_cell(V,cell)) continue;
        if (!vra_loop_invariant(V,bnd,H->id)) continue;
        int64_t step=0; int nstore=0, nupd=0;
        // The loop's REAL body, not "every block with an id at or above the header's" — the
        // same id-order heuristic that broke the widening selector, and here it counted stores
        // from unrelated later code as if they were loop updates.
        int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
        char *body = malloc((size_t)nbb);
        if (body) vra_natural_loop(V, H, nbb, body);
        for (IrBlock *b=V->f->blocks; b; b=b->next) {
            if (body && !(b->id>=0 && b->id<nbb && body[b->id])) continue;
            for (IrInstr *s=b->instrs; s; s=s->next) {
                if (s->op!=IR_STORE || s->n_operands<2 || s->operands[0]->id!=cell) continue;
                nstore++;
                IrInstr *vd=V->def[s->operands[1]->id];
                if (vd && (vd->op==IR_ADD||vd->op==IR_SUB) && vd->n_operands>=2) {
                    IrInstr *ld=V->def[vd->operands[0]->id]; int c=vd->operands[1]->id;
                    if (ld && ld->op==IR_LOAD && ld->operands[0]->id==cell && V->cknown[c]) {
                        step = (vd->op==IR_ADD)? V->cval[c] : -V->cval[c]; nupd++;
                    }
                } else if (vra_step_decreases(V, cell, vd)) {
                    step = -1; nupd++;      // a falling step the domain justifies (see above)
                }
            }
        }
        free(body);
        if (nstore!=1 || nupd!=1) continue;
        bool lt=(p==IR_CMP_SLT||p==IR_CMP_ULT||p==IR_CMP_SLE||p==IR_CMP_ULE);
        bool gt=(p==IR_CMP_SGT||p==IR_CMP_UGT||p==IR_CMP_SGE||p==IR_CMP_UGE);
        if (lt && step>0) return true;
        if (gt && step<0) return true;
    }
    return false;
}

/* ─────────────────────────────────────────────────────────────────────────────────────────
   B1 — LOOP SUMMARISATION: bounding a running total by TRIP COUNT x STEP

   `s = s + a[i]` inside a loop is the commonest rejection in the language and the largest
   category the A1 survey measured (21 of 129 unproven obligations). The fact needed is

       s  ∈  [ s0 + T*δlo , s0 + T*δhi ]

   with T the trip count and δ the per-iteration addend. That is a PRODUCT of two quantities,
   so no relational domain can hold it: octagons carry ±x±y ≤ c, polyhedra carry linear
   combinations, and T·δ is neither. It has to be derived OUTSIDE the domain and handed back
   as an interval, which is what this does.

   Deliberately a check-time refinement rather than a state injection: it can only turn an
   obligation from unproven to proven, so it cannot make the fixpoint less sound, and it costs
   nothing on programs that do not need it.

   It pays only when T is bounded. `while i < a.len` over a runtime slice gives T ≤ 2^64 and
   the sum really can overflow — refusing is right. Over `a i32[4096]` it gives T ≤ 4096, and
   4096 × 255 fits an i32 with room to spare.
   ───────────────────────────────────────────────────────────────────────────────────────── */
static bool vra_mul_ovf(int64_t a, int64_t b, int64_t *out) {
    if (a==0 || b==0) { *out = 0; return false; }
    int64_t r = a * b;
    if (r / b != a) return true;                      // wrapped
    *out = r; return false;
}

// The trip count of the natural loop headed at H, when the induction variable rises by a
// positive constant toward a bounded limit. Mirrors vra_loop_terminates' recovery of
// (cell, bound, step): that function proves the loop ENDS, this asks how late.
static bool vra_loop_trips(Vra *V, Octagon *W, IrBlock *H, int64_t *T) {
    if (H->term.kind != IR_TERM_BR_COND || !H->term.cond) return false;
    IrInstr *ic = V->def[H->term.cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return false;
    IrCmp pr = ic->aux.cmp;
    if (!(pr==IR_CMP_SLT||pr==IR_CMP_ULT||pr==IR_CMP_SLE||pr==IR_CMP_ULE)) return false;
    IrValue *ivv = ic->operands[0], *bnd = ic->operands[1];
    IrInstr *ivd = V->def[ivv->id];
    if (!ivd || ivd->op!=IR_LOAD || ivd->n_operands<1) return false;
    int cell = ivd->operands[0]->id;
    if (!vra_is_scalar_cell(V,cell)) return false;
    if (!vra_loop_invariant(V,bnd,H->id)) return false;

    int64_t blo,bhi; vra_range(V,W,bnd,&blo,&bhi); (void)blo;
    if (bhi >= INT64_MAX/2) return false;             // an unbounded limit bounds nothing
    int64_t ilo,ihi; bool hl,hh; vra_interval(V,W,cell,&ilo,&hl,&ihi,&hh); (void)ihi; (void)hh;
    if (!hl) ilo = 0;

    int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
    char *body = malloc((size_t)nbb); if (!body) return false;
    vra_natural_loop(V, H, nbb, body);
    int64_t step = 0; int nupd = 0;
    for (IrBlock *b=V->f->blocks; b; b=b->next) {
        if (!(b->id>=0 && b->id<nbb && body[b->id])) continue;
        for (IrInstr *st=b->instrs; st; st=st->next) {
            if (st->op!=IR_STORE || st->n_operands<2 || st->operands[0]->id!=cell) continue;
            IrInstr *vd=V->def[st->operands[1]->id];
            if (vd && vd->op==IR_ADD && vd->n_operands>=2) {
                IrInstr *ld=V->def[vd->operands[0]->id]; int k=vd->operands[1]->id;
                if (ld && ld->op==IR_LOAD && ld->operands[0]->id==cell &&
                    k>=0 && k<V->nvar && V->cknown[k] && V->cval[k] > 0) { step=V->cval[k]; nupd++; }
                else nupd += 2;                        // an update we cannot read: give up
            } else nupd += 2;
        }
    }
    free(body);
    if (nupd != 1 || step <= 0) return false;
    if (bhi < ilo) { *T = 0; return true; }
    *T = (bhi - ilo + step - 1) / step;                // ceil((limit − start) / step)
    return *T >= 0;
}

// Is `val` a running total in a loop, and what does one iteration add?
static bool vra_accum_delta(Vra *V, Octagon *W, IrValue *val, IrBlock **H,
                            int64_t *s0lo, int64_t *s0hi, int64_t *dlo, int64_t *dhi) {
    if (!val || val->id<0 || val->id>=V->nvar) return false;
    IrInstr *add = V->def[val->id];
    if (!add || (add->op!=IR_ADD && add->op!=IR_SUB) || add->n_operands<2) return false;
    IrInstr *ld = V->def[add->operands[0]->id];
    if (!ld || ld->op!=IR_LOAD || ld->n_operands<1) return false;
    int cell = ld->operands[0]->id;

    int blk = V->defblk[val->id]; if (blk < 0) return false;
    int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
    char *body = malloc((size_t)nbb); if (!body) return false;
    // `is_loop_header` is set by the back-edge pass; anything else is not a loop and
    // vra_natural_loop over it means nothing.
    IrBlock *found = NULL;
    for (IrBlock *h=V->f->blocks; h && !found; h=h->next) {
        if (!h->is_loop_header) continue;
        vra_natural_loop(V, h, nbb, body);
        if (blk>=0 && blk<nbb && body[blk]) found = h;
    }
    free(body);
    if (!found) return false;
    *H = found;

    // The addend's range. vra_range follows widening casts to the source's own type, so
    // `(a[i] as i32)` on a u8 element reads as [0,255] rather than [-2^31,255] — without
    // which no sum is bounded by anything.
    vra_range(V, W, add->operands[1], dlo, dhi);
    if (add->op==IR_SUB) { int64_t t=*dlo; *dlo = -*dhi; *dhi = -t; }
    int64_t clo,chi; bool hl,hh; vra_interval(V,W,cell,&clo,&hl,&chi,&hh);
    *s0lo = hl ? clo : 0; *s0hi = hh ? chi : 0;
    return true;
}

// Same computation, but it reports what it found even when the bound does not hold — that is
// what the diagnostic needs.
static bool vra_accum_info(Vra *V, Octagon *W, IrValue *val, VraCheck *c,
                           int64_t tlo, int64_t thi) {
    IrBlock *H = NULL; int64_t s0lo, s0hi, dlo, dhi, T;
    if (!vra_accum_delta(V, W, val, &H, &s0lo, &s0hi, &dlo, &dhi)) return false;
    bool haveT = vra_loop_trips(V, W, H, &T);
    if (c) {
        c->accum = true; c->accum_dlo = dlo; c->accum_dhi = dhi;
        c->accum_s0lo = s0lo; c->accum_s0hi = s0hi;
        c->accum_T = haveT ? T : -1;                 // -1 = the trip count is not bounded
    }
    if (!haveT) return false;
    int64_t alo, ahi;
    if (vra_mul_ovf(T, dlo, &alo)) return false;
    if (vra_mul_ovf(T, dhi, &ahi)) return false;
    if (alo > 0) alo = 0;
    if (ahi < 0) ahi = 0;
    int64_t lo, hi;
    if (__builtin_add_overflow(s0lo, alo, &lo)) return false;
    if (__builtin_add_overflow(s0hi, ahi, &hi)) return false;
    return lo >= tlo && hi <= thi;
}


// ── the fixpoint over the CFG ────────────────────────────────────────────────
static Vra *vra_analyze(IrFunc *f) {
    Vra *V = calloc(1, sizeof *V);
    V->f=f; V->nvar = f->next_value_id>0 ? f->next_value_id : 1;
    // ── VARIABLE PACKING (2.2). Only values that can appear in a numeric relation get an
    // octagon dimension. An element pointer, a slice, a struct, a unit never can, and giving
    // them one cost cubically: a 128-element array literal reached dim 645 and a 3.3 MB
    // matrix per block. Scalar ALLOCAs are kept despite being pointers — the domain models
    // one as a memory CELL, which is exactly a tracked numeric variable.
    V->odim = malloc((size_t)V->nvar*sizeof(int));
    for (int i=0;i<V->nvar;i++) V->odim[i] = -1;
    V->noct = 0;
    for (IrParam *p=f->params; p; p=p->next) {
        IrType *pt = p->value ? p->value->type : NULL;
        if (!p->value || p->value->id<0 || p->value->id>=V->nvar || !pt) continue;
        // A `var x i32` parameter is a POINTER, but it is also a numeric CELL (see
        // vra_is_param_cell), so it needs a dimension like any other — without one every
        // constraint written about it was silently discarded and the cell modelling below
        // did nothing at all.
        bool cell = pt->kind==IRT_PTR && pt->ptr_mut && pt->elem
                 && (pt->elem->kind==IRT_INT || pt->elem->kind==IRT_BOOL);
        if (pt->kind==IRT_INT || pt->kind==IRT_BOOL || cell)
            V->odim[p->value->id] = V->noct++;
    }
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            IrValue *rv = ins->result;
            if (!rv || rv->id<0 || rv->id>=V->nvar || V->odim[rv->id]>=0) continue;
            bool keep;
            if (ins->op==IR_ALLOCA)
                keep = ins->aux.alloca_ty && ins->aux.alloca_ty->kind!=IRT_ARRAY;  // scalar cell
            else if (ins->op==IR_CONST)
                keep = false;                      // exact in cval/cknown; see vra_range
            else
                keep = rv->type && (rv->type->kind==IRT_INT || rv->type->kind==IRT_BOOL);
            if (keep) V->odim[rv->id] = V->noct++;
        }
    if (V->noct == 0) V->noct = 1;                 // never size the matrix to zero
    int dim=2*V->noct; V->dsz=dim*dim;
    // The map is a global the octagon accessors consult, and vra_analyze RECURSES (a call's
    // return range is read by analysing the callee), so the caller's map must be restored on
    // the way out or its values would translate through the callee's packing.
    const int *oct_map_saved = oct_map;
    oct_map = V->odim;
    int nb=f->next_block_id;
    V->in=calloc(nb,sizeof(int64_t*)); V->reached=calloc(nb,sizeof(bool));
    V->def=calloc(V->nvar,sizeof(IrInstr*)); V->defblk=calloc(V->nvar,sizeof(int));
    V->val=calloc(V->nvar,sizeof(IrValue*));
    V->cval=calloc(V->nvar,sizeof(int64_t)); V->cknown=calloc(V->nvar,sizeof(bool));
    V->slicelen=calloc(V->nvar,sizeof(int)); V->subslice_gep=calloc(V->nvar,sizeof(bool));
    V->escaped=calloc(V->nvar,sizeof(bool));
    V->persist=calloc(V->nvar,sizeof(bool));
    V->shape_rank=calloc(V->nvar,sizeof(int));
    V->shape_ext=calloc(V->nvar,sizeof(*V->shape_ext));
    vra_prepass(V);
    for (int i=0;i<nb;i++) V->in[i]=malloc(V->dsz*sizeof(int64_t));

    int64_t wb[1]; (void)wb;
    int64_t *W_m=malloc(V->dsz*8), *T_m=malloc(V->dsz*8), *J_m=malloc(V->dsz*8), *D_m=malloc(V->dsz*8);
    Octagon W={V->noct,dim,W_m}, T={V->noct,dim,T_m}, J={V->noct,dim,J_m}, D={V->noct,dim,D_m};

    { Octagon E={V->noct,dim,V->in[f->entry->id]}; oct_init_top(&E,V->noct,E.m);
      // seed each integer parameter's type interval (a usize is ≥ 0, etc.). Skip a
      // bound whose doubled DBM entry would overflow (e.g. u64's ~2^63 upper).
      for (IrParam *p=f->params; p; p=p->next) {   // only the type interval; refinements
          int64_t tlo,thi;                          // now arrive as entry IR_ASSUME nodes
          if (!irtype_int_range(p->value->type,&tlo,&thi)) continue;
          if (tlo > -OCT_INF/2) oct_add_lb(&E, p->value->id, tlo);
          if (thi <  OCT_INF/2) oct_add_ub(&E, p->value->id, thi);
      }
    }
    V->reached[f->entry->id]=true;

    // Per-loop-header set of cells MODIFIED inside the loop (for selective widening — an
    // outer induction var, invariant in an inner loop, must not be widened at the inner
    // header). Loop body ≈ block-id range [H, max back-edge-source id]; Lain's CFG is
    // structured, so a natural loop is a contiguous id range. A `store %c,_` modifies %c.
    char **loopmod = calloc(nb, sizeof(char*));
    for (IrBlock *H=f->blocks; H; H=H->next) {
        if (!H->is_loop_header) continue;
        char *inloop = malloc((size_t)nb);
        if (inloop) vra_natural_loop(V, H, nb, inloop);
        // ★ Keyed by OCTAGON SLOT, not value id. oct_widen_sel reads `mod[i/2]` where i is a
        // DBM index, so the table must live in slot space. While the mapping was the identity
        // the two coincided; under variable packing they do not, and the mismatch made the
        // widening consult an unrelated variable — a loop counter that was never widened kept
        // a stale bound and `src[i]` under an UNBOUNDED `i < n` was PROVEN check-free. That is
        // a removed bounds check, caught by the corpus's own soundness lock for this shape.
        char *mod = calloc((size_t)V->noct,1);
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (inloop && !(b->id>=0 && b->id<nb && inloop[b->id])) continue;
            for (IrInstr *ins=b->instrs; ins; ins=ins->next)
                if (ins->op==IR_STORE && ins->n_operands>=1 && ins->operands[0]->id < V->nvar) {
                    int sl = V->odim[ins->operands[0]->id];
                    if (sl >= 0) mod[sl]=1;
                }
        }
        free(inloop);
        loopmod[H->id]=mod;
    }

    // ★ THE THRESHOLD LADDER: every integer constant the function itself mentions, at both DBM
    // scalings (a unary bound `v ≤ c` is stored doubled, a difference `v−w ≤ c` is not), plus
    // the type maxima the guards are written against. Ascending and deduplicated, so the
    // widening can climb it. Capped, because a program with hundreds of distinct constants
    // would otherwise pay for a ladder it does not need.
    #define VRA_MAX_THR 96
    int64_t thr[VRA_MAX_THR]; int nthr = 0;
    for (int i=0; i<V->nvar && nthr < VRA_MAX_THR-2; i++) {
        if (!V->cknown[i]) continue;
        int64_t c1 = V->cval[i];
        if (c1 < 0 || c1 > OCT_INF/4) continue;
        for (int k=0;k<2;k++) {
            int64_t cand = k ? c1*2 : c1;
            bool seen=false; for (int t=0;t<nthr;t++) if (thr[t]==cand) { seen=true; break; }
            if (!seen && nthr < VRA_MAX_THR) thr[nthr++] = cand;
        }
    }
    for (int i=0;i<nthr;i++) for (int j=i+1;j<nthr;j++)
        if (thr[j] < thr[i]) { int64_t t=thr[i]; thr[i]=thr[j]; thr[j]=t; }

    bool changed=true; int sweeps=0;
    while (changed && sweeps++ < 1000) {
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!V->reached[b->id]) continue;
            memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->noct; W.dim=dim;
            oct_close(&W);
            for (IrInstr *ins=b->instrs; ins; ins=ins->next) vra_transfer_instr(V,&W,ins);
            oct_close(&W);
            IrBlock *succ[2]={NULL,NULL}; int ns=0; bool guarded=false; IrValue *cond=NULL;
            if (b->term.kind==IR_TERM_BR){ succ[0]=b->term.a; ns=1; }
            else if (b->term.kind==IR_TERM_BR_COND){ succ[0]=b->term.a; succ[1]=b->term.b; ns=2; guarded=true; cond=b->term.cond; }
            for (int k=0;k<ns;k++) {
                IrBlock *s=succ[k]; if(!s) continue;
                memcpy(T_m, W_m, V->dsz*8); T.nvar=V->noct; T.dim=dim;
                if (guarded){ vra_refine_guard(V,&T,cond,k==0); oct_close(&T); }
                if (oct_is_bottom(&T)) continue;
                if (!V->reached[s->id]) { memcpy(V->in[s->id],T_m,V->dsz*8); V->reached[s->id]=true; changed=true; continue; }
                Octagon In={V->noct,dim,V->in[s->id]};
                oct_join(&J,&In,&T);
                if (s->is_loop_header){ oct_widen_thr(&D,&In,&J,loopmod[s->id],thr,nthr); memcpy(J_m,D_m,V->dsz*8); }
                if (!oct_leq(&J,&In)){ memcpy(V->in[s->id],J_m,V->dsz*8); changed=true; }
            }
        }
    }
    // ★ FAIL CLOSED IF THE FIXPOINT DID NOT CONVERGE. The sweep cap exists so a pathological
    // CFG cannot hang the compiler, but exiting through it leaves a PARTIAL fixpoint — an
    // UNDER-approximation — and every proof discharged against one would be unsound. Nothing
    // said so: the final pass ran over it exactly as if it had converged. The established
    // mechanism for "this function cannot be judged" is `incomplete`, and every consumer
    // already honours it.
    if (changed) {
        f->incomplete = true;
        if (!f->incomplete_why) f->incomplete_why = "vra-fixpoint-did-not-converge";
    }
    // final pass: discharge index obligations against the converged in-states
    for (IrBlock *b=f->blocks; b; b=b->next) {
        if (!V->reached[b->id]) continue;
        memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->noct; W.dim=dim; oct_close(&W);
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            switch (ins->op) {
                // a constant index into a FIXED array is fully decidable from constants
                // (index + compile-time length), so skip the O(dim^3) close — this saves a
                // large array literal `[0,…]` (N const-index stores) from N closes. A slice
                // base still needs closure for its (relational) length, so don't skip there.
                case IR_ELEM_PTR: {
                    bool cidx = ins->n_operands>=2 && V->cknown[ins->operands[1]->id];
                    IrValue *eb = ins->n_operands>=1 ? ins->operands[0] : NULL;
                    IrInstr *ebd = eb ? V->def[eb->id] : NULL;
                    bool fixed = (ebd && ebd->op==IR_ALLOCA && ebd->aux.alloca_ty && ebd->aux.alloca_ty->kind==IRT_ARRAY)
                               || (eb && eb->type && eb->type->kind==IRT_ARRAY);
                    if (!(cidx && fixed)) oct_close(&W);
                    vra_check_elem(V,&W,ins); break;
                }
                case IR_MAKE_SLICE: oct_close(&W); vra_check_subslice(V,&W,ins); break;
                case IR_ASSERT: oct_close(&W); vra_check_assert(V,&W,ins); break;
                case IR_ADD: case IR_SUB: case IR_MUL: oct_close(&W); vra_check_overflow(V,&W,ins); break;
                // Path-F's other half: the widened result meets a narrower slot HERE.
                case IR_STORE: {
                    if (ins->n_operands < 2) break;
                    IrType *pt = ins->operands[0]->type;
                    IrInstr *ad = V->def[ins->operands[0]->id];
                    IrType *slot = (pt && pt->kind==IRT_PTR) ? pt->elem
                                 : (ad && ad->op==IR_ALLOCA) ? ad->aux.alloca_ty : NULL;
                    if (slot && slot->kind==IRT_INT && ins->operands[1]->type
                        && ins->operands[1]->type->kind==IRT_INT
                        && ins->operands[1]->type->bits > slot->bits) {
                        oct_close(&W);
                        vra_check_narrow(V,&W, ins->operands[1], slot, ins, ins->line, ins->col);
                    }
                    break;
                }
                case IR_CAST:
                    if (ins->n_operands >= 1 && ins->aux.cast_kind == IR_CAST_TRUNC
                        && ins->result && ins->result->type) {
                        oct_close(&W);
                        vra_check_narrow(V,&W, ins->operands[0], ins->result->type,
                                         ins, ins->line, ins->col);
                    }
                    break;
                case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: oct_close(&W); vra_check_divzero(V,&W,ins,b); break;
                default: break;
            }
            vra_transfer_instr(V,&W,ins);
        }
    }
    if (vra_dump_enabled) vra_dump_state(V, stderr);
    // one termination obligation per loop header — but ONLY for a `func` (totality is a
    // func requirement; a `proc` may loop forever, e.g. an event loop). Emitting it for
    // procs was spuriously marking terminating procs "partially proven".
    if (f->kind == IR_FUNC_PURE)
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!b->is_loop_header) continue;
            VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_TERMINATION; c.ok=vra_loop_terminates(V,b);
            vra_add_check(V, c);
        }
    // RETURN RANGE: union the interval of every returned value, read from that block's
    // converged state replayed to its terminator. Only for a faithfully lowered function —
    // an `incomplete` body could return anything.
    if (f->ret_type && !f->incomplete) {
        int64_t rlo=INT64_MAX, rhi=INT64_MIN; bool any=false, all=true;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (b->term.kind!=IR_TERM_RET) continue;
            if (!b->term.cond) { all=false; continue; }
            if (!V->reached[b->id]) continue;          // unreachable: contributes nothing
            memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->noct; W.dim=dim;
            for (IrInstr *ins=b->instrs; ins; ins=ins->next) vra_transfer_instr(V,&W,ins);
            oct_close(&W);
            int64_t lo,hi; vra_range(V,&W,b->term.cond,&lo,&hi);
            if (lo<rlo) rlo=lo;
            if (hi>rhi) rhi=hi;
            any=true;
        }
        if (any && all && rlo<=rhi) { f->ret_range_lo=rlo; f->ret_range_hi=rhi; f->ret_range_state=2; }
        else f->ret_range_state=3;                     // analysed, nothing usable
    } else if (f->ret_type) f->ret_range_state=3;
    // fail closed: if lowering was infaithful (a dropped/placeholder'd construct), no
    // proof over this IR is trustworthy — the dropped code could change a checked value.
    if (f->incomplete) for (int i=0;i<V->nchecks;i++) V->checks[i].ok=false;
    for (int i=0;i<nb;i++) if (loopmod[i]) free(loopmod[i]);
    free(loopmod);
    free(W_m); free(T_m); free(J_m); free(D_m);
    oct_map = oct_map_saved;
    return V;
}

static bool vra_ret_range(IrFunc *g, int64_t *lo, int64_t *hi) {
    if (!g || g->is_extern || !g->ret_type) return false;
    if (g->ret_range_state==1) return false;      // recursive query — no fixpoint over itself
    if (g->ret_range_state==3) return false;      // analysed, nothing usable
    if (g->ret_range_state==0) {
        g->ret_range_state = 1;                   // mark in-progress BEFORE recursing
        Vra *sub = vra_analyze(g);                // sets state to 2 or 3 as a side effect
        vra_free(sub);
        if (g->ret_range_state==1) g->ret_range_state=3;   // defensive: never leave it pending
    }
    if (g->ret_range_state!=2) return false;
    *lo=g->ret_range_lo; *hi=g->ret_range_hi;
    return true;
}
// ── borrow phase D: the NUMERIC DISJOINTNESS bridge (design §4) ─────────────────────────
//
// ★ The beyond-Rust seam. Rust's borrow checker cannot distinguish `a[i]` from `a[j]` at all
// — both are the place `a[_]` — which is why `split_at_mut` must be written with `unsafe`
// and justified by a comment. The octagon already knows whether i and j differ, so the same
// question is a QUERY here rather than an axiom the programmer asserts.
//
// The state is per-block, so a query needs a POINT: `V->in[block]` replayed up to the
// instruction. Answering from the block ENTRY alone would miss everything established
// inside the block (`var i = 1; var j = 5; f(var a[i], var a[j])` assigns both there), and
// answering from the converged fixpoint of the whole function would be UNSOUND — a fact
// true at one point is not true at another.
typedef struct {
    Vra      *V;
    IrBlock  *blk;      // the block being examined
    IrInstr  *at;       // the instruction the query is about (replay stops BEFORE it)
    int64_t  *scratch;  // dsz doubles, reused across queries
} VraDisjoint;

// Are `a` and `b` PROVABLY different values at the current point? Conservative: false means
// "cannot prove", never "provably equal".
static bool vra_index_disjoint(void *vctx, IrValue *a, IrValue *b) {
    VraDisjoint *D = (VraDisjoint*)vctx;
    if (!D || !D->V || !D->blk || !a || !b) return false;
    Vra *V = D->V;
    if (a->id < 0 || a->id >= V->nvar || b->id < 0 || b->id >= V->nvar) return false;
    if (a->id == b->id) return false;                       // the same value is the same index
    if (!V->reached[D->blk->id] || !V->in[D->blk->id]) return false;
    // two known, differing constants need no octagon
    if (V->cknown[a->id] && V->cknown[b->id]) return V->cval[a->id] != V->cval[b->id];
    int dim = 2*V->noct;
    memcpy(D->scratch, V->in[D->blk->id], (size_t)V->dsz*8);
    const int *oct_map_saved = oct_map; oct_map = V->odim;   // queries run outside vra_analyze
    Octagon W = { V->noct, dim, D->scratch };
    oct_close(&W);
    for (IrInstr *ins = D->blk->instrs; ins && ins != D->at; ins = ins->next)
        vra_transfer_instr(V, &W, ins);
    oct_close(&W);
    // a − b ≤ −1  (a < b)   or   b − a ≤ −1  (b < a)
    bool disj = vra_diff_ub(V, &W, a->id, b->id) <= -1
             || vra_diff_ub(V, &W, b->id, a->id) <= -1;
    oct_map = oct_map_saved;
    return disj;
}

static VraDisjoint *vra_disjoint_open(IrFunc *f) {
    Vra *V = vra_analyze(f);
    if (!V) return NULL;
    VraDisjoint *D = calloc(1, sizeof *D);
    D->V = V; D->scratch = malloc((size_t)V->dsz*8);
    ir_place_index_disjoint_fn  = vra_index_disjoint;
    ir_place_index_disjoint_ctx = D;
    return D;
}
static void vra_disjoint_close(VraDisjoint *D) {
    ir_place_index_disjoint_fn  = NULL;
    ir_place_index_disjoint_ctx = NULL;
    if (!D) return;
    vra_free(D->V); free(D->scratch); free(D);
}

// ── 3.4: RECURSION TERMINATION ──────────────────────────────────────────────────────────
// A self-recursive function terminates if some parameter is a WELL-FOUNDED MEASURE: at every
// self-call the argument passed for it is provably SMALLER than the current value, and it is
// bounded below. Both halves are octagon questions, asked at the call site — the same shape
// as the loop measure (`vra_loop_terminates`), lifted from a back-edge to a call edge.
//
// Without this every recursive function is DIVERGE by the conservative cycle rule, so a
// perfectly well-founded recursion (binary search, tree walk) can never be a total `func`.
// Conservative: any self-call that does not shrink the candidate disqualifies it, and a
// function with no parameters or no self-call is not our business.
static bool vra_recursion_terminates(Vra *V, IrFunc *f) {
    if (!V || !f || !f->name) return false;
    int nparams = 0;
    for (IrParam *p=f->params; p; p=p->next) nparams++;
    if (nparams == 0 || nparams > 64) return false;
    // collect the self-calls
    bool any_self = false;
    for (IrBlock *b=f->blocks; b && !any_self; b=b->next)
        for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_CALL && i->aux.callee && f->name
                && i->aux.callee->length==f->name->length
                && memcmp(i->aux.callee->name, f->name->name, (size_t)f->name->length)==0) { any_self=true; break; }
    if (!any_self) return false;                       // not recursive: not this rule's job
    int dim = 2*V->noct;                       // the PACKED dimension — V->dsz is sized from it
    int64_t *scratch = malloc((size_t)V->dsz*8);
    if (!scratch) return false;
    // Runs outside vra_analyze (effects.h asks it), so it must install the packing itself.
    const int *oct_map_saved = oct_map; oct_map = V->odim;
    // try each parameter as the measure
    int k = 0;
    for (IrParam *p=f->params; p; p=p->next, k++) {
        IrValue *pv = p->value;
        if (!pv || !pv->type || pv->type->kind != IRT_INT) continue;
        if (pv->id < 0 || pv->id >= V->nvar) continue;
        bool ok = true;
        for (IrBlock *b=f->blocks; b && ok; b=b->next) {
            if (!V->reached[b->id] || !V->in[b->id]) continue;
            memcpy(scratch, V->in[b->id], (size_t)V->dsz*8);
            Octagon W = { V->noct, dim, scratch };
            oct_close(&W);
            for (IrInstr *i=b->instrs; i && ok; i=i->next) {
                bool self = (i->op==IR_CALL && i->aux.callee && f->name
                             && i->aux.callee->length==f->name->length
                             && memcmp(i->aux.callee->name, f->name->name, (size_t)f->name->length)==0);
                if (self) {
                    if (k >= i->n_operands) { ok = false; break; }
                    IrValue *arg = i->operands[k];
                    if (!arg || arg->id<0 || arg->id>=V->nvar) { ok = false; break; }
                    // arg < pv  (arg − pv ≤ −1)   AND   pv ≥ 0 (well-founded below)
                    bool shrinks = vra_diff_ub(V, &W, arg->id, pv->id) <= -1;
                    int64_t lo,hi; bool hl,hh; vra_interval(V, &W, pv->id, &lo,&hl,&hi,&hh);
                    bool grounded = (hl && lo >= 0) ||
                                    (pv->type->kind==IRT_INT && !pv->type->is_signed);
                    if (!(shrinks && grounded)) { ok = false; break; }
                }
                vra_transfer_instr(V, &W, i);
            }
        }
        if (ok) { free(scratch); oct_map = oct_map_saved; return true; }  // this param is a measure
    }
    free(scratch);
    oct_map = oct_map_saved;
    return false;
}

// ── STATE INSTRUMENT ─────────────────────────────────────────────────────────────────────
// Print the CONVERGED in-state of every block. Three precision questions in a row had been
// answered by bisecting programs — shrinking a failing case until it passed — instead of by
// reading the abstract state, which is slower and only ever localises to a shape, never to a
// link. This prints what the domain actually holds: each tracked value's interval, and every
// difference it knows that is not ⊤.
//
// Names come from IrValue.src_name where lowering recorded one. They are DIAGNOSTIC only —
// the analysis is keyed on ids, and that is the whole point of the rebuild.
static void vra_dump_val(Vra *V, int id, FILE *o) {
    IrValue *v = (id>=0 && id<V->nvar) ? V->val[id] : NULL;
    if (v && v->src_name) fprintf(o, "%%%d:%.*s", id, (int)v->src_name->length, v->src_name->name);
    else                  fprintf(o, "%%%d", id);
}
static void vra_dump_state(Vra *V, FILE *o) {
    int dim = 2*V->noct;
    int64_t *m = malloc((size_t)V->dsz*8);
    if (!m) return;
    fprintf(o, "── octagon state: %.*s ──\n",
            V->f->name?(int)V->f->name->length:1, V->f->name?V->f->name->name:"?");
    for (IrBlock *b=V->f->blocks; b; b=b->next) {
        if (!V->reached[b->id]) { fprintf(o, "  bb%d: UNREACHED\n", b->id); continue; }
        memcpy(m, V->in[b->id], (size_t)V->dsz*8);
        Octagon W = { V->noct, dim, m };
        oct_close(&W);
        fprintf(o, "  bb%d%s%s\n", b->id, b->is_loop_header ? "  [loop header]" : "",
                oct_is_bottom(&W) ? "  BOTTOM" : "");
        if (oct_is_bottom(&W)) continue;
        for (int id=0; id<V->nvar; id++) {
            if (V->odim[id] < 0) continue;                 // packed out: no dimension
            int64_t lo,hi; bool hl,hh;
            vra_interval(V,&W,id,&lo,&hl,&hi,&hh);
            if (!hl && !hh) continue;                      // ⊤ — nothing to say
            fputs("      ", o); vra_dump_val(V,id,o);
            fputs(" ∈ [", o);
            if (hl) fprintf(o, "%lld", (long long)lo); else fputs("-inf", o);
            fputs(", ", o);
            if (hh) fprintf(o, "%lld", (long long)hi); else fputs("+inf", o);
            fputs("]\n", o);
        }
        for (int a=0; a<V->nvar; a++) {
            if (V->odim[a] < 0) continue;
            for (int c=0; c<V->nvar; c++) {
                if (c==a || V->odim[c] < 0) continue;
                int64_t d = oct_get(&W, oct_pos(c), oct_pos(a));   // a − c ≤ d
                if (d >= OCT_INF) continue;
                fputs("      ", o); vra_dump_val(V,a,o); fputs(" − ", o); vra_dump_val(V,c,o);
                fprintf(o, " ≤ %lld\n", (long long)d);
            }
        }
        // ...and then the block's own instructions, each with what its RESULT is known to be
        // afterwards. The entry state alone cannot answer "where was this relation lost?" —
        // every value defined inside the block is ⊤ there by construction.
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            vra_transfer_instr(V,&W,ins);
            if (!ins->result || ins->result->id<0 || ins->result->id>=V->nvar) continue;
            int rid = ins->result->id;
            if (V->odim[rid] < 0) { fprintf(o, "    · "); vra_dump_val(V,rid,o);
                                    fputs(" = <packed out>\n", o); continue; }
            oct_close(&W);
            int64_t lo,hi; bool hl,hh; vra_interval(V,&W,rid,&lo,&hl,&hi,&hh);
            fputs("    · ", o); vra_dump_val(V,rid,o); fputs(" ∈ [", o);
            if (hl) fprintf(o,"%lld",(long long)lo); else fputs("-inf",o);
            fputs(", ", o);
            if (hh) fprintf(o,"%lld",(long long)hi); else fputs("+inf",o);
            fputs("]", o);
            for (int c=0; c<V->nvar; c++) {
                if (c==rid || V->odim[c] < 0) continue;
                int64_t d = oct_get(&W, oct_pos(c), oct_pos(rid));
                if (d >= OCT_INF) continue;
                fputs("   ", o); vra_dump_val(V,rid,o); fputs("−", o); vra_dump_val(V,c,o);
                fprintf(o, "≤%lld", (long long)d);
            }
            fputc('\n', o);
        }
    }
    free(m);
}

static void vra_free(Vra *V){
    if(!V) return;
    for(int i=0;i<V->f->next_block_id;i++) free(V->in[i]);
    free(V->in); free(V->reached); free(V->def); free(V->defblk); free(V->cval); free(V->cknown);
    free(V->slicelen); free(V->cellcanon); free(V->val); free(V->subslice_gep); free(V->escaped); free(V->persist); free(V->shape_rank); free(V->shape_ext); free(V->odim); free(V->checks); free(V);
}

#endif // LAIN_VRA_H
