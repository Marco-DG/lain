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
#include <stdlib.h>

// One discharged (or not) proof obligation.
typedef enum { VRA_BOUNDS, VRA_OVERFLOW, VRA_DIVZERO, VRA_TERMINATION, VRA_PRECOND } VraCheckKind;
typedef struct {
    VraCheckKind kind;
    IrInstr *at;
    bool     ok;        // discharged: the access/op is provably safe (check-free)
    // bounds detail
    bool     lo_ok;     // proved idx ≥ 0
    bool     hi_ok;     // proved idx < len
    bool     has_len;   // a length was found at all
    int64_t  line, col;
} VraCheck;

typedef struct {
    IrFunc  *f;
    int      nvar;      // = next_value_id
    int      dsz;       // octagon storage per block = dim*dim
    int64_t **in;       // in[bid] : entry octagon storage (NULL = unreached)
    bool    *reached;
    IrInstr **def;      // def[val id] = producing instruction (NULL for params)
    int     *defblk;    // defblk[val id] = id of the block defining it (-1 = param)
    int64_t *cval; bool *cknown;   // constant values (from IR_CONST)
    int     *slicelen;  // slice value id → its canonical length var (−1 = none)
    bool    *subslice_gep;  // elem_ptr result feeding a make_slice (a subslice start,
                            // not an element access — checked by the make_slice instead)
    VraCheck *checks; int nchecks, cap_checks;
} Vra;

// ── small helpers ────────────────────────────────────────────────────────────
static int vra_var(IrValue *v) { return v ? v->id : -1; }
static bool vra_is_int(IrValue *v){ return v && v->type &&
        (v->type->kind==IRT_INT || v->type->kind==IRT_BOOL); }

// is value id `v` a slice-typed alloca cell?
static bool vra_is_slice_cell(Vra *V, int v) {
    IrInstr *d = (v>=0 && v<V->nvar) ? V->def[v] : NULL;
    return d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind==IRT_SLICE;
}
// pre-pass: def sites, constants, and canonical slice-length vars. A slice's length
// var is its make_slice length operand or its first slice_len read; it is propagated
// through a slice local's store/load so `s = a[lo..hi]; s[k]` knows len(s).
static void vra_prepass(Vra *V) {
    for (int i=0;i<V->nvar;i++){ V->def[i]=NULL; V->defblk[i]=-1; V->cknown[i]=false; V->slicelen[i]=-1; V->subslice_gep[i]=false; }
    // first: def sites + constants (needed to classify slice cells below)
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->result){ V->def[ins->result->id]=ins; V->defblk[ins->result->id]=b->id; }
            if (ins->op==IR_CONST && ins->result){ V->cknown[ins->result->id]=true; V->cval[ins->result->id]=ins->aux.imm; }
        }
    int *cell_len = malloc(V->nvar*sizeof(int));
    for (int i=0;i<V->nvar;i++) cell_len[i]=-1;
    for (IrBlock *b=V->f->blocks; b; b=b->next)
        for (IrInstr *ins=b->instrs; ins; ins=ins->next) {
            if (ins->op==IR_MAKE_SLICE && ins->result && ins->n_operands>=2) {
                V->slicelen[ins->result->id] = ins->operands[1]->id;      // {data,len}: len is the length var
                IrInstr *dd = V->def[ins->operands[0]->id];               // subslice start (vs array→slice decay)
                if (dd && dd->op==IR_ELEM_PTR) V->subslice_gep[ins->operands[0]->id] = true;
            }
            else if (ins->op==IR_SLICE_LEN && ins->result && ins->n_operands>=1) {
                int s=ins->operands[0]->id;
                if (V->slicelen[s]<0) V->slicelen[s]=ins->result->id;     // first len read is canonical
            }
            else if (ins->op==IR_STORE && ins->n_operands>=2) {
                int cell=ins->operands[0]->id, v=ins->operands[1]->id;
                if (vra_is_slice_cell(V,cell) && V->slicelen[v]>=0) cell_len[cell]=V->slicelen[v];
            }
            else if (ins->op==IR_LOAD && ins->result && ins->n_operands>=1) {
                int cell=ins->operands[0]->id;
                if (vra_is_slice_cell(V,cell) && cell_len[cell]>=0) V->slicelen[ins->result->id]=cell_len[cell];
            }
        }
    free(cell_len);
}

