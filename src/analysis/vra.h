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
#include "ir/layout.h"  // the sizes Lain decides (@sizeof of an enum, a [packed] struct, a u32)
#include <stdlib.h>
#include <string.h>

static IrFunc *vra_mod = NULL;   // module for callee lookup; NULL disables the query
// ★ RE-ENTRANCY GUARD for the mutual-cycle obligation. It is raised INSIDE `vra_analyze`, next to the
// self-call one, and ranking a cycle requires analysing the OTHER function in it — so the naive
// version recursed for ever and segfaulted on the first cycle it met. (It worked while the obligation
// lived in `report.h`, which runs after the analysis; moving it in so the driver could see the verdict
// is what exposed this.) While set, the inner analyses skip the obligation, which is exactly right:
// the cycle is one fact and one function raises it.
static bool vra_in_mutual_check = false;
// The same by-content lookup, but against a module passed in rather than the global — the
// mutual-cycle walk is handed its module explicitly so it does not depend on `vra_mod` being set.
static IrFunc *vra_find_in(IrFunc *mod, const IrName *n) {
    if (!n || !mod) return NULL;
    for (IrFunc *g=mod; g; g=g->next)
        if (g->name && g->name->length==n->length && memcmp(g->name->name,n->name,(size_t)n->length)==0)
            return g;
    return NULL;
}
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
    VLOSS_TERM,       // a TERMINATION obligation. Not a numeric-domain capability at all, so
                      // running the operand classifier over it produced noise: it walks the
                      // operands of a check whose subject is a LOOP, and everything landed in
                      // `other`. Termination fails for its own reasons (a measure that is not
                      // a counter, an update the recogniser cannot see, a decrease that only
                      // holds across a join), and those want their own survey, not a bucket in
                      // this one.
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
        case VLOSS_TERM:    return "termination";
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
    // A VRA_TERMINATION check about a RECURSION rather than a loop. The two are one
    // obligation — "this does not run forever" — proved by two different arguments (a loop
    // measure, a well-founded ranking over the self-call's arguments), and a user needs to be
    // told which one failed: the loop answer is E082 and the recursive one is E011.
    bool     recursion;
    bool     mutual;         // the recursion is a CYCLE through another function, not a self-call
    // Did the source write a `decreasing` clause? Annex B makes the CODE depend on it — E011
    // for a recursion with no inferable measure, E082 for one whose measure is present and
    // fails — so the engine has to carry the distinction to be normatively right.
    bool     had_measure;
    // A VRA_OVERFLOW check about a SHIFT: 1 = the amount is not provably in [0, width-1],
    // 2 = a signed left shift may carry a bit into/through the sign. Both are UB in the C the
    // backend emits, and each needs its own sentence.
    int      shift;
    bool     bitcount;       // a VRA_DIVZERO check on the argument of @ctz/@clz, not a divisor
    int      diag;           // VRA_PRECOND: the assert's diagnostic class (85/86/87; 0 = E012)
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
    // in[bid] is KNOWN CLOSED: it was stored from a closed state, or from the join of two
    // closed states, and not widened since. Loading it then needs no closure. False is the safe
    // value, and the only one a write that does not know sets (see Octagon.clean).
    bool    *inclosed;
    IrInstr **def;      // def[val id] = producing instruction (NULL for params)
    IrValue **val;      // val[val id] = the value itself (for its TYPE — see vra_range)
    int     *defblk;    // defblk[val id] = id of the block defining it (-1 = param)
    int64_t *cval; bool *cknown;   // constant values (from IR_CONST)
    bool    *modwrap;  // a MODULAR +,−,× that MAY wrap: its ℤ reading is false (vra_zexact)
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
    // ── ELEMENT RANGES: what an array's CONTENTS can be ──────────────────────────────────
    // The domain models INDICES and forgets what is behind them, so `a[0] + a[3]` over
    // `var a i32[4] = [10, 20, 30, 40]` was refused: both loads fell back to the i32 type
    // range and their sum leaves i32. Measured 2026-09-14 as 11 of the 65 programs blocking
    // the 3.5 switchover — the single largest ENGINE gap in that set.
    // Keyed by the ARRAY CELL (the alloca's value id); see vra_seed_element_ranges for the
    // conditions that make the join sound.
    int64_t *elem_lo, *elem_hi;
    bool    *elem_known;
    // Call-site return ranges, memoised per CALL RESULT. The query re-analyses the callee, and
    // the transfer function runs once per fixpoint SWEEP — computing it there cost an 8.5x
    // compile-time regression (207s against 24s over 80 programs) before this cache existed.
    // Sound to cache because the query only fires when every bound argument is an exact
    // CONSTANT, and a constant does not change between sweeps.
    int64_t *cret_lo, *cret_hi;
    signed char *cret_state;      // 0 = not asked, 1 = no answer, 2 = have one
    // Cells that are LOOP ACCUMULATORS, marked structurally in the prepass: some store to the
    // cell is an add/sub whose first operand loads that same cell. Purely syntactic and
    // therefore stable across sweeps, which is what keeps the B1 query off the hot path —
    // vra_range asks this before doing anything expensive.
    bool    *accum_cell;
    // For each ALLOCA slot: the value id of its ONLY store, or -1 when it is stored zero or
    // several times. Read by vra_ref_target — see there.
    int     *uniq_store;
    // strict_esc[cell]: the cell's address escaped by a route OTHER than a benign reference
    // binding (see vra_store_is_benign_ref). `escaped` stays the conservative union that every
    // havoc, loop rule and guard matcher reads; only the field-cell precondition reads this.
    bool    *strict_esc;
    bool     marking_benign;   // set while marking the value of a benign reference store
    VraCheck *checks; int nchecks, cap_checks;
} Vra;

// ★ DOES THIS OPERATION'S RESULT EQUAL ITS VALUE OVER ℤ? Every structural rule in this file —
// the midpoint identity, a loop's `i + 1` step, an accumulator's delta, a slice's `hi − lo`
// length — reads an `add`/`sub`/`mul` as integer arithmetic. A CHECK-mode operation earns that
// reading from its own overflow obligation. A MODULAR one (`+% −% *%`) has none, and where it
// wraps the reading is false: `i = i +% 1` under `while i <= n` was a +1 step toward n, so
// `spin(255)` on a u8 was PROVEN TERMINATING (and emitted `const`) while it looped forever.
// A modular operation qualifies only once the check pass has shown, at the converged state,
// that it cannot wrap; until then (and during the fixpoint) `modwrap` says it may.
static bool vra_zexact(const Vra *V, const IrInstr *d) {
    if (!d || d->wrap != IR_WRAP_MODULAR) return true;
    if (!d->result || d->result->id < 0 || d->result->id >= V->nvar || !V->modwrap) return false;
    return !V->modwrap[d->result->id];
}

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
        if (V->strict_esc && !V->marking_benign) V->strict_esc[v->id] = true;
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
            // ...and a STRUCT parameter's value is the base its field cells hang off
            // (vra_field_cell_base), so `l.pos = v` through `l var L` names that base. It was
            // UNKNOWN: every write to a field of a `var` struct parameter was treated as an
            // unattributable store — it havocked every escaped cell and never assigned the
            // field, so `l.pos = l.pos + 1` in a lexer taught the domain nothing and read as
            // an opaque writer to every loop rule. The memory is the caller's, reachable here
            // only through this parameter: a store rooted at it can write nothing else.
            bool sparam = v->type && v->type->kind==IRT_PTR && v->type->elem &&
                          v->type->elem->kind==IRT_STRUCT;
            return (vra_is_param_cell(V, v->id) || sparam) ? v->id
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

// The same PLACE reached twice is two different SSA values: `l.text` lowers to a fresh
// field_ptr at each mention. Keyed by value id, the length learned at one mention was
// invisible at the next — which is exactly what made the struct `in` invariant useless, since
// the assume names one load of `text` and the index check another. Canonicalise a field
// access to the first field_ptr with the same (base, field); everything else is its own.
static int vra_canon_cell(Vra *V, int v) { return (v>=0 && v<V->nvar && V->cellcanon) ? V->cellcanon[v] : v; }

// ── A FIELD OF A LOCAL STRUCT IS A SCALAR CELL ───────────────────────────────────────────
// `p.x` has a stable identity already: cellcanon unifies every field_ptr with the same base
// and index. What it did not have was a numeric one — the LOAD/STORE transfer required an
// ALLOCA, so a field_ptr fell to oct_forget and `var p = Point(3,4)` then `p.x + p.y` proved
// nothing at all, though every value in it is a literal.
//
// Returns the base alloca when `v` names such a field, else −1. The gate is deliberately
// narrow, and each clause is load-bearing:
//   · the base must be an ALLOCA of a struct — NOT an elem_ptr. `a[i].x` for a runtime `i`
//     would give each field_ptr its own identity while the memory aliases, which is a false
//     proof rather than a precision choice.
//   · the field must be a scalar. A nested struct field is an address, not a number.
//   · the base must not ESCAPE. A callee handed the struct's address can write any field with
//     no IR_STORE here to see — the same argument vra_seed_element_ranges makes for arrays.
// A store to the base ITSELF (a whole-struct assignment, `p = q`) is handled in the STORE
// transfer by forgetting the base's field cells: it writes them all with nothing here to see.
static void vra_forget_fields_of(Vra *V, Octagon *W, int base);   // fwd

static int vra_field_cell_base(Vra *V, int v) {
    IrInstr *d = (v>=0 && v<V->nvar && V->def) ? V->def[v] : NULL;
    if (!d || d->op != IR_FIELD_PTR || d->n_operands < 1) return -1;
    IrType *rt = d->result ? d->result->type : NULL;
    if (!rt || rt->kind != IRT_PTR || !rt->elem ||
        (rt->elem->kind != IRT_INT && rt->elem->kind != IRT_BOOL && rt->elem->kind != IRT_SLICE)) return -1;
    int base = d->operands[0]->id;
    IrInstr *bd = (base>=0 && base<V->nvar) ? V->def[base] : NULL;
    if (bd) {
        if (bd->op != IR_ALLOCA || !bd->aux.alloca_ty ||
            bd->aux.alloca_ty->kind != IRT_STRUCT) return -1;
        // strictly escaped — a reference binding's benign escape keeps the fields modelled
        if (V->strict_esc ? V->strict_esc[base] : (V->escaped && V->escaped[base])) return -1;
        return base;
    }
    // ★ ...OR A STRUCT PARAMETER. No defining instruction means a parameter, and a `var B`
    // one points at storage the caller owns — the same relationship a `var i32` parameter has,
    // which vra_is_param_cell already models as a cell. Without this, `a.room` at a guard and
    // `a.room` at its use were two independent unknown loads and no guard on a field could
    // settle anything: `if a.room > a.cap { return } ... a.cap - a.room` stayed an underflow.
    //
    // A `var` parameter is marked ESCAPED, which is what havocs it at a call — so field cells
    // on one are dropped at every call too (see the IR_CALL transfer). What is NOT re-derived
    // is aliasing between two `var` parameters of the same call: the language forbids that
    // (two mutable borrows of one value is E004, which is the same promise `restrict` carries
    // into the emitted C), and scripts/fuzz/fuzz_alias.sh is the harness that tests it.
    IrValue *bv = (base>=0 && base<V->nvar) ? V->val[base] : NULL;
    if (!bv || !bv->type || bv->type->kind != IRT_PTR || !bv->type->elem ||
        bv->type->elem->kind != IRT_STRUCT) return -1;
    return base;
}

// ★ A POINTER LOADED FROM A ONCE-WRITTEN SLOT NAMES THE CELL THAT WAS STORED THERE. A reference
// binding (`var r = var n`) lowers to a slot holding n's address, and every use of r loads that
// slot and goes through it — so to this domain every read of r was unknown and every write was
// an unattributable store that forgets all escaped cells. `r = 0; n + 1` could not prove what
// `n = 0; n + 1` proves. The slot is written exactly once, so the loaded pointer IS that
// address, and the access is an access to that cell: exact, not an approximation. The same rule
// vra_array_root already applies to arrays (and bor_unique_store_value to places).
//
// Only the LOAD/STORE transfer consults this. The loop rules scan stores SYNTACTICALLY, and
// they must keep seeing a write through a reference as the opaque writer it is to them
// (vra_cell_opaque_write) — resolving it there would hide a counter reset from the scan.
static int vra_ref_target(Vra *V, int addr) {
    if (addr < 0 || addr >= V->nvar || !V->uniq_store) return addr;
    IrInstr *d = V->def[addr];
    if (!d || d->op != IR_LOAD || d->n_operands < 1 || !d->operands[0]) return addr;
    int slot = d->operands[0]->id;
    if (slot < 0 || slot >= V->nvar || !V->def[slot] || V->def[slot]->op != IR_ALLOCA) return addr;
    int held = V->uniq_store[slot];
    if (held < 0 || held >= V->nvar) return addr;
    IrInstr *hd = V->def[held];
    if (hd && hd->op == IR_ALLOCA && hd->aux.alloca_ty &&
        (hd->aux.alloca_ty->kind == IRT_INT || hd->aux.alloca_ty->kind == IRT_BOOL)) return held;
    if (vra_is_param_cell(V, held)) return held;
    if (vra_field_cell_base(V, held) >= 0) return held;     // a field of a modelled struct
    return addr;
}

// Every field cell of `base` becomes unknown. Used wherever the whole struct is written with
// no per-field IR_STORE to see: a call that may write through an escaped address, an opaque.
static void vra_forget_fields_of(Vra *V, Octagon *W, int base) {
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *q=b->instrs; q; q=q->next)
            if (q->op==IR_FIELD_PTR && q->result && q->n_operands>=1 &&
                q->operands[0]->id == base) oct_forget(W, vra_canon_cell(V, q->result->id));
}
// pre-pass: def sites, constants, and canonical slice-length vars. A slice's length
// var is its make_slice length operand or its first slice_len read; it is propagated
// through a slice local's store/load so `s = a[lo..hi]; s[k]` knows len(s).
// ── ELEMENT RANGES ───────────────────────────────────────────────────────────────────────
// Follow an address back to the ARRAY CELL it indexes, through the address-forming ops.
// Stops at anything that launders provenance, which is the conservative direction.
static int vra_array_root(Vra *V, int addr) {
    // 24 hops: a double subslice (`s = a[0..4]; t = s[2..4]`) is already 8 — make_slice,
    // elem_ptr, slice_data and a load-through-slot for each level — and the walk is linear.
    for (int hop=0; hop<24 && addr>=0 && addr<V->nvar; hop++) {
        IrInstr *d = V->def[addr];
        if (!d) return -1;                                   // a parameter: not a local array
        if (d->op == IR_ALLOCA)
            return (d->aux.alloca_ty && d->aux.alloca_ty->kind==IRT_ARRAY) ? addr : -1;
        if (d->op==IR_ELEM_PTR || d->op==IR_FIELD_PTR || d->op==IR_SLICE_DATA ||
            d->op==IR_MAKE_SLICE || d->op==IR_CAST) {
            if (d->n_operands < 1) return -1;
            addr = d->operands[0]->id; continue;
        }
        // A LOAD launders provenance in general — but when the slot it reads is written
        // EXACTLY ONCE, the loaded value IS that stored value and provenance survives. Without
        // this, `var s = xs[1..4]` defeats the walk: the slice is stored into a slot and read
        // back, so `s[0]` reaches a LOAD and the array behind it is invisible. Same rule the
        // borrow checker uses to see an escaping local (bor_unique_store_value).
        if (d->op==IR_LOAD && d->n_operands >= 1) {
            int slot = d->operands[0]->id;
            IrValue *only = NULL; int nst = 0;
            for (IrBlock *b=V->f->blocks; b; b=b->next)
                for (IrInstr *i=b->instrs; i; i=i->next)
                    if (i->op==IR_STORE && i->n_operands>=2 && i->operands[0]->id==slot) {
                        only = i->operands[1]; nst++;
                    }
            if (nst != 1 || !only) return -1;
            addr = only->id; continue;
        }
        return -1;
    }
    return -1;
}

// What can an element of this array BE? The join of every value stored into it — sound only
// when three things hold together, and each one is load-bearing:
//
//   · every store to the cell has a value we know exactly (the constant table). One unknown
//     store and the join says nothing, so the whole cell drops out.
//   · the constant store indices COVER [0, len). Without this a load could read an element
//     nothing wrote, and the join would describe values that element never held. Covering
//     also means the result does not depend on the definite-init pass being right — the
//     proof stands on this function's own evidence.
//   · the cell does not ESCAPE. A callee handed the address can store anything, with no
//     IR_STORE here to see — the same hole that produced three false proofs on 2026-09-09.
//
// A store at an UNKNOWN index with a known value is fine: coverage still holds from the
// constant stores, and the unknown-index value joins in like any other.
// ★ THE SECOND SEEDING MODE (C14). When this is set, a store whose value is neither a
// constant nor a load from a known cell is no longer an automatic give-up: its range is read
// off the CONVERGED octagon at that store. `var a i32[4]` filled by `while i < 4 { a[i] = i }`
// has all its values known — to the fixpoint, which runs AFTER this seeding did. Three corpus
// programs were pinned to `--engine=legacy` for exactly this.
//
// ⚠ SOUNDNESS, AND THE HAZARD IS SEEDING FROM YOUR OWN CONCLUSION. The refined range must be
// validated against a fixpoint that did NOT use it, or the two justify each other in a circle.
// So vra_analyze runs the fixpoint TWICE and no more: pass 0 with constants-and-copies only,
// then this refinement against pass 0's converged state — a true statement about the program,
// because pass 0 is a sound over-approximation — then pass 1 using it. Element ranges are
// never re-derived from pass 1.
//
// Everything else — coverage, escape, the any-call forfeit — is the same code and the same
// conditions as before, deliberately: this widens WHERE a value's range comes from, not which
// cells are eligible to have one.
static void vra_range(Vra *V, Octagon *W, IrValue *v, int64_t *lo, int64_t *hi);      // fwd
static void vra_transfer_instr(Vra *V, Octagon *W, IrInstr *ins);                     // fwd

static bool vra_seed_from_state = false;
static int  vra_seed_dim = 0;

static void vra_seed_element_ranges_round(Vra *V) {
    int n = V->nvar;
    int64_t *lo = malloc((size_t)n*sizeof(int64_t)), *hi = malloc((size_t)n*sizeof(int64_t));
    bool *ok = calloc((size_t)n, sizeof(bool)), *seen = calloc((size_t)n, sizeof(bool));
    unsigned char *cov = NULL;
    if (!lo || !hi || !ok || !seen) { free(lo); free(hi); free(ok); free(seen); return; }
    for (int i=0;i<n;i++) { ok[i]=true; lo[i]=INT64_MAX; hi[i]=INT64_MIN; }

    // pass 1: join the stored values per cell, and note which cells have an unknown store
    int64_t *Wm = NULL; Octagon Wv = {0,0,NULL};
    if (vra_seed_from_state && vra_seed_dim > 0) {
        Wm = malloc((size_t)V->dsz*8);
        Wv.nvar = V->noct; Wv.dim = vra_seed_dim; Wv.m = Wm;
    }
    for (IrBlock *b=V->f->blocks; b; b=b->next) {
        bool replay = Wm && V->reached && V->reached[b->id] && V->in && V->in[b->id];
        if (replay) { memcpy(Wm, V->in[b->id], (size_t)V->dsz*8); oct_set_clean(&Wv, V->inclosed && V->inclosed[b->id]); oct_close(&Wv); }
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (replay && ins->op != IR_STORE) vra_transfer_instr(V, &Wv, ins);
            if (ins->op != IR_STORE || ins->n_operands < 2) {
                continue;
            }
            int cell = vra_array_root(V, ins->operands[0]->id);
            if (cell < 0) continue;
            seen[cell] = true;
            int sv = ins->operands[1]->id;
            if (sv<0 || sv>=n) { ok[cell] = false; continue; }
            int64_t vlo, vhi;
            if (V->cknown[sv]) { vlo = vhi = V->cval[sv]; }
            else {
                // ★ A COPY IS NOT AN UNKNOWN STORE. `var a i32[4] = b` lowers to an
                // element-wise `store(a[q], load(b[q]))`, so every stored value is a LOAD and
                // the constant test rejected all of them — the copy lost what the SOURCE was
                // known to hold, while reading `b` directly still proved. The loaded value is
                // one of the source cell's values, so the source's own element join bounds it.
                // Sound because that join is over every store to the source anywhere in the
                // function, and a cell that escapes has already been dropped below.
                IrInstr *sd = V->def[sv];
                int scell = (sd && sd->op==IR_LOAD && sd->n_operands>=1)
                          ? vra_array_root(V, sd->operands[0]->id) : -1;
                if (scell < 0 || scell >= n || scell == cell || !V->elem_known[scell]) {
                    // C14: ask the converged state what this value can be.
                    int64_t rlo, rhi;
                    if (!replay || !V->val[sv]) { ok[cell] = false; continue; }
                    vra_range(V, &Wv, V->val[sv], &rlo, &rhi);
                    if (rlo > rhi) { ok[cell] = false; continue; }
                    vlo = rlo; vhi = rhi;
                } else {
                    vlo = V->elem_lo[scell]; vhi = V->elem_hi[scell];
                }
            }
            if (vlo < lo[cell]) lo[cell] = vlo;
            if (vhi > hi[cell]) hi[cell] = vhi;
            if (replay) vra_transfer_instr(V, &Wv, ins);   // the store itself, after reading it
        }
    }
    free(Wm);

    // pass 2: coverage — every index in [0,len) written by a CONSTANT-index store
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op != IR_ALLOCA || !ins->result) continue;
            int cell = ins->result->id;
            if (cell<0 || cell>=n || !seen[cell] || !ok[cell]) continue;
            IrType *at = ins->aux.alloca_ty;
            if (!at || at->kind!=IRT_ARRAY || at->array_len<=0 || at->array_len>4096) { ok[cell]=false; continue; }
            // ★ AN ESCAPE ONLY MATTERS IF SOMETHING CAN EXERCISE IT. Making a slice of a local
            // array marks the array escaped — `var s = xs[1..4]` stores an address — which
            // dropped the element range for the whole subslice family even when nothing else
            // in the function ever runs. A callee is the only thing that can write through an
            // escaped address without an IR_STORE here to see, so with NO CALL in the function
            // the escape cannot be taken up by anyone and the stores below are the complete
            // set of writers.
            //
            // Deliberately coarse: any call at all forfeits the range, rather than asking the
            // write footprint which cells a particular callee touches. `zero(var s)` really
            // does rewrite the array through the slice, and the join from before the call is
            // then stale — that case must keep failing, and does.
            //
            // ...and a store through an UNATTRIBUTABLE pointer is the other thing — a reference
            // binding (`var r = var xs[1]; r = 100`) or a raw pointer writes the array with an
            // IR_STORE whose address does not root at the cell, so the scan below never counts it.
            if (V->escaped && V->escaped[cell]) {
                bool anycall = false;
                for (IrBlock *bc=V->f->blocks; bc && !anycall; bc=bc->next)
                    for (IrInstr *ic=bc->instrs; ic; ic=ic->next)
                        if (ic->op==IR_CALL || (ic->op==IR_STORE && ic->n_operands>=2 &&
                            vra_arg_cell(V, ic->operands[0]) == VRA_ARG_UNKNOWN)) { anycall = true; break; }
                if (anycall) { ok[cell]=false; continue; }
            }
            int len = (int)at->array_len;
            // An IR_INIT fact on this cell IS coverage — it is lowering stating that every
            // element is written (a comprehension, or a loop that fills 0..len). That is the
            // same fact the constant-index scan below reconstructs, declared instead of
            // inferred, and it is what the definite-init pass already trusts.
            bool covered_by_fact = false;
            for (IrBlock *bf=V->f->blocks; bf && !covered_by_fact; bf=bf->next)
                for (IrInstr *fi=bf->instrs; fi; fi=fi->next)
                    if (fi->op==IR_INIT && fi->n_operands>=1 &&
                        vra_array_root(V, fi->operands[0]->id)==cell) { covered_by_fact=true; break; }
            if (covered_by_fact) continue;              // ok[cell] stays true
            cov = calloc((size_t)len, 1);
            if (!cov) { ok[cell]=false; continue; }
            for (IrBlock *b2=V->f->blocks; b2; b2=b2->next)
                for (IrInstr *s2=b2->instrs; s2; s2=s2->next) {
                    if (s2->op != IR_STORE || s2->n_operands < 2) continue;
                    if (vra_array_root(V, s2->operands[0]->id) != cell) continue;
                    IrInstr *ad = V->def[s2->operands[0]->id];
                    if (!ad || ad->op != IR_ELEM_PTR || ad->n_operands < 2) continue;
                    int ix = ad->operands[1]->id;
                    if (ix>=0 && ix<n && V->cknown[ix] &&
                        V->cval[ix]>=0 && V->cval[ix]<len) cov[V->cval[ix]] = 1;
                }
            for (int k=0;k<len;k++) if (!cov[k]) { ok[cell]=false; break; }
            free(cov); cov=NULL;
        }

    for (int i=0;i<n;i++)
        if (seen[i] && ok[i] && lo[i] <= hi[i]) {
            V->elem_known[i]=true; V->elem_lo[i]=lo[i]; V->elem_hi[i]=hi[i];
        }
    // Structural marking of accumulator cells — see the field's comment.
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op != IR_STORE || ins->n_operands < 2) continue;
            int cell = ins->operands[0]->id;
            if (cell < 0 || cell >= n) continue;
            IrInstr *vd = V->def[ins->operands[1]->id];
            if (!vd || (vd->op != IR_ADD && vd->op != IR_SUB) || vd->n_operands < 1) continue;
            IrInstr *ld = V->def[vd->operands[0]->id];
            if (ld && ld->op == IR_LOAD && ld->n_operands >= 1 && ld->operands[0]->id == cell)
                V->accum_cell[cell] = true;
        }
    free(lo); free(hi); free(ok); free(seen);
}

// A round can only resolve a copy whose SOURCE is already known, so `a = b; c = a` needs two.
// Three rounds, and each one only ever ADDS cells (a cell that fails stays failed within the
// round and is recomputed from scratch in the next), so this converges and is bounded. Chains
// longer than two copies simply do not get the range — a limit, not an unsoundness.
static void vra_seed_element_ranges(Vra *V) {
    // A module constant TABLE (IR_ALLOCA naming read-only static data) is never stored to:
    // its elements are the data's, known before any store is joined, so a copy of the table
    // can take its range in the rounds below as well.
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next)
            if (ins->op == IR_ALLOCA && ins->data && ins->result &&
                ins->result->id >= 0 && ins->result->id < V->nvar) {
                int cell = ins->result->id;
                V->elem_known[cell] = true;
                V->elem_lo[cell] = ins->data->lo;
                V->elem_hi[cell] = ins->data->hi;
            }
    for (int round = 0; round < 3; round++) vra_seed_element_ranges_round(V);
}

