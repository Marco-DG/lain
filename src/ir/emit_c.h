// src/ir/emit_c.h — IR → C backend (Phase 1.3), faithful & dumb.
//
// Blocks become labels + gotos; SSA values become C locals declared at function top
// (so a value defined in one block and used in another is in scope, and label-followed
// -by-declaration is avoided). The alloca/load/store memory form emits real slot
// pointers — the C compiler optimizes them away. Checks were discharged by analysis
// (none yet in this phase), so no runtime checks are emitted. Scalar core first
// (int/bool + arithmetic/cmp/call/branch); arrays/slices/structs grow later.
#ifndef LAIN_IR_EMIT_C_H
#define LAIN_IR_EMIT_C_H

#include <stdio.h>
#include "ir.h"

// round a non-standard integer width up to a standard C width
static int ir_c_stdbits(int bits) { return bits<=8?8 : bits<=16?16 : bits<=32?32 : 64; }

// mangle a slice element type into a valid C identifier suffix (for Slice_<tag>)
static int ir_slice_tag(const IrType *e, char *buf, int n) {
    if (n <= 1 || !e) { if (n>0) buf[0]=0; return 0; }
    switch (e->kind) {
        case IRT_INT:   return snprintf(buf, n, "%c%d", e->is_signed?'i':'u', e->bits);
        case IRT_BOOL:  return snprintf(buf, n, "b");
        case IRT_PTR:   { int k=snprintf(buf,n,"p"); return k + ir_slice_tag(e->elem, buf+k, n-k); }
        case IRT_SLICE: { int k=snprintf(buf,n,"s"); return k + ir_slice_tag(e->elem, buf+k, n-k); }
        default:        return snprintf(buf, n, "v");
    }
}

static void ir_ctype(const IrType *t, FILE *o) {
    if (!t) { fputs("void", o); return; }
    switch (t->kind) {
        case IRT_INT:  fprintf(o, "%sint%d_t", t->is_signed?"":"u", ir_c_stdbits(t->bits)); break;
        case IRT_BOOL: fputs("_Bool", o); break;
        case IRT_PTR:  ir_ctype(t->elem, o); fputc('*', o); break;
        case IRT_SLICE:{ char tag[128]; ir_slice_tag(t->elem, tag, sizeof tag); fprintf(o, "Slice_%s", tag); } break;
        case IRT_UNIT: case IRT_NEVER: fputs("void", o); break;
        default:       fputs("void*", o); break;   // by-value array (rare)/struct — later
    }
}

// value-table: id → IrValue* (ids are dense per function)
typedef struct { IrValue **v; int n; } IrValTab;
static void ir_vt_put(IrValTab *t, IrValue *v) { if (v && v->id < t->n) t->v[v->id] = v; }
static void ir_collect_vals(IrFunc *f, IrValTab *t) {
    for (IrParam *p=f->params; p; p=p->next) ir_vt_put(t, p->value);
    for (IrBlock *b=f->blocks; b; b=b->next) {
        for (IrInstr *i=b->phis;  i; i=i->next) ir_vt_put(t, i->result);
        for (IrInstr *i=b->instrs; i; i=i->next) ir_vt_put(t, i->result);
    }
}

static const char *ir_cmp_c(IrCmp c) {
    switch (c) { case IR_CMP_EQ:return "=="; case IR_CMP_NE:return "!=";
        case IR_CMP_SLT: case IR_CMP_ULT:return "<"; case IR_CMP_SLE: case IR_CMP_ULE:return "<=";
        case IR_CMP_SGT: case IR_CMP_UGT:return ">"; case IR_CMP_SGE: case IR_CMP_UGE:return ">="; }
    return "==";
}
static const char *ir_arith_c(IrOp op) {
    switch (op) { case IR_ADD:return "+"; case IR_SUB:return "-"; case IR_MUL:return "*";
        case IR_SDIV: case IR_UDIV:return "/"; case IR_SREM: case IR_UREM:return "%";
        case IR_AND:return "&"; case IR_OR:return "|"; case IR_XOR:return "^";
        case IR_SHL:return "<<"; case IR_LSHR: case IR_ASHR:return ">>"; default:return "+"; }
}

