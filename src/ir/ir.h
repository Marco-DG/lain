// src/ir/ir.h — Lain IR: typed SSA/CFG intermediate representation.
//
// Phase 1.1 of the clean-core rebuild (see REBUILD.md, design/ir.md). This is the
// data structure the new analyses run over and the backend lowers to C. It is built
// BESIDE the old sema/ engine and is not yet wired into main.c (that is Phase 1.4);
// nothing here changes the current pipeline.
//
// Design goals (see design/ir.md §1): value identity (SSA, not names), explicit
// control flow (functions are CFGs of basic blocks), typed + total, and a shape the
// abstract interpreter consumes directly (slice length is a value; array index is an
// explicit instruction where bounds are proven; calls carry the callee).
#ifndef LAIN_IR_H
#define LAIN_IR_H

#include <stdint.h>
#include <stdbool.h>
#include "../utils/common/def.h"   // isize
#include "../utils/arena.h"        // Arena, arena_push_*
// SOVEREIGNTY: the IR imports NO front-end header (ast.h / token.h). It owns its
// names (IrName, interned) and every fact it needs. lower.h is the sole AST↔IR
// adapter. Litmus (Phase 2.9 A5): src/ir/* and src/analysis/* compile without ast.h.

// An IR-owned interned name — the bytes are copied into the IR arena, so the AST is
// freeable once lowering is done. Same field shape as the old Id (`name`,`length`)
// to keep call sites stable.
typedef struct IrName { char *name; isize length; } IrName;

// ─────────────────────────────────────────────────────────────────────────────
// Types (design/ir.md §2). A fresh, analysis-facing type; the AST Type is bridged
// to it during lowering, not reused, so the IR is decoupled from frontend types.
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    IRT_INT,        // iN / uN, usize/isize — carries width + signedness
    IRT_BOOL,       // i1 truth value
    IRT_FLOAT,      // f32 / f64 (opaque to the numeric domain, for now)
    IRT_PTR,        // raw *T / *var T
    IRT_SLICE,      // T[] / u8[:0] — a fat value {data ptr, len}
    IRT_ARRAY,      // fixed T[N]
    IRT_STRUCT,     // named aggregate
    IRT_SUM,        // discriminated union (enum / ADT / error union) — see IR_SUM_* below
    IRT_UNIT,       // no value (proc with no return)
    IRT_NEVER,      // diverges (panic / unreachable) — bottom
} IrTypeKind;

typedef struct IrType {
    IrTypeKind kind;
    // IRT_INT
    int  bits;              // 1..64
    bool is_signed;
    // IRT_FLOAT
    int  float_bits;        // 32 | 64
    // IRT_PTR / IRT_SLICE / IRT_ARRAY
    struct IrType *elem;    // pointee / element type
    bool  ptr_mut;          // *var T
    bool  slice_sentinel;   // u8[:0]
    int64_t array_len;      // IRT_ARRAY fixed length (>= 0)
    // ── B4: a STATIC REFINEMENT carried ON the type ─────────────────────────
    // The interval a value of this type is known to inhabit, tighter than its width allows.
    // `type Small = u8 < 200` is a property of the TYPE, not of any one value, so it belongs
    // here — and every analysis reading the type gets it, with no side-map and no assume.
    //
    // The distinction that makes this the right home (and why IR_SHAPE's extents are NOT
    // here): a static interval is the same for every value of the type, while a slice's
    // length or a region's extents are per-value runtime quantities. Type-level facts on the
    // type; value-level facts in the IR.
    //
    // Without it, resolving an alias to its base DISCARDED the constraint: `n Small` was
    // indistinguishable from `n u8`, so `a[n]` over `a[200]` did not prove — while the SAME
    // constraint written on the parameter did, because that path goes through the assumes.
    bool    has_refine;
    int64_t refine_lo, refine_hi;
    // linearity/multiplicity qualifier (Phase 3.2 / B5 substrate) — a value of a `linear`
    // type must be consumed exactly once (an owned resource: `mov`d, freed, or returned).
    // Language-neutral: Lain's owned modes and Rust's affine owners both lower to this.
    bool  linear;
    // IRT_STRUCT — self-contained: carries its lowered field types + names so the
    // backend never re-touches the AST.
    //
    // IRT_SUM reuses the same three arrays with the obvious reading: n_fields is the VARIANT
    // count, field_names[i] is variant i's name, and fields[i] is its payload — an IRT_STRUCT
    // for a multi-field payload, or NULL for a payload-less variant. `sname` is the sum's own
    // name. What the IR deliberately does NOT record is the LAYOUT: whether a sum is stored
    // as tag+union or niche-packed into a spare value of its payload is the backend's choice
    // (internal/design/ir_sum_types.md §3). Recording the niche here would make a sum
    // indistinguishable from a pointer with an odd range and destroy the discrimination every
    // analysis depends on.
    IrName *sname;          // struct/sum name (identity + C typedef name) — IR-owned
    struct IrType **fields; // lowered field types / variant payloads, in declaration order
    IrName **field_names;   // field / variant names
    int   n_fields;         // field count, or VARIANT count for IRT_SUM
} IrType;

