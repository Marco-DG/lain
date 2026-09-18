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
#include "annot.h"   // Stage IV: the proofs, expressed as C the optimizer can use

// round a non-standard integer width up to a standard C width
static int ir_c_stdbits(int bits) { return bits<=8?8 : bits<=16?16 : bits<=32?32 : 64; }

// mangle a slice element type into a valid C identifier suffix (for Slice_<tag>)
static int ir_slice_tag(const IrType *e, char *buf, int n) {
    if (n <= 1 || !e) { if (n>0) buf[0]=0; return 0; }
    switch (e->kind) {
        case IRT_INT:   return snprintf(buf, n, "%c%d", e->is_signed?'i':'u', e->bits);
        case IRT_BOOL:  return snprintf(buf, n, "b");
        case IRT_PTR:   { int k=snprintf(buf,n,"p"); return k + ir_slice_tag(e->elem, buf+k, n-k); }
        case IRT_FLOAT: return snprintf(buf, n, "f%d", e->float_bits);
        case IRT_VECTOR: { int k=snprintf(buf,n,"v%d", (int)e->array_len);
                           return k + ir_slice_tag(e->elem, buf+k, n-k); }
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
        case IRT_FLOAT: fputs(t->float_bits==32 ? "float" : "double", o); break;
        case IRT_FUNC: {     // the function-pointer typedef declared in ir_emit_type_decls.
            // Named from the SIGNATURE rather than a counter, so two independent mentions of
            // the same type produce the same name without sharing state.
            char b[192]; int k = snprintf(b, sizeof b, "Fn_");
            k += ir_slice_tag(t->elem, b+k, (int)sizeof b - k);
            for (int i2=0;i2<t->n_fields;i2++) {
                k += snprintf(b+k, sizeof b - (size_t)k, "_");
                k += ir_slice_tag(t->fields[i2], b+k, (int)sizeof b - k);
            }
            fputs(b, o);
            break;
        }
        case IRT_VECTOR: {   // the GCC/Clang vector_size typedef declared in ir_emit_type_decls
            char tag[64]; ir_slice_tag(t->elem, tag, sizeof tag);
            fprintf(o, "Vec_%d_%s", (int)t->array_len, tag);
            break;
        }
        case IRT_PTR:  ir_ctype(t->elem, o); fputc('*', o); break;
        case IRT_SLICE:{ char tag[128]; ir_slice_tag(t->elem, tag, sizeof tag); fprintf(o, "Slice_%s", tag); } break;
        case IRT_STRUCT: case IRT_SUM:
            if (t->sname) { IrName *n = t->sname;
                                  fprintf(o, "%.*s", (int)n->length, n->name); }
            else fputs("void*", o);
            break;
        case IRT_UNIT: case IRT_NEVER: fputs("void", o); break;
        // An ARRAY in a value position is a parameter that has DECAYED, so it must carry its
        // element type: emitted as `void*` it fell into GCC's byte-arithmetic extension, and
        // `a[i]` on an `i32[100]` parameter advanced by BYTES — every element read past the
        // first landed on a misaligned address inside the array. It was not rare at all; it
        // was every fixed-array parameter in the new pipeline, and nothing could see it until
        // there was a fuzzer that EXECUTED what the new engine had proved.
        case IRT_ARRAY: ir_ctype(t->elem, o); fputc('*', o); break;
        default:       fputs("void*", o); break;
    }
}


// A Lain name that happens to be a C KEYWORD cannot be emitted verbatim: `func double(x i32)`
// produced `int32_t double(int32_t)`, which is not a declaration at all. The old backend hid
// this behind module-qualified mangling; the new one emits the IR name, so the escape belongs
// here. An EXTERN can never need it — no C symbol is named `double` — so escaping every
// keyword is safe without knowing whether the callee has a body, which is what lets call
// sites (where the module is not in hand) agree with definitions.
static int ir_name_is_c_keyword(const IrName *n) {
    static const char *kw[] = {
        "auto","break","case","char","const","continue","default","do","double","else","enum",
        "extern","float","for","goto","if","inline","int","long","register","restrict","return",
        "short","signed","sizeof","static","struct","switch","typedef","union","unsigned","void",
        "volatile","while","bool","true","false","complex","imaginary","alignas","alignof",
        "atomic","generic","noreturn","static_assert","thread_local","main",NULL };
    if (!n) return 0;
    for (int i=0; kw[i]; i++) {
        int L = (int)strlen(kw[i]);
        if (n->length==L && memcmp(n->name, kw[i], (size_t)L)==0) return 1;
    }
    return 0;
}
static void ir_emit_fname(const IrName *n, FILE *o) {
    if (!n) { fputs("__anon", o); return; }
    fprintf(o, "%.*s%s", (int)n->length, n->name, ir_name_is_c_keyword(n) ? "_" : "");
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
        else if (ch=='\n') fputs("\\n", o);
        else if (ch=='\t') fputs("\\t", o);
        else if (ch=='\r') fputs("\\r", o);
        else if (ch>=0x20 && ch<=0x7e) fputc(ch, o);
        else fprintf(o, "\\%03o", ch);   // 3 digits: never ambiguous next to a digit
    }
    fputc('"', o);
}