static void ir_emit_instr_c(IrInstr *i, FILE *o) {
    switch (i->op) {
        case IR_CONST:  fprintf(o, "  v%d = %lld;\n", i->result->id, (long long)i->aux.imm); break;
        case IR_ALLOCA: // array decays to its element base; scalar takes the slot address
            if (i->aux.alloca_ty && i->aux.alloca_ty->kind==IRT_ARRAY)
                 fprintf(o, "  v%d = slot%d;\n",  i->result->id, i->result->id);
            else fprintf(o, "  v%d = &slot%d;\n", i->result->id, i->result->id);
            break;
        case IR_ELEM_PTR: fprintf(o, "  v%d = &v%d[v%d];\n", i->result->id,
                                  i->operands[0]->id, i->operands[1]->id); break;
        case IR_LOAD:   fprintf(o, "  v%d = *v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_STORE:  fprintf(o, "  *v%d = v%d;\n", i->operands[0]->id, i->operands[1]->id); break;
        case IR_SLICE_LEN:  fprintf(o, "  v%d = v%d.len;\n",  i->result->id, i->operands[0]->id); break;
        case IR_SLICE_DATA: fprintf(o, "  v%d = v%d.data;\n", i->result->id, i->operands[0]->id); break;
        case IR_MAKE_SLICE: fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                            fprintf(o, "){ v%d, v%d };\n", i->operands[0]->id, i->operands[1]->id); break;
        case IR_ICMP:   fprintf(o, "  v%d = (v%d %s v%d);\n", i->result->id,
                                i->operands[0]->id, ir_cmp_c(i->aux.cmp), i->operands[1]->id); break;
        case IR_NEG:    fprintf(o, "  v%d = -v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_BNOT:   fprintf(o, "  v%d = ~v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_CAST:   fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                        fprintf(o, ")v%d;\n", i->operands[0]->id); break;
        case IR_CALL: {
            fputs("  ", o);
            if (i->result) fprintf(o, "v%d = ", i->result->id);
            if (i->aux.callee) fprintf(o, "%.*s", (int)i->aux.callee->as.function_decl.name->length,
                                        i->aux.callee->as.function_decl.name->name);
            fputc('(', o);
            for (int k=0;k<i->n_operands;k++){ if(k)fputs(", ",o); fprintf(o,"v%d",i->operands[k]->id); }
            fputs(");\n", o);
            break;
        }
        default:
            if (i->n_operands == 2 && i->result)
                fprintf(o, "  v%d = v%d %s v%d;\n", i->result->id,
                        i->operands[0]->id, ir_arith_c(i->op), i->operands[1]->id);
            break;
    }
}

static void ir_emit_func_c(IrFunc *f, FILE *o, Arena *a) {
    bool is_main = f->name->length==4 && strncmp(f->name->name,"main",4)==0;
    // signature
    if (is_main) fputs("int main(void)", o);
    else {
        ir_ctype(f->ret_type, o); fprintf(o, " %.*s(", (int)f->name->length, f->name->name);
        int k=0; for (IrParam *p=f->params; p; p=p->next,k++) {
            if (k) fputs(", ", o); ir_ctype(p->value->type, o); fprintf(o, " v%d", p->value->id);
        }
        if (!f->params) fputs("void", o);
        fputc(')', o);
    }
    fputs(" {\n", o);
    // declare all non-param values at the top, plus a backing slot for each alloca
    IrValTab vt = { arena_push_many_aligned(a, IrValue*, f->next_value_id), f->next_value_id };
    IrType **alloca_ty = arena_push_many_aligned(a, IrType*, f->next_value_id);
    for (int k=0;k<vt.n;k++){ vt.v[k]=NULL; alloca_ty[k]=NULL; }
    ir_collect_vals(f, &vt);
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_ALLOCA && i->result) alloca_ty[i->result->id] = i->aux.alloca_ty;
    bool param[4096] = {0};
    for (IrParam *p=f->params; p; p=p->next) if (p->value->id < 4096) param[p->value->id]=true;
    for (int id=0; id<vt.n; id++) {
        IrValue *v = vt.v[id];
        if (!v || (id<4096 && param[id])) continue;
        IrType *at = alloca_ty[id];
        if (at) {   // alloca result: declare the slot + its element/scalar pointer
            if (at->kind==IRT_ARRAY) {
                fputs("  ", o); ir_ctype(at->elem, o); fprintf(o, " slot%d[%lld];\n", id, (long long)at->array_len);
                fputs("  ", o); ir_ctype(at->elem, o); fprintf(o, "* v%d;\n", id);
            } else {
                fputs("  ", o); ir_ctype(at, o); fprintf(o, " slot%d;\n", id);
                fputs("  ", o); ir_ctype(at, o); fprintf(o, "* v%d;\n", id);
            }
        } else {
            fputs("  ", o); ir_ctype(v->type, o); fprintf(o, " v%d;\n", id);
        }
    }
    // blocks
    for (IrBlock *b=f->blocks; b; b=b->next) {
        fprintf(o, " L%d: ;\n", b->id);
        for (IrInstr *i=b->instrs; i; i=i->next) ir_emit_instr_c(i, o);
        switch (b->term.kind) {
            case IR_TERM_BR:      fprintf(o, "  goto L%d;\n", b->term.a->id); break;
            case IR_TERM_BR_COND: fprintf(o, "  if (v%d) goto L%d; else goto L%d;\n",
                                          b->term.cond->id, b->term.a->id, b->term.b->id); break;
            case IR_TERM_RET:     if (b->term.cond) fprintf(o, "  return v%d;\n", b->term.cond->id);
                                  else fputs(is_main ? "  return 0;\n" : "  return;\n", o); break;
            case IR_TERM_UNREACHABLE: fputs("  return 0;\n", o); break;
            default: break;
        }
    }
    fputs("}\n\n", o);
}

// forward declaration line for a function (so callers link regardless of order)
static void ir_emit_proto_c(IrFunc *f, FILE *o) {
    if (f->name->length==4 && strncmp(f->name->name,"main",4)==0) return;
    ir_ctype(f->ret_type, o); fprintf(o, " %.*s(", (int)f->name->length, f->name->name);
    int k=0; for (IrParam *p=f->params; p; p=p->next,k++){ if(k)fputs(", ",o); ir_ctype(p->value->type,o); }
    if (!f->params) fputs("void", o);
    fputs(");\n", o);
}

// Emit one `typedef struct { T* data; size_t len; } Slice_<tag>;` per distinct
// slice type used anywhere in the module (params, returns, locals, make_slice).
static void ir_emit_slice_typedefs(IrFunc *funcs, FILE *o, Arena *a) {
    char seen[64][128]; int nseen = 0;
    for (IrFunc *f=funcs; f; f=f->next) {
        IrValTab vt = { arena_push_many_aligned(a, IrValue*, f->next_value_id), f->next_value_id };
        for (int k=0;k<vt.n;k++) vt.v[k]=NULL;
        ir_collect_vals(f, &vt);
        for (int id=0; id<vt.n; id++) {
            IrValue *v = vt.v[id];
            if (!v || !v->type || v->type->kind != IRT_SLICE) continue;
            char tag[128]; ir_slice_tag(v->type->elem, tag, sizeof tag);
            bool dup=false; for (int s=0;s<nseen;s++) if (strcmp(seen[s],tag)==0){dup=true;break;}
            if (dup || nseen>=64) continue;
            strncpy(seen[nseen], tag, sizeof seen[0]); seen[nseen][sizeof seen[0]-1]=0; nseen++;
            fputs("typedef struct { ", o); ir_ctype(v->type->elem, o);
            fprintf(o, "* data; size_t len; } Slice_%s;\n", tag);
        }
    }
    if (nseen) fputc('\n', o);
}

void ir_emit_module_c(IrFunc *funcs, FILE *o, Arena *a) {
    fputs("#include <stdint.h>\n#include <stddef.h>\n\n", o);
    ir_emit_slice_typedefs(funcs, o, a);
    for (IrFunc *f=funcs; f; f=f->next) ir_emit_proto_c(f, o);
    fputc('\n', o);
    for (IrFunc *f=funcs; f; f=f->next) ir_emit_func_c(f, o, a);
}

#endif // LAIN_IR_EMIT_C_H
