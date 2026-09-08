// test_vra.c — soundness/precision of the octagon VRA on hand-built IR. Hand-built
// (not lowered) so we can feed it UNSAFE programs the old sema would reject with an
// exit(): the whole point is to confirm the new engine PROVES the safe patterns and
// REFUSES the unsafe ones. A VRA that "proves" an out-of-bounds access is worse than
// none, so the negatives matter as much as the positives.
//
//   gcc -std=c99 -o /tmp/test_vra src/analysis/test_vra.c -I src && /tmp/test_vra
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
/* no ast.h — this is a pure IR+analysis test (sovereignty) */
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/vra.h"
#include <stdio.h>

static Arena A;
static IrName *nm(const char *s){ return ir_intern(&A, s, (isize)strlen(s)); }
static IrType *arr_i32(int n){ IrType *t=ir_type_new(&A,IRT_ARRAY); t->elem=ir_type_int(&A,32,true); t->array_len=n; return t; }

static int failures=0;
static void vra_expect(const char *what, bool got, bool want){
    printf("  %-42s : %s (expected %s)%s\n", what, got?"PROVEN":"not-proven",
           want?"PROVEN":"not-proven", (got==want)?"":"   <<< WRONG");
    if (got!=want) failures++;
}

// Build `while i <CMP> BOUND { a[i]=0; i=i+1 }` over a fixed array of length `alen`,
// analyze it, and return whether the single a[i] index was proven check-free.
static bool counted_loop_proven(IrCmp cmp, int64_t bound, int alen) {
    IrFunc *f = ir_func_new(&A, nm("f"), ir_type_int(&A,32,true), IR_FUNC_PROC);
    IrType *usize = ir_type_int(&A,64,false);
    IrBlock *entry=f->entry, *head=ir_new_block(f), *body=ir_new_block(f), *exit=ir_new_block(f);
    IrValue *a = ir_alloca_array(f, entry, arr_i32(alen));
    IrValue *islot = ir_alloca(f, entry, usize);
    ir_store(f, entry, islot, ir_const_int(f,entry,0,usize));
    ir_set_br(entry, head);
    // head: i < bound ?
    IrValue *iv = ir_load(f, head, islot, usize);
    IrValue *bnd = ir_const_int(f, head, bound, usize);
    IrValue *c = ir_icmp(f, head, cmp, iv, bnd);
    ir_set_br_cond(head, c, body, exit);
    // body: a[i]=0 ; i=i+1
    IrValue *iv2 = ir_load(f, body, islot, usize);
    IrValue *p = ir_elem_ptr(f, body, a, iv2, ir_type_int(&A,32,true));
    ir_store(f, body, p, ir_const_int(f,body,0,ir_type_int(&A,32,true)));
    IrValue *iv3 = ir_load(f, body, islot, usize);
    IrValue *ni = ir_binop(f, body, IR_ADD, iv3, ir_const_int(f,body,1,usize), usize);
    ir_store(f, body, islot, ni);
    ir_set_br(body, head);
    ir_set_ret(exit, NULL);
    ir_finalize_cfg(f);

    Vra *V=vra_analyze(f);
    bool proven=false, found=false;
    for (int i=0;i<V->nchecks;i++) if (V->checks[i].kind==VRA_BOUNDS){ proven=V->checks[i].ok; found=true; }
    vra_free(V);
    return found && proven;
}

// REVERSE over a fixed array: `i=0; while i<N { a[(N-1)-i]; i+=1 }`. The index is
// const − i (a sum relation r+i=N-1); with i∈[0,N-1] the octagon gets r∈[0,N-1].
static bool reverse_fixed_proven(int N) {
    IrFunc *f=ir_func_new(&A,nm("rv"),ir_type_int(&A,32,true),IR_FUNC_PROC);
    IrType *i32=ir_type_int(&A,32,true);
    IrBlock *e=f->entry,*head=ir_new_block(f),*body=ir_new_block(f),*ex=ir_new_block(f);
    IrValue *a=ir_alloca_array(f,e,arr_i32(N)), *islot=ir_alloca(f,e,i32);
    ir_store(f,e,islot,ir_const_int(f,e,0,i32)); ir_set_br(e,head);
    IrValue *iv=ir_load(f,head,islot,i32);
    IrValue *cmp=ir_icmp(f,head,IR_CMP_SLT,iv,ir_const_int(f,head,N,i32));
    ir_set_br_cond(head,cmp,body,ex);
    IrValue *iv2=ir_load(f,body,islot,i32);
    IrValue *idx=ir_binop(f,body,IR_SUB,ir_const_int(f,body,N-1,i32),iv2,i32);  // (N-1) - i
    ir_elem_ptr(f,body,a,idx,i32);
    IrValue *iv3=ir_load(f,body,islot,i32);
    ir_store(f,body,islot,ir_binop(f,body,IR_ADD,iv3,ir_const_int(f,body,1,i32),i32));
    ir_set_br(body,head); ir_set_ret(ex,NULL); ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool ok=false;
    for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_BOUNDS) ok=V->checks[i].ok;
    vra_free(V); return ok;
}

