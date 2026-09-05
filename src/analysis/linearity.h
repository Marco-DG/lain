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
    bool       *linsl;      // linsl[slot] = the slot holds a LINEAR (must-consume) value
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
        if (ins->op==IR_STORE) {                         // store re-initializes the target slot
            if (ins->n_operands>=1) {
                int tgt = ins->operands[0]->id;
                if (report && ins->n_operands>=2 && st[ins->operands[1]->id])   // value read is a use
                    lin_add(L, ins->operands[1]->id, ins->line, ins->col, 1);
                if (tgt>=0 && tgt<L->nvar) st[tgt] = false;
            }
            continue;
        }
        if (ins->op==IR_CONSUME) {                       // `mov` — mark moved (double-move if already)
            if (ins->n_operands>=1) {
                int s = ins->operands[0]->id;
                if (s>=0 && s<L->nvar) {
                    if (report && st[s]) lin_add(L, s, ins->line, ins->col, 2);
                    st[s] = true;
                }
            }
            continue;
        }
        if (report)                                      // any other reference to a moved slot = use
            for (int k=0;k<ins->n_operands;k++) {
                int s = ins->operands[k]->id;
                if (s>=0 && s<L->nvar && st[s]) lin_add(L, s, ins->line, ins->col, 1);
            }
    }
}

static Lin *lin_analyze(IrFunc *f) {
    Lin *L = calloc(1,sizeof *L);
    L->f=f; L->nvar = f->next_value_id>0?f->next_value_id:1; L->nb = f->next_block_id;
    L->in = calloc(L->nb,sizeof(bool*));
    for (int i=0;i<L->nb;i++) L->in[i]=calloc(L->nvar,sizeof(bool));
    L->linsl = calloc(L->nvar,sizeof(bool));
    // A leak-relevant slot owns a RESOURCE that must be freed: an owned pointer or slice.
    // (An owned struct that is merely move-tracked but trivially droppable is NOT a leak;
    // a struct that transitively owns a resource needs per-field tracking — deferred.)
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next)
            if (ins->op==IR_ALLOCA && ins->result && ins->aux.alloca_ty && ins->aux.alloca_ty->linear
                && (ins->aux.alloca_ty->kind==IRT_PTR || ins->aux.alloca_ty->kind==IRT_SLICE))
                L->linsl[ins->result->id] = true;
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
static void lin_free(Lin *L){ if(!L)return; for(int i=0;i<L->nb;i++) free(L->in[i]); free(L->in); free(L->linsl); free(L->finds); free(L); }

#endif // LAIN_LINEARITY_H
