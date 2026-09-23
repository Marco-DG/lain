// src/analysis/linearity.h — Phase 3.2: linearity / ownership as an IR CFG pass.
//
// Tracks move semantics on the sovereign IR: `mov x` (an IR_CONSUME on x's slot) makes the
// source MOVED-FROM; any later reference to it before it is re-initialized is a use-after-
// move. This is the memory-safety core of linearity — using a moved-from owner is exactly a
// potential use-after-free.
//
//   E001  use of a slot after it was moved
//   E002  a slot moved twice (double consume)
//
// Method: a forward MAY analysis over the CFG (a slot is "maybe moved" at a point if it is
// moved on SOME path reaching it — the sound direction for flagging a use). A STORE to the
// slot re-initializes it (moved → live). Fixpoint over the blocks, then a reporting sweep.
//
// Sovereign: reads only ir.h. No AST. (Leak/consistency checks E003/E016 and linear-type
// tracking are follow-ups; this cut is the use-after-move / double-move core.)
#ifndef LAIN_LINEARITY_H
#define LAIN_LINEARITY_H

#include "../ir/ir.h"
#include "../ir/place.h"
#include <stdlib.h>
#include <string.h>

#define LIN_WHOLE_BIT 63u

#define LIN_MAX_LEAVES 63
#define LIN_MAX_PATH    8

typedef struct { int8_t n; int16_t step[LIN_MAX_PATH]; } LinPath;


typedef struct { int slot; isize line, col; int code; } LinFinding;  // 1=E001, 2=E002, 3=E003 leak

typedef struct {
    IrFunc     *f;
    int         nvar, nb;
    uint64_t  **in;         // in[block][slot] = consumption mask (bit63 whole, bits0..62 fields)
    bool       *linsl;      // linsl[slot] = the slot holds a LEAK-relevant resource (owned ptr/slice)
    bool       *movesl;     // movesl[slot] = the slot holds a LINEAR value (move-tracked)
    IrInstr   **def;        // def[value] = the instruction defining it
    IrType    **slot_ty;    // element type per tracked slot (for the "all linear fields" rule)
    LinPath   **slot_leaf;  // slot -> its obligation leaves (the location tree, A.2)
    int        *slot_nleaf; // how many; -1 = the type needs more than can be represented
    LinFinding *finds; int nfinds, cap;
} Lin;

// Does a value of this type carry a RELEASE OBLIGATION — i.e. does it (transitively) own a
// resource that someone must hand back? This is the leak question, and it is NOT the same as
// linearity. `Counter{ value i32 }` bound by `mov` is linear (exactly one owner) but owns
// nothing releasable: dropping it leaks no memory, so reporting E003 on it is an
// over-rejection. `Res{ mov h *u8 }` does own one.
//
// Language-neutral: this is "has a non-trivial destructor" (C++) / "impl Drop" (Rust). The
// IR's job is the FACT — an owned value dies unconsumed here. Whether that is an ERROR or a
// site for an implicit drop is the front end's policy, and front ends differ: Rust and C++
// insert the drop, a strict linear discipline rejects. We report only where a resource would
// actually be lost, which is the intersection every front end agrees is a bug.
static bool lin_has_release_obligation(const IrType *t, int depth) {
    if (!t || depth > 8) return false;
    if ((t->kind==IRT_PTR || t->kind==IRT_SLICE) && t->linear) return true;
    if (t->kind==IRT_STRUCT) {
        // An OPAQUE linear struct — no visible fields, because a cross-module type lowers
        // without them — must be assumed to own a resource. Concluding "owns nothing" about a
        // type we cannot SEE is a fail-OPEN, and it silently exempted every std handle:
        // `std/fs.ln`'s `File { mov handle *FILE }` reaches this pass with n_fields == 0, so
        // an unclosed file was not a leak. For a leak check the safe direction is to report.
        if (t->n_fields <= 0 || !t->fields) return t->linear;
        for (int i=0;i<t->n_fields;i++)
            if (lin_has_release_obligation(t->fields[i], depth+1)) return true;
    }
    if (t->kind==IRT_ARRAY) return lin_has_release_obligation(t->elem, depth+1);
    // A sum owns a resource if ANY variant's payload does — `Box{ Full{ptr mov *u8}, Empty }`
    // must be released on the Full arm. Which arm is taken is a runtime fact; the TYPE-level
    // answer is "may own", and the per-path refinement happens where the payload is projected.
    if (t->kind==IRT_SUM) {
        if (t->n_fields <= 0 || !t->fields) return t->linear;
        for (int i=0;i<t->n_fields;i++)
            if (lin_has_release_obligation(t->fields[i], depth+1)) return true;
    }
    return false;
}

static void lin_add(Lin *L, int slot, isize line, isize col, int code) {
    if (L->nfinds==L->cap){ L->cap = L->cap?L->cap*2:8; L->finds=realloc(L->finds,L->cap*sizeof*L->finds); }
    L->finds[L->nfinds++] = (LinFinding){slot,line,col,code};
}

// Is a slot's obligation discharged under mask `m`? Either the whole value was consumed, or
// every LINEAR field of it was — `consume(r mov Resource) { return mov r.handle }` discharges
// the struct by consuming its only linear field, which the corpus requires to be accepted.
static bool lin_discharged(const Lin *L, int slot, uint64_t m) {
    if ((m >> LIN_WHOLE_BIT) & 1u) return true;     // an escape marks the whole place gone
    if (slot < 0 || slot >= L->nvar) return false;
    int n = L->slot_nleaf[slot];
    if (n <= 0) return false;                        // unrepresentable, or nothing to discharge
    // ★ ONE RULE. Every obligation leaf of this place has been consumed. The old code had two
    // special cases — "all linear FIELDS of a struct" and, later, "all ELEMENTS of an array" —
    // and neither could express `a[0].h1`, which is a leaf of both kinds at once.
    uint64_t need = (n >= 63) ? ~(1ull<<LIN_WHOLE_BIT) : ((1ull<<n) - 1u);
    return (m & need) == need;
}

