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

    IrType *owned_ptr = ir_type_new(&A,IRT_PTR); owned_ptr->elem=i32; owned_ptr->linear=true;

    // 6) owned-ptr slot, never consumed, function returns  →  leak (E003)
    { IrFunc *f=ir_func_new(&A,nm("leak"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      ir_alloca(f,e,owned_ptr); ir_set_ret(e,NULL);
      lin_expect("owned resource dropped → E003 leak", count(f,3), 1); }

    // 7) owned-ptr slot consumed via mov before return  →  no leak
    { IrFunc *f=ir_func_new(&A,nm("moved"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,owned_ptr); ir_consume(f,e,s); ir_set_ret(e,NULL);
      lin_expect("owned resource moved → no leak", count(f,3), 0); }

    // 8) a plain (non-linear) i32 slot dropped  →  NOT a leak
    { IrFunc *f=ir_func_new(&A,nm("noleak"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
      ir_alloca(f,e,i32); ir_set_ret(e,NULL);
      lin_expect("non-linear drop is fine", count(f,3), 0); }

    // MOVE-ON-ASSIGN: `var q = p` on a LINEAR slot transfers ownership. Without this,
    // `var q = p; free(p); free(q)` is an invisible double free (a real P0 hole).
    { IrType *lp = ir_type_new(&A,IRT_PTR); lp->elem=i32; lp->linear = true;
      // p = linear; q = p;  then USE p  →  E001 use-after-move
      { IrFunc *f=ir_func_new(&A,nm("moveassign"),i32,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *p=ir_alloca(f,e,lp), *q=ir_alloca(f,e,lp);
        ir_store(f,e,q, ir_load(f,e,p,lp));            // var q = p   ⇒ moves p
        ir_load(f,e,p,lp);                             // read p again
        ir_set_ret(e,NULL);
        lin_expect("var q = p then use p fires E001", count(f,1), 1); }
      // p = linear; q = p;  and p NOT touched again  →  no use-after-move
      { IrFunc *f=ir_func_new(&A,nm("moveonce"),i32,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *p=ir_alloca(f,e,lp), *q=ir_alloca(f,e,lp);
        ir_store(f,e,q, ir_load(f,e,p,lp));
        ir_set_ret(e,NULL);
        lin_expect("var q = p alone is not a use", count(f,1), 0); }
      // `var q = mov p` — load;consume;store. The explicit consume must NOT be double-counted
      // by move-on-assign into a spurious E002.
      { IrFunc *f=ir_func_new(&A,nm("movexplicit"),i32,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *p=ir_alloca(f,e,lp), *q=ir_alloca(f,e,lp);
        IrValue *v=ir_load(f,e,p,lp);
        ir_consume(f,e,p);                             // the explicit `mov`
        ir_store(f,e,q,v);
        ir_set_ret(e,NULL);
        lin_expect("explicit mov is not double-counted", count(f,2), 0); }
      // a NON-linear slot copies freely
      { IrFunc *f=ir_func_new(&A,nm("copyok"),i32,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *p=ir_alloca(f,e,i32), *q=ir_alloca(f,e,i32);
        ir_store(f,e,q, ir_load(f,e,p,i32));
        ir_load(f,e,p,i32);
        ir_set_ret(e,NULL);
        lin_expect("non-linear copy is not a move", count(f,1), 0); }
    }

    // OWNERSHIP vs LINEARITY (IrValue.owns). The same linear type appears in an OWNING and a
    // BORROWING binding; only the owner can leak it. Keying the leak check on the type alone
    // reported a leak in every shared-borrow callee.
    { IrType *lp = ir_type_new(&A,IRT_PTR); lp->elem=i32; lp->linear=true;
      // a struct that OWNS a linear pointer field, never consumed  →  E003
      IrType *res = ir_type_new(&A,IRT_STRUCT); res->n_fields=1; res->linear=true;
      res->fields=arena_push_many_aligned(&A,IrType*,1); res->fields[0]=lp;
      { IrFunc *f=ir_func_new(&A,nm("structleak"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        ir_alloca(f,e,res); ir_set_ret(e,NULL);
        lin_expect("struct owning a resource leaks", count(f,3), 1); }
      // the SAME type in a BORROWED binding  →  not a leak (the callee owes nothing)
      { IrFunc *f=ir_func_new(&A,nm("structborrow"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *s=ir_alloca(f,e,res); s->owns = false;      // a shared-borrow param home slot
        ir_set_ret(e,NULL);
        lin_expect("borrowed binding never leaks", count(f,3), 0); }
      // an OWNED linear struct with NOTHING releasable  →  not a leak (no obligation)
      { IrType *plain = ir_type_new(&A,IRT_STRUCT); plain->n_fields=1; plain->linear=true;
        plain->fields=arena_push_many_aligned(&A,IrType*,1); plain->fields[0]=i32;
        IrFunc *f=ir_func_new(&A,nm("plainowned"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        ir_alloca(f,e,plain); ir_set_ret(e,NULL);
        lin_expect("owned struct with no resource is fine", count(f,3), 0); }
    }

    // PER-FIELD linear state + the opaque fail-closed rule.
    { IrType *lp = ir_type_new(&A,IRT_PTR); lp->elem=i32; lp->linear=true;
      IrType *res = ir_type_new(&A,IRT_STRUCT); res->n_fields=2; res->linear=true;
      res->fields=arena_push_many_aligned(&A,IrType*,2); res->fields[0]=lp; res->fields[1]=i32;
      // consuming the one LINEAR FIELD discharges the struct — `return mov r.handle`
      { IrFunc *f=ir_func_new(&A,nm("fieldconsume"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *s=ir_alloca(f,e,res);
        ir_consume(f,e, ir_field_ptr(f,e,s,0,lp));       // mov r.<linear field>
        ir_set_ret(e,NULL);
        lin_expect("consuming the linear field discharges", count(f,3), 0); }
      // consuming only the NON-linear field does not
      { IrFunc *f=ir_func_new(&A,nm("wrongfield"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *s=ir_alloca(f,e,res);
        ir_consume(f,e, ir_field_ptr(f,e,s,1,i32));
        ir_set_ret(e,NULL);
        lin_expect("consuming a plain field does not", count(f,3), 1); }
      // consuming the SAME field twice is a double move
      { IrFunc *f=ir_func_new(&A,nm("twicefield"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *s=ir_alloca(f,e,res);
        ir_consume(f,e, ir_field_ptr(f,e,s,0,lp));
        ir_consume(f,e, ir_field_ptr(f,e,s,0,lp));
        ir_set_ret(e,NULL);
        lin_expect("consuming one field twice is E002", count(f,2), 1); }
      // distinct fields are independent — no double move
      { IrFunc *f=ir_func_new(&A,nm("twofields"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *s=ir_alloca(f,e,res);
        ir_consume(f,e, ir_field_ptr(f,e,s,0,lp));
        ir_consume(f,e, ir_field_ptr(f,e,s,1,i32));
        ir_set_ret(e,NULL);
        lin_expect("distinct fields are independent", count(f,2), 0); }
      // an OPAQUE linear struct (a cross-module type: no visible fields) fails CLOSED
      { IrType *op = ir_type_new(&A,IRT_STRUCT); op->n_fields=0; op->linear=true;
        IrFunc *f=ir_func_new(&A,nm("opaqueleak"),unit,IR_FUNC_PROC); IrBlock *e=f->entry;
        ir_alloca(f,e,op); ir_set_ret(e,NULL);
        lin_expect("opaque linear struct leaks (fail closed)", count(f,3), 1); }
    }

    printf(failures? "LINEARITY: %d WRONG\n" : "LINEARITY: all expectations met\n", failures);
    return failures?1:0;
}