// The integer interval [lo,hi] implied by an IRT_INT type — seeds the numeric domain
// and gates overflow/narrowing. Returns false for non-integer types.
bool irtype_int_range(const IrType *t, int64_t *lo, int64_t *hi);

// ─────────────────────────────────────────────────────────────────────────────
// Values (design/ir.md §3). Every value is defined exactly once (SSA) and identified
// by its id — this is what replaces name-keying. A value is produced by an
// instruction, a block φ, or a function parameter.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct IrValue {
    int32_t   id;           // dense, unique within a function; the identity
    IrType   *type;
    // OWNERSHIP of a place (an alloca or a parameter). Distinct from IrType.linear, and the
    // distinction is load-bearing: `linear` says the TYPE must be handled linearly (exactly
    // one consumer), `owns` says whether THIS BINDING is that consumer. The same linear type
    // appears in both roles — Lain `mov r R` vs `r R`, Rust `T` vs `&T`, C++ `T` vs
    // `const T&`, Swift `consuming` vs `borrowing` — so ownership cannot live on the type.
    //
    // Conflating them is not merely imprecise: leak-checking every slot of linear type
    // reports a "leak" in every shared-borrow callee, which is where the naive widening
    // produced 4 false positives. A borrowed binding must NOT be consumed and releases
    // nothing when it dies.
    bool      owns;
    // provenance (for debugging / back-mapping diagnostics to source)
    isize     line, col;
    // optional source name (diagnostics only — NEVER used as an analysis key)
    IrName   *src_name;
} IrValue;

