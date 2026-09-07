// src/analysis/footprint.h — PARAMETER FOOTPRINTS: two IR-level facts about what a call
// can reach through its arguments. They sit BELOW both the numeric domain and the effect
// lattice because both need them and neither defines them.
//
//   ir_param_writes  (C5)  — which parameters may this function write THROUGH, during the call
//   ir_param_retains       — which parameters' ADDRESSES may OUTLIVE the call
//
// Together they are the alias oracle: a call can perturb a caller's cell either because it
// was handed the address and writes it (WRITES), or because some earlier call kept the
// address and writes it now (RETAINS). A cell that is neither is untouched by the call, and
// the numeric domain may keep every fact it holds about it across the call — which is the
// whole point. Rooted through the address-forming ops, transitive over the call graph, and
// conservative wherever the body is not visible (extern / opaque ⇒ all ones).
#ifndef LAIN_FOOTPRINT_H
#define LAIN_FOOTPRINT_H

#include "../ir/ir.h"
#include <string.h>
#include <stdlib.h>

static IrFunc *ireff_find(IrFunc *mod, const IrName *name) {
    if (!name) return NULL;
    for (IrFunc *f=mod; f; f=f->next)
        if (f->name && f->name->length==name->length &&
            memcmp(f->name->name, name->name, (size_t)name->length)==0) return f;
    return NULL;
}

// C5: which PARAMETERS may this function write through? Rooted through the address-forming
// ops, so writing `p.field` or `p[i]` counts as writing p. Transitive: handing a parameter to
// a callee that writes its k-th parameter writes ours too.
//
// Conservative wherever it cannot see: an extern (no body) or an opaque may write anything.
static int ireff_param_index(IrFunc *f, IrValue *v) {
    int i = 0;
    for (IrParam *p = f->params; p; p = p->next, i++) if (p->value == v) return i;
    return -1;
}
static int ireff_root_param(IrFunc *f, IrInstr **def, int nvar, IrValue *v) {
    for (int guard=0; v && v->id>=0 && v->id<nvar && guard<10000; guard++) {
        IrInstr *d = def[v->id];
        if (!d) return ireff_param_index(f, v);       // no defining instr ⇒ a parameter
        switch (d->op) {
            case IR_ELEM_PTR: case IR_FIELD_PTR:
            case IR_SLICE_DATA: case IR_MAKE_SLICE:
                v = d->n_operands>=1 ? d->operands[0] : NULL; break;
            default: return -1;
        }
    }
    return -1;
}
// Retention needs a MASK, not a single root, and it must root through more than the
// address-forming ops:
//
//   * CASTS — a pointer laundered through an integer (`h.n = usize(var x)`) is still x's
//     address, and a walk that stops at a cast would answer "retains nothing";
//   * AGGREGATE CONSTRUCTORS — `Holder(var x)` carries &x into a struct, and the store that
//     follows stores the STRUCT, not the pointer. A single-chain walk sees an IR_STRUCT_NEW,
//     gives up, and misses the escape entirely. That is why this returns a mask: one value
//     may carry the addresses of several parameters at once.
static IrRetainFootprint ireff_addr_mask(IrFunc *f, IrInstr **def, int nvar, IrValue *v, int depth) {
    if (!v || v->id<0 || v->id>=nvar || depth>64) return 0;
    IrInstr *d = def[v->id];
    if (!d) { int k = ireff_param_index(f, v); return (k>=0 && k<64) ? (IrRetainFootprint)1<<k : 0; }
    switch (d->op) {
        case IR_ELEM_PTR: case IR_FIELD_PTR:
        case IR_SLICE_DATA: case IR_MAKE_SLICE: case IR_SUBSLICE: case IR_CAST:
            return ireff_addr_mask(f, def, nvar, d->n_operands>=1?d->operands[0]:NULL, depth+1);
        case IR_STRUCT_NEW: case IR_ARRAY_NEW: case IR_SUM_NEW: {
            IrRetainFootprint m = 0;
            for (int i=0;i<d->n_operands;i++)
                m |= ireff_addr_mask(f, def, nvar, d->operands[i], depth+1);
            return m;
        }
        default: return 0;
    }
}
static IrWriteFootprint ir_param_writes(IrFunc *f, IrFunc *mod) {
    if (!f) return ~(IrWriteFootprint)0;
    if (f->is_extern) return ~(IrWriteFootprint)0;    // no body ⇒ assume it writes everything
    if (f->param_writes_done) return f->param_writes;
    f->param_writes_done = true;                       // recursion: the in-progress value is 0,
    f->param_writes = 0;                               // refined below (a fixpoint would only add)
    int nvar = f->next_value_id>0?f->next_value_id:1;
    IrInstr **def = calloc(nvar, sizeof(IrInstr*));
    if (!def) { f->param_writes = ~(IrWriteFootprint)0; return f->param_writes; }
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next)
            if (i->result && i->result->id>=0 && i->result->id<nvar) def[i->result->id]=i;
    IrWriteFootprint w = 0;
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next) {
            if (i->op == IR_STORE && i->n_operands>=1) {
                int k = ireff_root_param(f, def, nvar, i->operands[0]);
                if (k>=0 && k<64) w |= (IrWriteFootprint)1<<k;
            } else if (i->op == IR_OPAQUE && i->aux.opaque.writes) {
                w = ~(IrWriteFootprint)0;              // unmodelled ⇒ may write anything
            } else if (i->op == IR_CALL) {
                IrFunc *callee = ireff_find(mod, i->aux.callee);
                IrWriteFootprint cw = callee ? ir_param_writes(callee, mod) : ~(IrWriteFootprint)0;
                for (int a=0; a<i->n_operands; a++) {
                    if (a<64 && !((cw>>a)&1u)) continue;      // callee does not write there
                    int k = ireff_root_param(f, def, nvar, i->operands[a]);
                    if (k>=0 && k<64) w |= (IrWriteFootprint)1<<k;
                }
            }
        }
    free(def);
    f->param_writes = w;
    return w;
}