// ★ A REFERENCE BINDING'S ESCAPE IS NOT AN ESCAPE TO THE FIELD MODEL. `var x = var p.x` stores
// p.x's address into x's slot, which marks p escaped — and an escaped struct has no field cells,
// so every read of p.x (and every read through x) was unknown: `x = 7; y = x + 1` was E086. The
// address went nowhere but a slot that is written ONCE and whose every load is used only as the
// address of a direct LOAD or STORE — and each of those is resolved to the field cell exactly
// (vra_ref_target). Such a store is BENIGN: nothing can reach p.x except through accesses this
// domain sees. Anything else — the loaded pointer passed to a call, stored, returned, projected
// further, consumed — and it is an ordinary escape.
//
// Only the FIELD-CELL precondition is relaxed. `escaped` is still set, so every havoc, loop
// rule and guard matcher keyed on it stays exactly as conservative as before.
static bool vra_store_is_benign_ref(Vra *V, IrInstr *st) {
    if (!st || st->op != IR_STORE || st->n_operands < 2 || !V->uniq_store) return false;
    IrValue *slot = st->operands[0], *held = st->operands[1];
    if (!slot || !held || slot->id < 0 || slot->id >= V->nvar) return false;
    IrInstr *sd = V->def[slot->id];
    IrType *at = (sd && sd->op == IR_ALLOCA) ? sd->aux.alloca_ty : NULL;
    if (!at || at->kind != IRT_PTR || !at->borrowed) return false;       // a reference slot
    if (V->uniq_store[slot->id] != held->id) return false;               // written exactly once
    // every use of the slot is this store or a LOAD of it; every such load is used only as the
    // ADDRESS operand of a load or a store
    for (IrBlock *b = V->f->blocks; b; b = b->next) {
        for (IrInstr *i = b->instrs; i; i = i->next) {
            for (int k = 0; k < i->n_operands; k++) {
                IrValue *o = i->operands[k];
                if (!o) continue;
                if (o == slot) {
                    if (i == st && k == 0) continue;
                    if (i->op == IR_LOAD && k == 0) continue;
                    return false;
                }
                IrInstr *od = (o->id >= 0 && o->id < V->nvar) ? V->def[o->id] : NULL;
                if (od && od->op == IR_LOAD && od->n_operands >= 1 && od->operands[0] == slot) {
                    bool addr_use = (k == 0) && (i->op == IR_LOAD || i->op == IR_STORE);
                    if (!addr_use) return false;
                }
            }
        }
        IrValue *tc = b->term.cond;                       // a returned / branched-on pointer
        IrInstr *td = (tc && tc->id >= 0 && tc->id < V->nvar) ? V->def[tc->id] : NULL;
        if (td && td->op == IR_LOAD && td->n_operands >= 1 && td->operands[0] == slot) return false;
    }
    return true;
}

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
    // A NEGATED CONSTANT is a constant. A negative literal is `-` applied to a positive one, and
    // left as an octagon interval it was exact only while it fit the octagon's usable range
    // (about 2^60: OCT_INF is INT64_MAX/4 and bounds are stored doubled) — so
    // `var x i63 = -2875414689009298395` could not be shown to fit an i63, though it is a
    // literal. The constant table has no such limit. Block order defines before it uses.
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op==IR_NEG && ins->result && ins->n_operands>=1 && ins->operands[0] &&
                ins->operands[0]->id>=0 && ins->operands[0]->id<V->nvar &&
                V->cknown[ins->operands[0]->id] && V->cval[ins->operands[0]->id] != INT64_MIN) {
                V->cknown[ins->result->id] = true;
                V->cval[ins->result->id] = -V->cval[ins->operands[0]->id];
            }
            if ((ins->op==IR_ADD || ins->op==IR_SUB || ins->op==IR_MUL) &&
                ins->wrap==IR_WRAP_MODULAR && ins->result && ins->result->id>=0 && ins->result->id<V->nvar)
                V->modwrap[ins->result->id] = true;
            // ...and so is `+ − ×` of two constants, which is how i64::MIN has to be written
            // (`-9223372036854775807 - 1`: there is no literal for it). Its value is outside the
            // octagon's usable range, so as an interval it read as UNKNOWN — `x /% MIN` was
            // "divisor not provably non-zero". Folded where the value is the operation's: a
            // checked result that fits its type (one that does not fails its own obligation), or
            // a modular one reduced to the type's width. A u64 above i64::MAX has no cval.
            if ((ins->op==IR_ADD || ins->op==IR_SUB || ins->op==IR_MUL) && ins->n_operands>=2 &&
                ins->result && ins->result->type && ins->result->type->kind==IRT_INT &&
                ins->result->type->bits>=1 && ins->result->type->bits<=64 &&
                ins->operands[0] && ins->operands[1] &&
                ins->operands[0]->id>=0 && ins->operands[0]->id<V->nvar && V->cknown[ins->operands[0]->id] &&
                ins->operands[1]->id>=0 && ins->operands[1]->id<V->nvar && V->cknown[ins->operands[1]->id]) {
                __int128 x = V->cval[ins->operands[0]->id], y = V->cval[ins->operands[1]->id];
                __int128 z = ins->op==IR_ADD ? x + y : ins->op==IR_SUB ? x - y : x * y;
                IrType *t = ins->result->type; int nb = t->bits;
                __int128 tlo = t->is_signed ? -((__int128)1 << (nb-1)) : 0;
                __int128 thi = t->is_signed ? ((__int128)1 << (nb-1)) - 1 : ((__int128)1 << nb) - 1;
                bool ok = (z >= tlo && z <= thi);
                if (!ok && ins->wrap == IR_WRAP_MODULAR) {
                    unsigned __int128 m = ((unsigned __int128)1 << nb) - 1, u = (unsigned __int128)z & m;
                    z = (t->is_signed && (u >> (nb-1)) & 1) ? (__int128)u - ((__int128)1 << nb) : (__int128)u;
                    ok = true;
                }
                if (ok && z >= INT64_MIN && z <= INT64_MAX) {
                    V->cknown[ins->result->id] = true; V->cval[ins->result->id] = (int64_t)z;
                }
            }
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
    // ESCAPE AND PERSISTENCE FIRST. The slice-length rules below ask whether anything but a store
    // can write a slice FIELD's base — a call through an escaped or persisting address — so they
    // need these sets, and nothing here needs anything the slice-length section computes.
    // A `var` scalar parameter points at storage the CALLER owns, so anything we hand the
    // pointer to may write it: it havocs at a call exactly like an escaped alloca.
    // ...and so does a `var` STRUCT parameter, now that its FIELDS are cells. It was marked
    // neither escaped nor persisting, because before field cells it carried no numeric fact
    // worth havocing — a pointer to a struct has no range. With `a.room` tracked, skipping it
    // meant a callee could set a field out of range and the caller kept the value from before
    // the call. Same storage relationship as the scalar case, so the same treatment.
    for (IrParam *p=V->f->params; p; p=p->next)
        if (p->value && (vra_is_param_cell(V, p->value->id) ||
                         (p->value->type && p->value->type->kind==IRT_PTR &&
                          p->value->type->elem && p->value->type->elem->kind==IRT_STRUCT))) {
            V->escaped[p->value->id] = true;
            if (V->strict_esc) V->strict_esc[p->value->id] = true;
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
                    bool keeps = k>=64 || ((cr>>k)&1u);
                    // An address handed to a RESOLVED call that does not keep it is benign for
                    // the field cells: the call transfer forgets a base's fields whenever the
                    // callee writes it (or cannot be seen), so there is no write here that a
                    // field cell could miss. `peek(l)` after `l.pos = l.pos + 1` no longer
                    // un-models `l.pos` for the whole function.
                    V->marking_benign = !keeps && ins->op == IR_CALL && callee != NULL;
                    vra_mark_escape(V, ins->operands[k]);
                    V->marking_benign = false;
                    if (keeps) vra_mark_persist(V, ins->operands[k]);
                }
            } else if (ins->op == IR_STORE && ins->n_operands>=2) {
                V->marking_benign = vra_store_is_benign_ref(V, ins);
                vra_mark_persist(V, ins->operands[1]);   // an address stored into memory PERSISTS
                V->marking_benign = false;
            }
        }
        if (b->term.kind == IR_TERM_RET) vra_mark_persist(V, b->term.cond);   // outlives us
    }
    // ★ A SLICE'S LENGTH IS FLOW-SENSITIVE: its own octagon dimension. Every slice VALUE, slice
    // PARAMETER and slice CELL (an alloca or a struct field) has one, holding the length — set by
    // make_slice, copied by a store into a cell and by a load out of one, read by slice_len —
    // exactly the transfer shape a scalar cell has for its value. This replaced a prepass that
    // gave each slice cell ONE length for the whole function, sound only while the cell was
    // stored once: a slice field rewritten anywhere had no length at all, even at reads after
    // the rewrite (fuzz_vra `slicefield`: 37 of 63 safe variants refused), and every writer that
    // is not a store (a constructor, a whole-struct store, a call handed the base, the entry
    // value) had to be counted by hand to keep the single-store rule sound. The octagon already
    // has every one of those writers as a transfer.
    for (int i = 0; i < V->nvar; i++) {
        IrValue *vv = V->val ? V->val[i] : NULL;
        V->slicelen[i] = (vv && vv->type && vv->type->kind == IRT_SLICE && V->odim[i] >= 0) ? i : -1;
    }
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next)
            if (ins->op==IR_MAKE_SLICE && ins->result && ins->n_operands>=2) {
                IrInstr *dd = V->def[ins->operands[0]->id];               // subslice start (vs array→slice decay)
                if (dd && dd->op==IR_ELEM_PTR) V->subslice_gep[ins->operands[0]->id] = true;
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
    int64_t slo, shi; bool shl, shh;   // (so dst inherits them; this is where a loop invariant
    oct_interval(o, src, &slo,&shl,&shi,&shh);  // is carried through a memory-cell load).
    oct_forget(o, dst);
    vra_add_diff_le(V, o, dst, src, 0);// Copies are infrequent (loads/casts/lengths), so the
    vra_add_diff_le(V, o, src, dst, 0);// O(dim^3) close here is fine — unlike per-instruction
                                       // forget-closes.
    // ★ AND STATE dst's INTERVAL OUTRIGHT. The two differences say dst == src, but an ABSOLUTE
    // bound for dst follows from them only after ANOTHER closure — and most readers of a range
    // do not close first, so a copied value read as unbounded. That is why `a *? b` lost its
    // value while `a + b` kept it: the checked lowering computes in i64, so the multiply's
    // operands are sext CASTS, and the MUL interval-product read them without closing and saw
    // the whole type. The closure this function already pays for is right above; carrying the
    // interval across costs nothing more and makes it visible to every reader, not just the
    // ones that close.
    if (shl) oct_add_lb(o, dst, slo);
    if (shh) oct_add_ub(o, dst, shi);
}

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
// ★ IN 128 BITS. These were int64 subtractions, and a constant at the edge of i64 overflowed:
// `0 - INT64_MIN` wrapped to INT64_MIN, so a cell holding 0 and a value of -2^63 were said to
// satisfy `a - b <= -2^63` while a - b is 2^63 (found by checking the analysis's states against a
// running program, --check-invariants, in four corpus programs; signed overflow is also undefined
// in this compiler's own C). An upper bound beyond the domain's infinity is no bound; one below
// int64 is weakened to INT64_MIN, still true.
static int64_t vra_clamp_ub(__int128 d) { return d >= (__int128)OCT_INF ? OCT_INF : d < (__int128)INT64_MIN ? INT64_MIN : (int64_t)d; }
static int64_t vra_diff_ub(Vra *V, const Octagon *W, int a, int b) {   // upper bound on a − b
    bool ac = (a>=0 && a<V->nvar && V->cknown[a]), bc = (b>=0 && b<V->nvar && V->cknown[b]);
    if (ac && bc) return vra_clamp_ub((__int128)V->cval[a] - V->cval[b]);
    int64_t lo,hi; bool hl,hh;
    if (ac) { vra_interval(V,W,b,&lo,&hl,&hi,&hh); return hl ? vra_clamp_ub((__int128)V->cval[a]-lo) : OCT_INF; }
    if (bc) { vra_interval(V,W,a,&lo,&hl,&hi,&hh); return hh ? vra_clamp_ub((__int128)hi-V->cval[b]) : OCT_INF; }
    return oct_get(W, oct_pos(b), oct_pos(a));
}
static void vra_add_diff_le(Vra *V, Octagon *W, int a, int b, int64_t c) {   // a − b ≤ c
    if (c >= OCT_INF) return;
    bool ac = (a>=0 && a<V->nvar && V->cknown[a]), bc = (b>=0 && b<V->nvar && V->cknown[b]);
    if (ac && bc) return;                                  // both known: nothing to record
    int64_t k;                                             // (a sum that leaves int64 bounds nothing)
    if (bc)      { if (!__builtin_add_overflow(V->cval[b], c, &k)) oct_add_ub(W, a, k); }   // a ≤ const + c
    else if (ac) { if (!__builtin_sub_overflow(V->cval[a], c, &k)) oct_add_lb(W, b, k); }   // b ≥ const − c
    else         oct_add_diff_le(W, a, b, c);
}

static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir);  // fwd

// ★ A MODULAR `+% −% *%` IS ℤ ARITHMETIC ONLY WHERE IT CANNOT WRAP. The transfers record the
// result over ℤ — `r = a − b` — which a CHECK-mode operation may do because its obligation
// proves no wrap happened. A modular operation has no obligation: wrapping is its DEFINED
// behaviour, so the ℤ fact is false exactly when the operator does the one thing it exists for.
// `-2147483647 -% 191808860` is 1955674789 at run time and −2339292507 over ℤ; read back through
// the i32 type range that is an EMPTY interval, so `if r != 1955674789 { return 2 }` was taken on
// every path and the code after it was dead to the analysis — every later obligation discharged
// vacuously. fuzz_wrap found it as a u32 multiply that overflowed at run time under a "proof".
//
// may_wrap: true unless the ℤ result provably stays inside the type (then it IS the modular
// result). u64's range is clamped to INT64_MAX here (the domain is i64-based), so a u64 result
// above it counts as a possible wrap — conservative, never a false "cannot wrap".
static bool vra_bound_fits(int64_t c) { return c > -(OCT_INF/2) && c < OCT_INF/2; }
static bool vra_modular_may_wrap_c(Vra *V, Octagon *W, IrInstr *ins, __int128 *c0, bool *exact) {
    IrType *t = ins->result ? ins->result->type : NULL;
    int64_t tlo, thi;
    *exact = false; *c0 = 0;
    if (ins->n_operands < 2 || !t || t->kind != IRT_INT || t->bits < 1 || t->bits > 64 ||
        !irtype_int_range(t, &tlo, &thi)) return true;
    oct_close(W);
    int64_t alo, ahi, blo, bhi;
    vra_range(V, W, ins->operands[0], &alo, &ahi);
    vra_range(V, W, ins->operands[1], &blo, &bhi);
    // A u64 read as INT64_MAX is UNBOUNDED — its real top is 2^64−1 — and so is the type's.
    // Reading the clamp as a bound would call `n -% 1` under `n > 1` a possible wrap (it cannot
    // wrap at any n ≥ 1) and `x +% 1` near 2^63 safe (it is not); the true top gets both right.
    const __int128 U64MAX = ((__int128)1 << 64) - 1;
    __int128 A0 = alo, A1 = ahi, B0 = blo, B1 = bhi, T0 = tlo, T1 = thi;
    IrType *xa = ins->operands[0]->type, *xb = ins->operands[1]->type;
    if (xa && xa->kind == IRT_INT && !xa->is_signed && xa->bits >= 64 && ahi == INT64_MAX) A1 = U64MAX;
    if (xb && xb->kind == IRT_INT && !xb->is_signed && xb->bits >= 64 && bhi == INT64_MAX) B1 = U64MAX;
    if (!t->is_signed && t->bits >= 64) T1 = U64MAX;
    __int128 c[4]; bool ovf = false;
    for (int k = 0; k < 4; k++) {
        __int128 x = (k & 2) ? A1 : A0, y = (k & 1) ? B1 : B0;
        if (ins->op == IR_MUL) ovf |= __builtin_mul_overflow(x, y, &c[k]);   // |x|,|y| ≤ 2^64
        else c[k] = ins->op == IR_ADD ? x + y : x - y;
    }
    *c0 = c[0]; *exact = (alo == ahi && blo == bhi && A1 == ahi && B1 == bhi);
    if (ovf) return true;
    __int128 lo = c[0], hi = c[0];
    for (int k = 1; k < 4; k++) { if (c[k] < lo) lo = c[k]; if (c[k] > hi) hi = c[k]; }
    return !(lo >= T0 && hi <= T1);
}
// ★ `as%` AND `as|` CHANGE THE VALUE THAT DOES NOT FIT, so the cast is a copy only where the
// source provably fits — the same reason as a modular operation above. Read as a copy, `300 as| u8`
// was 300, EMPTY inside u8, and the code after `if r != 255 { return }` was dead to the analysis.
// Returns false when the copy is exact; otherwise states the result and returns true.
static bool vra_cast_policy(Vra *V, Octagon *W, IrInstr *ins, int r) {
    IrValue *x = ins->operands[0]; IrType *t = ins->result ? ins->result->type : NULL;
    int64_t tlo, thi, lo, hi;
    oct_forget(W, r);
    if (!t || !x || !x->type || x->type->kind != IRT_INT || t->kind != IRT_INT ||
        !irtype_int_range(t, &tlo, &thi)) return true;
    oct_close(W);
    vra_range(V, W, x, &lo, &hi);
    bool u64dst = !t->is_signed && t->bits >= 64;
    bool above  = !x->type->is_signed && x->type->bits >= 64 && hi == INT64_MAX;  // may exceed i64
    if (lo >= tlo && hi <= thi && (!above || u64dst)) return false;
    if (ins->wrap == IR_WRAP_MODULAR && lo == hi && !above) {
        uint64_t m = t->bits >= 64 ? ~0ull : ((1ull << t->bits) - 1), u = (uint64_t)lo & m;
        bool rep = true; int64_t v;
        if (t->is_signed) v = (t->bits < 64 && ((u >> (t->bits - 1)) & 1)) ? (int64_t)(u | ~m) : (int64_t)u;
        else if (u > (uint64_t)INT64_MAX) { rep = false; v = 0; }
        else v = (int64_t)u;
        if (rep && vra_bound_fits(v)) { oct_add_const(W, r, v); return true; }
    }
    int64_t nlo = tlo, nhi = thi;
    if (ins->wrap == IR_WRAP_SATURATE) {       // a clamp is monotone: clamp the bounds
        nlo = lo < tlo ? tlo : lo > thi ? thi : lo;
        nhi = above ? thi : hi < tlo ? tlo : hi > thi ? thi : hi;
    }
    if (vra_bound_fits(nlo)) oct_add_lb(W, r, nlo); else if (!t->is_signed) oct_add_lb(W, r, 0);
    if (vra_bound_fits(nhi) && !(u64dst && nhi == INT64_MAX)) oct_add_ub(W, r, nhi);
    return true;
}
static bool vra_modular_may_wrap(Vra *V, Octagon *W, IrInstr *ins) {
    __int128 c0; bool exact;
    return vra_modular_may_wrap_c(V, W, ins, &c0, &exact);
}
// The transfer: false = cannot wrap, run the exact ℤ transfer; true = handled here — the wrapped
// value when both operands are exact, the type's range when not.
static bool vra_modular_wraps(Vra *V, Octagon *W, IrInstr *ins, int r) {
    __int128 c0; bool exact;
    if (!vra_modular_may_wrap_c(V, W, ins, &c0, &exact)) return false;
    IrType *t = ins->result ? ins->result->type : NULL;
    int64_t tlo, thi;
    if (!t || t->kind != IRT_INT || t->bits < 1 || t->bits > 64 || !irtype_int_range(t, &tlo, &thi))
        return true;
    if (exact) {
        uint64_t m = t->bits == 64 ? ~0ull : ((1ull << t->bits) - 1);
        uint64_t u = (uint64_t)(unsigned __int128)c0 & m;
        bool rep = true; int64_t v;
        if (t->is_signed) v = (t->bits < 64 && ((u >> (t->bits - 1)) & 1)) ? (int64_t)(u | ~m) : (int64_t)u;
        else if (u > (uint64_t)INT64_MAX) { rep = false; v = 0; }   // u64 above i64: unrepresentable
        else v = (int64_t)u;
        if (rep && vra_bound_fits(v)) { oct_add_const(W, r, v); return true; }
    }
    if (vra_bound_fits(tlo)) oct_add_lb(W, r, tlo);
    else if (!t->is_signed) oct_add_lb(W, r, 0);
    if (vra_bound_fits(thi)) oct_add_ub(W, r, thi);
    return true;
}
static void vra_free(Vra *V);                                                    // fwd (phase D)
static bool vra_dump_enabled = false;   // --dump-octagon: print the converged state
// ── C.1: THE MEASURE A TERMINATION RULE FOUND ────────────────────────────────────────────
// A rule says THAT a loop or a recursion ends; it also knows WHY, and the why is what a
// termination certificate has to state (local/internal/design/certificates.md, C.1). The rule
// that succeeds records its measure here, and `--dump-measures` prints it in the program's own
// names: "why does Lain think this loop ends?".
typedef enum { VRA_MEAS_NONE, VRA_MEAS_RISES, VRA_MEAS_FALLS, VRA_MEAS_PAIR,
               VRA_MEAS_PARAM, VRA_MEAS_PARAM_DIFF } VraMeasKind;
// RISES: the counter `a` rises toward the bound `b`, measure b - a. FALLS: it falls toward `b`,
// measure a - b. PAIR: `a < b` with each moving toward the other, measure b - a. PARAM: the
// parameter `a` descends at every self-call. PARAM_DIFF: `a - b` over two parameters does.
typedef struct { VraMeasKind kind; IrValue *a, *b; } VraMeasure;
static VraMeasure vra_last_measure;
static bool vra_dump_measures_enabled = false;   // --dump-measures
static void vra_print_measure(Vra *V, IrFunc *f, isize line, bool rec, bool ok);   // fwd
static void vra_dump_state(Vra *V, FILE *o);   // fwd

// --dump-octagon prints once per function per compile, not once per analysis run.
static IrFunc *vra_dumped[512]; static int vra_dumped_n = 0;
static bool vra_dumped_already(IrFunc *f) {
    for (int i = 0; i < vra_dumped_n; i++) if (vra_dumped[i] == f) return true;
    if (vra_dumped_n < 512) vra_dumped[vra_dumped_n++] = f;
    return false;
}

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
static bool vra_cell_accum_range(Vra *V, Octagon *W, int cell, int64_t *lo, int64_t *hi);
static int  vra_range_depth = 0;
static void vra_arith_range(IrOp op, int64_t alo,int64_t ahi, int64_t blo,int64_t bhi,
                            __int128 *rlo, __int128 *rhi);
static bool vra_ret_range(IrFunc *g, int64_t *lo, int64_t *hi);
static bool vra_ret_range_at(Vra *V, Octagon *W, IrFunc *g, IrInstr *call,
                             int64_t *lo, int64_t *hi);