// ── THE SLICE CALLING CONVENTION: (length, pointer), not a fat struct ────────────────────
// A slice is two machine words either way, so this costs nothing at the ABI — both forms pass
// in two registers on every target that matters. What it BUYS is the one annotation the fat
// struct makes unreachable: `access(read_only, p, n)` names PARAMETER POSITIONS, telling gcc
// that parameter p is a pointer to n elements and is only read. With the length hidden inside
// a struct there is no position to name, and `annot_gate` measured the result — 38 of them in
// the old backend, zero here.
//
// It is also what README §8 documents as the product's C interface, and what the old emitter
// has always produced. The IR is untouched: a slice stays a first-class fat value there, and
// this is purely the backend's choice about how one crosses a function boundary — which is
// exactly where a calling convention belongs.
//
// The rule is UNIFORM, which is what makes it implementable without every call site looking up
// its callee: a slice PARAMETER becomes two parameters, and a slice ARGUMENT becomes two
// arguments. The two halves cannot disagree because neither consults anything but the type in
// front of it.
static bool ir_c_slice_split(const IrType *t) { return t && t->kind == IRT_SLICE; }

// Is this value a slice PARAMETER — i.e. one that arrived split? Set for the duration of one
// function's emission, because the instruction emitter has no other way to tell a parameter
// from an ordinary SSA value, and that is exactly what decides whether `.len` exists to read.
static bool *ir_emit_sliceparam = NULL;
static int   ir_emit_sliceparam_n = 0;
// The module, so a CALL can ask whether its callee is an `extern`. See ir_c_extern_slice.
static IrFunc *ir_emit_mod = NULL;
static IrFunc *ir_emit_find(const IrName *n) {
    if (!n || !ir_emit_mod) return NULL;
    for (IrFunc *g=ir_emit_mod; g; g=g->next)
        if (g->name && g->name->length==n->length
            && memcmp(g->name->name, n->name, (size_t)n->length)==0) return g;
    return NULL;
}

// ── AN EXTERN'S ABI IS NOT OURS TO CHOOSE ───────────────────────────────────────────────
// The (length, pointer) convention is Lain's, and it applies to functions Lain emits. An
// `extern` is a declaration of something the foreign world already defines, so its shape is
// fixed by whoever wrote it — and a slice parameter there means a plain pointer, which is what
// `extern proc printf(fmt u8[:0], ...)` denotes.
//
// Getting this wrong was a SEGFAULT, and instructively so: splitting `printf`'s parameter
// passed the LENGTH as the format string. It had also been latently wrong BEFORE the split —
// the fat struct was passed by value and `Slice_u8` happens to store `data` first, so the right
// word landed in the right register BY ACCIDENT. The convention only became visible when it
// changed.
static bool ir_c_extern_slice(IrFunc *callee) { return callee && callee->is_extern; }
static bool ir_emit_is_slice_param(const IrValue *v) {
    return v && ir_emit_sliceparam && v->id >= 0 && v->id < ir_emit_sliceparam_n
        && ir_emit_sliceparam[v->id];
}


