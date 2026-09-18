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

// ── `nonnull` and `returns_nonnull`: A BORROW CAN NEVER BE NULL ─────────────────────────
// The second-largest thing the old backend says and the new one did not: 358 `nonnull` and 18
// `returns_nonnull` across the corpus, against zero. They are not decoration — gcc uses them
// to delete null tests and to propagate non-nullness into callers — and they are theorems
// here rather than promises, for the same reason `restrict` is: a BORROW is a reference to
// storage that exists, created from a live place by a checker that refused every program
// where it could dangle. Lain has no null borrow to express.
//
// A RAW pointer is exactly the case that must NOT get it. `*T` in `unsafe` may be null by
// construction — `var p *u8 = 0` is an ordinary program — so the test is `borrowed`, the fact
// stated positively, and never the absence of `is_raw`. Same discipline as `restrict` above,
// and for the same reason: a type nobody marked must fall on the safe side.
//
// Emitted as ONE `nonnull` with no argument list, which in gcc means "every pointer parameter
// of this function". That is only correct when EVERY pointer parameter is a borrow, so the
// predicate below requires exactly that and says nothing otherwise — the alternative
// (`nonnull(1,3)`, listing positions) is more precise and needs the emitter to agree with the
// analysis about C parameter numbering, which is where a slice that becomes two parameters
// would quietly desynchronise them.
static bool ir_func_c_nonnull(IrFunc *f) {
    if (!f || f->is_extern || f->is_variadic) return false;
    bool any = false;
    for (IrParam *p=f->params; p; p=p->next) {
        IrType *t = p->value ? p->value->type : NULL;
        if (!t) return false;
        bool is_ref = (t->kind==IRT_PTR || t->kind==IRT_SLICE || t->kind==IRT_ARRAY);
        if (!is_ref) continue;                       // a scalar parameter is not our business
        if (t->kind==IRT_PTR && !(t->borrowed && !t->is_raw)) return false;   // a raw pointer
        any = true;
    }
    return any;
}

// The return is a reference into the caller's storage — so it is a borrow, so it is not null.
// `ir_ret_is_borrow` is the one reader of that fact (it used to be a side flag on IrFunc), and
// this is its second client, which is the point of having moved it onto the type.
static bool ir_func_c_returns_nonnull(IrFunc *f) {
    return f && !f->is_extern && ir_ret_is_borrow(f);
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
