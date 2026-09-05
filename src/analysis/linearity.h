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
#include <stdlib.h>
#include <string.h>

typedef struct { int slot; isize line, col; int code; } LinFinding;  // 1=E001, 2=E002, 3=E003 leak

typedef struct {
    IrFunc     *f;
    int         nvar, nb;
    bool      **in;         // in[block][slot] = maybe-moved on entry
    bool       *linsl;      // linsl[slot] = the slot holds a LEAK-relevant resource (owned ptr/slice)
    bool       *movesl;     // movesl[slot] = the slot holds a LINEAR value (move-tracked)
    IrInstr   **def;        // def[value] = the instruction defining it
    LinFinding *finds; int nfinds, cap;
} Lin;

static void lin_add(Lin *L, int slot, isize line, isize col, int code) {
    if (L->nfinds==L->cap){ L->cap = L->cap?L->cap*2:8; L->finds=realloc(L->finds,L->cap*sizeof*L->finds); }
    L->finds[L->nfinds++] = (LinFinding){slot,line,col,code};
}

// Apply one block's instructions to `st` (moved[]) — the transfer function. When `report`,
// flag a use/double-move against the running state (used only in the final sweep).
static void lin_run_block(Lin *L, IrBlock *b, bool *st, bool report) {
    for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
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
                if (o1 && o1->id>=0 && o1->id<L->nvar) {
                    IrInstr *d = L->def[o1->id];
                    if (d && d->op==IR_LOAD && d->n_operands>=1 && d->operands[0]) {
                        int src = d->operands[0]->id;
                        if (src>=0 && src<L->nvar && L->movesl[src] && !st[src]) st[src] = true;
                    }
                }
                if (o0->id>=0 && o0->id<L->nvar) st[o0->id] = false;
            }
            continue;
        }
        if (ins->op==IR_CONSUME) {                       // `mov` — mark moved (double-move if already)
            if (o0 && o0->id>=0 && o0->id<L->nvar) {
                if (report && st[o0->id]) lin_add(L, o0->id, ins->line, ins->col, 2);
                st[o0->id] = true;
            }
            continue;
        }
        if (report)                                      // any other reference to a moved slot = use
            for (int k=0;k<ins->n_operands;k++) {
                IrValue *ok = ins->operands[k];
                if (ok && ok->id>=0 && ok->id<L->nvar && st[ok->id]) lin_add(L, ok->id, ins->line, ins->col, 1);
            }
    }
}

static Lin *lin_analyze(IrFunc *f) {
    Lin *L = calloc(1,sizeof *L);
    L->f=f; L->nvar = f->next_value_id>0?f->next_value_id:1; L->nb = f->next_block_id;
    L->in = calloc(L->nb,sizeof(bool*));
    for (int i=0;i<L->nb;i++) L->in[i]=calloc(L->nvar,sizeof(bool));
    L->linsl  = calloc(L->nvar,sizeof(bool));
    L->movesl = calloc(L->nvar,sizeof(bool));
    L->def    = calloc(L->nvar,sizeof(IrInstr*));
    // Two DISTINCT sets, conflated at first and worth keeping apart:
    //   movesl — every LINEAR slot. Move-tracked: reading it out is a transfer of ownership.
    //   linsl  — the leak-relevant subset that owns a RESOURCE needing release (owned ptr or
    //            slice). An owned struct that is move-tracked but trivially droppable is not
    //            a leak; one that transitively owns a resource needs per-field tracking.
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->result && ins->result->id>=0 && ins->result->id<L->nvar)
                L->def[ins->result->id] = ins;
            if (ins->op==IR_ALLOCA && ins->result && ins->aux.alloca_ty && ins->aux.alloca_ty->linear) {
                L->movesl[ins->result->id] = true;
                if (ins->aux.alloca_ty->kind==IRT_PTR || ins->aux.alloca_ty->kind==IRT_SLICE)
                    L->linsl[ins->result->id] = true;
            }
        }
    bool *out = malloc(L->nvar), *tmp = malloc(L->nvar);

    // forward MAY fixpoint: in[succ] |= transfer(in[pred])
    bool changed=true; int sweeps=0;
    while (changed && sweeps++ < 1000) {
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            memcpy(tmp, L->in[b->id], L->nvar); lin_run_block(L, b, tmp, false);  // out = transfer(in)
            IrBlock *succ[3]={0,0,0}; int ns=0;
            switch (b->term.kind) {
                case IR_TERM_BR:      succ[ns++]=b->term.a; break;
                case IR_TERM_BR_COND: succ[ns++]=b->term.a; succ[ns++]=b->term.b; break;
                case IR_TERM_SWITCH:  succ[ns++]=b->term.a;
                    for (IrSwitchCase *c=b->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                default: break;
            }
            for (int k=0;k<ns;k++){ IrBlock *s=succ[k]; if(!s) continue;
                bool *si=L->in[s->id];
                for (int v=0;v<L->nvar;v++) if (tmp[v] && !si[v]){ si[v]=true; changed=true; }
            }
        }
    }
    // reporting sweep: replay each block from its converged in-state
    for (IrBlock *b=f->blocks; b; b=b->next) {
        memcpy(out, L->in[b->id], L->nvar);
        lin_run_block(L, b, out, true);
        // E003 leak: a linear slot still LIVE (not consumed) when the function returns.
        // `return mov x` consumes x first, so a returned resource is not flagged.
        if (b->term.kind==IR_TERM_RET)
            for (int s=0;s<L->nvar;s++)
                if (L->linsl[s] && !out[s]) lin_add(L, s, b->term.cond?b->term.cond->line:0,
                                                    b->term.cond?b->term.cond->col:0, 3);
    }
    free(out); free(tmp);
    return L;
}
static void lin_free(Lin *L){ if(!L)return; for(int i=0;i<L->nb;i++) free(L->in[i]); free(L->in); free(L->linsl); free(L->movesl); free(L->def); free(L->finds); free(L); }

#endif // LAIN_LINEARITY_H