// copy `src == dst` (equal values) into octagon o
// c*x, but only if it stays within the octagon's usable range (oct_add_* doubles the
// bound internally, so keep well inside OCT_INF). Returns false ⇒ leave that side unbounded.
static bool vra_safe_scale(int64_t c, int64_t x, int64_t *out) {
    __int128 p = (__int128)c * (__int128)x;
    if (p > (__int128)(OCT_INF/2) || p < -(__int128)(OCT_INF/2)) return false;
    *out = (int64_t)p; return true;
}
static void vra_assign_copy(Octagon *o, int dst, int src) {
    oct_close(o);                      // materialize src's transitive bounds BEFORE the copy
    oct_forget(o, dst);                // (so dst inherits them; this is where a loop invariant
    oct_add_diff_le(o, dst, src, 0);   //  is carried through a memory-cell load). Copies are
    oct_add_diff_le(o, src, dst, 0);   //  infrequent (loads/casts/lengths), so O(dim^3) here
}                                      //  is fine — unlike per-instruction forget-closes.

static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir);  // fwd

// ── transfer of one instruction over working octagon W ───────────────────────
static void vra_transfer_instr(Vra *V, Octagon *W, IrInstr *ins) {
    int r = ins->result ? ins->result->id : -1;
    switch (ins->op) {
        case IR_CONST:
            if (r>=0){ oct_forget(W,r); oct_add_const(W,r,ins->aux.imm); }
            break;
        case IR_LOAD: {
            if (r<0) break;
            IrInstr *d = ins->n_operands? V->def[ins->operands[0]->id] : NULL;
            if (d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind!=IRT_ARRAY)
                vra_assign_copy(W, r, ins->operands[0]->id);   // scalar cell → value
            else oct_forget(W, r);                             // array elem / unknown
            break;
        }
        case IR_STORE: {
            if (ins->n_operands<2) break;
            IrInstr *d = V->def[ins->operands[0]->id];
            if (d && d->op==IR_ALLOCA && d->aux.alloca_ty && d->aux.alloca_ty->kind!=IRT_ARRAY)
                vra_assign_copy(W, ins->operands[0]->id, ins->operands[1]->id);  // value → cell
            break;
        }
        case IR_ADD: case IR_SUB: {
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            bool ac=V->cknown[a], bc=V->cknown[b], isadd=(ins->op==IR_ADD);
            oct_forget(W, r);
            if (isadd && bc)      { oct_add_diff_le(W,r,a,V->cval[b]); oct_add_diff_le(W,a,r,-V->cval[b]); }   // r=a+c (exact)
            else if (isadd && ac) { oct_add_diff_le(W,r,b,V->cval[a]); oct_add_diff_le(W,b,r,-V->cval[a]); }
            else if (!isadd && bc){ oct_add_diff_le(W,r,a,-V->cval[b]); oct_add_diff_le(W,a,r,V->cval[b]); }   // r=a-c (exact)
            else if (!isadd && ac){ int64_t c=V->cval[a]; oct_add_sum_le(W,r,b,c); oct_add_negsum_le(W,r,b,-c); } // r=c-b ⇒ r+b=c
            else {
                // two-variable: sound difference bounds from the second operand's interval
                //   r=a+b, b∈[blo,bhi] ⇒ a+blo ≤ r ≤ a+bhi ;  r=a-b ⇒ a-bhi ≤ r ≤ a-blo
                if (!isadd) oct_close(W);   // materialize the a↔b relation before reading it
                int64_t blo,bhi; bool hl,hh; oct_interval(W,b,&blo,&hl,&bhi,&hh);
                if (isadd) { if (hh) oct_add_diff_le(W,r,a,bhi); if (hl) oct_add_diff_le(W,a,r,-blo); }
                else {
                    if (hl) oct_add_diff_le(W,r,a,-blo); if (hh) oct_add_diff_le(W,a,r,bhi);
                    // RELATIONAL r = a − b: transfer the octagon's OWN a↔b difference to r
                    // exactly (intervals miss it when the bound is symbolic). This proves the
                    // reverse index `a[L−i−1]`: from `i ≤ L` the octagon already holds, r=L−i
                    // gets r ≥ 0 (no underflow) and r < L.  a−b ≤ ub ⇒ r ≤ ub ; b−a ≤ lbe ⇒ r ≥ −lbe.
                    int64_t ub  = oct_get(W, oct_pos(b), oct_pos(a));   // bound on a − b
                    int64_t lbe = oct_get(W, oct_pos(a), oct_pos(b));   // bound on b − a
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
            if (ac && bc) { int64_t v; if (vra_safe_scale(V->cval[a],V->cval[b],&v)) oct_add_const(W,r,v); break; }
            int xv=-1; int64_t c=0;
            if (bc) { c=V->cval[b]; xv=a; } else if (ac) { c=V->cval[a]; xv=b; }
            if (xv>=0) {
                int64_t xlo,xhi,v; bool hl,hh; oct_interval(W, xv, &xlo,&hl,&xhi,&hh);
                if (c==0) oct_add_const(W,r,0);
                else if (c>0) { if (hl && vra_safe_scale(c,xlo,&v)) oct_add_lb(W,r,v);   // monotone
                                if (hh && vra_safe_scale(c,xhi,&v)) oct_add_ub(W,r,v); }
                else          { if (hh && vra_safe_scale(c,xhi,&v)) oct_add_lb(W,r,v);   // c<0: flip
                                if (hl && vra_safe_scale(c,xlo,&v)) oct_add_ub(W,r,v); }
            }
            break;
        }
        case IR_AND: {   // x & c  with c ≥ 0 constant  ⇒  0 ≤ r ≤ c   (mask idiom c=N−1)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_forget(W, r);
            if      (V->cknown[b] && V->cval[b]>=0){ oct_add_lb(W,r,0); oct_add_ub(W,r,V->cval[b]); }
            else if (V->cknown[a] && V->cval[a]>=0){ oct_add_lb(W,r,0); oct_add_ub(W,r,V->cval[a]); }
            break;
        }
        case IR_UDIV: {  // x / b  — in any defined exec (b > 0, x ≥ 0)  ⇒  0 ≤ r ≤ x
            if (r<0) break;
            int a=ins->operands[0]->id; oct_forget(W, r);
            oct_add_lb(W,r,0); oct_add_diff_le(W,r,a,0);   // r ≥ 0, r ≤ x
            break;
        }
        case IR_UREM: {  // x % b  (unsigned)  ⇒  0 ≤ r < b   (b > 0 in any defined exec;
            if (r<0) break;                                 // b = 0 is a separate div-by-zero)
            int b=ins->operands[1]->id; oct_forget(W, r);
            oct_add_lb(W, r, 0);
            if (V->cknown[b] && V->cval[b]>0) oct_add_ub(W, r, V->cval[b]-1);  // absolute ≤ c−1
            else oct_add_diff_le(W, r, b, -1);                                 // relative r < b
            break;
        }
        case IR_SREM: {  // signed a % c  ⇒  −(c−1) ≤ r ≤ c−1 (tighter to [0,c−1] if a≥0)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id; oct_forget(W, r);
            if (V->cknown[b] && V->cval[b]>0){
                int64_t c=V->cval[b];
                int64_t alo,ahi; bool hl,hh; oct_interval(W,a,&alo,&hl,&ahi,&hh);
                oct_add_lb(W,r, (hl&&alo>=0)?0:-(c-1)); oct_add_ub(W,r,c-1);
            }
            break;
        }
        case IR_LSHR: {  // x >> k  (logical) of a non-negative x is in [0, x]
            if (r<0) break;
            int a=ins->operands[0]->id; oct_forget(W,r);
            int64_t alo,ahi; bool hl,hh; oct_interval(W,a,&alo,&hl,&ahi,&hh);
            if (hl&&alo>=0){ oct_add_lb(W,r,0); if(hh) oct_add_ub(W,r,ahi); }  // 0 ≤ r ≤ a
            break;
        }
        case IR_SLICE_LEN: {
            if (r<0) break;
            oct_forget(W, r); oct_add_lb(W, r, 0);                 // a length is ≥ 0
            int s = ins->operands[0]->id, canon = V->slicelen[s];
            if (canon>=0 && canon!=r) vra_assign_copy(W, r, canon); // all len reads agree
            break;
        }
        case IR_CAST:
            if (r>=0){ // treat as a copy (widenings preserve value; a narrowing that
                       // changes it would be a separate proven-safe obligation)
                if (vra_is_int(ins->result) && ins->n_operands) vra_assign_copy(W, r, ins->operands[0]->id);
                else oct_forget(W, r);
            }
            break;
        case IR_ASSUME:   // the asserted fact holds from here — refine the octagon
            if (ins->n_operands>=1) vra_refine_guard(V, W, ins->operands[0], true);
            break;
        default:
            if (r>=0) oct_forget(W, r);   // conservative: result becomes unknown
            break;
    }
}

// refine W along a branch edge from `br` taken in direction `then_dir`
static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir) {
    if (!cond) return;
    IrInstr *ic = V->def[cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return;
    int a=ic->operands[0]->id, b=ic->operands[1]->id;
    IrCmp p = ic->aux.cmp;
    // normalize: on the else edge, the predicate negates
    // LT: a<b  LE: a≤b  GT: a>b  GE: a≥b  EQ  NE
    bool lt=(p==IR_CMP_SLT||p==IR_CMP_ULT), le=(p==IR_CMP_SLE||p==IR_CMP_ULE);
    bool gt=(p==IR_CMP_SGT||p==IR_CMP_UGT), ge=(p==IR_CMP_SGE||p==IR_CMP_UGE);
    bool eq=(p==IR_CMP_EQ), ne=(p==IR_CMP_NE);
    if (!then_dir) { // negate
        bool nl=ge, nle=gt, ng=le, nge=lt, neq=ne, nne=eq;
        lt=nl; le=nle; gt=ng; ge=nge; eq=neq; ne=nne;
    }
    if (lt) oct_add_diff_le(W,a,b,-1);        // a − b ≤ −1
    else if (le) oct_add_diff_le(W,a,b,0);    // a − b ≤ 0
    else if (gt) oct_add_diff_le(W,b,a,-1);   // b − a ≤ −1
    else if (ge) oct_add_diff_le(W,b,a,0);    // b − a ≤ 0
    else if (eq){ oct_add_diff_le(W,a,b,0); oct_add_diff_le(W,b,a,0); }
    (void)ne;                                 // a≠b is not an octagon constraint
}

// ── bounds consumer: discharge 0 ≤ idx < len at an IR_ELEM_PTR ───────────────
static void vra_add_check(Vra *V, VraCheck c) {
    if (V->nchecks==V->cap_checks){ V->cap_checks=V->cap_checks?V->cap_checks*2:8;
        V->checks=realloc(V->checks, V->cap_checks*sizeof(VraCheck)); }
    V->checks[V->nchecks++]=c;
}
static void vra_check_elem(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->n_operands<2) return;
    if (ins->result && V->subslice_gep[ins->result->id]) return;  // a subslice start — the make_slice checks it
    int idx = ins->operands[1]->id;
    IrValue *base = ins->operands[0];
    IrInstr *bd = V->def[base->id];
    int64_t clen=-1; int lenvar=-1;
    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty && bd->aux.alloca_ty->kind==IRT_ARRAY)
        clen = bd->aux.alloca_ty->array_len;                          // local fixed array
    else if (base->type && base->type->kind==IRT_ARRAY)
        clen = base->type->array_len;                                // fixed-array value (e.g. a param)
    else if (bd && bd->op==IR_SLICE_DATA && bd->n_operands>=1) {
        int s = bd->operands[0]->id; if (V->slicelen[s]>=0) lenvar=V->slicelen[s];
    }
    int64_t lo,hi; bool hl,hh;
    if (V->cknown[idx]) { lo=hi=V->cval[idx]; hl=hh=true; }   // constant index — no octagon needed
    else oct_interval(W, idx, &lo,&hl,&hi,&hh);
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_BOUNDS; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.lo_ok = hl && lo>=0;
    c.has_len = (clen>=0 || lenvar>=0);
    if (clen>=0)        c.hi_ok = hh && hi <= clen-1;
    else if (lenvar>=0) c.hi_ok = oct_get(W, oct_pos(lenvar), oct_pos(idx)) <= -1;  // idx − len ≤ −1
    else                c.hi_ok = false;
    c.ok = c.lo_ok && c.hi_ok;
    vra_add_check(V, c);
}

