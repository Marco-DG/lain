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
        case IRT_STRUCT: case IRT_SUM:
                        if (e->sname) { IrName *nm=e->sname;
                            return snprintf(buf, n, "%.*s", (int)nm->length, nm->name); }
                        return snprintf(buf, n, "v");
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
        case IRT_STRUCT: case IRT_SUM:
            if (t->sname) { IrName *n = t->sname;
                                  fprintf(o, "%.*s", (int)n->length, n->name); }
            else fputs("void*", o);
            break;
        case IRT_UNIT: case IRT_NEVER: fputs("void", o); break;
        default:       fputs("void*", o); break;   // by-value array (rare) — later
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

// emit bytes as a C string literal (3-digit octal for anything unsafe, so a
// following digit can never extend the escape)
static void ir_emit_cstr(const char *s, int len, FILE *o) {
    fputc('"', o);
    for (int i=0;i<len;i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch=='"' || ch=='\\') { fputc('\\', o); fputc(ch, o); }
        else if (ch>=0x20 && ch<=0x7e) fputc(ch, o);
        else fprintf(o, "\\%03o", ch);
    }
    fputc('"', o);
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
        case IR_FIELD_PTR: {   // base is a struct pointer; name the field from its type
            IrType *st = i->operands[0]->type ? i->operands[0]->type->elem : NULL;
            IrName *fn = (st && st->kind==IRT_STRUCT && i->aux.field_idx < st->n_fields)
                     ? st->field_names[i->aux.field_idx] : NULL;
            if (fn) fprintf(o, "  v%d = &v%d->%.*s;\n", i->result->id, i->operands[0]->id,
                            (int)fn->length, fn->name);
            else    fprintf(o, "  v%d = &v%d->f%d;\n", i->result->id, i->operands[0]->id, i->aux.field_idx);
            break;
        }
        case IR_ASSUME: case IR_ASSERT: case IR_CONSUME: break;  // verification-only; no runtime code
        case IR_LOAD:   fprintf(o, "  v%d = *v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_STORE:  fprintf(o, "  *v%d = v%d;\n", i->operands[0]->id, i->operands[1]->id); break;
        case IR_SLICE_LEN:  fprintf(o, "  v%d = v%d.len;\n",  i->result->id, i->operands[0]->id); break;
        case IR_SLICE_DATA: fprintf(o, "  v%d = v%d.data;\n", i->result->id, i->operands[0]->id); break;
        case IR_MAKE_SLICE: fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                            fprintf(o, "){ v%d, v%d };\n", i->operands[0]->id, i->operands[1]->id); break;
        case IR_STR_CONST:  fprintf(o, "  v%d = (uint8_t*)", i->result->id);
                            ir_emit_cstr(i->aux.str.bytes, i->aux.str.len, o); fputs(";\n", o); break;
        case IR_CTZ: case IR_CLZ: case IR_POPCOUNT:
            fprintf(o, "  v%d = (uint32_t)%s((unsigned)(v%d));\n", i->result->id,
                    i->op==IR_CTZ ? "__builtin_ctz" : i->op==IR_CLZ ? "__builtin_clz"
                                                                    : "__builtin_popcount",
                    i->operands[0]->id);
            break;
        case IR_OPAQUE:
            // The construct was not modelled, so there is nothing faithful to emit. Produce a
            // zero of the right type and SAY SO in the output — a silent placeholder is how
            // `mk(i).x` came to read uninitialised memory. Codegen through this path is not
            // trustworthy; the IR records that, and the analyses havoc around it.
            if (i->result) {
                fprintf(o, "  v%d = 0;  /* OPAQUE: %s (unmodelled) */\n", i->result->id,
                        i->aux.opaque.why ? i->aux.opaque.why : "?");
            } else {
                fprintf(o, "  /* OPAQUE: %s (unmodelled) */\n",
                        i->aux.opaque.why ? i->aux.opaque.why : "?");
            }
            break;
        case IR_SUM_TAG:    fprintf(o, "  v%d = v%d.tag;\n", i->result->id, i->operands[0]->id); break;
        case IR_SUM_PAYLOAD: {
            IrType *st = i->operands[0]->type;
            int k = i->aux.sum.variant, fi = i->aux.sum.field;
            IrType *pl = (st && k < st->n_fields) ? st->fields[k] : NULL;
            IrName *vn = (st && k < st->n_fields) ? st->field_names[k] : NULL;
            IrName *fn = (pl && fi < pl->n_fields) ? pl->field_names[fi] : NULL;
            fprintf(o, "  v%d = v%d.data.%.*s.", i->result->id, i->operands[0]->id,
                    vn?(int)vn->length:0, vn?vn->name:"");
            if (fn) fprintf(o, "%.*s;\n", (int)fn->length, fn->name);
            else    fprintf(o, "f%d;\n", fi);
            break;
        }
        case IR_SUM_NEW: {
            IrType *st = i->result->type;
            int k = i->aux.sum.variant;
            IrType *pl = (st && k < st->n_fields) ? st->fields[k] : NULL;
            IrName *vn = (st && k < st->n_fields) ? st->field_names[k] : NULL;
            fprintf(o, "  v%d = (", i->result->id); ir_ctype(st, o);
            fprintf(o, "){ .tag = %d", k);
            if (pl && i->n_operands > 0) {
                fprintf(o, ", .data.%.*s = { ", vn?(int)vn->length:0, vn?vn->name:"");
                for (int j=0;j<i->n_operands;j++) {
                    IrName *fn = (j < pl->n_fields) ? pl->field_names[j] : NULL;
                    if (j) fputs(", ", o);
                    if (fn) fprintf(o, ".%.*s = ", (int)fn->length, fn->name);
                    else    fprintf(o, ".f%d = ", j);
                    fprintf(o, "v%d", i->operands[j]->id);
                }
                fputs(" }", o);
            }
            fputs(" };\n", o);
            break;
        }
        case IR_STRUCT_NEW: fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                            fputs("){ ", o);
                            for (int k=0;k<i->n_operands;k++){ if(k)fputs(", ",o); fprintf(o,"v%d",i->operands[k]->id); }
                            fputs(" };\n", o); break;
        case IR_ICMP:   fprintf(o, "  v%d = (v%d %s v%d);\n", i->result->id,
                                i->operands[0]->id, ir_cmp_c(i->aux.cmp), i->operands[1]->id); break;
        case IR_NEG:    fprintf(o, "  v%d = -v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_BNOT:   fprintf(o, "  v%d = ~v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_CAST:   fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                        fprintf(o, ")v%d;\n", i->operands[0]->id); break;
        case IR_CALL: {
            fputs("  ", o);
            if (i->result) fprintf(o, "v%d = ", i->result->id);
            if (i->aux.callee) fprintf(o, "%.*s", (int)i->aux.callee->length, i->aux.callee->name);
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

// A set of the composite types the module needs C declarations for, gathered
// transitively so a struct that appears only as a slice element / pointer pointee
// / another struct's field is still declared.
typedef struct { IrType *structs[256]; int n_struct; IrType *slices[256]; int n_slice; } IrTypeSet;
static bool ir_name_eq(const IrName *a, const IrName *b) {
    return a && b && a->length==b->length && memcmp(a->name,b->name,(size_t)a->length)==0;
}
static bool ir_ts_struct_seen(IrTypeSet *ts, IrType *st) {
    for (int i=0;i<ts->n_struct;i++) if (ir_name_eq(ts->structs[i]->sname, st->sname)) return true;
    return false;
}
static bool ir_ts_slice_seen(IrTypeSet *ts, IrType *sl) {
    char a[128]; ir_slice_tag(sl->elem, a, sizeof a);
    for (int i=0;i<ts->n_slice;i++){ char b[128]; ir_slice_tag(ts->slices[i]->elem,b,sizeof b);
        if (strcmp(a,b)==0) return true; } return false;
}
static void ir_ts_visit(IrTypeSet *ts, IrType *t) {
    if (!t) return;
    switch (t->kind) {
        case IRT_PTR: case IRT_ARRAY: ir_ts_visit(ts, t->elem); break;
        case IRT_SLICE:
            ir_ts_visit(ts, t->elem);                         // element first (dependency)
            if (!ir_ts_slice_seen(ts, t) && ts->n_slice<256) ts->slices[ts->n_slice++]=t;
            break;
        case IRT_STRUCT:
            if (t->sname && !ir_ts_struct_seen(ts, t) && ts->n_struct<256) {
                ts->structs[ts->n_struct++]=t;               // add before fields so cycles stop
                for (int i=0;i<t->n_fields;i++) ir_ts_visit(ts, t->fields[i]);
            }
            break;
        case IRT_SUM:
            // A sum's variant payloads are INLINED into its union, so they are not
            // standalone types — visit through them for their own dependencies (a slice
            // payload still needs its Slice_ typedef) without registering them.
            if (t->sname && !ir_ts_struct_seen(ts, t) && ts->n_struct<256) {
                ts->structs[ts->n_struct++]=t;
                for (int i=0;i<t->n_fields;i++) {
                    IrType *pl = t->fields[i]; if (!pl) continue;
                    for (int j=0;j<pl->n_fields;j++) ir_ts_visit(ts, pl->fields[j]);
                }
            }
            break;
        default: break;
    }
}
static void ir_emit_one_slice(IrType *sl, FILE *o) {
    char tag[128]; ir_slice_tag(sl->elem, tag, sizeof tag);
    fputs("typedef struct { ", o); ir_ctype(sl->elem, o);
    fprintf(o, "* data; size_t len; } Slice_%s;\n", tag);
}
// A sum's C layout: `struct S { int32_t tag; union { …per-variant payload… } data; }`.
// This is a BACKEND decision — the IR records only which variants exist and what they carry
// (design/ir_sum_types.md §3) — so swapping in a niche packing later touches only this file.
// A payload-less variant contributes nothing to the union; if no variant carries a payload
// the union is omitted entirely (an empty union is not legal C).
static void ir_emit_one_sum_body(IrType *st, FILE *o) {
    IrName *nm = st->sname;
    fprintf(o, "struct %.*s { int32_t tag; ", (int)nm->length, nm->name);
    int carrying = 0;
    for (int k=0;k<st->n_fields;k++) if (st->fields[k]) carrying++;
    if (carrying) {
        fputs("union { ", o);
        for (int k=0;k<st->n_fields;k++) {
            IrType *pl = st->fields[k]; if (!pl) continue;
            IrName *vn = st->field_names[k];
            fputs("struct { ", o);
            for (int j=0;j<pl->n_fields;j++) {
                ir_ctype(pl->fields[j], o);
                IrName *fn = pl->field_names[j];
                if (fn) fprintf(o, " %.*s; ", (int)fn->length, fn->name);
                else    fprintf(o, " f%d; ", j);
            }
            fprintf(o, "} %.*s; ", vn?(int)vn->length:0, vn?vn->name:"");
        }
        fputs("} data; ", o);
    }
    fputs("};\n", o);
}
static void ir_emit_one_struct_body(IrType *st, FILE *o) {
    if (st->kind == IRT_SUM) { ir_emit_one_sum_body(st, o); return; }
    IrName *nm = st->sname;
    fprintf(o, "struct %.*s { ", (int)nm->length, nm->name);
    for (int fi=0; fi<st->n_fields; fi++) {
        IrType *ft = st->fields[fi]; IrName *fn = st->field_names[fi];
        if (ft && ft->kind==IRT_ARRAY) {   // an inline fixed-array field
            ir_ctype(ft->elem, o);
            fprintf(o, " %.*s[%lld]; ", fn?(int)fn->length:0, fn?fn->name:"", (long long)ft->array_len);
        } else {
            ir_ctype(ft, o);
            if (fn) fprintf(o, " %.*s; ", (int)fn->length, fn->name);
            else    fprintf(o, " f%d; ", fi);
        }
    }
    fputs("};\n", o);
}
// Emit `ts.structs[i]`'s body after everything it contains BY VALUE. Recurses through
// struct fields and sum-variant payload fields; a pointer/slice field needs only the
// forward declaration, so it is not a dependency.
static void ir_emit_struct_body_deps(IrTypeSet *ts, int i, bool *done, FILE *o) {
    if (i < 0 || i >= ts->n_struct || done[i]) return;
    done[i] = true;                          // set first: a pointer cycle must not recurse
    IrType *t = ts->structs[i];
    for (int f=0; f<t->n_fields; f++) {
        IrType *ft = t->fields[f];
        if (!ft) continue;
        if (t->kind == IRT_SUM) {            // a variant payload is inlined, so ITS fields
            for (int g=0; g<ft->n_fields; g++) {
                IrType *gt = ft->fields[g];
                if (!gt || (gt->kind!=IRT_STRUCT && gt->kind!=IRT_SUM)) continue;
                for (int k=0;k<ts->n_struct;k++) if (ts->structs[k]==gt) ir_emit_struct_body_deps(ts,k,done,o);
            }
            continue;
        }
        while (ft && ft->kind==IRT_ARRAY) ft = ft->elem;   // an inline array of structs too
        if (!ft || (ft->kind!=IRT_STRUCT && ft->kind!=IRT_SUM)) continue;
        for (int k=0;k<ts->n_struct;k++) if (ts->structs[k]==ft) ir_emit_struct_body_deps(ts,k,done,o);
    }
    ir_emit_one_struct_body(t, o);
}

// Forward-declare every struct, then slices (which only need the struct *pointer*),
// then the full struct bodies in reverse discovery order (a by-value nested struct
// is discovered after its container, so reverse puts the inner one first).
static void ir_emit_type_decls(IrFunc *funcs, FILE *o, Arena *a) {
    IrTypeSet ts = {0};
    for (IrFunc *f=funcs; f; f=f->next) {
        IrValTab vt = { arena_push_many_aligned(a, IrValue*, f->next_value_id), f->next_value_id };
        for (int k=0;k<vt.n;k++) vt.v[k]=NULL;
        ir_collect_vals(f, &vt);
        for (int id=0; id<vt.n; id++) if (vt.v[id]) ir_ts_visit(&ts, vt.v[id]->type);
    }
    for (int i=0;i<ts.n_struct;i++){ IrName *nm=ts.structs[i]->sname;
        fprintf(o, "typedef struct %.*s %.*s;\n", (int)nm->length, nm->name, (int)nm->length, nm->name); }
    for (int i=0;i<ts.n_slice;i++) ir_emit_one_slice(ts.slices[i], o);
    // Bodies in DEPENDENCY order: a struct that contains another BY VALUE needs the inner
    // one complete first. Reverse discovery order is not that — it works only when the
    // container happens to be discovered first, and `func mk() P` before `func mq() Q`
    // (Q containing a P) discovers P first, so Q's body was emitted with an incomplete
    // field type. By-value containment cannot be cyclic, so a simple depth-first emit with
    // a visited set is a correct topological order; pointer cycles are already handled by
    // the forward typedefs above.
    { bool done[256]; for (int i=0;i<ts.n_struct;i++) done[i]=false;
      for (int i=0;i<ts.n_struct;i++) ir_emit_struct_body_deps(&ts, i, done, o); }
    if (ts.n_struct || ts.n_slice) fputc('\n', o);
}

void ir_emit_module_c(IrFunc *funcs, FILE *o, Arena *a) {
    fputs("#include <stdint.h>\n#include <stddef.h>\n\n", o);
    ir_emit_type_decls(funcs, o, a);
    for (IrFunc *f=funcs; f; f=f->next) ir_emit_proto_c(f, o);
    fputc('\n', o);
    for (IrFunc *f=funcs; f; f=f->next) ir_emit_func_c(f, o, a);
}

#endif // LAIN_IR_EMIT_C_H