static void ir_emit_instr_c(IrInstr *i, FILE *o) {
    switch (i->op) {
        case IR_CONST:
            if (i->result->type && i->result->type->kind==IRT_FLOAT)
                 fprintf(o, "  v%d = %.17g;\n", i->result->id, i->aux.fimm);
            else fprintf(o, "  v%d = %lld;\n", i->result->id, (long long)i->aux.imm);
            break;
        case IR_ALLOCA: // array decays to its element base; scalar takes the slot address
            if (i->n_operands >= 1) {   // DYNAMIC: `count` elements, allocated in this frame
                fprintf(o, "  v%d = (", i->result->id);
                ir_ctype(i->aux.alloca_ty, o);
                fprintf(o, "*)__builtin_alloca(v%d * sizeof(", i->operands[0]->id);
                ir_ctype(i->aux.alloca_ty, o); fputs("));\n", o);
                break;
            }
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
            // An ARRAY field is already a base pointer once named — C decays it — so taking
            // its address would be one indirection too many.
            const char *amp = (i->result->type && i->result->type->kind==IRT_ARRAY) ? "" : "&";
            if (fn) fprintf(o, "  v%d = %sv%d->%.*s;\n", i->result->id, amp, i->operands[0]->id,
                            (int)fn->length, fn->name);
            else    fprintf(o, "  v%d = %sv%d->f%d;\n", i->result->id, amp, i->operands[0]->id, i->aux.field_idx);
            break;
        }
        case IR_ASSUME: case IR_ASSERT: case IR_CONSUME: break;  // verification-only; no runtime code
        // A WIDE access reads/writes more than the pointer's element type: a 16-lane vector
        // load through a `uint8_t*`. `*p` is a type error there, and a cast would assert an
        // alignment the address need not have — memcpy is the well-defined spelling and gcc
        // turns it into the single instruction anyway.
        case IR_LOAD:
            if (i->result->type && i->result->type->kind==IRT_VECTOR
                && i->operands[0]->type && i->operands[0]->type->elem
                && i->operands[0]->type->elem->kind != IRT_VECTOR)
                 fprintf(o, "  __builtin_memcpy(&v%d, v%d, sizeof v%d);\n",
                         i->result->id, i->operands[0]->id, i->result->id);
            else fprintf(o, "  v%d = *v%d;\n", i->result->id, i->operands[0]->id);
            break;
        case IR_STORE:
            if (i->operands[1]->type && i->operands[1]->type->kind==IRT_VECTOR
                && i->operands[0]->type && i->operands[0]->type->elem
                && i->operands[0]->type->elem->kind != IRT_VECTOR)
                 fprintf(o, "  __builtin_memcpy(v%d, &v%d, sizeof v%d);\n",
                         i->operands[0]->id, i->operands[1]->id, i->operands[1]->id);
            else fprintf(o, "  *v%d = v%d;\n", i->operands[0]->id, i->operands[1]->id);
            break;
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
        case IR_SHAPE: case IR_INIT:
        case IR_BORROW: case IR_BORROW_END: break;   // FACTS about a place; no runtime effect
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
        case IR_VEC_MOVEMASK: {   // the ISA's own lane-predicate reduction
            IrType *vt = i->operands[0]->type;
            int lanes = vt ? (int)vt->array_len : 16;
            fprintf(o, "  v%d = (uint32_t)%s((%s)v%d);\n", i->result->id,
                    lanes >= 32 ? "_mm256_movemask_epi8" : "_mm_movemask_epi8",
                    lanes >= 32 ? "__m256i" : "__m128i", i->operands[0]->id);
            break;
        }
        case IR_FUNC_REF: fprintf(o, "  v%d = ", i->result->id);
                          ir_emit_fname(i->aux.callee, o); fputs(";\n", o); break;
        // Two slices are equal iff same length and same bytes. `memcmp` is the C spelling of
        // the primitive; the length test comes first so a zero-length compare never reads.
        case IR_SEQ_EQ:
            fprintf(o, "  v%d = (v%d.len == v%d.len && __builtin_memcmp(v%d.data, v%d.data, v%d.len) == 0);\n",
                    i->result->id, i->operands[0]->id, i->operands[1]->id,
                    i->operands[0]->id, i->operands[1]->id, i->operands[0]->id);
            break;
        case IR_STRUCT_NEW: {
            // An ARRAY field cannot be initialised from a pointer in a C compound literal, and
            // the IR's uniform model gives every array value as its decayed base. So brace the
            // array fields empty and COPY them in — which is what `M([10,20,30,40], 4)` means.
            IrType *sty = i->result->type;
            fprintf(o, "  v%d = (", i->result->id); ir_ctype(sty, o); fputs("){ ", o);
            for (int k=0;k<i->n_operands;k++){
                if (k) fputs(", ", o);
                IrType *ft = (sty && sty->kind==IRT_STRUCT && k < sty->n_fields) ? sty->fields[k] : NULL;
                if (ft && ft->kind==IRT_ARRAY) fputs("{0}", o);
                else fprintf(o, "v%d", i->operands[k]->id);
            }
            fputs(" };\n", o);
            for (int k=0;k<i->n_operands;k++){
                IrType *ft = (sty && sty->kind==IRT_STRUCT && k < sty->n_fields) ? sty->fields[k] : NULL;
                if (!ft || ft->kind!=IRT_ARRAY) continue;
                IrName *fn = sty->field_names ? sty->field_names[k] : NULL;
                fputs("  __builtin_memcpy(v", o); fprintf(o, "%d.", i->result->id);
                if (fn) fprintf(o, "%.*s", (int)fn->length, fn->name); else fprintf(o, "f%d", k);
                fprintf(o, ", v%d, sizeof v%d.", i->operands[k]->id, i->result->id);
                if (fn) fprintf(o, "%.*s", (int)fn->length, fn->name); else fprintf(o, "f%d", k);
                fputs(");\n", o);
            }
            break;
        }
        case IR_ICMP: {
            // Comparing a vector to a SCALAR broadcasts it, but gcc requires the scalar to be
            // at the LANE type: `v == 3` with an i32 literal against a u8 vector is rejected
            // as a truncating conversion. Cast it.
            IrType *lt = i->operands[0]->type, *rt2 = i->operands[1]->type;
            fprintf(o, "  v%d = ", i->result->id);
            // A VECTOR comparison in gcc yields a SIGNED integer vector of the same width,
            // whatever the operands' lane signedness — so a `u8x16` compare produces
            // `__vector(16) signed char` and the next `|` against the declared `Vec_16_u8` is
            // a hard type error. The IR is right that the result is a lane-shaped mask; the
            // SIGNEDNESS of gcc's mask is a backend detail, so the backend converts it.
            if (i->result->type && i->result->type->kind==IRT_VECTOR) {
                fputc('(', o); ir_ctype(i->result->type, o); fputc(')', o);
            }
            fputc('(', o);
            if (lt && lt->kind==IRT_VECTOR && rt2 && rt2->kind!=IRT_VECTOR) {
                fprintf(o, "v%d %s (", i->operands[0]->id, ir_cmp_c(i->aux.cmp));
                ir_ctype(lt->elem, o); fprintf(o, ")v%d);\n", i->operands[1]->id);
            } else if (rt2 && rt2->kind==IRT_VECTOR && lt && lt->kind!=IRT_VECTOR) {
                fputc('(', o); ir_ctype(rt2->elem, o);
                fprintf(o, ")v%d %s v%d);\n", i->operands[0]->id, ir_cmp_c(i->aux.cmp), i->operands[1]->id);
            } else {
                fprintf(o, "v%d %s v%d);\n", i->operands[0]->id, ir_cmp_c(i->aux.cmp), i->operands[1]->id);
            }
            break;
        }
        case IR_NEG:    fprintf(o, "  v%d = -v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_BNOT:   fprintf(o, "  v%d = ~v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_CAST:   fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                        fprintf(o, ")v%d;\n", i->operands[0]->id); break;
        case IR_CALL: {
            fputs("  ", o);
            if (i->result) fprintf(o, "v%d = ", i->result->id);
            // INDIRECT (no name): operand 0 is the callee value and the rest are arguments.
            int a0 = 0;
            if (i->aux.callee) ir_emit_fname(i->aux.callee, o);
            else if (i->n_operands >= 1) { fprintf(o, "v%d", i->operands[0]->id); a0 = 1; }
            fputc('(', o);
            for (int k=a0;k<i->n_operands;k++){
                if(k>a0)fputs(", ",o);
                IrValue *av = i->operands[k];
                // The other half of the convention: a slice argument is (length, pointer), in
                // that order, matching the parameter list. Neither side consults the other —
                // both read the type in front of them — which is what keeps them in step.
                // The one exception is a foreign callee, whose shape we do not get to pick.
                if (av && ir_c_slice_split(av->type)) {
                    if (ir_c_extern_slice(ir_emit_find(i->aux.callee)))
                        fprintf(o, "v%d.data", av->id);
                    else
                        fprintf(o, "v%d.len, v%d.data", av->id, av->id);
                } else
                    fprintf(o,"v%d",av->id);
            }
            fputs(");\n", o);
            break;
        }
        default:
            if (i->n_operands == 2 && i->result) {
                fprintf(o, "  v%d = ", i->result->id);
                // Same reason as the vector comparison above: a mask's LANE SIGNEDNESS is
                // gcc's choice, not the IR's, so combining two masks with `|` can arrive at a
                // declared type that differs only in signedness. The IR agrees on the shape;
                // the backend reconciles the spelling.
                if (i->result->type && i->result->type->kind==IRT_VECTOR) {
                    fputc('(', o); ir_ctype(i->result->type, o); fputc(')', o);
                }
                fprintf(o, "(v%d %s v%d);\n",
                        i->operands[0]->id, ir_arith_c(i->op), i->operands[1]->id);
            }
            break;
    }
}

