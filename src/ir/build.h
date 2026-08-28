// src/ir/build.h — Lain IR construction API (builders + convenience emitters).
//
// Phase 1.1/1.2 of the clean-core rebuild. Defines the prototypes declared in ir.h
// plus small helpers used by the lowering (lower.h) and tests. Everything is arena-
// allocated. Not wired into main.c.
#ifndef LAIN_IR_BUILD_H
#define LAIN_IR_BUILD_H

#include <string.h>
#include <limits.h>
#include "ir.h"

// ── types ────────────────────────────────────────────────────────────────────
static IrType *ir_type_new(Arena *a, IrTypeKind k) {
    IrType *t = arena_push_aligned(a, IrType);
    memset(t, 0, sizeof *t);
    t->kind = k;
    return t;
}
IrType *ir_type_int(Arena *a, int bits, bool is_signed) {
    IrType *t = ir_type_new(a, IRT_INT);
    t->bits = bits; t->is_signed = is_signed;
    return t;
}
IrType *ir_type_bool(Arena *a) { IrType *t = ir_type_new(a, IRT_BOOL); t->bits = 1; return t; }

bool irtype_int_range(const IrType *t, int64_t *lo, int64_t *hi) {
    if (!t) return false;
    if (t->kind == IRT_BOOL) { *lo = 0; *hi = 1; return true; }
    if (t->kind != IRT_INT)  return false;
    int b = t->bits;
    if (t->is_signed) {
        if (b >= 64) { *lo = INT64_MIN;             *hi = INT64_MAX; }
        else         { *lo = -(1LL << (b - 1));     *hi = (1LL << (b - 1)) - 1; }
    } else {
        *lo = 0;
        *hi = (b >= 64) ? INT64_MAX : (1LL << b) - 1;   // domain is i64; u64 clamps to i64 max
    }
    return true;
}

// ── values, blocks, instructions ─────────────────────────────────────────────
IrValue *ir_new_value(IrFunc *f, IrType *t) {
    IrValue *v = arena_push_aligned(f->arena, IrValue);
    memset(v, 0, sizeof *v);
    v->id = f->next_value_id++;
    v->type = t;
    return v;
}
IrBlock *ir_new_block(IrFunc *f) {
    IrBlock *b = arena_push_aligned(f->arena, IrBlock);
    memset(b, 0, sizeof *b);
    b->id = f->next_block_id++;
    if (!f->blocks) f->blocks = f->blocks_tail = b;
    else { f->blocks_tail->next = b; f->blocks_tail = b; }
    return b;
}
IrInstr *ir_emit(IrBlock *b, IrInstr *ins) {
    ins->next = NULL;
    if (!b->instrs) b->instrs = b->instrs_tail = ins;
    else { b->instrs_tail->next = ins; b->instrs_tail = ins; }
    return ins;
}

// allocate an instruction with `nops` operand slots; a result value of type `rt`
// (NULL ⇒ no result). Does NOT append — caller appends via ir_emit.
static IrInstr *ir_instr(IrFunc *f, IrOp op, IrType *rt, int nops) {
    IrInstr *ins = arena_push_aligned(f->arena, IrInstr);
    memset(ins, 0, sizeof *ins);
    ins->op   = op;
    ins->wrap = IR_WRAP_CHECK;
    ins->n_operands = nops;
    if (nops > 0) ins->operands = arena_push_many_aligned(f->arena, IrValue *, nops);
    if (rt) ins->result = ir_new_value(f, rt);
    return ins;
}