// ── CALL-SITE-SENSITIVE RETURN RANGES ────────────────────────────────────────────────────
// `vra_ret_range` analyses the callee with its parameters at their DECLARED intervals, so
// `gcd(48, 36)` reads back as the whole usize range and the narrowing `as i32` is refused.
// Measured 2026-09-14 as 6 of the 65 programs blocking the 3.5 switchover.
//
// The channel below hands the callee the ACTUAL argument intervals for one analysis. It is a
// global rather than a parameter because `vra_analyze` is re-entered through several paths and
// threading it would touch all of them; it is saved and restored around the single call site
// that sets it, and it is INERT unless that site is active.
//
// Non-integer arguments (arrays, structs) simply bind nothing and the parameter keeps its type
// interval — strictly tighter than today in every case, never looser.
#define VRA_ARGBIND_MAX 16
#define VRA_CALLSITE_MAX_INSTR 48   // see the size cap in vra_ret_range_at
static int      vra_argbind_n = 0;
static int64_t  vra_argbind_lo[VRA_ARGBIND_MAX], vra_argbind_hi[VRA_ARGBIND_MAX];
static bool     vra_argbind_has[VRA_ARGBIND_MAX];
static int      vra_retq_depth = 0;
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
        if (!dd || dd->op != IR_SUB || dd->n_operands < 2 || !vra_zexact(V, dd)) continue;
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
            int cell = ins->n_operands ? vra_ref_target(V, ins->operands[0]->id) : -1;
            if (vra_field_cell_base(V, cell) >= 0) {        // a field of a local struct
                vra_assign_copy(V, W, r, vra_canon_cell(V, cell));
                break;
            }
            IrInstr *d = cell>=0 ? V->def[cell] : NULL;
            if ((d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind!=IRT_ARRAY)
                || vra_is_param_cell(V, cell))
                vra_assign_copy(V, W, r, cell);                // scalar cell → value
            else {
                oct_forget(W, r);                              // array elem / unknown
                // ...but an element of an array whose CONTENTS are known is bounded by them.
                // The bound goes into the OCTAGON, not just into vra_range: the add/sub
                // transfer records DIFFERENCE bounds (`r − a ≤ bhi`), so the closure can only
                // turn those into an absolute range for the result if the operand has one
                // here. Putting it only in vra_range proved the two loads and still lost
                // `a[0] + a[3] - 50`.
                int arr = vra_array_root(V, cell);
                if (arr >= 0 && V->elem_known && V->elem_known[arr]) {
                    oct_add_lb(W, r, V->elem_lo[arr]);
                    oct_add_ub(W, r, V->elem_hi[arr]);
                }
            }
            break;
        }
        case IR_STORE: {
            if (ins->n_operands<2) break;
            // ★ A STORE THROUGH A POINTER THIS DOMAIN CANNOT ATTRIBUTE WRITES A CELL IT CANNOT
            // NAME. The cases below all name their cell (an alloca, a field of one, a `var`
            // parameter); anything else fell off the end of this case and changed NOTHING, so
            //     var n usize = 4 ;  var r = var n ;  r = 200 ;  a[n]      // a is i32[5]
            // kept `n ∈ [4,4]` and PROVED a read of a[200] — and so did `unsafe { *q = 100 }`
            // through `q = &n`, which predates reference bindings. A call already assumes an
            // unattributable address may be any escaped cell; a store through one is the same
            // unknown writer without the call. (`var` parameter cells are in that set: the
            // prepass marks them escaped, since the storage is the caller's.)
            int cell = vra_ref_target(V, ins->operands[0]->id);   // through a reference binding
            if (cell == ins->operands[0]->id &&
                vra_arg_cell(V, ins->operands[0]) == VRA_ARG_UNKNOWN) {
                for (int c2=0; c2<V->nvar; c2++)
                    if (V->escaped && V->escaped[c2]) {
                        oct_forget(W, c2); vra_forget_fields_of(V, W, c2);
                    }
                break;
            }
            if (vra_field_cell_base(V, cell) >= 0) {        // a field of a local struct
                vra_assign_copy(V, W, vra_canon_cell(V, cell), ins->operands[1]->id);
                break;
            }
            IrInstr *d = V->def[cell];
            // A whole-struct assignment writes every field with no per-field IR_STORE to see,
            // so the field cells it invalidates have to be dropped here — UNLESS the value is
            // an IR_STRUCT_NEW, which carries its fields as OPERANDS. `var p = Point(3, 4)` is
            // exactly that: the constructor builds the value and it is stored whole, so
            // forgetting would throw away two literals that are sitting right there.
            //
            // ★ `var p = Point(3, 4)` does NOT reach this. A struct local with an initialiser
            // is lowered by the aggregate path as an OPAQUE write over the slot ("an
            // initialiser shape we do not model"), even though a constructor has a perfectly
            // good IR form — IR_STRUCT_NEW, carrying its fields as operands. So the
            // constructor spelling of a struct proves nothing while the field-by-field
            // spelling of the same struct proves everything. Reading the fields here was
            // tried and is unreachable; the fix belongs in lowering and is not yet found.
            // ...and a STRUCT PARAMETER is a base too (vra_arg_cell names it): `l = m` through
            // `l var L` writes every field of the caller's struct. Missing this case is a FALSE
            // PROOF — `l.k = 5; l = m; l.k - 5` kept k = 5 — the moment a store through the
            // parameter stopped being an unattributable (havoc-everything) write.
            IrValue *cv0 = V->val[cell];
            bool sparam_base = !d && cv0 && cv0->type && cv0->type->kind==IRT_PTR &&
                               cv0->type->elem && cv0->type->elem->kind==IRT_STRUCT;
            if ((d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind==IRT_STRUCT)
                || sparam_base) {
                IrInstr *sn = V->def[ins->operands[1]->id];
                if (sn && sn->op != IR_STRUCT_NEW) sn = NULL;
                for (int q=0; q<V->nvar; q++) {
                    IrInstr *qd = V->def[q];
                    if (!qd || qd->op!=IR_FIELD_PTR || qd->n_operands<1 ||
                        qd->operands[0]->id != cell) continue;
                    int fi = qd->aux.field_idx;
                    if (sn && fi >= 0 && fi < sn->n_operands)
                        vra_assign_copy(V, W, vra_canon_cell(V, q), sn->operands[fi]->id);
                    else
                        oct_forget(W, vra_canon_cell(V, q));
                }
            }
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
            if (ins->wrap == IR_WRAP_MODULAR && vra_modular_wraps(V, W, ins, r)) break;
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
            // ★ THE MIDPOINT IDENTITY: r = a + (b − a)/D with D ≥ 1 and a ≤ b.
            // Then (b−a)/D ≤ b−a, so r ≤ a + (b−a) = b, and r ≥ a because the quotient is
            // non-negative. The conclusion `a ≤ r ≤ b` is derived ENTIRELY from wrap-free
            // facts — the subtraction is guarded by `a ≤ b` and the division only shrinks —
            // which is what makes it usable to prove the ADD does not overflow.
            //
            // The octagon cannot reach this on its own: the step needs `%q + a ≤ b`, a bound
            // on a SUM by a third VALUE, and octagons hold `x + y ≤ c` only for a constant c.
            // Using the add's own recorded relation instead would be circular — that relation
            // is recorded assuming no wrap, which is the thing being proven.
            //
            // Without it `var mid usize = lo + (hi - lo) / 2` under `lo < hi` is refused: the
            // check reads the OPERAND ranges, both unbounded usize, and their sum leaves u64.
            // The old engine has this identity (47a88a2); the new one did not.
            if (isadd && !ac && !bc) {
                for (int side=0; side<2; side++) {
                    int q   = side ? a : b;               // the quotient operand
                    int lo_ = side ? b : a;               // the base operand
                    IrInstr *qd = (q>=0 && q<V->nvar) ? V->def[q] : NULL;
                    if (!qd || (qd->op!=IR_UDIV && qd->op!=IR_SDIV) || qd->n_operands<2) continue;
                    int dv = qd->operands[1]->id;
                    if (dv<0 || dv>=V->nvar || !V->cknown[dv] || V->cval[dv] < 1) continue;
                    IrInstr *sd2 = V->def[qd->operands[0]->id];
                    if (!sd2 || sd2->op!=IR_SUB || sd2->n_operands<2 || !vra_zexact(V, sd2)) continue;
                    // (b − a), same a: the same VALUE, not the same SSA id. Each mention of a
                    // variable is its own load, so `lo + (hi - lo) / 2` has two ids for `lo`;
                    // equal in the closed octagon means equal here (see vra_check_overflow).
                    int av  = sd2->operands[1]->id;
                    int hi_ = sd2->operands[0]->id;
                    oct_close(W);
                    if (av != lo_ && !(vra_diff_ub(V, W, av, lo_) <= 0 &&
                                       vra_diff_ub(V, W, lo_, av) <= 0)) continue;
                    if (vra_diff_ub(V, W, lo_, hi_) > 0) continue;      // a ≤ b not provable
                    vra_add_diff_le(V, W, r, hi_, 0);                   // r ≤ b
                    vra_add_diff_le(V, W, lo_, r, 0);                   // r ≥ a
                }
            }
            // (-INT64_MIN does not exist in int64: that half of the relation is left out.)
            if (isadd && bc)      { vra_add_diff_le(V,W,r,a,V->cval[b]); if (V->cval[b] != INT64_MIN) vra_add_diff_le(V,W,a,r,-V->cval[b]); }   // r=a+c (exact)
            else if (isadd && ac) { vra_add_diff_le(V,W,r,b,V->cval[a]); if (V->cval[a] != INT64_MIN) vra_add_diff_le(V,W,b,r,-V->cval[a]); }
            else if (!isadd && bc){ if (V->cval[b] != INT64_MIN) vra_add_diff_le(V,W,r,a,-V->cval[b]); vra_add_diff_le(V,W,a,r,V->cval[b]); }   // r=a-c (exact)
            else if (!isadd && ac){ int64_t c=V->cval[a]; oct_add_sum_le(W,r,b,c); if (c != INT64_MIN) oct_add_negsum_le(W,r,b,-c); } // r=c-b ⇒ r+b=c
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
            if (ins->wrap == IR_WRAP_MODULAR && vra_modular_wraps(V, W, ins, r)) break;
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
            // ★ BOTH OPERANDS SYMBOLIC BUT BOUNDED: the product's INTERVAL is the extremes of
            // the four corner products. The octagon cannot hold a product — that is why the
            // constant-stride cases above exist — but an interval for it is ordinary interval
            // arithmetic and the domain was simply not asking. Without it, `a * b` on two
            // parameters refined to [0,3] had no bound but its RESULT TYPE, so
            // `func f(a u8 >= 0 and <= 3, b u8 >= 0 and <= 3) u8 { return a * b }` was refused:
            // the product is [0,9] and plainly fits u8. Measured by fuzz_unsigned.sh as
            // over-strictness on 11 of 100 SAFE programs, which is what put a number on it.
            else {
                int64_t alo,ahi,blo,bhi;
                vra_range(V, W, ins->operands[0], &alo, &ahi);
                vra_range(V, W, ins->operands[1], &blo, &bhi);
                int64_t c1,c2,c3,c4;
                if (alo > INT64_MIN && ahi < INT64_MAX && blo > INT64_MIN && bhi < INT64_MAX
                    && vra_safe_scale(alo,blo,&c1) && vra_safe_scale(alo,bhi,&c2)
                    && vra_safe_scale(ahi,blo,&c3) && vra_safe_scale(ahi,bhi,&c4)) {
                    int64_t lo=c1, hi=c1;
                    if (c2<lo) lo=c2; if (c2>hi) hi=c2;
                    if (c3<lo) lo=c3; if (c3>hi) hi=c3;
                    if (c4<lo) lo=c4; if (c4>hi) hi=c4;
                    oct_add_lb(W,r,lo); oct_add_ub(W,r,hi);
                }
            }
            break;
        }
        case IR_SIZEOF: case IR_ALIGNOF: {
            // A complete C object type has size >= 1 and alignment >= 1. Where the type's C
            // spelling has an exact size (ir_fixed_size: an integer, an enumeration, a [packed]
            // struct, a vector, an array of those) @sizeof IS that constant, and its alignment
            // is at most the size (C sizes every type in whole multiples of its alignment).
            // Anything else is the C ABI's number, and nothing more is claimed. Before this,
            // `a[@sizeof(u32)]` on a `u8[8]` was refused.
            if (r<0) break;
            oct_forget(W, r);
            int64_t sz = ir_fixed_size(ins->aux.alloca_ty);
            if (sz > 0 && ins->op == IR_SIZEOF) { oct_add_const(W, r, sz); break; }
            oct_add_lb(W, r, 1);
            if (sz > 0) oct_add_ub(W, r, sz);
            break;
        }
        case IR_CTZ: case IR_CLZ: case IR_POPCOUNT: {
            // A bit intrinsic lands in [0, W] where W is the OPERAND's width — exactly the
            // fact that makes `a[@popcount(mask)]` provable without a runtime check. Modelled
            // as an op rather than an opaque call precisely so this range is free.
            if (r<0) break;
            const IrType *at = ins->operands[0] ? ins->operands[0]->type : NULL;
            int width = (at && at->kind==IRT_INT && at->bits>0 && at->bits<=64) ? at->bits : 32;
            int64_t xlo = INT64_MIN, xhi = INT64_MAX;
            if (ins->operands[0]) vra_range(V, W, ins->operands[0], &xlo, &xhi);
            oct_forget(W, r);
            int64_t lo = 0, hi = width;
            // @ctz/@clz owe a non-zero argument (vra_check_bitcount), so in any defined execution
            // they count at most width - 1 zeros; and @clz is monotone in a positive argument: the
            // larger it is, the fewer leading zeros. (`31 - @clz(x)` for x != 0 is then provable.)
            #define VRA_BITLEN(v) (64 - __builtin_clzll((unsigned long long)(v)))
            if (ins->op != IR_POPCOUNT) hi = width - 1;
            if (ins->op == IR_CLZ && xlo >= 1 && xhi < ((int64_t)1 << (width < 63 ? width : 62))) {
                lo = width - VRA_BITLEN(xhi); hi = width - VRA_BITLEN(xlo);
            }
            if (ins->op == IR_POPCOUNT && xlo >= 0 && xhi >= 0) { int64_t b = xhi ? VRA_BITLEN(xhi) : 0; if (b < hi) hi = b; }
            #undef VRA_BITLEN
            oct_add_lb(W, r, lo); oct_add_ub(W, r, hi);
            break;
        }
        case IR_AND: {   // x & c  with c ≥ 0 constant  ⇒  0 ≤ r ≤ c   (mask idiom c=N−1)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_forget(W, r);
            if      (V->cknown[b] && V->cval[b]>=0){ oct_add_lb(W,r,0); oct_add_ub(W,r,V->cval[b]); }
            else if (V->cknown[a] && V->cval[a]>=0){ oct_add_lb(W,r,0); oct_add_ub(W,r,V->cval[a]); }
            // ★ A SYMBOLIC MASK BOUNDS THE RESULT TOO. The bits of `x & y` are a subset of y's, so
            // for a NON-NEGATIVE y, 0 <= x & y <= y, whatever x is: one octagon difference,
            // r - y <= 0. Only the constant form was known, so `a[x & (a.len - 1)]` (a ring
            // buffer whose length is a runtime power of two, and in bounds for ANY len >= 1) was
            // refused, while rustc -O, clang and gcc all delete that check (bounds matrix, b5).
            // Per operand: a negative y (signed, sign bit set) bounds nothing and is skipped.
            for (int side=0; side<2; side++) {
                int y = side ? b : a;
                IrValue *yv = ins->operands[side ? 1 : 0];
                if (V->cknown[y] || !yv || !yv->type || yv->type->kind!=IRT_INT) continue;
                bool nonneg = !yv->type->is_signed;
                if (!nonneg) {
                    oct_close(W);
                    int64_t ylo, yhi; bool hl, hh; vra_interval(V, W, y, &ylo,&hl,&yhi,&hh);
                    nonneg = hl && ylo >= 0;
                }
                if (!nonneg) continue;
                oct_add_lb(W, r, 0);
                vra_add_diff_le(V, W, r, y, 0);                 // r <= y
            }
            break;
        }
        case IR_OR: case IR_XOR: {
            // ★ OR AND XOR HAD NO TRANSFER: the result could be any value. Two NON-NEGATIVE
            // operands set only bits below the higher one's top bit, so the result lies in
            // [0, 2^bitlen(max) - 1], and an OR is at least its larger operand. Without it
            // `(a & b) | (a ^ b) % 7` over u8 operands was refused: `% 7` makes the right side an
            // i32, and their OR read as unbounded where it narrowed back to u8 (plan I.32).
            if (r<0) break;
            oct_close(W);
            int64_t alo, ahi, blo, bhi;
            vra_range(V, W, ins->operands[0], &alo, &ahi);
            vra_range(V, W, ins->operands[1], &blo, &bhi);
            oct_forget(W, r);
            if (alo >= 0 && blo >= 0) {
                uint64_t m = (uint64_t)(ahi > bhi ? ahi : bhi);
                int bits = m ? 64 - __builtin_clzll((unsigned long long)m) : 0;
                int64_t cap = bits >= 63 ? INT64_MAX : (int64_t)(((uint64_t)1 << bits) - 1);
                oct_add_lb(W, r, ins->op == IR_OR ? (alo > blo ? alo : blo) : 0);
                oct_add_ub(W, r, cap);
            }
            break;
        }
        case IR_UDIV: {  // x / b  — in any defined exec (b > 0, x ≥ 0)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_close(W);                                  // the dividend's interval, relationally
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            oct_forget(W, r);
            if (V->cknown[b] && V->cval[b]>0) { vra_div_facts(V, W, r, a, V->cval[b], alo,hl,ahi,hh); break; }
            // ★ A DIVISOR THAT IS NOT A CONSTANT. This passed 1 as the divisor, and vra_div_facts
            // states both bounds for EXACTLY that divisor: `r ≤ hi(x)/1` holds for every divisor
            // ≥ 1, but `r ≥ lo(x)/1` holds for 1 alone, so `15 / y` with y = 39 was "at least
            // 15". A branch on the quotient then looked dead, and anything in it was proven: a
            // division by zero there compiled and died with SIGFPE (found by the per-operation
            // soundness harness, src/tools/soundness_driver.c). The quotient is largest at the
            // smallest divisor and smallest at the largest, so each bound takes its own end.
            int64_t Blo, Bhi; vra_range(V, W, ins->operands[1], &Blo, &Bhi);
            if (Blo < 1) Blo = 1;                          // b = 0 is the separate obligation
            oct_add_lb(W, r, 0);
            vra_add_diff_le(V,W, r, a, 0);                 // r ≤ x: every divisor is at least 1
            if (hh && ahi >= 0) oct_add_ub(W, r, ahi / Blo);
            if (hl && alo >= 0 && Bhi >= 1) oct_add_lb(W, r, alo / Bhi);
            if (Blo >= 2 && hl && alo >= 1) vra_add_diff_le(V,W, r, a, -1);   // r ≤ x − 1
            break;
        }
        case IR_SDIV: {  // signed x / c — for x ≥ 0 and c > 0 (the common index idiom
            if (r<0) break;                                 // `i / 2`) it is exactly udiv.
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_close(W);
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            oct_forget(W, r);
            if (hl && alo>=0 && V->cknown[b] && V->cval[b]>0) {
                vra_div_facts(V, W, r, a, V->cval[b], alo,hl,ahi,hh);
                break;
            }
            // ★ ANY OTHER SIGNED QUOTIENT (DECIDE-M). It was UNKNOWN, which cost nothing while a
            // quotient kept its operand's type; once `/` widens (i32 / i32 : i33), an unknown
            // quotient cannot narrow back and every `q i32 = a / b` would be refused. Truncating
            // division is monotone in each operand on each side of zero, so its extremes are at
            // the corners of the box — per sign of the divisor, zero itself excluded (dividing
            // by it is a separate obligation). In i128, because MIN / -1 is the corner that
            // matters.
            { int64_t Alo, Ahi, Blo, Bhi;
              vra_range(V, W, ins->operands[0], &Alo, &Ahi);
              vra_range(V, W, ins->operands[1], &Blo, &Bhi);
              __int128 qlo = 0, qhi = 0; bool any = false;
              int64_t parts[2][2]; int np = 0;
              if (Bhi >= 1)  { parts[np][0] = Blo > 1 ? Blo : 1;   parts[np][1] = Bhi;              np++; }
              if (Blo <= -1) { parts[np][0] = Blo;                  parts[np][1] = Bhi < -1 ? Bhi : -1; np++; }
              for (int k = 0; k < np; k++)
                  for (int ia = 0; ia < 2; ia++)
                      for (int ib = 0; ib < 2; ib++) {
                          __int128 q = (__int128)(ia ? Ahi : Alo) / (__int128)parts[k][ib];
                          if (!any || q < qlo) qlo = q;
                          if (!any || q > qhi) qhi = q;
                          any = true;
                      }
              int64_t tlo, thi;
              if (any && ins->result->type && irtype_int_range(ins->result->type, &tlo, &thi)) {
                  if (ins->wrap == IR_WRAP_MODULAR && qhi > thi) { qlo = tlo; qhi = thi; } // MIN/-1 wraps to MIN
                  if (qlo < tlo) qlo = tlo;                                                  // saturate, or no
                  if (qhi > thi) qhi = thi;                                                  // defined value out
                  if (qlo <= qhi) { oct_add_lb(W, r, (int64_t)qlo); oct_add_ub(W, r, (int64_t)qhi); }
              } }
            break;
        }
        case IR_UREM: {  // x % b  (unsigned)  ⇒  0 ≤ r < b   (b > 0 in any defined exec;
            if (r<0) break;                                 // b = 0 is a separate div-by-zero)
            int b=ins->operands[1]->id; oct_forget(W, r);
            oct_add_lb(W, r, 0);
            if (V->cknown[b] && V->cval[b]>0) oct_add_ub(W, r, V->cval[b]-1);  // absolute ≤ c−1
            else vra_add_diff_le(V,W, r, b, -1);                                 // relative r < b
            // ...and r ≤ x: a remainder never exceeds its dividend. Without it `n - n % 3` on a
            // usize was E086 — the subtraction's `n ≥ n % 3` was not a fact anything knew.
            vra_add_diff_le(V,W, r, ins->operands[0]->id, 0);
            break;
        }
        case IR_SREM: {  // signed a % c  ⇒  −(c−1) ≤ r ≤ c−1 (tighter to [0,c−1] if a≥0)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id; oct_forget(W, r);
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            if (hl && alo >= 0) vra_add_diff_le(V,W, r, a, 0);   // a ≥ 0 ⇒ r ≤ a, as for UREM
            if (V->cknown[b] && V->cval[b]>0){
                int64_t c=V->cval[b];
                oct_add_lb(W,r, (hl&&alo>=0)?0:-(c-1)); oct_add_ub(W,r,c-1);
            } else if (hl && alo>=0) {                     // a ≥ 0: the remainder is ≥ 0, and below |b|
                // ★ NOT `r < b`. That was stated on "b > 0 in any defined exec", which is true of
                // an UNSIGNED remainder only: a signed divisor need only be non-zero, and 5 % -3
                // is 2. With b negative `r < b` and `r ≥ 0` are contradictory, so the state after
                // the remainder was EMPTY and every later obligation was vacuously proven
                // (`(x % y) + 32767` on an i16 compiled; and 127 % -128 = 127 was "at most 126").
                // Found by the per-operation soundness harness.
                oct_add_lb(W,r,0);
                int64_t Blo, Bhi; vra_range(V, W, ins->operands[1], &Blo, &Bhi);
                if (Blo >= 1) vra_add_diff_le(V,W,r,b,-1);        // a positive divisor: r < b
                else {
                    __int128 mb = Blo < 0 ? -(__int128)Blo : (__int128)Blo, mb2 = Bhi < 0 ? -(__int128)Bhi : (__int128)Bhi;
                    if (mb2 > mb) mb = mb2;
                    if (mb >= 1 && mb - 1 <= INT64_MAX) oct_add_ub(W, r, (int64_t)(mb - 1));   // r < |b|
                }
            } else {
                // ★ The general remainder (DECIDE-M): it takes the dividend's sign, and its
                // magnitude is below both |b| and |a| + 1. Needed once `%` is computed in i64 and
                // narrowed back: the narrowing is proven by exactly this.
                int64_t Alo, Ahi, Blo, Bhi;
                vra_range(V, W, ins->operands[0], &Alo, &Ahi);
                vra_range(V, W, ins->operands[1], &Blo, &Bhi);
                __int128 mb = (__int128)(Blo < 0 ? -(__int128)Blo : Blo);
                __int128 mb2 = (__int128)(Bhi < 0 ? -(__int128)Bhi : Bhi);
                if (mb2 > mb) mb = mb2;
                __int128 rlo = Alo < 0 ? (-(mb - 1) > Alo ? -(mb - 1) : Alo) : 0;
                __int128 rhi = Ahi > 0 ? ((mb - 1) < Ahi ? (mb - 1) : Ahi) : 0;
                if (mb >= 1 && rlo <= rhi && rlo >= INT64_MIN && rhi <= INT64_MAX) {
                    oct_add_lb(W, r, (int64_t)rlo); oct_add_ub(W, r, (int64_t)rhi);
                }
            }
            break;
        }
        case IR_SHL: {
            // x << k for a bounded amount k: [xlo * 2^klo, xhi * 2^khi] when x >= 0, stated only
            // when that FITS the result type — then no bit was shifted out, whatever the mode (a
            // signed shift owes exactly that, vra_check_shift; an unsigned one wraps, and a range
            // that does not fit says nothing). There was no transfer at all: `1 << 7` was any
            // value, so `(1 << 7) as u8` and every use of a shifted constant were unprovable.
            if (r<0) break;
            oct_forget(W, r);
            IrType *rt = ins->result ? ins->result->type : NULL;
            int64_t tlo, thi;
            if (ins->n_operands < 2 || !rt || rt->kind != IRT_INT || !irtype_int_range(rt, &tlo, &thi)) break;
            int64_t alo, ahi, klo, khi;
            vra_range(V, W, ins->operands[0], &alo, &ahi);
            vra_range(V, W, ins->operands[1], &klo, &khi);
            // ...and for a NEGATIVE x too: x << k is x * 2^k, monotone in k for either sign, so
            // the bounds are the extremes over the two ends of k (lo << kmin vs lo << kmax for
            // the least, hi << kmin vs hi << kmax for the greatest). `-3 << 2` is -12. Without
            // this an i8 `x << 2` over [-16, -1] was computed wide and refused when narrowed back
            // (E086), though -64..-4 fits. Computed as products: shifting a negative __int128 is
            // itself undefined in C.
            if (alo <= -OCT_INF/2 || ahi >= OCT_INF/2 || klo < 0 || khi > 62) break;
            __int128 pmin = (__int128)1 << klo, pmax = (__int128)1 << khi;
            __int128 l1 = (__int128)alo * pmin, l2 = (__int128)alo * pmax;
            __int128 h1 = (__int128)ahi * pmin, h2 = (__int128)ahi * pmax;
            __int128 rlo = l1 < l2 ? l1 : l2, rhi = h1 > h2 ? h1 : h2;
            if (rlo < (__int128)tlo || rhi > (__int128)thi || rlo < (__int128)INT64_MIN || rhi > (__int128)INT64_MAX) break;
            oct_add_lb(W, r, (int64_t)rlo); oct_add_ub(W, r, (int64_t)rhi);
            break;
        }
        case IR_LSHR: {  // x >> k  (logical) of a non-negative x is in [0, x]
            if (r<0) break;
            int a=ins->operands[0]->id; oct_forget(W,r);
            int64_t alo,ahi; bool hl,hh; vra_interval(V, W,a,&alo,&hl,&ahi,&hh);
            if (hl&&alo>=0){ oct_add_lb(W,r,0); if(hh) oct_add_ub(W,r,ahi);    // 0 ≤ r ≤ a — and
                             vra_add_diff_le(V,W, r, a, 0); }                   // as a RELATION, so
            break;                                                // `n - (n >> 1)` needs no bound on n
        }
        case IR_SLICE_LEN: {
            if (r<0) break;
            oct_forget(W, r);
            int s = ins->operands[0]->id, canon = V->slicelen[s];     // the slice's own dimension
            if (canon>=0 && canon!=r) vra_assign_copy(V, W, r, canon);
            oct_add_lb(W, r, 0);                                        // a length is ≥ 0
            break;
        }
        case IR_MAKE_SLICE:     // {data, len}: the slice's dimension IS its length
            if (r>=0 && ins->n_operands>=2) { vra_assign_copy(V, W, r, ins->operands[1]->id); oct_add_lb(W, r, 0); }
            break;
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
            if (x>=0 && x<V->nvar && V->cknown[x]) { if (V->cval[x] != INT64_MIN) oct_add_const(W, r, -V->cval[x]); break; }
            oct_add_sum_le(W, r, x, 0);
            oct_add_negsum_le(W, r, x, 0);
            break;
        }
        case IR_BNOT: {
            // `r = ~x` — EXACT, and octagon-shaped, exactly as IR_NEG above is. In ℤ the
            // identity is signedness-dependent: for a signed type ~x = −x − 1, so r + x = −1;
            // for an unsigned N-bit type ~x = (2^N − 1) − x, so r + x = 2^N − 1.
            //
            // ★ There was no case at all, so the result was FORGOTTEN and read back as the
            // full signed range. That range contains negatives, so it does not fit an
            // UNSIGNED slot, and the NARROWING check refused every `~` on a uN:
            //     func f(a u64) u64 { return ~a }     // [E086] not provably free of overflow
            // while the identical expression on an iN compiled. The corpus never caught it:
            // bitwise_pass.ln inverts `var a = 12` (signed `int`), and simd_lexer_pass.ln
            // writes `(~@movemask(ws)) & 65535`, where the AND re-bounds the result before
            // anything narrows it. Bare `~` on unsigned was untested.
            if (r<0) break;
            oct_forget(W, r);
            if (ins->n_operands < 1) break;
            int x = ins->operands[0]->id;
            int64_t tlo, thi;
            const IrType *rt = ins->result ? ins->result->type : NULL;
            bool have_t = irtype_int_range(rt, &tlo, &thi);
            bool is_signed = rt && rt->kind == IRT_INT && rt->is_signed;
            if (x>=0 && x<V->nvar && V->cknown[x]) {
                // Fold exactly, in the result type's own arithmetic: ~c is −c−1 signed, and
                // (2^N−1)−c unsigned. Using C's ~ on the i64 carrier would give the SIGNED
                // answer for an unsigned type (u8: ~5 is 250, not −6).
                int64_t c = (is_signed || !have_t) ? ~V->cval[x] : thi - V->cval[x];   // ~c == -c-1, without -INT64_MIN
                oct_add_const(W, r, c);
                break;
            }
            // The result is a value of its own type, always. This alone is what the
            // narrowing check needs; the relation below is the precision.
            if (have_t) { oct_add_lb(W, r, tlo); oct_add_ub(W, r, thi); }
            // r + x = s. Signed s = −1 is always representable. Unsigned s = 2^N − 1 is not:
            // irtype_int_range clamps u64 to INT64_MAX (the domain is i64), and OCT_INF is
            // INT64_MAX/4, so the sum constraint is only added when it actually fits.
            int64_t s = is_signed ? -1 : (have_t ? thi : 0);
            if ((is_signed || have_t) && s > -OCT_INF && s < OCT_INF) {
                oct_add_sum_le(W, r, x, s);
                oct_add_negsum_le(W, r, x, -s);
            }
            break;
        }
        case IR_CAST:
            // ★ `x as bool` IS NOT A COPY. It is C's _Bool conversion: any non-zero value is 1. The
            // copy below made `(-5) as bool` read as -5, so `if (b as i32) < 0` looked always
            // taken and a division by zero on the other branch was proven (SIGFPE). Found by
            // checking this analysis's block states against a running program (--check-invariants:
            // `7 as bool` was held to be 7). A bool's range is [0, 1], exact when zero is or is not
            // excluded.
            if (r>=0 && ins->n_operands && ins->result && ins->result->type && ins->result->type->kind == IRT_BOOL
                && ins->operands[0]->type && ins->operands[0]->type->kind == IRT_INT) {
                int64_t slo, shi; vra_range(V, W, ins->operands[0], &slo, &shi);
                oct_forget(W, r);
                if (slo > 0 || shi < 0)        { oct_add_lb(W, r, 1); oct_add_ub(W, r, 1); }
                else if (slo == 0 && shi == 0) { oct_add_lb(W, r, 0); oct_add_ub(W, r, 0); }
                else                           { oct_add_lb(W, r, 0); oct_add_ub(W, r, 1); }
                break;
            }
            if (r>=0){ // treat as a copy (widenings preserve value; a narrowing that
                       // changes it would be a separate proven-safe obligation)
                if (vra_is_int(ins->result) && ins->n_operands) {
                    if (ins->wrap != IR_WRAP_CHECK && vra_cast_policy(V, W, ins, r)) break;
                    vra_assign_copy(V, W, r, ins->operands[0]->id);
                }
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
                for (int cell=0; cell<V->nvar; cell++) if (V->escaped[cell]) {
                    oct_forget(W, cell);
                    vra_forget_fields_of(V, W, cell);
                }
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
                    for (int cell=0; cell<V->nvar; cell++) if (V->escaped[cell]) {
                        oct_forget(W, cell);
                        vra_forget_fields_of(V, W, cell);
                    }
                } else {
                    // (a) needs a callee that can reach memory it was not handed. Without mutable
                    // globals or stored references that takes a stash — a pointer loaded from
                    // memory and written through — which ir_func_writes_only_owned rules out,
                    // transitively. `is_space(l.src[l.pos])` inside a lexer loop kept l.pos.
                    bool reaches_stash = !(cal && vra_mod && ir_func_writes_only_owned(cal, vra_mod));
                    if (reaches_stash)
                    for (int cell=0; cell<V->nvar; cell++) if (V->persist[cell]) {
                        oct_forget(W, cell); vra_forget_fields_of(V, W, cell);
                    }
                    for (int k=0; k<ins->n_operands; k++) {
                        if (k<64 && !((cw>>k)&1u)) continue;
                        int c = vra_arg_cell(V, ins->operands[k]);
                        // The FIELD cells of an argument go with it: a callee handed the
                        // struct's address writes any field of it, and there is no per-field
                        // IR_STORE here to see. Forgetting only the base left `a.room` reading
                        // as whatever it was before the call — an accepted program whose
                        // callee had just set it out of range.
                        if (c>=0) { oct_forget(W, c); vra_forget_fields_of(V, W, c); }
                    }
                }
            }
            if (r>=0) {
                oct_forget(W, r);
                // ...but the RESULT is not unknown: the callee's body bounds it.
                int64_t rlo, rhi;
                IrFunc *cal2 = vra_find_func(ins->aux.callee);
                bool got = vra_ret_range_at(V, W, cal2, ins, &rlo, &rhi);
                if (!got) got = vra_ret_range(cal2, &rlo, &rhi);
                if (got && ins->result && ins->result->type) {
                    if (rlo > -OCT_INF/2) oct_add_lb(W, r, rlo);
                    if (rhi <  OCT_INF/2) oct_add_ub(W, r, rhi);
                }
            }
            break;
        }
        case IR_FIELD_PTR:
            // ★ COMPUTING AN ADDRESS DOES NOT CHANGE WHAT IS AT IT. The default below forgets
            // every instruction result, which is right for a fresh value and wrong for a
            // field_ptr: it NAMES a cell that already holds something. Order made it visible —
            // `p.x = 3` creates the field_ptr BEFORE the store, so the forget was harmless,
            // while `var p = Point(3, 4)` stores first and reads later, so the read's own
            // field_ptr wiped the value the store had just put there. The constructor spelling
            // of a struct proved nothing and the field-by-field spelling of the same struct
            // proved everything, for no reason in the source.
            if (r>=0 && vra_field_cell_base(V, r) < 0) oct_forget(W, r);
            break;
        default:
            if (r>=0) oct_forget(W, r);   // conservative: result becomes unknown
            break;
    }
}

