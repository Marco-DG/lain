// src/analysis/definite_init.h — DEFINITE ASSIGNMENT (Stage III item 3.0).
//
// The analysis the plan had MISSED: the old linearity.h quietly implements it alongside
// ownership (E005 use-of-uninitialized, E019 partial-init), so "port linearity" silently
// meant porting two analyses. The C1 gate exposed it as 15 tracked divergences.
//
// Forward MUST-init dataflow over the CFG, on the shared place lattice (ir/place.h):
//   • a local alloca starts UNINITIALISED; parameters are initialised by definition
//   • a STORE to a place initialises it (whole, or one field)
//   • a struct whose every field is initialised becomes wholly initialised
//   • a READ of a place that is not initialised on EVERY path is an error
// Merge at a join is INTERSECTION — initialised only if initialised on all incoming paths,
// which is the sound direction (a value initialised on just one branch is not safe to read).
//
//   E005  read of an uninitialised place
//   E019  read of a partially-initialised aggregate (some fields written, not all)
//
// Sovereign: reads only ir.h + place.h. No AST.
#ifndef LAIN_DEFINITE_INIT_H
#define LAIN_DEFINITE_INIT_H

#include "../ir/ir.h"
#include "../ir/place.h"
#include <stdlib.h>
#include <string.h>

#define DI_WHOLE_BIT 63u

typedef struct { int base; isize line, col; int code; } DiFinding;   // 5 = E005, 19 = E019

typedef struct {
    IrFunc    *f;
    int        nvar, nb;
    IrInstr  **def;
    uint64_t **in;        // in[block][base] = init mask (bit63 = whole, bits0..62 = fields)
    bool      *tracked;   // tracked[v] = v is a local alloca we track
    IrType   **alloca_ty; // element type per tracked base (for the "all fields" rule)
    DiFinding *finds; int nfinds, cap;
} Di;

static void di_add(Di *D, int base, isize line, isize col, int code) {
    if (D->nfinds==D->cap){ D->cap=D->cap?D->cap*2:8; D->finds=realloc(D->finds,D->cap*sizeof*D->finds); }
    D->finds[D->nfinds++] = (DiFinding){base,line,col,code};
}
static bool di_is_whole(uint64_t m){ return (m >> DI_WHOLE_BIT) & 1u; }

// mark a place initialised in `st`; promote to whole when every field is covered
static void di_mark_init(Di *D, uint64_t *st, const IrPlace *p) {
    if (!p->valid || p->base_kind!=IRPB_LOCAL) return;
    int b = p->base_id; if (b<0 || b>=D->nvar || !D->tracked[b]) return;
    if (p->nproj == 0) { st[b] |= (1ull<<DI_WHOLE_BIT); return; }          // whole store
    if (p->proj[0].kind == IRPJ_FIELD) {
        int fi = p->proj[0].field;
        if (fi>=0 && fi<63) st[b] |= (1ull<<fi);
        IrType *t = D->alloca_ty[b];
        if (t && t->kind==IRT_STRUCT && t->n_fields>0 && t->n_fields<63) {
            uint64_t all = (t->n_fields==63)?~0ull:((1ull<<t->n_fields)-1u);
            if ((st[b] & all) == all) st[b] |= (1ull<<DI_WHOLE_BIT);        // every field written
        }
        return;
    }
    // an INDEXED store initialises an unknown element — conservatively treat the aggregate
    // as initialised (we do not track per-element state; flagging would false-positive).
    st[b] |= (1ull<<DI_WHOLE_BIT);
}

// is the place readable (initialised) under `st`?  returns 0 = ok, 5 = E005, 19 = E019
static int di_check_read(Di *D, const uint64_t *st, const IrPlace *p) {
    if (!p->valid || p->base_kind!=IRPB_LOCAL) return 0;                    // params/derefs: fine
    int b = p->base_id; if (b<0 || b>=D->nvar || !D->tracked[b]) return 0;
    uint64_t m = st[b];
    if (di_is_whole(m)) return 0;
    if (p->nproj>0 && p->proj[0].kind==IRPJ_FIELD) {
        int fi = p->proj[0].field;
        if (fi>=0 && fi<63 && (m & (1ull<<fi))) return 0;                   // that field is set
        return (m != 0) ? 19 : 5;                                           // partial vs none
    }
    return (m != 0) ? 19 : 5;   // reading the whole aggregate: partial ⇒ E019, else E005
}

