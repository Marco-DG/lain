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

// Intern a name into the IR arena (copies the bytes — the AST is then freeable).
IrName *ir_intern(Arena *a, const char *s, isize len) {
    IrName *n = arena_push_aligned(a, IrName);
    char *buf = arena_push_many(a, char, len+1);
    if (len>0) memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    n->name = buf; n->length = len;
    return n;
}

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
    // B4: a static refinement on the TYPE tightens the interval for EVERY consumer at once —
    // bounds, overflow, div-by-zero — because they all seed from here.
    if (t->has_refine) {
        if (t->refine_lo > *lo) *lo = t->refine_lo;
        if (t->refine_hi < *hi) *hi = t->refine_hi;
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
// The source position instructions are stamped with. Exactly ONE instruction in the whole
// lowering used to carry a location (the elem_ptr in ir_lower_addr), so every diagnostic the
// new engine could produce — a leak, a use-after-move, an uninitialised read, an overflow —
// pointed at line 0. That is not a cosmetic gap: an engine that cannot say WHERE cannot be
// the one a user runs, whatever it can prove. Lowering keeps this current; ir_emit stamps it.
static isize ir_cur_line = 0, ir_cur_col = 0;

IrInstr *ir_emit(IrBlock *b, IrInstr *ins) {
    if (!ins->line) { ins->line = ir_cur_line; ins->col = ir_cur_col; }
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
// `assume(cond)` — cond (a bool) holds from here on. No result. The verification
// substrate: guards, param refinements, and callee ensures all become assumes, so
// analyses read facts from the IR, never from the AST.
void ir_assume(IrFunc *f, IrBlock *b, IrValue *cond) {
    IrInstr *ins = ir_instr(f, IR_ASSUME, NULL, 1);
    ins->operands[0] = cond;
    ir_emit(b, ins);
}
// `assert(cond)` — cond (a bool) is an OBLIGATION the analysis must discharge here
// (a precondition, a user assert). No result.
void ir_assert(IrFunc *f, IrBlock *b, IrValue *cond) {
    IrInstr *ins = ir_instr(f, IR_ASSERT, NULL, 1);
    ins->operands[0] = cond;
    ir_emit(b, ins);
}
// `consume(slot)` — Phase 3.2: `mov x` invalidates x's storage; op[0] is the moved-from
// slot. The linearity pass reads it; codegen ignores it. No result.
void ir_consume(IrFunc *f, IrBlock *b, IrValue *slot) {
    IrInstr *ins = ir_instr(f, IR_CONSUME, NULL, 1);
    ins->operands[0] = slot;
    ir_emit(b, ins);
}
// A scoped borrow of `place`: live until the matching ir_borrow_end. No runtime effect.
void ir_borrow_begin(IrFunc *f, IrBlock *b, IrValue *place) {
    IrInstr *ins = ir_instr(f, IR_BORROW, NULL, 1); ins->operands[0] = place; ir_emit(b, ins);
}
void ir_borrow_end(IrFunc *f, IrBlock *b, IrValue *place) {
    IrInstr *ins = ir_instr(f, IR_BORROW_END, NULL, 1); ins->operands[0] = place; ir_emit(b, ins);
}

// Declare that `place` is wholly initialised from here (no runtime effect).
void ir_init_fact(IrFunc *f, IrBlock *b, IrValue *place) {
    IrInstr *ins = ir_instr(f, IR_INIT, NULL, 1);
    ins->operands[0] = place;
    ir_emit(b, ins);
}

// S2: declare a rank-N shape for a flat region. op[0]=base, op[1..n]=extents (outermost
// first). No result — it states a fact about the base, like an assume.
void ir_shape(IrFunc *f, IrBlock *b, IrValue *base, IrValue **extents, int rank) {
    IrInstr *ins = ir_instr(f, IR_SHAPE, NULL, rank+1);
    ins->operands[0] = base;
    for (int i=0;i<rank;i++) ins->operands[1+i] = extents[i];
    ir_emit(b, ins);
}

// An UNMODELLED construct (B3). Result type may be NULL (a statement); `writes` declares
// whether it may write tracked memory. Operands are the values it may read.
IrValue *ir_opaque(IrFunc *f, IrBlock *b, IrType *rt, bool writes, const char *why,
                   IrValue **reads, int nreads) {
    IrInstr *ins = ir_instr(f, IR_OPAQUE, rt, nreads);
    for (int i=0;i<nreads;i++) ins->operands[i] = reads[i];
    ins->aux.opaque.writes = writes;
    ins->aux.opaque.why    = why;
    ir_emit(b, ins);
    return ins->result;
}

// A bit intrinsic (ctz/clz/popcount): one integer operand, an integer result.
IrValue *ir_bitcount(IrFunc *f, IrBlock *b, IrOp op, IrValue *x, IrType *t) {
    IrInstr *ins = ir_instr(f, op, t, 1);
    ins->operands[0] = x;
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
    ins->result->owns = true;    // a local slot OWNS its contents by default; the borrow
    ir_emit(b, ins);             // bindings (param home slots) clear it explicitly
    return ins->result;
}
// A fixed-array local: the alloca decays to an element pointer (result type *elem),
// while aux.alloca_ty records the ARRAY type so the backend declares `elem slot[N]`.
IrValue *ir_alloca_array(IrFunc *f, IrBlock *b, IrType *arr_ty) {
    IrType *pt = ir_type_new(f->arena, IRT_PTR); pt->elem = arr_ty->elem;
    IrInstr *ins = ir_instr(f, IR_ALLOCA, pt, 0);
    ins->aux.alloca_ty = arr_ty;   // IRT_ARRAY(elem, N)
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
// Extract the data pointer from a slice (result type *elem).
IrValue *ir_slice_data(IrFunc *f, IrBlock *b, IrValue *slice, IrType *elem) {
    IrType *pt = ir_type_new(f->arena, IRT_PTR); pt->elem = elem;
    IrInstr *ins = ir_instr(f, IR_SLICE_DATA, pt, 1);
    ins->operands[0] = slice;
    ir_emit(b, ins);
    return ins->result;
}
// A string literal's bytes — result is *u8 pointing at static storage.
IrValue *ir_str_const(IrFunc *f, IrBlock *b, const char *bytes, int32_t len) {
    IrType *pt = ir_type_new(f->arena, IRT_PTR); pt->elem = ir_type_int(f->arena, 8, false);
    IrInstr *ins = ir_instr(f, IR_STR_CONST, pt, 0);
    ins->aux.str.bytes = bytes; ins->aux.str.len = len;
    ir_emit(b, ins);
    return ins->result;
}
// Build a slice value {data, len} — for array→slice decay and sub-slicing.
IrValue *ir_make_slice(IrFunc *f, IrBlock *b, IrValue *data, IrValue *len, IrType *elem) {
    IrType *st = ir_type_new(f->arena, IRT_SLICE); st->elem = elem;
    IrInstr *ins = ir_instr(f, IR_MAKE_SLICE, st, 2);
    ins->operands[0] = data; ins->operands[1] = len;
    ir_emit(b, ins);
    return ins->result;
}
// Construct a struct value from its fields, in declaration order.
IrValue *ir_struct_new(IrFunc *f, IrBlock *b, IrType *sty, IrValue **fields, int n) {
    IrInstr *ins = ir_instr(f, IR_STRUCT_NEW, sty, n);
    for (int i=0;i<n;i++) ins->operands[i] = fields[i];
    ir_emit(b, ins);   // the struct's name lives on the result type (sty->sname)
    return ins->result;
}
// ── sum types (design/ir_sum_types.md) ───────────────────────────────────────
// Construct variant `k` of `sty` from its payload fields (n may be 0).
IrValue *ir_sum_new(IrFunc *f, IrBlock *b, IrType *sty, int k, IrValue **payload, int n) {
    IrInstr *ins = ir_instr(f, IR_SUM_NEW, sty, n);
    for (int i=0;i<n;i++) ins->operands[i] = payload[i];
    ins->aux.sum.variant = k; ins->aux.sum.field = 0;
    ir_emit(b, ins);
    return ins->result;
}
// The discriminant of a sum VALUE, as a plain integer the numeric domain can track.
IrValue *ir_sum_tag(IrFunc *f, IrBlock *b, IrValue *sum) {
    IrInstr *ins = ir_instr(f, IR_SUM_TAG, ir_type_int(f->arena, 32, true), 1);
    ins->operands[0] = sum;
    ir_emit(b, ins);
    return ins->result;
}
// Payload field `fi` of variant `k`. Well-defined only where the tag is known to be k —
// the CFG establishes that, so this carries no check of its own.
IrValue *ir_sum_payload(IrFunc *f, IrBlock *b, IrValue *sum, int k, int fi, IrType *fty) {
    IrInstr *ins = ir_instr(f, IR_SUM_PAYLOAD, fty, 1);
    ins->operands[0] = sum;
    ins->aux.sum.variant = k; ins->aux.sum.field = fi;
    ir_emit(b, ins);
    return ins->result;
}

// Address of struct field #idx (base is the struct's address).
IrValue *ir_field_ptr(IrFunc *f, IrBlock *b, IrValue *base, int idx, IrType *fty) {
    IrType *pt = ir_type_new(f->arena, IRT_PTR); pt->elem = fty;
    IrInstr *ins = ir_instr(f, IR_FIELD_PTR, pt, 1);
    ins->operands[0] = base; ins->aux.field_idx = idx;
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
IrFunc *ir_func_new(Arena *a, IrName *name, IrType *ret, IrFuncKind kind) {
    IrFunc *f = arena_push_aligned(a, IrFunc);
    memset(f, 0, sizeof *f);
    f->arena = a; f->name = name; f->ret_type = ret; f->kind = kind;
    f->entry = ir_new_block(f);
    return f;
}
IrValue *ir_add_param(IrFunc *f, IrType *t, IrName *src_name) {
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