static void ir_emit_func_c(IrFunc *f, IrFunc *mod, FILE *o, Arena *a) {
    bool is_main = f->name->length==4 && strncmp(f->name->name,"main",4)==0;
    // signature
    if (is_main) fputs("int main(void)", o);
    else {
        IrCAnnot an = ir_c_annot(f, mod);
        if (an.const_attr)     fputs("__attribute__((const)) ", o);
        else if (an.pure_attr) fputs("__attribute__((pure)) ", o);
        // A borrow can never be null, and gcc uses that to delete null tests and propagate
        // non-nullness into callers. Theorems, not promises — see annot.h.
        if (ir_func_c_nonnull(f))         fputs("__attribute__((nonnull)) ", o);
        if (ir_func_c_returns_nonnull(f)) fputs("__attribute__((returns_nonnull)) ", o);
        { IrCAccess ac[16]; int na = ir_c_access_list(f, mod, ac, 16);
          for (int q=0;q<na;q++)
              fprintf(o, "__attribute__((access(%s, %d, %d))) ",
                      ac[q].read_only ? "read_only" : "read_write", ac[q].ptr_pos, ac[q].len_pos); }
        ir_ctype(f->ret_type, o); fputc(' ', o); ir_emit_fname(f->name, o); fputc('(', o);
        int k=0; for (IrParam *p=f->params; p; p=p->next,k++) {
            if (k) fputs(", ", o);
            IrType *pt = p->value->type;
            if (ir_c_slice_split(pt)) {          // (length, pointer) — see the note above
                fprintf(o, "size_t __len_v%d, ", p->value->id);
                ir_ctype(pt->elem, o); fputs("*", o);
                if (ir_param_c_restrict(p->value)) fputs(" restrict", o);
                fprintf(o, " __ptr_v%d", p->value->id);
                continue;
            }
            ir_ctype(pt, o);
            if (pt && (pt->kind==IRT_PTR || pt->kind==IRT_ARRAY) && ir_param_c_restrict(p->value))
                fputs(" restrict", o);
            fprintf(o, " v%d", p->value->id);
        }
        if (!f->params) fputs("void", o);
        fputc(')', o);
    }
    fputs(" {\n", o);
    // declare all non-param values at the top, plus a backing slot for each alloca
    int nval = f->next_value_id > 0 ? f->next_value_id : 1;   // see ir_emit_type_decls
    IrValTab vt = { arena_push_many_aligned(a, IrValue*, nval), f->next_value_id };
    IrType **alloca_ty = arena_push_many_aligned(a, IrType*, nval);
    IrInstr **defof    = arena_push_many_aligned(a, IrInstr*, nval);
    for (int k=0;k<vt.n;k++){ vt.v[k]=NULL; alloca_ty[k]=NULL; defof[k]=NULL; }
    ir_collect_vals(f, &vt);
    // Mark the slice PARAMETERS for this function — they arrived split as (length, pointer).
    { bool *sp = arena_push_many_aligned(a, bool, f->next_value_id>0?f->next_value_id:1);
      for (int q=0;q<f->next_value_id;q++) sp[q]=false;
      for (IrParam *p=f->params; p; p=p->next)
          if (p->value && ir_c_slice_split(p->value->type) && p->value->id>=0
              && p->value->id < f->next_value_id) sp[p->value->id]=true;
      ir_emit_sliceparam = sp; ir_emit_sliceparam_n = f->next_value_id; }
    ir_emit_mod = mod;                 // so a CALL can ask whether its callee is an extern
    for (IrBlock *b=f->blocks; b; b=b->next)
        for (IrInstr *i=b->instrs; i; i=i->next) {
            if (i->result && i->result->id < f->next_value_id) defof[i->result->id] = i;
            if (i->op==IR_ALLOCA && i->result) alloca_ty[i->result->id] = i->aux.alloca_ty;
        }
    // Which parameter (if any) is a given value? Needed to decide the slice-data qualifier.
    int *pidx = arena_push_many_aligned(a, int, nval);
    for (int k=0;k<vt.n;k++) pidx[k] = -1;
    { int k=0; for (IrParam *p=f->params; p; p=p->next,k++)
        if (p->value && p->value->id < f->next_value_id) pidx[p->value->id] = k; }
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
            // ★ A SLICE's DATA POINTER is where `restrict` actually lands. A slice is passed
            // as a by-value struct, so the parameter itself cannot carry the qualifier — but
            // the pointer every access goes through is this SSA value, and qualifying its
            // declaration says exactly the right thing over exactly the right block. This is
            // the annotation that pays: on `out[i] = a[i] + b[i]` it is the difference
            // between one vectorized loop and TWO plus a runtime overlap test.
            IrInstr *d = defof[id];
            int sp = (d && d->op==IR_SLICE_DATA && d->n_operands>=1 && d->operands[0])
                     ? pidx[d->operands[0]->id] : -1;
            if (sp >= 0 && ir_param_c_restrict(d->operands[0])) {
                fputs("  ", o);
                ir_ctype(v->type && v->type->elem ? v->type->elem : v->type, o);
                fprintf(o, "* restrict v%d;\n", id);
            } else {
                fputs("  ", o); ir_ctype(v->type, o); fprintf(o, " v%d;\n", id);
            }
        }
    }
    // ── REASSEMBLE EACH SPLIT SLICE PARAMETER ───────────────────────────────────────────
    // The split is a CALLING CONVENTION, not a change to what a slice IS. Inside the body a
    // slice parameter is still a first-class value: it gets returned, passed on, compared,
    // stored. Rebuilding it here under its own name means not one instruction in the body has
    // to know the convention exists — `vN.len`, `vN.data` and `return vN` all keep working.
    //
    // The first attempt did the opposite: it left the pointer under the value's name and
    // taught IR_SLICE_LEN / IR_SLICE_DATA to read the two halves. That handles reads and
    // nothing else, so twenty programs stopped building the moment a slice parameter was used
    // AS A VALUE — returned, or handed to `panic`. Reassembling costs two register moves gcc
    // deletes, and it costs no special cases at all.
    for (IrParam *p=f->params; p; p=p->next) {
        if (!p->value || !ir_c_slice_split(p->value->type)) continue;
        fputs("  ", o); ir_ctype(p->value->type, o);
        fprintf(o, " v%d = { __ptr_v%d, __len_v%d };\n",
                p->value->id, p->value->id, p->value->id);
    }
    // blocks
    for (IrBlock *b=f->blocks; b; b=b->next) {
        fprintf(o, " L%d: ;\n", b->id);
        for (IrInstr *i=b->instrs; i; i=i->next) ir_emit_instr_c(i, o);
        switch (b->term.kind) {
            // IR_TERM_BR is the ZERO value of the enum, so an UNTERMINATED block reads as a
            // branch to nowhere. Emitting `goto L(null)` crashed the emitter; the honest
            // response is a well-defined dead end that is visibly wrong if it is ever reached,
            // not a segfault in the compiler.
            case IR_TERM_BR:
                if (b->term.a) fprintf(o, "  goto L%d;\n", b->term.a->id);
                else fputs("  /* unterminated block */\n", o);
                break;
            case IR_TERM_BR_COND:
                if (b->term.cond && b->term.a && b->term.b)
                    fprintf(o, "  if (v%d) goto L%d; else goto L%d;\n",
                            b->term.cond->id, b->term.a->id, b->term.b->id);
                else fputs("  /* malformed conditional */\n", o);
                break;
            case IR_TERM_RET:     if (b->term.cond) fprintf(o, "  return v%d;\n", b->term.cond->id);
                                  else fputs(is_main ? "  return 0;\n" : "  return;\n", o); break;
            case IR_TERM_UNREACHABLE:
                // Unreachable, but C still needs a well-typed exit: `return 0` is a type error
                // the moment the function returns a struct or a union. A zero compound literal
                // is valid for any type and never executes.
                if (is_main || !f->ret_type || f->ret_type->kind==IRT_UNIT || f->ret_type->kind==IRT_NEVER)
                    fputs(is_main ? "  return 0;\n" : "  return;\n", o);
                else if (f->ret_type->kind==IRT_INT || f->ret_type->kind==IRT_BOOL
                      || f->ret_type->kind==IRT_PTR || f->ret_type->kind==IRT_FUNC)
                    fputs("  return 0;\n", o);
                else { fputs("  return (", o); ir_ctype(f->ret_type, o); fputs("){0};\n", o); }
                break;
            default: break;
        }
    }
    fputs("}\n\n", o);
}

