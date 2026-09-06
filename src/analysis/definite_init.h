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
// Depth-2 paths. The state per tracked base is DI_W words: word 0 is the top mask (bit63 =
// the whole value, bits 0..62 = depth-1 fields fully initialised), and words 1..DI_SUBF are
// SUBMASKS for depth-1 fields 0..DI_SUBF-1. Without them a nested write was treated as
// initialising its whole parent — `p.a.y = 1` marked all of `p.a` set, so reading the still
// -uninitialised `p.a.x` reported nothing. That is a FAIL-OPEN on the one thing this pass
// exists to catch, so the depth the analysis models has to reach the depth programs write at.
#define DI_SUBF 8u
#define DI_W    (1u + DI_SUBF)

typedef struct { int base; isize line, col; int code; } DiFinding;   // 5 = E005, 19 = E019

typedef struct {
    IrFunc    *f;
    int        nvar, nb;
    IrInstr  **def;
    uint64_t **in;        // in[block][base*DI_W + w] — see DI_W above
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
// The CONSTANT value of an index, or -1. Per-element tracking only ever applies to indices
// the IR states as literals; anything computed stays in the conservative path.
static int64_t di_const_index(Di *D, const IrValue *ix) {
    if (!ix || ix->id < 0 || ix->id >= D->nvar) return -1;
    IrInstr *d = D->def[ix->id];
    if (!d || d->op != IR_CONST) return -1;
    return d->aux.imm;
}

// promote: a struct whose every field is initialised is wholly initialised
static void di_promote(Di *D, uint64_t *st, int b) {
    IrType *t = D->alloca_ty[b];
    if (t && t->kind==IRT_STRUCT && t->n_fields>0 && t->n_fields<63) {
        uint64_t all = (1ull<<t->n_fields)-1u;
        if ((st[b*DI_W] & all) == all) st[b*DI_W] |= (1ull<<DI_WHOLE_BIT);
    }
}
static void di_mark_init(Di *D, uint64_t *st, const IrPlace *p) {
    if (!p->valid || p->base_kind!=IRPB_LOCAL) return;
    int b = p->base_id; if (b<0 || b>=D->nvar || !D->tracked[b]) return;
    if (p->nproj == 0) { st[b*DI_W] |= (1ull<<DI_WHOLE_BIT); return; }      // whole store
    if (p->proj[0].kind == IRPJ_FIELD) {
        int fi = p->proj[0].field;
        if (fi<0 || fi>=63) { st[b*DI_W] |= (1ull<<DI_WHOLE_BIT); return; } // beyond the mask
        bool nested = (p->nproj >= 2 && p->proj[1].kind == IRPJ_FIELD);
        if (!nested) {                                                      // p.f = v
            st[b*DI_W] |= (1ull<<fi);
            if ((unsigned)fi < DI_SUBF) st[b*DI_W + 1 + fi] |= (1ull<<DI_WHOLE_BIT);
            di_promote(D, st, b);
            return;
        }
        // p.f.g = v — initialises ONLY the leaf. The parent field becomes initialised when
        // every one of ITS fields is, which is what separates this from the old behaviour.
        if ((unsigned)fi >= DI_SUBF) { st[b*DI_W] |= (1ull<<fi); di_promote(D,st,b); return; }
        int gi = p->proj[1].field;
        if (gi<0 || gi>=63) { st[b*DI_W] |= (1ull<<fi); di_promote(D,st,b); return; }
        uint64_t *sub = &st[b*DI_W + 1 + fi];
        *sub |= (1ull<<gi);
        IrType *t = D->alloca_ty[b];
        IrType *ft = (t && t->kind==IRT_STRUCT && fi<t->n_fields && t->fields) ? t->fields[fi] : NULL;
        if (ft && ft->kind==IRT_STRUCT && ft->n_fields>0 && ft->n_fields<63) {
            uint64_t all = (1ull<<ft->n_fields)-1u;
            if ((*sub & all) == all) { *sub |= (1ull<<DI_WHOLE_BIT); st[b*DI_W] |= (1ull<<fi); }
        } else { st[b*DI_W] |= (1ull<<fi); }      // parent's shape unknown ⇒ keep old behaviour
        di_promote(D, st, b);
        return;
    }
    if (p->proj[0].kind == IRPJ_INDEX) {
        // A CONSTANT index initialises exactly that element — `a[0] = 5` makes a[0] readable
        // without making a[1] readable. The old engine demands a WHOLE-array initialiser
        // before ANY element read, which rejects the perfectly safe `a[0] = 5; return a[0]`.
        int64_t k = di_const_index(D, p->proj[0].index);
        IrType *t = D->alloca_ty[b];
        if (k >= 0 && k < 63) {
            st[b*DI_W] |= (1ull<<k);
            if (t && t->kind==IRT_ARRAY && t->array_len>0 && t->array_len<63) {
                uint64_t all = (1ull<<t->array_len)-1u;
                if ((st[b*DI_W] & all) == all) st[b*DI_W] |= (1ull<<DI_WHOLE_BIT);
            }
            return;
        }
        // An UNKNOWN index could be any element, so we cannot say WHICH became initialised.
        // Treating it as initialising the whole aggregate is deliberately FAIL-OPEN: it is
        // what lets a fill loop (`while i<n { a[i]=… }`) be followed by a read without 8
        // false positives, and proving such a loop TOTAL needs the numeric domain to show it
        // covers 0..len — a genuine gap, recorded rather than papered over.
        st[b*DI_W] |= (1ull<<DI_WHOLE_BIT);
        return;
    }
    st[b*DI_W] |= (1ull<<DI_WHOLE_BIT);
}

// is the place readable (initialised) under `st`?  returns 0 = ok, 5 = E005, 19 = E019
static int di_check_read(Di *D, const uint64_t *st, const IrPlace *p) {
    if (!p->valid || p->base_kind!=IRPB_LOCAL) return 0;                    // params/derefs: fine
    int b = p->base_id; if (b<0 || b>=D->nvar || !D->tracked[b]) return 0;
    uint64_t m = st[b*DI_W];
    if (di_is_whole(m)) return 0;
    if (p->nproj>0 && p->proj[0].kind==IRPJ_INDEX) {
        int64_t k = di_const_index(D, p->proj[0].index);
        if (k >= 0 && k < 63) return (m & (1ull<<k)) ? 0 : ((m != 0) ? 19 : 5);
        return (m != 0) ? 0 : 5;      // unknown index: any initialisation makes it plausible
    }
    if (p->nproj>0 && p->proj[0].kind==IRPJ_FIELD) {
        int fi = p->proj[0].field;
        if (fi<0 || fi>=63) return (m != 0) ? 19 : 5;
        if (m & (1ull<<fi)) return 0;                                       // that field is set
        // p.f.g — the leaf may be set even though the parent field is not yet complete
        if (p->nproj>=2 && p->proj[1].kind==IRPJ_FIELD && (unsigned)fi<DI_SUBF) {
            uint64_t sub = st[b*DI_W + 1 + fi];
            int gi = p->proj[1].field;
            if (di_is_whole(sub)) return 0;
            if (gi>=0 && gi<63 && (sub & (1ull<<gi))) return 0;
            return 5;   // THIS leaf is unset — E005; E019 is for reading a partial aggregate
        }
        // reading the parent field WHOLE while only some of its leaves are set ⇒ partial
        if ((unsigned)fi<DI_SUBF && st[b*DI_W + 1 + fi] != 0) return 19;
        return (m != 0) ? 19 : 5;                                           // partial vs none
    }
    return (m != 0) ? 19 : 5;   // reading the whole aggregate: partial ⇒ E019, else E005
}

static void di_run_block(Di *D, IrBlock *b, uint64_t *st, bool report) {
    for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
        if (ins->op==IR_INIT && ins->n_operands>=1) {      // a declared whole-initialisation
            IrPlace t = ir_place_of(D->def, D->nvar, ins->operands[0]);
            if (t.valid && t.base_kind==IRPB_LOCAL && t.base_id>=0 && t.base_id<D->nvar
                && D->tracked[t.base_id]) st[t.base_id*DI_W] |= (1ull<<DI_WHOLE_BIT);
            continue;
        }
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
                // ARRAYS are now tracked per ELEMENT for constant indices (see di_mark_init):
                // that catches `var a i32[4]; return a[0]` — reading storage nothing ever
                // wrote — which whole-array-only tracking missed entirely. A slice/VLA still
                // is not tracked: it has no static extent to enumerate.
                bool trackable = (k==IRT_INT || k==IRT_BOOL || k==IRT_FLOAT
                                  || k==IRT_PTR || k==IRT_STRUCT
                                  || (k==IRT_ARRAY && i->aux.alloca_ty->array_len>0
                                      && i->aux.alloca_ty->array_len<63));
                if (trackable) { D->tracked[i->result->id]=true; D->alloca_ty[i->result->id]=i->aux.alloca_ty; }
            }
        }
    D->in = calloc(D->nb,sizeof(uint64_t*));
    for (int i=0;i<D->nb;i++) D->in[i]=calloc((size_t)D->nvar*DI_W,sizeof(uint64_t));
    bool *seen = calloc(D->nb,sizeof(bool));
    size_t DIN = (size_t)D->nvar*DI_W;
    uint64_t *cur = malloc(DIN*sizeof(uint64_t));

    // MUST fixpoint: in[succ] = INTERSECTION over preds of out[pred]
    bool changed=true; int sweeps=0;
    seen[f->entry->id]=true;
    while (changed && sweeps++ < 1000) {
        changed=false;
        for (IrBlock *b=f->blocks;b;b=b->next) {
            if (!seen[b->id]) continue;
            memcpy(cur, D->in[b->id], DIN*sizeof(uint64_t));
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
                if (!seen[s->id]) { memcpy(D->in[s->id],cur,DIN*sizeof(uint64_t)); seen[s->id]=true; changed=true; continue; }
                for (size_t v=0;v<DIN;v++) {
                    uint64_t merged = D->in[s->id][v] & cur[v];       // INTERSECTION (must-init)
                    if (merged != D->in[s->id][v]) { D->in[s->id][v]=merged; changed=true; }
                }
            }
        }
    }
    for (IrBlock *b=f->blocks;b;b=b->next) {                                  // reporting sweep
        if (!seen[b->id]) continue;
        memcpy(cur, D->in[b->id], DIN*sizeof(uint64_t));
        di_run_block(D,b,cur,true);
    }
    free(cur); free(seen);
    return D;
}
static void di_free(Di *D){ if(!D)return; for(int i=0;i<D->nb;i++) free(D->in[i]);
    free(D->in); free(D->def); free(D->tracked); free(D->alloca_ty); free(D->finds); free(D); }

#endif // LAIN_DEFINITE_INIT_H
