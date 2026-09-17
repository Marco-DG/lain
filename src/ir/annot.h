// src/ir/annot.h — Stage IV item 4.1/4.3: THE BACKEND EARNS THE PROOFS.
//
// Everything the analyses established is, at the C boundary, a promise the optimizer cannot
// make for itself. `restrict` is the famous one — it is what lets a vectorizer skip the
// runtime overlap test — and in C it is the PROGRAMMER's word, unchecked, undefined if wrong.
// Here it is a THEOREM: the borrow checker has already refused every program in which two
// reference parameters could name the same object.
//
// The other two come from the EFFECT ROW, which is likewise computed rather than declared:
//   * `pure`        — no side effects; repeated calls may be folded together.
//   * `const`       — pure, and reads nothing but its arguments (no reference parameters).
//
// ★ MEASURED, so it is not overclaimed: in a single translation unit with every body visible,
// `pure`/`const` are INERT — gcc derives the same facts itself and its own IPA overrides the
// attribute (a `const` function that prints still prints at -O3, and one that aborts still
// aborts). The measured win here is entirely `restrict`: on `out[i] = a[i] + b[i]` over 1024
// elements, WITH it gcc emits one vectorized loop in 12 instructions; WITHOUT it, "loop
// versioned for vectorization because of possible aliasing" — two loops plus a runtime overlap
// test, 33 instructions. The attributes are kept because they are correct by construction and
// become live the moment the emission is split across TUs, or under -flto, or on the LLVM seam
// where they are `readnone`/`readonly`.
//
// ★ The one rule that is not a free win, recorded because getting it wrong already produced a
// MISCOMPILE once in this project: `const`/`pure` license gcc to DELETE a call whose result is
// unused, so a function that can PANIC must never carry them — the abort disappears at -O2 and
// survives at -O0. Hence `effects == 0` (which includes Raises), not "no writes".
//
// Sovereign: reads ir.h plus the two analysis facts, and nothing from the front end. A second
// front end gets every one of these annotations without asking for them.
#ifndef LAIN_IR_ANNOT_H
#define LAIN_IR_ANNOT_H

#include "ir.h"
#include "../analysis/footprint.h"
#include "../analysis/effects.h"

typedef struct {
    bool const_attr;    // __attribute__((const))  — result depends only on argument VALUES
    bool pure_attr;     // __attribute__((pure))   — no side effects; may read memory
} IrCAnnot;

// Does this parameter list contain anything the function could read memory through?
static bool ir_annot_reads_memory(IrFunc *f) {
    for (IrParam *p=f->params; p; p=p->next) {
        IrType *t = p->value ? p->value->type : NULL;
        if (t && (t->kind==IRT_PTR || t->kind==IRT_SLICE || t->kind==IRT_ARRAY)) return true;
    }
    // A function with no reference parameters can still read a global. The IR models globals
    // by folding their values in (there is no load-from-global op), so reaching one means an
    // OPAQUE — and an opaque already forces every effect bit, so `effects == 0` covers it.
    return false;
}

static IrCAnnot ir_c_annot(IrFunc *f, IrFunc *mod) {
    IrCAnnot a = {0};
    if (!f || f->is_extern) return a;                    // no body ⇒ nothing computed to trust
    if (!f->ret_type || f->ret_type->kind==IRT_UNIT) return a;   // both attributes need a value
    if (f->is_variadic) return a;
    // ★ effects == 0, not "no writes". Raises is in there deliberately: `const`/`pure` let gcc
    // drop a call whose result is unused, and dropping a call that ABORTS is a miscompile.
    if (ir_effects(f, mod) != 0) return a;
    // ★★ AND an empty WRITE FOOTPRINT — the effect row alone is NOT enough, and finding out
    // why is the sharpest thing this file taught. `IR_EFFECT_WRITE` means "writes mutable
    // GLOBAL state", and says so: a write through a `var` parameter is deliberately excluded,
    // because it is part of the function's INTERFACE, visible to the caller through the
    // argument, not a hidden effect. That is the right lattice. It is the wrong gate for
    // gcc's `pure`, which forbids EVERY memory write. So `proc bump2(p *var i32, q *var i32)`
    // — which mutates both its arguments — had `effects == 0` and was being handed `pure`.
    // The two facts are complementary and both are needed; the footprint is the other half.
    if (mod && ir_param_writes(f, mod) != 0) return a;
    a.pure_attr  = true;
    a.const_attr = !ir_annot_reads_memory(f);
    return a;
}

// `restrict` on a reference parameter. Justified by the BORROW CHECKER, not by syntax:
//   * a MUTABLE borrow is exclusive — no other live reference names that object, so nothing
//     can reach it except through this parameter;
//   * a SHARED borrow forbids any concurrent mutable borrow, so the object is not MODIFIED
//     during the call — and restrict's requirement only bites on modification (C11 6.7.3.1p4),
//     which is why two shared references to the same array are still fine.
// A RAW pointer (`*T` in `unsafe`) carries no such guarantee and gets nothing.
static bool ir_param_c_restrict(IrValue *pv) {
    IrType *t = pv ? pv->type : NULL;
    if (!t) return false;
    if (t->kind == IRT_SLICE) return true;               // fat pointer: its data pointer
    if (t->kind == IRT_ARRAY) return true;               // a DECAYED fixed-array reference
    // ★ POSITIVE EVIDENCE, NOT THE ABSENCE OF EVIDENCE. This read `!t->is_raw`, and `is_raw`
    // defaults to false — so a pointer type built by any code path that did not think about the
    // question got `restrict`, which is UB if wrong. The default was the unsafe one, and chapter
    // 0's gap list has said so since it was written.
    //
    // `borrowed` is the fact stated positively: this pointer is a reference into storage the
    // caller owns, whose exclusivity some checker has discharged. A type nobody marked is now
    // simply not a borrow, and gets nothing. `is_raw` stays as the belt-and-braces half —
    // `&x` and `*T` in `unsafe` set it explicitly — so both a missing mark and an explicit raw
    // pointer fall on the safe side.
    if (t->kind == IRT_PTR)  return t->borrowed && !t->is_raw;
    return false;
}

// `const` on the POINTEE, from the computed write footprint rather than the declaration.
//
// NOT currently emitted, and the reason is worth keeping: C's `const` is not an aliasing
// guarantee — it can be cast away, so no optimizer may rely on it — and qualifying a
// parameter cascades into every pointer derived from it (`&p[i]` is not const), which is real
// emitter complexity bought for nothing an optimizer can use. `restrict` is the qualifier
// that carries a fact. Kept because the footprint is the honest source for it the day the
// emitter propagates qualifiers, and because it documents which parameters are read-only.
static bool ir_param_c_const(IrFunc *f, IrFunc *mod, int k, IrValue *pv) {
    IrType *t = pv ? pv->type : NULL;
    if (!t || (t->kind!=IRT_PTR && t->kind!=IRT_SLICE)) return false;
    if (k >= 64) return false;                           // beyond the footprint's width
    IrWriteFootprint w = ir_param_writes(f, mod);
    return ((w >> k) & 1u) == 0;
}

#endif // LAIN_IR_ANNOT_H
