// fuzz_vra.c — SOUNDNESS fuzzer for the octagon VRA. Generates random counted-loop
// index programs as IR (bypassing the frontend, so we can make UNSAFE ones the old
// sema would exit() on), then cross-checks the abstract verdict against ground truth
// from a concrete interpreter of the very same IR:
//
//     the yardstick — if the VRA says an index site is PROVEN check-free, then NO
//     concrete execution of that site may go out of bounds. A single counterexample
//     is an unsoundness in a transfer function.
//
//   gcc -std=c99 -O2 -o /tmp/fuzz_vra src/analysis/fuzz_vra.c -I src && /tmp/fuzz_vra
#include "utils/common/def.h"
#include "utils/arena.h"
#include "ast.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/vra.h"
#include <stdio.h>

static Arena A;
static Id *nm(const char *s){ Id *i=arena_push_aligned(&A,Id); i->name=(char*)s; i->length=(isize)strlen(s); return i; }
static IrType *arr_i(int n){ IrType *t=ir_type_new(&A,IRT_ARRAY); t->elem=ir_type_int(&A,32,true); t->array_len=n; return t; }

// ── the generated shape ──────────────────────────────────────────────────────
//   var a i32[N]; var i = INIT; while i <CMP> BOUND { a[ IDX(i) ]; i = i + STEP }
// IDX(i) ∈ { i, i+C, i-C }.  All constants small; i stays well within int64.
typedef struct { int N, init, bound, step, idx_kind, idx_c; IrCmp cmp; IrInstr *ep; } Spec;

static IrFunc *gen(Spec *s) {
    IrFunc *f = ir_func_new(&A, nm("k"), ir_type_int(&A,32,true), IR_FUNC_PROC);
    IrType *i32=ir_type_int(&A,32,true);
    IrBlock *e=f->entry,*head=ir_new_block(f),*body=ir_new_block(f),*ex=ir_new_block(f);
    IrValue *a=ir_alloca_array(f,e,arr_i(s->N));
    IrValue *islot=ir_alloca(f,e,i32);
    ir_store(f,e,islot,ir_const_int(f,e,s->init,i32));
    ir_set_br(e,head);
    IrValue *iv=ir_load(f,head,islot,i32);
    IrValue *cmp=ir_icmp(f,head,s->cmp,iv,ir_const_int(f,head,s->bound,i32));
    ir_set_br_cond(head,cmp,body,ex);
    IrValue *iv2=ir_load(f,body,islot,i32), *idx=iv2;
    if (s->idx_kind==1) idx=ir_binop(f,body,IR_ADD,iv2,ir_const_int(f,body,s->idx_c,i32),i32);
    else if (s->idx_kind==2) idx=ir_binop(f,body,IR_SUB,iv2,ir_const_int(f,body,s->idx_c,i32),i32);
    else if (s->idx_kind==3) idx=ir_binop(f,body,IR_AND,iv2,ir_const_int(f,body,s->idx_c,i32),i32);
    else if (s->idx_kind==4) idx=ir_binop(f,body,IR_UREM,iv2,ir_const_int(f,body,s->idx_c?s->idx_c:1,i32),i32);
    ir_elem_ptr(f,body,a,idx,i32);
    s->ep = body->instrs_tail;                 // the elem_ptr INSTRUCTION we just emitted
    IrValue *iv3=ir_load(f,body,islot,i32);
    ir_store(f,body,islot,ir_binop(f,body,IR_ADD,iv3,ir_const_int(f,body,s->step,i32),i32));
    ir_set_br(body,head);
    ir_set_ret(ex,NULL); ir_finalize_cfg(f);
    return f;
}

// ── concrete interpreter of the SAME IR (ground truth) ───────────────────────
// Returns true iff, over the real execution, the site `ep` never went out of
// bounds of its array. Deterministic program ⇒ one execution covering all iters.
static bool concrete_inbounds(IrFunc *f, IrInstr *ep) {
    int64_t *val=calloc(f->next_value_id,sizeof(int64_t));
    int64_t *cell=calloc(f->next_value_id,sizeof(int64_t));   // alloca id → content
    IrInstr **def=calloc(f->next_value_id,sizeof(IrInstr*));
    for (IrBlock *b=f->blocks;b;b=b->next) for (IrInstr *i=b->instrs;i;i=i->next) if(i->result) def[i->result->id]=i;
    bool ok=true;
    IrBlock *b=f->entry; int steps=0;
    while (b && steps++<2000000) {
        for (IrInstr *i=b->instrs;i;i=i->next) {
            int r = i->result?i->result->id:-1;
            switch (i->op) {
                case IR_CONST: val[r]=i->aux.imm; break;
                case IR_ALLOCA: break;   // the id doubles as its cell (below)
                case IR_LOAD:   val[r]=cell[i->operands[0]->id]; break;
                case IR_STORE:  cell[i->operands[0]->id]=val[i->operands[1]->id]; break;
                case IR_ADD: val[r]=val[i->operands[0]->id]+val[i->operands[1]->id]; break;
                case IR_SUB: val[r]=val[i->operands[0]->id]-val[i->operands[1]->id]; break;
                case IR_MUL: val[r]=val[i->operands[0]->id]*val[i->operands[1]->id]; break;
                case IR_AND: val[r]=val[i->operands[0]->id]&val[i->operands[1]->id]; break;
                case IR_UREM:{ int64_t d=val[i->operands[1]->id]; val[r]= d? (int64_t)((uint64_t)val[i->operands[0]->id]%(uint64_t)d):0; } break;
                case IR_SREM:{ int64_t d=val[i->operands[1]->id]; val[r]= d? val[i->operands[0]->id]%d:0; } break;
                case IR_ICMP: {
                    int64_t x=val[i->operands[0]->id],y=val[i->operands[1]->id]; int64_t rr=0;
                    switch(i->aux.cmp){ case IR_CMP_EQ:rr=x==y;break; case IR_CMP_NE:rr=x!=y;break;
                        case IR_CMP_SLT:case IR_CMP_ULT:rr=x<y;break; case IR_CMP_SLE:case IR_CMP_ULE:rr=x<=y;break;
                        case IR_CMP_SGT:case IR_CMP_UGT:rr=x>y;break; case IR_CMP_SGE:case IR_CMP_UGE:rr=x>=y;break; }
                    val[r]=rr; break;
                }
                case IR_ELEM_PTR: {
                    IrInstr *bd=def[i->operands[0]->id];
                    if (bd && bd->op==IR_ALLOCA && bd->aux.alloca_ty && bd->aux.alloca_ty->kind==IRT_ARRAY) {
                        int64_t ix=val[i->operands[1]->id], N=bd->aux.alloca_ty->array_len;
                        if (i==ep && (ix<0 || ix>=N)) ok=false;   // the site under test escaped
                    }
                    break;
                }
                default: break;
            }
        }
        switch (b->term.kind) {
            case IR_TERM_BR: b=b->term.a; break;
            case IR_TERM_BR_COND: b = val[b->term.cond->id]? b->term.a : b->term.b; break;
            default: b=NULL; break;
        }
    }
    free(val); free(cell); free(def);
    return ok;
}