// refine W along a branch edge from `br` taken in direction `then_dir`
static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir) {
    if (!cond) return;
    // A CONSTANT condition decides its branch, and the edge it rules out is dead (⊥). Only a
    // comparison was read here, so `while true { }` kept a live exit edge, and code after the
    // loop — a `[noreturn]` function's implicit return above all — counted as reachable.
    if (cond->id >= 0 && cond->id < V->nvar && V->cknown[cond->id]) {
        if ((V->cval[cond->id] != 0) != then_dir && W->dim > 0) {
            *oct_at(W, 0, 0) = -1;            // a negative self-distance IS ⊥ (oct_is_bottom)
            oct_set_clean(W, false);
        }
        return;
    }
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
                // ...and NON-NEGATIVE: a signed bound of -1 is 2^64 - 1 unsigned, and every
                // negative `small` is below it.
                trustworthy = hh && bhi >= 0 && bhi < ((int64_t)1<<62) && hl && blo >= 0;
                if (!trustworthy && bound<V->nvar && V->cknown[bound]) trustworthy = V->cval[bound] >= 0;
            }
            if (trustworthy) oct_add_lb(W, small, 0);
        }
        // ★ AND ONLY THEN IS IT A SIGNED COMPARISON. Everything below states the predicate on
        // signed values, which an unsigned comparison is only between two NON-NEGATIVE ones. The
        // false edge of `p in a` (`(unsigned)p < 32`) is `p < 0 or p >= 32`; it was read as
        // `p >= 32`, so code there under `if p < 0` was dead to the analysis and a division by
        // zero in it was proven (SIGFPE). Found by --check-invariants on a fuzz_vra program:
        // the state said p >= 32 where the run had p = -1.
        bool an, bn;
        { int64_t lo,hi; bool hl,hh;
          an = (a<V->nvar && V->cknown[a]) ? V->cval[a] >= 0
             : (a<V->nvar && V->val[a] && V->val[a]->type && V->val[a]->type->kind==IRT_INT && !V->val[a]->type->is_signed)
             || (vra_interval(V, W, a, &lo,&hl,&hi,&hh), hl && lo >= 0);
          bn = (b<V->nvar && V->cknown[b]) ? V->cval[b] >= 0
             : (b<V->nvar && V->val[b] && V->val[b]->type && V->val[b]->type->kind==IRT_INT && !V->val[b]->type->is_signed)
             || (vra_interval(V, W, b, &lo,&hl,&hi,&hh), hl && lo >= 0); }
        if (!an || !bn) return;
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
// Is this alloca just where a PARAMETER was spilled? Exactly one store to it, of a value with
// no defining instruction — i.e. a parameter. Distinct from vra_is_param_cell, which asks
// whether a value IS a pointer parameter; this asks whether a local cell merely HOLDS one.
static bool vra_cell_is_param_spill(Vra *V, int cell) {
    if (cell < 0 || cell >= V->nvar) return false;
    IrInstr *d = V->def[cell];
    if (!d || d->op != IR_ALLOCA) return false;
    int nstore = 0; bool from_param = false;
    for (IrBlock *b = V->f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (i->op == IR_STORE && i->n_operands >= 2 && i->operands[0]->id == cell) {
                nstore++;
                int sv = i->operands[1]->id;
                from_param = (sv >= 0 && sv < V->nvar && !V->def[sv]);
            }
    return nstore == 1 && from_param;
}

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
                if (ad->op == IR_ALLOCA) {
                    // ...unless the alloca is the PARAMETER'S OWN SPILL SLOT. Lowering stores
                    // an aggregate parameter into a local cell at entry, so `p.x` on a
                    // `p Point` parameter reaches an IR_ALLOCA and looked like a local — which
                    // put every `return p.x + p.y` into the UNCLASSIFIED bucket instead of
                    // `unbounded`, where it belongs: two unrefined i32 fields really can sum
                    // out of i32, and refusing is the language working, not a lost proof.
                    if (!vra_cell_is_param_spill(V, addr->id)) { *all_param = false; }
                    break;
                }
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
        c.loss = (c.kind == VRA_TERMINATION)          ? VLOSS_TERM
               : (c.kind == VRA_BOUNDS && !c.has_len) ? VLOSS_NOLEN
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
    if (!d || d->op != IR_ADD || d->n_operands < 2 || !vra_zexact(V, d)) return false;
    int e0 = V->shape_ext[sbase][0], e1 = V->shape_ext[sbase][1];
    // match ADD(MUL(i, e1), j)  — and the commuted forms
    for (int side=0; side<2; side++) {
        IrValue *mulv = d->operands[side], *jv = d->operands[1-side];
        if (!mulv || !jv) continue;
        IrInstr *m = V->def[mulv->id];
        if (!m || m->op != IR_MUL || m->n_operands < 2 || !vra_zexact(V, m)) continue;
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
    // ★ `unsafe` waives the BOUNDS obligation too, not only the numeric ones. The flag was
    // read by vra_check_narrow, vra_check_overflow and vra_check_divzero and by neither of
    // the two bounds checks, so `unsafe { a[i] }` — which the language documents as turning
    // these checks off, and which the old engine accepts — was [E085] under --engine=ir-full.
    // That made every SIMD program in the corpus unbuildable on the new engine.
    if (ins->unchecked) return;
    if (ins->n_operands<2) return;
    if (ins->result && V->subslice_gep[ins->result->id]) return;  // a subslice start — the make_slice checks it
    int idx = ins->operands[1]->id;
    IrValue *base = ins->operands[0];
    IrInstr *bd = V->def[base->id];
    int64_t clen=-1; int lenvar=-1;
    // ★ A VECTOR IS A FIXED-LENGTH THING AND ITS LENGTH IS RIGHT HERE. `IRT_VECTOR` carries N
    // in the same `array_len` field as `IRT_ARRAY`, and this was reading only the array case —
    // so `i32x4[0]` reported "no length is known here" and every SIMD program was rejected by
    // --engine=ir-full while the default accepted it. A lane index is a bounds obligation like
    // any other; the length was never missing, only unread.
    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty &&
        (bd->aux.alloca_ty->kind==IRT_ARRAY || bd->aux.alloca_ty->kind==IRT_VECTOR))
        clen = bd->aux.alloca_ty->array_len;                          // local fixed array or vector
    else if (base->type && (base->type->kind==IRT_ARRAY || base->type->kind==IRT_VECTOR))
        clen = base->type->array_len;                                // fixed-length value (e.g. a param)
    else if (base->type && base->type->kind==IRT_PTR && base->type->elem &&
             base->type->elem->kind==IRT_VECTOR)
        clen = base->type->elem->array_len;           // a lane of a vector reached by address (a field)
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
    // ★ CANCEL A SHARED TERM. Writing the second half of a concatenation is
    //
    //     concat(a, b, out i32[a.len + b.len])   ...   out[j + a.len] = b[j]
    //
    // and the obligation `j + a.len < a.len + b.len` is `j < b.len` — which the domain HAS.
    // What it cannot do is get there: the index and the length are both sums, the shared term
    // has to be cancelled, and `len = x + y` is a three-variable fact an octagon cannot hold
    // (`x ± y ≤ c` wants a CONSTANT on the right). The shape machinery does not help either:
    // it models a region as a PRODUCT of extents, and a sum is a partition, a different thing.
    //
    // Cancelling is syntactic and exact. Both sums carry their own overflow obligations, so
    // neither wraps by the time this is asked, and over the integers the cancellation is an
    // identity rather than an approximation.
    if (!c.hi_ok && lenvar >= 0 && w == 1) {
        IrInstr *ixd = (idx>=0 && idx<V->nvar) ? V->def[idx] : NULL;
        IrInstr *lnd = (lenvar>=0 && lenvar<V->nvar) ? V->def[lenvar] : NULL;
        // The canonical length var is whatever named the length first — often a slice_len
        // read, not the `na + nb` that produced it. Anything the domain proves EQUAL to it
        // will do, which is the same move vra_same_value exists for.
        if (!lnd || lnd->op != IR_ADD)
            for (int y=0; y<V->nvar; y++) {
                IrInstr *yd = V->def[y];
                if (!yd || yd->op != IR_ADD || yd->n_operands < 2 || y == lenvar) continue;
                if (vra_same_value(V, W, lenvar, y)) { lnd = yd; break; }
            }
        if (ixd && ixd->op==IR_ADD && ixd->n_operands>=2 && vra_zexact(V, ixd) &&
            lnd && lnd->op==IR_ADD && lnd->n_operands>=2 && vra_zexact(V, lnd)) {
            for (int si=0; si<2 && !c.hi_ok; si++)
                for (int sl=0; sl<2 && !c.hi_ok; sl++) {
                    int shared_i = ixd->operands[si]->id,  other_i = ixd->operands[1-si]->id;
                    int shared_l = lnd->operands[sl]->id,  other_l = lnd->operands[1-sl]->id;
                    if (!vra_same_value(V, W, shared_i, shared_l)) continue;
                    if (vra_diff_ub(V, W, other_i, other_l) <= -1) {   // x − y ≤ −1
                        c.hi_ok = true;
                        if (!c.lo_ok) {                                 // 0 ≤ x and 0 ≤ t ⇒ 0 ≤ idx
                            int64_t xl,xh,tl,th; bool xhl,xhh,thl,thh;
                            vra_interval(V,W,other_i,&xl,&xhl,&xh,&xhh);
                            vra_interval(V,W,shared_i,&tl,&thl,&th,&thh);
                            if (xhl && xl>=0 && thl && tl>=0) c.lo_ok = true;
                        }
                    }
                }
        }
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
    // ★ A LOAD OUT OF AN ARRAY WHOSE CONTENTS ARE KNOWN carries the element range. The domain
    // models indices and forgets what is behind them, so `a[0] + a[3]` over
    // `var a i32[4] = [10,20,30,40]` fell back to the i32 type range on both loads and their
    // sum left i32. See vra_seed_element_ranges for the three conditions that make the join
    // sound; by the time it is consulted the cell is fully written, unescaped, and every
    // stored value is exact.
    if (v->id>=0 && v->id<V->nvar && V->def[v->id] && V->elem_known) {
        IrInstr *ld = V->def[v->id];
        if (ld->op == IR_LOAD && ld->n_operands >= 1) {
            int cell = vra_array_root(V, ld->operands[0]->id);
            if (cell >= 0 && V->elem_known[cell]) {
                if (V->elem_lo[cell] > tlo) tlo = V->elem_lo[cell];
                if (V->elem_hi[cell] < thi) thi = V->elem_hi[cell];
            }
        }
    }
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
        // ...and unsigned -> signed only when it WIDENS: at the same width `u32 4294967295 as%
        // i32` is -1, and this loop intersected the result with [0, 2^32) as if it were
        // value-preserving, so `(x as% i32) - 2147483646` was proven to fit (soundness harness).
        if (!st->is_signed && dt->is_signed && st->bits >= dt->bits) break;
        src = d->operands[0];
        int64_t slo, shi;
        if (!irtype_int_range(st, &slo, &shi)) break;
        if (slo > tlo) tlo = slo;
        if (shi < thi) thi = shi;
    }
    int64_t olo,ohi; bool hl,hh; vra_interval(V, W, v->id, &olo,&hl,&ohi,&hh);
    if (hl && olo>tlo) tlo=olo;
    if (hh && ohi<thi) thi=ohi;
    // ★ INTERVAL ARITHMETIC OVER THE OPERANDS, when the octagon's own answer is looser.
    // The octagon records add/sub results as DIFFERENCE bounds built from `vra_interval` —
    // the octagon's view — so any range that lives only in this function (an element range, a
    // call-site return range, B1's accumulator bound) never reaches an arithmetic RESULT.
    // `for i in 0..3 { s = s + a[i] }` then bounded `s` at [0,90] and still lost
    // `return s + a.len`, because the add's own range came back as the whole i33.
    //
    // Recomputing from the operands' ranges closes that, and it is only ever an INTERSECTION —
    // it can tighten the answer, never widen it. Depth-bounded because the operands are
    // themselves queried this way.
    if (v->id>=0 && v->id<V->nvar && vra_range_depth < 3) {
        IrInstr *d0 = V->def[v->id];
        if (d0 && (d0->op==IR_ADD || d0->op==IR_SUB || d0->op==IR_MUL) && d0->n_operands>=2 && vra_zexact(V, d0)) {
            vra_range_depth++;
            int64_t xlo,xhi,ylo,yhi;
            vra_range(V, W, d0->operands[0], &xlo, &xhi);
            vra_range(V, W, d0->operands[1], &ylo, &yhi);
            vra_range_depth--;
            __int128 rlo, rhi;
            vra_arith_range(d0->op, xlo,xhi, ylo,yhi, &rlo, &rhi);
            if (rlo > (__int128)tlo) tlo = (int64_t)rlo;
            if (rhi < (__int128)thi) thi = (int64_t)rhi;
        }
    }
    // A LOAD out of a loop ACCUMULATOR carries B1's bound. Gated on the structural
    // `accum_cell` marker, so an ordinary range query pays one array read; the widened value
    // usually still HAS octagon bounds (the type interval), just useless ones, which is why
    // this cannot be conditioned on the octagon having left it unbounded.
    if (v->id>=0 && v->id<V->nvar && V->def[v->id]) {
        IrInstr *ld = V->def[v->id];
        if (ld->op == IR_LOAD && ld->n_operands >= 1) {
            int64_t blo, bhi;
            if (vra_cell_accum_range(V, W, ld->operands[0]->id, &blo, &bhi)) {
                if (blo > tlo) tlo = blo;
                if (bhi < thi) thi = bhi;
            }
        }
    }
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
    // ★ THE SAME ARGUMENT DOWNWARD, which was missing — and Path-F needs both halves.
    // `y − v ≤ c` with y bounded below by its type gives `v ≥ ylo − c`. Only the upper half
    // existed, so a value whose type is wide but whose partner is narrow kept the WIDE type's
    // minimum. That is exactly the shape Path-F creates: `TABLE[2] as i32 + 1` on a u8 element
    // is an i33 add, and the obligation is the narrowing back to i32. Its upper bound came out
    // 256 from `%sum − %elem ≤ 1`, while its lower bound stayed i33's −2^32 — so a sum that
    // can only be [1, 256] was not provably an i32, and a test whose whole subject is "no false
    // overflow on a u8 element" failed on the narrowing rather than the add.
    if (!hl && v->id>=0 && v->id<V->nvar) {
        for (int y=0; y<V->nvar; y++) {
            if (y==v->id || !V->val[y] || !V->val[y]->type) continue;
            int64_t c = oct_get(W, oct_pos(v->id), oct_pos(y));   // y − v ≤ c
            if (c >= OCT_INF) continue;
            int64_t ylo, yhi; (void)yhi;
            if (!irtype_int_range(V->val[y]->type, &ylo, &yhi)) continue;
            if (c > 0 && ylo < INT64_MIN + c) continue;           // no wrap in the checker
            if (c < 0 && ylo > INT64_MAX + c) continue;
            int64_t cand = ylo - c;
            if (cand > tlo) tlo = cand;
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
static bool vra_guard_counter_fits(Vra *V, Octagon *W, IrInstr *ins, int64_t thi); // fwd

// Can a value of type `from` fail to fit type `to`? The question every narrowing site is
// really asking, stated once.
//
// ★ IT IS NOT `from->bits > to->bits`, which is what every site used to test. That is a PROXY
// for "the target can hold every value of the source type", and the two part exactly at a SIGN
// CHANGE: `i32 -> u32` is the same width and is not a subset — `func to_unsigned(a i32) u32 {
// return a }` compiled, and a negative `a` becomes a huge positive. Same for `usize <- i65`,
// where Path-F's widened subtraction meets an unsigned slot and `x - 1` at x == 0 wraps to
// SIZE_MAX. Comparing the two types' INTERVALS asks the property directly; the width comparison
// is then just the common case of it.
static bool vra_type_may_lose(const IrType *from, const IrType *to) {
    if (!from || !to || from->kind != IRT_INT || to->kind != IRT_INT) return false;
    int64_t flo, fhi, tlo, thi;
    if (!irtype_int_range(from, &flo, &fhi) || !irtype_int_range(to, &tlo, &thi)) return false;
    // A target that EXCLUDES a value the source may hold can lose it too (`NonZero` from i32).
    if (to->has_ne && !(from->has_ne && from->refine_ne == to->refine_ne) &&
        flo <= to->refine_ne && to->refine_ne <= fhi) return true;
    // ★ u64 HOLDS 2^63 .. 2^64−1, which the i64-based domain clamps away: irtype_int_range gives
    // u64 the range [0, INT64_MAX], and read literally that is inside i64 — so `y i64 = x` on a
    // u64 owed NOTHING, and f(2^64 − 1) returned −1. Only a u64 target holds those values.
    if (!from->is_signed && from->bits >= 64 && fhi == INT64_MAX && !(!to->is_signed && to->bits >= 64))
        return true;
    return !(tlo <= flo && fhi <= thi);       // the target does NOT contain the source's range
}

static void vra_check_narrow(Vra *V, Octagon *W, IrValue *val, IrType *target,
                             IrInstr *at, int64_t line, int64_t col) {
    if (at && at->unchecked) return;                        // inside `unsafe`
    if (!val || !val->type || !target) return;
    if (val->type->kind != IRT_INT || target->kind != IRT_INT) return;
    if (!vra_type_may_lose(val->type, target)) return;      // the target holds every value
    int64_t tlo, thi, vlo, vhi;
    if (!irtype_int_range(target, &tlo, &thi)) return;
    vra_range(V, W, val, &vlo, &vhi);
    VraCheck c; memset(&c,0,sizeof c);
    c.kind = VRA_OVERFLOW; c.at = at; c.line = line; c.col = col;
    c.ok = (vlo >= tlo) && (vhi <= thi);
    // A u64 read as INT64_MAX is UNBOUNDED, not at INT64_MAX (see vra_type_may_lose).
    if (!val->type->is_signed && val->type->bits >= 64 && vhi == INT64_MAX) c.ok = false;
    if (c.ok && target->has_ne) {                          // the excluded value must be excluded
        bool from_ne = val->type->has_ne && val->type->refine_ne == target->refine_ne;
        if (!from_ne && vlo <= target->refine_ne && target->refine_ne <= vhi) c.ok = false;
    }
    // B1: the domain cannot bound a running total, because the bound is a PRODUCT of the trip
    // count and the step. Derive it outside the domain and hand back the interval.
    if (!c.ok) c.ok = vra_accum_info(V, W, val, &c, tlo, thi);
    // NAME THE ONE CASE. A signed `/` below 64 bits is computed one bit wider, and its quotient
    // fits the operands' type except TYPE_MIN / -1. Narrowed to a type that holds the operands'
    // type, that is the only value that can fail, and "arithmetic is not provably free of
    // overflow" did not say so: `b int != 0` reads like the whole precondition of `a / b`.
    if (!c.ok && !c.accum && val->id >= 0 && val->id < V->nvar) {
        IrInstr *d = V->def[val->id];
        int64_t dlo, dhi;
        if (d && d->op == IR_SDIV && d->n_operands >= 2 && d->operands[0] && d->operands[0]->type &&
            irtype_int_range(d->operands[0]->type, &dlo, &dhi) && tlo <= dlo && dhi <= thi)
            c.shift = 5;
    }
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
    // ★ THE MIDPOINT IDENTITY, applied HERE rather than read off the octagon. `a + (b − a)/D`
    // with D >= 1 and `a <= b` lies in [a, b], because the quotient is non-negative and no
    // greater than `b − a`. Both operands are values of this type, so the result is too and the
    // add cannot overflow — `var mid usize = lo + (hi - lo) / 2` under `lo < hi`, which the
    // operand intervals refuse (both are unbounded usize and their sum leaves u64).
    //
    // It must be decided here and not from the octagon's r-bound: the ADD transfer records its
    // relation ASSUMING no wrap, so proving no-wrap from it would be circular. The facts used
    // below are all wrap-free — the subtraction is guarded by `a <= b` and the division only
    // shrinks. The old engine carries the same identity (47a88a2).
    if (!c.ok && ins->op == IR_ADD) {
        for (int side=0; side<2 && !c.ok; side++) {
            IrValue *qv = side ? a : b, *lv = side ? b : a;
            IrInstr *qd = (qv->id>=0 && qv->id<V->nvar) ? V->def[qv->id] : NULL;
            if (!qd || (qd->op!=IR_UDIV && qd->op!=IR_SDIV) || qd->n_operands<2) continue;
            int dv = qd->operands[1]->id;
            if (dv<0 || dv>=V->nvar || !V->cknown[dv] || V->cval[dv] < 1) continue;
            IrInstr *sd2 = V->def[qd->operands[0]->id];
            if (!sd2 || sd2->op!=IR_SUB || sd2->n_operands<2 || !vra_zexact(V, sd2)) continue;
            // The SAME `a`, as a VALUE rather than as an SSA id. `lo + (hi - lo) / 2` lowers to
            // two loads of `lo` (one per mention), so the ids differ whenever `lo` is a variable,
            // and the identity only ever fired where plain intervals already sufficed. Binary
            // search over a SLICE was refused with a false E086 for exactly this reason. Two
            // values the closed octagon holds equal (a - a' <= 0 and a' - a <= 0) are the same
            // number at this point, and SSA values never change afterwards.
            int av = sd2->operands[1]->id;
            if (av != lv->id && !(vra_diff_ub(V, W, av, lv->id) <= 0 &&
                                  vra_diff_ub(V, W, lv->id, av) <= 0)) continue;
            if (vra_diff_ub(V, W, lv->id, sd2->operands[0]->id) > 0) continue;   // a <= b?
            int64_t hlo, hhi; vra_range(V, W, sd2->operands[0], &hlo, &hhi);
            int64_t llo, lhi2; vra_range(V, W, lv, &llo, &lhi2); (void)lhi2;
            if (llo >= tlo && hhi <= thi) c.ok = true;                 // a <= r <= b, both fit
        }
    }
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
    // A loop guard's OWN arithmetic is evaluated before the guard refines anything, so it has
    // to be proved from the loop's shape instead. See vra_guard_counter_fits.
    if (!c.ok) c.ok = vra_guard_counter_fits(V, W, ins, thi);
    if (!c.ok && ins->result) c.ok = vra_accum_info(V, W, ins->result, &c, tlo, thi);
    vra_add_check(V, c);
}
// ★ SHIFTS RAISED NO OBLIGATION IN THIS ENGINE. `x << n` with n unproven, `x >> 32` on a u32, and
// `1 << 31` on an i32 are undefined behaviour in the C the backend emits, and only the front
// end's LEGACY range check refused them — so they were answered by the old engine alone, and
// the IR's verdict was never asked. Two obligations, the ones the legacy check stated:
//   · the AMOUNT is in [0, width-1] (any shift);
//   · a SIGNED left shift fits: `x << n` is `x * 2^n`, and a bit reaching the sign is UB.
// An unsigned left shift discards bits by definition and is not checked. `unsafe` waives both.
static void vra_check_shift(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->unchecked || ins->n_operands < 2) return;
    IrValue *a = ins->operands[0], *b = ins->operands[1];
    // ★ A VECTOR SHIFT HAD NO OBLIGATION: `v << k` with k unproven, and `v << 40` on u32 lanes,
    // compiled — undefined in C lane by lane (UBSan does not instrument vector shifts, so nothing
    // said so). A SCALAR amount is judged like a scalar shift's; an amount per LANE cannot be,
    // since the domain has no lane ranges, so it is refused. The lanes themselves wrap
    // (emitted through the unsigned type), so there is no value question.
    if (a && a->type && a->type->kind == IRT_VECTOR && a->type->elem && a->type->elem->kind == IRT_INT) {
        int lb = a->type->elem->bits;
        VraCheck c; memset(&c,0,sizeof c); c.kind = VRA_OVERFLOW; c.at = ins;
        c.line = ins->line; c.col = ins->col; c.shift = 1;
        if (b && b->type && b->type->kind == IRT_INT) {
            int64_t blo, bhi; vra_range(V, W, b, &blo, &bhi);
            c.ok = (blo >= 0) && (bhi <= lb - 1);
        } else c.ok = false;
        vra_add_check(V, c);
        return;
    }
    if (!a || !b || !a->type || a->type->kind != IRT_INT) return;
    int bits = a->type->bits;
    if (bits <= 0 || bits > 64) return;
    int64_t blo, bhi; vra_range(V, W, b, &blo, &bhi);
    VraCheck c; memset(&c,0,sizeof c); c.kind = VRA_OVERFLOW; c.at = ins;
    c.line = ins->line; c.col = ins->col; c.shift = 1;
    c.ok = (blo >= 0) && (bhi <= bits - 1);
    vra_add_check(V, c);
    if (!c.ok) return;                                   // the value question needs a bounded n
    if (ins->op != IR_SHL) return;
    if (ins->wrap == IR_WRAP_MODULAR) return;            // `<<%`: discarding bits is what it says
    // ★ AN UNSIGNED `<<` MAY NOT LOSE BITS EITHER. `x << k` is x * 2^k, Path F refuses an unproven
    // overflow of `*`, and the signed check below refuses a bit reaching the sign, so silently
    // dropping high bits of an unsigned value was the one hole in the family: `(x << 6) as u8`
    // over x <= 4 dropped 256's bit and read 0. The wrapping shift `<<%` (IR_WRAP_MODULAR) is
    // how a program says it means to discard them.
    if (!a->type->is_signed) {
        IrType *ut = (ins->result && ins->result->type) ? ins->result->type : a->type;
        int64_t ulo, uhi; if (!irtype_int_range(ut, &ulo, &uhi)) return;
        int64_t alo, ahi; vra_range(V, W, a, &alo, &ahi);
        __int128 top = (alo >= 0 && ahi < OCT_INF/2) ? (__int128)ahi * ((__int128)1 << bhi) : ((__int128)1 << 127) - 1;
        VraCheck u; memset(&u,0,sizeof u); u.kind = VRA_OVERFLOW; u.at = ins;
        u.line = ins->line; u.col = ins->col; u.shift = 4;
        u.ok = alo >= 0 && ahi < OCT_INF/2 && top <= (__int128)uhi;
        vra_add_check(V, u);
        return;
    }
    IrType *tt = (ins->result && ins->result->type) ? ins->result->type : a->type;
    int64_t tlo, thi; if (!irtype_int_range(tt, &tlo, &thi)) return;
    int64_t alo, ahi; vra_range(V, W, a, &alo, &ahi);
    __int128 m = (__int128)1 << bhi;                     // the extreme magnitudes are at n = bhi
    __int128 rlo = (__int128)alo * m, rhi = (__int128)ahi * m;
    if (rlo > rhi) { __int128 t = rlo; rlo = rhi; rhi = t; }
    VraCheck v; memset(&v,0,sizeof v); v.kind = VRA_OVERFLOW; v.at = ins;
    v.line = ins->line; v.col = ins->col; v.shift = 2;
    v.ok = (alo > -OCT_INF/2) && (ahi < OCT_INF/2) && rlo >= (__int128)tlo && rhi <= (__int128)thi;
    vra_add_check(V, v);
}

// ★ NEGATION HAD NO OBLIGATION. `-x` on an i32 is exact in ℤ (the transfer above records r = -x),
// but nothing compared that result with its TYPE: `func f(x i32) i32 { return -x }` compiled, and
// f(-2147483648) is UB (UBSan: "negation of -2147483648 cannot be represented"). Unary `-` was
// prove-or-reject in the old engine and the obligation did not survive the move to the IR — the
// same gap the shift checks had. `0 -% x` is the wrapping spelling; `unsafe` waives it.
static void vra_check_neg(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->unchecked || ins->n_operands < 1 || !ins->result) return;
    IrType *tt = ins->result->type; int64_t tlo, thi;
    if (!tt || tt->kind != IRT_INT || !irtype_int_range(tt, &tlo, &thi)) return;
    int64_t xlo, xhi; vra_range(V, W, ins->operands[0], &xlo, &xhi);
    __int128 rlo = -(__int128)xhi, rhi = -(__int128)xlo;
    VraCheck c; memset(&c,0,sizeof c); c.kind = VRA_OVERFLOW; c.at = ins;
    c.line = ins->line; c.col = ins->col;
    c.ok = rlo >= (__int128)tlo && rhi <= (__int128)thi;
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
    // ★ THE GUARD AND THE USE ARE DIFFERENT SSA VALUES. `var d = f()` puts the result in a
    // SLOT; `if d != 0` loads it once and `left / d` loads it again, so matching the compared
    // value by id compared two distinct loads and found nothing. Any divisor that is not a
    // parameter — every `var d = <call>` — therefore failed its guard. Match through the CELL
    // instead: two loads of the same slot are the same value as long as nothing writes the
    // slot in between, which is what the walk below checks.
    IrInstr *vdd = (vid>=0 && vid<V->nvar) ? V->def[vid] : NULL;
    int vcell = (vdd && vdd->op==IR_LOAD && vdd->n_operands>=1) ? vdd->operands[0]->id : -1;

    for (int depth=0; b && depth<64; depth++) {
        // Anything that could write the slot between the guard and the use invalidates it.
        // Deliberately coarse: the whole block, not just the instructions before the use, and
        // any call at all once the cell has escaped.
        //
        // ...and so can a store through a pointer this analysis cannot attribute, once the cell
        // has escaped: `if d != 0 { var r = var d; r = 0; return 100 / d }` — and the same with
        // `unsafe { *q = 0 }` through `q = &d` — kept the guard and divided by zero (SIGFPE).
        // An unattributable store is the call's unknown writer without the call.
        if (vcell >= 0)
            for (IrInstr *q=b->instrs; q; q=q->next) {
                if (q->op==IR_STORE && q->n_operands>=1 && q->operands[0]->id==vcell) return false;
                bool esc = vcell<V->nvar && V->escaped && V->escaped[vcell];
                if (q->op==IR_CALL && esc) return false;
                if (q->op==IR_STORE && q->n_operands>=1 && esc &&
                    vra_arg_cell(V, q->operands[0]) == VRA_ARG_UNKNOWN) return false;
            }
        IrEdge *e = b->preds;
        if (!e || e->next) return false;                     // not a single-predecessor chain
        IrBlock *p = e->block;
        if (!p) return false;
        if (p->term.kind == IR_TERM_BR_COND && p->term.cond) {
            // `if b {` on a bool itself: its then-edge is the guard (an `assert(b)` inside it).
            int cid = p->term.cond->id;
            bool csame = (cid == vid);
            if (!csame && vcell >= 0 && cid>=0 && cid<V->nvar) {
                IrInstr *cd = V->def[cid];
                csame = cd && cd->op==IR_LOAD && cd->n_operands>=1 && cd->operands[0]->id == vcell;
            }
            if (csame && p->term.a == b) return true;
            IrInstr *ic = V->def[p->term.cond->id];
            if (ic && ic->op==IR_ICMP && ic->n_operands>=2) {
                int a = ic->operands[0]->id, z = ic->operands[1]->id;
                bool zero_is_const = (z>=0 && z<V->nvar && V->cknown[z] && V->cval[z]==0);
                bool same = (a == vid);
                if (!same && vcell >= 0 && a>=0 && a<V->nvar) {
                    IrInstr *ad = V->def[a];
                    same = ad && ad->op==IR_LOAD && ad->n_operands>=1 &&
                           ad->operands[0]->id == vcell;
                }
                if (same && zero_is_const) {
                    if (ic->aux.cmp==IR_CMP_NE && p->term.a == b) return true;   // `if d != 0 {`
                    if (ic->aux.cmp==IR_CMP_EQ && p->term.b == b) return true;   // `if d == 0 { return }`
                }
            }
        }
        b = p;
    }
    return false;
}
static int vra_sdiv_may_trap_count = 0;   // measurement: divisions the STRICT rule would refuse
static void vra_check_divzero(Vra *V, Octagon *W, IrInstr *ins, IrBlock *at) {
    if (ins->n_operands<2 || ins->unchecked) return;         // `unsafe` waives it, as for bounds
    int64_t lo,hi; vra_range(V,W,ins->operands[1],&lo,&hi);
    // ★ SIGNED TYPE_MIN / -1 overflows (UB; SIGFPE on x86). Stated here as the legacy front-end
    // check stated it — the divisor is provably EXACTLY -1 and the dividend can reach TYPE_MIN —
    // so this engine refuses what that check refused. (A divisor that merely MAY be -1 is not
    // refused: that is the language's current, deliberate trade, recorded in plan Part 7G.)
    // ★ DECIDE-M (Marco, 2026-09-28). The divisor used to have to be PROVABLY exactly -1 for this
    // to be refused, so a possibly-TYPE_MIN dividend over a possibly-(-1) divisor compiled to a
    // plain C division: SIGFPE at -O0, a silent value at -O2. Now the obligation is the fact
    // itself — not (dividend = TYPE_MIN and divisor = -1) — and it is owed only where the
    // division happens at the operand's own width: a widened `/` (i32 / i32 : i33) and a `%`
    // below 64 bits are computed where the case is an ordinary value, and `/%` `/|` define it.
    if ((ins->op==IR_SDIV || ins->op==IR_SREM) && ins->wrap == IR_WRAP_CHECK &&
        ins->operands[0] && ins->operands[0]->type &&
        ins->operands[0]->type->kind==IRT_INT && ins->operands[0]->type->is_signed) {
        IrType *dt = ins->operands[0]->type, *rt = ins->result ? ins->result->type : NULL;
        bool widened = rt && rt->kind==IRT_INT && rt->bits > dt->bits;
        int64_t tlo, thi, alo, ahi;
        if (!widened && irtype_int_range(dt, &tlo, &thi)) {
            vra_range(V, W, ins->operands[0], &alo, &ahi);
            bool divisor_may_be_m1 = (lo <= -1 && -1 <= hi);
            if (divisor_may_be_m1 && alo <= tlo) vra_sdiv_may_trap_count++;
            VraCheck m; memset(&m,0,sizeof m); m.kind = VRA_OVERFLOW; m.at = ins;
            m.line = ins->line; m.col = ins->col; m.shift = 3;
            m.ok = !(divisor_may_be_m1 && alo <= tlo);
            vra_add_check(V, m);
        }
    }
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_DIVZERO; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = (lo>0) || (hi<0);                                // 0 ∉ [lo,hi]
    // The divisor as the program wrote it: a signed `%` is computed on a WIDENED copy
    // (DECIDE-M), and a widening cast keeps zero and non-zero exactly, so the facts about the
    // original — its `!= 0` type, a guard on it — are facts about the copy.
    IrValue *dv = ins->operands[1];
    for (int g = 0; g < 4 && dv && dv->id >= 0 && dv->id < V->nvar; g++) {
        IrInstr *dd = V->def[dv->id];
        if (!dd || dd->op != IR_CAST || dd->n_operands < 1 ||
            (dd->aux.cast_kind != IR_CAST_SEXT && dd->aux.cast_kind != IR_CAST_ZEXT)) break;
        dv = dd->operands[0];
    }
    { IrType *dt = dv ? dv->type : NULL;                     // a `!= 0` TYPE is a proof by itself
      if (!c.ok && dt && dt->has_ne && dt->refine_ne == 0) c.ok = true; }
    if (!c.ok && dv) c.ok = vra_guarded_nonzero(V, at, dv->id);
    vra_add_check(V, c);
}

// @ctz / @clz OF ZERO. The C is __builtin_ctz / __builtin_clz, undefined at 0 (bsf leaves its
// destination unchanged), and the interpreter calls it a failed proof, but nothing asked for one:
// `@ctz(x)` over any u32 compiled, and `tz(0)` returned whatever the register held. The argument
// owes exactly what a divisor owes, from the same three sources (found by the per-operation
// soundness harness, src/tools/soundness_driver.c).
static void vra_check_bitcount(Vra *V, Octagon *W, IrInstr *ins, IrBlock *at) {
    if (ins->n_operands < 1 || ins->unchecked) return;      // `unsafe` waives it, as for a divisor
    int64_t lo, hi; vra_range(V, W, ins->operands[0], &lo, &hi);
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_DIVZERO; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.bitcount = true;
    c.ok = (lo>0) || (hi<0);                                // 0 ∉ [lo,hi]
    IrValue *xv = ins->operands[0];
    IrType *xt = xv ? xv->type : NULL;                      // a `!= 0` type
    if (!c.ok && xt && xt->has_ne && xt->refine_ne == 0) c.ok = true;
    if (!c.ok && xv) c.ok = vra_guarded_nonzero(V, at, xv->id);   // a guard on the path
    vra_add_check(V, c);
}

// A subslice `src[lo..hi]` lowered to make_slice(elem_ptr(src_data,lo), hi−lo) is in
// bounds iff 0 ≤ lo AND hi ≤ len(src). (The start elem_ptr is NOT an element access,
// so it is skipped in vra_check_elem; the real obligation is checked here.)
static void vra_check_subslice(Vra *V, Octagon *W, IrInstr *ms) {
    if (ms->unchecked) return;                              // inside `unsafe`, as for vra_check_elem
    if (ms->n_operands<2) return;
    IrInstr *ndd = V->def[ms->operands[0]->id];
    if (!ndd || ndd->op!=IR_ELEM_PTR || ndd->n_operands<2) return;    // array→slice decay, not a subslice
    int lo = ndd->operands[1]->id;
    IrInstr *bd = V->def[ndd->operands[0]->id];
    int64_t clen=-1; int lenvar=-1;
    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty && bd->aux.alloca_ty->kind==IRT_ARRAY) clen=bd->aux.alloca_ty->array_len;
    else if (bd && bd->op==IR_SLICE_DATA && bd->n_operands>=1){ int s=bd->operands[0]->id; if(V->slicelen[s]>=0) lenvar=V->slicelen[s]; }
    // ...or the base is an ARRAY VALUE, whose length is in its own type. A fixed-array
    // PARAMETER has no defining instruction, so neither branch above sees it — and the
    // array->slice decay at a call site builds exactly this shape: elem_ptr(param, 0) feeding
    // a make_slice. vra_check_elem already reads the length from the type here; this check did
    // not, so the decay reported "no length is known here" for a length written in the
    // signature. Same resolution, same place, one branch apart.
    else if (ndd->operands[0]->type && ndd->operands[0]->type->kind==IRT_ARRAY)
        clen = ndd->operands[0]->type->array_len;
    int hi=-1;
    IrInstr *lend = V->def[ms->operands[1]->id];
    if (lend && lend->op==IR_SUB && lend->n_operands>=2 && vra_zexact(V, lend) && lend->operands[1]->id==lo) hi=lend->operands[0]->id; // len = hi − lo
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_BOUNDS; c.at=ms; c.line=ms->line; c.col=ms->col;
    int64_t llo,lhi; bool lhl,lhh; vra_interval(V, W,lo,&llo,&lhl,&lhi,&lhh);
    c.lo_ok = lhl && llo>=0;                                          // 0 ≤ lo
    c.has_len = (clen>=0 || lenvar>=0);
    // ...or the LENGTH is given directly rather than as `hi - lo`. The array->slice decay
    // builds make_slice(elem_ptr(base, 0), N) with N a constant, and the `hi = len + lo`
    // recovery below only fires for a SUB — so a whole-array decay, the most obviously
    // in-bounds slice there is, fell to the conservative branch and was refused. In bounds
    // iff lo + len <= clen, which is the same obligation stated the other way round.
    int64_t dlo, dhi; bool dhl, dhh;
    vra_interval(V, W, ms->operands[1]->id, &dlo,&dhl,&dhi,&dhh);
    if (hi<0 && clen>=0 && dhh && lhh && dhi>=0 && lhi>=0 && lhi <= clen - dhi) c.hi_ok = true;
    else if (hi<0) c.hi_ok=false;                                     // couldn't recover hi ⇒ conservative
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
static bool vra_guarded_nonzero(Vra *V, IrBlock *b, int vid);   // fwd
static void vra_check_assert(Vra *V, Octagon *W, IrInstr *ins, IrBlock *at) {
    if (ins->n_operands<1) return;
    IrInstr *ic = V->def[ins->operands[0]->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) {
        // ★ ANY OTHER CONDITION OWES THE PROOF TOO. This returned without recording an obligation,
        // so `assert(b)` on a bool parameter compiled (the interpreter: "an obligation the analysis
        // discharged does not hold"), and so did the refusal of a contract the call site cannot
        // evaluate, an assert of `false` (ir_assert_unresolvable). Proven only when the condition
        // is known to be true: a guard, an entry assume, or a constant.
        VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_PRECOND; c.at=ins; c.line=ins->line; c.col=ins->col;
        c.diag = (int)ins->aux.imm;
        int64_t lo, hi; vra_range(V, W, ins->operands[0], &lo, &hi);
        c.ok = lo >= 1 || vra_guarded_nonzero(V, at, ins->operands[0]->id);
        vra_add_check(V, c);
        return;
    }
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_PRECOND; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.diag = (int)ins->aux.imm;
    c.ok = vra_icmp_holds(V, W, ic->operands[0]->id, ic->operands[1]->id, ic->aux.cmp);
    // `x != k` cannot be read off an octagon (a hole is not a difference bound). It holds when
    // the value's TYPE excludes k, or — for k == 0 — when an entry assume or a dominating guard
    // says so: the same facts the division check reads. This is what lets a `d != 0` parameter
    // be forwarded to another `!= 0` parameter.
    if (!c.ok && ic->aux.cmp == IR_CMP_NE) {
        IrValue *x = ic->operands[0], *k = ic->operands[1];
        bool kc = k && k->id>=0 && k->id<V->nvar && V->cknown[k->id];
        int64_t kv = kc ? V->cval[k->id] : 0;
        if (kc && x->type && x->type->has_ne && x->type->refine_ne == kv) c.ok = true;
        if (!c.ok && kc && kv == 0) c.ok = vra_guarded_nonzero(V, at, x->id);
        if (!c.ok && kc) {                                   // [lo,hi] that misses k
            int64_t lo, hi; vra_range(V, W, x, &lo, &hi);
            if (kv < lo || kv > hi) c.ok = true;
        }
    }
    vra_add_check(V, c);
}

// ── termination consumer (a func must have only terminating loops) ───────────
static bool vra_is_scalar_cell(Vra *V, int v) {
    IrInstr *d=(v>=0&&v<V->nvar)?V->def[v]:NULL;
    return d && d->op==IR_ALLOCA && d->aux.alloca_ty &&
           d->aux.alloca_ty->kind!=IRT_ARRAY && d->aux.alloca_ty->kind!=IRT_SLICE;
}
static void vra_natural_loop(Vra *V, IrBlock *H, int nb, char *inloop);   // fwd

// A guard bound is loop-invariant if it is a parameter, a constant, a slice length,
// or defined in a block strictly above the loop header (structured CFG order).
//
// ★ ...and ALSO when it is a LOAD from a scalar cell the loop never writes. `for i in
// 0..arr.len` proved termination and `var n = arr.len; for i in 0..n` did not: binding the
// bound to a local puts its LOAD in the header itself, so the block-order test failed on a
// value nothing in the loop can change. Two of the four termination losses the A1 survey
// found were exactly this, both in ordinary code (`reverse`, `last_n`) — the block-order
// test is a proxy for "cannot change", and a load is where the proxy and the property part.
//
// Sound because both ways the cell could change are excluded: a STORE anywhere in the natural
// loop, and a call writing through an ESCAPED address. `V->escaped` is precisely the set the
// memory model already havocs at every call, for this same reason.
static bool vra_cell_opaque_write(Vra *V, int cell, int nbb, const char *inloop);   // fwd
static bool vra_loop_invariant_d(Vra *V, IrValue *val, IrBlock *H, int depth);
static bool vra_loop_invariant(Vra *V, IrValue *val, IrBlock *H) {
    return vra_loop_invariant_d(V, val, H, 0);
}
static bool vra_loop_invariant_d(Vra *V, IrValue *val, IrBlock *H, int depth) {
    int Hid = H->id;
    IrInstr *d=V->def[val->id];
    if (!d) return true;
    if (d->op==IR_CONST || d->op==IR_SLICE_LEN) return true;
    // ── AN OPERATION IS INVARIANT WHEN ITS OPERANDS ARE ─────────────────────────────────
    // The textbook definition, and its absence refused a whole family of ordinary loops:
    // `while i < n / 2` (two-pointer reverse) computes its BOUND in the header block, so the
    // "defined above the header" test fails and the bound was treated as varying — even though
    // nothing in the loop writes `n`. The same for `while i < h * w` and every guard whose
    // limit is arithmetic over loop-invariant values.
    //
    // Pure arithmetic only: no loads (the cell case below has its own, stronger test), no
    // calls, nothing that can read memory the loop writes. Depth-capped because this is a walk
    // over a DAG and a cap is cheaper than a visited set for the shapes that occur.
    if (depth < 8) {
        switch (d->op) {
            case IR_ADD: case IR_SUB: case IR_MUL:
            case IR_UDIV: case IR_SDIV: case IR_UREM: case IR_SREM:
            case IR_AND: case IR_OR: case IR_XOR:
            case IR_SHL: case IR_LSHR: case IR_ASHR:
            case IR_NEG: case IR_BNOT: case IR_CAST: {
                for (int q=0; q<d->n_operands; q++)
                    if (!d->operands[q] || !vra_loop_invariant_d(V, d->operands[q], H, depth+1))
                        return false;
                return true;
            }
            default: break;
        }
    }
    if (V->defblk[val->id]>=0 && V->defblk[val->id] < Hid) return true;
    if (d->op==IR_LOAD && d->n_operands>=1) {
        int cell = d->operands[0]->id;
        // ★ ...AND A FIELD the loop never writes: `while b.len < b.cap`, `while l.pos < l.end`.
        // Its stores are to the CANONICAL cell (a fresh field_ptr per mention), a whole-struct
        // store to the base writes it too, and so does anything that writes the base opaquely —
        // the same three writers the counter rule counts.
        int fbase = vra_is_scalar_cell(V, cell) ? -1 : vra_field_cell_base(V, cell);
        if (fbase >= 0) {
            int ccell = vra_canon_cell(V, cell);
            int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
            char *body = malloc((size_t)nbb);
            if (!body) return false;
            vra_natural_loop(V, H, nbb, body);
            bool written = false;
            for (IrBlock *b=V->f->blocks; b && !written; b=b->next) {
                if (!(b->id>=0 && b->id<nbb && body[b->id])) continue;
                for (IrInstr *st=b->instrs; st; st=st->next)
                    if (st->op==IR_STORE && st->n_operands>=2 &&
                        (st->operands[0]->id == fbase ||
                         vra_canon_cell(V, st->operands[0]->id) == ccell)) { written = true; break; }
            }
            if (!written) written = vra_cell_opaque_write(V, fbase, nbb, body);
            free(body);
            return !written;
        }
        if (!vra_is_scalar_cell(V, cell)) return false;
        if (cell>=0 && cell<V->nvar && V->escaped && V->escaped[cell]) return false;
        int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
        char *body = malloc((size_t)nbb);
        if (!body) return false;                       // fail closed
        vra_natural_loop(V, H, nbb, body);
        bool written = false;
        for (IrBlock *b=V->f->blocks; b && !written; b=b->next) {
            if (!(b->id>=0 && b->id<nbb && body[b->id])) continue;
            for (IrInstr *st=b->instrs; st; st=st->next)
                if (st->op==IR_STORE && st->n_operands>=2 &&
                    st->operands[0]->id==cell) { written = true; break; }
        }
        free(body);
        return !written;
    }
    return false;
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
    // ── A BACK EDGE NEEDS DOMINANCE, NOT REACHABILITY ───────────────────────────────────
    // "a pred of H that H can reach" is not a back edge, and in a NESTED loop it is wrong in a
    // way that quietly refused every nested program in the corpus. From the inner header,
    // control reaches the inner exit, the outer latch, the outer header and back into the outer
    // BODY — which is also a pred of the inner header. So the outer body was classified as a
    // back-edge source and the inner loop's "natural loop" swallowed the whole outer body,
    // including the `var c = 0` that initialises the inner counter. That store is not progress,
    // every store must be progress, and the inner loop was refused. The outer one proved, which
    // is why the symptom was always "the first obligation passes and the second does not".
    //
    // H dominates P exactly when P is NOT reachable from the entry while avoiding H — one DFS,
    // and it is the definition rather than a proxy for it. Deliberately not an id-order test:
    // this file has been bitten twice by "later id means inside the loop" (the widening
    // selector, and the store scan this same function feeds).
    char *avoid = calloc((size_t)nb, 1);
    if (avoid) {
        sp = 0;
        if (V->f->entry && V->f->entry->id>=0 && V->f->entry->id<nb && V->f->entry != H) {
            avoid[V->f->entry->id] = 1; st[sp++] = V->f->entry;
        }
        while (sp > 0) {
            IrBlock *u = st[--sp];
            IrBlock *sv[2]; int ns = 0;
            if (u->term.kind==IR_TERM_BR)            { if (u->term.a) sv[ns++]=u->term.a; }
            else if (u->term.kind==IR_TERM_BR_COND)  { if (u->term.a) sv[ns++]=u->term.a;
                                                       if (u->term.b) sv[ns++]=u->term.b; }
            else if (u->term.kind==IR_TERM_SWITCH) {
                if (u->term.a) sv[ns++]=u->term.a;
                for (IrSwitchCase *c=u->term.cases; c; c=c->next)
                    if (c->target && c->target->id>=0 && c->target->id<nb
                        && c->target!=H && !avoid[c->target->id])
                        { avoid[c->target->id]=1; st[sp++]=c->target; }
            }
            for (int k=0;k<ns;k++)
                if (sv[k] && sv[k]!=H && sv[k]->id>=0 && sv[k]->id<nb && !avoid[sv[k]->id])
                    { avoid[sv[k]->id]=1; st[sp++]=sv[k]; }
        }
    }
    // backward closure from the back-edge sources, stopping at H
    inloop[H->id] = 1; sp = 0;
    for (IrEdge *e=H->preds; e; e=e->next)
        if (e->block && e->block->id>=0 && e->block->id<nb && fwd[e->block->id]
            && (!avoid || !avoid[e->block->id])          // H must DOMINATE the source
            && !inloop[e->block->id])
            { inloop[e->block->id]=1; st[sp++]=e->block; }
    while (sp > 0) {
        IrBlock *u = st[--sp];
        for (IrEdge *e=u->preds; e; e=e->next)
            if (e->block && e->block->id>=0 && e->block->id<nb && !inloop[e->block->id])
                { inloop[e->block->id]=1; st[sp++]=e->block; }
    }
    free(fwd); free(st); free(avoid);
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
            if (!sub || sub->op!=IR_SUB || sub->n_operands<2 || !vra_zexact(V, sub)) return false;
            IrInstr *ls = V->def[sub->operands[0]->id];
            int one = sub->operands[1]->id;
            return ls && ls->op==IR_LOAD && ls->n_operands>=1 && ls->operands[0]->id==cell
                && one>=0 && one<V->nvar && V->cknown[one] && V->cval[one]==1;
        }
        default: return false;
    }
}
// ★ CAN ANYTHING THIS SCAN CANNOT SEE CHANGE `cell` WHILE THE LOOP RUNS?
// Every rule below recovers a per-iteration fact by pattern-matching IR_STOREs — the counter's
// step, the trip count, the accumulator's starting value. A call handed the cell's ADDRESS
// writes it with no store to find, so all three read a clean per-iteration update that the
// program does not actually have. That produced three separate FALSE PROOFS, of termination
// and of no-overflow.
//
// `V->escaped` is the same set the memory model already havocs at every call, and if nothing
// in the loop calls anything then nothing can exercise the escape while the loop runs — so
// the guard costs no precision on the ordinary case, where a counter's address never leaves.
//
// A call is not the only thing that can exercise the escape. A store through a pointer this
// analysis cannot attribute — a raw `*q = 0` with `q = &i`, or a reference binding's write —
// moves the counter with no store TO THE CELL to find: `while i < 10 { unsafe { *q = 0 }
// i = i + 1 }` read as a clean +1 per iteration and was PROVEN TERMINATING. It never ends.
static bool vra_cell_opaque_write(Vra *V, int cell, int nbb, const char *inloop) {
    if (cell < 0 || cell >= V->nvar || !V->escaped || !V->escaped[cell]) return false;
    for (IrBlock *b=V->f->blocks; b; b=b->next) {
        if (!(b->id>=0 && b->id<nbb && inloop[b->id])) continue;
        for (IrInstr *st=b->instrs; st; st=st->next) {
            // A call writes the cell only if it is HANDED it (in a position the callee writes),
            // or reaches it through a stash — the IR_CALL transfer's own two questions. Any call
            // at all was the old answer, which refused `while l.pos < l.src.len { if
            // !is_space(l.src[l.pos]) { return } l.pos = l.pos + 1 }` — the lexer's loop.
            if (st->op==IR_CALL) {
                IrFunc *cal = vra_find_func(st->aux.callee);
                if (!cal || !vra_mod || !ir_func_writes_only_owned(cal, vra_mod)) return true;
                IrWriteFootprint cw = ir_param_writes(cal, vra_mod);
                for (int k=0; k<st->n_operands; k++) {
                    if (k < 64 && !((cw>>k)&1u)) continue;
                    int ac = vra_arg_cell(V, st->operands[k]);
                    if (ac == cell || ac == VRA_ARG_UNKNOWN) return true;
                }
            }
            if (st->op==IR_STORE && st->n_operands>=1 &&
                vra_arg_cell(V, st->operands[0]) == VRA_ARG_UNKNOWN) return true;
        }
    }
    return false;
}

