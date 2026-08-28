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
    IrFunc *f=ir_func_new(&A,nm("t"),ir_type_int(&A,32,true),IR_FUNC_PROC);
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
    vra_expect("mask a[x & 7] over a[8]  (0..7 < 8)",        mask_index_proven(7,8),  true);
    vra_expect("mask a[x & 15] over a[8] (0..15 escapes)",   mask_index_proven(15,8), false);
    vra_expect("reverse a[(N-1)-i] over a[10]  (i<10)",       reverse_fixed_proven(10), true);
    vra_expect("nested loops: a[j] under j<N (2 headers)",    nested_loop_proven(10),   true);
    // TERMINATION — a func's loops must drain their bound.
    vra_expect("terminates: i+=1 while i<8",                  loop_terminates(1,IR_CMP_SLT,8),  true);
    vra_expect("terminates: i+=2 while i<=8",                 loop_terminates(2,IR_CMP_SLE,8),  true);
    vra_expect("NON-term: i+=0 while i<8 (stuck)",            loop_terminates(0,IR_CMP_SLT,8),  false);
    vra_expect("NON-term: i-=1 while i<8 (diverges)",         loop_terminates(-1,IR_CMP_SLT,8), false);

    if (failures==0) printf("VRA: all soundness+precision expectations met\n");
    else             printf("VRA: %d WRONG results\n", failures);
    return failures?1:0;
}
