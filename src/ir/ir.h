// src/ir/ir.h — Lain IR: typed SSA/CFG intermediate representation.
//
// Phase 1.1 of the clean-core rebuild (see REBUILD.md, local/internal/design/ir.md). This
// is the data structure the new analyses run over and the backend lowers to C. It is
// built BESIDE the old sema/ engine and is not yet wired into main.c (that is Phase 1.4);
// nothing here changes the current pipeline.
//
// Design goals (see local/internal/design/ir.md §1): value identity (SSA, not names),
// explicit control flow (functions are CFGs of basic blocks), typed + total, and a shape
// the abstract interpreter consumes directly (slice length is a value; array index is an
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
// Types (local/internal/design/ir.md §2). A fresh, analysis-facing type; the AST Type is
// bridged to it during lowering, not reused, so the IR is decoupled from frontend types.
// ─────────────────────────────────────────────────────────────────────────────
typedef enum {
    IRT_INT,        // iN / uN, usize/isize — carries width + signedness
    IRT_BOOL,       // i1 truth value
    IRT_FLOAT,      // f32 / f64 (opaque to the numeric domain, for now)
    IRT_FUNC,       // a non-capturing FUNCTION POINTER. `elem` = return type (NULL = void),
                    // `fields[0..n_fields)` = parameter types. Values of this type are the
                    // only ones a call can reach INDIRECTLY, which is why the type has to
                    // exist: without it an indirect call is unattributable and every analysis
                    // has to assume the worst about the whole program.
    IRT_VECTOR,     // SIMD `Vec(N, T)` — N lanes of T. `elem` = lane type, `array_len` = N.
                    // A plain Copy value: no linearity, register-resident. The LAYOUT (a
                    // vector_size typedef, or N scalars) is the backend's, exactly as for a
                    // sum's tag-vs-niche — the IR says only "N lanes of T".
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
    // A RAW pointer (`*T`, `&x`) rather than a BORROW. The distinction is not Lain's: a
    // borrow comes with an exclusivity/liveness guarantee some checker has discharged, a raw
    // pointer comes with nothing. C has only the second kind, Rust has both, and the backend
    // needs to know which — `restrict` is a theorem about the first and a guess about the
    // second. Defaults to false, i.e. "a checked reference", which is what every IR builder
    // that does not say otherwise is producing.
    bool  is_raw;
    // ── A BORROW IS A PROPERTY OF THE TYPE, NOT A FLAG BESIDE IT ────────────────────────
    // True when a value of this type is a REFERENCE INTO something the caller owns, for the
    // duration of a scope some checker has discharged — as opposed to a value that was copied
    // or a raw address that promises nothing.
    //
    // This existed as `IrFunc.ret_borrows`, a boolean on the FUNCTION, read by
    // analysis/borrow.h and by nothing else. The IR was then ill-typed for the most important
    // case in the language (`func ref_val(%0: *var struct) -> i32` whose body is
    // `ret %1 : *i32`), and every client that trusts types rather than the side flag got it
    // wrong: the IR's own C backend emitted an `int32_t` return and the write through the
    // returned reference never reached the owner. The borrow analysis was correct BECAUSE it
    // read the flag, and its correctness is what hid that the type was wrong.
    //
    // Why a bit here and not just `ptr_mut` on a pointer: a mutable borrow of a STRUCT or a
    // scalar travels as an address (so it is an IRT_PTR and could be recognised by its
    // shape), but a mutable borrow of a SLICE or an ARRAY travels as itself — it already
    // carries a data pointer — so there is no wrapper to recognise. One bit answers both, and
    // answers it for every kind of type, which is what the side flag was really saying.
    bool  borrowed;
    bool  slice_sentinel;   // u8[:0]
    // IRT_FUNC. The declared EFFECT BOUND on the arrow: true for `*func`, false for `*proc`.
    // Nielson & Nielson write this as the latent effect of a function type, tau ->^phi tau',
    // and it is the reason a function type has to exist at all for effects: a declaration's
    // effect row cannot follow a value that is passed around, but the TYPE can.
    //
    // Dropping it at lowering made every indirect call unattributable, and the effect pass
    // then charged an indirect call NOTHING — so a function whose only impurity was calling
    // through a pointer came out `{}` (pure & total), W130 advised downgrading it to `func`,
    // and taking that advice produced a `func` that performs IO (D-42). The bound is
    // conservative in the right direction: false ("may do anything") is the default for any
    // builder that does not say otherwise.
    bool  fn_is_total;
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
    // name. What the IR deliberately does NOT record is the LAYOUT: whether a sum is
    // stored as tag+union or niche-packed into a spare value of its payload is the
    // backend's choice (local/internal/design/ir_sum_types.md §3). Recording the niche
    // here would make a sum indistinguishable from a pointer with an odd range and
    // destroy the discrimination every analysis depends on.
    IrName *sname;          // struct/sum name (identity + C typedef name) — IR-owned
    struct IrType **fields; // lowered field types / variant payloads, in declaration order
    IrName **field_names;   // field / variant names
    int   n_fields;         // field count, or VARIANT count for IRT_SUM
} IrType;