// Successors of a block, as a small array. (Two at most: the CFG has no switch.)
static int vra_succs(IrBlock *b, IrBlock **out) {
    int n=0;
    if (b->term.kind==IR_TERM_BR)          { if (b->term.a) out[n++]=b->term.a; }
    else if (b->term.kind==IR_TERM_BR_COND){ if (b->term.a) out[n++]=b->term.a;
                                             if (b->term.b) out[n++]=b->term.b; }
    return n;
}

// Does EVERY path from the header around to the header pass through a block that makes
// progress? A forward MUST analysis over the natural loop:
//
//   seen[H] = false                                   (an iteration starts having done nothing)
//   seen[b] = AND over in-loop preds q of ( seen[q] OR prog[q] )
//   terminates iff every back-edge source q satisfies seen[q] OR prog[q]
//
// The direction matters and the obvious backward reading is WRONG: "every path FROM b reaches
// an update" fails on the join block after a two-armed `if`, where the update has already
// happened upstream. Asking whether it has happened YET is the question that composes.
//
// A successor outside the loop is not a constraint — that path leaves, which is termination,
// not a failure to progress. Optimistic initialisation (true everywhere but H) with an AND
// meet is the greatest fixpoint, which is the correct one for "on all paths": an inner cycle
// with no progress drives itself to false rather than assuming its own conclusion.
// ★ AN EDGE THE STATE MAKES IMPOSSIBLE IS NOT A PATH. `if i >= n { break }` then
// `if i < n { … }` is the lexer's shape, and the second guard's false edge cannot be taken — the
// fixpoint knows it (its refined state is ⊥ and the edge is dropped there). This walk read the
// CFG alone, so that dead edge was a path around the loop with no progress on it, and the loop
// was refused. An edge counts only if the converged state, replayed through its source and
// refined by the edge's guard, is not ⊥ — the fixpoint's own test. An unreached block has none.
static bool vra_edge_live(Vra *V, IrBlock *q, int k) {
    if (!V->in || !V->reached || !V->reached[q->id] || !V->in[q->id]) return false;
    if (q->term.kind != IR_TERM_BR_COND || !q->term.cond) return true;
    int64_t *sc = malloc((size_t)V->dsz*8);
    if (!sc) return true;                                     // unknown: keep the edge
    memcpy(sc, V->in[q->id], (size_t)V->dsz*8);
    Octagon E = { V->noct, 2*V->noct, sc };
    oct_close(&E);
    for (IrInstr *x=q->instrs; x; x=x->next) vra_transfer_instr(V,&E,x);
    oct_close(&E);
    vra_refine_guard(V, &E, q->term.cond, k==0);
    oct_close(&E);
    bool live = !oct_is_bottom(&E);
    free(sc);
    return live;
}
static bool vra_progress_on_every_path(Vra *V, IrBlock *H, int nbb,
                                       const char *inloop, const char *prog) {
    char *seen = malloc((size_t)nbb);
    if (!seen) return false;                                  // fail closed
    char *live = malloc((size_t)nbb*2);                       // live[q*2+k]: edge q → succ k
    if (!live) { free(seen); return false; }
    for (IrBlock *q=V->f->blocks; q; q=q->next) {
        if (!(q->id>=0 && q->id<nbb && inloop[q->id])) continue;
        live[q->id*2] = vra_edge_live(V, q, 0); live[q->id*2+1] = vra_edge_live(V, q, 1);
    }
    for (int i=0;i<nbb;i++) seen[i]=1;
    if (H->id>=0 && H->id<nbb) seen[H->id]=0;
    for (int round=0; round<=nbb+1; round++) {
        bool changed=false;
        for (IrBlock *b=V->f->blocks; b; b=b->next) {
            if (!(b->id>=0 && b->id<nbb && inloop[b->id])) continue;
            if (b->id==H->id) continue;
            char v=1;
            for (IrBlock *q=V->f->blocks; q && v; q=q->next) {
                if (!(q->id>=0 && q->id<nbb && inloop[q->id])) continue;
                IrBlock *sc[2]; int ns=vra_succs(q,sc);
                bool is_pred=false;
                for (int k=0;k<ns;k++) if (sc[k] && sc[k]->id==b->id && live[q->id*2+k]) is_pred=true;
                if (!is_pred) continue;
                if (!(seen[q->id] || prog[q->id])) v=0;
            }
            if (v!=seen[b->id]) { seen[b->id]=v; changed=true; }
        }
        if (!changed) break;
    }
    bool any=false, anydead=false, ok=true;
    for (IrBlock *q=V->f->blocks; q && ok; q=q->next) {
        if (!(q->id>=0 && q->id<nbb && inloop[q->id])) continue;
        IrBlock *sc[2]; int ns=vra_succs(q,sc);
        for (int k=0;k<ns;k++) {
            if (!sc[k] || sc[k]->id!=H->id) continue;
            if (!live[q->id*2+k]) { anydead=true; continue; }
            any=true;
            if (!(seen[q->id] || prog[q->id])) ok=false;
        }
    }
    free(seen); free(live);
    // No LIVE back edge while some exist: the body cannot return to the header, so the loop
    // runs at most once — `while i < n { if i < n { return … } i = i + 1 }`. It terminates.
    return ok && (any || anydead);
}

// `a CMP b` read as `b CMP' a`. Needed because the counter may sit on EITHER side of the
// guard, and every direction test below is written from the counter's point of view.
static IrCmp vra_cmp_swap(IrCmp c) {
    switch (c) {
        case IR_CMP_SLT: return IR_CMP_SGT;   case IR_CMP_SGT: return IR_CMP_SLT;
        case IR_CMP_SLE: return IR_CMP_SGE;   case IR_CMP_SGE: return IR_CMP_SLE;
        case IR_CMP_ULT: return IR_CMP_UGT;   case IR_CMP_UGT: return IR_CMP_ULT;
        case IR_CMP_ULE: return IR_CMP_UGE;   case IR_CMP_UGE: return IR_CMP_ULE;
        default:         return c;            // EQ / NE are symmetric
    }
}