// ══ THE LOCATION TREE (plan A.2) ═════════════════════════════════════════════════════════
//
// A place is a root plus a path of steps — `a[0].h1` is "slot a, element 0, field 1". The
// flat mask could name ONE step: bit 63 for the whole value, bits 0..62 for a depth-1 field
// OR a depth-1 element, never both and never deeper. Everything that went wrong with arrays
// came from that: `a[0].h1` had to either claim the whole element (fail-OPEN — a sibling
// obligation lost in silence) or resolve to nothing (fail-closed, and the shape unusable).
//
// ★ THE REPRESENTATION. Rather than carry path SETS through the dataflow — which would mean
// replacing union/intersection over uint64_t with something the fixpoint cannot do cheaply —
// enumerate each type's OBLIGATION LEAVES once and give each leaf a bit. A leaf is a place
// that owns a resource and has no owning parts of its own:
//
//     type R { mov h1 H, mov h2 H }   R[2]  ->  a[0].h1  a[0].h2  a[1].h1  a[1].h2
//                                                 bit 0    bit 1    bit 2    bit 3
//
// Then every operation is a MASK over leaves, and the existing machinery is untouched:
//
//     mov a[0].h1   sets one bit          consume covers the leaves UNDER the named place
//     mov a[0]      sets bits 0 and 1
//     mov a         sets all four
//     discharged    all leaf bits set     — the "all linear fields" rule falls out, and so
//                                           does the array rule, instead of being two cases
//     moved twice   a bit already set     — precise at any depth
//     use-after-move  any leaf under the place being read is set
//
// Depth and width are bounded (8 and 63) and EXCEEDING EITHER FAILS CLOSED, the same way the
// old field bound did: an obligation with no bit is one nobody is tracking, and for a
// memory-safety property that must refuse, not shrug.

// Enumerate the obligation leaves of `t`, appending each one's path. Returns false if the
// type needs more leaves or more depth than can be represented.
static bool lin_leaves(const IrType *t, LinPath *cur, LinPath *out, int *n) {
    if (!t) return true;
    if (cur->n > LIN_MAX_PATH) return false;
    // A sum's payload is projected by IR_SUM_PAYLOAD, which owns its own slot from that point
    // on; the sum itself is one obligation until then. An OPAQUE struct (no visible fields,
    // a cross-module type) is one obligation for the reason lin_has_release_obligation gives:
    // concluding "owns nothing" about a type we cannot see is a fail-open.
    bool opaque_agg = (t->kind==IRT_STRUCT || t->kind==IRT_SUM) && (t->n_fields <= 0 || !t->fields);
    if (((t->kind==IRT_PTR || t->kind==IRT_SLICE) && t->linear) || t->kind==IRT_SUM || opaque_agg) {
        if (!lin_has_release_obligation(t, 0)) return true;
        if (*n >= LIN_MAX_LEAVES) return false;
        out[(*n)++] = *cur;
        return true;
    }
    if (t->kind == IRT_STRUCT) {
        for (int i=0;i<t->n_fields;i++) {
            if (!t->fields[i] || !lin_has_release_obligation(t->fields[i],0)) continue;
            if (cur->n >= LIN_MAX_PATH) return false;
            LinPath sub = *cur; sub.step[sub.n++] = (int16_t)i;
            if (!lin_leaves(t->fields[i], &sub, out, n)) return false;
        }
        return true;
    }
    if (t->kind == IRT_ARRAY) {
        if (!t->elem || !lin_has_release_obligation(t->elem,0)) return true;
        if (t->array_len <= 0 || t->array_len > LIN_MAX_LEAVES) return false;
        for (int64_t k=0;k<t->array_len;k++) {
            if (cur->n >= LIN_MAX_PATH) return false;
            LinPath sub = *cur; sub.step[sub.n++] = (int16_t)k;
            if (!lin_leaves(t->elem, &sub, out, n)) return false;
        }
        return true;
    }
    return true;
}

// The mask of every leaf lying UNDER the path `pre` (a prefix match). An empty prefix is the
// whole value, so `mov a` and "all leaves" are the same statement rather than a special case.
static uint64_t lin_mask_under(const LinPath *leaves, int nleaf, const int16_t *pre, int npre) {
    uint64_t m = 0;
    for (int i=0;i<nleaf;i++) {
        if (leaves[i].n < npre) continue;
        bool pfx = true;
        for (int k=0;k<npre && pfx;k++) if (leaves[i].step[k] != pre[k]) pfx = false;
        if (pfx) m |= (1ull<<i);
    }
    return m;
}