// The integer interval [lo,hi] implied by an IRT_INT type — seeds the numeric domain
// and gates overflow/narrowing. Returns false for non-integer types.
bool irtype_int_range(const IrType *t, int64_t *lo, int64_t *hi);

// ─────────────────────────────────────────────────────────────────────────────
// Values (local/internal/design/ir.md §3). Every value is defined exactly once (SSA) and
// identified by its id — this is what replaces name-keying. A value is produced by an
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
// Instructions (local/internal/design/ir.md §4). Each defines 0 or 1 result value
// (`result`, NULL if none). Operands are IrValue* (SSA edges).
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
                            // With ONE OPERAND it is a DYNAMIC frame allocation: operand 0 is
                            // the element COUNT and aux.alloca_ty the element type — a C VLA,
                            // LLVM's `alloca T, n`, Zig's stack buffer. The extent is a
                            // runtime value, so it cannot live on the type; that is the same
                            // reason IR_SHAPE's extents are operands.
    IR_LOAD,                // op[0] = address
    IR_STORE,               // op[0] = address, op[1] = value ; no result
    IR_VEC_MOVEMASK,        // op[0] = a Vec(N,u8) ; result : u32 — one bit per lane's sign.
                            // Language-neutral: "reduce a lane-wise predicate to a bitmask" is
                            // what every SIMD ISA calls it, not a Lain idea.
    IR_FIELD_PTR,           // op[0] = base ; aux.field_idx
    IR_ELEM_PTR,            // op[0] = base (array/slice), op[1] = index ; BOUNDS proven here
    IR_SLICE_LEN,           // op[0] = slice ; result : the length value (first-class)
    IR_SLICE_DATA,          // op[0] = slice ; result : data pointer
    IR_MAKE_SLICE,          // op[0] = data, op[1] = len
    IR_SEQ_EQ,              // op[0], op[1] = two slices → Bool: SAME LENGTH and same bytes.
                            // Primitive in every target (C memcmp, Rust slice ==, Zig
                            // mem.eql, LLVM recognises the loop), so it is an op rather than
                            // an opaque call — an opaque would suppress every proof in the
                            // function, and a lowered LOOP would invent a termination
                            // obligation for something with no loop in it. Memory-safe by
                            // construction: both operands carry their own lengths.
    IR_SUBSLICE,            // op[0] = slice, op[1] = lo, op[2] = hi
    IR_ARRAY_NEW,           // op[0..] = elements
    IR_STR_CONST,           // aux.str : static string literal bytes ; result : *u8
    IR_STRUCT_NEW,          // op[0..] = fields ; aux.struct_decl
    // Sum types (local/internal/design/ir_sum_types.md). Discrimination is EXACT: the tag
    // is an ordinary integer the numeric domain can track, so `case`-arm refinement is
    // ordinary guard refinement rather than pattern-matching on magic constants.
    IR_SUM_NEW,             // aux.sum.variant = k ; op[0..] = variant k's payload fields
    IR_SUM_TAG,             // op[0] = a sum value → its discriminant (an integer)
    IR_SUM_PAYLOAD,         // op[0] = a sum ; aux.sum.{variant,field} → that payload field.
                            // Well-defined ONLY where the tag is known to be `variant`, which
                            // the CFG already establishes — no extra machinery needed.
    // calls
    IR_FUNC_REF,            // aux.callee = a function's name ; result : IRT_FUNC. Naming a
                            // function without calling it — what `var f *func(..) = choose`
                            // stores. Without it the initializer had no value at all.
    IR_CALL,                // DIRECT:   aux.callee = the name ; op[0..] = args
                            // INDIRECT: aux.callee = NULL      ; op[0] = the callee VALUE,
                            //           op[1..] = args. A call through a pointer names no
                            //           function, so every analysis must treat it as opaque.
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
        // IR_ELEM_PTR: how many ELEMENTS the access through this address spans. 1 for an
        // ordinary `a[i]`; N for a WIDE access — a 32-lane vector load starting at `i` reads
        // a[i..i+32), so its obligation is `i + 32 <= len`, not `i < len`. Without the width
        // the IR cannot state the difference and a wide read past the end looks in bounds.
        int32_t     elem_width;
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
// Terminators (local/internal/design/ir.md §5) — every block ends in exactly one.
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
// Basic blocks & functions (local/internal/design/ir.md §6). The CFG the fixpoint
// iterates.
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
    // ── Did the SOURCE write a `decreasing <measure>` on THIS loop? ─────────────────────
    // A source fact the IR could not state, and its absence was a real hole. The sovereign
    // engine raises a loop-termination obligation only inside a `func`, because totality is a
    // `func` requirement and a `proc` may loop forever by design. But `decreasing` is accepted
    // on ANY loop, and the old engine verified it wherever it was written — so standing the
    // legacy loop checks down silently accepted three corpus programs whose whole subject is a
    // written measure that does not hold (a step whose range includes zero, a signed halving,
    // a binary search that sticks at hi-lo == 1). All three are `proc`s.
    //
    // The language answer (D-44) is that a written measure is a CLAIM THE COMPILER DEFENDS —
    // the same relationship `effects ...` has to the effect row: it unlocks no inference, it
    // states an intention and the compiler refuses to let it drift. Defending it needs exactly
    // this bit, per loop rather than per function, because a `proc` may hold both a checked
    // loop and a deliberately unbounded one.
    bool      has_measure;
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
    // AUDIT (effect_lattice_audit.md): Lain has NO mutable globals — top-level bindings are
    // compile-time constants (E100) — so this bit's domain is EMPTY by language design. Its
    // only live trigger is an OPAQUE's declared write footprint, i.e. "unmodelled code might
    // write", which is a fail-closed signal rather than an observation. Reserved, not dead by
    // oversight. The PARAMETER-write channel is C5's IrWriteFootprint, which is separate and
    // live — the row was never the right shape for it.
    // ★ RENAMED FROM IR_EFFECT_WRITE (2026-09-24). The comment above already said the domain
    // is empty and the only live trigger is an OPAQUE's declared write footprint — so the NAME
    // described a thing that cannot happen, while the BIT meant "unmodelled code might write".
    // Those are different claims, and a reader had to get to the fourth line of the comment to
    // learn which one was in force. `effects write` is no longer sayable in source for the same
    // reason: a bound on an impossible effect is an assertion of nothing (L3).
    IR_EFFECT_UNMODELLED_WRITE = 1 << 0,
    IR_EFFECT_DIVERGE = 1 << 1,  // may not terminate (unbounded loop / non-well-founded recursion)
    IR_EFFECT_RAISES  = 1 << 2,  // may panic / abort
    // An extern's IO bit defaults from WHICH KEYWORD the programmer wrote (`extern func` is
    // trusted pure, `extern proc` is IO) — consistent with "believed on an extern", but the
    // one Lain-shaped default in this row: a front end with no func/proc split cannot express
    // it. F3's `effects` clause is the neutral carrier; the keyword is only the default.
    IR_EFFECT_IO      = 1 << 3,  // external side effects (calls a proc / extern proc)
    IR_EFFECT_ALLOC   = 1 << 4,  // PRODUCES owned storage it did not receive — an owned
                                 // pointer/slice return whose provenance does not root in a
                                 // parameter. Language-neutral, and the distinction the row
                                 // could not previously draw (mem_alloc vs mem_free).
} IrEffectBit;
typedef unsigned IrEffect;