// ── A TWO-ENDPOINT MEASURE: `while lo < hi { ... lo = mid+1 ... hi = mid ... }` ──────────
// The rule above tracks ONE counter against a loop-INVARIANT bound. Binary search has neither:
// both endpoints are written in the loop, and which one moves depends on the branch. What
// shrinks is the DIFFERENCE `hi - lo`, and the corpus says so out loud — those loops carry
// `decreasing hi - lo` — so refusing them made the engine unable to prove the very measure the
// programmer wrote.
//
// It is the same algebra the RECURSION rule uses one level up, and for the same reason it is
// affordable: a difference is a four-variable relation in general, but each store site moves
// exactly ONE endpoint and leaves the other alone, so every question collapses to a single
// difference the octagon already relates:
//
//     a store to `lo`  is progress iff the new value is strictly GREATER than the old
//     a store to `hi`  is progress iff the new value is strictly LESS    than the old
//
// Both are read from the converged loop state replayed to the store, so they hold on every
// iteration rather than the first. Groundedness is the guard itself: `lo < hi` is what stops
// the difference falling below zero, and it is re-tested at the header every time round.
//
// Soundness rests on the same three conditions as the single-counter rule, and they are not
// relaxed: EVERY store to either cell must be progress (so no path can undo one), every path
// around the loop must pass through at least one (the `prog` dataflow), and neither cell may be
// written through an escaped address by a call.
static bool vra_loop_terminates_pair(Vra *V, IrBlock *H) {
    if (H->term.kind != IR_TERM_BR_COND) return false;
    IrInstr *ic = V->def[H->term.cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return false;
    // ★ INSTALL THE PACKING. `oct_map` translates a VALUE id into the octagon's packed slot,
    // and the domain's own operations index with it. vra_analyze installs it for the duration
    // of its own work — but this rule also runs from analysis/effects.h, which asks about
    // totality OUTSIDE that window, and there `oct_map` is whatever the last caller left. A
    // transfer replayed without it indexes a dim-sized matrix with raw value ids and writes off
    // the end: an ASan heap-buffer-overflow in oct_forget, reached only through the emitter's
    // annotation path. The corpus never saw it; `emit_gate` did.
    const int *oct_map_saved_pair = oct_map; oct_map = V->odim;
    bool result_pair = false;
    for (int side=0; side<2; side++) {
        IrCmp p = (side==0) ? ic->aux.cmp : vra_cmp_swap(ic->aux.cmp);
        bool lt = (p==IR_CMP_SLT||p==IR_CMP_ULT||p==IR_CMP_SLE||p==IR_CMP_ULE);
        if (!lt) continue;                                  // read as `low < high`
        IrInstr *dlo = V->def[ic->operands[side]->id];
        IrInstr *dhi = V->def[ic->operands[side^1]->id];
        if (!dlo || dlo->op!=IR_LOAD || dlo->n_operands<1) continue;
        if (!dhi || dhi->op!=IR_LOAD || dhi->n_operands<1) continue;
        int clo = dlo->operands[0]->id, chi = dhi->operands[0]->id;
        if (clo==chi) continue;
        if (!vra_is_scalar_cell(V,clo) || !vra_is_scalar_cell(V,chi)) continue;
        int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
        char *body = malloc((size_t)nbb); if (!body) continue;
        vra_natural_loop(V, H, nbb, body);
        char *prog = calloc((size_t)nbb,1); if (!prog) { free(body); continue; }
        bool bad=false, anyprog=false;
        for (IrBlock *b=V->f->blocks; b && !bad; b=b->next) {
            if (!(b->id>=0 && b->id<nbb && body[b->id])) continue;
            for (IrInstr *st=b->instrs; st && !bad; st=st->next) {
                if (st->op!=IR_STORE || st->n_operands<2) continue;
                int tgt = st->operands[0]->id;
                if (tgt!=clo && tgt!=chi) continue;
                int nv = st->operands[1]->id;
                if (nv<0 || nv>=V->nvar || !V->in[b->id]) { bad=true; break; }
                int64_t *sc = malloc((size_t)V->dsz*8);
                if (!sc) { bad=true; break; }
                memcpy(sc, V->in[b->id], (size_t)V->dsz*8);
                Octagon SW = { V->noct, 2*V->noct, sc };
                oct_close(&SW);
                for (IrInstr *q=b->instrs; q && q!=st; q=q->next) vra_transfer_instr(V,&SW,q);
                oct_close(&SW);
                // the OLD value of the cell being written, as the guard's load names it
                int old = (tgt==clo) ? dlo->result->id : dhi->result->id;
                bool ok_step = (tgt==clo) ? (vra_diff_ub(V,&SW,old,nv) <= -1)   // lo rises
                                          : (vra_diff_ub(V,&SW,nv,old) <= -1);  // hi falls
                free(sc);
                if (!ok_step) { bad = true; break; }
                prog[b->id] = 1; anyprog = true;
            }
        }
        bool ok = false;
        if (!bad && anyprog
            && !vra_cell_opaque_write(V, clo, nbb, body)
            && !vra_cell_opaque_write(V, chi, nbb, body))
            ok = vra_progress_on_every_path(V, H, nbb, body, prog);
        free(body); free(prog);
        if (ok) {
            vra_last_measure = (VraMeasure){ VRA_MEAS_PAIR, ic->operands[side], ic->operands[side^1] };
            result_pair = true; break;
        }
    }
    oct_map = oct_map_saved_pair;
    return result_pair;
}

static bool vra_loop_terminates(Vra *V, IrBlock *H) {
    vra_last_measure.kind = VRA_MEAS_NONE;
    if (H->term.kind != IR_TERM_BR_COND) return false;
    IrInstr *ic = V->def[H->term.cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return false;
    IrCmp p0 = ic->aux.cmp;
    // See the note in vra_loop_terminates_pair: this rule is also asked from effects.h, outside
    // vra_analyze's window, so it must install the packing map itself before replaying any
    // transfer. The variable-step rule below is what made that necessary — until it existed,
    // this function never touched an octagon.
    const int *oct_map_saved_loop = oct_map; oct_map = V->odim;
    bool result_loop = false;
    for (int side=0; side<2; side++) {
        IrValue *ivv=ic->operands[side], *bnd=ic->operands[side^1];
        // ★ THE PREDICATE IS READ FROM THE COUNTER'S SIDE. `p0` relates operands[0] to
        // operands[1]; when the counter is operands[1] the relation has to be turned round,
        // and it was not. That is a FALSE PROOF, not a missed one: `while 0 < i { i = i + 1 }`
        // lowers to `icmp.slt 0, i`, was read as `i < 0`, and a rising step then satisfied the
        // "counts up toward a bound" rule — the engine proved an INFINITE loop terminating.
        // Not reachable from source today only because the old front end refuses that shape
        // with [E082] first, which is the accident this relies on until 3.5.
        IrCmp p = (side==0) ? p0 : vra_cmp_swap(p0);
        IrInstr *ivd=V->def[ivv->id];
        // ── THE COUNTER MAY CARRY A CONSTANT OFFSET ─────────────────────────────────────
        // `while i + 1 < n` is the sliding-window guard, and it is the spelling the corpus
        // PREFERS: `i < n - 1` underflows at n == 0 on a usize and was replaced everywhere for
        // exactly that reason (C-7). The rule matched only a BARE load, so the ubiquitous form
        // was refused while the dangerous one proved.
        //
        // Peeling is sound because adding a loop-invariant constant is MONOTONE: `i + k` rises
        // exactly when `i` rises, so the predicate's direction is unchanged and the bound moves
        // by a constant. Progress is then checked on the CELL, as before — which is the part
        // that must not be relaxed, because it is what the counter actually is.
        if (ivd && (ivd->op==IR_ADD || ivd->op==IR_SUB) && ivd->n_operands>=2 && vra_zexact(V, ivd)) {
            IrValue *a0=ivd->operands[0], *a1=ivd->operands[1];
            bool c1 = a1 && a1->id>=0 && a1->id<V->nvar && V->cknown[a1->id];
            IrInstr *ld = (a0 && a0->id>=0 && a0->id<V->nvar) ? V->def[a0->id] : NULL;
            if (c1 && ld && ld->op==IR_LOAD && ld->n_operands>=1) ivd = ld;   // `i ± const`
        }
        if (!ivd || ivd->op!=IR_LOAD || ivd->n_operands<1) continue;
        int cell=ivd->operands[0]->id;
        // ── A FIELD COUNTER ─────────────────────────────────────────────────────────────
        // `while l.pos < l.src.len { l.pos = l.pos + 1 }` is the lexer's main loop, and it was
        // refused: the counter had to be a scalar ALLOCA, and even as a field the guard's
        // `l.pos` and the body's are different field_ptrs, so no store matched. They name ONE
        // canonical cell (cellcanon), and that is what is compared below. Three things change
        // with it, each load-bearing:
        //   · a store to the BASE itself (`l = L(...)`) writes the field with no per-field
        //     store to find — it is a non-progress write, and refuses the loop;
        //   · the opaque-writer test (a call, an unattributable store) is asked of the BASE,
        //     which is what escapes — the field_ptr never does;
        //   · a `var` struct parameter's base is always escaped (the caller owns it), so any
        //     call in the loop refuses — conservative, and exactly the scalar rule's reading.
        int fbase = -1;
        if (!vra_is_scalar_cell(V,cell)) {
            fbase = vra_field_cell_base(V, cell);
            if (fbase < 0) continue;
        }
        int ccell = vra_canon_cell(V, cell);
        #define VRA_SAME_CELL(x) (fbase < 0 ? (x) == cell : vra_canon_cell(V, (x)) == ccell)
        if (!vra_loop_invariant(V,bnd,H)) continue;
        bool lt=(p==IR_CMP_SLT||p==IR_CMP_ULT||p==IR_CMP_SLE||p==IR_CMP_ULE);
        bool gt=(p==IR_CMP_SGT||p==IR_CMP_UGT||p==IR_CMP_SGE||p==IR_CMP_UGE);
        if (!lt && !gt) continue;
        // The loop's REAL body, not "every block with an id at or above the header's" — the
        // same id-order heuristic that broke the widening selector, and here it counted stores
        // from unrelated later code as if they were loop updates.
        int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
        char *body = malloc((size_t)nbb);
        if (!body) continue;                                  // fail closed
        vra_natural_loop(V, H, nbb, body);
        // ★ PROGRESS PER BLOCK, not "exactly one update in the whole loop". Requiring a single
        // update site refused `if flag > 0 { i = i + 1 } else { i = i + 2 }` — both arms move
        // the counter the right way, so the loop plainly terminates, and it is an ordinary
        // shape. What actually has to hold is (a) EVERY store to the cell is progress, so no
        // path can undo one, and (b) every path around the loop passes through at least one.
        // (b) is a dataflow question, computed below; counting cannot answer it, and the two
        // ways of being wrong are opposite: `if c { i = i + 1 }` has one update site and does
        // NOT terminate, while the two-armed `if` has two and does.
        char *prog = calloc((size_t)nbb, 1);
        if (!prog) { free(body); continue; }
        bool bad = false, anyprog = false, has_call = false;
        for (IrBlock *b=V->f->blocks; b && !bad; b=b->next) {
            if (!(b->id>=0 && b->id<nbb && body[b->id])) continue;
            for (IrInstr *st=b->instrs; st; st=st->next) {
                // ★ A CALL CAN MOVE THE COUNTER WITH NO STORE TO SEE. This scan reads
                // IR_STOREs, so `while i < 8 { i = i + 1; reset(var i) }` looked like a clean
                // +1 per iteration and was PROVEN TERMINATING — `reset` can put i back to 0
                // forever. Sound only when the cell's address never left: `V->escaped` is the
                // same set the memory model havocs at every call, and if nothing in the loop
                // calls anything then nothing can exercise the escape while the loop runs.
                if (st->op==IR_CALL) has_call = true;
                if (st->op==IR_STORE && st->n_operands>=2 && fbase >= 0 &&
                    st->operands[0]->id == fbase) { bad = true; break; }   // whole-struct write
                if (st->op!=IR_STORE || st->n_operands<2 || !VRA_SAME_CELL(st->operands[0]->id)) continue;
                bool ok_step = false;
                IrInstr *vd=V->def[st->operands[1]->id];
                if (vd && (vd->op==IR_ADD||vd->op==IR_SUB) && vd->n_operands>=2 && vra_zexact(V, vd)) {
                    IrInstr *ld=V->def[vd->operands[0]->id]; int c=vd->operands[1]->id;
                    if (ld && ld->op==IR_LOAD && VRA_SAME_CELL(ld->operands[0]->id)) {
                        if (V->cknown[c]) {
                            int64_t stp = (vd->op==IR_ADD)? V->cval[c] : (V->cval[c] == INT64_MIN ? 0 : -V->cval[c]);
                            ok_step = (lt && stp>0) || (gt && stp<0);
                        } else if (c>=0 && c<V->nvar && V->in[b->id]) {
                            // ── A VARIABLE STEP, WHICH ONLY NEEDS ITS SIGN ──────────────────
                            // `while i < n decreasing n - i { i = i + k }` is an ordinary
                            // strided scan and the step need not be a literal: what termination
                            // requires is that every iteration MOVES THE COUNTER THE RIGHT WAY,
                            // i.e. that `k` is bounded away from zero on the correct side. The
                            // rule asked `V->cknown[c]` — is it a compile-time constant — which
                            // is a much stronger question than the one that matters, and it
                            // refused the whole family.
                            //
                            // The bound is read from the CONVERGED loop state, so it holds on
                            // every iteration rather than the first; a step that is sometimes
                            // zero (the `decreasing_zero_step_fail` shape, `m` in [0, ...])
                            // yields lo == 0 and is still refused, which is the soundness
                            // condition and is pinned by that test.
                            int64_t slo,shi; bool hl,hh;
                            int64_t *sc = malloc((size_t)V->dsz*8);
                            if (sc) {
                                memcpy(sc, V->in[b->id], (size_t)V->dsz*8);
                                Octagon SW = { V->noct, 2*V->noct, sc };
                                oct_close(&SW);
                                for (IrInstr *q=b->instrs; q && q!=st; q=q->next)
                                    vra_transfer_instr(V,&SW,q);
                                oct_close(&SW);
                                vra_interval(V,&SW,c,&slo,&hl,&shi,&hh);
                                if (vd->op==IR_ADD) ok_step = (lt && hl && slo>=1) || (gt && hh && shi<=-1);
                                else                ok_step = (lt && hh && shi<=-1) || (gt && hl && slo>=1);
                                free(sc);
                            }
                        }
                    }
                } else if (gt && vra_step_decreases(V, cell, vd)) {
                    ok_step = true;      // a falling step the domain justifies (see above)
                }
                // A store of a CONSTANT that makes the guard false is progress too: that path
                // leaves the loop at the next header test. `while j > 0 { ... else { j = 0 } }`
                // is the shape — the else arm does not decrease j by a step, it ends the loop.
                if (!ok_step) {
                    int sv = st->operands[1]->id, bv = bnd->id;
                    if (sv>=0 && sv<V->nvar && V->cknown[sv] &&
                        bv>=0 && bv<V->nvar && V->cknown[bv]) {
                        int64_t cv=V->cval[sv], bc=V->cval[bv];
                        uint64_t cu=(uint64_t)cv, bu=(uint64_t)bc;
                        bool holds = true;
                        switch (p) {
                            case IR_CMP_SLT: holds = cv <  bc; break;
                            case IR_CMP_SLE: holds = cv <= bc; break;
                            case IR_CMP_SGT: holds = cv >  bc; break;
                            case IR_CMP_SGE: holds = cv >= bc; break;
                            case IR_CMP_ULT: holds = cu <  bu; break;
                            case IR_CMP_ULE: holds = cu <= bu; break;
                            case IR_CMP_UGT: holds = cu >  bu; break;
                            case IR_CMP_UGE: holds = cu >= bu; break;
                            default: holds = true; break;
                        }
                        ok_step = !holds;               // guard now false -> the loop exits
                    }
                }
                // ── A STORE THE DOMAIN ORDERS AGAINST THE CELL ───────────────────────────────
                // `i = scan_to(src, n, i, 42)` (a callee whose return refinement says `>= start`)
                // and `i = r + 1` after it are the scanner's shape, and both were refused: the
                // rules above recognise `cell ± step` by its SPELLING. The converged state at the
                // store knows more. Replayed to just before it, the octagon bounds `cell − v`:
                //   ≤ −1  the store moves the counter forward by at least one — PROGRESS;
                //   ≤ 0   it never moves it back — allowed, but not progress by itself.
                // Every path must still pass a progress store, so each iteration moves the
                // counter by ≥ 1 and none moves it back: the same argument as `cell + k`, with
                // the order read from the domain instead of the syntax. (For a falling counter,
                // the mirror image.) A wrapping `+%` that may wrap has no such relation (the
                // transfer drops it under modwrap), so it cannot pass here.
                bool mono_only = false;
                if (!ok_step && b->id >= 0 && V->in[b->id]) {
                    int sv = st->operands[1]->id;
                    int dcell = (fbase < 0) ? cell : ccell;
                    int64_t *sc2 = malloc((size_t)V->dsz*8);
                    if (sc2 && sv >= 0 && sv < V->nvar) {
                        memcpy(sc2, V->in[b->id], (size_t)V->dsz*8);
                        Octagon SW2 = { V->noct, 2*V->noct, sc2 };
                        oct_close(&SW2);
                        for (IrInstr *q=b->instrs; q && q!=st; q=q->next) vra_transfer_instr(V,&SW2,q);
                        oct_close(&SW2);
                        if (!oct_is_bottom(&SW2)) {
                            // cell − v ≤ d  and  v − cell ≤ e
                            int64_t d = oct_get(&SW2, oct_pos(sv), oct_pos(dcell));
                            int64_t e = oct_get(&SW2, oct_pos(dcell), oct_pos(sv));
                            if (lt) { if (d <= -1) ok_step = true; else if (d <= 0) mono_only = true; }
                            else    { if (e <= -1) ok_step = true; else if (e <= 0) mono_only = true; }
                        }
                    }
                    free(sc2);
                }
                if (mono_only) continue;                // neither progress nor a step back
                if (!ok_step) { bad = true; break; }   // a store that is not progress
                prog[b->id] = 1; anyprog = true;
            }
        }
        (void)has_call;
        #undef VRA_SAME_CELL
        if (bad || !anyprog) { free(body); free(prog); continue; }
        if (vra_cell_opaque_write(V, fbase >= 0 ? fbase : cell, nbb, body)) {
            free(body); free(prog); continue;          // the callee may write the counter
        }
        bool ok = vra_progress_on_every_path(V, H, nbb, body, prog);
        free(body); free(prog);
        if (ok) {
            vra_last_measure = (VraMeasure){ lt ? VRA_MEAS_RISES : VRA_MEAS_FALLS, ivv, bnd };
            result_loop = true; break;
        }
    }
    oct_map = oct_map_saved_loop;
    // Neither endpoint is a counter against an invariant bound — try the DIFFERENCE.
    return result_loop ? true : vra_loop_terminates_pair(V, H);
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

    // ★ THE GUARD NEED NOT BE SPELLED `i < n`. A sliding window is guarded `i + 1 < n`, and
    // that is not a stylistic choice: `i < n - 1` UNDERFLOWS at n == 0 on an unsigned type and
    // runs the loop on an empty slice (corpus C-7), so the offset form is the only SAFE way to
    // write the idiom. Reading only a bare LOAD here meant the safe spelling produced no trip
    // count at all, and every accumulator under it was refused with "the loop has no bounded
    // trip count" — the diagnostic asking the author to bound something that was bounded.
    //
    // `i + k < n` for a constant k >= 0 is `i < n - k` over the integers, so the same recovery
    // works against a limit lowered by k. k is subtracted from the BOUND rather than added to
    // the start: the start may be unknown, the bound is what has to be finite anyway.
    int64_t off = 0;
    if (ivd && ivd->op==IR_ADD && ivd->n_operands>=2 && vra_zexact(V, ivd)) {
        IrInstr *ld = V->def[ivd->operands[0]->id];
        int k = ivd->operands[1]->id;
        if (ld && ld->op==IR_LOAD && ld->n_operands>=1 &&
            k>=0 && k<V->nvar && V->cknown[k] && V->cval[k] >= 0) {
            off = V->cval[k];
            ivd = ld;
        }
    }
    if (!ivd || ivd->op!=IR_LOAD || ivd->n_operands<1) return false;
    int cell = ivd->operands[0]->id;
    if (!vra_is_scalar_cell(V,cell)) return false;
    if (!vra_loop_invariant(V,bnd,H)) return false;

    int64_t blo,bhi; vra_range(V,W,bnd,&blo,&bhi); (void)blo;
    if (bhi >= INT64_MAX/2) return false;             // an unbounded limit bounds nothing
    bhi -= off;                                       // `i + k < n`  ==>  `i < n - k`
    int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
    char *body = malloc((size_t)nbb); if (!body) return false;
    vra_natural_loop(V, H, nbb, body);
    // ★ THE START IS THE COUNTER'S VALUE ON ENTRY, not at the question. This read the cell's
    // interval in W — the state where the bound is ASKED — and T = limit − start then counts the
    // trips still to come, not the trips taken. After `while i < 200 { s = s + 1; i = i + 1 }`
    // i is 200, so T was 0 and s — 300 at run time — was bounded at its entry value 100:
    // `var t u8 = s` compiled and truncated, and `a[s]` on 101 elements read out of bounds
    // under a proof. Inside the loop any guard that raised i's lower bound shrank T the same
    // way. And an unknown start was taken as 0. The start is now joined over the stores OUTSIDE
    // the loop, and each stored VALUE is read in W: it is an SSA value and never changes, so what
    // W knows of it held when it was stored — unlike the counter CELL, which W sees after the
    // very trips it is asked to count. (Read from its TYPE instead, `var d i32 = 0 - 2` started
    // at −2^31 and every fuzz_div program lost its bound.)
    int64_t ilo = INT64_MAX; bool any_start = false;
    for (IrBlock *b2=V->f->blocks; b2; b2=b2->next) {
        if (b2->id>=0 && b2->id<nbb && body[b2->id]) continue;
        for (IrInstr *st=b2->instrs; st; st=st->next) {
            if (st->op!=IR_STORE || st->n_operands<2 || st->operands[0]->id!=cell) continue;
            IrValue *sv = st->operands[1];
            int64_t vlo, vhi;
            if (!sv || !sv->type || sv->type->kind != IRT_INT) { free(body); return false; }
            vra_range(V, W, sv, &vlo, &vhi);
            if (vlo < ilo) ilo = vlo;
            any_start = true;
        }
    }
    if (!any_start || ilo <= INT64_MIN/4) { free(body); return false; }
    int64_t step = 0; int nupd = 0;
    for (IrBlock *b=V->f->blocks; b; b=b->next) {
        if (!(b->id>=0 && b->id<nbb && body[b->id])) continue;
        for (IrInstr *st=b->instrs; st; st=st->next) {
            if (st->op!=IR_STORE || st->n_operands<2 || st->operands[0]->id!=cell) continue;
            IrInstr *vd=V->def[st->operands[1]->id];
            if (vd && vd->op==IR_ADD && vd->n_operands>=2 && vra_zexact(V, vd)) {
                IrInstr *ld=V->def[vd->operands[0]->id]; int k=vd->operands[1]->id;
                if (ld && ld->op==IR_LOAD && ld->operands[0]->id==cell &&
                    k>=0 && k<V->nvar && V->cknown[k] && V->cval[k] > 0) { step=V->cval[k]; nupd++; }
                else nupd += 2;                        // an update we cannot read: give up
            } else nupd += 2;
        }
    }
    bool opaque = vra_cell_opaque_write(V, cell, nbb, body);
    free(body);
    if (opaque) return false;          // a call may reset the counter: T is not a trip count
    if (nupd != 1 || step <= 0) return false;
    if (bhi < ilo) { *T = 0; return true; }
    if (bhi - ilo > INT64_MAX/2) return false;         // no trip count worth the name
    *T = (bhi - ilo + step - 1) / step;                // ceil((limit − start) / step)
    return *T >= 0;
}

// ── A LOOP GUARD'S OWN ARITHMETIC ────────────────────────────────────────────────────────
// `while i + 1 < n` is the only SAFE spelling of a sliding window — `i < n - 1` underflows at
// n == 0 on an unsigned type and runs the loop on an empty slice (corpus C-7) — and its `i + 1`
// was reported as a possible overflow. The reason is structural, not a missing fact:
//
//   A guard's arithmetic is evaluated at the loop HEADER, before the guard refines anything,
//   so it cannot benefit from the guard it is part of. With `i < n` the increment lives in the
//   BODY, where `i <= n - 1` already holds; with `i + 1 < n` the arithmetic IS the condition
//   and meets only the widened `i` in [0, +inf].
//
// The invariant that settles it is inductive and the octagon cannot state it (it needs a case
// split on the first iteration), so it is proved here, syntactically, on one narrow shape:
//
//   the cell's ONLY in-loop update is `i := i + 1`   (step exactly one)
//   the guard is `i + k < n`, STRICT, k a non-negative constant
//   `n` is loop-invariant and a value of the same type
//   the cell's only pre-loop store is a constant c with c + k in range
//
// Then at every header entry after the first, the previous visit had `i_prev + k < n`, hence
// i_prev <= n - k - 1, hence i = i_prev + 1 <= n - k, hence `i + k <= n <= typemax`. On the
// first entry `i = c` and `c + k` was checked directly. Step one is load-bearing: with step
// s the bound becomes n + s - 1, which can leave the type.
static bool vra_guard_counter_fits(Vra *V, Octagon *W, IrInstr *ins, int64_t thi) {
    if (ins->op != IR_ADD || ins->n_operands < 2 || !ins->result) return false;
    IrInstr *ld = V->def[ins->operands[0]->id];
    int kv = ins->operands[1]->id;
    if (!ld || ld->op != IR_LOAD || ld->n_operands < 1) return false;
    if (kv < 0 || kv >= V->nvar || !V->cknown[kv] || V->cval[kv] < 0) return false;
    int64_t k = V->cval[kv];
    int cell = ld->operands[0]->id;

    for (IrBlock *H = V->f->blocks; H; H = H->next) {
        if (!H->is_loop_header || H->term.kind != IR_TERM_BR_COND || !H->term.cond) continue;
        IrInstr *ic = V->def[H->term.cond->id];
        if (!ic || ic->op != IR_ICMP || ic->n_operands < 2) continue;
        if (ic->operands[0]->id != ins->result->id) continue;          // this ADD IS the guard
        if (!(ic->aux.cmp == IR_CMP_SLT || ic->aux.cmp == IR_CMP_ULT)) continue;  // strict only
        IrValue *bnd = ic->operands[1];
        if (!vra_loop_invariant(V, bnd, H)) continue;
        int64_t nlo, nhi;                                              // n must fit the result type
        if (!bnd->type || bnd->type->kind != IRT_INT) continue;
        if (!irtype_int_range(bnd->type, &nlo, &nhi) || nhi > thi) continue;

        int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
        char *body = malloc((size_t)nbb); if (!body) return false;
        vra_natural_loop(V, H, nbb, body);

        int64_t step = 0; int nupd = 0, nentry = 0; int64_t c0 = 0; bool c0_known = true;
        for (IrBlock *b = V->f->blocks; b; b = b->next) {
            bool in = (b->id >= 0 && b->id < nbb && body[b->id]);
            for (IrInstr *st = b->instrs; st; st = st->next) {
                if (st->op != IR_STORE || st->n_operands < 2 || st->operands[0]->id != cell) continue;
                if (!in) {                                             // the pre-loop initialiser
                    int sv = st->operands[1]->id;
                    nentry++;
                    if (sv < 0 || sv >= V->nvar || !V->cknown[sv]) c0_known = false;
                    else c0 = V->cval[sv];
                    continue;
                }
                IrInstr *vd = V->def[st->operands[1]->id];
                if (vd && vd->op == IR_ADD && vd->n_operands >= 2 && vra_zexact(V, vd)) {
                    IrInstr *l2 = V->def[vd->operands[0]->id]; int a2 = vd->operands[1]->id;
                    if (l2 && l2->op == IR_LOAD && l2->operands[0]->id == cell &&
                        a2 >= 0 && a2 < V->nvar && V->cknown[a2]) { step = V->cval[a2]; nupd++; }
                    else nupd += 2;
                } else nupd += 2;
            }
        }
        bool opaque = vra_cell_opaque_write(V, cell, nbb, body);
        free(body);
        if (opaque || nupd != 1 || step != 1) continue;                 // step ONE, and nothing else
        if (nentry != 1 || !c0_known) continue;
        if (c0 > thi - k) continue;                                     // the first entry
        return true;
    }
    return false;
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
    // `body` still holds the found loop's block set. The ACCUMULATOR's cell needs the same
    // question asked of it as the counter's: s0 + T*delta says nothing if a callee can assign
    // to s behind the loop's back.
    bool opaque = found && vra_cell_opaque_write(V, cell, nbb, body);
    if (!found || opaque) { free(body); return false; }
    *H = found;

    // The addend's range. vra_range follows widening casts to the source's own type, so
    // `(a[i] as i32)` on a u8 element reads as [0,255] rather than [-2^31,255] — without
    // which no sum is bounded by anything.
    vra_range(V, W, add->operands[1], dlo, dhi);
    if (add->op==IR_SUB) { int64_t t=*dlo; *dlo = -*dhi; *dhi = -t; }
    // ★ s0 IS THE VALUE ON ENTRY TO THE LOOP, and it must be read from the stores that happen
    // OUTSIDE it. This used to read the cell's octagon interval at the check point — inside
    // the loop, where the accumulator has been WIDENED — and fall back to 0 when that gave
    // nothing. Falling back to 0 is assuming the accumulator starts at zero. For
    // `var s i32 = b` with `b` refined [1,3] that is false, and `0 + T*delta` was small enough
    // to discharge an obligation the real `s0 + T*delta` fails:
    //     proc f(a i32 >= 1 and <= 1073741823, b i32 >= 1 and <= 3) i32 {
    //         var s i32 = b ; while i < 2 { s = s + a ; i = i + 1 } ; return s }
    // was PROVEN check-free and overflows (UBSan: 1073741826 + 1073741823). PRE-EXISTING —
    // reproduced at HEAD — and the second false proof in B1. A missing bound is "I do not
    // know", never "zero".
    //
    // The join is over every store to the cell from a block NOT in the loop. A store AFTER the
    // loop joins in too, which can only WIDEN s0 and therefore prove less; that is the safe
    // direction. Each stored value is read from the CONSTANT table or its declared type, never
    // from the octagon — the octagon state here is the one inside the loop, where a guard may
    // have narrowed a value below its entry range.
    int64_t s_lo = INT64_MAX, s_hi = INT64_MIN; bool any = false;
    for (IrBlock *b2=V->f->blocks; b2; b2=b2->next) {
        if (b2->id>=0 && b2->id<nbb && body[b2->id]) continue;      // inside the loop: not s0
        for (IrInstr *st=b2->instrs; st; st=st->next) {
            if (st->op!=IR_STORE || st->n_operands<2 || st->operands[0]->id!=cell) continue;
            IrValue *sv = st->operands[1];
            int64_t vlo, vhi;
            if (sv && sv->id>=0 && sv->id<V->nvar && V->cknown[sv->id]) {
                vlo = vhi = V->cval[sv->id];
            } else if (!sv || !sv->type || !irtype_int_range(sv->type, &vlo, &vhi)) {
                free(body); return false;
            }
            if (vlo < s_lo) s_lo = vlo;
            if (vhi > s_hi) s_hi = vhi;
            any = true;
        }
    }
    free(body);
    if (!any || s_lo > s_hi) return false;
    *s0lo = s_lo; *s0hi = s_hi;
    return true;
}

// ★ A MODULAR ACCUMULATOR (`count = count +% d`) is bounded by the same argument ONLY if the
// bound fits the step's own type: then no partial sum s0 + k·d left the type, so no iteration
// wrapped and each one computed the ℤ sum — induction on k. A bound that leaves the type says
// nothing, because the value that left it came back in somewhere else.
static bool vra_accum_fits_step(Vra *V, IrValue *val, int64_t lo, int64_t hi) {
    IrInstr *d = (val && val->id >= 0 && val->id < V->nvar) ? V->def[val->id] : NULL;
    if (!d || d->wrap != IR_WRAP_MODULAR) return true;
    int64_t tlo, thi;
    if (!d->result || !irtype_int_range(d->result->type, &tlo, &thi)) return false;
    return lo >= tlo && hi <= thi;
}

// ★ IS THIS A RUNNING TOTAL? The diagnostic below explains an accumulator ("it starts in ... each
// iteration adds ... bound the count, the element, or the total") and it was printed for ANY
// loop-carried `x + d` whose check failed — a binary search's midpoint, a window's `i + w`, a
// stride-2 guard — sending the user to fix the wrong thing (O-5, local/ambrus/ISSUES.md). A running
// total is stored BACK into the cell it read (`s = s + d`), and it is not the loop's own counter
// (a cell the header's condition reads is bounded by the guard, not by a trip count).
static bool vra_is_running_total(Vra *V, IrValue *val, IrBlock *H) {
    IrInstr *add = (val && val->id >= 0 && val->id < V->nvar) ? V->def[val->id] : NULL;
    if (!add || add->n_operands < 1) return false;
    IrInstr *ld = V->def[add->operands[0]->id];
    if (!ld || ld->op != IR_LOAD || ld->n_operands < 1) return false;
    int cell = vra_canon_cell(V, ld->operands[0]->id);
    int nbb = V->f->next_block_id > 0 ? V->f->next_block_id : 1;
    char *body = malloc((size_t)nbb); if (!body) return false;
    vra_natural_loop(V, H, nbb, body);
    bool back = false;
    for (IrBlock *b = V->f->blocks; b && !back; b = b->next) {
        if (!(b->id >= 0 && b->id < nbb && body[b->id])) continue;
        for (IrInstr *st = b->instrs; st; st = st->next)
            if (st->op == IR_STORE && st->n_operands >= 2 && st->operands[1] == val &&
                vra_canon_cell(V, st->operands[0]->id) == cell) { back = true; break; }
    }
    free(body);
    if (!back) return false;
    IrInstr *ic = (H->term.kind == IR_TERM_BR_COND && H->term.cond) ? V->def[H->term.cond->id] : NULL;
    for (int q = 0; ic && ic->op == IR_ICMP && q < ic->n_operands && q < 2; q++) {
        IrInstr *d = V->def[ic->operands[q]->id];
        for (int hop = 0; d && hop < 4; hop++) {                  // peel `i + k`, casts
            if (d->op == IR_LOAD && d->n_operands >= 1) {
                if (vra_canon_cell(V, d->operands[0]->id) == cell) return false;   // the counter
                break;
            }
            if ((d->op == IR_CAST || d->op == IR_ADD || d->op == IR_SUB) && d->n_operands >= 1)
                d = V->def[d->operands[0]->id];
            else break;
        }
    }
    return true;
}

// Same computation, but it reports what it found even when the bound does not hold — that is
// what the diagnostic needs.
static bool vra_accum_info(Vra *V, Octagon *W, IrValue *val, VraCheck *c,
                           int64_t tlo, int64_t thi) {
    IrBlock *H = NULL; int64_t s0lo, s0hi, dlo, dhi, T;
    if (!vra_accum_delta(V, W, val, &H, &s0lo, &s0hi, &dlo, &dhi)) return false;
    bool haveT = vra_loop_trips(V, W, H, &T);
    if (c && vra_is_running_total(V, val, H)) {
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
    if (!vra_accum_fits_step(V, val, lo, hi)) return false;
    return lo >= tlo && hi <= thi;
}

// ★ B1'S BOUND IS NOT ONLY FOR THE OBLIGATION IT WAS INVENTED FOR.
// The clamps above (alo <= 0, ahi >= 0) make [s0lo+alo, s0hi+ahi] contain s0 itself and every
// partial sum s0 + k*delta for 0 <= k <= T — so the interval holds at EVERY point in and after
// the loop, not just at the end. That makes it answerable as an ordinary range query.
//
// Without this the bound reached the accumulator's own narrowing and nothing else:
// `for i in 0..3 { s = s + a[i] }` proved, and the very next line `return s + a.len` did not,
// because the octagon still had only the widened value for s. B1 was deliberately built as a
// check-time refinement rather than a state injection ("it can only turn an obligation from
// unproven to proven, so it cannot make the fixpoint less sound"), and that caution was right;
// this keeps the property — nothing is written into the octagon — while letting any query see
// the answer.
//
// `V->accum_cell` is the structural pre-filter: vra_range is hot, and without it every range
// query would walk the function looking for a self-referencing store.
static bool vra_accum_busy = false;
static bool vra_cell_accum_range(Vra *V, Octagon *W, int cell, int64_t *lo, int64_t *hi) {
    if (cell < 0 || cell >= V->nvar || !V->accum_cell || !V->accum_cell[cell]) return false;
    if (vra_accum_busy) return false;                 // vra_accum_delta asks vra_range back
    // ★ EXACTLY ONE accumulating store, and EXACTLY ONE store overall.
    // `s0 + T*delta` describes one update per iteration. With two — `x = x + 1; x = x + 2` —
    // taking the first gives +1/iter where the truth is +3, and the bound is an UNDERCOUNT:
    // `arr[x]` with x == 30 on a 21-element array came out PROVEN. That is the exact defect
    // `affine_recap_multistep_fail.ln` exists to pin, and this query walked straight back into
    // it by picking the first store it found. A second store of ANY kind is equally fatal —
    // it can reset or jump the accumulator between iterations — so both are counted.
    IrValue *acc = NULL; int nstore = 0, nacc = 0;
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op != IR_STORE || ins->n_operands < 2) continue;
            if (ins->operands[0]->id != cell) continue;
            nstore++;
            IrInstr *vd = V->def[ins->operands[1]->id];
            if (!vd || (vd->op != IR_ADD && vd->op != IR_SUB)) continue;
            IrInstr *ld = V->def[vd->operands[0]->id];
            if (!ld || ld->op != IR_LOAD || ld->n_operands < 1 ||
                ld->operands[0]->id != cell) continue;
            acc = ins->operands[1]; nacc++;
        }
    // one accumulating store, plus at most the initialiser before the loop
    if (!acc || nacc != 1 || nstore > 2) return false;
    vra_accum_busy = true;
    IrBlock *H = NULL; int64_t s0lo, s0hi, dlo, dhi, T; bool ok = false;
    if (vra_accum_delta(V, W, acc, &H, &s0lo, &s0hi, &dlo, &dhi) &&
        vra_loop_trips(V, W, H, &T)) {
        int64_t alo, ahi;
        if (!vra_mul_ovf(T, dlo, &alo) && !vra_mul_ovf(T, dhi, &ahi)) {
            if (alo > 0) alo = 0;
            if (ahi < 0) ahi = 0;
            int64_t l, h;
            if (!__builtin_add_overflow(s0lo, alo, &l) &&
                !__builtin_add_overflow(s0hi, ahi, &h) && l <= h &&
                vra_accum_fits_step(V, acc, l, h)) {
                *lo = l; *hi = h; ok = true;
            }
        }
    }
    vra_accum_busy = false;
    return ok;
}


// ── the fixpoint over the CFG ────────────────────────────────────────────────
// Does `f` call itself, and where? Returns the FIRST self-call instruction, so a diagnostic
// can point at a line the programmer wrote rather than at the function's opening brace.
// Direct calls only: an indirect one names no function, which the effect row already charges
// as the worst case.
static IrInstr *vra_self_call_site(IrFunc *f) {
    if (!f || !f->name) return NULL;
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_CALL && i->aux.callee
                && i->aux.callee->length==f->name->length
                && memcmp(i->aux.callee->name, f->name->name, (size_t)f->name->length)==0)
                return i;
    return NULL;
}
static bool vra_recursion_terminates(Vra *V, IrFunc *f);   // fwd — defined after the domain helpers
static bool vra_mutual_cycle_edges(IrFunc *f, IrFunc *mod, IrInstr **site,
                                   IrFunc **via, IrInstr **back);            // fwd