// ── RETENTION footprint: does a parameter's ADDRESS outlive the call? ────────────────────
// The alias oracle in vra.h wants to stop havocing every escaped cell at every call and havoc
// only what THIS call can reach. That is sound exactly when a callee cannot squirrel an address
// away for someone ELSE to write later — which Lain permits, because a struct field may hold a
// borrow (`type Holder { r var i32 }`), so the question has to be asked rather than assumed.
//
// A parameter is RETAINED if a pointer derived from it is STORED into memory, RETURNED, or
// passed to a callee that retains it. Storing an integer derived from it is not retention:
// `bump(var x) { x = x + 1 }` retains nothing, `stash(var h, var x) { h.r = var x }` retains x.
static IrRetainFootprint ir_param_retains(IrFunc *f, IrFunc *mod) {
    if (!f) return ~(IrRetainFootprint)0;
    if (f->is_extern) return ~(IrRetainFootprint)0;   // no body ⇒ assume it keeps everything
    if (f->param_retains_done) return f->param_retains;
    f->param_retains_done = true;                     // recursion: 0 in progress, refined below
    f->param_retains = 0;
    int nvar = f->next_value_id>0?f->next_value_id:1;
    IrInstr **def = calloc(nvar, sizeof(IrInstr*));
    if (!def) { f->param_retains = ~(IrRetainFootprint)0; return f->param_retains; }
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next)
            if (i->result && i->result->id>=0 && i->result->id<nvar) def[i->result->id]=i;
    IrRetainFootprint r = 0;
    for (IrBlock *b=f->blocks;b;b=b->next) {
        for (IrInstr *i=b->instrs;i;i=i->next) {
            if (i->op == IR_STORE && i->n_operands>=2) {
                // The VALUE stored. No type filter: the walk itself is the filter — it
                // reaches a parameter only through address-forming ops and casts, so a stored
                // `x + 1` roots nowhere while a stored `&x` (however disguised) roots at x.
                r |= ireff_addr_mask(f, def, nvar, i->operands[1], 0);
            } else if (i->op == IR_OPAQUE) {
                r = ~(IrRetainFootprint)0;             // unmodelled ⇒ assume it keeps everything
            } else if (i->op == IR_CALL) {
                IrFunc *callee = ireff_find(mod, i->aux.callee);
                IrRetainFootprint cr = callee ? ir_param_retains(callee, mod) : ~(IrRetainFootprint)0;
                for (int a=0; a<i->n_operands; a++) {
                    if (a<64 && !((cr>>a)&1u)) continue;
                    r |= ireff_addr_mask(f, def, nvar, i->operands[a], 0);
                }
            }
        }
        // a RETURNED pointer outlives the call by definition
        if (b->term.kind==IR_TERM_RET && b->term.cond)
            r |= ireff_addr_mask(f, def, nvar, b->term.cond, 0);
    }
    free(def);
    f->param_retains = r;
    return r;
}

#endif // LAIN_FOOTPRINT_H