// ── C5: the WRITE FOOTPRINT ─────────────────────────────────────────────────
// The bits above are a coarse binary — IR_EFFECT_UNMODELLED_WRITE means "writes mutable GLOBAL state"
// and explicitly EXCLUDES a write through a parameter, so the row could not say WHICH memory
// a function touches. That is the "region footprints" half of the effect row (C5), and its
// absence is not academic: whether a callee actually writes through a reference parameter
// decides whether two arguments naming the same place is undefined behaviour at all. C's
// `restrict` is only violated by a WRITE, so `f(var a[i], var a[i])` where f writes neither
// parameter is legal — and was being rejected.
//
// Universal: Fortran states it natively (INTENT(IN/OUT/INOUT)), Rust in the &/&mut split,
// C in `const`. Conservative default: unknown ⇒ assume written.
typedef uint64_t IrWriteFootprint;   // bit k = may write through parameter k

// ── The RETENTION footprint — the companion the alias oracle needs ───────────
// bit k = "a POINTER derived from parameter k may be STORED into memory that outlives the
// call". `stash(var h, var x) { h.r = var x }` retains x; `bump(var x) { x = x + 1 }` does
// not — it stores an integer derived from x, not x's address.
//
// Why it exists: the numeric domain wants to stop havocing every escaped cell at every call,
// and havoc only what THIS call can reach. That is sound exactly when a callee cannot squirrel
// an address away for someone else to write later. Retention is the precise statement of when
// it can, and it is the fact Rust's lifetimes encode in a signature.
typedef uint64_t IrRetainFootprint;

