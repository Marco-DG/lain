// src/analysis/effects.h — Phase 3.3: the effect row as an IR pass (audit finding F1).
//
// Computes each function's effect set (IR_EFFECT_*) from the IR ALONE — no AST — and
// propagates it transitively over the call graph (callee ⊆ caller), memoized on IrFunc.
// This is the general effect LATTICE that replaces the PURE/PROC binary as the semantic
// authority on side effects; a `func` is exactly one whose effects avoid IO and DIVERGE.
//
// Sovereign: reads only ir.h + vra.h (the latter for loop-termination, i.e. DIVERGE). The
// module (IrFunc list, from ir_lower_module) carries extern STUBS so calls to externs are
// classified without the front-end.
#ifndef LAIN_EFFECTS_H
#define LAIN_EFFECTS_H

#include "../ir/ir.h"
#include "vra.h"
#include <string.h>

static bool ireff_name_is(const IrName *n, const char *s, int len) {
    return n && n->length==len && memcmp(n->name, s, (size_t)len)==0;
}
static IrFunc *ireff_find(IrFunc *mod, const IrName *name) {
    if (!name) return NULL;
    for (IrFunc *f=mod; f; f=f->next)
        if (f->name && f->name->length==name->length &&
            memcmp(f->name->name, name->name, (size_t)name->length)==0) return f;
    return NULL;
}

static IrEffect ir_effects(IrFunc *f, IrFunc *mod);   // fwd (recursion)

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

// A function's DIRECT effects: its own calls, panics, allocations, and (semantic) divergence.
static IrEffect ir_effects_direct(IrFunc *f, IrFunc *mod) {
    IrEffect e = 0;
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            // B3: an UNMODELLED construct could do anything, so it contributes the
            // conservative observable footprint. Without this, a function whose only
            // unmodelled part was an opaque looked PURE — and the effect row is what gates
            // the `const`/`pure` C annotations, where a wrong answer is a miscompile (gcc
            // eliding a call that must happen). This is the case the gate caught: 4
            // functions the effects pass previously SKIPPED (they were `incomplete`) started
            // being analysed and dropped their observable effects.
            if (ins->op == IR_OPAQUE) {
                // EVERY observable effect: an unmodelled construct may print, may panic, and
                // (per its footprint) may write. Leaving RAISES out was not conservative
                // enough — `else panic` inside an unmodelled expression lost its Raises and
                // the function looked non-panicking, which is exactly the annotation
                // miscompile this row exists to prevent.
                e |= IR_EFFECT_IO | IR_EFFECT_RAISES;
                if (ins->aux.opaque.writes) e |= IR_EFFECT_WRITE;
                continue;
            }
            if (ins->op != IR_CALL) continue;
            const IrName *cn = ins->aux.callee;
            if (ireff_name_is(cn, "panic", 5)) { e |= IR_EFFECT_RAISES; continue; }
            IrFunc *callee = ireff_find(mod, cn);
            if (callee) e |= ir_effects(callee, mod);   // transitive (memoized)
            else       e |= IR_EFFECT_IO;               // unknown callee ⇒ opaque external effect
        }
    // ALLOC: the function PRODUCES owned storage it did not receive.
    //
    // The bit existed in the lattice from the start and NOTHING EVER SET IT (D-14), so a real
    // allocator and a `printf` had the same row — `{IO}` — and `mem_alloc` was indistinguishable
    // from `mem_free`. Computing it needs a definition that is not Lain-shaped, and this is it:
    // a function allocates iff it RETURNS an owned pointer/slice whose provenance does not root
    // in one of its own parameters. That is what allocation IS in any language — new owned
    // storage appearing at the boundary — and it is decided from the IR alone.
    //
    // It also draws the distinction the row could not: `mem_alloc` returns owned storage rooted
    // in a call ⇒ ALLOC; `mem_free` consumes and returns nothing ⇒ not; a pass-through that
    // returns a parameter's own storage ⇒ not.
    if (f->ret_type && (f->ret_type->kind==IRT_PTR || f->ret_type->kind==IRT_SLICE)
        && f->ret_type->linear) {
        for (IrBlock *b=f->blocks; b && !(e & IR_EFFECT_ALLOC); b=b->next) {
            if (b->term.kind != IR_TERM_RET || !b->term.cond) continue;
            // walk the returned value back to its root through the address-forming ops
            IrValue *v = b->term.cond; bool from_param = false;
            for (int guard=0; v && guard<64; guard++) {
                IrInstr *d = NULL;
                for (IrBlock *bb=f->blocks; bb && !d; bb=bb->next)
                    for (IrInstr *i=bb->instrs; i; i=i->next)
                        if (i->result == v) { d = i; break; }
                if (!d) { from_param = true; break; }          // no defining instr ⇒ a parameter
                if (d->op==IR_CAST || d->op==IR_SLICE_DATA || d->op==IR_MAKE_SLICE
                    || d->op==IR_FIELD_PTR || d->op==IR_ELEM_PTR) { v = d->operands[0]; continue; }
                if (d->op==IR_LOAD && d->n_operands>=1) { v = d->operands[0]; continue; }
                break;                                          // a call/alloca/opaque root
            }
            if (!from_param) e |= IR_EFFECT_ALLOC;
        }
    }

    // DIVERGE: any loop the analyzer can't prove terminates. Uses vra_loop_terminates
    // DIRECTLY (not the func-gated VRA_TERMINATION obligation), so it applies to procs too.
    Vra *V = vra_analyze(f);
    for (IrBlock *b=f->blocks; b; b=b->next)
        if (b->is_loop_header && !vra_loop_terminates(V, b)) { e |= IR_EFFECT_DIVERGE; break; }
    vra_free(V);
    return e;
}

// Transitive effects, memoized. extern func = pure {}, extern proc = IO. A recursion cycle
// yields DIVERGE via the in-progress guard (conservative: the IR does not carry the
// `decreasing` measure that would let a well-founded self-call stay total — a known gap).
static IrEffect ir_effects(IrFunc *f, IrFunc *mod) {
    if (!f) return 0;
    if (f->is_extern)          return f->kind==IR_FUNC_PROC ? IR_EFFECT_IO : 0u;
    if (f->effects_done)       return f->effects;
    if (f->effects_in_progress) {
        // 3.4: a recursion cycle is DIVERGE only if no parameter is a WELL-FOUNDED MEASURE.
        // Ask the octagon: does some parameter strictly shrink at every self-call and stay
        // bounded below? A binary search or a tree walk does, and was being called divergent
        // purely because the cycle rule could not look.
        Vra *V = vra_analyze(f);
        bool total = vra_recursion_terminates(V, f);
        vra_free(V);
        return total ? 0u : IR_EFFECT_DIVERGE;
    }
    f->effects_in_progress = true;
    IrEffect e = ir_effects_direct(f, mod);
    f->effects = e; f->effects_done = true; f->effects_in_progress = false;
    return e;
}

// Compute effects for every function in the module (drives the fixpoint from each root).
static void ir_effects_module(IrFunc *mod) {
    for (IrFunc *f=mod; f; f=f->next) if (!f->is_extern) ir_effects(f, mod);
}

#endif // LAIN_EFFECTS_H