static bool vra_mutual_cycle_terminates(IrFunc *f, IrFunc *g, IrInstr *cfg, IrInstr *cgf);  // fwd

// The function's entry state: each integer parameter's type interval, intersected with any
// call-site binding. Factored out because the fixpoint now runs TWICE (see vpass) and pass 1
// must start from the same entry state pass 0 did — re-running from a stale `in[entry]` would
// start pass 1 inside pass 0's conclusions.
static void vra_seed_entry(Vra *V, IrFunc *f, int dim) {
    Octagon E={V->noct,dim,V->in[f->entry->id]}; oct_init_top(&E,V->noct,E.m);
    V->inclosed[f->entry->id] = false;          // seeded constraint by constraint, never closed
      // seed each integer parameter's type interval (a usize is ≥ 0, etc.). Skip a
      // bound whose doubled DBM entry would overflow (e.g. u64's ~2^63 upper).
      int pidx = 0;
      for (IrParam *p=f->params; p; p=p->next, pidx++) {   // only the type interval; refinements
          int64_t tlo,thi;                          // now arrive as entry IR_ASSUME nodes
          if (!irtype_int_range(p->value->type,&tlo,&thi)) continue;
          // A call-site binding INTERSECTS with the declared interval — never replaces it, so
          // a wrong binding can only ever be narrower than something already true.
          if (pidx < vra_argbind_n && pidx < VRA_ARGBIND_MAX && vra_argbind_has[pidx]) {
              if (vra_argbind_lo[pidx] > tlo) tlo = vra_argbind_lo[pidx];
              if (vra_argbind_hi[pidx] < thi) thi = vra_argbind_hi[pidx];
              if (tlo > thi) { tlo = thi; }         // empty: the call is unreachable; stay sound
          }
          if (tlo > -OCT_INF/2) oct_add_lb(&E, p->value->id, tlo);
          if (thi <  OCT_INF/2) oct_add_ub(&E, p->value->id, thi);
      }
    }

// ★ ONE ANALYSIS, READ BY EVERY PASS THAT CAN. A program's functions were each analysed about
// four times — the report, the borrow pass's disjointness queries, the effects pass's
// termination question at emission, and callee summaries: 23 runs for 6 functions in the
// calculator, 88% of the time in oct_close. The report now shares its analysis with the borrow
// pass (vra_shared_*), and a top-level analysis leaves the loop-termination answer on the
// function for the effects pass (vra_loops_summary). Nested analyses (a callee summary, a
// mutual-recursion check) leave nothing: they run under guards a top-level one does not.
static int     vra_depth = 0;
static IrFunc *vra_shared_f = NULL;     // the function whose analysis the borrow pass may reuse
static Vra    *vra_shared_V = NULL;
static Vra *vra_analyze(IrFunc *f) {
    vra_depth++;
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
        if (pt->kind==IRT_INT || pt->kind==IRT_BOOL || pt->kind==IRT_SLICE || cell)
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
            else if (ins->op==IR_FIELD_PTR)
                // A scalar field's address is modelled as a numeric CELL (see
                // vra_field_cell_base), so it needs a dimension exactly as a scalar alloca
                // does. A superset of what the transfer will actually use — the narrow gate
                // needs V->def and V->escaped, which the prepass has not run yet.
                keep = rv->type && rv->type->kind==IRT_PTR && rv->type->elem &&
                       (rv->type->elem->kind==IRT_INT || rv->type->elem->kind==IRT_BOOL ||
                        rv->type->elem->kind==IRT_SLICE);
            else
                // ★ A SLICE's dimension is its LENGTH (see vra_prepass, "flow-sensitive").
                keep = rv->type && (rv->type->kind==IRT_INT || rv->type->kind==IRT_BOOL ||
                                    rv->type->kind==IRT_SLICE);
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
    V->in=calloc(nb,sizeof(int64_t*)); V->reached=calloc(nb,sizeof(bool)); V->inclosed=calloc(nb,sizeof(bool));
    V->def=calloc(V->nvar,sizeof(IrInstr*)); V->defblk=calloc(V->nvar,sizeof(int));
    V->val=calloc(V->nvar,sizeof(IrValue*));
    V->cval=calloc(V->nvar,sizeof(int64_t)); V->cknown=calloc(V->nvar,sizeof(bool));
    V->modwrap=calloc(V->nvar,sizeof(bool));
    V->slicelen=calloc(V->nvar,sizeof(int)); V->subslice_gep=calloc(V->nvar,sizeof(bool));
    V->escaped=calloc(V->nvar,sizeof(bool));
    V->persist=calloc(V->nvar,sizeof(bool));
    V->shape_rank=calloc(V->nvar,sizeof(int));
    V->shape_ext=calloc(V->nvar,sizeof(*V->shape_ext));
    V->elem_lo=calloc(V->nvar,sizeof(int64_t));
    V->elem_hi=calloc(V->nvar,sizeof(int64_t));
    V->elem_known=calloc(V->nvar,sizeof(bool));
    V->cret_lo=calloc(V->nvar,sizeof(int64_t)); V->cret_hi=calloc(V->nvar,sizeof(int64_t));
    V->cret_state=calloc(V->nvar,sizeof(signed char));
    V->accum_cell=calloc(V->nvar,sizeof(bool));
    V->strict_esc=calloc(V->nvar,sizeof(bool)); V->marking_benign=false;
    V->uniq_store=malloc(V->nvar*sizeof(int));
    if (V->uniq_store) {
        int *cnt = calloc(V->nvar, sizeof(int));
        for (int q=0;q<V->nvar;q++) V->uniq_store[q] = -1;
        for (IrBlock *b=f->blocks; b && cnt; b=b->next)
            for (IrInstr *i=b->instrs; i; i=i->next)
                if (i->op==IR_STORE && i->n_operands>=2 && i->operands[0] && i->operands[1]) {
                    int sl = i->operands[0]->id;
                    if (sl>=0 && sl<V->nvar && ++cnt[sl]==1) V->uniq_store[sl] = i->operands[1]->id;
                }
        for (int q=0;q<V->nvar && cnt;q++) if (cnt[q]!=1) V->uniq_store[q] = -1;
        if (!cnt) { free(V->uniq_store); V->uniq_store = NULL; }
        free(cnt);
    }
    vra_prepass(V);
    vra_seed_element_ranges(V);
    for (int i=0;i<nb;i++) V->in[i]=malloc(V->dsz*sizeof(int64_t));

    int64_t wb[1]; (void)wb;
    int64_t *W_m=malloc(V->dsz*8), *T_m=malloc(V->dsz*8), *J_m=malloc(V->dsz*8), *D_m=malloc(V->dsz*8);
    Octagon W={V->noct,dim,W_m}, T={V->noct,dim,T_m}, J={V->noct,dim,J_m}, D={V->noct,dim,D_m};

    vra_seed_entry(V, f, dim);
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
                    int a = ins->operands[0]->id;
                    int sl = V->odim[a];
                    if (sl >= 0) mod[sl]=1;
                    // ★ MARK THE SLOT THE TRANSFER WRITES, not the one the store names. A field
                    // store's address is a fresh FIELD_PTR each iteration, but the transfer writes
                    // the CANONICAL cell (the first FIELD_PTR with that base and index), and a
                    // store through a reference binding writes the cell the reference holds. Only
                    // the named slot was marked, so the cell really written was never widened:
                    // `while l.pos < l.src.len { l.pos = l.pos + 1 }` climbed one step per sweep
                    // and hit the fixpoint bound — an internal error on the simplest lexer loop.
                    int t = vra_canon_cell(V, vra_ref_target(V, a));
                    if (t>=0 && t<V->nvar && V->odim[t] >= 0) mod[V->odim[t]]=1;
                    // A WHOLE-STRUCT store writes every field cell of the base with no per-field
                    // store to find (`l = L(l.src, l.pos + 1)` assigns them from the constructor's
                    // operands), so each of them is modified too.
                    IrInstr *ad = (a>=0) ? V->def[a] : NULL;
                    if (ad && ad->op==IR_ALLOCA && ad->aux.alloca_ty &&
                        ad->aux.alloca_ty->kind==IRT_STRUCT)
                        for (IrBlock *fb=f->blocks; fb; fb=fb->next)
                            for (IrInstr *fp=fb->instrs; fp; fp=fp->next)
                                if (fp->op==IR_FIELD_PTR && fp->n_operands>=1 && fp->result &&
                                    fp->operands[0]->id==a) {
                                    int fc = vra_canon_cell(V, fp->result->id);
                                    if (fc>=0 && fc<V->nvar && V->odim[fc] >= 0) mod[V->odim[fc]]=1;
                                }
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

    // P3 again, but this lattice is NOT the bit-vector kind: it is the octagon, and what
    // guarantees termination is WIDENING, not finite height. So the bound cannot be derived as
    // (blocks x slots) the way linearity's and definite-init's can — widening converges because
    // each widen jumps to a threshold from a finite set, and the argument is Cousot & Cousot's,
    // not a counting one.
    //
    // What can be fixed is the silence. Measured over the corpus and std the high-water mark is
    // 114 sweeps; the bound stays generous, and reaching it now means WIDENING FAILED TO
    // CONVERGE, which is a compiler bug rather than a large program. Say so and stop, instead of
    // proceeding over a non-fixpoint — the numeric engine's results licence removed bounds
    // checks, so a non-fixpoint here is a miscompile waiting to happen.
    #define VRA_FIXPOINT_BOUND 4096
    bool changed=true; int sweeps=0;
    // ★ TWO PASSES, AND EXACTLY TWO (C14). Pass 0 runs with element ranges seeded from
    // constants and copies only. Then the seeding runs AGAIN against pass 0's converged state,
    // which can see what a loop fill or a comprehension actually stores — information that did
    // not exist when the first seeding ran, because it is produced by the fixpoint itself.
    // Pass 1 then re-runs from scratch with those ranges.
    //
    // ⚠ Two and no more, because the hazard here is circular justification: a range derived
    // from a state that was itself computed using that range proves nothing. Pass 0 never sees
    // the refinement, so the refined range is a true statement about the program, and using a
    // true statement in pass 1 is sound. Element ranges are never re-derived from pass 1.
    for (int vpass = 0; vpass < 2; vpass++) {
    if (vpass == 1) {
        bool improved = false;
        { // re-seed against the converged state
            int64_t *sv_lo = malloc((size_t)V->nvar*sizeof(int64_t));
            int64_t *sv_hi = malloc((size_t)V->nvar*sizeof(int64_t));
            bool *sv_k = malloc((size_t)V->nvar*sizeof(bool));
            if (sv_lo && sv_hi && sv_k) {
                memcpy(sv_lo, V->elem_lo, (size_t)V->nvar*sizeof(int64_t));
                memcpy(sv_hi, V->elem_hi, (size_t)V->nvar*sizeof(int64_t));
                memcpy(sv_k,  V->elem_known, (size_t)V->nvar*sizeof(bool));
                vra_seed_from_state = true; vra_seed_dim = dim;
                vra_seed_element_ranges(V);
                vra_seed_from_state = false;
                for (int i=0;i<V->nvar;i++)
                    if (V->elem_known[i] != sv_k[i] ||
                        V->elem_lo[i] != sv_lo[i] || V->elem_hi[i] != sv_hi[i]) { improved = true; break; }
            }
            free(sv_lo); free(sv_hi); free(sv_k);
        }
        if (!improved) break;                     // nothing new to learn: pass 0 stands
        for (int i=0;i<nb;i++) V->reached[i]=false;
        vra_seed_entry(V, f, dim);
        V->reached[f->entry->id]=true;
        changed=true; sweeps=0;
    }
    while (changed) {
        if (sweeps++ > VRA_FIXPOINT_BOUND) {
            fprintf(stderr, "internal error: the numeric fixpoint did not converge within %d "
                    "sweeps. Widening should make that impossible, so this is a compiler bug. "
                    "Refusing to report numeric results.\n", VRA_FIXPOINT_BOUND);
            exit(70);
        }
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!V->reached[b->id]) continue;
            // ★ A copy of a closed matrix is closed. Each block's in-state was re-closed here, and
            // each edge's copy of W below, although most of them were copies of a matrix closed a
            // moment before: 70% of the closures that followed a copy or a join were such
            // re-closures of a closed state.
            memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->noct; W.dim=dim; oct_set_clean(&W, V->inclosed[b->id]);
            oct_close(&W);
            for (IrInstr *ins=b->instrs; ins; ins=ins->next) vra_transfer_instr(V,&W,ins);
            oct_close(&W);
            IrBlock *succ[2]={NULL,NULL}; int ns=0; bool guarded=false; IrValue *cond=NULL;
            if (b->term.kind==IR_TERM_BR){ succ[0]=b->term.a; ns=1; }
            else if (b->term.kind==IR_TERM_BR_COND){ succ[0]=b->term.a; succ[1]=b->term.b; ns=2; guarded=true; cond=b->term.cond; }
            for (int k=0;k<ns;k++) {
                IrBlock *s=succ[k]; if(!s) continue;
                memcpy(T_m, W_m, V->dsz*8); T.nvar=V->noct; T.dim=dim; oct_set_clean(&T, W.clean);
                if (guarded){ vra_refine_guard(V,&T,cond,k==0); oct_close(&T); }
                if (oct_is_bottom(&T)) continue;
                if (!V->reached[s->id]) { memcpy(V->in[s->id],T_m,V->dsz*8); V->inclosed[s->id]=T.clean; V->reached[s->id]=true; changed=true; continue; }
                Octagon In={V->noct,dim,V->in[s->id]};
                oct_join(&J,&In,&T);
                // The pointwise max of two tightly closed octagons is tightly closed (Miné; the
                // unary entries stay even). A widened one is not.
                bool jclosed = V->inclosed[s->id] && T.clean;
                if (s->is_loop_header){ oct_widen_thr(&D,&In,&J,loopmod[s->id],thr,nthr); memcpy(J_m,D_m,V->dsz*8); oct_set_clean(&J, false); jclosed=false; }
                if (!oct_leq(&J,&In)){ memcpy(V->in[s->id],J_m,V->dsz*8); V->inclosed[s->id]=jclosed; changed=true; }
            }
        }
    }
    }   // vpass
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
        if (!V->reached[b->id]) {
            // An operation that never runs cannot wrap — the same reachability every other
            // obligation in this pass is discharged by.
            for (IrInstr *ins=b->instrs; ins; ins=ins->next)
                if (ins->wrap == IR_WRAP_MODULAR && ins->result && ins->result->id >= 0 &&
                    ins->result->id < V->nvar) V->modwrap[ins->result->id] = false;
            continue;
        }
        memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->noct; W.dim=dim; oct_set_clean(&W, V->inclosed[b->id]); oct_close(&W);
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
                case IR_ASSERT: oct_close(&W); vra_check_assert(V,&W,ins,b); break;
                case IR_ADD: case IR_SUB: case IR_MUL:
                    oct_close(&W); vra_check_overflow(V,&W,ins);
                    if (ins->wrap == IR_WRAP_MODULAR && ins->result && ins->result->id >= 0 &&
                        ins->result->id < V->nvar)
                        V->modwrap[ins->result->id] = vra_modular_may_wrap(V, &W, ins);
                    break;
                case IR_SHL: case IR_LSHR: case IR_ASHR: oct_close(&W); vra_check_shift(V,&W,ins); break;
                case IR_NEG: oct_close(&W); vra_check_neg(V,&W,ins); break;
                // Path-F's other half: the widened result meets a narrower slot HERE.
                case IR_STORE: {
                    if (ins->n_operands < 2) break;
                    IrType *pt = ins->operands[0]->type;
                    IrInstr *ad = V->def[ins->operands[0]->id];
                    IrType *slot = (pt && pt->kind==IRT_PTR) ? pt->elem
                                 : (ad && ad->op==IR_ALLOCA) ? ad->aux.alloca_ty : NULL;
                    if (slot && vra_type_may_lose(ins->operands[1]->type, slot)) {
                        oct_close(&W);
                        vra_check_narrow(V,&W, ins->operands[1], slot, ins, ins->line, ins->col);
                    }
                    break;
                }
                case IR_CAST:
                    // Any CHECK-mode cast that may lose a value owes the proof — the kind says
                    // only how the bits move, and a same-width signedness change moves none.
                    if (ins->n_operands >= 1 && ins->wrap == IR_WRAP_CHECK
                        && ins->result && ins->result->type) {
                        oct_close(&W);
                        vra_check_narrow(V,&W, ins->operands[0], ins->result->type,
                                         ins, ins->line, ins->col);
                    }
                    break;
                // ── THE OTHER NARROWING SITES: every place a value MEETS A DECLARED SLOT ───
                // Path-F puts the overflow obligation on the NARROWING, and the engine wired
                // three of the places one happens — a STORE into a cell, a TRUNC cast, and the
                // `ret` out of a function. It missed three more, and the omission was not
                // theoretical: `take(300)` where `take(x u8)`, `S(300)` where `S.x` is `u8`,
                // and `Shape.Circle(300)` where the payload is `u8` all compiled under the
                // sovereign engine alone. Seven corpus programs asserted those refusals and
                // the legacy engine was the only thing still making them, which is what D-47
                // recorded. Closed 2026-09-26: all five narrowing sites (assignment, return, call
                // argument, struct field initialiser, enum payload) were each re-tested with a
                // written violation and are refused here, so the legacy overflow half — and the
                // flag that stood it down — are deleted.
                //
                // They are one rule, not three, and they are written as one deliberately: a
                // LIST OF SITES is exactly the shape that hides a missing entry — the same
                // shape as `ir_mut_by_address` omitting IRT_SUM and the projection path
                // omitting a source form (D-45). Stating the question once ("what declared
                // slot does this value land in?") is what makes the next omission visible.
                case IR_CALL: {
                    // the callee's PARAMETER type, by position. An indirect call names no
                    // function and an extern's declaration is believed, as everywhere else.
                    IrFunc *cal = ins->aux.callee ? vra_find_func(ins->aux.callee) : NULL;
                    if (!cal) break;
                    IrParam *pp = cal->params; int k = 0;
                    for (; pp && k < ins->n_operands; pp = pp->next, k++) {
                        IrValue *arg = ins->operands[k];
                        IrType  *pt  = pp->value ? pp->value->type : NULL;
                        if (!arg || !vra_type_may_lose(arg->type, pt)) continue;
                        oct_close(&W);
                        vra_check_narrow(V,&W, arg, pt, ins, ins->line, ins->col);
                    }
                    break;
                }
                case IR_STRUCT_NEW: {
                    // NOTE: no pointer-peeling helper here on purpose — `ir_struct_of` lives
                    // in ir/lower.h, and analysis/ may include only ir/* and analysis/* (the
                    // sovereignty litmus the cmin gate enforces). A struct_new's result IS the
                    // struct type; anything else is not this rule's business.
                    IrType *st = ins->result ? ins->result->type : NULL;
                    if (!st || st->kind!=IRT_STRUCT) break;
                    for (int k=0; k<ins->n_operands && k<st->n_fields; k++) {
                        IrValue *fv = ins->operands[k];
                        IrType  *ft = st->fields[k];
                        if (!fv || !vra_type_may_lose(fv->type, ft)) continue;
                        oct_close(&W);
                        vra_check_narrow(V,&W, fv, ft, ins, ins->line, ins->col);
                    }
                    break;
                }
                case IR_SUM_NEW: {
                    IrType *su = ins->result ? ins->result->type : NULL;
                    if (!su || su->kind!=IRT_SUM) break;
                    int kv = ins->aux.sum.variant;
                    if (kv < 0 || kv >= su->n_fields) break;
                    IrType *pl = su->fields[kv];             // the variant's payload
                    if (!pl) break;
                    for (int k=0; k<ins->n_operands; k++) {
                        IrValue *pv2 = ins->operands[k];
                        // a multi-field payload is a struct; a single one may be the type
                        IrType *ft = (pl->kind==IRT_STRUCT && k < pl->n_fields) ? pl->fields[k]
                                   : (k==0 ? pl : NULL);
                        if (!pv2 || !vra_type_may_lose(pv2->type, ft)) continue;
                        oct_close(&W);
                        vra_check_narrow(V,&W, pv2, ft, ins, ins->line, ins->col);
                    }
                    break;
                }
                case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: oct_close(&W); vra_check_divzero(V,&W,ins,b); break;
                case IR_CTZ: case IR_CLZ: oct_close(&W); vra_check_bitcount(V,&W,ins,b); break;
                default: break;
            }
            vra_transfer_instr(V,&W,ins);
        }
        // ★ THE RETURN IS A NARROWING SITE, and it had no obligation at all.
        // Path-F widens, so `(a * b) * a` on i16 parameters has result type i48 — and
        // `ret %11` out of a function declared `-> i16` narrows 48 bits to 16 with nothing
        // checking it. The engine PROVED both multiplies (correctly: neither overflows its
        // own widened type) and then said the function was check-free.
        //
        // Found by fuzz_overflow.sh on its first real run, executing what the engine had
        // proved: UBSan reported `268402689 * 16383 cannot be represented in type 'int'`.
        // The old engine catches this one — it judges the RETURN VALUE's range against the
        // declared type — so it was a new-engine-only hole and a switchover blocker.
        //
        // The check is the same one STORE and CAST already use; only the site was missing.
        if (b->term.kind == IR_TERM_RET && b->term.cond && V->f->ret_type) {
            IrValue *rv = b->term.cond;
            if (vra_type_may_lose(rv->type, V->f->ret_type)) {
                oct_close(&W);
                IrInstr *site = V->def[rv->id];
                vra_check_narrow(V,&W, rv, V->f->ret_type, site,
                                 site ? site->line : 0, site ? site->col : 0);
            }
        }
    }
    // The analysis runs more than once per function in a compile (effects.h drives it for
    // totality, report.h again for the numeric obligations under --engine=ir-full), and a
    // flag that prints the same converged state twice reads as two different states.
    if (vra_dump_enabled && !vra_dumped_already(f)) vra_dump_state(V, stderr);
    // one termination obligation per loop header — but ONLY for a `func` (totality is a
    // func requirement; a `proc` may loop forever, e.g. an event loop). Emitting it for
    // procs was spuriously marking terminating procs "partially proven".
    // ── D-44: A WRITTEN MEASURE IS AN OBLIGATION WHEREVER IT IS WRITTEN ──────────────────
    // A `func` owes termination on every loop, because totality is what `func` means. A `proc`
    // owes it on the loops where the PROGRAMMER SAID SO — `while ... decreasing m` is a claim,
    // and a claim the compiler does not check is worse than one nobody made: it reads as
    // verified. The old engine checked both; the sovereign engine checked only the first, and
    // the gap was invisible until the legacy checks were stood down and three `proc` programs
    // asserting a bad measure compiled.
    //
    // This is the assertion rule applied one level down from `effects ...`: the declaration
    // unlocks no inference (the measure is inferred anyway where it can be), it states an
    // intention, and the compiler defends it against drift.
    for (IrBlock *b=f->blocks; b; b=b->next) {
        if (!b->is_loop_header) continue;
        // ★ THE DEFAULT IS TERMINATION (2026-09-24). Every loop carries the obligation unless
        // its function declared `effects diverge` — and a WRITTEN measure is defended even
        // then, because a claim the compiler does not check reads as verified (D-44).
        //
        // This line used to read `f->kind != IR_FUNC_PURE`, i.e. "a proc may loop forever".
        // That made the guarantee depend on a keyword answering a different question: `proc`
        // means "may do IO", and every IO function was exempt from termination checking for
        // sharing a keyword with the ones that genuinely hang. The exemption is now stated by
        // the function that wants it rather than inherited from how it prints.
        if (f->may_diverge && !b->has_measure) continue;
        VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_TERMINATION; c.ok=vra_loop_terminates(V,b);
        c.had_measure = b->has_measure;
        // The loop's POSITION: its header's condition. The check had none, so E011 printed
        // "Error:" with no line — in a file with several loops, no way to tell which.
        { IrInstr *hc = (b->term.cond && b->term.cond->id>=0 && b->term.cond->id<V->nvar)
                        ? V->def[b->term.cond->id] : NULL;
          if (hc) { c.line = hc->line; c.col = hc->col; } }
        if (vra_dump_measures_enabled) vra_print_measure(V, f, c.line, false, c.ok);
        vra_add_check(V, c);
    }
    // ── RECURSION: the same obligation, one level up ────────────────────────────────────────
    // `vra_recursion_terminates` has existed here since the effect row needed to know whether a
    // recursive cycle diverges, and it is a real well-founded-ranking check: a parameter that
    // strictly descends at every self-call (read from the octagon, so `n/2` and `n-k` count) and
    // is grounded below. But `analysis/effects.h` was its ONLY caller, so it never produced a
    // user-facing verdict, and E011/E082 for recursion went on coming from `src/sema/`.
    //
    // That made an engine that HAS an opinion indistinguishable from one that has none: the
    // project's own plan recorded twice that no recursion analysis existed, and the seam rule
    // ("stand the legacy check down only where the new engine speaks") kept the legacy checks
    // on for exactly that reason. It speaks. Raising the obligation here is what lets it be
    // heard — and is what unblocks standing the legacy recursion checks down.
    // ★ AND THE ROW GOVERNS BOTH SOURCES OF DIVERGENCE. `effects diverge` licensed an unbounded
    // LOOP (the line above) but not an unbounded RECURSION, so one effect had two sources and the
    // row governed one of them — a function that declared it may not terminate was still refused
    // for recursing, with a diagnostic that said "a `func` must be total" and advised declaring it
    // `proc`. Same condition as the loop, for the same reason, including the part that matters:
    // a WRITTEN `decreasing` measure is still checked even under the row, because a claim the
    // compiler does not check reads as verified (D-44).
    if (f->may_diverge && !f->has_decreasing) { /* the row states it; no obligation */ }
    else if (f->kind == IR_FUNC_PURE) {
        IrInstr *site = vra_self_call_site(f);
        if (site) {
            VraCheck c; memset(&c,0,sizeof c);
            c.kind = VRA_TERMINATION; c.recursion = true; c.at = site;
            c.had_measure = f->has_decreasing;
            c.line = site->line; c.col = site->col;
            c.ok = vra_recursion_terminates(V, f);
            if (vra_dump_measures_enabled) vra_print_measure(V, f, c.line, true, c.ok);
            vra_add_check(V, c);
        } else if (vra_mod && !vra_in_mutual_check) {
            // ★ MUTUAL RECURSION IS AN ORDINARY OBLIGATION, raised here beside the self-call one
            // rather than as a bespoke diagnostic in report.h. That placement was not cosmetic: a
            // finding invented in the reporter is invisible to every other client, and
            // `fuzz_termination` — which reads the vra driver's verdict for `run` and then EXECUTES
            // what was proven — silently SKIPPED 24 of 200 generated programs because no verdict
            // line existed for the shape. A skip bucket is data; this one said the new proof was
            // not being fuzzed at all.
            IrInstr *msite = NULL, *mback = NULL; IrFunc *mvia = NULL;
            if (vra_mutual_cycle_edges(f, vra_mod, &msite, &mvia, &mback) && msite) {
                VraCheck c; memset(&c,0,sizeof c);
                c.kind = VRA_TERMINATION; c.recursion = true; c.mutual = true; c.at = msite;
                c.had_measure = f->has_decreasing;
                c.line = msite->line; c.col = msite->col;
                c.ok = (mvia && mback) && vra_mutual_cycle_terminates(f, mvia, msite, mback);
                vra_add_check(V, c);
            }
        }
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
            memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->noct; W.dim=dim; oct_set_clean(&W, V->inclosed[b->id]);
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
    if (vra_depth == 1 && vra_mod) {
        bool all = !f->incomplete;
        for (IrBlock *b=f->blocks; b && all; b=b->next)
            if (b->is_loop_header && !vra_loop_terminates(V, b)) all = false;
        f->vra_loops_summary = all ? 1 : 2;
    }
    oct_map = oct_map_saved;
    vra_depth--;
    return V;
}