// forward declaration line for a function (so callers link regardless of order)


static void ir_emit_proto_c(IrFunc *f, IrFunc *mod, FILE *o) {
    if (f->name->length==4 && strncmp(f->name->name,"main",4)==0) return;
    IrCAnnot an = ir_c_annot(f, mod);
    if (an.const_attr)     fputs("__attribute__((const)) ", o);
    else if (an.pure_attr) fputs("__attribute__((pure)) ", o);
    // The PROTOTYPE must carry the same attributes as the definition — it is the only thing a
    // caller in another translation unit ever sees, and it is where these facts do their work.
    if (ir_func_c_nonnull(f))         fputs("__attribute__((nonnull)) ", o);
    if (ir_func_c_returns_nonnull(f)) fputs("__attribute__((returns_nonnull)) ", o);
    { IrCAccess ac[16]; int na = ir_c_access_list(f, mod, ac, 16);
      for (int q=0;q<na;q++)
          fprintf(o, "__attribute__((access(%s, %d, %d))) ",
                  ac[q].read_only ? "read_only" : "read_write", ac[q].ptr_pos, ac[q].len_pos); }
    ir_ctype(f->ret_type, o); fputc(' ', o); ir_emit_fname(f->name, o); fputc('(', o);
    int k=0; for (IrParam *p=f->params; p; p=p->next,k++){
        if(k)fputs(", ",o);
        IrType *pt = p->value->type;
        if (ir_c_slice_split(pt)) {              // (length, pointer) — see the note above
            if (f->is_extern) {                  // ...but a foreign declaration keeps its shape
                ir_ctype(pt->elem, o); fputs("*", o);
                continue;
            }
            fputs("size_t, ", o); ir_ctype(pt->elem, o); fputs("*", o);
            if (ir_param_c_restrict(p->value)) fputs(" restrict", o);
            continue;
        }
        ir_ctype(pt,o);
        if (pt && (pt->kind==IRT_PTR || pt->kind==IRT_ARRAY) && ir_param_c_restrict(p->value))
            fputs(" restrict", o);
    }
    // `...` — without it every call to printf and friends is an implicit declaration, and the
    // generated C does not compile at all.
    if (f->is_variadic) fputs(k ? ", ..." : "...", o);
    else if (!f->params) fputs("void", o);
    fputs(");\n", o);
}

