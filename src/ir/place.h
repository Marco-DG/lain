// src/ir/place.h — PLACES: rooted access paths over the IR memory model (Stage II, B5-real).
//
// A place names a piece of storage: a base (local / param / static / unknown-deref) plus a
// chain of projections (.field, [index], *). It is the shared substrate for every
// memory-oriented analysis:
//   • definite assignment (3.0) — per-place init state
//   • borrow checking (3.1)     — loans are taken ON places; conflicts are place OVERLAPS
//   • linearity                 — a move consumes a place
// Building it once means those three stop being three bespoke pointer-chasing passes.
//
// LANGUAGE-NEUTRAL by construction (semantic-independence litmus): a place is Rust MIR's
// Place, C's lvalue, and Fortran's designator. Nothing here encodes Lain policy.
//
// The key operation is `ir_place_overlaps(a,b)` — used by the conflict rule. It is
// CONSERVATIVE by default (unknown ⇒ assume overlap) and gets sharper in two ways:
//   1. distinct FIELDS of the same base are provably disjoint (structural, done here);
//   2. distinct INDICES need arithmetic — constants are handled here, and the general case
//      is delegated through `ir_place_index_disjoint_fn`, which the VRA fills in
//      (Stage III-D). That hook is how we beat Rust, which cannot distinguish a[i] from a[j].
#ifndef LAIN_IR_PLACE_H
#define LAIN_IR_PLACE_H

#include "ir.h"

#define IR_PLACE_MAX_PROJ 8

typedef enum {
    IRPB_INVALID = 0,
    IRPB_LOCAL,     // an alloca in THIS function (dies at return)
    IRPB_PARAM,     // a parameter value (the caller's storage)
    IRPB_DEREF,     // through a pointer whose root we could not resolve
} IrPlaceBaseKind;

typedef enum { IRPJ_FIELD, IRPJ_INDEX, IRPJ_DEREF } IrProjKind;

typedef struct {
    IrProjKind kind;
    int32_t    field;     // IRPJ_FIELD
    IrValue   *index;     // IRPJ_INDEX (NULL if not a tracked value)
    // ★ A SLICE IS A RANGE, NOT A POINT, and conflating the two was a MISCOMPILE. `a[2..6]`
    // lowers to `make_slice(elem_ptr a, 2)`, and the walk below skipped the make_slice as "same
    // storage, no projection" — so the place was "element 2" and `a[0..4]` was "element 0". Two
    // distinct constant indices are PROVABLY disjoint, so the co-argument check saw no conflict,
    // accepted `vadd(4, var a[2..6], a[0..4])`, and emitted both parameters `restrict`. gcc then
    // printed `4 6 9 12` at -O0 and `4 6 8 10` at -O2.
    //
    // The front end's deleted E087 had this right (`sema_arg_const_span`, disjoint iff
    // `ihi <= jlo || jhi <= ilo`); the span reasoning was lost when the check moved to the IR.
    // Resolved to constants HERE, where the def table is in scope, so the overlap predicate stays
    // a pure function of two places.
    int64_t    off;       // IRPJ_INDEX: the index, when constant
    bool       off_known;
    int64_t    span;      // IRPJ_INDEX: element count covered (a slice's length), when constant
    bool       span_known;
    // Did this index come from a SLICE? Then it covers a range whatever the length turns out to
    // be, and that is the fail-closed half: `ranged` alone forces may-overlap, and `span_known`
    // is only the precision that lets two LITERAL sub-slices still be proven apart. Keeping them
    // separate matters because the length is usually not a bare constant — `a[2..6]` lowers the
    // length as `sub 6, 2`, so a version keyed on `span_known` alone changed nothing at all.
    bool       ranged;
} IrProj;

typedef struct {
    IrPlaceBaseKind base_kind;
    int32_t         base_id;                    // value id of the root
    IrProj          proj[IR_PLACE_MAX_PROJ];
    int             nproj;
    bool            valid;
} IrPlace;