// The place a consume/store names, as (slot, bit). bit = LIN_WHOLE_BIT for the whole value,
// or the field index for a depth-1 field. Returns -1 for anything it cannot resolve.
// Build (once) the leaf table for a slot. A type that cannot be enumerated — too wide, too
// deep — records -1, and lin_place_of then refuses to name any place in it. An obligation
// with no bit is one nobody is tracking, and for a memory-safety property that must refuse.
static void lin_build_leaves(Lin *L, int slot, IrType *t) {
    if (slot < 0 || slot >= L->nvar || L->slot_leaf[slot] || L->slot_nleaf[slot]) return;
    if (!t) return;
    LinPath tmp[LIN_MAX_LEAVES]; LinPath cur; cur.n = 0;
    int n = 0;
    if (!lin_leaves(t, &cur, tmp, &n)) { L->slot_nleaf[slot] = -1; return; }
    if (n <= 0) { L->slot_nleaf[slot] = 0; return; }
    L->slot_leaf[slot] = malloc((size_t)n * sizeof(LinPath));
    if (!L->slot_leaf[slot]) { L->slot_nleaf[slot] = -1; return; }
    memcpy(L->slot_leaf[slot], tmp, (size_t)n * sizeof(LinPath));
    L->slot_nleaf[slot] = n;
}

// Resolve the place an address names to (slot, MASK OF LEAVES it covers). The mask is what
// makes depth work: `a[0]` covers every leaf under element 0, `a[0].h1` covers exactly one,
// `a` covers them all — one rule instead of three special cases.
//
// Returns -1 when the place cannot be named, which leaves the whole obligation outstanding.
// That is the fail-closed direction, and it is what a DEREF base, a variable index, or a type
// too wide or too deep to enumerate all get.
static int lin_place_of(Lin *L, IrValue *addr, uint64_t *mask) {
    IrPlace p = ir_place_of(L->def, L->nvar, addr);
    if (!p.valid || p.base_kind==IRPB_DEREF) return -1;
    int slot = p.base_id;
    if (slot < 0 || slot >= L->nvar) return -1;
    if (L->slot_nleaf[slot] <= 0) {
        // -1 means the type could not be enumerated, and that must not silently become
        // "whole": it is the fail-closed case.
        if (L->slot_nleaf[slot] < 0) return -1;
        // ★ ZERO LEAVES IS NOT "NOTHING TO TRACK". `type Resource { id i32 }` bound by `mov`
        // is LINEAR — exactly one owner, so moving it twice is an error — while owning nothing
        // releasable, so dropping it leaks nothing. Move-tracking and leak-tracking are
        // different questions, and the leaf table only answers the second.
        //
        // Returning an empty mask here made the consume record nothing, and
        // `defer drop(mov r); drop(mov r)` was ACCEPTED. The whole-value bit is the unit for
        // a value whose obligation cannot be subdivided, which is exactly this case.
        *mask = (1ull<<LIN_WHOLE_BIT);
        return slot;
    }
    // Walk the projection into a path of steps. A FIELD step is its index; an INDEX step is
    // its constant value. Anything else — a variable index, a deref partway down — stops the
    // walk and the place is unnameable.
    int16_t pre[LIN_MAX_PATH]; int npre = 0;
    for (int i=0;i<p.nproj;i++) {
        if (npre >= LIN_MAX_PATH) return -1;
        if (p.proj[i].kind == IRPJ_FIELD) {
            if (p.proj[i].field < 0) return -1;
            pre[npre++] = (int16_t)p.proj[i].field;
        } else if (p.proj[i].kind == IRPJ_INDEX && p.proj[i].index) {
            IrValue *iv = p.proj[i].index;
            IrInstr *d = (iv->id >= 0 && iv->id < L->nvar) ? L->def[iv->id] : NULL;
            if (!d || d->op != IR_CONST) return -1;          // a variable index names no leaf
            int64_t k = d->aux.imm;
            if (k < 0 || k > INT16_MAX) return -1;
            pre[npre++] = (int16_t)k;
        } else {
            return -1;                                        // IRPJ_DEREF, or an untracked index
        }
    }
    uint64_t m = lin_mask_under(L->slot_leaf[slot], L->slot_nleaf[slot], pre, npre);
    // A path that covers NO leaf is a place with no obligation under it — reading `r.id` next
    // to a linear `r.h`. That is not unresolvable, it is empty, and treating the two the same
    // made a non-owning field look untrackable.
    *mask = m;
    return slot;
}

// The module, for resolving a call's callee. Set by the caller (as borrow.h does for its
// write-footprint query); NULL simply makes the call rule fall back to the conservative side.
static IrFunc *lin_mod = NULL;
static IrFunc *lin_find_func(const IrName *n) {
    if (!n || !lin_mod) return NULL;
    // By CONTENT: ir_intern allocates a fresh IrName per call despite its name, so comparing
    // pointers silently never matches — and "callee not found" is the permissive branch here.
    for (IrFunc *g=lin_mod; g; g=g->next)
        if (g->name && g->name->length==n->length && memcmp(g->name->name,n->name,(size_t)n->length)==0)
            return g;
    return NULL;
}

// ── ESCAPE = MOVE ─────────────────────────────────────────────────────────────
// Consuming a place is not only `mov`. An owned value READ OUT of its slot and then handed
// somewhere the function can no longer reach it has transferred ownership just as surely,
// and the pass must see that or it reports a leak on correct code. Three escapes:
//
//   return v            ownership goes to the caller          (`return File(raw)`)
//   Struct{ .., v, .. } ownership goes into the aggregate     (`File(raw)`)
//   f(.., v, ..)        ownership goes to the callee, iff that parameter is OWNED
//
// The last one needs the callee, because passing a linear value to a BORROWING parameter is
// not a move — `write_file(f, s)` must leave `f` live. That distinction is the whole reason
// this cannot be a syntactic rule over "appears as an operand".
//
// Reads that do NOT escape (`raw == 0`, `p.x`, arithmetic) are not moves, matching every
// affine system: comparing a value borrows it.
//
// `lin_root_slot` walks back from a value to the slot it was loaded out of. Casts are
// ownership-TRANSPARENT — `libc_free(mov ptr as *void)` loads the owned pointer, casts it,
// and frees the cast; without following the cast the source slot looks unconsumed.
static int lin_root_slot(Lin *L, IrValue *v, int depth) {
    if (!v || v->id<0 || v->id>=L->nvar || depth>8) return -1;
    if (L->movesl[v->id]) return v->id;   // the value IS the resource (sum payload, home-less param)
    IrInstr *d = L->def[v->id];
    if (!d) return -1;
    if (d->op==IR_CAST && d->n_operands>=1) return lin_root_slot(L, d->operands[0], depth+1);
    if (d->op==IR_LOAD && d->n_operands>=1 && d->operands[0]) {
        int b = d->operands[0]->id;
        if (b>=0 && b<L->nvar && L->movesl[b]) return b;
    }
    return -1;
}

