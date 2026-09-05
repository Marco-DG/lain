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

// A function's DIRECT effects: its own calls, panics, allocations, and (semantic) divergence.
static IrEffect ir_effects_direct(IrFunc *f, IrFunc *mod) {
    IrEffect e = 0;
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op != IR_CALL) continue;
            const IrName *cn = ins->aux.callee;
            if (ireff_name_is(cn, "panic", 5)) { e |= IR_EFFECT_RAISES; continue; }
            IrFunc *callee = ireff_find(mod, cn);
            if (callee) e |= ir_effects(callee, mod);   // transitive (memoized)
            else       e |= IR_EFFECT_IO;               // unknown callee ⇒ opaque external effect
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
    if (f->effects_in_progress) return IR_EFFECT_DIVERGE;   // recursion cycle
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