// Build `a[i]=0` with `i` an unconstrained parameter over a length-`alen` array.
static bool unguarded_param_proven(int alen) {
    IrFunc *f = ir_func_new(&A, nm("g"), ir_type_int(&A,32,true), IR_FUNC_PROC);
    IrValue *pi = ir_add_param(f, ir_type_int(&A,64,false), nm("i"));
    IrBlock *e=f->entry;
    IrValue *a = ir_alloca_array(f, e, arr_i32(alen));
    IrValue *p = ir_elem_ptr(f, e, a, pi, ir_type_int(&A,32,true));
    ir_store(f, e, p, ir_const_int(f,e,0,ir_type_int(&A,32,true)));
    ir_set_ret(e, NULL);
    ir_finalize_cfg(f);
    Vra *V=vra_analyze(f);
    bool proven=false, found=false;
    for (int i=0;i<V->nchecks;i++) if (V->checks[i].kind==VRA_BOUNDS){ proven=V->checks[i].ok; found=true; }
    vra_free(V);
    return found && proven;
}

static IrType *slice_i32(void){ IrType *t=ir_type_new(&A,IRT_SLICE); t->elem=ir_type_int(&A,32,true); return t; }

// MASK idiom: a[ x & MASK ] for an unconstrained x. Provable iff MASK < alen
// (x & MASK ∈ [0,MASK] for any x when MASK ≥ 0).
static bool mask_index_proven(int mask, int alen) {
    IrFunc *f=ir_func_new(&A,nm("mk"),ir_type_int(&A,32,true),IR_FUNC_PROC);
    IrType *u32=ir_type_int(&A,32,false), *i32=ir_type_int(&A,32,true);
    IrValue *x=ir_add_param(f,u32,nm("x"));
    IrBlock *e=f->entry;
    IrValue *a=ir_alloca_array(f,e,arr_i32(alen));
    IrValue *idx=ir_binop(f,e,IR_AND,x,ir_const_int(f,e,mask,u32),u32);
    ir_elem_ptr(f,e,a,idx,i32);
    ir_set_ret(e,NULL); ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool ok=false;
    for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_BOUNDS) ok=V->checks[i].ok;
    vra_free(V); return ok;
}

// A bit intrinsic is bounded by its OPERAND's width, for ANY input: @popcount(x) on a u32
// is in [0,32]. Modelling ctz/clz/popcount as IR ops rather than opaque calls is what makes
// `a[@popcount(x)]` provable with no runtime check — and refusable when the array is too
// small, which is the soundness half.
static bool bitcount_index_proven(IrOp op, int opbits, int alen) {
    IrFunc *f=ir_func_new(&A,nm("bc"),ir_type_int(&A,32,true),IR_FUNC_PROC);
    IrType *ub=ir_type_int(&A,opbits,false), *u32=ir_type_int(&A,32,false), *i32=ir_type_int(&A,32,true);
    IrValue *x=ir_add_param(f,ub,nm("x"));
    IrBlock *e=f->entry;
    IrValue *a=ir_alloca_array(f,e,arr_i32(alen));
    IrValue *idx=ir_bitcount(f,e,op,x,u32);
    ir_elem_ptr(f,e,a,idx,i32);
    ir_set_ret(e,NULL); ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool ok=false;
    for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_BOUNDS) ok=V->checks[i].ok;
    vra_free(V); return ok;
}

