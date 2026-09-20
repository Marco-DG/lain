#ifndef LAIN_LAYOUT_CORE_H
#define LAIN_LAYOUT_CORE_H
// layout_core.h — the sentinel algebra, in ONE place.
//
// ★ WHY THIS FILE EXISTS. Niche layout had two implementations: `src/sema/niche.h` over the
// AST (for the legacy backend and the E064 check) and `src/ir/layout.h` over IrType (for the
// IR backend). They disagreed twice in two days, and both times the number looked fine:
//
//   D-62  the IR backend did not pack at all, so 29 sums in the corpus silently became
//         16-byte tagged structs while every gate stayed green — a behavioural differential
//         cannot see a representation change
//   D-63  the AST side packed a SLICE payload, which cannot hold an integer sentinel, so a
//         program was accepted and emitted `typedef Slice_u8_0` naming a type nothing defines
//
// `scripts/gates/layout_gate.sh` now CATCHES divergence. This file makes the part that can be
// shared impossible to diverge, which is what law L3 ("one mechanism per concern") actually
// asks for. What stays split is only the ADAPTER — "what backing does this type offer" — and
// that is genuinely two questions, because a `Type*` and an `IrType*` are different things.
// The pool algebra and the sentinel ASSIGNMENT are one question and now have one answer.
//
// ★ The assignment order is load-bearing and is why sharing it matters more than sharing the
// decision: two backends that both pack but pick DIFFERENT bit patterns for the same variant
// produce programs that disagree about what `none` is. Agreeing to pack is not enough; they
// must agree on the number.
#include "target.h"
#include "utils/common/libc.h"

typedef enum {
    POOL_EMPTY,             // no sentinel slots available
    POOL_POINTER,           // slots = [0, zero_page_size) step pointer_alignment
    POOL_INTEGER_BELOW,     // slots = [lo, refine_lo)
    POOL_INTEGER_ABOVE,     // slots = (refine_hi, hi]
    POOL_INTEGER_SPLIT,     // slots = [lo, refine_lo) U (refine_hi, hi]
    POOL_BOOL,              // slots = [2, 255]
} SentinelPoolKind;

typedef struct {
    SentinelPoolKind kind;
    long long size;           // total slot count (already adjusted for index_offset)
    long long below_start;    // POOL_INTEGER_*: low end of the below-range
    long long below_count;
    long long above_start;    // POOL_INTEGER_*: low end of the above-range
    long long above_count;
    long long ptr_stride;     // POOL_POINTER: spacing between slots (= pointer_alignment)
    long long index_offset;   // cascade: skip the first N slots (an inner enum already took them)
} SentinelPool;

// The zero page: addresses no user mapping can occupy, stepped by alignment so every sentinel
// is a validly-aligned (if unmappable) pointer value.
static SentinelPool sentinel_pool_pointer(void) {
    SentinelPool p = {0};
    if (target.zero_page_size == 0) { p.kind = POOL_EMPTY; return p; }  // bare metal: it is real memory
    p.kind       = POOL_POINTER;
    p.ptr_stride = (long long)target.pointer_alignment;
    p.size       = (long long)(target.zero_page_size / target.pointer_alignment);
    return p;
}

// A bool occupies one byte and uses two of its 256 patterns.
// ⚠ The STORAGE must be uint8_t, not C's `_Bool`, which normalises every nonzero store to 1
// and would collapse sentinel 2 onto `true` (D-62b). Both backends widen it.
static SentinelPool sentinel_pool_bool(void) {
    SentinelPool p = {0};
    p.kind = POOL_BOOL; p.size = 254;
    return p;
}

// An integer is a niche source only where something NARROWED it: a plain i32 uses every
// pattern it has. `[tlo,thi]` is the type's range, `[rlo,rhi]` the refinement's.
static SentinelPool sentinel_pool_int_refined(long long tlo, long long thi,
                                              long long rlo, long long rhi) {
    SentinelPool p = {0};
    p.kind = POOL_EMPTY;
    long long below = (rlo > tlo) ? (rlo - tlo) : 0;
    long long above = (thi > rhi) ? (thi - rhi) : 0;
    if (below == 0 && above == 0) return p;
    if (above == 0) { p.kind=POOL_INTEGER_BELOW; p.below_start=tlo; p.below_count=below;
                      p.size=below; return p; }
    if (below == 0) { p.kind=POOL_INTEGER_ABOVE; p.above_start=rhi+1; p.above_count=above;
                      p.size=above; return p; }
    p.kind=POOL_INTEGER_SPLIT;
    p.below_start=tlo; p.below_count=below;
    p.above_start=rhi+1; p.above_count=above;
    p.size = below + above;
    return p;
}

// ★ THE ASSIGNMENT. Deterministic, and the same for every backend — see the header note: two
// backends that pack but choose different patterns disagree about what `none` IS.
//
//   POOL_POINTER        0, stride, 2*stride, ...
//   POOL_INTEGER_BELOW  descending from refine_lo-1, so `Option(int >= 0)` gives None = -1
//   POOL_INTEGER_ABOVE  ascending from refine_hi+1
//   POOL_INTEGER_SPLIT  below first (descending), then above (ascending)
//   POOL_BOOL           2, 3, 4, ...
static long long sentinel_pick(const SentinelPool *p, long long index) {
    long long i = index + p->index_offset;
    switch (p->kind) {
        case POOL_POINTER:       return p->ptr_stride * i;
        case POOL_INTEGER_BELOW: return (p->below_start + p->below_count - 1) - i;
        case POOL_INTEGER_ABOVE: return p->above_start + i;
        case POOL_INTEGER_SPLIT:
            if (i < p->below_count) return (p->below_start + p->below_count - 1) - i;
            return p->above_start + (i - p->below_count);
        case POOL_BOOL:          return 2 + i;
        case POOL_EMPTY:
        default:                 return 0;   // the caller must have checked size
    }
}

// The value range of a sized integer, the input `sentinel_pool_int_refined` narrows.
static bool sentinel_int_range(int bits, bool is_signed, long long *lo, long long *hi) {
    if (bits <= 0 || bits > 64) return false;
    if (is_signed) {
        if (bits == 64) { *lo = INT64_MIN; *hi = INT64_MAX; return true; }
        *hi = (1LL << (bits - 1)) - 1;
        *lo = -(1LL << (bits - 1));
    } else {
        if (bits >= 63) { *lo = 0; *hi = INT64_MAX; return true; }
        *lo = 0;
        *hi = (1LL << bits) - 1;
    }
    return true;
}

#endif // LAIN_LAYOUT_CORE_H