// The ℤ range of a value = its type interval, tightened by the octagon.
static void vra_range(Vra *V, Octagon *W, IrValue *v, int64_t *lo, int64_t *hi) {
    int64_t tlo=INT64_MIN, thi=INT64_MAX; (void)V;
    irtype_int_range(v->type, &tlo, &thi);
    int64_t olo,ohi; bool hl,hh; oct_interval(W, v->id, &olo,&hl,&ohi,&hh);
    if (hl && olo>tlo) tlo=olo;
    if (hh && ohi<thi) thi=ohi;
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
// Overflow obligation: a CHECK-mode +,−,× on two same-typed integers must land
// back inside that type. The octagon reasons in ℤ; here we compare the ℤ result
// range against the operand type's interval (design §2.6).
static void vra_check_overflow(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->wrap != IR_WRAP_CHECK) return;                 // .wrap/.sat skip the obligation
    if (ins->n_operands<2) return;
    IrValue *a=ins->operands[0], *b=ins->operands[1];
    int64_t tlo,thi;
    if (!irtype_int_range(a->type, &tlo, &thi)) return;     // target = the operand type
    int64_t alo,ahi,blo,bhi; vra_range(V,W,a,&alo,&ahi); vra_range(V,W,b,&blo,&bhi);
    __int128 rlo,rhi; vra_arith_range(ins->op, alo,ahi, blo,bhi, &rlo,&rhi);
    // for SUB, refine with the octagon's OWN a−b relation (W is closed here) — this proves
    // `L − i ≥ 0` (no underflow) from `i ≤ L`, which operand intervals miss when symbolic.
    if (ins->op==IR_SUB) {
        int64_t abu = oct_get(W, oct_pos(b->id), oct_pos(a->id));   // a − b ≤ abu
        int64_t bau = oct_get(W, oct_pos(a->id), oct_pos(b->id));   // b − a ≤ bau ⇒ a − b ≥ −bau
        if (abu < OCT_INF && (__int128)abu < rhi) rhi = abu;
        if (bau < OCT_INF && -(__int128)bau > rlo) rlo = -(__int128)bau;
    }
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_OVERFLOW; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = (rlo >= (__int128)tlo) && (rhi <= (__int128)thi);
    vra_add_check(V, c);
}
// Division/remainder: the divisor must be provably non-zero.
static void vra_check_divzero(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->n_operands<2) return;
    int64_t lo,hi; vra_range(V,W,ins->operands[1],&lo,&hi);
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_DIVZERO; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = (lo>0) || (hi<0);                                // 0 ∉ [lo,hi]
    vra_add_check(V, c);
}

