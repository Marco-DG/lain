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
    // IRT_STRUCT — self-contained: carries its lowered field types + names so the
    // backend never re-touches the AST.
    IrName *sname;          // struct name (identity + C typedef name) — IR-owned
    struct IrType **fields; // lowered field types, in declaration order
    IrName **field_names;   // field names (for the C typedef)
    int   n_fields;
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
    // calls
    IR_CALL,                // aux.callee : Decl ; op[0..] = args
    // verification layer (Phase 2.9 — the assume/assert substrate)
    IR_ASSUME,              // op[0] = a bool that HOLDS here (guard/refinement/precondition)
    IR_ASSERT,              // op[0] = a bool the analysis must DISCHARGE (obligation)
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

typedef struct IrFunc {
    IrName    *name;
    IrFuncKind kind;
    IrParam   *params;      // parameter values
    IrType    *ret_type;
    IrBlock   *entry;
    IrBlock   *blocks;      // all blocks (list); entry is first
    IrBlock   *blocks_tail; // O(1) append
    int32_t    next_value_id;
    int32_t    next_block_id;
    void      *src_decl;    // OPAQUE provenance handle (front-end's; the IR never derefs it)
    bool       incomplete;  // lowering dropped/placeholder'd a construct ⇒ the IR is
                            // NOT faithful, so no analysis may claim a proof over it
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