// The RELATIONAL frontier the old engine REJECTS: sliding window a[i+1] under a
// guard `i+1 < a.len`. Intervals can't relate i+1 to len; octagons can. Build it
// by hand (sema would exit on the reject) and confirm the new engine PROVES it.
//   func f(a i32[]) { i=0; while i+1 < a.len { a[i+1]; i=i+1 } }
static bool sliding_window_proven(void) {
    IrFunc *f = ir_func_new(&A, nm("win"), ir_type_int(&A,32,true), IR_FUNC_PROC);
    IrType *usize=ir_type_int(&A,64,false), *i32=ir_type_int(&A,32,true);
    IrValue *a = ir_add_param(f, slice_i32(), nm("a"));
    IrBlock *entry=f->entry, *head=ir_new_block(f), *body=ir_new_block(f), *exit=ir_new_block(f);
    IrValue *islot=ir_alloca(f, entry, usize);
    ir_store(f, entry, islot, ir_const_int(f,entry,0,usize));
    ir_set_br(entry, head);
    // head: (i+1) < a.len ?
    IrValue *iv=ir_load(f,head,islot,usize);
    IrValue *ip1=ir_binop(f,head,IR_ADD,iv,ir_const_int(f,head,1,usize),usize);
    IrValue *L=ir_slice_len(f,head,a);
    IrValue *cmp=ir_icmp(f,head,IR_CMP_ULT,ip1,L);
    ir_set_br_cond(head,cmp,body,exit);
    // body: a[i+1] ; i=i+1
    IrValue *iv2=ir_load(f,body,islot,usize);
    IrValue *idx=ir_binop(f,body,IR_ADD,iv2,ir_const_int(f,body,1,usize),usize);
    IrValue *data=ir_slice_data(f,body,a,i32);
    ir_elem_ptr(f,body,data,idx,i32);              // bounds obligation: idx < len(a)
    IrValue *iv3=ir_load(f,body,islot,usize);
    IrValue *ni=ir_binop(f,body,IR_ADD,iv3,ir_const_int(f,body,1,usize),usize);
    ir_store(f,body,islot,ni);
    ir_set_br(body,head);
    ir_set_ret(exit,NULL);
    ir_finalize_cfg(f);
    Vra *V=vra_analyze(f);
    bool proven=false; for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_BOUNDS) proven=V->checks[i].ok;
    vra_free(V); return proven;
}

// TWO-POINTER relational: a[i] under `i ≤ j` and `j < a.len`. Neither guard alone
// bounds i by len; only the octagon's transitive closure (i ≤ j ≤ len−1) does.
//   func f(a i32[], i usize, j usize) { if i<=j { if j<a.len { a[i] } } }
static bool two_pointer_proven(void) {
    IrFunc *f = ir_func_new(&A, nm("tp"), ir_type_int(&A,32,true), IR_FUNC_PROC);
    IrType *usize=ir_type_int(&A,64,false), *i32=ir_type_int(&A,32,true);
    IrValue *a=ir_add_param(f, slice_i32(), nm("a"));
    IrValue *i=ir_add_param(f, usize, nm("i"));
    IrValue *j=ir_add_param(f, usize, nm("j"));
    IrBlock *e=f->entry, *b1=ir_new_block(f), *b2=ir_new_block(f), *ex=ir_new_block(f);
    IrValue *c1=ir_icmp(f,e,IR_CMP_ULE,i,j);      // i <= j
    ir_set_br_cond(e,c1,b1,ex);
    IrValue *L=ir_slice_len(f,b1,a);
    IrValue *c2=ir_icmp(f,b1,IR_CMP_ULT,j,L);     // j < len
    ir_set_br_cond(b1,c2,b2,ex);
    IrValue *data=ir_slice_data(f,b2,a,i32);
    ir_elem_ptr(f,b2,data,i,i32);                 // prove i < len via i<=j<len
    ir_set_ret(b2,NULL); ir_set_ret(ex,NULL);
    ir_finalize_cfg(f);
    Vra *V=vra_analyze(f);
    bool proven=false; for(int k=0;k<V->nchecks;k++) if(V->checks[k].kind==VRA_BOUNDS) proven=V->checks[k].ok;
    vra_free(V); return proven;
}

static bool find_ok(Vra *V, VraCheckKind k){ for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==k) return V->checks[i].ok; return false; }
static bool find_any(Vra *V, VraCheckKind k){ for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==k) return true; return false; }