// A subslice `src[lo..hi]` lowered to make_slice(elem_ptr(src_data,lo), hi−lo) is in
// bounds iff 0 ≤ lo AND hi ≤ len(src). (The start elem_ptr is NOT an element access,
// so it is skipped in vra_check_elem; the real obligation is checked here.)
static void vra_check_subslice(Vra *V, Octagon *W, IrInstr *ms) {
    if (ms->n_operands<2) return;
    IrInstr *ndd = V->def[ms->operands[0]->id];
    if (!ndd || ndd->op!=IR_ELEM_PTR || ndd->n_operands<2) return;    // array→slice decay, not a subslice
    int lo = ndd->operands[1]->id;
    IrInstr *bd = V->def[ndd->operands[0]->id];
    int64_t clen=-1; int lenvar=-1;
    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty && bd->aux.alloca_ty->kind==IRT_ARRAY) clen=bd->aux.alloca_ty->array_len;
    else if (bd && bd->op==IR_SLICE_DATA && bd->n_operands>=1){ int s=bd->operands[0]->id; if(V->slicelen[s]>=0) lenvar=V->slicelen[s]; }
    int hi=-1;
    IrInstr *lend = V->def[ms->operands[1]->id];
    if (lend && lend->op==IR_SUB && lend->n_operands>=2 && lend->operands[1]->id==lo) hi=lend->operands[0]->id; // len = hi − lo
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_BOUNDS; c.at=ms; c.line=ms->line; c.col=ms->col;
    int64_t llo,lhi; bool lhl,lhh; oct_interval(W,lo,&llo,&lhl,&lhi,&lhh);
    c.lo_ok = lhl && llo>=0;                                          // 0 ≤ lo
    c.has_len = (clen>=0 || lenvar>=0);
    if (hi<0) c.hi_ok=false;                                          // couldn't recover hi ⇒ conservative
    else if (clen>=0){ int64_t hl,hh_; bool a,bb; oct_interval(W,hi,&hl,&a,&hh_,&bb); c.hi_ok = bb && hh_<=clen; } // hi ≤ N
    else if (lenvar>=0) c.hi_ok = oct_get(W, oct_pos(lenvar), oct_pos(hi)) <= 0;   // hi − len ≤ 0
    else c.hi_ok=false;
    c.ok = c.lo_ok && c.hi_ok;
    vra_add_check(V, c);
}