int main(void) {
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    srand(20260828);
    int trials=200000, unsound=0, proven=0, safe=0, proven_and_safe=0;
    for (int t=0;t<trials;t++) {
        // fresh arena per batch to bound memory
        if ((t & 0x3ff)==0){ /* periodic reset */ A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096); }
        Spec s;
        s.N = 1+rand()%12;
        s.init = rand()%4;
        s.bound = rand()%(s.N+3);
        s.step = 1+rand()%3;
        s.idx_kind = rand()%5;          // i, i+c, i-c, i&c, i%c
        s.idx_c = rand()%16;
        s.cmp = (rand()%2)?IR_CMP_SLT:IR_CMP_SLE;
        IrFunc *f=gen(&s);
        bool csafe=concrete_inbounds(f, s.ep);
        Vra *V=vra_analyze(f);
        bool vproven=false; for(int i=0;i<V->nchecks;i++) if(V->checks[i].kind==VRA_BOUNDS && V->checks[i].at==s.ep) vproven=V->checks[i].ok;
        vra_free(V);
        if (csafe) safe++;
        if (vproven) proven++;
        if (vproven && csafe) proven_and_safe++;
        if (vproven && !csafe) {   // the fatal case: proved safe, actually escapes
            unsound++;
            if (unsound<=5) printf("UNSOUND: N=%d init=%d bound=%d step=%d idx=%d+%d cmp=%s — VRA proved, concrete OOB\n",
                s.N,s.init,s.bound,s.step,s.idx_kind,s.idx_c, s.cmp==IR_CMP_SLT?"<":"<=");
        }
    }
    printf("bounds: %d trials | concrete-safe %d | VRA-proven %d (all also safe: %d) | UNSOUND %d\n",
           trials, safe, proven, proven_and_safe, unsound);

    // ── phase 2: overflow-range soundness ────────────────────────────────────
    // vra_arith_range must OVER-approximate { a OP b : a∈[alo,ahi], b∈[blo,bhi] },
    // so the overflow fit-check can never call an escaping op "in range".
    int ov_trials=300000, ov_bad=0;
    for (int t=0;t<ov_trials;t++) {
        int alo=(rand()%41)-20, ahi=alo+rand()%21;
        int blo=(rand()%41)-20, bhi=blo+rand()%21;
        IrOp op = (IrOp)(IR_ADD + rand()%3);   // ADD, SUB, MUL are consecutive
        __int128 rlo,rhi; vra_arith_range(op, alo,ahi, blo,bhi, &rlo,&rhi);
        __int128 tmin=(__int128)1<<100, tmax=-((__int128)1<<100);
        for (int a=alo;a<=ahi;a++) for (int b=blo;b<=bhi;b++) {
            __int128 v = op==IR_ADD? (__int128)a+b : op==IR_SUB? (__int128)a-b : (__int128)a*b;
            if (v<tmin) tmin=v; if (v>tmax) tmax=v;
        }
        if (!(rlo<=tmin && rhi>=tmax)) {   // abstract range failed to contain the real one
            ov_bad++;
            if (ov_bad<=5) printf("OV-UNSOUND: op=%d a∈[%d,%d] b∈[%d,%d] abstract=[%lld,%lld] real=[%lld,%lld]\n",
                op,alo,ahi,blo,bhi,(long long)rlo,(long long)rhi,(long long)tmin,(long long)tmax);
        }
    }
    printf("overflow-range: %d trials | not-over-approximating: %d\n", ov_trials, ov_bad);

    int bad = unsound + ov_bad + (proven!=proven_and_safe);
    printf(bad==0 ? "\nSOUND: every VRA proof matched concrete ground truth.\n"
                  : "\n*** UNSOUNDNESS DETECTED ***\n");
    return bad?1:0;
}