// Optional bridge to the numeric domain: return true only if the two index values are
// PROVABLY different. Left NULL ⇒ conservative (assume they may be equal). The VRA installs
// a real implementation in Stage III-D; that is the "beyond Rust" seam.
typedef bool (*IrIndexDisjointFn)(void *ctx, IrValue *i, IrValue *j);
static IrIndexDisjointFn ir_place_index_disjoint_fn = NULL;
static void             *ir_place_index_disjoint_ctx = NULL;

// Canonicalise the place denoted by an ADDRESS value, walking the address-forming chain
// back to its root. `def` maps value id → defining instruction (as the analyses build it).
// Read an integer constant out of a value's defining instruction, folding one level of constant
// add/sub — which is not a luxury: a slice's length is emitted as `sub hi, lo`, so a reader that
// only accepted IR_CONST saw no constant length anywhere in the corpus.
static bool ir_place_const_of(IrInstr **def, int nvar, IrValue *v, int64_t *out) {
    if (!v || v->id < 0 || v->id >= nvar) return false;
    IrInstr *d = def[v->id];
    if (!d) return false;
    if (d->op == IR_CONST) { *out = d->aux.imm; return true; }
    if ((d->op == IR_ADD || d->op == IR_SUB) && d->n_operands >= 2) {
        int64_t l, r;
        if (ir_place_const_of(def, nvar, d->operands[0], &l) &&
            ir_place_const_of(def, nvar, d->operands[1], &r)) {
            *out = (d->op == IR_ADD) ? (l + r) : (l - r);
            return true;
        }
    }
    return false;
}

static IrPlace ir_place_of(IrInstr **def, int nvar, IrValue *v) {
    IrPlace p; memset(&p, 0, sizeof p);
    IrProj rev[IR_PLACE_MAX_PROJ]; int nrev = 0;
    // The length of the innermost slice seen so far, to be attached to the index projection it
    // is a slice OF. `make_slice` is walked before the `elem_ptr` it wraps, so it is pending.
    IrValue *pending_len = NULL;
    for (int guard=0; v && v->id>=0 && v->id<nvar && guard<10000; guard++) {
        IrInstr *d = def[v->id];
        if (!d) { p.base_kind = IRPB_PARAM; p.base_id = v->id; p.valid = true; break; }
        if (d->op == IR_ALLOCA) { p.base_kind = IRPB_LOCAL; p.base_id = v->id; p.valid = true; break; }
        if (d->op == IR_FIELD_PTR && d->n_operands>=1) {
            if (nrev < IR_PLACE_MAX_PROJ) { rev[nrev].kind=IRPJ_FIELD; rev[nrev].field=d->aux.field_idx; rev[nrev].index=NULL; nrev++; }
            v = d->operands[0]; continue;
        }
        if (d->op == IR_ELEM_PTR && d->n_operands>=2) {
            if (nrev < IR_PLACE_MAX_PROJ) {
                rev[nrev].kind=IRPJ_INDEX; rev[nrev].field=0; rev[nrev].index=d->operands[1];
                rev[nrev].off_known  = ir_place_const_of(def, nvar, d->operands[1], &rev[nrev].off);
                rev[nrev].ranged     = (pending_len != NULL);
                rev[nrev].span_known = pending_len
                                     && ir_place_const_of(def, nvar, pending_len, &rev[nrev].span);
                nrev++;
            }
            pending_len = NULL;
            v = d->operands[0]; continue;
        }
        if ((d->op == IR_SLICE_DATA || d->op == IR_MAKE_SLICE) && d->n_operands>=1) {
            // Same storage, no projection of its own — but a slice COVERS A RANGE, and that
            // length belongs to the index projection underneath it.
            if (d->op == IR_MAKE_SLICE && d->n_operands >= 2) pending_len = d->operands[1];
            v = d->operands[0]; continue;
        }
        if (d->op == IR_LOAD && d->n_operands>=1) {              // through an unresolved pointer
            p.base_kind = IRPB_DEREF; p.base_id = v->id; p.valid = true; break;
        }
        p.base_kind = IRPB_DEREF; p.base_id = v->id; p.valid = true; break;   // call result, etc.
    }
    for (int i=0;i<nrev;i++) p.proj[i] = rev[nrev-1-i];          // reverse: outermost-first
    p.nproj = nrev;
    return p;
}

