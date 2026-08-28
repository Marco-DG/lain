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
    bool proven = V->nchecks==1 && V->checks[0].lo_ok && V->checks[0].hi_ok;
    vra_free(V);
    return proven;
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
    bool proven = V->nchecks==1 && V->checks[0].lo_ok && V->checks[0].hi_ok;
    vra_free(V);
    return proven;
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

    if (failures==0) printf("VRA: all soundness+precision expectations met\n");
    else             printf("VRA: %d WRONG results\n", failures);
    return failures?1:0;
}