// ─────────────────────────────────────────────────────────────────────────────
// Instructions (design/ir.md §4). Each defines 0 or 1 result value (`result`, NULL
// if none). Operands are IrValue* (SSA edges).
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    // constants & casts
    IR_CONST,               // aux.imm : the literal (int/bool); float in aux.fimm
    IR_CAST,                // aux.cast_kind ; op[0] = source
    // arithmetic (over ℤ; overflow is a separate obligation flagged in `checked`)
    IR_ADD, IR_SUB, IR_MUL, IR_SDIV, IR_UDIV, IR_SREM, IR_UREM, IR_NEG,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_LSHR, IR_ASHR, IR_BNOT,
    // Bit intrinsics. Primitive in every target (C __builtin_*, Rust trailing_zeros,
    // LLVM cttz/ctlz/ctpop), so they are ops rather than opaque calls — and the numeric
    // domain gets an exact range for free: all three land in [0, width].
    IR_CTZ, IR_CLZ, IR_POPCOUNT,
    // comparison → Bool
    IR_ICMP,                // aux.cmp : the predicate
    // aggregates / slices / memory
    IR_ALLOCA,              // aux.alloca_ty : slot element type ; result : Ptr
    IR_LOAD,                // op[0] = address
    IR_STORE,               // op[0] = address, op[1] = value ; no result
    IR_FIELD_PTR,           // op[0] = base ; aux.field_idx
    IR_ELEM_PTR,            // op[0] = base (array/slice), op[1] = index ; BOUNDS proven here
    IR_SLICE_LEN,           // op[0] = slice ; result : the length value (first-class)
    IR_SLICE_DATA,          // op[0] = slice ; result : data pointer
    IR_MAKE_SLICE,          // op[0] = data, op[1] = len
    IR_SUBSLICE,            // op[0] = slice, op[1] = lo, op[2] = hi
    IR_ARRAY_NEW,           // op[0..] = elements
    IR_STR_CONST,           // aux.str : static string literal bytes ; result : *u8
    IR_STRUCT_NEW,          // op[0..] = fields ; aux.struct_decl
    // Sum types (design/ir_sum_types.md). Discrimination is EXACT: the tag is an ordinary
    // integer the numeric domain can track, so `case`-arm refinement is ordinary guard
    // refinement rather than pattern-matching on magic constants.
    IR_SUM_NEW,             // aux.sum.variant = k ; op[0..] = variant k's payload fields
    IR_SUM_TAG,             // op[0] = a sum value → its discriminant (an integer)
    IR_SUM_PAYLOAD,         // op[0] = a sum ; aux.sum.{variant,field} → that payload field.
                            // Well-defined ONLY where the tag is known to be `variant`, which
                            // the CFG already establishes — no extra machinery needed.
    // calls
    IR_CALL,                // aux.callee : Decl ; op[0..] = args
    // ── S2: the MEMORY MODEL — a rank-N strided region ──────────────────────
    // Declares that a flat region has a SHAPE: operand 0 is the base (array/slice), and
    // operands 1..n are its EXTENTS, outermost first. Strides are row-major and derived
    // (stride_k = product of the extents right of k), so a rank-2 region (h, w) has
    // strides (w, 1) and the flat index `i*w + j` is the coordinate access (i, j).
    //
    // Why this is a PRIMITIVE and not a precision tweak: without it, `a[i*w + j]` over
    // `i32[h*w]` is a NONLINEAR obligation (i*w multiplies two runtime values) and the
    // octagon cannot express it. With it, the obligation FACTORS per dimension into
    // `i < h ∧ j < w` — two facts the loop guards already put in the octagon. The 2D case
    // was manufactured hard by the missing memory model, exactly as the retrospective
    // found; it is not a numeric-domain problem at all.
    //
    // The extents are RUNTIME VALUES, which is why the shape cannot live on IrType and is
    // stated here in the IR rather than rediscovered by each analysis from a side-map.
    IR_SHAPE,
    // Declares that op[0] (a place) is WHOLLY INITIALISED from here on. No runtime effect.
    //
    // Some constructs initialise a whole aggregate BY CONSTRUCTION and the fact is known at
    // lowering: an array comprehension `[expr for i in 0..N]` fills every element, by
    // definition. A MUST-analysis cannot rediscover that — it lowers to a fill LOOP, and the
    // zero-iteration path through the loop intersects the fact away at the join, so
    // definite-assignment reported every comprehension-filled array as uninitialised.
    // Stating what lowering already knows is honest and costs nothing; making the analysis
    // re-derive it would need a proof that the loop covers 0..len.
    IR_INIT,
    // A SCOPED borrow: op[0] is the borrowed place, and the loan is live from IR_BORROW to
    // the matching IR_BORROW_END. No runtime effect.
    //
    // Most loans are inferred from LIVENESS — a reference is live until its last use — but
    // some are scoped by a CONSTRUCT instead, and no liveness of any value expresses them.
    // `case &x { 42: x = 99 }` borrows x for the whole match: nothing reads the borrow in
    // the arms, so a liveness-derived region would be empty and the write would look legal.
    // Language-neutral: Rust's borrow regions, C++ reference lifetimes, Fortran ASSOCIATE.
    IR_BORROW, IR_BORROW_END,
    // ── B3: the TOTALITY primitive ──────────────────────────────────────────
    // An UNMODELLED construct, represented honestly instead of poisoning its whole
    // function. `IrFunc.incomplete` suppressed every proof over a function that contained
    // one unlowered expression — 12% of the corpus went unanalysed, and every survey number
    // was silently conditioned on "among the functions we modelled" (backlog C3, E0.1).
    //
    // IR_OPAQUE says: this produces an UNKNOWN value, and it may touch memory as declared by
    // aux.opaque. An analysis handles it by havocking exactly that footprint and carrying on
    // — so the rest of the function is still analysed, soundly. That is what makes the IR
    // TOTAL: there is no "gave up here" bit, only a declared unknown.
    IR_OPAQUE,
    // verification layer (Phase 2.9 — the assume/assert substrate)
    IR_ASSUME,              // op[0] = a bool that HOLDS here (guard/refinement/precondition)
    IR_ASSERT,              // op[0] = a bool the analysis must DISCHARGE (obligation)
    // linearity (Phase 3.2) — `mov x` invalidates x's storage here; the source becomes
    // moved-from. op[0] = the consumed slot/value. A no-op at run time (codegen ignores it).
    IR_CONSUME,
    // SSA merge
    IR_PHI,                 // phi_args : (value, block) pairs
} IrOp;

