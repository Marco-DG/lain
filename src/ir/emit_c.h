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
#include "layout.h"  // D-62: how a sum is REPRESENTED — decided once, below the IR

static void ir_emit_decl_attrs(const IrFunc *f, FILE *o);   // fwd: [cold]/[hot]/[allocator]/[noreturn]

// round a non-standard integer width up to a standard C width
static int ir_c_stdbits(int bits) { return ir_int_storage_bits(bits); }

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

// ★ THE BACKING TYPE MUST BE ABLE TO HOLD ITS OWN SENTINELS, and for `bool` the obvious
// choice cannot. C's `_Bool` normalises every nonzero store to 1, so a niche over a bool —
// whose spare patterns are 2..255 — writes sentinel 2 and reads back 1, which is `true`.
// It even looks right on the marker path, because the comparison normalises identically:
// `x == (_Bool)2` is `x == 1`. The collision only shows when the payload IS true, and the
// corpus happened to test only the other path. The old backend widens to uint8_t; so does
// this. (`ir_ctype` prints the SEMANTIC type, which is still bool — this is storage.)
static void ir_layout_backing_ctype(const IrType *back, FILE *o) {
    if (back && back->kind == IRT_BOOL) { fputs("uint8_t", o); return; }
    ir_ctype(back, o);
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
// ── C'S INTEGER PROMOTIONS ARE PART OF THE TARGET, NOT AN OPTIMIZATION DETAIL ────────────
// `uint16_t a = 60000, b = 60000; uint32_t p = a * b;` is UNDEFINED in C: both operands
// promote to `int`, and 3.6e9 does not fit there. The value Lain proved — a u32 product that
// fits — is never computed; UBSan traps on it, and that is how the trust harness found this
// the day the IR backend became the default.
//
// A WIDENING operation must therefore be spelled as one: cast each operand up to the result
// type so the multiply happens in the type the IR says it happens in. The same-width case
// needs nothing — the engine has already proven the result fits, so the promoted computation
// and the declared one agree — and narrowing arithmetic is not produced.
static void ir_arith_operand_c(IrInstr *i, int k, FILE *o) {
    IrType *rt = i->result ? i->result->type : NULL;
    IrType *ot = i->operands[k]->type;
    if (rt && ot && rt->kind == IRT_INT && ot->kind == IRT_INT && rt->bits > ot->bits) {
        fputc('(', o); ir_ctype(rt, o); fputc(')', o);
    }
    // A SCALAR operand of a vector operation takes the LANE type, as the vector comparison's
    // does: `(x >> 4) & 15` on a u8x16 held the 15 in an int32_t, and gcc and clang both refuse
    // "conversion of scalar int32_t to vector involves truncation" for a variable operand.
    // A float scalar too: a float literal is an f64, so `v * 2.0` on an f32x4 put a `double`
    // beside a float vector, and gcc refuses that the same way.
    if (rt && ot && rt->kind == IRT_VECTOR && rt->elem &&
        (ot->kind == IRT_INT || ot->kind == IRT_FLOAT)) {
        fputc('(', o); ir_ctype(rt->elem, o); fputc(')', o);
    }
    fprintf(o, "v%d", i->operands[k]->id);
}

// The unsigned vector type of a vector's shape, spelled inline: `uint32_t
// __attribute__((vector_size(16)))`. Used to make a signed lane's arithmetic wrap.
static void ir_vec_unsigned_ctype(const IrType *vt, FILE *o) {
    int bits = vt->elem ? vt->elem->bits : 32;
    fprintf(o, "uint%d_t __attribute__((vector_size(%d)))", bits, (int)vt->array_len * bits / 8);
}
static const char *ir_arith_c(IrOp op) {
    switch (op) { case IR_ADD:return "+"; case IR_SUB:return "-"; case IR_MUL:return "*";
        case IR_SDIV: case IR_UDIV:return "/"; case IR_SREM: case IR_UREM:return "%";
        case IR_AND:return "&"; case IR_OR:return "|"; case IR_XOR:return "^";
        case IR_SHL:return "<<"; case IR_LSHR: case IR_ASHR:return ">>"; default:return "+"; }
}

// ── WRAPPING ARITHMETIC MUST NOT BE C SIGNED OVERFLOW ───────────────────────────────────────
// `+% -% *%` promise the result modulo 2^N. They were emitted as a plain `(a + b)`, which on a
// signed C type is UNDEFINED when it overflows — UBSan: "2147483647 + 1 cannot be represented
// in type 'int'" — so the operator that exists to make overflow defined compiled to the one
// construct C leaves undefined; gcc may assume it never happens (loop reasoning, `x + 1 > x`).
// The emitted C is compiled with no `-fwrapv` (the documented recipe is a plain `gcc`). An
// unsigned u16 `*%` was undefined too: both operands promote to `int` and 60000 * 60000 does not
// fit there.
//
// So: compute in an UNSIGNED type at least as wide as `unsigned int` (where C defines wrap
// around), then bring the bits back — a plain conversion for a standard width (modular in every
// compiler this targets: gcc and clang define it), a mask for an odd unsigned width (u4), and a
// mask plus sign extension for an odd signed one (i7).
static void ir_emit_modular(IrInstr *i, FILE *o) {
    IrType *rt = i->result->type;
    int n = rt->bits > 0 ? rt->bits : 32;
    const char *U = n > 32 ? "uint64_t" : "uint32_t";
    bool std_w = (n == 8 || n == 16 || n == 32 || n == 64);
    unsigned long long mask = (n >= 64) ? ~0ULL : ((1ULL << n) - 1);
    unsigned long long sb = 1ULL << (n - 1);
    char core[160];
    snprintf(core, sizeof core, "(%s)v%d %s (%s)v%d", U, i->operands[0]->id, ir_arith_c(i->op),
             U, i->operands[1]->id);
    fprintf(o, "  v%d = (", i->result->id); ir_ctype(rt, o); fputs(")", o);
    if (std_w)                 fprintf(o, "(%s);\n", core);                              // (T)(a op b)
    else if (!rt->is_signed)   fprintf(o, "((%s) & 0x%llxULL);\n", core, mask);        // mask to N bits
    else                       fprintf(o, "((((%s) & 0x%llxULL) ^ 0x%llxULL) - 0x%llxULL);\n",
                                       core, mask, sb, sb);                             // + sign-extend
}

// `as%` and `as|` (F3.5): a cast with a POLICY for the value that does not fit. Both were the
// plain C conversion, which is neither: it does not clamp, and at an odd width it does not wrap
// either — the container is wider than the type (300 as% u4 stored 44).
//   as%  modular: through uint64_t (defined), masked and sign-extended at an odd width;
//   as|  clamp:   compared in the SOURCE's signedness, so no comparison mixes signs.
static void ir_emit_c_int128(__int128 v, FILE *o) {
    if (v > (__int128)INT64_MAX)      fprintf(o, "%lluULL", (unsigned long long)v);
    else if (v == (__int128)INT64_MIN) fputs("(-9223372036854775807LL - 1)", o);
    else                              fprintf(o, "%lldLL", (long long)v);
}
static void ir_emit_cast_policy(IrInstr *i, FILE *o) {
    IrType *st = i->operands[0]->type, *dt = i->result->type;
    int sb = st->bits > 0 ? st->bits : 32, db = dt->bits > 0 ? dt->bits : 32;
    int v = i->operands[0]->id, r = i->result->id;
    if (i->wrap == IR_WRAP_MODULAR) {
        bool std_w = (db == 8 || db == 16 || db == 32 || db == 64);
        unsigned long long mask = db >= 64 ? ~0ULL : ((1ULL << db) - 1), sbit = 1ULL << (db - 1);
        fprintf(o, "  v%d = (", r); ir_ctype(dt, o); fputs(")", o);
        if (std_w)              fprintf(o, "(uint64_t)v%d;\n", v);
        else if (!dt->is_signed) fprintf(o, "((uint64_t)v%d & 0x%llxULL);\n", v, mask);
        else                    fprintf(o, "((((uint64_t)v%d & 0x%llxULL) ^ 0x%llxULL) - 0x%llxULL);\n",
                                        v, mask, sbit, sbit);
        return;
    }
    __int128 smin = st->is_signed ? -((__int128)1 << (sb - 1)) : 0;
    __int128 smax = st->is_signed ? ((__int128)1 << (sb - 1)) - 1 : ((__int128)1 << sb) - 1;
    __int128 tmin = dt->is_signed ? -((__int128)1 << (db - 1)) : 0;
    __int128 tmax = dt->is_signed ? ((__int128)1 << (db - 1)) - 1 : ((__int128)1 << db) - 1;
    const char *cmp_t = st->is_signed ? "(int64_t)" : "(uint64_t)";
    fprintf(o, "  v%d = ", r);
    if (tmax < smax) {
        fprintf(o, "(%sv%d > ", cmp_t, v); ir_emit_c_int128(tmax, o); fputs(") ? (", o);
        ir_ctype(dt, o); fputs(")", o); ir_emit_c_int128(tmax, o); fputs(" : ", o);
    }
    if (tmin > smin) {                         // only a signed source reaches below 0
        fprintf(o, "(%sv%d < ", cmp_t, v); ir_emit_c_int128(tmin, o); fputs(") ? (", o);
        ir_ctype(dt, o); fputs(")", o); ir_emit_c_int128(tmin, o); fputs(" : ", o);
    }
    fputs("(", o); ir_ctype(dt, o); fprintf(o, ")v%d;\n", v);
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


// ── EMITTING A PACKED SUM ────────────────────────────────────────────────────────────────
// When ir_layout_of says a sum is packed, the VALUE IS THE PAYLOAD and the payload-less
// variants are spare bit patterns of it. So the three sum instructions stop being struct
// accesses and become, respectively: a comparison chain, the value itself, and a constant.
//
// The comparison goes through `uintptr_t` for a pointer backing because a sentinel is an
// integer and C will not compare it to a pointer otherwise — the same cast the old backend
// uses, and the same one a null-pointer store needs (D-38 tier 1).
static void ir_emit_sentinel_cmp(IrType *back, int vid, long long sent, FILE *o) {
    if (back && (back->kind == IRT_PTR || back->kind == IRT_SLICE))
        fprintf(o, "(uintptr_t)v%d == (uintptr_t)%lldull", vid, sent);
    else
        fprintf(o, "v%d == (", vid), ir_layout_backing_ctype(back, o), fprintf(o, ")%lldll", sent);
}

static IrInstr *ir_pk_of(IrValue *p);                  // [packed] field access (below)
static void ir_emit_packed_load(IrInstr *i, FILE *o);
static void ir_emit_packed_store(IrInstr *i, FILE *o);
// ★ A FACT THE COMPILER ESTABLISHED IS HANDED TO GCC. A refinement is checked at every call, a
// struct invariant at every construction and store, a callee's return range in the callee: at
// the assume it holds, and `if (!c) __builtin_unreachable()` lets gcc use it (a removed `n == 0`
// guard, a dropped defensive branch). From cf702c5 until this, nothing was emitted: the
// comparison went into a temporary nobody read, while README's "Where no optimiser can follow"
// quoted the deleted backend's hint. A TRUSTED assume (the programmer's `assume`, an extern's
// return range) is not handed on: a wrong one stays a wrong answer instead of becoming undefined
// behaviour gcc exploits, which is what the deleted backend did too.
// ONE HINT PER RUN, conjoined. gcc 13 turns an unreachable guard into a range only when no use
// of the variable precedes the guard, so after `if (!(len <= n)) unreachable` a following
// `if (!(n >= 4)) unreachable` was ignored and the zero-trip test stayed in `dot`; as one
// `if (!(a & b & c))` all three are used. A hint about SSA values may move LATER in its block,
// never earlier, so deferring it to the end of the run is sound.
static void ir_emit_hints(int *hint, int *nh, FILE *o) {
    if (*nh == 0) return;
    fputs("  if (!(", o);
    for (int k = 0; k < *nh; k++) fprintf(o, "%sv%d", k ? " & " : "", hint[k]);
    fputs(")) __builtin_unreachable();\n", o);
    *nh = 0;
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
        case IR_ELEM_PTR: {
            // A LANE of a vector: the base points at the vector, and `&p[k]` would be the k-th
            // VECTOR. The lane is `(lane *)p + k` (a vector type aliases its lane type in gcc
            // and clang). `&v[k]` on a vector VALUE is a gcc extension clang refuses.
            IrType *bt = i->operands[0]->type;
            IrType *vt = (bt && bt->kind==IRT_PTR && bt->elem && bt->elem->kind==IRT_VECTOR) ? bt->elem
                       : (bt && bt->kind==IRT_VECTOR) ? bt : NULL;
            if (vt && vt->elem) {
                fprintf(o, "  v%d = (", i->result->id); ir_ctype(vt->elem, o);
                fprintf(o, " *)%sv%d + v%d;\n", bt->kind==IRT_VECTOR ? "&" : "",
                        i->operands[0]->id, i->operands[1]->id);
                break;
            }
            fprintf(o, "  v%d = &v%d[v%d];\n", i->result->id, i->operands[0]->id, i->operands[1]->id);
            break;
        }
        case IR_FIELD_PTR: {   // base is a struct pointer; name the field from its type
            IrType *st = i->operands[0]->type ? i->operands[0]->type->elem : NULL;
            if (st && ir_struct_layout(st).packed) break;   // bits, not an address: see ir_pk_of
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
        case IR_ASSUME: case IR_ASSERT: case IR_CONSUME: break;  // verification-only; an established
                                                                 // assume's HINT: see ir_emit_hints
        // A WIDE access reads/writes more than the pointer's element type: a 16-lane vector
        // load through a `uint8_t*`. `*p` is a type error there, and a cast would assert an
        // alignment the address need not have — memcpy is the well-defined spelling and gcc
        // turns it into the single instruction anyway.
        case IR_LOAD:
            if (ir_pk_of(i->operands[0])) { ir_emit_packed_load(i, o); break; }
            if (i->result->type && i->result->type->kind==IRT_VECTOR
                && i->operands[0]->type && i->operands[0]->type->elem
                && i->operands[0]->type->elem->kind != IRT_VECTOR)
                 fprintf(o, "  __builtin_memcpy(&v%d, v%d, sizeof v%d);\n",
                         i->result->id, i->operands[0]->id, i->result->id);
            else fprintf(o, "  v%d = *v%d;\n", i->result->id, i->operands[0]->id);
            break;
        case IR_STORE:
            if (ir_pk_of(i->operands[0])) { ir_emit_packed_store(i, o); break; }
            if (i->operands[1]->type && i->operands[1]->type->kind==IRT_VECTOR
                && i->operands[0]->type && i->operands[0]->type->elem
                && i->operands[0]->type->elem->kind != IRT_VECTOR)
                 fprintf(o, "  __builtin_memcpy(v%d, &v%d, sizeof v%d);\n",
                         i->operands[0]->id, i->operands[1]->id, i->operands[1]->id);
            else {
                // ── A POINTER SLOT WRITTEN FROM AN INTEGER NEEDS THE CAST SPELLED ────────
                // `var p *i32 = 0` is a null pointer, and the IR says so exactly: a `const 0`
                // of integer type stored into a `*i32` slot. C does not accept that silently
                // — it is `-Wint-conversion`, "makes pointer from integer without a cast", and
                // this corpus promotes that warning to an ERROR on purpose, because the same
                // class WAS a live miscompile once (D-38 tier 1: a pointer stored in an int).
                //
                // The IR is right and the C spelling was incomplete. The cast is written from
                // the SLOT's type — the destination is what the value must become — and only
                // where the kinds actually disagree, so nothing else acquires a cast it does
                // not need.
                IrType *slot = i->operands[0]->type ? i->operands[0]->type->elem : NULL;
                IrType *val  = i->operands[1]->type;
                bool needs = slot && val && slot->kind != val->kind
                          && (slot->kind==IRT_PTR || slot->kind==IRT_FUNC);
                fputs("  *", o); fprintf(o, "v%d = ", i->operands[0]->id);
                // Through `uintptr_t`, because the integer is typically narrower than a
                // pointer and a direct cast is `-Wint-to-pointer-cast`. The old emitter has
                // always spelled it this way — `(const uint8_t *)(uintptr_t)0LL` is how a
                // niche-packed `none` is written — so this is the same idiom, not a new one.
                if (needs) { fputc('(', o); ir_ctype(slot, o); fputs(")(uintptr_t)", o); }
                fprintf(o, "v%d;\n", i->operands[1]->id);
            }
            break;
        case IR_SLICE_LEN:  fprintf(o, "  v%d = v%d.len;\n",  i->result->id, i->operands[0]->id); break;
        case IR_SLICE_DATA: fprintf(o, "  v%d = v%d.data;\n", i->result->id, i->operands[0]->id); break;
        case IR_MAKE_SLICE: fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                            fprintf(o, "){ v%d, v%d };\n", i->operands[0]->id, i->operands[1]->id); break;
        case IR_STR_CONST:  fprintf(o, "  v%d = (uint8_t*)", i->result->id);
                            ir_emit_cstr(i->aux.str.bytes, i->aux.str.len, o); fputs(";\n", o); break;
        case IR_SIZEOF: case IR_ALIGNOF: {
            // An ARRAY type has no C name to put inside sizeof(); its size is N elements, with
            // no padding between them, and its alignment is its element's.
            IrType *qt = i->aux.alloca_ty;
            int64_t n = 1;
            while (qt && qt->kind == IRT_ARRAY) { n *= (qt->array_len > 0 ? qt->array_len : 0); qt = qt->elem; }
            fprintf(o, "  v%d = (uint64_t)", i->result->id);
            if (i->op == IR_SIZEOF) fprintf(o, "%lld * ", (long long)n);
            fputs(i->op == IR_SIZEOF ? "sizeof(" : "_Alignof(", o);
            ir_ctype(qt, o); fputs(");\n", o);
            break;
        }
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

        case IR_SUM_TAG: {
            IrType *st = i->operands[0]->type;
            IrLayout L = ir_layout_of(st);
            if (L.packed && L.all_empty) {            // the value already IS the ordinal
                fprintf(o, "  v%d = (int32_t)v%d;\n", i->result->id, i->operands[0]->id);
                break;
            }
            if (L.packed) {
                // tag = (v == s_a) ? a : (v == s_b) ? b : <the payload variant>
                fprintf(o, "  v%d = ", i->result->id);
                for (int k = 0; k < st->n_fields; k++) {
                    if (!L.has_sentinel[k]) continue;
                    fputc('(', o);
                    ir_emit_sentinel_cmp(L.backing, i->operands[0]->id, L.sentinel[k], o);
                    fprintf(o, ") ? %d : ", k);
                }
                fprintf(o, "%d;\n", L.primary);
                break;
            }
            fprintf(o, "  v%d = v%d.tag;\n", i->result->id, i->operands[0]->id);
            break;
        }
        case IR_SUM_PAYLOAD: {
            IrType *st = i->operands[0]->type;
            int k = i->aux.sum.variant, fi = i->aux.sum.field;
            {   IrLayout L = ir_layout_of(st);
                if (L.packed) {                        // the value is the payload
                    fprintf(o, "  v%d = (", i->result->id);
                    ir_ctype(i->result->type, o);
                    fprintf(o, ")v%d;\n", i->operands[0]->id);
                    break;
                } }
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
            {   IrLayout L = ir_layout_of(st);
                if (L.packed) {
                    fprintf(o, "  v%d = (", i->result->id); ir_ctype(st, o); fputs(")", o);
                    if (L.has_sentinel[k]) {
                        // a payload-less variant: its spare bit pattern, as an integer the
                        // backing type can hold
                        if (L.backing && (L.backing->kind==IRT_PTR || L.backing->kind==IRT_SLICE))
                            fprintf(o, "(uintptr_t)%lldull;\n", L.sentinel[k]);
                        else fprintf(o, "%lldll;\n", L.sentinel[k]);
                    } else if (i->n_operands > 0) {
                        fprintf(o, "v%d;\n", i->operands[0]->id);
                    } else {
                        fputs("0;\n", o);
                    }
                    break;
                } }
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
        case IR_VEC_MOVEMASK: {   // the ISA's own lane-predicate reduction, where there is one
            IrType *vt = i->operands[0]->type;
            int lanes = vt ? (int)vt->array_len : 16;
            if (lanes == 16 || lanes == 32)
                fprintf(o, "  v%d = LAIN_MOVEMASK_%d(v%d);\n", i->result->id, lanes, i->operands[0]->id);
            else
                fprintf(o, "  v%d = lain_movemask_bytes((const unsigned char *)&v%d, %d);\n",
                        i->result->id, i->operands[0]->id, lanes);
            break;
        }
        case IR_VEC_SHUFFLE: {
            // Lane i = idx[i] < N ? t[idx[i]] : 0. GCC's __builtin_shuffle reads an index modulo
            // N, so the mask zeroes exactly the out-of-range lanes (with SSSE3 that is pshufb and
            // a compare). Clang has no variable-index __builtin_shuffle; the lane loop is the same
            // function, written out. The ISA is the C compiler's flag, never the language's.
            IrType *vt = i->result->type; int nl = vt ? (int)vt->array_len : 0;
            int r = i->result->id, t = i->operands[0]->id, x = i->operands[1]->id;
            if (i->aux.imm == 1) {     // every index is below N (lower.h): no zeroing to do
                fputs("#if defined(__GNUC__) && !defined(__clang__)\n", o);
                fprintf(o, "  v%d = __builtin_shuffle(v%d, v%d);\n", r, t, x);
                fputs("#else\n", o);
                fprintf(o, "  for (int lk = 0; lk < %d; lk++) v%d[lk] = v%d[v%d[lk]];\n", nl, r, t, x);
                fputs("#endif\n", o);
                break;
            }
            fputs("#if defined(__GNUC__) && !defined(__clang__)\n", o);
            fprintf(o, "  v%d = (", r); ir_ctype(vt, o);
            fprintf(o, ")(__builtin_shuffle(v%d, v%d) & (", t, x); ir_ctype(vt, o);
            fprintf(o, ")(v%d < %d));\n", x, nl);
            fputs("#else\n", o);
            fprintf(o, "  for (int lk = 0; lk < %d; lk++) v%d[lk] = v%d[lk] < %d ? v%d[v%d[lk]] : 0;\n",
                    nl, r, x, nl, t, x);
            fputs("#endif\n", o);
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
            { IrStructLayout PL = ir_struct_layout(i->result->type);
              if (PL.packed) {                  // OR the fields, each masked, into place
                  fprintf(o, "  v%d = (uint%d_t)(0", i->result->id, PL.container_bits);
                  for (int k = 0; k < i->n_operands && k < PL.n; k++)
                      fprintf(o, " | (((uint64_t)v%d & 0x%llxULL) << %d)", i->operands[k]->id,
                              PL.width[k] >= 64 ? ~0ULL : ((1ULL << PL.width[k]) - 1), PL.off[k]);
                  fputs(");\n", o);
                  break;
              } }
            // An ARRAY field cannot be initialised from a pointer in a C compound literal, and
            // the IR's uniform model gives every array value as its decayed base. So brace the
            // array fields empty and COPY them in — which is what `M([10,20,30,40], 4)` means.
            // Every field is initialised BY NAME (`.pos = v1`): operand k is declared field k,
            // and the C struct's member order is the layout's to choose (DECIDE-U), so a
            // positional initialiser would assign whatever field happens to be stored k-th.
            IrType *sty = i->result->type;
            fprintf(o, "  v%d = (", i->result->id); ir_ctype(sty, o); fputs("){ ", o);
            for (int k=0;k<i->n_operands;k++){
                if (k) fputs(", ", o);
                IrType *ft = (sty && sty->kind==IRT_STRUCT && k < sty->n_fields) ? sty->fields[k] : NULL;
                IrName *fn = (sty && sty->kind==IRT_STRUCT && k < sty->n_fields && sty->field_names)
                           ? sty->field_names[k] : NULL;
                if (fn) fprintf(o, ".%.*s = ", (int)fn->length, fn->name);   // a VECTOR's lanes stay positional
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
        case IR_NEG:
            if (i->result->type && i->result->type->kind == IRT_VECTOR && i->result->type->elem &&
                i->result->type->elem->kind == IRT_INT && i->result->type->elem->is_signed) {
                fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
                fputs(")(-(", o); ir_vec_unsigned_ctype(i->result->type, o);
                fprintf(o, ")v%d);\n", i->operands[0]->id);         // lane-wise wrap, as above
                break;
            }
            fprintf(o, "  v%d = -v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_BNOT:   fprintf(o, "  v%d = ~v%d;\n", i->result->id, i->operands[0]->id); break;
        case IR_CAST:   if (i->wrap != IR_WRAP_CHECK && i->n_operands == 1 && i->operands[0]->type &&
                            i->operands[0]->type->kind == IRT_INT && i->result->type &&
                            i->result->type->kind == IRT_INT) { ir_emit_cast_policy(i, o); break; }
                        fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o);
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
            // `/%` and `/|` (DECIDE-M): the divisor -1 is the only one that can overflow, and it
            // is spelled out — a negation, except at TYPE_MIN, which wraps to itself or saturates
            // to MAX. No C division ever sees TYPE_MIN / -1.
            if (i->n_operands == 2 && i->result && i->op == IR_SDIV && i->wrap != IR_WRAP_CHECK &&
                i->result->type && i->result->type->kind == IRT_INT) {
                int64_t tlo, thi; irtype_int_range(i->result->type, &tlo, &thi);
                int a = i->operands[0]->id, b = i->operands[1]->id, r = i->result->id;
                char mn[48], at[48];
                snprintf(mn, sizeof mn, "(%lldLL - 1)", (long long)(tlo + 1));
                if (i->wrap == IR_WRAP_SATURATE) snprintf(at, sizeof at, "%lldLL", (long long)thi);
                else snprintf(at, sizeof at, "%s", mn);
                fprintf(o, "  v%d = (v%d == -1) ? ((v%d == %s) ? (", r, b, a, mn);
                ir_ctype(i->result->type, o); fprintf(o, ")%s : (", at);
                ir_ctype(i->result->type, o); fprintf(o, ")-v%d) : (", a);
                ir_ctype(i->result->type, o); fprintf(o, ")(v%d / v%d);\n", a, b);
                break;
            }
            if (i->n_operands == 2 && i->result && i->wrap == IR_WRAP_MODULAR &&
                (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL) &&
                i->result->type && i->result->type->kind == IRT_INT) {
                ir_emit_modular(i, o);
                break;
            }
            // ★ A VECTOR'S LANES WRAP (the language's rule: sema exempts vector arithmetic from
            // the overflow check for that reason). On SIGNED lanes the C `+ - * <<` is signed
            // overflow, undefined: UBSan, on `i32x4 + i32x4` at 2147483647, "signed integer
            // overflow ... cannot be represented in type 'int'". Through the unsigned vector
            // type it is the wrap the language promises — the scalar `+%` rule, lane-wise.
            if (i->n_operands == 2 && i->result && i->result->type &&
                i->result->type->kind == IRT_VECTOR && i->result->type->elem &&
                i->result->type->elem->kind == IRT_INT && i->result->type->elem->is_signed &&
                (i->op == IR_ADD || i->op == IR_SUB || i->op == IR_MUL || i->op == IR_SHL) &&
                i->operands[0]->type && i->operands[1]->type &&
                (i->operands[0]->type->kind == IRT_VECTOR || i->operands[0]->type->kind == IRT_INT) &&
                (i->operands[1]->type->kind == IRT_VECTOR || i->operands[1]->type->kind == IRT_INT)) {
                IrType *vt = i->result->type;
                fprintf(o, "  v%d = (", i->result->id); ir_ctype(vt, o); fputs(")(", o);
                for (int k = 0; k < 2; k++) {
                    IrType *ot = i->operands[k]->type;
                    if (k) fprintf(o, " %s ", ir_arith_c(i->op));
                    fputc('(', o);
                    if (ot && ot->kind == IRT_VECTOR) ir_vec_unsigned_ctype(vt, o);
                    else fprintf(o, "uint%d_t", vt->elem->bits);
                    fprintf(o, ")v%d", i->operands[k]->id);
                }
                fputs(");\n", o);
                break;
            }
            if (i->n_operands == 2 && i->result) {
                fprintf(o, "  v%d = ", i->result->id);
                // Same reason as the vector comparison above: a mask's LANE SIGNEDNESS is
                // gcc's choice, not the IR's, so combining two masks with `|` can arrive at a
                // declared type that differs only in signedness. The IR agrees on the shape;
                // the backend reconciles the spelling.
                if (i->result->type && i->result->type->kind==IRT_VECTOR) {
                    fputc('(', o); ir_ctype(i->result->type, o); fputc(')', o);
                }
                fputc('(', o);
                ir_arith_operand_c(i, 0, o);
                fprintf(o, " %s ", ir_arith_c(i->op));
                ir_arith_operand_c(i, 1, o);
                fputs(");\n", o);
            }
            break;
    }
}

// A FIELD_PTR into a [packed] struct names BITS, not an address: it is emitted as nothing, and the
// LOAD or STORE through it becomes shift-and-mask on the container its base points to. The front
// end refuses every other use (a `var` reference, `&`), and ir_emit_func_c checks that none
// reached here — an escaping one would be a pointer to nothing.
static IrInstr **ir_pk_tab = NULL;      // value id -> its [packed] FIELD_PTR, per function
static int       ir_pk_n   = 0;
static IrInstr *ir_pk_of(IrValue *p) {
    return (p && ir_pk_tab && p->id >= 0 && p->id < ir_pk_n) ? ir_pk_tab[p->id] : NULL;
}
static void ir_pk_build(IrFunc *f, Arena *a) {
    ir_pk_n = f->next_value_id > 0 ? f->next_value_id : 0;
    ir_pk_tab = ir_pk_n ? arena_push_many_aligned(a, IrInstr*, ir_pk_n) : NULL;
    for (int k = 0; k < ir_pk_n; k++) ir_pk_tab[k] = NULL;
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (i->op != IR_FIELD_PTR || i->n_operands < 1 || !i->result) continue;
            IrValue *bv = i->operands[0];
            IrType *st = (bv && bv->type) ? bv->type->elem : NULL;
            if (st && ir_struct_layout(st).packed && i->result->id < ir_pk_n) ir_pk_tab[i->result->id] = i;
        }
    // Fail closed: a packed field's "address" may only be what a LOAD reads or a STORE writes.
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next)
            for (int q = 0; q < i->n_operands; q++) {
                if (!ir_pk_of(i->operands[q])) continue;
                if ((i->op == IR_LOAD || i->op == IR_STORE) && q == 0) continue;
                fprintf(stderr, "[E121] Error: a field of a [packed] struct is used as an address "
                        "(%s) — it is bits inside an integer and has none. (internal: the front end "
                        "should have refused this)\n", i->op == IR_CALL ? "passed to a call" : "an address use");
                exit(1);
            }
}
static void ir_emit_packed_load(IrInstr *i, FILE *o) {
    IrInstr *fp = ir_pk_of(i->operands[0]);
    IrStructLayout L = ir_struct_layout(fp->operands[0]->type->elem);
    int k = fp->aux.field_idx, w = L.width[k];
    unsigned long long m = w >= 64 ? ~0ULL : ((1ULL << w) - 1);
    fprintf(o, "  v%d = (", i->result->id); ir_ctype(i->result->type, o); fputs(")", o);
    if (L.sgn[k] && w < 64)                 // sign-extend the field's top bit
        fprintf(o, "((int64_t)(((((uint64_t)*v%d >> %d) & 0x%llxULL) ^ 0x%llxULL) - 0x%llxULL));\n",
                fp->operands[0]->id, L.off[k], m, 1ULL << (w - 1), 1ULL << (w - 1));
    else
        fprintf(o, "(((uint64_t)*v%d >> %d) & 0x%llxULL);\n", fp->operands[0]->id, L.off[k], m);
}
static void ir_emit_packed_store(IrInstr *i, FILE *o) {
    IrInstr *fp = ir_pk_of(i->operands[0]);
    IrStructLayout L = ir_struct_layout(fp->operands[0]->type->elem);
    int k = fp->aux.field_idx, w = L.width[k], b = fp->operands[0]->id;
    unsigned long long m = w >= 64 ? ~0ULL : ((1ULL << w) - 1);
    fprintf(o, "  *v%d = (uint%d_t)(((uint64_t)*v%d & ~(0x%llxULL << %d)) | (((uint64_t)v%d & 0x%llxULL) << %d));\n",
            b, L.container_bits, b, m, L.off[k], i->operands[1]->id, m, L.off[k]);
}

static void ir_emit_func_c(IrFunc *f, IrFunc *mod, FILE *o, Arena *a) {
    ir_pk_build(f, a);
    bool is_main = f->name->length==4 && strncmp(f->name->name,"main",4)==0;
    // signature
    if (is_main) fputs("int main(void)", o);
    else {
        IrCAnnot an = ir_c_annot(f, mod);
        if (an.const_attr)     fputs("__attribute__((const)) ", o);
        else if (an.pure_attr) fputs("__attribute__((pure)) ", o);
        ir_emit_decl_attrs(f, o);   // [cold] / [hot] / [allocator] / [noreturn]
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
        int hint[32], nh = 0;
        for (IrInstr *i=b->instrs; i; i=i->next) {
            if (i->op == IR_ASSUME) {
                if (i->aux.imm == 0 && i->n_operands >= 1 && i->operands[0]) {
                    if (nh == 32) ir_emit_hints(hint, &nh, o);
                    hint[nh++] = i->operands[0]->id;
                }
                continue;
            }
            // Only the comparisons that feed the next assume may come between; anything else
            // (a division by the parameter, a load through it) must already see the fact.
            if (nh && i->op != IR_ICMP && i->op != IR_CONST && i->op != IR_SLICE_LEN)
                ir_emit_hints(hint, &nh, o);
            ir_emit_instr_c(i, o);
        }
        ir_emit_hints(hint, &nh, o);
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


// ── THE METADATA THE FRONT END DECLARED (restored 2026-09-25) ─────────────────────────────
// `[cold]`, `[hot]`, `[allocator]`, `[noreturn]`. The AST emitter wrote these and the IR
// backend did not, so deleting src/emit/ dropped them silently — behaviour is identical, and
// the one gate that compared annotations went out in the same commit. `malloc` is the one that
// is more than a hint: it tells the optimizer the returned pointer aliases nothing.
static void ir_emit_decl_attrs(const IrFunc *f, FILE *o) {
    if (!f) return;
    if (f->is_cold)      fputs("__attribute__((cold)) ", o);
    if (f->is_hot)       fputs("__attribute__((hot)) ", o);
    if (f->is_allocator) fputs("__attribute__((malloc, returns_nonnull)) ", o);
    if (f->is_noreturn)  fputs("__attribute__((noreturn)) ", o);
}

static void ir_emit_proto_c(IrFunc *f, IrFunc *mod, FILE *o) {
    ir_emit_decl_attrs(f, o);
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
    // ★ THE NICHE, and the reason this is a query rather than a choice made here. Layout is
    // ONE decision (ir/layout.h) so that two backends cannot answer it differently — which is
    // exactly what happened when this function unconditionally emitted a tagged struct and
    // the old emitter packed the same sum into a single pointer (D-62).
    IrLayout L = ir_layout_of(st);
    if (L.packed) {
        if (L.all_empty) {   // a plain enumeration: the smallest integer that holds it
            // ...which is what this comment always said and the code never did: every
            // enumeration was an int32_t, so an 11-kind TokenKind made a {kind, pos u32, len u16}
            // token 12 bytes where `uint8_t` and field order make it 8 (the lexer report). The
            // tag values are 0..n-1, so an unsigned type of the smallest width holds them; no
            // sum niche-packs INTO a plain enumeration (layout.h gives it no backing), so no
            // sentinel depends on the width.
            fprintf(o, "typedef uint%d_t %.*s;\n", ir_plain_enum_bits(st->n_fields),
                    (int)nm->length, nm->name);
        } else {
            fputs("typedef ", o); ir_layout_backing_ctype(L.backing, o);
            fprintf(o, " %.*s;\n", (int)nm->length, nm->name);
        }
        return;
    }
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
    { IrStructLayout PL = ir_struct_layout(st);
      if (PL.packed) {                      // [packed]: the whole struct IS one integer
          fprintf(o, "typedef uint%d_t %.*s;\n", PL.container_bits, (int)nm->length, nm->name);
          return;
      } }
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
static void ir_sa_visit_types(IrTypeSet *ts, IrSAExpr *x) {
    if (!x) return;
    if ((x->kind == IR_SA_SIZEOF || x->kind == IR_SA_ALIGNOF) && x->type) ir_ts_visit(ts, x->type);
    ir_sa_visit_types(ts, x->l); ir_sa_visit_types(ts, x->r);
}
static void ir_emit_type_decls(IrFunc *funcs, FILE *o, Arena *a) {
    IrTypeSet ts = {0};
    // A type measured only by a module-scope assert is carried by no value either.
    for (IrStaticAssert *s = ir_static_asserts; s; s = s->next) ir_sa_visit_types(&ts, s->cond);
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
        // A type named only inside @sizeof/@alignof is carried by no value, and must still be
        // declared before the expression that measures it.
        for (IrBlock *b=f->blocks; b; b=b->next)
            for (IrInstr *i=b->instrs; i; i=i->next)
                if ((i->op==IR_SIZEOF || i->op==IR_ALIGNOF) && i->aux.alloca_ty) ir_ts_visit(&ts, i->aux.alloca_ty);
    }
    for (int i=0;i<ts.n_struct;i++){ IrName *nm=ts.structs[i]->sname;
        // A PACKED sum is not a struct — it is a typedef for its backing type, emitted whole
        // by ir_emit_one_sum_body. A forward `typedef struct X X;` for it would collide with
        // that (and name a struct that is never defined), exactly as it would for a packed
        // struct in the old backend.
        if (ts.structs[i]->kind == IRT_SUM && ir_layout_of(ts.structs[i]).packed) continue;
        if (ir_struct_layout(ts.structs[i]).packed) continue;   // a [packed] struct is a typedef too
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
        int bytes = lanes * ir_lane_bytes(v->elem);
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

// ── WHAT THIS BACKEND CANNOT EMIT, SAID OUT LOUD ─────────────────────────────────────────
// IR_OPAQUE is the IR's TOTALITY primitive and it is exactly right for the ANALYSES: "an
// unknown value with this memory footprint" lets them havoc precisely and keep proving the
// rest of the function, instead of one unlowered expression poisoning all of it.
//
// ★ IT IS NOT RIGHT FOR CODEGEN, and emitting `v0 = 0 /* unmodelled */` is worse than broken
// C. Broken C is caught by the C compiler; a zero of the right type COMPILES. The program
// that found this is `array_comprehension_expr_position_fail`, where the old backend refused
// with E100 and this one passed a NULL to a function that dereferences it — a clean compile
// and a segfault, from a construct the compiler knew it had not modelled.
//
// So the backend is prove-or-reject too: it emits what it can represent faithfully and
// REFUSES the rest. Measured before it was written — in the whole corpus exactly ONE program
// reaches an emitted OPAQUE, and it is a test asserting that this construct must be refused.
static int ir_emit_refuse_opaque(IrFunc *funcs, const char *file) {
    int n = 0;
    for (IrFunc *f = funcs; f; f = f->next) {
        if (f->is_extern) continue;
        for (IrBlock *b = f->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next) {
                if (i->op != IR_OPAQUE) continue;
                const char *why = i->aux.opaque.why ? i->aux.opaque.why : "?";
                fprintf(stderr, "[E100] Error");
                if (i->line) fprintf(stderr, " Ln %lld, Col %lld", (long long)i->line, (long long)i->col);
                fprintf(stderr, ": this construct is not supported by the code generator yet"
                                " (%s).\n", why);
                if (file && i->line)
                    fprintf(stderr, "  --> %s:%lld:%lld\n", file, (long long)i->line, (long long)i->col);
                fprintf(stderr, "       the compiler did not model it, so there is nothing "
                                "faithful to emit — refusing rather than emitting a "
                                "placeholder\n");
                n++;
            }
    }
    return n;
}

// Every operand is widened to `long long`, so C computes what Lain means: `@sizeof(T) - 16 < 0`
// is a signed question in Lain (Path F), and in C's size_t it would wrap and answer false.
static void ir_emit_sa_expr(IrSAExpr *x, FILE *o) {
    if (!x) { fputs("0", o); return; }
    switch (x->kind) {
        case IR_SA_CONST:
            if (x->value == INT64_MIN) fputs("(-9223372036854775807LL - 1)", o);
            else fprintf(o, "%lldLL", (long long)x->value);
            break;
        case IR_SA_SIZEOF: case IR_SA_ALIGNOF:
            fputs(x->kind == IR_SA_SIZEOF ? "((long long)sizeof(" : "((long long)_Alignof(", o);
            ir_ctype(x->type, o); fputs("))", o);
            break;
        case IR_SA_UNARY:  fprintf(o, "(%s", x->op); ir_emit_sa_expr(x->l, o); fputc(')', o); break;
        case IR_SA_BINARY: fputc('(', o); ir_emit_sa_expr(x->l, o); fprintf(o, " %s ", x->op);
                           ir_emit_sa_expr(x->r, o); fputc(')', o); break;
    }
}

void ir_emit_module_c(IrFunc *funcs, FILE *o, Arena *a) {
    fputs("#include <stdint.h>\n#include <stddef.h>\n\n", o);
    ir_emit_type_decls(funcs, o, a);
    // DECIDE-O: after every type is complete, so `sizeof` measures the real layout. The message
    // carries the diagnostic code Lain would have printed had it known the number.
    for (IrStaticAssert *s = ir_static_asserts; s; s = s->next) {
        fputs("_Static_assert(", o); ir_emit_sa_expr(s->cond, o);
        fprintf(o, ", \"[E134] Ln %lld, Col %lld: module-scope assertion is false\");\n",
                (long long)s->line, (long long)s->col);
    }
    if (ir_static_asserts) fputc('\n', o);
    // `panic` is a declless builtin — it has no Lain declaration, so nothing declares it in
    // the generated C either and every `else panic(...)` failed to LINK. The old backend
    // inlines fprintf+abort at the site; emit one helper instead, and only when it is used,
    // so a module that never panics is unchanged.
    { bool needs_x86 = false;                 // only when a movemask is actually emitted
      // ★ PORTABLY. This emitted `_mm_movemask_epi8` and <immintrin.h> unconditionally, so any
      // program using @movemask failed to COMPILE on ARM (aarch64, Apple silicon) — and a
      // 32-lane one failed on x86 too without -mavx2. The intrinsic is used where the target has
      // it; elsewhere each lane's top bit is collected by a loop gcc vectorises (NEON included).
      // LAIN_PORTABLE_SIMD forces the portable path, which is how it is tested on x86.
      for (IrFunc *f=funcs; f && !needs_x86; f=f->next)
        for (IrBlock *b=f->blocks; b && !needs_x86; b=b->next)
          for (IrInstr *i=b->instrs; i; i=i->next)
            if (i->op==IR_VEC_MOVEMASK) { needs_x86 = true; break; }
      if (needs_x86) fputs(
        "static inline uint32_t lain_movemask_bytes(const unsigned char *p, int n) {\n"
        "    uint32_t m = 0;\n"
        "    for (int k = 0; k < n; k++) m |= (uint32_t)(p[k] >> 7) << k;\n"
        "    return m;\n"
        "}\n"
        "#if defined(__SSE2__) && !defined(LAIN_PORTABLE_SIMD)\n"
        "#include <immintrin.h>\n"
        "#define LAIN_MOVEMASK_16(v) ((uint32_t)_mm_movemask_epi8((__m128i)(v)))\n"
        "#else\n"
        "#define LAIN_MOVEMASK_16(v) lain_movemask_bytes((const unsigned char *)&(v), 16)\n"
        "#endif\n"
        "#if defined(__AVX2__) && !defined(LAIN_PORTABLE_SIMD)\n"
        "#include <immintrin.h>\n"
        "#define LAIN_MOVEMASK_32(v) ((uint32_t)_mm256_movemask_epi8((__m256i)(v)))\n"
        "#else\n"
        "#define LAIN_MOVEMASK_32(v) lain_movemask_bytes((const unsigned char *)&(v), 32)\n"
        "#endif\n\n", o); }
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