// Consume the slot `v` was loaded out of, if it is still live. Guarded on !already-consumed
// for the same reason move-on-assign is: `mov p` lowers to load;consume;<escape>, so the slot
// is already marked and re-flagging it would be a spurious E002.
// ⚠ AN ESCAPE MARKS EVERY LEAF, NOT JUST THE WHOLE-VALUE BIT. Once consumes became masks
// over leaves, setting only bit 63 here put the two in different bit spaces: a later
// `release(mov r)` tested `st & leafmask` and found nothing, so a double free through an
// escape was ACCEPTED. Three corpus tests caught it immediately — defer_double_consume,
// defer_double_defer and linear_copy_double_free — which is what they are for.
static void lin_escape(Lin *L, IrValue *v, uint64_t *st) {
    int sl = lin_root_slot(L, v, 0);
    if (sl<0 || sl>=L->nvar || st[sl]) return;
    int n = L->slot_nleaf[sl];
    uint64_t leaves = (n > 0) ? ((n >= 63) ? ~(1ull<<LIN_WHOLE_BIT) : ((1ull<<n) - 1u)) : 0;
    st[sl] = (1ull<<LIN_WHOLE_BIT) | leaves;
}

// Apply one block's instructions to `st` (consumption masks) — the transfer function. When
// `report`, flag a use/double-move against the running state (used only in the final sweep).
static void lin_run_block(Lin *L, IrBlock *b, uint64_t *st, bool report) {
    IrInstr *prev = NULL;                 // for the `consume %p; call f(%p)` handover, below
    for (IrInstr *ins=b->instrs; ins; prev = ins, ins=ins->next) {
        IrValue *o0 = ins->n_operands>=1 ? ins->operands[0] : NULL;
        if (ins->op==IR_STORE) {                         // store re-initializes the target slot
            if (o0) {
                IrValue *o1 = ins->n_operands>=2 ? ins->operands[1] : NULL;
                if (report && o1 && o1->id>=0 && o1->id<L->nvar && st[o1->id])   // value read is a use
                    lin_add(L, o1->id, ins->line, ins->col, 1);
                // MOVE-ON-ASSIGN. `var q = p` where p is LINEAR is a move, not a copy: the
                // resource has one owner, so reading it out of its slot into another slot
                // transfers it. This is an IR-level rule over IrType.linear, not a Lain one —
                // C++ move semantics, Rust affine types and Lain `mov` all agree that copying
                // a non-copyable value moves it. Without it `var q = p; free(p); free(q)` is a
                // double free the pass cannot see (it was a real P0 hole, ASan-confirmed).
                //
                // Guarded on !st[src]: `var q = mov p` lowers to load;consume;store, so the
                // slot is ALREADY moved here and re-flagging it would be a spurious E002. A
                // genuine second read (`var r = p`) is caught at its own LOAD by the moved-use
                // check below, which is the more precise report anyway.
                lin_escape(L, o1, st);   // storing it elsewhere is an escape like any other
                { uint64_t m; int sl = lin_place_of(L, o0, &m);          // a store RE-INITIALISES
                  if (sl>=0) st[sl] &= ~m; }   // every leaf under the written place is live again
            }
            continue;
        }
        if (ins->op==IR_CONSUME) {                       // `mov` — consume the PLACE it names
            uint64_t m; int sl = o0 ? lin_place_of(L, o0, &m) : -1;
            if (sl>=0 && sl<L->nvar && m) {
                // Any leaf under this place that is ALREADY consumed makes this a second move
                // of something — precise at any depth now, so `mov a[0].h1` twice is caught
                // while `mov a[0].h1; mov a[0].h2` is not.
                if (report && (st[sl] & m)) lin_add(L, sl, ins->line, ins->col, 2);
                st[sl] |= m;
            }
            continue;
        }
        // A `mov` of an already-moved place lowers to `load; consume`, so the LOAD reaches
        // the catch-all below first and reports it as a use-after-move (code 1) before the
        // CONSUME can report it as the double move it is (code 2). Both are true, but E002
        // "this value is moved twice" names what the programmer did; E001 describes the
        // symptom. Let the consume speak.
        bool feeds_consume = false;
        if (ins->op==IR_LOAD && ins->next && ins->next->op==IR_CONSUME &&
            ins->n_operands>=1 && ins->next->n_operands>=1) {
            uint64_t lm, cm;
            int ls = lin_place_of(L, ins->operands[0], &lm);
            int cs = lin_place_of(L, ins->next->operands[0], &cm);
            feeds_consume = (ls>=0 && ls==cs && lm==cm);
        }
        if (report && !feeds_consume)                    // any other reference to a moved slot = use
            for (int k=0;k<ins->n_operands;k++) {
                // Projecting the payload follows the tag test of the SAME destructure, which
                // already moved the resource out. It is one `case`, not a second use — the
                // second `case b` gets its own sum_tag and IS reported there.
                if (k==0 && ins->op==IR_SUM_PAYLOAD) continue;
                IrValue *ok = ins->operands[k];
                if (!ok || ok->id<0 || ok->id>=L->nvar || !st[ok->id]) continue;
                // ── FORMING THE ADDRESS OF A SIBLING IS NOT A USE (D-31) ─────────────────
                // `take(mov a[0]); take(mov a[1])` computes `&a[1]` from the slot `a`, whose
                // mask now has bit 0 set. Reporting any non-zero mask made that a
                // use-after-move, so consuming an array one element at a time — the only
                // correct way to release an array of resources — was refused.
                //
                // A PARTIAL consume says a sibling place is gone, not this one. Whether THIS
                // place is gone is decided where it is actually read or consumed: the consume
                // rule above reports the double move (E002), which is the precise diagnosis.
                // A WHOLE consume still reports here — after `mov a`, `a[1]` really is a use
                // of something that is gone.
                // ── `mov a` ON AN AGGREGATE IS A HANDOVER, NOT A USE (D-35) ──────────
                // An array or struct passed by ADDRESS lowers `f(mov a)` to
                // `consume %a; call f(%a)` — the call's own operand IS the place the consume
                // just marked, so the use check fired on the very instruction the `mov` exists
                // to feed. A by-VALUE argument never hit this: it lowers to `load; consume;
                // call(%loaded)` and the call names the loaded value, whose slot is clean.
                // The distinction was the calling convention, not the language.
                if (ins->op==IR_CALL && prev && prev->op==IR_CONSUME &&
                    prev->n_operands>=1 && prev->operands[0]==ok) continue;
                bool projecting = (ins->op==IR_ELEM_PTR || ins->op==IR_FIELD_PTR) && k==0;
                bool whole_gone = (st[ok->id] >> LIN_WHOLE_BIT) & 1u;
                if (projecting && !whole_gone) continue;
                lin_add(L, ok->id, ins->line, ins->col, 1);
            }
        // ESCAPES (see lin_escape). An aggregate takes ownership of every operand it is built
        // from; a call takes ownership only of the arguments its callee OWNS.
        if (ins->op==IR_STRUCT_NEW || ins->op==IR_SUM_NEW || ins->op==IR_MAKE_SLICE) {
            for (int k=0;k<ins->n_operands;k++) lin_escape(L, ins->operands[k], st);
        } else if (ins->op==IR_SUM_TAG || ins->op==IR_SUM_PAYLOAD) {
            // DESTRUCTURING an owned sum moves the resource OUT of it: after `case b`, `b` no
            // longer owns anything — each arm's payload binding does, and carries its own
            // obligation (classified above). The corpus warns that discharging the scrutinee
            // on a `case` is unsound, and it was: without payload state the resource simply
            // vanished, accepting both `Full(ptr): noop()` (leak) and a double free. With the
            // payload tracked the transfer is complete rather than a hole — both of those are
            // caught, and the Empty arm, which owns nothing, stops reporting a phantom leak.
            lin_escape(L, ins->operands[0], st);
        } else if (ins->op==IR_CALL) {
            IrFunc *callee = lin_find_func(ins->aux.callee);
            IrParam *p = callee ? callee->params : NULL;
            for (int k=0;k<ins->n_operands;k++) {
                // No callee (unresolved / indirect): a by-value LINEAR argument is taken to
                // move. That is the strict side for the memory-safety property (use after
                // move) and the permissive side for leaks, which is the right trade when the
                // callee cannot be seen at all.
                bool owned = p ? (p->value && p->value->owns) : true;
                if (owned) lin_escape(L, ins->operands[k], st);
                if (p) p = p->next;
            }
        }
    }
    if (b->term.kind==IR_TERM_RET && b->term.cond) lin_escape(L, b->term.cond, st);  // to the caller
}