typedef enum {
    IR_CMP_EQ, IR_CMP_NE,
    IR_CMP_SLT, IR_CMP_SLE, IR_CMP_SGT, IR_CMP_SGE,
    IR_CMP_ULT, IR_CMP_ULE, IR_CMP_UGT, IR_CMP_UGE,
} IrCmp;

typedef enum {
    IR_CAST_ZEXT, IR_CAST_SEXT, IR_CAST_TRUNC,   // TRUNC carries the narrowing check
    IR_CAST_ITOF, IR_CAST_FTOI, IR_CAST_BITCAST, IR_CAST_PTR,
} IrCastKind;

// arithmetic wrap/overflow mode
typedef enum { IR_WRAP_CHECK, IR_WRAP_MODULAR, IR_WRAP_SATURATE } IrWrapMode;

typedef struct IrPhiArg { IrValue *value; struct IrBlock *pred; struct IrPhiArg *next; } IrPhiArg;

typedef struct IrInstr {
    IrOp      op;
    IrValue  *result;       // NULL for STORE / void calls
    IrValue **operands;     // SSA operands
    int       n_operands;
    IrWrapMode wrap;        // arithmetic overflow mode (default IR_WRAP_CHECK)
    // op-specific aux
    union {
        int64_t     imm;        // IR_CONST integer/bool
        double      fimm;       // IR_CONST float
        IrCmp       cmp;        // IR_ICMP
        IrCastKind  cast_kind;  // IR_CAST
        IrType     *alloca_ty;  // IR_ALLOCA
        int32_t     field_idx;  // IR_FIELD_PTR
        IrName     *callee;     // IR_CALL — the callee's name (IR-owned)
        struct { const char *bytes; int32_t len; } str;  // IR_STR_CONST
        struct { int32_t variant, field; } sum;           // IR_SUM_NEW / IR_SUM_PAYLOAD
        // IR_OPAQUE footprint. `writes` is the load-bearing bit: a write may invalidate the
        // memory cells the numeric domain tracks, exactly as a call does. `why` is the same
        // label the incomplete survey ranks, so the reason survives into the IR.
        struct { bool writes; const char *why; } opaque;
    } aux;
    IrPhiArg  *phi_args;    // IR_PHI
    bool       unchecked;   // ELEM_PTR / arithmetic inside an `unsafe` block
    isize      line, col;   // for diagnostics
    struct IrInstr *next;   // intrusive list within a block
} IrInstr;

// ─────────────────────────────────────────────────────────────────────────────
// Terminators (design/ir.md §5) — every block ends in exactly one.
// ─────────────────────────────────────────────────────────────────────────────
typedef enum { IR_TERM_BR, IR_TERM_BR_COND, IR_TERM_SWITCH, IR_TERM_RET, IR_TERM_UNREACHABLE } IrTermKind;

typedef struct IrSwitchCase { int64_t key; struct IrBlock *target; struct IrSwitchCase *next; } IrSwitchCase;

typedef struct IrTerm {
    IrTermKind kind;
    IrValue   *cond;        // BR_COND (bool) / SWITCH (int) / RET (value|NULL)
    struct IrBlock *a;      // BR target / BR_COND then / SWITCH default
    struct IrBlock *b;      // BR_COND else
    IrSwitchCase   *cases;  // SWITCH
} IrTerm;

// ─────────────────────────────────────────────────────────────────────────────
// Basic blocks & functions (design/ir.md §6). The CFG the fixpoint iterates.
// ─────────────────────────────────────────────────────────────────────────────
typedef struct IrEdge { struct IrBlock *block; struct IrEdge *next; } IrEdge;

typedef struct IrBlock {
    int32_t   id;
    IrInstr  *phis;         // φ-nodes at the head (IR_PHI instrs)
    IrInstr  *instrs;       // body (intrusive list; `instrs_tail` for O(1) append)
    IrInstr  *instrs_tail;
    IrTerm    term;
    IrEdge   *preds;        // predecessors (filled after CFG is built)
    bool      is_loop_header;   // widening point (set by a back-edge pass)
    struct IrBlock *next;   // list within the function
} IrBlock;

