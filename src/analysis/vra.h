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
#include "ir/place.h"   // phase D: the index-disjointness seam we fill
#include <stdlib.h>

// One discharged (or not) proof obligation.
#define VRA_MAX_RANK 4

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
    // ESCAPED cells: an alloca whose ADDRESS leaves this instruction's control — passed to a
    // call, stored as a value, or returned. The octagon models a scalar alloca as a stable
    // memory cell, and nothing was invalidating that cell when someone else could write it:
    //     proc bump(var i usize) { i = i +% 100 }
    //     var i usize = 0 ;  bump(var i) ;  a[i]        // a is i32[4]
    // left the octagon believing i == 0, so `a[i]` was PROVEN check-free while the program
    // actually reads a[100]. A FALSE PROOF is a removed bounds check, so any call must havoc
    // every cell whose address could have reached it.
    bool    *escaped;
    // S2: rank-N region shapes, read off the IR_SHAPE instructions. Indexed by the BASE
    // value id; shape_rank[b] > 0 means b has extents shape_ext[b][0..rank-1].
    int     *shape_rank;
    int    (*shape_ext)[VRA_MAX_RANK];
    VraCheck *checks; int nchecks, cap_checks;
} Vra;

// ── small helpers ────────────────────────────────────────────────────────────
static int vra_var(IrValue *v) { return v ? v->id : -1; }
static bool vra_is_int(IrValue *v){ return v && v->type &&
        (v->type->kind==IRT_INT || v->type->kind==IRT_BOOL); }

// is value id `v` a slice-typed alloca cell?
// Follow an address back to the alloca it roots in and mark that cell escaped.
static void vra_mark_escape(Vra *V, IrValue *v) {
    for (int guard=0; v && v->id>=0 && v->id<V->nvar && guard<10000; guard++) {
        IrInstr *d = V->def[v->id];
        if (!d) return;                                   // a parameter: not our cell
        if (d->op == IR_ALLOCA) { V->escaped[v->id] = true; return; }
        switch (d->op) {
            case IR_ELEM_PTR: case IR_FIELD_PTR:
            case IR_SLICE_DATA: case IR_MAKE_SLICE:
                v = d->n_operands>=1 ? d->operands[0] : NULL; break;
            default: return;                              // not an address we can attribute
        }
    }
}

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
                for (int k=0;k<ins->n_operands;k++) vra_mark_escape(V, ins->operands[k]);
            } else if (ins->op == IR_STORE && ins->n_operands>=2) {
                vra_mark_escape(V, ins->operands[1]);          // the VALUE stored, not the target
            }
        }
        if (b->term.kind == IR_TERM_RET) vra_mark_escape(V, b->term.cond);
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
static void vra_assign_copy(Octagon *o, int dst, int src) {
    oct_close(o);                      // materialize src's transitive bounds BEFORE the copy
    oct_forget(o, dst);                // (so dst inherits them; this is where a loop invariant
    oct_add_diff_le(o, dst, src, 0);   //  is carried through a memory-cell load). Copies are
    oct_add_diff_le(o, src, dst, 0);   //  infrequent (loads/casts/lengths), so O(dim^3) here
}                                      //  is fine — unlike per-instruction forget-closes.

