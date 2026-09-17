// test_place.c — the PLACE lattice on hand-built IR (sovereign, no AST).
// Places are the shared substrate for definite-assignment, borrow checking and linearity,
// so their overlap relation must be exactly right: too coarse and every borrow conflicts
// (useless), too sharp and we miss real aliasing (unsound).
//   gcc -std=c99 -o /tmp/test_place src/analysis/test_place.c -I src && /tmp/test_place
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "ir/place.h"
#include <stdio.h>

static Arena A;
static IrName *nm(const char *s){ return ir_intern(&A, s, (isize)strlen(s)); }
static int failures=0;
static void pexpect(const char *what, bool got, bool want){
    printf("  %-52s : %s (expected %s)%s\n", what, got?"overlap":"DISJOINT", want?"overlap":"DISJOINT",
           got==want?"":"   <<< WRONG");
    if (got!=want) failures++;
}

// build def[] for a function (value id → defining instruction)
static IrInstr **mkdef(IrFunc *f, int *nvar){
    *nvar = f->next_value_id>0?f->next_value_id:1;
    IrInstr **def = calloc(*nvar, sizeof *def);
    for (IrBlock *b=f->blocks;b;b=b->next)
        for (IrInstr *i=b->instrs;i;i=i->next) if (i->result) def[i->result->id]=i;
    return def;
}

// a test hook: "indices are disjoint iff they are different SSA values" — stands in for the
// VRA's real i≠j proof (Stage III-D) so we can verify the seam actually sharpens overlap.
static bool fake_disjoint(void *ctx, IrValue *i, IrValue *j){ (void)ctx; return i && j && i->id != j->id; }

int main(void){
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*256);
    IrType *i32=ir_type_int(&A,32,true);
    IrType *st=ir_type_new(&A,IRT_STRUCT); st->n_fields=2;
    st->fields=arena_push_many_aligned(&A,IrType*,2); st->fields[0]=i32; st->fields[1]=i32;
    IrType *arr=ir_type_new(&A,IRT_ARRAY); arr->elem=i32; arr->array_len=8;

    IrFunc *f=ir_func_new(&A,nm("t"),ir_type_new(&A,IRT_UNIT),IR_FUNC_PROC);
    IrBlock *e=f->entry;
    IrValue *x = ir_alloca(f,e,st);          // local struct x
    IrValue *y = ir_alloca(f,e,st);          // a DIFFERENT local struct y
    IrValue *a = ir_alloca(f,e,arr);         // local array a
    IrValue *xa = ir_field_ptr(f,e,x,0,i32); // x.f0
    IrValue *xb = ir_field_ptr(f,e,x,1,i32); // x.f1
    IrValue *xa2= ir_field_ptr(f,e,x,0,i32); // x.f0 again (distinct SSA value, same place)
    IrValue *i  = ir_const_int(f,e,0,i32);
    IrValue *j  = ir_const_int(f,e,1,i32);
    IrValue *ai = ir_elem_ptr(f,e,a,i,i32);  // a[i]
    IrValue *aj = ir_elem_ptr(f,e,a,j,i32);  // a[j]
    IrValue *p  = ir_add_param(f, ir_type_new(&A,IRT_PTR), nm("p"));
    ir_set_ret(e,NULL);

    int nvar; IrInstr **def = mkdef(f,&nvar);
    #define P(v) ir_place_of(def,nvar,(v))
    IrPlace px=P(x), py=P(y), pxa=P(xa), pxb=P(xb), pxa2=P(xa2), pai=P(ai), paj=P(aj), pp=P(p);

    pexpect("x vs x                        (same local)",        ir_place_overlaps(&px,&px),   true);
    pexpect("x vs y                        (distinct locals)",   ir_place_overlaps(&px,&py),   false);
    pexpect("x.f0 vs x.f1                  (distinct fields)",   ir_place_overlaps(&pxa,&pxb), false);
    pexpect("x.f0 vs x.f0                  (same field, 2 SSA)", ir_place_overlaps(&pxa,&pxa2),true);
    pexpect("x.f0 vs x                     (prefix)",            ir_place_overlaps(&pxa,&px),  true);
    pexpect("x.f0 vs y                     (distinct roots)",    ir_place_overlaps(&pxa,&py),  false);
    pexpect("local vs param                (distinct roots)",    ir_place_overlaps(&px,&pp),   false);

    // Without the numeric bridge, distinct indices must be assumed to alias.
    pexpect("a[i] vs a[j]  WITHOUT numeric bridge (conservative)", ir_place_overlaps(&pai,&paj), true);
    // ★ the beyond-Rust seam: with a disjointness oracle, they separate.
    ir_place_index_disjoint_fn = fake_disjoint;
    pexpect("a[i] vs a[j]  WITH numeric bridge (i != j proven)",   ir_place_overlaps(&pai,&paj), false);
    pexpect("a[i] vs a[i]  WITH numeric bridge (same index)",      ir_place_overlaps(&pai,&pai), true);
    ir_place_index_disjoint_fn = NULL;

    // equality is stricter than overlap
    printf("  %-52s : %s\n", "x.f0 == x.f0 (structural equality)", ir_place_equal(&pxa,&pxa2)?"yes":"NO <<< WRONG");
    if (!ir_place_equal(&pxa,&pxa2)) failures++;
    printf("  %-52s : %s\n", "x.f0 != x.f1", !ir_place_equal(&pxa,&pxb)?"yes":"NO <<< WRONG");
    if (ir_place_equal(&pxa,&pxb)) failures++;

    printf(failures? "PLACE: %d WRONG\n" : "PLACE: all expectations met\n", failures);
    return failures?1:0;
}