// `x + x` for a u8 param, optionally under an `x < 100` guard. Overflow-proven iff
// the guard bounds x enough that x+x fits u8.
static bool add_overflow_proven(bool guarded) {
    IrFunc *f = ir_func_new(&A, nm("ov"), ir_type_int(&A,8,false), IR_FUNC_PROC);
    IrType *u8=ir_type_int(&A,8,false);
    IrValue *x = ir_add_param(f, u8, nm("x"));
    IrBlock *e=f->entry;
    IrBlock *body = guarded ? ir_new_block(f) : e;
    if (guarded) {
        IrValue *c=ir_const_int(f,e,100,u8);
        IrValue *cmp=ir_icmp(f,e,IR_CMP_ULT,x,c);
        IrBlock *els=ir_new_block(f);
        ir_set_br_cond(e, cmp, body, els);
        ir_set_ret(els, NULL);
    }
    ir_binop(f, body, IR_ADD, x, x, u8);      // overflow obligation here
    ir_set_ret(body, NULL);
    ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool ok=find_any(V,VRA_OVERFLOW)&&find_ok(V,VRA_OVERFLOW); vra_free(V); return ok;
}
// `a / b` for i32 params, optionally under a `b > 0` guard.
static bool div_proven(bool guarded) {
    IrFunc *f = ir_func_new(&A, nm("dv"), ir_type_int(&A,32,true), IR_FUNC_PROC);
    IrType *i32=ir_type_int(&A,32,true);
    IrValue *a=ir_add_param(f,i32,nm("a")), *b=ir_add_param(f,i32,nm("b"));
    IrBlock *e=f->entry, *body = guarded?ir_new_block(f):e;
    if (guarded) {
        IrValue *z=ir_const_int(f,e,0,i32);
        IrValue *cmp=ir_icmp(f,e,IR_CMP_SGT,b,z);      // b > 0
        IrBlock *els=ir_new_block(f);
        ir_set_br_cond(e,cmp,body,els); ir_set_ret(els,NULL);
    }
    ir_binop(f, body, IR_SDIV, a, b, i32);     // div-by-zero obligation here
    ir_set_ret(body, NULL);
    ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool ok=find_any(V,VRA_DIVZERO)&&find_ok(V,VRA_DIVZERO); vra_free(V); return ok;
}

// NESTED loops: `i=0; while i<N { j=0; while j<N { a[j]; j+=1 } i+=1 }` — two loop
// headers, two induction cells. Confirms the multi-header fixpoint proves a[j].
static bool nested_loop_proven(int N) {
    IrFunc *f=ir_func_new(&A,nm("ns"),ir_type_int(&A,32,true),IR_FUNC_PROC);
    IrType *i32=ir_type_int(&A,32,true);
    IrBlock *e=f->entry,*oh=ir_new_block(f),*ii=ir_new_block(f),*ih=ir_new_block(f),
            *ib=ir_new_block(f),*ol=ir_new_block(f),*ex=ir_new_block(f);
    IrValue *a=ir_alloca_array(f,e,arr_i32(N)), *is=ir_alloca(f,e,i32), *js=ir_alloca(f,e,i32);
    ir_store(f,e,is,ir_const_int(f,e,0,i32)); ir_set_br(e,oh);
    IrValue *iv=ir_load(f,oh,is,i32);
    ir_set_br_cond(oh, ir_icmp(f,oh,IR_CMP_SLT,iv,ir_const_int(f,oh,N,i32)), ii, ex);
    ir_store(f,ii,js,ir_const_int(f,ii,0,i32)); ir_set_br(ii,ih);
    IrValue *jv=ir_load(f,ih,js,i32);
    ir_set_br_cond(ih, ir_icmp(f,ih,IR_CMP_SLT,jv,ir_const_int(f,ih,N,i32)), ib, ol);
    IrValue *jv2=ir_load(f,ib,js,i32);
    ir_elem_ptr(f,ib,a,jv2,i32);                       // prove j < N
    ir_store(f,ib,js,ir_binop(f,ib,IR_ADD,ir_load(f,ib,js,i32),ir_const_int(f,ib,1,i32),i32));
    ir_set_br(ib,ih);
    ir_store(f,ol,is,ir_binop(f,ol,IR_ADD,ir_load(f,ol,is,i32),ir_const_int(f,ol,1,i32),i32));
    ir_set_br(ol,oh);
    ir_set_ret(ex,NULL); ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool ok=false;
    for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_BOUNDS) ok=V->checks[i].ok;
    vra_free(V); return ok;
}

// termination: `i=0; while i <cmp> B { i = i + step }`.
static bool loop_terminates(int step, IrCmp cmp, int bound) {
    IrFunc *f=ir_func_new(&A,nm("t"),ir_type_int(&A,32,true),IR_FUNC_PURE);  // termination is a func property
    IrType *i32=ir_type_int(&A,32,true);
    IrBlock *e=f->entry,*head=ir_new_block(f),*body=ir_new_block(f),*ex=ir_new_block(f);
    IrValue *islot=ir_alloca(f,e,i32);
    ir_store(f,e,islot,ir_const_int(f,e,0,i32)); ir_set_br(e,head);
    IrValue *iv=ir_load(f,head,islot,i32);
    ir_set_br_cond(head, ir_icmp(f,head,cmp,iv,ir_const_int(f,head,bound,i32)), body, ex);
    IrValue *iv3=ir_load(f,body,islot,i32);
    ir_store(f,body,islot,ir_binop(f,body,IR_ADD,iv3,ir_const_int(f,body,step,i32),i32));
    ir_set_br(body,head); ir_set_ret(ex,NULL); ir_finalize_cfg(f);
    Vra *V=vra_analyze(f); bool term=false;
    for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_TERMINATION) term=V->checks[i].ok;
    vra_free(V); return term;
}