// ── P3: a fixpoint bound must be DERIVED and its exhaustion must be LOUD ───────────────────
// Spec pillar P3 requires verification to be "decidable and polynomial in program size. No SMT
// solver or unbounded fixpoint iteration is required." So a bound here is not a wart — it is
// the pillar. What was wrong is that it was the constant 1000 with no diagnostic: measured over
// the corpus and std, the high-water mark is 114 sweeps, so the headroom was 8.8x, and a
// program an order of magnitude larger would have silently proceeded over a NON-fixpoint.
// Silently, because the loop simply stopped.
//
// These lattices are finite and the transfer functions monotone, so the iteration cannot need
// more sweeps than there are (block, slot) pairs it could still change, plus one to observe
// that nothing did. That is the derived bound below. Reaching it does not mean "the program is
// too big" — it means the monotonicity assumption is false, which is a compiler bug, so it
// aborts rather than reporting a result it cannot stand behind.
#ifndef LAIN_FIXPOINT_BOUND
#define LAIN_FIXPOINT_BOUND(nb, nvar) ((long)(nb) * (long)(nvar) + 2L)
#define LAIN_FIXPOINT_EXHAUSTED(what, nb, nvar) do { \
    fprintf(stderr, "internal error: %s did not reach a fixpoint within the derived bound " \
            "(%ld sweeps for %d blocks x %d slots). This is a compiler bug: the transfer " \
            "function is not monotone. Refusing to report analysis results.\n", \
            (what), LAIN_FIXPOINT_BOUND(nb, nvar), (int)(nb), (int)(nvar)); \
    exit(70); \
} while (0)
#endif