static void vra_refine_guard(Vra *V, Octagon *W, IrValue *cond, bool then_dir);  // fwd
static void vra_free(Vra *V);                                                    // fwd (phase D)

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
static IrFunc *vra_mod = NULL;   // module for callee lookup; NULL disables the query
static IrFunc *vra_find_func(const IrName *n) {
    if (!n || !vra_mod) return NULL;
    // by CONTENT — ir_intern allocates a fresh IrName per call despite its name
    for (IrFunc *g=vra_mod; g; g=g->next)
        if (g->name && g->name->length==n->length && memcmp(g->name->name,n->name,(size_t)n->length)==0)
            return g;
    return NULL;
}
static bool vra_ret_range(IrFunc *g, int64_t *lo, int64_t *hi);
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
static void vra_div_facts(Octagon *W, int r, int a, int64_t D, int64_t alo, bool hl, int64_t ahi, bool hh) {
    if (D < 1) return;
    oct_add_lb(W, r, 0);
    oct_add_diff_le(W, r, a, 0);                        // r ≤ x
    if (hl && alo >= 0) oct_add_lb(W, r, alo / D);
    if (hh && ahi >= 0) oct_add_ub(W, r, ahi / D);
    if (D >= 2) {
        if (hl && alo >= 1) oct_add_diff_le(W, r, a, -1);          // r ≤ x − 1
        if (hh && ahi >= 0) oct_add_diff_le(W, a, r, ahi - ahi/D); // x − r ≤ max(x − x/D)
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
static void vra_add_via_diff(Vra *V, Octagon *W, int r, int a, int q) {
    for (int d=0; d<V->nvar; d++) {
        IrInstr *dd = V->def[d];
        if (!dd || dd->op != IR_SUB || dd->n_operands < 2) continue;
        if (!dd->operands[0] || !dd->operands[1]) continue;
        if (dd->operands[1]->id != a) continue;         // d = B − a, the same a we are adding to
        int B = dd->operands[0]->id;
        if (B == r || B < 0 || B >= V->nvar) continue;
        int64_t c = oct_get(W, oct_pos(d), oct_pos(q)); // q − d ≤ c   ⇒   r − B ≤ c
        if (c < OCT_INF) oct_add_diff_le(W, r, B, c);
        int64_t c2 = oct_get(W, oct_pos(q), oct_pos(d)); // d − q ≤ c2  ⇒   B − r ≤ c2
        if (c2 < OCT_INF) oct_add_diff_le(W, B, r, c2);
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
                oct_close(W);   // materialize the a↔b (and q↔d) relations before reading them
                int64_t blo,bhi; bool hl,hh; oct_interval(W,b,&blo,&hl,&bhi,&hh);
                if (isadd) { if (hh) oct_add_diff_le(W,r,a,bhi); if (hl) oct_add_diff_le(W,a,r,-blo);
                             vra_add_via_diff(V,W,r,a,b);      // r = a + b where b is bounded vs (B − a)
                             vra_add_via_diff(V,W,r,b,a); }    // ...and symmetrically
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
                oct_interval(W, iv, &ilo,&ihl,&ihi,&ihh);
                oct_interval(W, stride, &slo,&shl,&shi,&shh);
                bool i_lt_e0 = oct_get(W, oct_pos(e0), oct_pos(iv)) <= -1;
                if (i_lt_e0 && ihl && ilo>=0 && shl && slo>=0) oct_add_diff_le(W, r, lenv, 0);
            }
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
        case IR_CTZ: case IR_CLZ: case IR_POPCOUNT: {
            // A bit intrinsic lands in [0, W] where W is the OPERAND's width — exactly the
            // fact that makes `a[@popcount(mask)]` provable without a runtime check. Modelled
            // as an op rather than an opaque call precisely so this range is free.
            if (r<0) break;
            oct_forget(W, r);
            const IrType *at = ins->operands[0] ? ins->operands[0]->type : NULL;
            int width = (at && at->kind==IRT_INT && at->bits>0 && at->bits<=64) ? at->bits : 32;
            oct_add_lb(W, r, 0); oct_add_ub(W, r, width);
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
        case IR_UDIV: {  // x / b  — in any defined exec (b > 0, x ≥ 0)
            if (r<0) break;
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_close(W);                                  // the dividend's interval, relationally
            int64_t alo,ahi; bool hl,hh; oct_interval(W,a,&alo,&hl,&ahi,&hh);
            oct_forget(W, r);
            vra_div_facts(W, r, a, (V->cknown[b] && V->cval[b]>0) ? V->cval[b] : 1, alo,hl,ahi,hh);
            break;
        }
        case IR_SDIV: {  // signed x / c — for x ≥ 0 and c > 0 (the common index idiom
            if (r<0) break;                                 // `i / 2`) it is exactly udiv.
            int a=ins->operands[0]->id, b=ins->operands[1]->id;
            oct_close(W);
            int64_t alo,ahi; bool hl,hh; oct_interval(W,a,&alo,&hl,&ahi,&hh);
            oct_forget(W, r);
            if (hl && alo>=0 && V->cknown[b] && V->cval[b]>0)
                vra_div_facts(W, r, a, V->cval[b], alo,hl,ahi,hh);
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
            int64_t alo,ahi; bool hl,hh; oct_interval(W,a,&alo,&hl,&ahi,&hh);
            if (V->cknown[b] && V->cval[b]>0){
                int64_t c=V->cval[b];
                oct_add_lb(W,r, (hl&&alo>=0)?0:-(c-1)); oct_add_ub(W,r,c-1);
            } else if (hl && alo>=0) {                     // non-const divisor, a ≥ 0, b > 0 in
                oct_add_lb(W,r,0); oct_add_diff_le(W,r,b,-1);   // any defined exec ⇒ 0 ≤ r < b
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
        case IR_OPAQUE:
            // B3: an unmodelled construct. Its RESULT is unknown, and if its declared
            // footprint includes a write it may have changed any escaped cell — the same
            // havoc a call needs, for the same reason. Handling it here is what lets the
            // REST of the function stay analysed instead of the whole function being
            // written off as `incomplete`.
            if (ins->aux.opaque.writes)
                for (int cell=0; cell<V->nvar; cell++) if (V->escaped[cell]) oct_forget(W, cell);
            if (r>=0) oct_forget(W, r);
            break;
        case IR_CALL: {
            // A call may write through any address it was given, and the octagon's memory
            // cells are exactly the scalar allocas — so every ESCAPED cell must be forgotten.
            // Without this the analysis kept a stale value across `bump(var i)` and proved an
            // out-of-bounds `a[i]` check-free.
            for (int cell=0; cell<V->nvar; cell++) if (V->escaped[cell]) oct_forget(W, cell);
            if (r>=0) {
                oct_forget(W, r);
                // ...but the RESULT is not unknown: the callee's body bounds it.
                int64_t rlo, rhi;
                if (ins->result && ins->result->type
                    && vra_ret_range(vra_find_func(ins->aux.callee), &rlo, &rhi)) {
                    if (rlo > -OCT_INF/2) oct_add_lb(W, r, rlo);
                    if (rhi <  OCT_INF/2) oct_add_ub(W, r, rhi);
                }
            }
            break;
        }
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
    if (!d || d->op != IR_ADD || d->n_operands < 2) return false;
    int e0 = V->shape_ext[sbase][0], e1 = V->shape_ext[sbase][1];
    // match ADD(MUL(i, e1), j)  — and the commuted forms
    for (int side=0; side<2; side++) {
        IrValue *mulv = d->operands[side], *jv = d->operands[1-side];
        if (!mulv || !jv) continue;
        IrInstr *m = V->def[mulv->id];
        if (!m || m->op != IR_MUL || m->n_operands < 2) continue;
        int i_id = -1;
        if      (m->operands[1]->id == e1) i_id = m->operands[0]->id;
        else if (m->operands[0]->id == e1) i_id = m->operands[1]->id;
        if (i_id < 0) continue;
        int j_id = jv->id;
        // i < e0  and  j < e1, both as octagon differences (x − y ≤ −1)
        bool i_ok = oct_get(W, oct_pos(e0), oct_pos(i_id)) <= -1;
        bool j_ok = oct_get(W, oct_pos(e1), oct_pos(j_id)) <= -1;
        // and both non-negative (usize gives this, but check the octagon too)
        int64_t ilo,ihi,jlo,jhi; bool ihl,ihh,jhl,jhh;
        oct_interval(W, i_id, &ilo,&ihl,&ihi,&ihh);
        oct_interval(W, j_id, &jlo,&jhl,&jhi,&jhh);
        bool nonneg = (ihl && ilo>=0) && (jhl && jlo>=0);
        if (i_ok && j_ok && nonneg) return true;
    }
    return false;
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
    int shape_base = -1;
    if (bd && bd->op==IR_SLICE_DATA && bd->n_operands>=1) {
        int s = bd->operands[0]->id; if (V->slicelen[s]>=0) lenvar=V->slicelen[s];
        shape_base = s;                                    // the slice value carries the shape
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
    // S2: if the flat check failed, try FACTORING the index against the region's shape.
    if (!c.hi_ok && shape_base>=0 && vra_factor_shape(V, W, shape_base, idx)) {
        c.hi_ok = true; c.lo_ok = true; c.has_len = true;   // both halves come from the factors
    }
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
                oct_interval(W, iv, &ilo2,&ihl2,&ihi2,&ihh2);
                oct_interval(W, stride, &slo2,&shl2,&shi2,&shh2);
                if (oct_get(W, oct_pos(e0), oct_pos(iv)) <= -1 && ihl2 && ilo2>=0
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
    V->escaped=calloc(V->nvar,sizeof(bool));
    V->shape_rank=calloc(V->nvar,sizeof(int));
    V->shape_ext=calloc(V->nvar,sizeof(*V->shape_ext));
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
    // one termination obligation per loop header — but ONLY for a `func` (totality is a
    // func requirement; a `proc` may loop forever, e.g. an event loop). Emitting it for
    // procs was spuriously marking terminating procs "partially proven".
    if (f->kind == IR_FUNC_PURE)
        for (IrBlock *b=f->blocks; b; b=b->next) {
            if (!b->is_loop_header) continue;
            VraCheck c; memset(&c,0,sizeof c); c.kind=VRA_TERMINATION; c.ok=vra_loop_terminates(V,b);
            vra_add_check(V, c);
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
            memcpy(W_m, V->in[b->id], V->dsz*8); W.nvar=V->nvar; W.dim=dim;
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
    return V;
}

static bool vra_ret_range(IrFunc *g, int64_t *lo, int64_t *hi) {
    if (!g || g->is_extern || !g->ret_type) return false;
    if (g->ret_range_state==1) return false;      // recursive query — no fixpoint over itself
    if (g->ret_range_state==3) return false;      // analysed, nothing usable
    if (g->ret_range_state==0) {
        g->ret_range_state = 1;                   // mark in-progress BEFORE recursing
        Vra *sub = vra_analyze(g);                // sets state to 2 or 3 as a side effect
        vra_free(sub);
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
    int dim = 2*V->nvar;
    memcpy(D->scratch, V->in[D->blk->id], (size_t)V->dsz*8);
    Octagon W = { V->nvar, dim, D->scratch };
    oct_close(&W);
    for (IrInstr *ins = D->blk->instrs; ins && ins != D->at; ins = ins->next)
        vra_transfer_instr(V, &W, ins);
    oct_close(&W);
    // a − b ≤ −1  (a < b)   or   b − a ≤ −1  (b < a)
    return oct_get(&W, oct_pos(b->id), oct_pos(a->id)) <= -1
        || oct_get(&W, oct_pos(a->id), oct_pos(b->id)) <= -1;
}

static VraDisjoint *vra_disjoint_open(IrFunc *f) {
    Vra *V = vra_analyze(f);
    if (!V) return NULL;
    VraDisjoint *D = calloc(1, sizeof *D);
    D->V = V; D->scratch = malloc((size_t)V->dsz*8);
    ir_place_index_disjoint_fn  = vra_index_disjoint;
    ir_place_index_disjoint_ctx = D;
    return D;
}
static void vra_disjoint_close(VraDisjoint *D) {
    ir_place_index_disjoint_fn  = NULL;
    ir_place_index_disjoint_ctx = NULL;
    if (!D) return;
    vra_free(D->V); free(D->scratch); free(D);
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
    int dim = 2*V->nvar;
    int64_t *scratch = malloc((size_t)V->dsz*8);
    if (!scratch) return false;
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
            Octagon W = { V->nvar, dim, scratch };
            oct_close(&W);
            for (IrInstr *i=b->instrs; i && ok; i=i->next) {
                bool self = (i->op==IR_CALL && i->aux.callee && f->name
                             && i->aux.callee->length==f->name->length
                             && memcmp(i->aux.callee->name, f->name->name, (size_t)f->name->length)==0);
                if (self) {
                    if (k >= i->n_operands) { ok = false; break; }
                    IrValue *arg = i->operands[k];
                    if (!arg || arg->id<0 || arg->id>=V->nvar) { ok = false; break; }
                    // arg < pv  (arg − pv ≤ −1)   AND   pv ≥ 0 (well-founded below)
                    bool shrinks = oct_get(&W, oct_pos(pv->id), oct_pos(arg->id)) <= -1;
                    int64_t lo,hi; bool hl,hh; oct_interval(&W, pv->id, &lo,&hl,&hi,&hh);
                    bool grounded = (hl && lo >= 0) ||
                                    (pv->type->kind==IRT_INT && !pv->type->is_signed);
                    if (!(shrinks && grounded)) { ok = false; break; }
                }
                vra_transfer_instr(V, &W, i);
            }
        }
        if (ok) { free(scratch); return true; }        // this parameter is a measure
    }
    free(scratch);
    return false;
}

static void vra_free(Vra *V){
    if(!V) return;
    for(int i=0;i<V->f->next_block_id;i++) free(V->in[i]);
    free(V->in); free(V->reached); free(V->def); free(V->defblk); free(V->cval); free(V->cknown);
    free(V->slicelen); free(V->subslice_gep); free(V->escaped); free(V->shape_rank); free(V->shape_ext); free(V->checks); free(V);
}

#endif // LAIN_VRA_H