static bool ir_place_index_provably_disjoint(IrValue *a, IrValue *b) {
    if (!a || !b) return false;
    if (a->id == b->id) return false;                            // same value ⇒ same index
    if (ir_place_index_disjoint_fn) return ir_place_index_disjoint_fn(ir_place_index_disjoint_ctx, a, b);
    return false;                                                // conservative without the VRA
}

// Do these two places MAY-overlap? The conflict rule and definite-assignment both use this.
// Returns false ONLY when disjointness is established; unknown ⇒ true (conservative).
static bool ir_place_overlaps(const IrPlace *a, const IrPlace *b) {
    if (!a->valid || !b->valid) return true;
    // A deref of unknown provenance may alias anything.
    if (a->base_kind==IRPB_DEREF || b->base_kind==IRPB_DEREF) return true;
    // Two DISTINCT roots are distinct storage (an alloca cannot alias another alloca, and a
    // param pointer is the caller's storage — modelled as its own root).
    if (a->base_id != b->base_id) return false;
    // Same root: walk the common projection prefix looking for a proven split.
    int n = a->nproj < b->nproj ? a->nproj : b->nproj;
    for (int i=0;i<n;i++) {
        const IrProj *x=&a->proj[i], *y=&b->proj[i];
        if (x->kind != y->kind) return true;                     // shape mismatch ⇒ unknown
        if (x->kind==IRPJ_FIELD && x->field != y->field) return false;   // distinct fields: DISJOINT
        if (x->kind==IRPJ_INDEX) {
            // When either side spans a range, disjointness is a RANGE question and is claimed
            // only from constants on both sides: `[off, off+span)` apart. A span of 1 is the
            // single-element case, so one ranged side and one point side still compares right.
            if (x->ranged || y->ranged) {
                // A ranged side needs its LENGTH to claim disjointness. A non-ranged side is a
                // single element, span 1.
                bool xs_ok = x->ranged ? x->span_known : true;
                bool ys_ok = y->ranged ? y->span_known : true;
                if (x->off_known && y->off_known && xs_ok && ys_ok) {
                    int64_t xs = x->ranged ? x->span : 1;
                    int64_t ys = y->ranged ? y->span : 1;
                    if (xs > 0 && ys > 0 &&
                        (x->off + xs <= y->off || y->off + ys <= x->off)) return false;
                }
                continue;   // unknown offset or length ⇒ may overlap (fail-closed)
            }
            if (ir_place_index_provably_disjoint(x->index, y->index)) return false;
        }
    }
    return true;   // one is a prefix of the other (or all projections may coincide) ⇒ overlap
}

// Is `a` the same place as `b` (not merely overlapping)? Used for "the access is through
// this very loan" and for exact init-state updates.
static bool ir_place_equal(const IrPlace *a, const IrPlace *b) {
    if (!a->valid || !b->valid) return false;
    if (a->base_kind!=b->base_kind || a->base_id!=b->base_id || a->nproj!=b->nproj) return false;
    for (int i=0;i<a->nproj;i++) {
        if (a->proj[i].kind != b->proj[i].kind) return false;
        if (a->proj[i].kind==IRPJ_FIELD && a->proj[i].field != b->proj[i].field) return false;
        if (a->proj[i].kind==IRPJ_INDEX) {
            IrValue *x=a->proj[i].index, *y=b->proj[i].index;
            if (!x || !y || x->id != y->id) return false;
        }
    }
    return true;
}

#endif // LAIN_IR_PLACE_H