static void di_run_block(Di *D, IrBlock *b, uint64_t *st, bool report) {
    for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
        if (ins->op==IR_STORE && ins->n_operands>=1) {
            IrPlace t = ir_place_of(D->def, D->nvar, ins->operands[0]);
            di_mark_init(D, st, &t);
            continue;
        }
        // A READ of memory is exactly an IR_LOAD. Every other instruction consumes SSA
        // VALUES, not places — and crucially, taking an ADDRESS (field_ptr / elem_ptr /
        // passing `var x`) is NOT a read: `a[i] = v` feeds the alloca to elem_ptr as an
        // address, and treating that as a read flagged every array write as uninitialised.
        if (report && ins->op==IR_LOAD && ins->n_operands>=1 && ins->operands[0]) {
            IrPlace q = ir_place_of(D->def, D->nvar, ins->operands[0]);
            int c = di_check_read(D, st, &q);
            if (c) di_add(D, q.base_id, ins->line, ins->col, c);
        }
    }
}

static Di *di_analyze(IrFunc *f) {
    Di *D = calloc(1,sizeof *D);
    D->f=f; D->nvar = f->next_value_id>0?f->next_value_id:1; D->nb = f->next_block_id;
    D->def = calloc(D->nvar,sizeof(IrInstr*));
    D->tracked = calloc(D->nvar,sizeof(bool));
    D->alloca_ty = calloc(D->nvar,sizeof(IrType*));
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next) {
            if (i->result) D->def[i->result->id]=i;
            // Track SCALARS and STRUCTS only. An ARRAY is typically filled by a loop or a
            // comprehension, and a MUST-analysis cannot prove the loop body runs — tracking
            // them reported every such array as uninitialised (8 false positives on the
            // corpus). Per-element init needs the numeric domain to prove the loop covers
            // 0..len; until then, not tracking is the sound-and-quiet choice (we lose
            // array-element uninit detection — adjudicated as a known gap).
            if (i->op==IR_ALLOCA && i->result && i->aux.alloca_ty) {
                IrTypeKind k = i->aux.alloca_ty->kind;
                bool trackable = (k==IRT_INT || k==IRT_BOOL || k==IRT_FLOAT
                                  || k==IRT_PTR || k==IRT_STRUCT);   // NOT array/slice/VLA
                if (trackable) { D->tracked[i->result->id]=true; D->alloca_ty[i->result->id]=i->aux.alloca_ty; }
            }
        }
    D->in = calloc(D->nb,sizeof(uint64_t*));
    for (int i=0;i<D->nb;i++) D->in[i]=calloc(D->nvar,sizeof(uint64_t));
    bool *seen = calloc(D->nb,sizeof(bool));
    uint64_t *cur = malloc(D->nvar*sizeof(uint64_t));

    // MUST fixpoint: in[succ] = INTERSECTION over preds of out[pred]
    bool changed=true; int sweeps=0;
    seen[f->entry->id]=true;
    while (changed && sweeps++ < 1000) {
        changed=false;
        for (IrBlock *b=f->blocks;b;b=b->next) {
            if (!seen[b->id]) continue;
            memcpy(cur, D->in[b->id], D->nvar*sizeof(uint64_t));
            di_run_block(D,b,cur,false);
            IrBlock *succ[3]={0,0,0}; int ns=0;
            switch (b->term.kind) {
                case IR_TERM_BR:      succ[ns++]=b->term.a; break;
                case IR_TERM_BR_COND: succ[ns++]=b->term.a; succ[ns++]=b->term.b; break;
                case IR_TERM_SWITCH:  succ[ns++]=b->term.a;
                    for (IrSwitchCase *c=b->term.cases;c;c=c->next) if(ns<3) succ[ns++]=c->target; break;
                default: break;
            }
            for (int k=0;k<ns;k++){ IrBlock *s=succ[k]; if(!s) continue;
                if (!seen[s->id]) { memcpy(D->in[s->id],cur,D->nvar*sizeof(uint64_t)); seen[s->id]=true; changed=true; continue; }
                for (int v=0;v<D->nvar;v++) {
                    uint64_t merged = D->in[s->id][v] & cur[v];       // INTERSECTION (must-init)
                    if (merged != D->in[s->id][v]) { D->in[s->id][v]=merged; changed=true; }
                }
            }
        }
    }
    for (IrBlock *b=f->blocks;b;b=b->next) {                                  // reporting sweep
        if (!seen[b->id]) continue;
        memcpy(cur, D->in[b->id], D->nvar*sizeof(uint64_t));
        di_run_block(D,b,cur,true);
    }
    free(cur); free(seen);
    return D;
}
static void di_free(Di *D){ if(!D)return; for(int i=0;i<D->nb;i++) free(D->in[i]);
    free(D->in); free(D->def); free(D->tracked); free(D->alloca_ty); free(D->finds); free(D); }

#endif // LAIN_DEFINITE_INIT_H