int main(void) {
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*256);

    // POSITIVES — the canonical safe patterns must be proven.
    vra_expect("counted loop  i < 8  over a[8]",          counted_loop_proven(IR_CMP_ULT, 8, 8), true);
    vra_expect("counted loop  i < 8  over a[16] (slack)", counted_loop_proven(IR_CMP_ULT, 8, 16), true);

    // NEGATIVES — the VRA must REFUSE these (each is a real out-of-bounds).
    vra_expect("off-by-one   i <= 8  over a[8]  (i hits 8)", counted_loop_proven(IR_CMP_ULE, 8, 8), false);
    vra_expect("over-bound   i < 16 over a[8]  (i hits 8..15)", counted_loop_proven(IR_CMP_ULT, 16, 8), false);
    vra_expect("unguarded param index  a[i], i:param, a[4]",   unguarded_param_proven(4), false);

    // OVERFLOW (design §2.6) — prove when the range fits, refuse when it can wrap.
    vra_expect("u8 x+x under guard x<100  (≤198 fits u8)",  add_overflow_proven(true),  true);
    vra_expect("u8 x+x unguarded          (≤510 wraps u8)", add_overflow_proven(false), false);
    // DIV-BY-ZERO — prove when the divisor is provably nonzero, else refuse.
    vra_expect("i32 a/b under guard b>0   (b≠0)",           div_proven(true),  true);
    vra_expect("i32 a/b unguarded         (b may be 0)",    div_proven(false), false);
    // RELATIONAL frontier — octagons crack these; intervals/recognizers struggle.
    vra_expect("sliding window a[i+1] under i+1<a.len",     sliding_window_proven(), true);
    vra_expect("two-pointer a[i] under i<=j & j<a.len",     two_pointer_proven(),    true);
    // MASK idiom — nonlinear transfer: x & (N-1) ∈ [0,N-1] for ANY x.
    // ── 3.4: RECURSION TERMINATION ─────────────────────────────────────────────────────
    // A self-recursive function is total when some parameter STRICTLY SHRINKS at every
    // self-call and is bounded below. Both halves are octagon questions asked at the call
    // edge — the loop measure lifted from a back-edge. Without it, the conservative cycle
    // rule calls every recursion DIVERGE, so a well-founded one can never be a total `func`.
    // ★ What makes a recursion WELL-FOUNDED, stated as the four cases that actually differ.
    // The original test asserted that `f(n − 1)` on a bare u64 is total. It is not: at n = 0
    // the subtraction underflows to U64MAX and the recursion runs forever. It passed only
    // because the domain used to record `n − 1` as an exact ℤ relation for an UNSIGNED
    // subtraction that can wrap — the same defect the corpus's own soundness lock caught
    // (CORPUS_CORRECTIONS C-6/C-7). A guard is what makes the shape total, so the test now
    // says that: with `n ≥ 1` assumed it is total, and without it it is not.
    //
    // A SIGNED measure does not help either, and for a different reason worth keeping: it is
    // not GROUNDED. `f(n − 1)` on an i32 descends forever toward INT32_MIN.
    { IrType *u64t = ir_type_int(&A,64,false);
      struct { const char *what; int delta; bool guard; bool want; } rc[] = {
          { "recursion f(n-1) under `n >= 1` is TOTAL",        -1, true,  true  },
          { "recursion f(n-1) with NO guard: n=0 WRAPS",       -1, false, false },
          { "recursion f(n+1) DIVERGES (measure grows)",       +1, false, false },
          { "recursion f(n)   DIVERGES (measure stuck)",        0, false, false },
      };
      for (unsigned t=0;t<sizeof rc/sizeof rc[0];t++) {
        IrFunc *f=ir_func_new(&A,nm("rec"),u64t,IR_FUNC_PROC);
        IrValue *n=ir_add_param(f,u64t,nm("n"));
        IrBlock *e=f->entry;
        if (rc[t].guard)
            ir_assume(f, e, ir_icmp(f, e, IR_CMP_UGE, n, ir_const_int(f,e,1,u64t)));
        IrValue *arg = rc[t].delta==0 ? n
                     : ir_binop(f,e, rc[t].delta<0?IR_SUB:IR_ADD, n,
                                ir_const_int(f,e,1,u64t), u64t);
        IrInstr *call = ir_instr(f, IR_CALL, u64t, 1);
        call->operands[0]=arg; call->aux.callee = nm("rec"); ir_emit(e, call);
        ir_set_ret(e, call->result); ir_finalize_cfg(f);
        Vra *V=vra_analyze(f);
        bool tot = vra_recursion_terminates(V, f);
        vra_free(V);
        vra_expect(rc[t].what, tot, rc[t].want);
      }
    }

    // ── B4: a STATIC REFINEMENT on the TYPE ────────────────────────────────────────────
    // `type Small = u8 < 200` is a property of the TYPE, so every value of it inherits the
    // interval with no assume and no side-map. Resolving the alias to its base DISCARDED it,
    // making `n Small` indistinguishable from `n u8`. The negative case matters equally: the
    // refinement must not prove an access the interval does not cover.
    { IrType *u8r = ir_type_int(&A,8,false);
      u8r->has_refine = true; u8r->refine_lo = 0; u8r->refine_hi = 199;
      IrType *u8p = ir_type_int(&A,8,false);          // the same width, NO refinement
      struct { const char *what; IrType *ty; int alen; bool want; } cs[] = {
          { "refined u8<200 indexes a[200]  PROVES",       u8r, 200, true  },
          { "refined u8<200 indexes a[150]  refused",      u8r, 150, false },
          { "PLAIN u8 indexes a[200]        refused",      u8p, 200, false },
      };
      for (unsigned t=0;t<sizeof cs/sizeof cs[0];t++) {
        IrFunc *f=ir_func_new(&A,nm("ref"),ir_type_int(&A,32,true),IR_FUNC_PROC);
        IrValue *n=ir_add_param(f,cs[t].ty,nm("n"));
        IrBlock *e=f->entry;
        IrValue *a=ir_alloca_array(f,e,arr_i32(cs[t].alen));
        ir_elem_ptr(f,e,a,n,ir_type_int(&A,32,true));
        ir_set_ret(e,NULL); ir_finalize_cfg(f);
        Vra *V=vra_analyze(f); bool ok=false;
        for(int k=0;k<V->nchecks;k++) if(V->checks[k].kind==VRA_BOUNDS) ok=V->checks[k].ok;
        vra_free(V);
        vra_expect(cs[t].what, ok, cs[t].want);
      }
    }

    // ── S2: rank-N strided regions ─────────────────────────────────────────────────────
    // `a[i*w + j]` over a region declared `i32[h*w]`. The FLAT obligation
    // `i*w + j < h*w` is NONLINEAR and no octagon can express it; with the shape it FACTORS
    // into `i < h ∧ j < w`, both already in the octagon. The negative cases matter as much:
    // widen either guard by one and the access leaves its row / the region, and factoring
    // must refuse.
    { IrType *u64t=ir_type_int(&A,64,false), *i32t=ir_type_int(&A,32,true);
      // build: shape(a,h,w); assume i<h; assume j<(w or w+1); a.data[i*w+j]
      // `hi_slack`/`wi_slack` widen a guard to make the access genuinely out of bounds.
      struct { const char *what; int hs, ws; bool want; } cases[] = {
          { "2D a[i*w+j] under i<h, j<w  PROVES (factored)",      0, 0, true  },
          { "2D a[i*w+j] under i<h, j<w+1 refused (out of row)",  0, 1, false },
          { "2D a[i*w+j] under i<h+1, j<w refused (past end)",    1, 0, false },
      };
      for (unsigned t=0; t<sizeof cases/sizeof cases[0]; t++) {
        IrFunc *f=ir_func_new(&A,nm("m2d"),i32t,IR_FUNC_PROC);
        IrValue *a=ir_add_param(f,slice_i32(),nm("a"));
        IrValue *h=ir_add_param(f,u64t,nm("h")), *w=ir_add_param(f,u64t,nm("w"));
        IrValue *i=ir_add_param(f,u64t,nm("i")), *j=ir_add_param(f,u64t,nm("j"));
        IrBlock *e=f->entry;
        IrValue *ext[2]={h,w}; ir_shape(f,e,a,ext,2);
        ir_slice_len(f,e,a);                                  // canonical length var
        IrValue *hb=h, *wb=w;
        if (cases[t].hs) hb=ir_binop(f,e,IR_ADD,h,ir_const_int(f,e,1,u64t),u64t);
        if (cases[t].ws) wb=ir_binop(f,e,IR_ADD,w,ir_const_int(f,e,1,u64t),u64t);
        ir_assume(f,e, ir_icmp(f,e,IR_CMP_ULT,i,hb));
        ir_assume(f,e, ir_icmp(f,e,IR_CMP_ULT,j,wb));
        IrValue *idx=ir_binop(f,e,IR_ADD, ir_binop(f,e,IR_MUL,i,w,u64t), j, u64t);
        ir_elem_ptr(f,e, ir_slice_data(f,e,a,i32t), idx, i32t);
        ir_set_ret(e,NULL); ir_finalize_cfg(f);
        Vra *V=vra_analyze(f); bool ok=false;
        for(int k=0;k<V->nchecks;k++) if(V->checks[k].kind==VRA_BOUNDS) ok=V->checks[k].ok;
        vra_free(V);
        vra_expect(cases[t].what, ok, cases[t].want);
      }
    }

    // ESCAPE: a call may write through any address it was given, so a cell whose address
    // escaped cannot keep its value across one. Modelling a scalar alloca as a stable cell
    // without this proved an out-of-bounds a[i] check-free after `bump(var i)` set i = 100.
    { IrFunc *f=ir_func_new(&A,nm("esc"),ir_type_int(&A,32,true),IR_FUNC_PROC);
      IrType *i32e=ir_type_int(&A,32,true); IrBlock *e=f->entry;
      IrValue *a=ir_alloca_array(f,e,arr_i32(4));
      IrValue *i=ir_alloca(f,e,i32e);
      ir_store(f,e,i,ir_const_int(f,e,0,i32e));      // i = 0  (in bounds for a[4])
      { IrInstr *c=ir_instr(f,IR_CALL,NULL,1); c->operands[0]=i; ir_emit(e,c); }  // f(&i)
      ir_elem_ptr(f,e,a,ir_load(f,e,i,i32e),i32e);   // a[i]  — must NOT be proven
      ir_set_ret(e,NULL); ir_finalize_cfg(f);
      Vra *V=vra_analyze(f); bool ok=false;
      for(int k=0;k<V->nchecks;k++) if(V->checks[k].kind==VRA_BOUNDS) ok=V->checks[k].ok;
      vra_free(V);
      vra_expect("a[i] after a call took &i is NOT proven", ok, false); }
    // ...but a cell whose address never escapes keeps its value across a call
    { IrFunc *f=ir_func_new(&A,nm("noesc"),ir_type_int(&A,32,true),IR_FUNC_PROC);
      IrType *i32e=ir_type_int(&A,32,true); IrBlock *e=f->entry;
      IrValue *a=ir_alloca_array(f,e,arr_i32(4));
      IrValue *i=ir_alloca(f,e,i32e);
      ir_store(f,e,i,ir_const_int(f,e,0,i32e));
      { IrInstr *c=ir_instr(f,IR_CALL,NULL,0); ir_emit(e,c); }   // a call touching nothing
      ir_elem_ptr(f,e,a,ir_load(f,e,i,i32e),i32e);
      ir_set_ret(e,NULL); ir_finalize_cfg(f);
      Vra *V=vra_analyze(f); bool ok=false;
      for(int k=0;k<V->nchecks;k++) if(V->checks[k].kind==VRA_BOUNDS) ok=V->checks[k].ok;
      vra_free(V);
      vra_expect("a[i] across a call that never saw &i IS proven", ok, true); }

    vra_expect("mask a[x & 7] over a[8]  (0..7 < 8)",        mask_index_proven(7,8),  true);
    vra_expect("mask a[x & 15] over a[8] (0..15 escapes)",   mask_index_proven(15,8), false);
    vra_expect("a[@popcount(u32 x)] over a[33] (0..32 < 33)", bitcount_index_proven(IR_POPCOUNT,32,33), true);
    vra_expect("a[@popcount(u32 x)] over a[32] (32 escapes)", bitcount_index_proven(IR_POPCOUNT,32,32), false);
    vra_expect("a[@ctz(u8 x)] over a[9]      (0..8 < 9)",     bitcount_index_proven(IR_CTZ,8,9),        true);
    vra_expect("a[@clz(u8 x)] over a[8]      (8 escapes)",    bitcount_index_proven(IR_CLZ,8,8),        false);
    vra_expect("reverse a[(N-1)-i] over a[10]  (i<10)",       reverse_fixed_proven(10), true);
    vra_expect("nested loops: a[j] under j<N (2 headers)",    nested_loop_proven(10),   true);
    // TERMINATION — a func's loops must drain their bound.
    vra_expect("terminates: i+=1 while i<8",                  loop_terminates(1,IR_CMP_SLT,8),  true);
    vra_expect("terminates: i+=2 while i<=8",                 loop_terminates(2,IR_CMP_SLE,8),  true);
    vra_expect("NON-term: i+=0 while i<8 (stuck)",            loop_terminates(0,IR_CMP_SLT,8),  false);
    vra_expect("NON-term: i-=1 while i<8 (diverges)",         loop_terminates(-1,IR_CMP_SLT,8), false);


    // ── INFERRED RETURN RANGES ────────────────────────────────────────────────────────
    // A call's result used to be FORGOTTEN, so an index computed by a helper could never be
    // proven. The range now comes from the callee's body. Both directions matter: a callee
    // that really does bound its result must PROVE, and one that does not must NOT — the
    // failure mode here is a removed bounds check.
    {
        IrType *u8t = ir_type_int(&A,8,false);
        // callee: `func g(c u8) u8 { return c & MASK }`  (MASK=15 ⇒ result in [0,15])
        // caller: `proc caller() { a[g(200)] }` over a[16]
        // `mask` < 0 builds the UNBOUNDED callee `return c`, which must not prove.
        struct { int mask; int alen; bool want; const char *what; } cases[] = {
            { 15, 16, true,  "a[g(x)] where g returns x & 15, a[16]" },
            { 15,  8, false, "...same g, but a[8] — 15 is out of range" },
            { -1, 16, false, "a[g(x)] where g returns x unmasked (0..255)" },
        };
        for (unsigned k=0; k<sizeof cases/sizeof *cases; k++) {
            IrFunc *g = ir_func_new(&A, nm("g"), u8t, IR_FUNC_PURE);
            IrValue *c = ir_add_param(g, u8t, nm("c"));
            IrValue *rv = c;
            if (cases[k].mask >= 0)
                rv = ir_binop(g, g->entry, IR_AND, c, ir_const_int(g,g->entry,cases[k].mask,u8t), u8t);
            ir_set_ret(g->entry, rv);

            IrFunc *f = ir_func_new(&A, nm("caller"), ir_type_int(&A,32,true), IR_FUNC_PROC);
            IrValue *a = ir_alloca_array(f, f->entry, arr_i32(cases[k].alen));
            IrInstr *call = ir_instr(f, IR_CALL, u8t, 1);
            call->aux.callee = g->name;
            call->operands[0] = ir_const_int(f,f->entry,200,u8t);
            ir_emit(f->entry, call);
            ir_elem_ptr(f, f->entry, a, call->result, ir_type_int(&A,32,true));
            ir_set_ret(f->entry, NULL);

            f->next = g; vra_mod = f;              // the module the callee is looked up in
            g->ret_range_state = 0;                // fresh query per case
            Vra *V = vra_analyze(f);
            bool ok=false; for (int i=0;i<V->nchecks;i++) if (V->checks[i].kind==VRA_BOUNDS) ok=V->checks[i].ok;
            vra_free(V); vra_mod = NULL;
            vra_expect(cases[k].what, ok, cases[k].want);
        }

        // A self-recursive callee must terminate the query rather than recurse forever, and
        // must not invent a range: `state == 1` (in progress) falls back to nothing known.
        {
            IrFunc *g = ir_func_new(&A, nm("rec"), u8t, IR_FUNC_PURE);
            IrValue *c = ir_add_param(g, u8t, nm("c"));
            IrInstr *self = ir_instr(g, IR_CALL, u8t, 1);
            self->aux.callee = g->name; self->operands[0] = c;
            ir_emit(g->entry, self);
            ir_set_ret(g->entry, self->result);

            IrFunc *f = ir_func_new(&A, nm("caller2"), ir_type_int(&A,32,true), IR_FUNC_PROC);
            IrValue *a = ir_alloca_array(f, f->entry, arr_i32(4));
            IrInstr *call = ir_instr(f, IR_CALL, u8t, 1);
            call->aux.callee = g->name;
            call->operands[0] = ir_const_int(f,f->entry,200,u8t);
            ir_emit(f->entry, call);
            ir_elem_ptr(f, f->entry, a, call->result, ir_type_int(&A,32,true));
            ir_set_ret(f->entry, NULL);

            f->next = g; vra_mod = f; g->ret_range_state = 0;
            Vra *V = vra_analyze(f);               // must return, not spin
            bool ok=false; for (int i=0;i<V->nchecks;i++) if (V->checks[i].kind==VRA_BOUNDS) ok=V->checks[i].ok;
            vra_free(V); vra_mod = NULL;
            vra_expect("a recursive callee terminates and proves nothing", ok, false);
        }
    }

    if (failures==0) printf("VRA: all soundness+precision expectations met\n");
    else             printf("VRA: %d WRONG results\n", failures);
    return failures?1:0;
}