static Lin *lin_analyze(IrFunc *f) {
    Lin *L = calloc(1,sizeof *L);
    L->f=f; L->nvar = f->next_value_id>0?f->next_value_id:1; L->nb = f->next_block_id;
    L->in = calloc(L->nb,sizeof(uint64_t*));
    for (int i=0;i<L->nb;i++) L->in[i]=calloc(L->nvar,sizeof(uint64_t));
    L->linsl  = calloc(L->nvar,sizeof(bool));
    L->movesl = calloc(L->nvar,sizeof(bool));
    L->def    = calloc(L->nvar,sizeof(IrInstr*));
    L->slot_ty= calloc(L->nvar,sizeof(IrType*));
    L->slot_leaf  = calloc(L->nvar,sizeof(LinPath*));
    L->slot_nleaf = calloc(L->nvar,sizeof(int));
    // Two DISTINCT sets, conflated at first and worth keeping apart:
    //   movesl — every LINEAR slot. Move-tracked: reading it out is a transfer of ownership.
    //   linsl  — the leak-relevant subset: linear AND OWNED by this binding, so this function
    //            is the one obliged to consume it. A borrowed binding of the same linear type
    //            (`get_id(r Resource)`) releases nothing and must not be reported — keying on
    //            the type alone reported a leak in every shared-borrow callee.
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->result && ins->result->id>=0 && ins->result->id<L->nvar)
                L->def[ins->result->id] = ins;
            // A payload PROJECTED OUT of an owned sum is itself an owned resource: the
            // `case` arm that binds it is the only place it can be released. Tracking it here
            // — rather than consuming the scrutinee wholesale, which is unsound (it discharges
            // the obligation with nobody releasing anything) — is what makes
            // `Full(ptr): noop()` a leak and `Full(ptr): free(mov ptr)` clean. The arm is a
            // distinct CFG path, so the created-liveness pass below gives the Empty arm, where
            // no payload exists, no obligation at all.
            if (ins->op==IR_SUM_PAYLOAD && ins->result && ins->result->id>=0
                && ins->result->id<L->nvar && ins->result->type
                && lin_has_release_obligation(ins->result->type,0)
                && ins->n_operands>=1 && ins->operands[0] && ins->operands[0]->owns) {
                L->movesl[ins->result->id] = true;
                L->linsl [ins->result->id] = true;
                L->slot_ty[ins->result->id] = ins->result->type;
                lin_build_leaves(L, ins->result->id, ins->result->type);
            }
            if (ins->op==IR_ALLOCA && ins->result && ins->aux.alloca_ty && ins->aux.alloca_ty->linear) {
                L->movesl[ins->result->id] = true;
                L->slot_ty[ins->result->id] = ins->aux.alloca_ty;
                lin_build_leaves(L, ins->result->id, ins->aux.alloca_ty);
                if (ins->result->owns && lin_has_release_obligation(ins->aux.alloca_ty,0))
                    L->linsl[ins->result->id] = true;
            }
        }
    // An OWNED parameter (`mov r R`) is a resource the callee must consume before returning.
    // Track it only when it has NO HOME SLOT: a struct param is materialised as
    // `%s = alloca; store %s, %p`, and `mov r` consumes the SLOT, not the incoming value —
    // tracking both reported the same resource twice and flagged a correctly-consumed
    // parameter as leaked.
    for (IrParam *p=f->params; p; p=p->next) {
        IrValue *pv = p->value;
        if (!pv || !pv->owns || !pv->type || !pv->type->linear) continue;
        if (!lin_has_release_obligation(pv->type,0)) continue;   // nothing to release ⇒ no leak
        if (pv->id<0 || pv->id>=L->nvar) continue;
        // The discharge rule reads slot_ty to ask "which sub-obligations does this place
        // have" — all linear fields of a struct, all elements of an array. It was only ever
        // set for allocas, so an OWNED PARAMETER had no type there: `proc drain(mov a R[2])`
        // releasing both elements still reported a leak, because the rule could not see that
        // `a` was an array of two in the first place. A parameter is a place like any other.
        // ⚠ Only a POINTER is stripped. Writing `pv->type->elem ? ... : pv->type` took the
        // ELEMENT type of an array parameter, so the discharge rule ran the STRUCT branch and
        // one released element satisfied the whole array — a callee that took `mov a R[2]`
        // and released only `a[0]` was ACCEPTED, leaking the other. A weaker check that
        // happens to make the target program pass is worse than the refusal it replaced.
        if (!L->slot_ty[pv->id])
            L->slot_ty[pv->id] = (pv->type->kind == IRT_PTR && pv->type->elem)
                               ? pv->type->elem : pv->type;
        lin_build_leaves(L, pv->id, L->slot_ty[pv->id]);
        bool has_home = false;
        for (IrBlock *b=f->blocks; b && !has_home; b=b->next)
            for (IrInstr *i=b->instrs; i; i=i->next)
                if (i->op==IR_STORE && i->n_operands>=2 && i->operands[1]==pv
                    && i->operands[0] && L->def[i->operands[0]->id]
                    && L->def[i->operands[0]->id]->op==IR_ALLOCA) { has_home = true; break; }
        if (has_home) continue;                       // the home slot carries the obligation
        L->movesl[pv->id] = true;
        L->linsl[pv->id]  = true;
    }
    uint64_t *out = malloc(L->nvar*sizeof(uint64_t)), *tmp = malloc(L->nvar*sizeof(uint64_t));

    // forward MAY fixpoint: in[succ] |= transfer(in[pred])
    bool changed=true; int sweeps=0;
    while (changed) {
        if (sweeps++ > LAIN_FIXPOINT_BOUND(L->nb, L->nvar)) LAIN_FIXPOINT_EXHAUSTED("linearity", L->nb, L->nvar);
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            memcpy(tmp, L->in[b->id], L->nvar*sizeof(uint64_t)); lin_run_block(L, b, tmp, false);  // out = transfer(in)
            IrBlock *succ[3]={0,0,0}; int ns=0;
            switch (b->term.kind) {
                case IR_TERM_BR:      succ[ns++]=b->term.a; break;
                case IR_TERM_BR_COND: succ[ns++]=b->term.a; succ[ns++]=b->term.b; break;
                case IR_TERM_SWITCH:  succ[ns++]=b->term.a;
                    for (IrSwitchCase *c=b->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                default: break;
            }
            for (int k=0;k<ns;k++){ IrBlock *s=succ[k]; if(!s) continue;
                uint64_t *si=L->in[s->id];
                for (int v=0;v<L->nvar;v++) if ((si[v]|tmp[v]) != si[v]){ si[v]|=tmp[v]; changed=true; }
            }
        }
    }
    // A second, MUST fixpoint over the SAME transfer function, differing only in the merge:
    // INTERSECTION instead of union. The two lattices answer different questions and the pass
    // needs both — asking one of them twice is what left conditional consumption invisible.
    //   MAY  (union)        moved on SOME path   ⇒ a later use is a use-after-move (E001/E002)
    //   MUST (intersection) moved on EVERY path  ⇒ the obligation is discharged
    // MAY ∧ ¬MUST is exactly "consumed on some branches but not others" — E016, the
    // inconsistent linear state that a discipline without implicit drop must reject. It was
    // invisible because the leak check asked only ¬MAY ("never consumed anywhere").
    uint64_t **inmust = calloc(L->nb,sizeof(uint64_t*));
    for (int i=0;i<L->nb;i++) inmust[i]=calloc(L->nvar,sizeof(uint64_t));
    bool *seen = calloc(L->nb,sizeof(bool));
    seen[f->entry->id] = true;
    changed=true; sweeps=0;
    while (changed) {
        if (sweeps++ > LAIN_FIXPOINT_BOUND(L->nb, L->nvar)) LAIN_FIXPOINT_EXHAUSTED("linearity", L->nb, L->nvar);
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!seen[b->id]) continue;
            memcpy(tmp, inmust[b->id], L->nvar*sizeof(uint64_t)); lin_run_block(L, b, tmp, false);
            IrBlock *succ[3]={0,0,0}; int ns=0;
            switch (b->term.kind) {
                case IR_TERM_BR:      succ[ns++]=b->term.a; break;
                case IR_TERM_BR_COND: succ[ns++]=b->term.a; succ[ns++]=b->term.b; break;
                case IR_TERM_SWITCH:  succ[ns++]=b->term.a;
                    for (IrSwitchCase *c=b->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                default: break;
            }
            for (int k=0;k<ns;k++){ IrBlock *s=succ[k]; if(!s) continue;
                if (!seen[s->id]) { memcpy(inmust[s->id],tmp,L->nvar*sizeof(uint64_t)); seen[s->id]=true; changed=true; continue; }
                uint64_t *si=inmust[s->id];
                for (int v=0;v<L->nvar;v++) if ((si[v]&tmp[v]) != si[v]){ si[v]&=tmp[v]; changed=true; }
            }
        }
    }
    // ── WHERE DOES THE OBLIGATION EXIST? ─────────────────────────────────────────
    // A leak is reported at a RET, but a slot only carries an obligation at a return the
    // resource actually reaches. `main` returning 1 from an early error branch has not yet
    // run the `var ptr = malloc(..)` below it — there is nothing to leak on that path, and
    // reporting one is a false positive on every function that validates its arguments before
    // acquiring anything. MAY-created (union) is the sound side: created on SOME path and
    // never consumed IS a leak.
    bool **crt = calloc(L->nb,sizeof(bool*));
    for (int i=0;i<L->nb;i++) crt[i]=calloc(L->nvar,sizeof(bool));
    for (IrParam *p=f->params; p; p=p->next)          // parameters exist from entry
        if (p->value && p->value->id>=0 && p->value->id<L->nvar) crt[f->entry->id][p->value->id]=true;
    changed=true; sweeps=0;
    while (changed) {
        if (sweeps++ > LAIN_FIXPOINT_BOUND(L->nb, L->nvar)) LAIN_FIXPOINT_EXHAUSTED("linearity", L->nb, L->nvar);
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            bool *cur = calloc(L->nvar,sizeof(bool));
            memcpy(cur, crt[b->id], L->nvar*sizeof(bool));
            for (IrInstr *ins=b->instrs; ins; ins=ins->next)
                if ((ins->op==IR_ALLOCA || ins->op==IR_SUM_PAYLOAD)
                    && ins->result && ins->result->id>=0 && ins->result->id<L->nvar)
                    cur[ins->result->id]=true;
            IrBlock *succ[3]={0,0,0}; int ns=0;
            switch (b->term.kind) {
                case IR_TERM_BR:      succ[ns++]=b->term.a; break;
                case IR_TERM_BR_COND: succ[ns++]=b->term.a; succ[ns++]=b->term.b; break;
                case IR_TERM_SWITCH:  succ[ns++]=b->term.a;
                    for (IrSwitchCase *c=b->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                default: break;
            }
            for (int k=0;k<ns;k++){ IrBlock *sb=succ[k]; if(!sb) continue;
                for (int v=0;v<L->nvar;v++) if (cur[v] && !crt[sb->id][v]){ crt[sb->id][v]=true; changed=true; }
            }
            free(cur);
        }
    }

    // MUST-created (intersection), the companion to the MAY set above. The two answer
    // different questions and E003/E016 need one each:
    //   MAY-created  ∧ never consumed anywhere        ⇒ E003, a resource is lost
    //   MUST-created ∧ consumed on some paths, not all ⇒ E016, the state is not well-defined
    // Asking MAY for E016 reports a phantom on every `case` arm: a payload projected on the
    // Full arm is not consumed on the Empty arm because it does not EXIST there.
    bool **crtm = calloc(L->nb,sizeof(bool*));
    for (int i=0;i<L->nb;i++) crtm[i]=calloc(L->nvar,sizeof(bool));
    bool *cseen = calloc(L->nb,sizeof(bool)); cseen[f->entry->id]=true;
    for (IrParam *p=f->params; p; p=p->next)
        if (p->value && p->value->id>=0 && p->value->id<L->nvar) crtm[f->entry->id][p->value->id]=true;
    changed=true; sweeps=0;
    while (changed) {
        if (sweeps++ > LAIN_FIXPOINT_BOUND(L->nb, L->nvar)) LAIN_FIXPOINT_EXHAUSTED("linearity", L->nb, L->nvar);
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!cseen[b->id]) continue;
            bool *cur = calloc(L->nvar,sizeof(bool));
            memcpy(cur, crtm[b->id], L->nvar*sizeof(bool));
            for (IrInstr *ins=b->instrs; ins; ins=ins->next)
                if ((ins->op==IR_ALLOCA || ins->op==IR_SUM_PAYLOAD)
                    && ins->result && ins->result->id>=0 && ins->result->id<L->nvar)
                    cur[ins->result->id]=true;
            IrBlock *succ[3]={0,0,0}; int ns=0;
            switch (b->term.kind) {
                case IR_TERM_BR:      succ[ns++]=b->term.a; break;
                case IR_TERM_BR_COND: succ[ns++]=b->term.a; succ[ns++]=b->term.b; break;
                case IR_TERM_SWITCH:  succ[ns++]=b->term.a;
                    for (IrSwitchCase *c=b->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                default: break;
            }
            for (int k=0;k<ns;k++){ IrBlock *sb=succ[k]; if(!sb) continue;
                if (!cseen[sb->id]) { memcpy(crtm[sb->id],cur,L->nvar*sizeof(bool)); cseen[sb->id]=true; changed=true; continue; }
                for (int v=0;v<L->nvar;v++) if (crtm[sb->id][v] && !cur[v]){ crtm[sb->id][v]=false; changed=true; }
            }
            free(cur);
        }
    }

    uint64_t *must = malloc(L->nvar*sizeof(uint64_t));
    // reporting sweep: replay each block from its converged in-state
    for (IrBlock *b=f->blocks; b; b=b->next) {
        memcpy(out, L->in[b->id], L->nvar*sizeof(uint64_t));
        lin_run_block(L, b, out, true);
        if (b->term.kind==IR_TERM_RET) {
            memcpy(must, inmust[b->id], L->nvar*sizeof(uint64_t));
            if (seen[b->id]) lin_run_block(L, b, must, false);
            // The block's OUT-created set: what reaches this return, including the allocas in
            // this very block (a function whose whole body is one block has all of them here).
            bool *here = calloc(L->nvar,sizeof(bool));
            memcpy(here, crt[b->id], L->nvar*sizeof(bool));
            bool *hered = calloc(L->nvar,sizeof(bool));
            memcpy(hered, crtm[b->id], L->nvar*sizeof(bool));
            for (IrInstr *ins=b->instrs; ins; ins=ins->next)
                if ((ins->op==IR_ALLOCA || ins->op==IR_SUM_PAYLOAD)
                    && ins->result && ins->result->id>=0 && ins->result->id<L->nvar)
                    { here[ins->result->id]=true; hered[ins->result->id]=true; }
            // A terminator carries no position of its own, and a `return` with no value has
            // no operand to borrow one from — so every leak reported at a void return pointed
            // at line 0. The last instruction in the block is where control actually leaves.
            isize bl = 0, bc = 0;
            for (IrInstr *q=b->instrs; q; q=q->next) if (q->line) { bl=q->line; bc=q->col; }
            for (int s=0;s<L->nvar;s++) {
                if (!here[s]) continue;               // the resource never reaches this return
                isize ln = b->term.cond&&b->term.cond->line?b->term.cond->line:bl;
                isize cl = b->term.cond&&b->term.cond->line?b->term.cond->col:bc;
                // E003 leak: never consumed on ANY path, and a resource would be lost.
                if (L->linsl[s] && !lin_discharged(L,s,out[s])) { lin_add(L, s, ln, cl, 3); continue; }
                // E016: consumed on some paths, not others — the state is not well-defined.
                if (L->movesl[s] && lin_discharged(L,s,out[s]) && seen[b->id] && hered[s]
                    && !lin_discharged(L,s,must[s])) lin_add(L, s, ln, cl, 16);
            }
            free(here); free(hered);
        }
    }
    for (int i=0;i<L->nb;i++){ free(inmust[i]); free(crt[i]); free(crtm[i]); }
    free(crt); free(crtm); free(cseen);
    free(inmust); free(seen); free(must);
    free(out); free(tmp);
    return L;
}
static void lin_free(Lin *L){ if(!L)return; for(int i=0;i<L->nb;i++) free(L->in[i]); free(L->in);
    for(int i=0;i<L->nvar;i++) free(L->slot_leaf[i]); free(L->slot_leaf); free(L->slot_nleaf); free(L->slot_ty); free(L->linsl); free(L->movesl); free(L->def); free(L->finds); free(L); }

#endif // LAIN_LINEARITY_H
