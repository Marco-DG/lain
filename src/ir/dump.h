// src/ir/dump.h — human-readable IR printer (debugging + golden tests).
// Phase 1.2/1.5 of the clean-core rebuild.
#ifndef LAIN_IR_DUMP_H
#define LAIN_IR_DUMP_H

#include <stdio.h>
#include "ir.h"

static void ir_dump_type(const IrType *t, FILE *o) {
    if (!t) { fputs("?", o); return; }
    switch (t->kind) {
        case IRT_INT:   fprintf(o, "%c%d", t->is_signed ? 'i' : 'u', t->bits); break;
        case IRT_BOOL:  fputs("bool", o); break;
        case IRT_FLOAT: fprintf(o, "f%d", t->float_bits); break;
        case IRT_PTR:   fputc('*', o); if (t->ptr_mut) fputs("var ", o); ir_dump_type(t->elem, o); break;
        case IRT_SLICE: fputs("[]", o); ir_dump_type(t->elem, o); break;
        case IRT_ARRAY: fprintf(o, "[%lld]", (long long)t->array_len); ir_dump_type(t->elem, o); break;
        case IRT_STRUCT:fputs("struct", o); break;
        case IRT_UNIT:  fputs("unit", o); break;
        case IRT_NEVER: fputs("never", o); break;
    }
}

static const char *ir_op_name(IrOp op) {
    switch (op) {
        case IR_CONST: return "const"; case IR_CAST: return "cast";
        case IR_ADD: return "add"; case IR_SUB: return "sub"; case IR_MUL: return "mul";
        case IR_SDIV: return "sdiv"; case IR_UDIV: return "udiv";
        case IR_SREM: return "srem"; case IR_UREM: return "urem"; case IR_NEG: return "neg";
        case IR_AND: return "and"; case IR_OR: return "or"; case IR_XOR: return "xor";
        case IR_SHL: return "shl"; case IR_LSHR: return "lshr"; case IR_ASHR: return "ashr"; case IR_BNOT: return "bnot";
        case IR_ICMP: return "icmp";
        case IR_ALLOCA: return "alloca"; case IR_LOAD: return "load"; case IR_STORE: return "store";
        case IR_FIELD_PTR: return "field_ptr"; case IR_ELEM_PTR: return "elem_ptr";
        case IR_SLICE_LEN: return "slice_len"; case IR_SLICE_DATA: return "slice_data";
        case IR_MAKE_SLICE: return "make_slice"; case IR_SUBSLICE: return "subslice";
        case IR_ARRAY_NEW: return "array_new"; case IR_STRUCT_NEW: return "struct_new";
        case IR_STR_CONST: return "str_const";
        case IR_ASSUME: return "assume"; case IR_ASSERT: return "assert";
        case IR_CALL: return "call"; case IR_PHI: return "phi";
    }
    return "??";
}
static const char *ir_cmp_name(IrCmp c) {
    switch (c) {
        case IR_CMP_EQ:return "eq"; case IR_CMP_NE:return "ne";
        case IR_CMP_SLT:return "slt"; case IR_CMP_SLE:return "sle"; case IR_CMP_SGT:return "sgt"; case IR_CMP_SGE:return "sge";
        case IR_CMP_ULT:return "ult"; case IR_CMP_ULE:return "ule"; case IR_CMP_UGT:return "ugt"; case IR_CMP_UGE:return "uge";
    }
    return "?";
}

static void ir_dump_val(const IrValue *v, FILE *o) {
    if (!v) { fputs("<null>", o); return; }
    fprintf(o, "%%%d", v->id);
}

static void ir_dump_instr(const IrInstr *ins, FILE *o) {
    fputs("  ", o);
    if (ins->result) { ir_dump_val(ins->result, o); fputs(" = ", o); }
    fputs(ir_op_name(ins->op), o);
    if (ins->wrap == IR_WRAP_MODULAR)  fputs(".wrap", o);
    if (ins->wrap == IR_WRAP_SATURATE) fputs(".sat", o);
    if (ins->unchecked) fputs(".unchecked", o);
    if (ins->op == IR_ICMP) fprintf(o, ".%s", ir_cmp_name(ins->aux.cmp));
    fputc(' ', o);
    if (ins->op == IR_CONST) { fprintf(o, "%lld", (long long)ins->aux.imm); }
    else if (ins->op == IR_CALL) {
        if (ins->aux.callee) fprintf(o, "@%.*s", (int)ins->aux.callee->length, ins->aux.callee->name);
        fputc('(', o);
        for (int i = 0; i < ins->n_operands; i++) { if (i) fputs(", ", o); ir_dump_val(ins->operands[i], o); }
        fputc(')', o);
    } else if (ins->op == IR_PHI) {
        for (IrPhiArg *p = ins->phi_args; p; p = p->next) {
            fprintf(o, "[ "); ir_dump_val(p->value, o); fprintf(o, ", bb%d ]", p->pred ? p->pred->id : -1);
        }
    } else {
        for (int i = 0; i < ins->n_operands; i++) { if (i) fputs(", ", o); ir_dump_val(ins->operands[i], o); }
    }
    if (ins->result) { fputs(" : ", o); ir_dump_type(ins->result->type, o); }
    fputc('\n', o);
}

static void ir_dump_term(const IrTerm *t, FILE *o) {
    fputs("  ", o);
    switch (t->kind) {
        case IR_TERM_BR:      fprintf(o, "br bb%d\n", t->a ? t->a->id : -1); break;
        case IR_TERM_BR_COND: fputs("br_cond ", o); ir_dump_val(t->cond, o);
                              fprintf(o, ", bb%d, bb%d\n", t->a?t->a->id:-1, t->b?t->b->id:-1); break;
        case IR_TERM_SWITCH:  fputs("switch ", o); ir_dump_val(t->cond, o);
                              for (IrSwitchCase *c = t->cases; c; c = c->next)
                                  fprintf(o, " [%lld → bb%d]", (long long)c->key, c->target?c->target->id:-1);
                              fprintf(o, " default bb%d\n", t->a?t->a->id:-1); break;
        case IR_TERM_RET:     fputs("ret", o); if (t->cond) { fputc(' ', o); ir_dump_val(t->cond, o); } fputc('\n', o); break;
        case IR_TERM_UNREACHABLE: fputs("unreachable\n", o); break;
    }
}

void ir_dump_func(const IrFunc *f, FILE *o) {
    fprintf(o, "%s %.*s(", f->kind == IR_FUNC_PURE ? "func" : "proc",
            (int)f->name->length, f->name->name);
    int i = 0;
    for (IrParam *p = f->params; p; p = p->next, i++) {
        if (i) fputs(", ", o);
        ir_dump_val(p->value, o); fputs(": ", o); ir_dump_type(p->value->type, o);
    }
    fputs(") -> ", o); ir_dump_type(f->ret_type, o); fputs(" {\n", o);
    for (const IrBlock *b = f->blocks; b; b = b->next) {
        fprintf(o, "bb%d:%s\n", b->id, b->is_loop_header ? "   ; loop header" : "");
        for (const IrInstr *ins = b->phis;  ins; ins = ins->next) ir_dump_instr(ins, o);
        for (const IrInstr *ins = b->instrs; ins; ins = ins->next) ir_dump_instr(ins, o);
        ir_dump_term(&b->term, o);
    }
    fputs("}\n", o);
}

#endif // LAIN_IR_DUMP_H