// ── convenience emitters (build value-producing instrs into block b) ─────────
IrValue *ir_const_int(IrFunc *f, IrBlock *b, int64_t v, IrType *t) {
    IrInstr *ins = ir_instr(f, IR_CONST, t, 0);
    ins->aux.imm = v;
    ir_emit(b, ins);
    return ins->result;
}
IrValue *ir_binop(IrFunc *f, IrBlock *b, IrOp op, IrValue *x, IrValue *y, IrType *t) {
    IrInstr *ins = ir_instr(f, op, t, 2);
    ins->operands[0] = x; ins->operands[1] = y;
    ir_emit(b, ins);
    return ins->result;
}
IrValue *ir_icmp(IrFunc *f, IrBlock *b, IrCmp c, IrValue *x, IrValue *y) {
    IrInstr *ins = ir_instr(f, IR_ICMP, ir_type_bool(f->arena), 2);
    ins->aux.cmp = c; ins->operands[0] = x; ins->operands[1] = y;
    ir_emit(b, ins);
    return ins->result;
}
IrValue *ir_alloca(IrFunc *f, IrBlock *b, IrType *slot_ty) {
    IrType *pt = ir_type_new(f->arena, IRT_PTR); pt->elem = slot_ty; pt->ptr_mut = true;
    IrInstr *ins = ir_instr(f, IR_ALLOCA, pt, 0);
    ins->aux.alloca_ty = slot_ty;
    ir_emit(b, ins);
    return ins->result;
}
IrValue *ir_load(IrFunc *f, IrBlock *b, IrValue *addr, IrType *t) {
    IrInstr *ins = ir_instr(f, IR_LOAD, t, 1);
    ins->operands[0] = addr;
    ir_emit(b, ins);
    return ins->result;
}
void ir_store(IrFunc *f, IrBlock *b, IrValue *addr, IrValue *val) {
    IrInstr *ins = ir_instr(f, IR_STORE, NULL, 2);
    ins->operands[0] = addr; ins->operands[1] = val;
    ir_emit(b, ins);
}
IrValue *ir_slice_len(IrFunc *f, IrBlock *b, IrValue *slice) {
    IrInstr *ins = ir_instr(f, IR_SLICE_LEN, ir_type_int(f->arena, 64, false), 1);
    ins->operands[0] = slice;
    ir_emit(b, ins);
    return ins->result;
}
IrValue *ir_elem_ptr(IrFunc *f, IrBlock *b, IrValue *base, IrValue *idx, IrType *elem) {
    IrType *pt = ir_type_new(f->arena, IRT_PTR); pt->elem = elem;
    IrInstr *ins = ir_instr(f, IR_ELEM_PTR, pt, 2);
    ins->operands[0] = base; ins->operands[1] = idx;
    ir_emit(b, ins);
    return ins->result;
}

// ── terminators ──────────────────────────────────────────────────────────────
void ir_set_br(IrBlock *b, IrBlock *target) {
    b->term.kind = IR_TERM_BR; b->term.a = target;
}
void ir_set_br_cond(IrBlock *b, IrValue *cond, IrBlock *t, IrBlock *e) {
    b->term.kind = IR_TERM_BR_COND; b->term.cond = cond; b->term.a = t; b->term.b = e;
}
void ir_set_ret(IrBlock *b, IrValue *v /*NULL for unit*/) {
    b->term.kind = IR_TERM_RET; b->term.cond = v;
}
void ir_set_unreachable(IrBlock *b) { b->term.kind = IR_TERM_UNREACHABLE; }

// ── functions ────────────────────────────────────────────────────────────────
IrFunc *ir_func_new(Arena *a, Id *name, IrType *ret, IrFuncKind kind) {
    IrFunc *f = arena_push_aligned(a, IrFunc);
    memset(f, 0, sizeof *f);
    f->arena = a; f->name = name; f->ret_type = ret; f->kind = kind;
    f->entry = ir_new_block(f);
    return f;
}
IrValue *ir_add_param(IrFunc *f, IrType *t, Id *src_name) {
    IrValue *v = ir_new_value(f, t);
    v->src_name = src_name;
    IrParam *p = arena_push_aligned(f->arena, IrParam);
    p->value = v; p->next = NULL;
    if (!f->params) f->params = p;
    else { IrParam *q = f->params; while (q->next) q = q->next; q->next = p; }
    return v;
}

// ── CFG finalize: fill each block's predecessor edges + loop-header flags ─────
static void ir_add_pred(IrFunc *f, IrBlock *to, IrBlock *from) {
    if (!to) return;
    IrEdge *e = arena_push_aligned(f->arena, IrEdge);
    e->block = from; e->next = to->preds; to->preds = e;
}
void ir_finalize_cfg(IrFunc *f) {
    for (IrBlock *b = f->blocks; b; b = b->next) {
        switch (b->term.kind) {
            case IR_TERM_BR:      ir_add_pred(f, b->term.a, b); break;
            case IR_TERM_BR_COND: ir_add_pred(f, b->term.a, b); ir_add_pred(f, b->term.b, b); break;
            case IR_TERM_SWITCH:
                ir_add_pred(f, b->term.a, b);
                for (IrSwitchCase *c = b->term.cases; c; c = c->next) ir_add_pred(f, c->target, b);
                break;
            default: break;
        }
    }
    // A block is a loop header if a later block branches back to it (back-edge by
    // block order — sufficient for the structured CFGs lowering produces).
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrEdge *e = b->preds; e; e = e->next)
            if (e->block->id >= b->id) { b->is_loop_header = true; break; }
}

#endif // LAIN_IR_BUILD_H
