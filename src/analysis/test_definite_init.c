// test_definite_init.c — the definite-assignment pass on hand-built IR (sovereign, no AST).
// The pass had no unit test at all, and its depth limit was a FAIL-OPEN: a nested write
// (`p.a.y = 1`) marked the whole parent field initialised, so reading the still-unset
// `p.a.x` reported nothing. These pin both directions of the depth-2 model — it must fire on
// an unset leaf and stay silent once every leaf is set.
//   gcc -std=c99 -o /tmp/test_di src/analysis/test_definite_init.c -I src && /tmp/test_di
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/definite_init.h"
#include <stdio.h>

static Arena A;
static IrName *nm(const char *s){ return ir_intern(&A, s, (isize)strlen(s)); }
static int failures=0;
static int ncode(IrFunc *f, int code){
    Di *D=di_analyze(f); int c=0;
    for (int i=0;i<D->nfinds;i++) if (D->finds[i].code==code) c++;
    di_free(D); return c;
}
static void di_expect(const char *what, int got, int want){
    printf("  %-48s : %d (expected %d)%s\n", what, got, want, got==want?"":"   <<< WRONG");
    if (got!=want) failures++;
}

int main(void){
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*256);
    IrType *i32=ir_type_int(&A,32,true), *unit=ir_type_new(&A,IRT_UNIT);
    // Inner { x, y }   Outer { a: Inner, b: i32 }
    IrType *inner=ir_type_new(&A,IRT_STRUCT); inner->n_fields=2;
    inner->fields=arena_push_many_aligned(&A,IrType*,2); inner->fields[0]=i32; inner->fields[1]=i32;
    IrType *outer=ir_type_new(&A,IRT_STRUCT); outer->n_fields=2;
    outer->fields=arena_push_many_aligned(&A,IrType*,2); outer->fields[0]=inner; outer->fields[1]=i32;

    // 1) plain scalar: read before any store  →  E005
    { IrFunc *f=ir_func_new(&A,nm("bare"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32); ir_load(f,e,s,i32); ir_set_ret(e,NULL);
      di_expect("uninitialised scalar read fires E005", ncode(f,5), 1); }

    // 2) stored first  →  clean
    { IrFunc *f=ir_func_new(&A,nm("init"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32);
      ir_store(f,e,s,ir_const_int(f,e,1,i32)); ir_load(f,e,s,i32); ir_set_ret(e,NULL);
      di_expect("initialised scalar read is clean", ncode(f,5), 0); }

    // 3) ★ THE FAIL-OPEN: p.a.y = 1 then read p.a.x  →  E005 (the leaf is still unset)
    { IrFunc *f=ir_func_new(&A,nm("nested"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *p=ir_alloca(f,e,outer);
      IrValue *a=ir_field_ptr(f,e,p,0,inner);
      ir_store(f,e, ir_field_ptr(f,e,a,1,i32), ir_const_int(f,e,1,i32));   // p.a.y = 1
      IrValue *a2=ir_field_ptr(f,e,p,0,inner);
      ir_load(f,e, ir_field_ptr(f,e,a2,0,i32), i32);                       // read p.a.x
      ir_set_ret(e,NULL);
      di_expect("nested sibling write does NOT initialise p.a.x", ncode(f,5), 1); }

    // 4) both leaves written  →  reading either is clean (no over-rejection)
    { IrFunc *f=ir_func_new(&A,nm("nestedfull"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *p=ir_alloca(f,e,outer);
      IrValue *a=ir_field_ptr(f,e,p,0,inner);
      ir_store(f,e, ir_field_ptr(f,e,a,0,i32), ir_const_int(f,e,1,i32));   // p.a.x = 1
      ir_store(f,e, ir_field_ptr(f,e,a,1,i32), ir_const_int(f,e,2,i32));   // p.a.y = 2
      IrValue *a2=ir_field_ptr(f,e,p,0,inner);
      ir_load(f,e, ir_field_ptr(f,e,a2,0,i32), i32);
      ir_set_ret(e,NULL);
      di_expect("both leaves written ⇒ clean", ncode(f,5)+ncode(f,19), 0); }

    // 5) reading the PARENT whole with one leaf unset  →  E019 (partially initialised)
    { IrFunc *f=ir_func_new(&A,nm("partial"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *p=ir_alloca(f,e,outer);
      IrValue *a=ir_field_ptr(f,e,p,0,inner);
      ir_store(f,e, ir_field_ptr(f,e,a,0,i32), ir_const_int(f,e,1,i32));   // only p.a.x
      ir_load(f,e, ir_field_ptr(f,e,p,0,inner), inner);                    // read p.a whole
      ir_set_ret(e,NULL);
      di_expect("partial parent read fires E019", ncode(f,19), 1); }

    // 6) a write on ONE branch only  →  the merge is an INTERSECTION, so the read is unsafe
    { IrFunc *f=ir_func_new(&A,nm("branch"),unit,IR_FUNC_PROC);
      IrBlock *e=f->entry, *t=ir_new_block(f), *j=ir_new_block(f);
      IrValue *s=ir_alloca(f,e,i32);
      ir_set_br_cond(e, ir_const_int(f,e,1,ir_type_bool(&A)), t, j);
      ir_store(f,t,s,ir_const_int(f,t,1,i32)); ir_set_br(t,j);
      ir_load(f,j,s,i32); ir_set_ret(j,NULL);
      di_expect("initialised on one branch only fires E005", ncode(f,5), 1); }

    printf(failures? "DEFINITE-INIT: %d WRONG\n" : "DEFINITE-INIT: all expectations met\n", failures);
    return failures?1:0;
}