// A set of the composite types the module needs C declarations for, gathered
// transitively so a struct that appears only as a slice element / pointer pointee
// / another struct's field is still declared.
typedef struct { IrType *structs[256]; int n_struct; IrType *slices[256]; int n_slice;
                 IrType *vecs[64];     int n_vec;
                 IrType *fns[64];      int n_fn; } IrTypeSet;
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
        case IRT_FUNC: {     // one typedef per distinct signature
            char mine[192]; { int k = snprintf(mine,sizeof mine,"Fn_");
                k += ir_slice_tag(t->elem, mine+k, (int)sizeof mine - k);
                for (int i2=0;i2<t->n_fields;i2++) { k += snprintf(mine+k,sizeof mine-(size_t)k,"_");
                    k += ir_slice_tag(t->fields[i2], mine+k, (int)sizeof mine - k); } }
            for (int i2=0;i2<ts->n_fn;i2++) {
                char other[192]; { IrType *f2=ts->fns[i2]; int k = snprintf(other,sizeof other,"Fn_");
                    k += ir_slice_tag(f2->elem, other+k, (int)sizeof other - k);
                    for (int j=0;j<f2->n_fields;j++) { k += snprintf(other+k,sizeof other-(size_t)k,"_");
                        k += ir_slice_tag(f2->fields[j], other+k, (int)sizeof other - k); } }
                if (strcmp(mine, other)==0) return;
            }
            ir_ts_visit(ts, t->elem);
            for (int i2=0;i2<t->n_fields;i2++) ir_ts_visit(ts, t->fields[i2]);
            if (ts->n_fn < 64) ts->fns[ts->n_fn++] = t;
            break;
        }
        case IRT_VECTOR: {   // one vector_size typedef per (lanes, lane type)
            for (int i=0;i<ts->n_vec;i++)
                if (ts->vecs[i]->array_len==t->array_len && ts->vecs[i]->elem
                    && t->elem && ts->vecs[i]->elem->kind==t->elem->kind
                    && ts->vecs[i]->elem->bits==t->elem->bits
                    && ts->vecs[i]->elem->is_signed==t->elem->is_signed) return;
            if (ts->n_vec < 64) ts->vecs[ts->n_vec++] = t;
            break;
        }
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
// This is a BACKEND decision — the IR records only which variants exist and what they
// carry (local/internal/design/ir_sum_types.md §3) — so swapping in a niche packing later
// touches only this file. A payload-less variant contributes nothing to the union; if no
// variant carries a payload
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
        // ★ AN EXTERN HAS NO VALUES. `next_value_id` is 0 for a declaration with no body, and
        // the arena refuses a zero-count push — so a program whose only aggregates came from
        // an extern's signature aborted the compiler here rather than emitting anything.
        // `mov p *u8 = acquire()` with `extern proc acquire() mov *u8` is the whole program.
        //
        // The guard is the count, not the extern-ness: a body-less function is one way to have
        // no values and there is no reason to enumerate the others.
        if (f->next_value_id <= 0) continue;
        IrValTab vt = { arena_push_many_aligned(a, IrValue*, f->next_value_id), f->next_value_id };
        for (int k=0;k<vt.n;k++) vt.v[k]=NULL;
        ir_collect_vals(f, &vt);
        for (int id=0; id<vt.n; id++) if (vt.v[id]) ir_ts_visit(&ts, vt.v[id]->type);
    }
    for (int i=0;i<ts.n_struct;i++){ IrName *nm=ts.structs[i]->sname;
        fprintf(o, "typedef struct %.*s %.*s;\n", (int)nm->length, nm->name, (int)nm->length, nm->name); }
    for (int i=0;i<ts.n_fn;i++) {          // function-pointer typedefs
        IrType *ft = ts.fns[i];
        fputs("typedef ", o);
        if (ft->elem) ir_ctype(ft->elem, o); else fputs("void", o);
        fputs(" (*", o); ir_ctype(ft, o); fputs(")(", o);
        for (int k=0;k<ft->n_fields;k++) { if (k) fputs(", ", o); ir_ctype(ft->fields[k], o); }
        if (!ft->n_fields) fputs("void", o);
        fputs(");\n", o);
    }
    if (ts.n_fn) fputc('\n', o);
    // SIMD vectors first: they are primitives, and a slice or struct may contain one.
    for (int i=0;i<ts.n_vec;i++) {
        IrType *v = ts.vecs[i];
        int lanes = (int)v->array_len;
        int bytes = lanes * ((v->elem && v->elem->bits) ? v->elem->bits/8 : 4);
        char tag[64]; ir_slice_tag(v->elem, tag, sizeof tag);
        fputs("typedef ", o); ir_ctype(v->elem, o);
        fprintf(o, " Vec_%d_%s __attribute__((vector_size(%d)));\n", lanes, tag, bytes);
    }
    if (ts.n_vec) fputc('\n', o);
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
    // `panic` is a declless builtin — it has no Lain declaration, so nothing declares it in
    // the generated C either and every `else panic(...)` failed to LINK. The old backend
    // inlines fprintf+abort at the site; emit one helper instead, and only when it is used,
    // so a module that never panics is unchanged.
    { bool needs_x86 = false;                 // only when a movemask is actually emitted
      for (IrFunc *f=funcs; f && !needs_x86; f=f->next)
        for (IrBlock *b=f->blocks; b && !needs_x86; b=b->next)
          for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_VEC_MOVEMASK) { needs_x86 = true; break; }
      if (needs_x86) fputs("#include <immintrin.h>\n\n", o); }
    { IrInstr *pc = NULL;
      for (IrFunc *f=funcs; f && !pc; f=f->next)
        for (IrBlock *b=f->blocks; b && !pc; b=b->next)
          for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_CALL && i->aux.callee && i->aux.callee->length==5
                && memcmp(i->aux.callee->name,"panic",5)==0) { pc = i; break; }
      if (pc) {
        // Declared, not #included: pulling in <stdio.h> makes the emitted extern for
        // `libc_printf` collide with the real printf once the harness maps one to the other.
        fputs("extern void abort(void);\n", o);
        ir_ctype(pc->result ? pc->result->type : NULL, o);
        fputs(" panic(", o);
        // ★ The runtime helper obeys the SAME convention as every other function. It is
        // hand-written here rather than lowered, so it does not get the split for free — and
        // that is exactly how a convention rots: one function that "obviously" does not need
        // it, and every call site that passes it a slice stops compiling. Four programs did.
        IrType *mt = (pc->n_operands >= 1 && pc->operands[0]) ? pc->operands[0]->type : NULL;
        if (mt && ir_c_slice_split(mt)) {
            fputs("size_t __len_m, ", o); ir_ctype(mt->elem, o); fputs("* __ptr_m", o);
            fputs(") { (void)__len_m; (void)__ptr_m; abort(); }\n\n", o);
        } else {
            if (mt) ir_ctype(mt, o); else fputs("void", o);
            fputs(" m) { (void)m; abort(); }\n\n", o);
        }
      } }
    for (IrFunc *f=funcs; f; f=f->next) ir_emit_proto_c(f, funcs, o);
    fputc('\n', o);
    // An EXTERN has no body: emitting one would define printf locally and collide with libc.
    for (IrFunc *f=funcs; f; f=f->next) if (!f->is_extern) ir_emit_func_c(f, funcs, o, a);
}

#endif // LAIN_IR_EMIT_C_H
