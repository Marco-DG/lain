// test_borrow.c — the borrow/escape pass on hand-built IR (sovereign, no AST). Proves the
// dangling-reference core FIRES on an escaping local and stays QUIET on a returned param or
// heap pointer. A borrow checker that misses a dangling return is worse than none.
//   gcc -std=c99 -o /tmp/test_bor src/analysis/test_borrow.c -I src && /tmp/test_bor
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/borrow.h"
#include <stdio.h>

static Arena A;
static IrName *nm(const char *s){ return ir_intern(&A, s, (isize)strlen(s)); }
static int failures=0;
static int nfind(IrFunc *f){ Borrow *B=borrow_analyze(f); int n=B->nfinds; borrow_free(B); return n; }
static void bexpect(const char *what, int got, int want){
    printf("  %-48s : %d (expected %d)%s\n", what, got, want, got==want?"":"   <<< WRONG");
    if (got!=want) failures++;
}

int main(void){
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*256);
    IrType *i32=ir_type_int(&A,32,true);
    IrType *pi32=ir_type_new(&A,IRT_PTR); pi32->elem=i32;

    // 1) `return &local`  →  dangling (root is a local alloca)
    { IrFunc *f=ir_func_new(&A,nm("dang"),pi32,IR_FUNC_PURE); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,i32);
      ir_store(f,e,s,ir_const_int(f,e,42,i32));
      ir_set_ret(e, s);                                  // return the local's address
      bexpect("return &local fires dangling", nfind(f), 1); }

    // 2) return a POINTER PARAM  →  fine (a borrow of the caller's storage)
    { IrFunc *f=ir_func_new(&A,nm("passthru"),pi32,IR_FUNC_PURE);
      IrValue *p=ir_add_param(f, pi32, nm("p")); IrBlock *e=f->entry;
      ir_set_ret(e, p);
      bexpect("return param pointer is fine", nfind(f), 0); }

    // 3) `return &local.field` (field_ptr into a local)  →  dangling
    { IrType *st=ir_type_new(&A,IRT_STRUCT); st->n_fields=1;
      st->fields=arena_push_many_aligned(&A,IrType*,1); st->fields[0]=i32;
      IrFunc *f=ir_func_new(&A,nm("dfield"),pi32,IR_FUNC_PURE); IrBlock *e=f->entry;
      IrValue *s=ir_alloca(f,e,st);
      IrValue *fp=ir_field_ptr(f,e,s,0,i32);
      ir_set_ret(e, fp);
      bexpect("return &local.field fires dangling", nfind(f), 1); }

    // 4) return a pointer loaded from memory (provenance unknown)  →  not flagged (conservative)
    { IrFunc *f=ir_func_new(&A,nm("viaload"),pi32,IR_FUNC_PURE);
      IrValue *pp=ir_add_param(f, ir_type_new(&A,IRT_PTR), nm("pp")); IrBlock *e=f->entry;
      pp->type->elem = pi32;
      IrValue *loaded=ir_load(f,e,pp,pi32);
      ir_set_ret(e, loaded);
      bexpect("return loaded pointer not flagged", nfind(f), 0); }

    // 5) return a struct BY VALUE whose pointer field borrows a local  →  dangling (E010)
    { IrType *st=ir_type_new(&A,IRT_STRUCT); st->n_fields=1;
      st->fields=arena_push_many_aligned(&A,IrType*,1); st->fields[0]=pi32;
      IrFunc *f=ir_func_new(&A,nm("sret"),st,IR_FUNC_PROC); IrBlock *e=f->entry;
      IrValue *loc=ir_alloca(f,e,i32);                 // a local
      IrValue **fs=arena_push_many_aligned(&A,IrValue*,1); fs[0]=loc;   // field = &local
      IrValue *sn=ir_struct_new(f,e,st,fs,1);
      ir_set_ret(e, sn);
      bexpect("return struct borrowing local fires E010", nfind(f), 1); }

    // 6) return a struct whose pointer field is a PARAM  →  fine
    { IrType *st=ir_type_new(&A,IRT_STRUCT); st->n_fields=1;
      st->fields=arena_push_many_aligned(&A,IrType*,1); st->fields[0]=pi32;
      IrFunc *f=ir_func_new(&A,nm("sok"),st,IR_FUNC_PROC);
      IrValue *p=ir_add_param(f, pi32, nm("p")); IrBlock *e=f->entry;
      IrValue **fs=arena_push_many_aligned(&A,IrValue*,1); fs[0]=p;     // field = a param pointer
      IrValue *sn=ir_struct_new(f,e,st,fs,1);
      ir_set_ret(e, sn);
      bexpect("return struct borrowing param is fine", nfind(f), 0); }

    // 7) RET-BORROW SOURCE INFERENCE (design §5). The source of a returned reference must be
    // read off the BODY, not guessed from the signature. `pick(var a, var b) var i32` may
    // return either; the old syntactic rule ("first mutable reference param") mis-attributed
    // `return var b.x` to `a` and silently lost every conflict on `b`. These pin the mask.
    { IrType *pty=ir_type_new(&A,IRT_PTR); pty->elem=i32;
      for (int which=0; which<2; which++) {
          IrFunc *f=ir_func_new(&A,nm("pick"),pi32,IR_FUNC_PURE);
          IrValue *pa=ir_add_param(f,pty,nm("a")), *pb=ir_add_param(f,pty,nm("b"));
          f->ret_borrows = true;
          ir_set_ret(f->entry, which ? pb : pa);
          uint64_t m = bor_ret_borrow_mask(f);
          bexpect(which ? "ret-borrow mask isolates param b" : "ret-borrow mask isolates param a",
                  (int)m, which ? 2 : 1);
      }
      // returning EITHER on different paths ⇒ both bits (a union, still exact)
      { IrFunc *f=ir_func_new(&A,nm("pick2"),pi32,IR_FUNC_PURE);
        IrValue *pa=ir_add_param(f,pty,nm("a")), *pb=ir_add_param(f,pty,nm("b"));
        f->ret_borrows = true;
        IrBlock *t=ir_new_block(f), *e2=ir_new_block(f);
        ir_set_br_cond(f->entry, ir_const_int(f,f->entry,i32,1), t, e2);
        ir_set_ret(t, pa); ir_set_ret(e2, pb);
        bexpect("ret-borrow mask unions both branches", (int)bor_ret_borrow_mask(f), 3); }
      // an OPAQUE return (provenance laundered through a load) ⇒ fall back to ALL ref params
      { IrFunc *f=ir_func_new(&A,nm("opaque"),pi32,IR_FUNC_PURE);
        IrValue *pa=ir_add_param(f,pty,nm("a")); ir_add_param(f,pty,nm("b"));
        f->ret_borrows = true;
        ir_set_ret(f->entry, ir_load(f,f->entry,pa,pi32));
        bexpect("opaque ret-borrow falls back to all ref params", (int)bor_ret_borrow_mask(f), 3); }
    }

    printf(failures? "BORROW: %d WRONG\n" : "BORROW: all expectations met\n", failures);
    return failures?1:0;
}
