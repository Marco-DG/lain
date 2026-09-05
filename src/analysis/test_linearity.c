// test_linearity.c — the linearity pass on hand-built IR (sovereign, no AST). Hand-built so
// we can feed the exact move patterns the old sema would exit() on. A checker that MISSES a
// use-after-move is worse than none, so the positives (must fire) matter as much as the
// negatives (must stay quiet after a re-init).
//   gcc -std=c99 -o /tmp/test_lin src/analysis/test_linearity.c -I src && /tmp/test_lin
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/linearity.h"
#include <stdio.h>

static Arena A;
static IrName *nm(const char *s){ return ir_intern(&A, s, (isize)strlen(s)); }
static int failures=0;

// count findings of a given code (1=E001, 2=E002) after analysing f.
static int count(IrFunc *f, int code){
    Lin *L=lin_analyze(f); int c=0;
    for (int i=0;i<L->nfinds;i++) if (L->finds[i].code==code) c++;
    lin_free(L); return c;
}
static void lin_expect(const char *what, int got, int want){
    printf("  %-46s : %d (expected %d)%s\n", what, got, want, got==want?"":"   <<< WRONG");
    if (got!=want) failures++;
}

int main(void){
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*256);
    IrType *i32=ir_type_int(&A,32,true), *unit=ir_type_new(&A,IRT_UNIT);

    // 1) alloca; store 0; mov; load  →  use-after-move (E001)
    { IrFunc *f=ir_func_new(&A,nm("uam"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32);
      ir_store(f,e,s,ir_const_int(f,e,0,i32)); ir_consume(f,e,s); ir_load(f,e,s,i32);
      ir_set_ret(e,NULL);
      lin_expect("use after move fires E001", count(f,1), 1);
      lin_expect("use after move no E002",    count(f,2), 0); }

    // 2) alloca; store; mov; mov  →  double move (E002)
    { IrFunc *f=ir_func_new(&A,nm("dm"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32);
      ir_store(f,e,s,ir_const_int(f,e,0,i32)); ir_consume(f,e,s); ir_consume(f,e,s);
      ir_set_ret(e,NULL);
      lin_expect("double move fires E002", count(f,2), 1); }

    // 3) alloca; store; mov; store (re-init); load  →  NO finding (re-initialised)
    { IrFunc *f=ir_func_new(&A,nm("reinit"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32);
      ir_store(f,e,s,ir_const_int(f,e,0,i32)); ir_consume(f,e,s);
      ir_store(f,e,s,ir_const_int(f,e,9,i32)); ir_load(f,e,s,i32);
      ir_set_ret(e,NULL);
      lin_expect("re-init clears the move", count(f,1)+count(f,2), 0); }

    // 4) if c { mov s }  then  load s  →  maybe-moved use across the join (E001)
    { IrFunc *f=ir_func_new(&A,nm("br"),unit,IR_FUNC_PROC);
      IrBlock *e=f->entry, *t=ir_new_block(f), *j=ir_new_block(f);
      IrValue *s=ir_alloca(f,e,i32); ir_store(f,e,s,ir_const_int(f,e,0,i32));
      ir_set_br_cond(e, ir_const_int(f,e,1,ir_type_bool(&A)), t, j);
      ir_consume(f,t,s); ir_set_br(t,j);
      ir_load(f,j,s,i32); ir_set_ret(j,NULL);
      lin_expect("moved-on-one-branch use fires E001", count(f,1), 1); }

    // 5) never moved: alloca; store; load; load  →  clean
    { IrFunc *f=ir_func_new(&A,nm("clean"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32);
      ir_store(f,e,s,ir_const_int(f,e,0,i32)); ir_load(f,e,s,i32); ir_load(f,e,s,i32);
      ir_set_ret(e,NULL);
      lin_expect("never-moved stays clean", count(f,1)+count(f,2), 0); }

    printf(failures? "LINEARITY: %d WRONG\n" : "LINEARITY: all expectations met\n", failures);
    return failures?1:0;
}