typedef struct IrFunc {
    IrName    *name;
    IrFuncKind kind;
    // ── MAY THIS FUNCTION RUN FOREVER? ──────────────────────────────────────────────────
    // Set from a declared `effects diverge`. Termination is an obligation for EVERY loop
    // unless this is true — the default is "it terminates", and the exception is written.
    //
    // ★ It used to be the other way round, keyed on `kind`: a `proc` could loop forever
    // because the keyword that lets a function print also let it hang. Those are independent
    // questions and conflating them cost the guarantee: measured over the corpus, 173 procs
    // did IO and terminated while being exempt from the check, against 61 that actually
    // diverge — and no function in `std/` needs the permission at all.
    bool       may_diverge;
    // ── OPTIMIZER METADATA THE FRONT END DECLARED ────────────────────────────────────────
    // ⚠ These were LOST when src/emit/ was deleted (2026-09-23): the AST emitter wrote
    // `__attribute__((cold))`, `((hot))` and `((noreturn))`, the IR never carried them, and the
    // one gate that compared annotations — `annot_gate` — was retired in the same commit. The
    // baseline was then recorded from the backend that had already dropped them, so it could not
    // see the loss either. Behaviour is identical (they are hints), which is exactly why nothing
    // caught it: the same blind spot as D-62, one subsystem over.
    bool       is_cold, is_hot, is_noreturn, is_allocator;
    bool       is_extern;   // declaration only (no body) — a trusted boundary
    bool       is_variadic; // `...` — a C-style variadic boundary (printf and friends). The
                            // IR must carry it or the emitter cannot declare the function at
                            // all, and every call to one becomes an implicit-declaration
                            // error in the generated C.
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
    IrWriteFootprint param_writes;
    IrRetainFootprint param_retains;           // which parameters' ADDRESSES may outlive the call
    bool              param_retains_done;
    bool       param_writes_done;              // C5: which parameters this function may write
    // B5 region info: does the RETURN borrow from a parameter, and from WHICH? Lain has no
    // lifetime syntax, so the relationship must be recovered, or `r = get_ref(var d)` looks
    // like a plain value and the loan on `d` is invisible across statements.
    //
    // WHETHER the return is a reference is a SIGNATURE fact and now lives where signature
    // facts belong — on `ret_type->borrowed`, set at lowering, readable by every client
    // including the backends. It used to be `IrFunc.ret_borrows`, a boolean beside a type that
    // contradicted it; `ir_ret_is_borrow()` is the reader.
    //
    // The SOURCE is a BODY fact and is INFERRED by analysis/borrow.h, never guessed from the
    // signature: `pick(var a, var b) var i32` may return either, and picking "the first
    // mutable param" mis-attributes the loan for `return var b.x`, silently losing every
    // conflict on `b`. Rust cannot infer this and rejects such a signature outright ("missing
    // lifetime specifier"); being whole-program, we read it off the returns instead.
    // Did the SOURCE carry a `decreasing <measure>` clause? A source fact, and the IR had no
    // way to state it — which mattered the moment the sovereign engine started reporting
    // recursion, because Annex B makes the DIAGNOSTIC depend on it: E011 is "a direct
    // self-recursion for which no measure can be inferred", E082/E091 "a present-but-failing
    // measure". Without the bit the engine could only pick one code and be wrong about half
    // the programs — normatively wrong, against a spec that already decided.
    //
    // It is also the first plank of the open language question (D-44): a written `decreasing`
    // in a `proc` is a claim nothing sovereign currently checks, and checking it needs exactly
    // this fact, per loop rather than per function.
    bool       has_decreasing;
    uint64_t   ret_borrow_mask;      // bit i = it may borrow from param i (0 with a borrowed ret
    bool       ret_borrow_mask_done; // is impossible: the fallback is every reference param)
    // F1: the DECLARED `in <param>` mask, if the signature carried one. Kept apart from the
    // inferred mask on purpose — on an extern it REPLACES inference (there is nothing to
    // infer from), and on a function with a body it is CHECKED against it.
    bool       ret_borrow_annot;     // an annotation was written
    uint64_t   ret_borrow_annot_mask;
    bool       ret_borrow_annot_wrong;  // the annotation claims LESS than the body does

    // INFERRED RETURN RANGE (the numeric analogue of ret_borrow_mask): what interval the
    // callee's result provably lies in, taken from its body rather than its type. Without it
    // a call's result is unknown and `LUT[nib(c)]` cannot be proven, though `nib` returns
    // `c & 0x0F` and can only be 0..15. Memoized because the query is per call site.
    //   0 = not computed, 1 = computation in progress (recursive: fall back), 2 = done
    int8_t   ret_range_state;
    int64_t  ret_range_lo, ret_range_hi;
    Arena     *arena;       // where this function's IR is allocated
    struct IrFunc *next;
} IrFunc;

// Does this function return a BORROW — a reference into storage the caller owns? ONE reader
// for what used to be `IrFunc.ret_borrows`, a boolean beside a type that contradicted it, so
// the question now has a single answer and every client asks the TYPE. `is_raw` is what keeps
// a plain `*T` the programmer took out of it: a raw address carries no discharged guarantee
// and must not create a loan.
static inline bool ir_ret_is_borrow(const IrFunc *f) {
    return f && f->ret_type && f->ret_type->borrowed && !f->ret_type->is_raw;
}

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