// Re-analyse the callee with THIS call's argument intervals. Returns false when nothing is
// tighter than the declared intervals, so the memoised whole-function answer is used instead
// and no work is wasted.
//
// Cost is the reason for the depth cap: this re-analyses a function body per call site, and
// session (70) recorded an 800x slowdown from an unbounded numeric query. Depth 1 means a
// callee analysed under bindings does not itself re-analyse ITS callees under bindings — it
// falls back to the memoised answer, which is exactly today's behaviour.
static bool vra_ret_range_at(Vra *V, Octagon *W, IrFunc *g, IrInstr *call,
                             int64_t *lo, int64_t *hi) {
    if (!g || g->is_extern || !g->ret_type || !call) return false;
    if (g->ret_range_state == 1) return false;            // recursive query, already in flight
    if (vra_retq_depth >= 1) return false;                // bound the work (see above)
    if (call->n_operands <= 0 || call->n_operands > VRA_ARGBIND_MAX) return false;
    // ★ SIZE CAP. This re-analyses a whole function body, and the transfer runs per fixpoint
    // sweep — even with the per-call cache below, an uncapped version cost 2.7x compile time
    // over the corpus (65s against a 24s baseline). The functions this precision is for are
    // small (`dbl`, `pure_id`, `gcd`, `fill`); a large callee called with a constant is not
    // worth a second full analysis, and falls back to the memoised whole-function answer.
    {
        int ninstr = 0;
        for (IrBlock *b = g->blocks; b && ninstr <= VRA_CALLSITE_MAX_INSTR; b = b->next)
            for (IrInstr *i = b->instrs; i && ninstr <= VRA_CALLSITE_MAX_INSTR; i = i->next) ninstr++;
        if (ninstr > VRA_CALLSITE_MAX_INSTR) return false;
    }
    int rid = (call->result && call->result->id >= 0 && call->result->id < V->nvar)
              ? call->result->id : -1;
    if (rid >= 0 && V->cret_state[rid]) {                 // asked before, on an earlier sweep
        if (V->cret_state[rid] == 1) return false;
        *lo = V->cret_lo[rid]; *hi = V->cret_hi[rid]; return true;
    }

    int64_t blo[VRA_ARGBIND_MAX], bhi[VRA_ARGBIND_MAX]; bool bhas[VRA_ARGBIND_MAX];
    bool tighter = false; int n = call->n_operands;
    IrParam *p = g->params;
    for (int k=0; k<n; k++, p = p ? p->next : NULL) {
        bhas[k] = false;
        IrValue *a = call->operands[k];
        if (!p || !p->value || !p->value->type || p->value->type->kind != IRT_INT) continue;
        if (!a || !a->type || a->type->kind != IRT_INT) continue;
        // ONLY an exact constant. Two reasons, both load-bearing: a constant is stable across
        // fixpoint sweeps, which is what makes the per-call cache below sound; and it keeps the
        // trigger rare enough that re-analysing a whole function body stays affordable.
        if (a->id < 0 || a->id >= V->nvar || !V->cknown[a->id]) continue;
        int64_t tlo, thi;
        if (!irtype_int_range(p->value->type, &tlo, &thi)) continue;
        int64_t c = V->cval[a->id];
        if (c <= tlo && c >= thi) continue;               // no news
        bhas[k] = true; blo[k] = c; bhi[k] = c; tighter = true;
    }
    if (!tighter) { if (rid>=0) V->cret_state[rid]=1; return false; }

    // save/restore: the channel is global, and this runs inside an analysis of the caller
    int sn = vra_argbind_n; int sd = vra_retq_depth;
    int64_t slo[VRA_ARGBIND_MAX], shi[VRA_ARGBIND_MAX]; bool shas[VRA_ARGBIND_MAX];
    memcpy(slo, vra_argbind_lo, sizeof slo); memcpy(shi, vra_argbind_hi, sizeof shi);
    memcpy(shas, vra_argbind_has, sizeof shas);
    memcpy(vra_argbind_lo, blo, sizeof blo); memcpy(vra_argbind_hi, bhi, sizeof bhi);
    memcpy(vra_argbind_has, bhas, sizeof bhas);
    vra_argbind_n = n; vra_retq_depth = sd + 1;

    int saved_state = g->ret_range_state;
    int64_t saved_lo = g->ret_range_lo, saved_hi = g->ret_range_hi;
    g->ret_range_state = 1;                                // guard against self-recursion
    Vra *sub = vra_analyze(g);
    bool ok = (g->ret_range_state == 2);
    if (ok) { *lo = g->ret_range_lo; *hi = g->ret_range_hi; }
    vra_free(sub);
    // the memo belongs to the DECLARED-interval analysis; this one is call-site specific
    g->ret_range_state = saved_state; g->ret_range_lo = saved_lo; g->ret_range_hi = saved_hi;

    vra_argbind_n = sn; vra_retq_depth = sd;
    memcpy(vra_argbind_lo, slo, sizeof slo); memcpy(vra_argbind_hi, shi, sizeof shi);
    memcpy(vra_argbind_has, shas, sizeof shas);
    if (rid >= 0) {
        V->cret_state[rid] = ok ? 2 : 1;
        if (ok) { V->cret_lo[rid] = *lo; V->cret_hi[rid] = *hi; }
    }
    return ok;
}

static bool vra_ret_range(IrFunc *g, int64_t *lo, int64_t *hi) {
    if (!g || g->is_extern || !g->ret_type) return false;
    if (g->ret_range_state==1) return false;      // recursive query — no fixpoint over itself
    if (g->ret_range_state==3) return false;      // analysed, nothing usable
    if (g->ret_range_state==0) {
        g->ret_range_state = 1;                   // mark in-progress BEFORE recursing
        // ★ CLEAR THE CALL-SITE BINDINGS FIRST. They are a global channel, and this analysis
        // is of a DIFFERENT function reached from inside a bound one — leaving them set would
        // apply one callee's argument intervals to another callee's parameters, which is a
        // tighter-than-true entry state and therefore a false-proof generator. The memoised
        // answer this computes must be the DECLARED-interval one, valid at every call site.
        int sn = vra_argbind_n; vra_argbind_n = 0;
        Vra *sub = vra_analyze(g);                // sets state to 2 or 3 as a side effect
        vra_free(sub);
        vra_argbind_n = sn;
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
    bool      shared;   // V is the report's analysis (vra_shared_V): not ours to free
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
    bool shared = (vra_shared_f == f && vra_shared_V);
    Vra *V = shared ? vra_shared_V : vra_analyze(f);
    if (!V) return NULL;
    VraDisjoint *D = calloc(1, sizeof *D);
    D->V = V; D->scratch = malloc((size_t)V->dsz*8); D->shared = shared;
    ir_place_index_disjoint_fn  = vra_index_disjoint;
    ir_place_index_disjoint_ctx = D;
    return D;
}
static void vra_disjoint_close(VraDisjoint *D) {
    ir_place_index_disjoint_fn  = NULL;
    ir_place_index_disjoint_ctx = NULL;
    if (!D) return;
    if (!D->shared) vra_free(D->V);
    free(D->scratch); free(D);
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
    vra_last_measure.kind = VRA_MEAS_NONE;
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
                    // ★ CLOSE BEFORE ASKING. The transfers above ADD constraints; it is the
                    // CLOSURE that makes them transitive, and a descent fact is almost always
                    // derived rather than stated: `mid = lo + (hi-lo)/2` yields `mid - hi <= -1`
                    // only by composing the division's fact with the subtraction's. Querying a
                    // non-closed DBM answers about what was written down, not about what is
                    // known — so this read INF for every derived descent, and the rule silently
                    // covered only the shapes some transfer had stated outright.
                    oct_close(&W);
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
        if (ok) {                                          // this param is a measure
            vra_last_measure = (VraMeasure){ VRA_MEAS_PARAM, pv, NULL };
            free(scratch); oct_map = oct_map_saved; return true;
        }
    }

    // ── A DIFFERENCE OF TWO PARAMETERS, WHICH IS WHAT DIVIDE-AND-CONQUER DESCENDS ON ────────
    // Binary search recurses as `bs(mid+1, hi)` and `bs(lo, mid)`: NEITHER bound descends on
    // its own — one rises and one falls, depending on the branch — so the loop above, which
    // asks each parameter in isolation, refuses the whole family. What shrinks is `hi - lo`.
    //
    // Measured before writing this: over every corpus program with a self-call, the
    // single-parameter rule proved 50 termination obligations and refused 10, and NINE of the
    // ten are programs the corpus asserts must be refused. The tenth is exactly this shape.
    // One idiom is worth incomparably more than a sense that the rule is strict — and the
    // language already lets you NAME this measure (`decreasing hi - lo`), so refusing it would
    // be the engine failing to prove something the surface syntax advertises.
    //
    // ── WHY THIS IS AN OCTAGON QUESTION AT ALL ──────────────────────────────────────────
    // Descent of `a - b` is a FOUR-variable relation — (a' - b') - (a - b) <= -1 — and no
    // octagon can hold one. It collapses to two variables exactly when one of the two
    // arguments is UNCHANGED, which is precisely what divide-and-conquer does: it moves one
    // bound and carries the other through. So:
    //
    //   arg_a is param_a  ->  the measure falls iff b STRICTLY GROWS:  ub(b - arg_b) <= -1
    //   arg_b is param_b  ->  the measure falls iff a STRICTLY FALLS:  ub(arg_a - a) <= -1
    //   neither           ->  not answerable here; refuse
    //
    // and the measure is well-founded where `ub(b - a) <= 0`, i.e. `a - b >= 0`, which is what
    // a `lo < hi` guard above the recursion puts in the octagon.
    //
    // Both surviving questions are single differences between two values the domain already
    // relates, which is the whole reason this is affordable: `bsearch(a, mid+1, hi, t)` needs
    // `lo <= mid` and `bsearch(a, lo, mid, t)` needs `mid < hi`, and the midpoint transfer
    // derives both. The first formulation tried here compared ub(new) against lb(old) and
    // failed on every program, because both sides mention an UNBOUNDED `hi` and infinity is
    // not a bound. Asking about the CHANGE rather than the VALUE is what makes it finite.
    {
        IrParam *pa = f->params; int ka = 0;
        for (; pa; pa=pa->next, ka++) {
            IrValue *av = pa->value;
            if (!av || !av->type || av->type->kind != IRT_INT || av->id<0 || av->id>=V->nvar) continue;
            IrParam *pb = f->params; int kb = 0;
            for (; pb; pb=pb->next, kb++) {
                if (kb == ka) continue;
                IrValue *bv = pb->value;
                if (!bv || !bv->type || bv->type->kind != IRT_INT || bv->id<0 || bv->id>=V->nvar) continue;
                bool ok = true;
                for (IrBlock *b=f->blocks; b && ok; b=b->next) {
                    if (!V->reached[b->id] || !V->in[b->id]) continue;
                    memcpy(scratch, V->in[b->id], (size_t)V->dsz*8);
                    Octagon W = { V->noct, dim, scratch };
                    oct_close(&W);
                    for (IrInstr *i=b->instrs; i && ok; i=i->next) {
                        bool self = (i->op==IR_CALL && i->aux.callee && f->name
                                     && i->aux.callee->length==f->name->length
                                     && memcmp(i->aux.callee->name, f->name->name,
                                               (size_t)f->name->length)==0);
                        if (self) {
                            if (ka >= i->n_operands || kb >= i->n_operands) { ok=false; break; }
                            IrValue *aa = i->operands[ka], *ab = i->operands[kb];
                            if (!aa || !ab || aa->id<0 || aa->id>=V->nvar
                                           || ab->id<0 || ab->id>=V->nvar) { ok=false; break; }
                            oct_close(&W);              // see the note in the loop above
                            bool shrinks = false;
                            if (aa->id == av->id)                                  // a carried through
                                shrinks = vra_diff_ub(V, &W, bv->id, ab->id) <= -1;
                            else if (ab->id == bv->id)                             // b carried through
                                shrinks = vra_diff_ub(V, &W, aa->id, av->id) <= -1;
                            bool grounded = vra_diff_ub(V, &W, bv->id, av->id) <= 0;
                            if (!(shrinks && grounded)) { ok=false; break; }
                        }
                        vra_transfer_instr(V, &W, i);
                    }
                }
                if (ok) {
                    vra_last_measure = (VraMeasure){ VRA_MEAS_PARAM_DIFF, av, bv };
                    free(scratch); oct_map = oct_map_saved; return true;
                }
            }
        }
    }
    free(scratch);
    oct_map = oct_map_saved;
    return false;
}

// ── --dump-measures: the measure in the program's own names ─────────────────────────────
static void vra_src_place(Vra *V, IrValue *a, char *buf, size_t n, int depth);
static void vra_src_expr(Vra *V, IrValue *v, char *buf, size_t n, int depth) {
    if (!v || n < 2) { snprintf(buf, n, "?"); return; }
    if (depth > 8) { snprintf(buf, n, "%%%d", v->id); return; }
    if (v->id >= 0 && v->id < V->nvar && V->cknown[v->id]) { snprintf(buf, n, "%lld", (long long)V->cval[v->id]); return; }
    IrInstr *d = (v->id >= 0 && v->id < V->nvar) ? V->def[v->id] : NULL;
    if (!d) {                                                     // a parameter
        if (v->src_name) snprintf(buf, n, "%.*s", (int)v->src_name->length, v->src_name->name);
        else snprintf(buf, n, "%%%d", v->id);
        return;
    }
    char x[160], y[160];
    switch (d->op) {
        case IR_LOAD: vra_src_place(V, d->n_operands >= 1 ? d->operands[0] : NULL, buf, n, depth + 1); return;
        case IR_SLICE_LEN: vra_src_expr(V, d->operands[0], x, sizeof x, depth + 1); snprintf(buf, n, "%s.len", x); return;
        case IR_CAST: vra_src_expr(V, d->operands[0], buf, n, depth + 1); return;
        case IR_ADD: case IR_SUB:
            vra_src_expr(V, d->operands[0], x, sizeof x, depth + 1);
            vra_src_expr(V, d->operands[1], y, sizeof y, depth + 1);
            snprintf(buf, n, "%s %c %s", x, d->op == IR_ADD ? '+' : '-', y);
            return;
        default: snprintf(buf, n, "%%%d", v->id); return;
    }
}
static void vra_src_place(Vra *V, IrValue *a, char *buf, size_t n, int depth) {
    if (!a || depth > 8) { snprintf(buf, n, "?"); return; }
    IrInstr *d = (a->id >= 0 && a->id < V->nvar) ? V->def[a->id] : NULL;
    if (!d || d->op == IR_ALLOCA) {                               // a parameter or a local
        if (a->src_name) snprintf(buf, n, "%.*s", (int)a->src_name->length, a->src_name->name);
        else snprintf(buf, n, "%%%d", a->id);
        return;
    }
    char x[160];
    if (d->op == IR_FIELD_PTR && d->n_operands >= 1) {
        vra_src_place(V, d->operands[0], x, sizeof x, depth + 1);
        IrType *bt = d->operands[0]->type;
        if (bt && (bt->kind == IRT_PTR) && bt->elem) bt = bt->elem;
        int k = (int)d->aux.field_idx;
        IrName *fn = (bt && bt->kind == IRT_STRUCT && k >= 0 && k < bt->n_fields && bt->field_names) ? bt->field_names[k] : NULL;
        if (fn) snprintf(buf, n, "%s.%.*s", x, (int)fn->length, fn->name);
        else snprintf(buf, n, "%s.%d", x, k);
        return;
    }
    if (d->op == IR_LOAD) { vra_src_expr(V, a, buf, n, depth + 1); return; }   // through a reference
    snprintf(buf, n, "%%%d", a->id);
}
// Printed once per loop or recursion per compile, however often the function is re-analysed.
static struct { IrFunc *f; isize line; bool rec; } vra_meas_seen[1024]; static int vra_meas_seen_n;
static void vra_print_measure(Vra *V, IrFunc *f, isize line, bool rec, bool ok) {
    for (int k = 0; k < vra_meas_seen_n; k++)
        if (vra_meas_seen[k].f == f && vra_meas_seen[k].line == line && vra_meas_seen[k].rec == rec) return;
    if (vra_meas_seen_n < 1024) { vra_meas_seen[vra_meas_seen_n].f = f; vra_meas_seen[vra_meas_seen_n].line = line; vra_meas_seen[vra_meas_seen_n++].rec = rec; }
    char a[200], b[200];
    VraMeasure m = vra_last_measure;
    fprintf(stderr, "[measure] %.*s: %s at line %lld: ", f->name ? (int)f->name->length : 1,
            f->name ? f->name->name : "?", rec ? "recursion" : "loop", (long long)line);
    if (!ok || m.kind == VRA_MEAS_NONE) { fprintf(stderr, "no measure found\n"); return; }
    vra_src_expr(V, m.a, a, sizeof a, 0);
    if (m.b) vra_src_expr(V, m.b, b, sizeof b, 0); else snprintf(b, sizeof b, "?");
    // the subtrahend in parentheses only when it is itself a sum: `n - (i + 1)`, `n - i`
    char pa[210], pb[210];
    snprintf(pa, sizeof pa, strchr(a, ' ') ? "(%s)" : "%s", a);
    snprintf(pb, sizeof pb, strchr(b, ' ') ? "(%s)" : "%s", b);
    switch (m.kind) {
        case VRA_MEAS_RISES: fprintf(stderr, "`%s - %s` decreases: %s rises by at least 1 each iteration and stays below %s\n", b, pa, a, b); break;
        case VRA_MEAS_FALLS: fprintf(stderr, "`%s - %s` decreases: %s falls by at least 1 each iteration and stays above %s\n", a, pb, a, b); break;
        case VRA_MEAS_PAIR:  fprintf(stderr, "`%s - %s` decreases: each iteration raises %s or lowers %s, and the loop runs while %s < %s\n", b, a, a, b, a, b); break;
        case VRA_MEAS_PARAM: fprintf(stderr, "`%s` decreases at every self-call and stays at least 0\n", a); break;
        case VRA_MEAS_PARAM_DIFF: fprintf(stderr, "`%s - %s` decreases at every self-call and stays at least 0\n", a, b); break;
        default: fprintf(stderr, "?\n"); break;
    }
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
// ── MUTUAL RECURSION: the sovereign engine's own opinion ─────────────────────────────────────
// `vra_recursion_terminates` reasons about SELF-calls, so `f -> g -> f` drew no obligation from the
// engine at all — and the termination seam STRIPS the front end's DIVERGE bit on the assumption that
// the engine will speak. Measured 2026-09-26 by disabling the one legacy check that was still
// refusing these: `func ping(n) { return pong(n) }  func pong(n) { return ping(n) }` was ACCEPTED,
// and it never terminates. So a single check in `src/frontends/lain/sema.h` was the only thing
// standing between the corpus and a non-terminating `func`.
//
// The engine cannot yet PROVE a mutual cycle well-founded — that needs a ranking over the cycle, not
// over one function — so its honest opinion is "I cannot prove this". That is an obligation, which is
// what the seam rule requires ([[seam-only-where-engine-opines]]) and what lets the legacy check go.
// A function whose row names `diverge` has already said the same thing and is left alone.
//
// Walked with an explicit visited set rather than a bare recursion: a call graph with shared callees
// makes the naive version exponential, and a depth cap would trade that for silent under-reporting.
static bool vra_mutual_cycle_edges(IrFunc *f, IrFunc *mod, IrInstr **site,
                                   IrFunc **via, IrInstr **back) {
    if (!f || !mod) return false;
    int n = 0; for (IrFunc *g = mod; g; g = g->next) n++;
    if (n <= 0) return false;
    IrFunc **idx = (IrFunc**)calloc((size_t)n, sizeof *idx);
    bool   *seen = (bool*)  calloc((size_t)n, sizeof *seen);
    IrFunc **stk = (IrFunc**)calloc((size_t)n, sizeof *stk);
    if (!idx || !seen || !stk) { free(idx); free(seen); free(stk); return false; }
    { int k = 0; for (IrFunc *g = mod; g; g = g->next) idx[k++] = g; }
    bool found = false;
    // Seed with f's DIRECT callees other than f itself: a self-call is the other analysis's
    // business, and seeding with f would report every self-recursive function as mutual.
    for (IrBlock *b = f->blocks; b && !found; b = b->next)
        for (IrInstr *i = b->instrs; i && !found; i = i->next) {
            if (i->op != IR_CALL || !i->aux.callee) continue;
            IrFunc *c = vra_find_in(mod, i->aux.callee);
            if (!c || c == f) continue;
            // Can this callee reach f again? Then f lies on a cycle through c.
            int top = 0; for (int k = 0; k < n; k++) seen[k] = false;
            stk[top++] = c;
            while (top > 0 && !found) {
                IrFunc *cur = stk[--top];
                int ci = -1; for (int k = 0; k < n; k++) if (idx[k] == cur) { ci = k; break; }
                if (ci < 0 || seen[ci]) continue;
                seen[ci] = true;
                for (IrBlock *cb = cur->blocks; cb && !found; cb = cb->next)
                    for (IrInstr *ci2 = cb->instrs; ci2 && !found; ci2 = ci2->next) {
                        if (ci2->op != IR_CALL || !ci2->aux.callee) continue;
                        IrFunc *cc = vra_find_in(mod, ci2->aux.callee);
                        if (!cc) continue;
                        if (cc == f) {
                            found = true;
                            if (site) *site = i;         // f's own call that enters the cycle
                            if (via)  *via  = cur;       // the function that closes it
                            if (back) *back = ci2;       // and the call that closes it
                            break;
                        }
                        if (top < n) stk[top++] = cc;
                    }
            }
        }
    free(idx); free(seen); free(stk);
    return found;
}

// ── MUTUAL RECURSION: A RANKING OVER THE CYCLE ──────────────────────────────────────────────
// `vra_recursion_terminates` reasons about a SELF-call: some parameter strictly shrinks and is
// bounded below. A cycle `f -> g -> f` has no self-call, so until now the engine could only say "I
// cannot rank this" — which is an honest obligation but refuses a shape the language needs, most
// obviously a recursive-descent parser and the textbook `even`/`odd` pair.
//
// The facts COMPOSE, which is what makes this tractable without a new domain. At f's call to g the
// octagon can prove `arg[kg] < f.param[kf]`; at g's call back to f it can prove
// `arg[kf] < g.param[kg]`. Chaining the two:
//
//     f.param[kf]  >  arg[kg] = g.param[kg]  >  arg[kf] = f.param[kf]   (next time round)
//
// so the value threaded through positions (kf, kg) strictly decreases once per lap, and if it is
// bounded below the cycle is well-founded. Both halves are ordinary octagon queries at a program
// POINT, exactly as the self-call rule makes them — no four-variable relation, no new lattice.
//
// Searched over PAIRS of positions rather than assuming they match: `even(n)` calling `odd(n-1)`
// happens to use position 0 on both sides, but a parser's `expr(src, i)` calling `term(src, i)`
// threads its index through position 1, and a helper may take its arguments in another order.
//
// Fail-closed: anything unproven leaves the obligation standing, so a wrong answer here costs
// precision and never soundness. Limited to a 2-cycle deliberately — that is what the corpus and
// every idiom in the language limits document actually contain, and a longer chain is the same
// composition applied more times, which can be added when something needs it.
// `strict` distinguishes the two questions a lap needs: every edge must be NON-INCREASING, and at
// least one must strictly DECREASE. Requiring strict on every edge is the obvious rule and it is
// wrong — it refuses a cycle that threads its measure through a pass-through edge, which is what a
// pair like `p1(x,y) -> p2(y,x-1) -> p1(b,a)` does: the decrease happens once per lap, not twice.
static bool vra_edge_shrinks(Vra *V, IrFunc *caller, IrInstr *call,
                             int k_param, int j_arg, bool strict) {
    if (!V || !caller || !call) return false;
    IrParam *p = caller->params; int idx = 0;
    while (p && idx < k_param) { p = p->next; idx++; }
    if (!p || !p->value || !p->value->type || p->value->type->kind != IRT_INT) return false;
    IrValue *pv = p->value;
    if (pv->id < 0 || pv->id >= V->nvar) return false;
    if (j_arg < 0 || j_arg >= call->n_operands) return false;
    IrValue *arg = call->operands[j_arg];
    if (!arg || arg->id < 0 || arg->id >= V->nvar) return false;

    int dim = 2*V->noct;
    int64_t *scratch = malloc((size_t)V->dsz*8);
    if (!scratch) return false;
    const int *oct_map_saved = oct_map; oct_map = V->odim;
    bool proved = false;
    for (IrBlock *b = caller->blocks; b && !proved; b = b->next) {
        if (!V->reached[b->id] || !V->in[b->id]) continue;
        bool holds_here = false, saw = false;
        memcpy(scratch, V->in[b->id], (size_t)V->dsz*8);
        Octagon W = { V->noct, dim, scratch };
        oct_close(&W);
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (i == call) {
                oct_close(&W);                       // the descent fact is usually DERIVED
                bool shrinks = vra_diff_ub(V, &W, arg->id, pv->id) <= (strict ? -1 : 0);
                int64_t lo, hi; bool hl, hh;
                vra_interval(V, &W, pv->id, &lo, &hl, &hi, &hh);
                bool grounded = (hl && lo >= 0) || !pv->type->is_signed;
                saw = true; holds_here = shrinks && grounded;
                break;
            }
            vra_transfer_instr(V, &W, i);
        }
        if (saw) { proved = holds_here; break; }      // the call appears once; that block decides
    }
    free(scratch); oct_map = oct_map_saved;
    return proved;
}

// Can the 2-cycle f --cfg--> g --cgf--> f be ranked? Analyses both functions and searches the
// position pairs.
static bool vra_mutual_cycle_terminates(IrFunc *f, IrFunc *g, IrInstr *cfg, IrInstr *cgf) {
    if (!f || !g || !cfg || !cgf) return false;
    int nf = 0, ng = 0;
    for (IrParam *p = f->params; p; p = p->next) nf++;
    for (IrParam *p = g->params; p; p = p->next) ng++;
    if (nf == 0 || ng == 0 || nf > 16 || ng > 16) return false;
    bool guard_saved = vra_in_mutual_check;
    vra_in_mutual_check = true;                 // the inner analyses must not re-raise the cycle
    Vra *Vf = vra_analyze(f); if (!Vf) { vra_in_mutual_check = guard_saved; return false; }
    Vra *Vg = vra_analyze(g); if (!Vg) { vra_free(Vf); vra_in_mutual_check = guard_saved; return false; }
    bool ok = false;
    for (int kf = 0; kf < nf && !ok; kf++)
        for (int kg = 0; kg < ng && !ok; kg++) {
            // Over one lap the measure must not GROW on either edge and must FALL on at least one.
            // That is well-foundedness exactly: total change <= -1 per lap, with a floor, so the
            // cycle cannot run forever.
            if (!vra_edge_shrinks(Vf, f, cfg, kf, kg, false)) continue;
            if (!vra_edge_shrinks(Vg, g, cgf, kg, kf, false)) continue;
            if (vra_edge_shrinks(Vf, f, cfg, kf, kg, true) ||
                vra_edge_shrinks(Vg, g, cgf, kg, kf, true)) ok = true;
        }
    vra_free(Vf); vra_free(Vg);
    vra_in_mutual_check = guard_saved;
    return ok;
}

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
    free(V->in); free(V->reached); free(V->inclosed); free(V->def); free(V->defblk); free(V->cval); free(V->cknown); free(V->modwrap);
    free(V->slicelen); free(V->cellcanon); free(V->val); free(V->subslice_gep); free(V->escaped); free(V->persist); free(V->shape_rank); free(V->shape_ext); free(V->elem_lo); free(V->elem_hi); free(V->elem_known); free(V->cret_lo); free(V->cret_hi); free(V->cret_state); free(V->accum_cell); free(V->uniq_store); free(V->strict_esc); free(V->odim); free(V->checks); free(V);
}

#endif // LAIN_VRA_H