// Does `a cmp b` hold in the (closed) octagon? The discharge dual of refine.
static bool vra_icmp_holds(Octagon *W, int a, int b, IrCmp cmp) {
    int64_t ab = oct_get(W, oct_pos(b), oct_pos(a));   // bound on a − b
    int64_t ba = oct_get(W, oct_pos(a), oct_pos(b));   // bound on b − a
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
static void vra_check_assert(Vra *V, Octagon *W, IrInstr *ins) {
    if (ins->n_operands<1) return;
    IrInstr *ic = V->def[ins->operands[0]->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return;
    VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_PRECOND; c.at=ins; c.line=ins->line; c.col=ins->col;
    c.ok = vra_icmp_holds(W, ic->operands[0]->id, ic->operands[1]->id, ic->aux.cmp);
    vra_add_check(V, c);
}

// ── termination consumer (a func must have only terminating loops) ───────────
static bool vra_is_scalar_cell(Vra *V, int v) {
    IrInstr *d=(v>=0&&v<V->nvar)?V->def[v]:NULL;
    return d && d->op==IR_ALLOCA && d->aux.alloca_ty &&
           d->aux.alloca_ty->kind!=IRT_ARRAY && d->aux.alloca_ty->kind!=IRT_SLICE;
}
// A guard bound is loop-invariant if it is a parameter, a constant, a slice length,
// or defined in a block strictly above the loop header (structured CFG order).
static bool vra_loop_invariant(Vra *V, IrValue *val, int Hid) {
    IrInstr *d=V->def[val->id];
    if (!d) return true;
    if (d->op==IR_CONST || d->op==IR_SLICE_LEN) return true;
    return V->defblk[val->id]>=0 && V->defblk[val->id] < Hid;
}
// A structured while-loop terminates if its guard variable is a memory cell updated
// by EXACTLY ONE well-formed step `cell = load(cell) ± c` whose direction drains the
// loop-invariant bound (rise toward an upper bound / fall toward a lower one). The
// "exactly one store" rule is conservative: any other write to the cell ⇒ not proven.
static bool vra_loop_terminates(Vra *V, IrBlock *H) {
    if (H->term.kind != IR_TERM_BR_COND) return false;
    IrInstr *ic = V->def[H->term.cond->id];
    if (!ic || ic->op!=IR_ICMP || ic->n_operands<2) return false;
    IrCmp p = ic->aux.cmp;
    for (int side=0; side<2; side++) {
        IrValue *ivv=ic->operands[side], *bnd=ic->operands[side^1];
        IrInstr *ivd=V->def[ivv->id];
        if (!ivd || ivd->op!=IR_LOAD || ivd->n_operands<1) continue;
        int cell=ivd->operands[0]->id;
        if (!vra_is_scalar_cell(V,cell)) continue;
        if (!vra_loop_invariant(V,bnd,H->id)) continue;
        int64_t step=0; int nstore=0, nupd=0;
        for (IrBlock *b=V->f->blocks; b; b=b->next) {
            if (b->id < H->id) continue;                       // above the loop region
            for (IrInstr *s=b->instrs; s; s=s->next) {
                if (s->op!=IR_STORE || s->n_operands<2 || s->operands[0]->id!=cell) continue;
                nstore++;
                IrInstr *vd=V->def[s->operands[1]->id];
                if (vd && (vd->op==IR_ADD||vd->op==IR_SUB) && vd->n_operands>=2) {
                    IrInstr *ld=V->def[vd->operands[0]->id]; int c=vd->operands[1]->id;
                    if (ld && ld->op==IR_LOAD && ld->operands[0]->id==cell && V->cknown[c]) {
                        step = (vd->op==IR_ADD)? V->cval[c] : -V->cval[c]; nupd++;
                    }
                }
            }
        }
        if (nstore!=1 || nupd!=1) continue;
        bool lt=(p==IR_CMP_SLT||p==IR_CMP_ULT||p==IR_CMP_SLE||p==IR_CMP_ULE);
        bool gt=(p==IR_CMP_SGT||p==IR_CMP_UGT||p==IR_CMP_SGE||p==IR_CMP_UGE);
        if (lt && step>0) return true;
        if (gt && step<0) return true;
    }
    return false;
}

// ── the fixpoint over the CFG ────────────────────────────────────────────────
static Vra *vra_analyze(IrFunc *f) {
    Vra *V = calloc(1, sizeof *V);
    V->f=f; V->nvar = f->next_value_id>0 ? f->next_value_id : 1;
    int dim=2*V->nvar; V->dsz=dim*dim;
    int nb=f->next_block_id;
    V->in=calloc(nb,sizeof(int64_t*)); V->reached=calloc(nb,sizeof(bool));
    V->def=calloc(V->nvar,sizeof(IrInstr*)); V->defblk=calloc(V->nvar,sizeof(int));
    V->cval=calloc(V->nvar,sizeof(int64_t)); V->cknown=calloc(V->nvar,sizeof(bool));
    V->slicelen=calloc(V->nvar,sizeof(int)); V->subslice_gep=calloc(V->nvar,sizeof(bool));
    vra_prepass(V);
    for (int i=0;i<nb;i++) V->in[i]=malloc(V->dsz*sizeof(int64_t));

    int64_t wb[1]; (void)wb;
    int64_t *W_m=malloc(V->dsz*8), *T_m=malloc(V->dsz*8), *J_m=malloc(V->dsz*8), *D_m=malloc(V->dsz*8);
    Octagon W={V->nvar,dim,W_m}, T={V->nvar,dim,T_m}, J={V->nvar,dim,J_m}, D={V->nvar,dim,D_m};

    { Octagon E={V->nvar,dim,V->in[f->entry->id]}; oct_init_top(&E,V->nvar,E.m);
      // seed each integer parameter's type interval (a usize is ≥ 0, etc.). Skip a
      // bound whose doubled DBM entry would overflow (e.g. u64's ~2^63 upper).
      for (IrParam *p=f->params; p; p=p->next) {   // only the type interval; refinements
          int64_t tlo,thi;                          // now arrive as entry IR_ASSUME nodes
          if (!irtype_int_range(p->value->type,&tlo,&thi)) continue;
          if (tlo > -OCT_INF/2) oct_add_lb(&E, p->value->id, tlo);
          if (thi <  OCT_INF/2) oct_add_ub(&E, p->value->id, thi);
      }
    }
    V->reached[f->entry->id]=true;

    // Per-loop-header set of cells MODIFIED inside the loop (for selective widening — an
    // outer induction var, invariant in an inner loop, must not be widened at the inner
    // header). Loop body ≈ block-id range [H, max back-edge-source id]; Lain's CFG is
    // structured, so a natural loop is a contiguous id range. A `store %c,_` modifies %c.
    char **loopmod = calloc(nb, sizeof(char*));
    for (IrBlock *H=f->blocks; H; H=H->next) {
        if (!H->is_loop_header) continue;
        int himax=H->id;
        for (IrEdge *e=H->preds; e; e=e->next) if (e->block->id > himax) himax=e->block->id;
        char *mod = calloc(V->nvar,1);
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (b->id < H->id || b->id > himax) continue;
            for (IrInstr *ins=b->instrs; ins; ins=ins->next)
                if (ins->op==IR_STORE && ins->n_operands>=1 && ins->operands[0]->id < V->nvar)
                    mod[ins->operands[0]->id]=1;
        }
        loopmod[H->id]=mod;
    }

    bool changed=true; int sweeps=0;
    while (changed && sweeps++ < 1000) {
        changed=false;
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!V->reached[b->id]) continue;
            memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->nvar; W.dim=dim;
            oct_close(&W);
            for (IrInstr *ins=b->instrs; ins; ins=ins->next) vra_transfer_instr(V,&W,ins);
            oct_close(&W);
            IrBlock *succ[2]={NULL,NULL}; int ns=0; bool guarded=false; IrValue *cond=NULL;
            if (b->term.kind==IR_TERM_BR){ succ[0]=b->term.a; ns=1; }
            else if (b->term.kind==IR_TERM_BR_COND){ succ[0]=b->term.a; succ[1]=b->term.b; ns=2; guarded=true; cond=b->term.cond; }
            for (int k=0;k<ns;k++) {
                IrBlock *s=succ[k]; if(!s) continue;
                memcpy(T_m, W_m, V->dsz*8); T.nvar=V->nvar; T.dim=dim;
                if (guarded){ vra_refine_guard(V,&T,cond,k==0); oct_close(&T); }
                if (oct_is_bottom(&T)) continue;
                if (!V->reached[s->id]) { memcpy(V->in[s->id],T_m,V->dsz*8); V->reached[s->id]=true; changed=true; continue; }
                Octagon In={V->nvar,dim,V->in[s->id]};
                oct_join(&J,&In,&T);
                if (s->is_loop_header){ oct_widen_sel(&D,&In,&J,loopmod[s->id]); memcpy(J_m,D_m,V->dsz*8); }
                if (!oct_leq(&J,&In)){ memcpy(V->in[s->id],J_m,V->dsz*8); changed=true; }
            }
        }
    }
    // final pass: discharge index obligations against the converged in-states
    for (IrBlock *b=f->blocks; b; b=b->next) {
        if (!V->reached[b->id]) continue;
        memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->nvar; W.dim=dim; oct_close(&W);
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
                case IR_ASSERT: oct_close(&W); vra_check_assert(V,&W,ins); break;
                case IR_ADD: case IR_SUB: case IR_MUL: oct_close(&W); vra_check_overflow(V,&W,ins); break;
                case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: oct_close(&W); vra_check_divzero(V,&W,ins); break;
                default: break;
            }
            vra_transfer_instr(V,&W,ins);
        }
    }
    // one termination obligation per loop header (a func must clear all of them)
    for (IrBlock *b=f->blocks; b; b=b->next) {
        if (!b->is_loop_header) continue;
        VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_TERMINATION; c.ok=vra_loop_terminates(V,b);
        vra_add_check(V, c);
    }
    // fail closed: if lowering was infaithful (a dropped/placeholder'd construct), no
    // proof over this IR is trustworthy — the dropped code could change a checked value.
    if (f->incomplete) for (int i=0;i<V->nchecks;i++) V->checks[i].ok=false;
    for (int i=0;i<nb;i++) if (loopmod[i]) free(loopmod[i]);
    free(loopmod);
    free(W_m); free(T_m); free(J_m); free(D_m);
    return V;
}
static void vra_free(Vra *V){
    if(!V) return;
    for(int i=0;i<V->f->next_block_id;i++) free(V->in[i]);
    free(V->in); free(V->reached); free(V->def); free(V->defblk); free(V->cval); free(V->cknown);
    free(V->slicelen); free(V->subslice_gep); free(V->checks); free(V);
}

#endif // LAIN_VRA_H
