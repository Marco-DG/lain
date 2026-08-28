// test_vra.c — soundness/precision of the octagon VRA on hand-built IR. Hand-built
// (not lowered) so we can feed it UNSAFE programs the old sema would reject with an
// exit(): the whole point is to confirm the new engine PROVES the safe patterns and
// REFUSES the unsafe ones. A VRA that "proves" an out-of-bounds access is worse than
// none, so the negatives matter as much as the positives.
//
//   gcc -std=c99 -o /tmp/test_vra src/analysis/test_vra.c -I src && /tmp/test_vra
#include "utils/common/def.h"
#include "utils/arena.h"
#include "ast.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/vra.h"
#include <stdio.h>

static Arena A;
static Id *nm(const char *s){ Id *i=arena_push_aligned(&A,Id); i->name=(char*)s; i->length=(isize)strlen(s); return i; }
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
    // RELATIONAL frontier — the old engine REJECTS this; octagons prove it.
    vra_expect("sliding window a[i+1] under i+1<a.len",     sliding_window_proven(), true);

    if (failures==0) printf("VRA: all soundness+precision expectations met\n");
    else             printf("VRA: %d WRONG results\n", failures);
    return failures?1:0;
}