typedef struct IrParam { IrValue *value; struct IrParam *next; } IrParam;

typedef enum { IR_FUNC_PURE, IR_FUNC_PROC } IrFuncKind;

// Effect row (Phase 3.3 / audit finding F1 — the general effect LATTICE that replaces the
// PURE/PROC binary as the semantic authority on side effects). A function is pure & total
// iff its set is empty; a `func` is exactly one whose effects avoid IO and DIVERGE. Computed
// by an IR pass (analysis/effects.h) and propagated callee ⊆ caller. Language-neutral: any
// front-end's function lowers to a footprint over these bits.
typedef enum {
    IR_EFFECT_WRITE   = 1 << 0,  // writes mutable GLOBAL state (a var-param write is NOT this)
    IR_EFFECT_DIVERGE = 1 << 1,  // may not terminate (unbounded loop / non-well-founded recursion)
    IR_EFFECT_RAISES  = 1 << 2,  // may panic / abort
    IR_EFFECT_IO      = 1 << 3,  // external side effects (calls a proc / extern proc)
    IR_EFFECT_ALLOC   = 1 << 4,  // allocates
} IrEffectBit;
typedef unsigned IrEffect;

typedef struct IrFunc {
    IrName    *name;
    IrFuncKind kind;
    bool       is_extern;   // declaration only (no body) — a trusted boundary
    IrParam   *params;      // parameter values
    IrType    *ret_type;
    IrBlock   *entry;
    IrBlock   *blocks;      // all blocks (list); entry is first
    IrBlock   *blocks_tail; // O(1) append
    int32_t    next_value_id;
    int32_t    next_block_id;
    void      *src_decl;    // OPAQUE provenance handle (front-end's; the IR never derefs it)
    bool       incomplete;
    // WHY this function could not be lowered faithfully — a static string, first reason wins.
    // `incomplete` suppresses every proof over the function, so an UNLABELLED one is an
    // unmeasured escape hatch silently conditioning every survey number (backlog C3). The
    // label makes the remaining gap a ranked work list instead of a single opaque count.
    const char *incomplete_why;  // lowering dropped/placeholder'd a construct ⇒ the IR is
                            // NOT faithful, so no analysis may claim a proof over it
    // effect row (analysis/effects.h fills these — memoized transitive fixpoint)
    IrEffect   effects;
    bool       effects_done, effects_in_progress;
    // B5 region info: does the RETURN borrow from a parameter, and from WHICH? Lain has no
    // lifetime syntax, so the relationship must be recovered, or `r = get_ref(var d)` looks
    // like a plain value and the loan on `d` is invisible across statements.
    //
    // `ret_borrows` is a SIGNATURE fact (the return type is a reference) — set at lowering.
    // The SOURCE is a BODY fact and is INFERRED by analysis/borrow.h, never guessed from the
    // signature: `pick(var a, var b) var i32` may return either, and picking "the first
    // mutable param" mis-attributes the loan for `return var b.x`, silently losing every
    // conflict on `b`. Rust cannot infer this and rejects such a signature outright ("missing
    // lifetime specifier"); being whole-program, we read it off the returns instead.
    bool       ret_borrows;          // the returned reference borrows from a parameter
    uint64_t   ret_borrow_mask;      // bit i = it may borrow from param i (0 with the flag set
    bool       ret_borrow_mask_done; // is impossible: the fallback is every reference param)
    Arena     *arena;       // where this function's IR is allocated
    struct IrFunc *next;
} IrFunc;

typedef struct IrModule {
    IrFunc  *funcs;
    void    *types;         // TODO(2.9): IR-OWNED type table (struct/enum descriptors);
                            // opaque for now — IrModule is not yet wired.
    Arena   *arena;
} IrModule;

// ─────────────────────────────────────────────────────────────────────────────
// Construction helpers (arena-allocated; defined in ir.c / builder).
// ─────────────────────────────────────────────────────────────────────────────
IrType  *ir_type_int(Arena *a, int bits, bool is_signed);
IrType  *ir_type_bool(Arena *a);
IrValue *ir_new_value(IrFunc *f, IrType *t);
IrBlock *ir_new_block(IrFunc *f);
IrInstr *ir_emit(IrBlock *b, IrInstr *ins);   // append to block body, return it

#endif // LAIN_IR_H
