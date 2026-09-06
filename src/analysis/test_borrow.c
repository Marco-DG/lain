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
static IrType *arr_i32_4(void){ IrType *t=ir_type_new(&A,IRT_ARRAY); t->elem=ir_type_int(&A,32,true); t->array_len=4; return t; }
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

    // 6b) provenance through a LOAD from a singly-stored slot. `var local = <a local buffer>;
    // return S(local)` loads the value out of local's slot before it enters the struct, and
    // stopping at the load reported nothing. Following the unique store recovers it — while a
    // slot written from a PARAMETER still roots at the param, so no false positive.
    { IrType *st=ir_type_new(&A,IRT_STRUCT); st->n_fields=1;
      st->fields=arena_push_many_aligned(&A,IrType*,1); st->fields[0]=pi32;
      { IrFunc *f=ir_func_new(&A,nm("viaslot"),st,IR_FUNC_PROC); IrBlock *e=f->entry;
        IrValue *buf=ir_alloca(f,e,i32);                    // the escaping local
        IrValue *cell=ir_alloca(f,e,pi32);                  // `var local = &buf`
        ir_store(f,e,cell,buf);
        IrValue **fs=arena_push_many_aligned(&A,IrValue*,1); fs[0]=ir_load(f,e,cell,pi32);
        ir_set_ret(e, ir_struct_new(f,e,st,fs,1));
        bexpect("escape through a loaded slot fires E010", nfind(f), 1); }
      { IrFunc *f=ir_func_new(&A,nm("viaslotparam"),st,IR_FUNC_PROC);
        IrValue *p=ir_add_param(f,pi32,nm("p")); IrBlock *e=f->entry;
        IrValue *cell=ir_alloca(f,e,pi32);
        ir_store(f,e,cell,p);                                // the slot holds a PARAM pointer
        IrValue **fs=arena_push_many_aligned(&A,IrValue*,1); fs[0]=ir_load(f,e,cell,pi32);
        ir_set_ret(e, ir_struct_new(f,e,st,fs,1));
        bexpect("loaded slot holding a param is fine", nfind(f), 0); }
    }

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
        ir_set_br_cond(f->entry, ir_const_int(f,f->entry,1,i32), t, e2);
        ir_set_ret(t, pa); ir_set_ret(e2, pb);
        bexpect("ret-borrow mask unions both branches", (int)bor_ret_borrow_mask(f), 3); }
      // an OPAQUE return (provenance laundered through a load) ⇒ fall back to ALL ref params
      { IrFunc *f=ir_func_new(&A,nm("opaque"),pi32,IR_FUNC_PURE);
        IrValue *pa=ir_add_param(f,pty,nm("a")); ir_add_param(f,pty,nm("b"));
        f->ret_borrows = true;
        ir_set_ret(f->entry, ir_load(f,f->entry,pa,pi32));
        bexpect("opaque ret-borrow falls back to all ref params", (int)bor_ret_borrow_mask(f), 3); }
    }

    // 8) ★ PHASE D — numeric index disjointness. Rust cannot distinguish a[i] from a[j] at
    // all (both are the place `a[_]`), which is why split_at_mut needs `unsafe`. Here the
    // octagon answers it. The DEFAULT must stay conservative — that is what catches passing
    // the same element twice to two `restrict` parameters, a demonstrated miscompile in the
    // old engine — and the numeric bridge may only ever REMOVE a conflict it can prove away.
    { IrType *pi = ir_type_new(&A,IRT_PTR); pi->elem=i32; pi->ptr_mut = true;
      IrType *u64t = ir_type_int(&A,64,false);
      // callee: swap2(var x i32, var y i32)
      IrFunc *callee = ir_func_new(&A,nm("swap2"),ir_type_new(&A,IRT_UNIT),IR_FUNC_PROC);
      ir_add_param(callee, pi, nm("x")); ir_add_param(callee, pi, nm("y"));
      ir_set_ret(callee->entry, NULL); ir_finalize_cfg(callee);

      // caller: a[4]; f(&a[I], &a[J]) for various I, J
      // mode 0 = same index VALUE, 1 = two constants 1 and 2, 2 = the same constant twice,
      // 3 = two unconstrained params.
      const char *what[5] = {
          "phase D: a[i] vs a[i] (same value) CONFLICTS",
          "phase D: a[1] vs a[2] (distinct consts) is allowed",
          "phase D: a[3] vs a[3] (same const) CONFLICTS",
          "phase D: a[p] vs a[q] (unknown) CONFLICTS (conservative)",
          "phase D: a[p] vs a[q] under p<q is ALLOWED  <-- beyond Rust",
      };
      int want[5] = {1, 0, 1, 1, 0};
      for (int mode=0; mode<5; mode++) {
          IrFunc *f=ir_func_new(&A,nm("caller"),ir_type_new(&A,IRT_UNIT),IR_FUNC_PROC);
          IrValue *p0=NULL,*p1=NULL;
          if (mode>=3) { p0=ir_add_param(f,u64t,nm("p")); p1=ir_add_param(f,u64t,nm("q")); }
          IrBlock *e=f->entry;
          IrValue *a=ir_alloca_array(f,e,arr_i32_4());
          IrValue *ix, *jx;
          if (mode==0)      { ix = ir_const_int(f,e,1,u64t); jx = ix; }
          else if (mode==1) { ix = ir_const_int(f,e,1,u64t); jx = ir_const_int(f,e,2,u64t); }
          else if (mode==2) { ix = ir_const_int(f,e,3,u64t); jx = ir_const_int(f,e,3,u64t); }
          else              { ix = p0; jx = p1; }
          // mode 4: the RELATIONAL case — `p < q` known, so the two elements are distinct
          // even though neither index has a value. This is the one Rust cannot express:
          // its borrow checker has no numeric domain to ask.
          if (mode==4) ir_assume(f, e, ir_icmp(f,e,IR_CMP_ULT,p0,p1));
          IrValue *ea = ir_elem_ptr(f,e,a,ix,i32), *eb = ir_elem_ptr(f,e,a,jx,i32);
          IrInstr *call = ir_instr(f, IR_CALL, NULL, 2);
          call->operands[0]=ea; call->operands[1]=eb;
          call->aux.callee = nm("swap2");
          ir_emit(e, call);
          ir_set_ret(e,NULL); ir_finalize_cfg(f);
          f->next = callee;                                  // a 2-function module
          Borrow *B = borrow_analyze_mod(f, f);
          int n=0; for (int k=0;k<B->nfinds;k++) if (B->finds[k].code==4) n++;
          borrow_free(B);
          bexpect(what[mode], n, want[mode]);
      }
    }

    printf(failures? "BORROW: %d WRONG\n" : "BORROW: all expectations met\n", failures);
    return failures?1:0;
}
