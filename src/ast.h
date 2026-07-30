#ifndef AST_H
#define AST_H

#include "utils/arena.h"
#include "token.h"

/*──────────────────────────────────────────────────────────────────╗
│ FORWARD DECLARATIONS                                              │
╚──────────────────────────────────────────────────────────────────*/

typedef struct Id Id;

typedef struct Decl Decl;
typedef struct Stmt Stmt;
typedef struct Expr Expr;

typedef struct IdList IdList;
typedef struct ExprList ExprList;
typedef struct StmtList StmtList;
typedef struct DeclList DeclList;

/*──────────────────────────────────────────────────────────────────╗
│ IDENTIFIER NODE                                                   │
╚──────────────────────────────────────────────────────────────────*/

typedef struct Id {
    isize       length;
    const char* name;
} Id;

/*──────────────────────────────────────────────────────────────────╗
│ TYPE NODE                                                   │
╚──────────────────────────────────────────────────────────────────*/

// Ownership mode for linear type system
typedef enum {
    MODE_OWNED,    // mov T - linear, must consume exactly once
    MODE_SHARED,   // T     - immutable borrow (default)
    MODE_MUTABLE,  // mut T - mutable borrow
} OwnershipMode;

typedef enum {
    TYPE_SIMPLE,    // e.g. "u8", "Kind"
    TYPE_ARRAY,     // e.g. "u8[]"
    TYPE_SLICE,     // e.g. "u8[:0]"
    TYPE_POINTER,   // pointer to element type, e.g. "u8 *"
    TYPE_COMPTIME,  // comptime modifier
    TYPE_VARIANT,   // inner payload of an ADT variant
    TYPE_FUNC,      // non-capturing function pointer: *func(params) ret / *proc(params) ret
    TYPE_META,      // the meta-type `type` — the "type" of a type parameter (`T type`)
} TypeKind;

typedef struct Type {
    TypeKind kind;
    OwnershipMode mode;         // Ownership semantics (owned/shared/mutable)
    Id* base_type;              // The base type, e.g., "u8"
    struct Type* element_type;  // Used for nested arrays, e.g., "SomeType[][]"

    /* For fixed-length arrays (TYPE_ARRAY), or -1 for dynamic-length arrays:
       - array_len >= 0 : a compile-time fixed length (u8[5])
       - array_len == -1 : dynamic-length array / runtime slice (u8[])
    */
    isize array_len;

    /* Sized-slice annotations: i32[out.len], i32[>= n], i32[a.len + b.len]
       size_expr  == NULL → no size constraint (plain dynamic slice)
       size_relop == TOKEN_EQUAL_EQUAL → arr.len == size_expr  (i32[out.len])
       size_relop == other relop       → arr.len relop size_expr (i32[>= n])
    */
    struct Expr *size_expr;
    TokenKind    size_relop;

    const char* sentinel_str;
    isize       sentinel_len;
    bool        sentinel_is_string;

    /* TYPE_FUNC (non-capturing function pointer, `*func(P..)R` / `*proc(P..)R`):
       func_params = ordered parameter types; element_type = return type (NULL = void);
       func_is_total = true for `func` (provably terminating), false for `proc`. */
    struct TypeList *func_params;
    bool            func_is_total;

    /* Generic type-application `Vec(i32)`: a TYPE_SIMPLE whose base_type names a
       generic type, with the concrete type arguments here (NULL = not an
       application). Sema instantiates it and rewrites the node to the instance. */
    struct TypeList *type_args;

    /* P2/S1: cached integer width/signedness on the node, so the type name
       ("i32") is parsed once instead of on every width query.
       int_width_cache: 0 = not computed yet, -1 = not an integer, 1..64 = width. */
    signed char int_width_cache;
    bool        int_signed_cache;

    /* P2/Stage0: the mode/refinement-stripped canonical CORE of this type.
       Set to the interned core by every constructor; ownership wrappers
       (type_move/type_mut) inherit it via struct-copy, so `mov i32`, `var i32`
       and `i32` all share one canon. NULL only on Types built without a
       constructor — core_identical() falls back to pointer identity there. */
    struct Type *canon;

    struct Variant* variant; // For TYPE_VARIANT
} Type;


/*──────────────────────────────────────────────────────────────────╗
│ LIST NODES                                                        │
╚──────────────────────────────────────────────────────────────────*/

typedef struct IdList {
    Id*       id;
    IdList*   next;
} IdList;

typedef struct ExprList {
    Expr*       expr;
    ExprList*   next;
} ExprList;

typedef struct StmtList {
    Stmt*       stmt;
    StmtList*   next;
} StmtList;

typedef struct DeclList{
    Decl*       decl;
    DeclList*   next;
} DeclList;

typedef struct TypeList {
    struct Type*     type;
    struct TypeList* next;
} TypeList;

/*──────────────────────────────────────────────────────────────────╗
│ DECLARATION NODES                                                 │
╚──────────────────────────────────────────────────────────────────*/

typedef enum {
    DECL_VARIABLE,
    DECL_FUNCTION,
    DECL_PROCEDURE,
    DECL_EXTERN_FUNCTION,
    DECL_EXTERN_PROCEDURE,
    DECL_STRUCT,
    DECL_ENUM,
    DECL_IMPORT,
    DECL_EVAL_IMPORT,
    DECL_C_INCLUDE,
    DECL_DESTRUCT,
    DECL_EXTERN_TYPE,
    DECL_TYPE_ALIAS, // New: type Name = Expr
} DeclKind;

typedef struct {
    Id *name;
} DeclExternType;

typedef struct {
    IdList* names;  // The fields to extract
    Type*   type;   // The struct type
} DeclDestruct;

typedef struct {
    Id*   name;
    Type* type;

    // OPTIONAL "in <identifier>" annotation used in struct field
    // definitions like: `cursor u8 in text`
    Id*   in_field;
    
    // Equation-style constraints: b int != 0, x int >= 0 and <= 100
    ExprList* constraints;
    
    bool  is_parameter; // New: true if this is a function parameter
    bool  is_mutable;   // New: true if declared with 'var' (mutable binding)
} DeclVariable;

typedef struct {
    Id* name;
    Expr* expr; // The right-hand side of type Alias = Expr
    struct ExprList *constraints; // Q-002 refinement: type Foo = int >= 0 and <= 100
} DeclTypeAlias;

typedef struct Variant {
    Id*       name;
    DeclList* fields; // NULL if no fields (like a simple enum variant)
    struct Variant* next;
} Variant;

typedef struct EnumDecl {
    Id* type_name;          // Enum name
    Variant* variants;      // Linked list of variants
    DeclList* type_params;  // generic params `type R(T type){...}` (NULL = non-generic)
} DeclEnum;

typedef struct StructDecl {
    Id* name;          // Struct name
    DeclList* fields;  // List of fields (should be DeclList)
    bool is_packed;    // Q-002 / Sprint 19: bit-exact layout via [packed]
    DeclList* type_params;  // generic params `type Vec(T type){...}` (NULL = non-generic)
} DeclStruct;

typedef struct {
    Id*         name;           // Function name
    DeclList*   params;         // Parameters (linked list or array)
    Type*       return_type;    // Changed to Type* to support array types
    StmtList*   body;           // Function body
    ExprList*   pre_contracts;  // New: pre-conditions (requires/pre)
    ExprList*   post_contracts; // New: post-conditions (ensures/post)
    ExprList*   return_constraints; // Equation-style: func f() int >= 0
    bool        is_extern;      // true for “extern func”
    bool        is_variadic;    // true for “...”
    bool        is_cold;        // @cold:     GCC moves to .text.cold, pessimizes branch
    bool        is_hot;         // @hot:      GCC optimizes aggressively, prefers inline
    bool        is_allocator;   // @allocator: return ptr doesn't alias any existing ptr
    bool        is_noreturn;    // @noreturn:  function never returns (exit, panic, etc.)
} DeclFunction;

typedef struct {
    Id *module_name;   // contains "foo.bar"
} DeclImport;

typedef struct {
    const char *path;
} DeclCInclude;

// Attribute support (Q-017): [name] or [name(args)] before declarations
typedef struct Attr {
    Id *name;             // attribute name (e.g., "fast_math", "private")
    struct ExprList *args; // optional args
    struct Attr *next;
} Attr;

typedef struct Decl {
    DeclKind kind;
    union {
        DeclVariable    variable_decl;
        DeclStruct      struct_decl;
        DeclEnum        enum_decl;
        DeclFunction    function_decl;
        DeclImport      import_decl;
        DeclCInclude    c_include_decl;
        DeclDestruct    destruct_decl;
        DeclExternType  extern_type_decl;
        DeclTypeAlias   type_alias_decl;
    } as;
    isize line;  // NEW
    isize col;   // NEW
    Attr *attributes;   // Q-017: linked list of attributes; NULL if none
    bool is_private;    // Q-018: cached from [private] attribute (default: false = public)
    const char *defining_module;  // Q-018: module path where this decl was defined (set by load_module); NULL = current/main
} Decl;

/*──────────────────────────────────────────────────────────────────╗
│ STATEMENT NODES                                                   │
╚──────────────────────────────────────────────────────────────────*/

typedef enum {
    STMT_VAR,
    STMT_ASSIGN,
    STMT_EXPR,
    STMT_IF,
    STMT_FOR,
    STMT_CONTINUE, // continue has no payload
    STMT_BREAK,     // break has no payload
    STMT_MATCH_CASE,
    STMT_MATCH,
    STMT_USE,
    STMT_RETURN,
    STMT_UNSAFE,
    STMT_WHILE,
    STMT_DEFER,
    STMT_COMPTIME_IF,
} StmtKind;

typedef struct {
    Id*         name;     // Variable declaration
    Type*       type;     // NULL if no annotation
    Expr*       expr;     // NULL if no init
    bool        is_mutable; // New: true if declared with 'var'
    bool        explicit_undefined; // true iff user wrote `= undefined` (not synthesized)
    Expr*       in_expr;  // NULL, or the container for `var p *T in arr` (local pointer invariant)
} StmtVar;

typedef struct {
    //Id*         name;       // Assignment target
    Expr*       target;     // Assignment target (any expression, typically an identifier or member‐access)
    Expr*       expr;       // Assigned expression
    bool        is_const;    // ← new field: true if turned into a decl
} StmtAssign;

typedef struct {
    Expr* expr; // Expression used as a statement
} StmtExpr;

typedef struct {
    Expr *target;     // the thing being aliased (identifier or member)
    Id   *alias_name; // the new local name
} StmtUse;

typedef struct StmtIf {
    Expr *cond;
    StmtList *then_body;
    StmtList *else_branch;  // possibly NULL, or a single-item list if it’s an “else if”
  } StmtIf;

typedef struct {
    Id       *index_name;  // may be NULL if you wrote `for c in …`
    Id       *value_name;  // always non‐NULL
    Expr     *iterable;
    StmtList *body;
} StmtFor;

typedef struct {
    Expr     *cond;
    Expr     *measure;   // termination measure (NULL = unbounded, banned in func)
    StmtList *body;
} StmtWhile;

typedef struct StmtMatchCase {
    ExprList        *patterns;        // NULL for `else`
    StmtList        *body;
    struct StmtMatchCase   *next;
} StmtMatchCase;

typedef struct {
    Expr *value;
    StmtMatchCase *cases;
    bool is_borrowed;  // true for `case &expr { ... }` — non-consuming match
} StmtMatch;

typedef struct {
    Expr *value;    // the expression to return
} StmtReturn;

typedef struct {
    StmtList *body;
} StmtUnsafe;

typedef struct {
    struct Stmt *stmt; // The statement to be executed later (usually a block or an expression)
} StmtDefer;

typedef struct {
    Expr *cond;              // compile-time condition (e.g. @os == .Linux)
    StmtList *then_body;
    StmtList *else_branch;   // NULL if no else
    bool evaluated;          // true after sema has resolved this
    bool is_taken;           // which branch was selected (true = then, false = else)
} StmtComptimeIf;

typedef struct Stmt {
    StmtKind kind;
    isize line;  // source line number
    isize col;   // source column number
    union {
        StmtVar         var_stmt;
        StmtAssign      assign_stmt;
        StmtExpr        expr_stmt;
        StmtIf          if_stmt;
        StmtFor         for_stmt;
        StmtUse         use_stmt;
        StmtMatch       match_stmt;
        StmtReturn      return_stmt;
        StmtUnsafe      unsafe_stmt;
        StmtWhile       while_stmt;
        StmtDefer       defer_stmt;
        StmtComptimeIf  comptime_if_stmt;
    } as;
} Stmt;

/*──────────────────────────────────────────────────────────────────╗
│ EXPRESSION NODES                                                  │
╚──────────────────────────────────────────────────────────────────*/

typedef enum {
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_IDENTIFIER,
    EXPR_LITERAL,
    EXPR_MEMBER,
    EXPR_CALL,
    EXPR_STRING,
    EXPR_CHAR,
    EXPR_RANGE,
    EXPR_INDEX,
    EXPR_MOVE,
    EXPR_MUT, // New
    EXPR_CAST, // x as Type
    EXPR_FLOAT_LITERAL,
    EXPR_MATCH,
    EXPR_UNDEFINED, // New
    EXPR_TYPE,      // a resolved Type* used as a parameter value
    EXPR_ANON_STRUCT,
    EXPR_ANON_ENUM,
    EXPR_ARRAY_LITERAL,
    EXPR_BUILTIN,
    EXPR_ADDR,   // &lvalue — address of an element
    EXPR_DEREF,  // *ptr   — dereference a pointer (safe when ptr in arr proven)
} ExprKind;

typedef enum {
    BUILTIN_OS,
    BUILTIN_ARCH,
    BUILTIN_LIKELY,          // @likely(cond)        → __builtin_expect(!!(cond), 1)
    BUILTIN_UNLIKELY,        // @unlikely(cond)      → __builtin_expect(!!(cond), 0)
    BUILTIN_ASSUME_ALIGNED,  // @assume_aligned(p,N) → __builtin_assume_aligned(p, N)
} BuiltinKind;

typedef struct {
    DeclList* fields;
} ExprAnonStruct;

typedef struct {
    Variant* variants;
} ExprAnonEnum;

typedef struct {
    Expr*       left;   // Left operand
    TokenKind   op;     // Operator
    Expr*       right;  // Right operand
    bool        l3_upper_dead; // L3: upper-bound part of `ptr in arr` is dead code
    bool        l3_lower_dead; // L3: lower-bound part of `ptr in arr` is dead code
} ExprBinary;

typedef struct {
    TokenKind   op;     // Unary operator
    Expr*       right;  // Right operand
} ExprUnary;

typedef struct {
    Expr*       expr;   // The expression being moved
} ExprMove;

typedef struct {
    Expr*       expr;   // lvalue whose address is taken (&expr)
} ExprAddr;

typedef struct {
    Expr*       expr;   // pointer being dereferenced (*ptr)
} ExprDeref;

typedef struct {
    Expr*       expr;
} ExprMut; // New

typedef struct {
    Expr*       expr;        // expression being cast
    Type*       target_type; // target type
} ExprCast;


typedef struct {
    Id*         id;   // Identifier name
} ExprIdentifier;

typedef struct {
    int64_t     value;  // Literal integer value (64-bit; was 32-bit int — truncated
                        // any literal above 2^31-1, e.g. i64/u64 constants).
} ExprLiteral;

typedef struct {
       Expr *target;    // the “a”
       Id   *member;    // the “b”
} ExprMember;

typedef struct {
    Expr*       callee;   // The function being called (could be an identifier or another expression)
    ExprList*   args;     // A list of argument expressions
} ExprCall;

typedef struct {
    const char* value;
    isize       length;
} ExprString;

typedef struct {
    Expr *start;
    Expr *end;
    bool  inclusive;    // true for '..=' (inclusive), false for '..' (exclusive)
} ExprRange;

typedef struct {
    char value;
} ExprChar;

typedef struct {
    double value;
} ExprFloat;

typedef struct {
    Expr *target;   // e.g. the `text`
    Expr *index;    // e.g. the `start..cursor` (an ExprRange)
} ExprIndex;

typedef struct ExprMatchCase {
    ExprList        *patterns;        // NULL for `else`
    struct Expr     *body;
    struct ExprMatchCase   *next;
} ExprMatchCase;

typedef struct {
    struct Expr *value;
    ExprMatchCase *cases;
    bool is_borrowed;  // true for `case &expr { ... }` — non-consuming match
} ExprMatch;

typedef struct {
    Type *type_value; // The resolved type
} ExprType;

typedef struct {
    ExprList *elements;
} ExprArrayLiteral;

typedef struct {
    BuiltinKind builtin_kind;
    struct Expr *arg;   // argument for @likely/@unlikely/@assume_aligned; NULL for @os/@arch
    isize       align;  // alignment value for BUILTIN_ASSUME_ALIGNED
} ExprBuiltin;

typedef struct Expr {
    ExprKind kind;
    isize line;  // source line number
    isize col;   // source column number
    union {
        ExprBinary      binary_expr;
        ExprUnary       unary_expr;
        ExprIdentifier  identifier_expr;
        ExprLiteral     literal_expr;
        ExprMember      member_expr;
        ExprCall        call_expr;
        ExprString      string_expr;
        ExprChar        char_expr;
        ExprRange       range_expr;
        ExprIndex       index_expr;
        ExprMove        move_expr;
        ExprMut         mut_expr; // New
        ExprCast        cast_expr;
        ExprFloat       float_expr;
        ExprMatch       match_expr;
        ExprType        type_expr;
        ExprAnonStruct  anon_struct_expr;
        ExprAnonEnum    anon_enum_expr;
        ExprArrayLiteral array_literal_expr;
        ExprBuiltin     builtin_expr;
        ExprAddr        addr_expr;
        ExprDeref       deref_expr;
    } as;
    Type *type;
    Decl *decl;      // The declaration this expression refers to (if any)
    bool  is_global; // True if this refers to a global symbol
} Expr;

/*──────────────────────────────────────────────────────────────────╗
│ ID CONSTRUCTOR                                                    │
╚──────────────────────────────────────────────────────────────────*/

Id *id(Arena *arena, isize length, const char* name) {
    Id *id = arena_push_aligned(arena, Id);
    id->length = length;
    id->name = name;
    return id;
}

/*──────────────────────────────────────────────────────────────────╗
│ TYPE CONSTRUCTORS                                               │
╚──────────────────────────────────────────────────────────────────*/

// P2/S1b: intern TYPE_SIMPLE nodes so equal scalar/nominal types (same name,
// same mode) share ONE canonical, immutable node. Safe: Type nodes are now
// immutable and nothing compares Type* by identity. Single-shot compiler, so a
// global list is fine.
typedef struct InternedSimple {
    Type *type;
    struct InternedSimple *next;
} InternedSimple;
static InternedSimple *g_interned_simple = NULL;

static bool id_bytes_equal(Id *a, Id *b) {
    if (!a || !b || a->length != b->length) return false;
    for (isize i = 0; i < a->length; i++) if (a->name[i] != b->name[i]) return false;
    return true;
}

// P2/Stage0: parse an iN/uN type name to width (1..64) + signedness. Sets
// *w = -1 for non-fixed-width names. This is the AUTHORITATIVE source of a
// TYPE_SIMPLE's integer width — set eagerly at construction (below), not
// re-derived from the name string on each query.
static void ast_parse_int_width(const char *n, isize len, signed char *w, bool *sgn) {
    *w = -1; *sgn = false;
    if (n && len >= 2 && len <= 3 && (n[0] == 'i' || n[0] == 'u')) {
        int v = 0; bool ok = true;
        for (isize k = 1; k < len; k++) {
            if (n[k] < '0' || n[k] > '9') { ok = false; break; }
            v = v * 10 + (n[k] - '0');
        }
        if (ok && v >= 1 && v <= 64) { *w = (signed char)v; *sgn = (n[0] == 'i'); }
    }
}

// Simple names: no element_type, no sentinel
Type *type_simple(Arena *arena, Id *base) {
    for (InternedSimple *it = g_interned_simple; it; it = it->next) {
        if (it->type->mode == MODE_SHARED && id_bytes_equal(it->type->base_type, base))
            return it->type;  // canonical, deduplicated
    }
    Type *t = arena_push_aligned(arena, Type);
    t->kind      = TYPE_SIMPLE;
    t->mode      = MODE_SHARED;  // default ownership
    t->base_type = base;
    // Authoritative integer width/signedness, computed once at construction.
    ast_parse_int_width(base ? base->name : NULL, base ? (isize)base->length : 0,
                        &t->int_width_cache, &t->int_signed_cache);
    t->canon = t;  // interned scalar/nominal core is its own canon
    InternedSimple *node = arena_push_aligned(arena, InternedSimple);
    node->type = t;
    node->next = g_interned_simple;
    g_interned_simple = node;
    return t;
}

/* Modify type_array constructor to accept a length parameter.
   array_len >= 0 -> fixed-length array
   array_len == -1 -> dynamic-length array (slice-like) */
Type *type_array(Arena *arena, Type *element_type, isize array_len) {
    Type *t = arena_push_aligned(arena, Type);
    t->kind         = TYPE_ARRAY;
    t->mode         = MODE_SHARED;  // default ownership
    t->element_type = element_type;
    t->array_len    = array_len;
    t->canon = t;
    return t;
}

/* Dynamic slice with a size constraint expression: i32[out.len], i32[>= n].
   size_relop defaults to TOKEN_EQUAL_EQUAL for the implicit-equality case. */
Type *type_sized_array(Arena *arena, Type *element_type, struct Expr *size_expr, TokenKind size_relop) {
    Type *t = type_array(arena, element_type, -1);
    t->size_expr  = size_expr;
    t->size_relop = size_relop;
    return t;
}

// Slices with a compile-time sentinel
Type *type_slice(Arena *arena, Type *element_type, const char *sentinel_str,
                 isize sentinel_len, bool sentinel_is_string) {
    Type *t = arena_push_aligned(arena, Type);
    t->kind              = TYPE_SLICE;
    t->mode              = MODE_SHARED;  // default ownership
    t->element_type      = element_type;
    t->sentinel_str      = sentinel_str;
    t->sentinel_len      = sentinel_len;
    t->sentinel_is_string = sentinel_is_string;
    t->canon = t;
    return t;
}

// Create a copy of a type with MODE_OWNED (linear/move semantics)
Type *type_move(Arena *arena, Type *inner) {
    assert(inner != NULL);
    Type *t = arena_push_aligned(arena, Type);
    *t = *inner;           // Copy all fields from inner type
    t->mode = MODE_OWNED;  // Override mode to owned
    return t;
}

// Create a copy of a type with MODE_MUTABLE (mutable borrow)
Type *type_mut(Arena *arena, Type *inner) {
    assert(inner != NULL);
    Type *t = arena_push_aligned(arena, Type);
    *t = *inner;            // Copy all fields from inner type
    t->mode = MODE_MUTABLE; // Override mode to mutable
    return t;
}




Type *type_comptime(Arena *arena, Type *base) {
    assert(base != NULL);
    Type *t = arena_push_aligned(arena, Type);
    t->kind         = TYPE_COMPTIME;
    t->mode         = base->mode;  // preserve ownership
    t->element_type = base;
    t->canon = t;
    return t;
}

Type *type_pointer(Arena *arena, Type *element_type) {
    assert(element_type != NULL);
    Type *t = arena_push_aligned(arena, Type);
    t->kind         = TYPE_POINTER;
    t->mode         = MODE_SHARED;  // pointers default to shared
    t->element_type = element_type;
    t->canon = t;
    return t;
}

// A generic type-application `Name(args)` in type position (e.g. `Vec(i32)`).
// A fresh, non-interned TYPE_SIMPLE carrying the type arguments; sema replaces
// it with the concrete instance type.
Type *type_application(Arena *arena, Id *name, struct TypeList *type_args) {
    Type *t = arena_push_aligned(arena, Type);
    t->kind      = TYPE_SIMPLE;
    t->mode      = MODE_SHARED;
    t->base_type = name;
    t->type_args = type_args;
    t->canon     = t;
    return t;
}

// The meta-type `type` — the "type" of a type parameter (`T type`). A parameter
// whose type is TYPE_META is a type parameter; the function/type is generic.
Type *type_meta(Arena *arena) {
    Type *t = arena_push_aligned(arena, Type);
    t->kind = TYPE_META;
    t->mode = MODE_SHARED;
    t->canon = t;
    return t;
}

// Non-capturing function pointer type. `ret == NULL` means a void return.
// `is_total` distinguishes `*func` (provably terminating) from `*proc`.
Type *type_func(Arena *arena, TypeList *params, Type *ret, bool is_total) {
    Type *t = arena_push_aligned(arena, Type);
    t->kind          = TYPE_FUNC;
    t->mode          = MODE_SHARED;  // a bare function pointer is a shared value
    t->element_type  = ret;          // return type (NULL = void)
    t->func_params   = params;
    t->func_is_total = is_total;
    t->canon = t;
    return t;
}


// Helper: get underlying type without ownership wrapper
static inline Type *type_unwrap(Type *t) {
    // With the new system, mode is a field, not a wrapper type
    // So "unwrapping" just returns the same type pointer
    return t;
}

// Helper: check if type has linear/owned semantics
static inline bool type_is_linear(Type *t) {
    return t && t->mode == MODE_OWNED;
}

// Helper: check if type is a mutable borrow
static inline bool type_is_mutable(Type *t) {
    return t && t->mode == MODE_MUTABLE;
}

/*──────────────────────────────────────────────────────────────────╗
│ LISTS CONSTRUCTORS                                                │
╚──────────────────────────────────────────────────────────────────*/

IdList *id_list(Arena *arena, Id *id) {
    IdList *l = arena_push_aligned(arena, IdList);
    l->id = id;
    l->next = NULL;
    return l;
}

ExprList *expr_list(Arena *arena, Expr *expr) {
    ExprList *l = arena_push_aligned(arena, ExprList);
    l->expr = expr;
    l->next = NULL;
    return l;
}

TypeList *type_list(Arena *arena, Type *type) {
    TypeList *l = arena_push_aligned(arena, TypeList);
    l->type = type;
    l->next = NULL;
    return l;
}

StmtList *stmt_list(Arena *arena, Stmt *stmt) {
    StmtList *l = arena_push_aligned(arena, StmtList);
    l->stmt = stmt;
    l->next = NULL;
    return l;
}

DeclList *decl_list(Arena *arena, Decl *decl) {
    DeclList *l = arena_push_aligned(arena, DeclList);
    l->decl = decl;
    l->next = NULL;
    return l;
}

/*──────────────────────────────────────────────────────────────────╗
│ DECLARATION CONSTRUCTORS                                          │
╚──────────────────────────────────────────────────────────────────*/

Decl *decl_variable(Arena *arena, Id *name, Type *type) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_VARIABLE;
    d->as.variable_decl.name = name;
    d->as.variable_decl.type = type;
    d->as.variable_decl.in_field = NULL; // default: no "in" annotation
    d->as.variable_decl.constraints = NULL; // default: no constraints
    d->as.variable_decl.is_parameter = false;
    d->as.variable_decl.is_mutable = false; // default
    return d;
}

Decl *decl_function(Arena *arena, Id *name, DeclList *params, Type *return_type, StmtList *body, bool is_extern, bool is_variadic) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = is_extern ? DECL_EXTERN_FUNCTION : DECL_FUNCTION;
    d->as.function_decl.name = name;
    d->as.function_decl.params = params;
    d->as.function_decl.return_type = return_type;
    d->as.function_decl.body = body;
    d->as.function_decl.pre_contracts = NULL;
    d->as.function_decl.post_contracts = NULL;
    d->as.function_decl.return_constraints = NULL;
    d->as.function_decl.is_extern   = is_extern;
    d->as.function_decl.is_variadic = is_variadic;
    return d;
}

Decl *decl_procedure(Arena *arena, Id *name, DeclList *params, Type *return_type, StmtList *body, bool is_extern, bool is_variadic) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = is_extern ? DECL_EXTERN_PROCEDURE : DECL_PROCEDURE;
    d->as.function_decl.name = name;
    d->as.function_decl.params = params;
    d->as.function_decl.return_type = return_type;
    d->as.function_decl.body = body;
    d->as.function_decl.pre_contracts = NULL;
    d->as.function_decl.post_contracts = NULL;
    d->as.function_decl.return_constraints = NULL;
    d->as.function_decl.is_extern   = is_extern;
    d->as.function_decl.is_variadic = is_variadic;
    return d;
}

Decl* decl_struct(Arena* arena, Id* name, DeclList* fields) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_STRUCT;  // FIXED: Use correct enum value
    d->as.struct_decl.name = name;  // FIXED: Correct member
    d->as.struct_decl.fields = fields;  // FIXED: Correct member
    d->as.struct_decl.is_packed = false;
    d->as.struct_decl.type_params = NULL;
    return d;
}

Decl *decl_enum(Arena *arena, Id *type_name, Variant *variants) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_ENUM;
    d->as.enum_decl.type_name = type_name;
    d->as.enum_decl.variants = variants;
    d->as.enum_decl.type_params = NULL;
    return d;
}

// A decl is a generic TEMPLATE iff it carries a type parameter: a function with
// a parameter of meta-type `type`, or a struct/enum with a `(…)` header. Pure
// AST predicate (used by both sema/monomorph and emit, so it lives here).
static bool decl_is_generic_template(Decl *d) {
    if (!d) return false;
    if (d->kind == DECL_STRUCT) return d->as.struct_decl.type_params != NULL;
    if (d->kind == DECL_ENUM)   return d->as.enum_decl.type_params != NULL;
    if (d->kind == DECL_FUNCTION) {
        for (DeclList *p = d->as.function_decl.params; p; p = p->next)
            if (p->decl && p->decl->kind == DECL_VARIABLE &&
                p->decl->as.variable_decl.type &&
                p->decl->as.variable_decl.type->kind == TYPE_META)
                return true;
    }
    return false;
}

Variant *variant(Arena *arena, Id *name, DeclList *fields) {
    Variant *v = arena_push_aligned(arena, Variant);
    v->name = name;
    v->fields = fields;
    v->next = NULL;
    return v;
}

Decl* decl_import(Arena* arena, Id* module_name) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_IMPORT;
    d->as.import_decl.module_name = module_name;
    return d;
}

Decl* decl_destruct(Arena* arena, IdList* names, Type* type) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_DESTRUCT;
    d->as.destruct_decl.names = names;
    d->as.destruct_decl.type = type;
    return d;
}

Decl* decl_c_include(Arena* arena, const char* path) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_C_INCLUDE;
    d->as.c_include_decl.path = path;
    return d;
}

Decl *decl_extern_type(Arena *arena, Id *name) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_EXTERN_TYPE;
    d->as.extern_type_decl.name = name;
    return d;
}

/*──────────────────────────────────────────────────────────────────╗
│ STATEMENT CONSTRUCTORS                                            │
╚──────────────────────────────────────────────────────────────────*/

Stmt *stmt_var(Arena *arena, Id *name, Type* type, Expr *expr) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_VAR;
    s->as.var_stmt.name = name;
    s->as.var_stmt.expr = expr;
    s->as.var_stmt.type = type;
    s->as.var_stmt.is_mutable = false; // default
    s->as.var_stmt.explicit_undefined = false; // default (synthesized)
    s->as.var_stmt.in_expr = NULL; // default: no pointer invariant
    return s;
}

Stmt *stmt_unsafe(Arena *arena, StmtList *body) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_UNSAFE;
    s->as.unsafe_stmt.body = body;
    return s;
}

Stmt *stmt_assign(Arena *arena, Expr *lhs, Expr *rhs) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_ASSIGN;
    s->as.assign_stmt.target = lhs;
    s->as.assign_stmt.expr   = rhs;
    s->as.assign_stmt.is_const = false;  // default
    return s;
}

Stmt *stmt_expr(Arena *arena, Expr *expr) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_EXPR;
    s->as.expr_stmt.expr = expr;
    return s;
}

Stmt *stmt_if(Arena *arena, Expr *cond, StmtList *then_branch, StmtList *else_branch) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_IF;
    s->as.if_stmt.cond         = cond;
    s->as.if_stmt.then_body = then_branch;
    s->as.if_stmt.else_branch = else_branch;  // may be NULL
    return s;
}


Stmt *stmt_for(Arena *arena, Id *index_name, Id *value_name, Expr *iterable, StmtList *body)
{
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_FOR;
    s->as.for_stmt.index_name = index_name;
    s->as.for_stmt.value_name = value_name;
    s->as.for_stmt.iterable   = iterable;
    s->as.for_stmt.body       = body;
    return s;
}

Stmt *stmt_while(Arena *arena, Expr *cond, Expr *measure, StmtList *body) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_WHILE;
    s->as.while_stmt.cond = cond;
    s->as.while_stmt.measure = measure;
    s->as.while_stmt.body = body;
    return s;
}

Stmt *stmt_continue(Arena *arena) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_CONTINUE;
    return s;
}

Stmt *stmt_break(Arena *arena) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_BREAK;
    return s;
}

StmtMatchCase *stmt_match_case(Arena *arena, ExprList *patterns, StmtList *body) {
    StmtMatchCase *node = arena_push_aligned(arena, StmtMatchCase);
    node->patterns = patterns; // NULL means `else`
    node->body    = body;
    node->next    = NULL;
    return node;
}


Stmt *stmt_match(Arena *arena, Expr *value, StmtMatchCase *cases, bool is_borrowed) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_MATCH;
    s->as.match_stmt.value = value;
    s->as.match_stmt.cases = cases;
    s->as.match_stmt.is_borrowed = is_borrowed;
    return s;
}

Stmt *stmt_use(Arena *arena, Expr *target, Id *alias) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind                 = STMT_USE;
    s->as.use_stmt.target   = target;
    s->as.use_stmt.alias_name = alias;
    return s;
}

Stmt *stmt_return(Arena *arena, Expr *value) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_RETURN;
    s->as.return_stmt.value = value;
    return s;
}

Stmt *stmt_defer(Arena *arena, Stmt *deferred_stmt) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_DEFER;
    s->as.defer_stmt.stmt = deferred_stmt;
    return s;
}

Stmt *stmt_comptime_if(Arena *arena, Expr *cond, StmtList *then_branch, StmtList *else_branch) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_COMPTIME_IF;
    s->as.comptime_if_stmt.cond = cond;
    s->as.comptime_if_stmt.then_body = then_branch;
    s->as.comptime_if_stmt.else_branch = else_branch;
    s->as.comptime_if_stmt.evaluated = false;
    s->as.comptime_if_stmt.is_taken = false;
    return s;
}

/*──────────────────────────────────────────────────────────────────╗
│ EXPRESSION CONSTRUCTORS                                           │
╚──────────────────────────────────────────────────────────────────*/

Expr *expr_binary(Arena *arena, TokenKind op, Expr *left, Expr *right) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_BINARY;
    e->as.binary_expr.left = left;
    e->as.binary_expr.op = op;
    e->as.binary_expr.right = right;
    return e;
}

Expr *expr_unary(Arena *arena, TokenKind op, Expr *right) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_UNARY;
    e->as.unary_expr.op = op;
    e->as.unary_expr.right = right;
    return e;
}

Expr *expr_identifier(Arena *arena, Id *id) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_IDENTIFIER;
    e->as.identifier_expr.id = id;
    return e;
}

Expr *expr_literal(Arena *arena, int64_t value) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_LITERAL;
    e->as.literal_expr.value = value;
    return e;
}

Expr *expr_float_literal(Arena *arena, double value) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_FLOAT_LITERAL;
    e->as.float_expr.value = value;
    return e;
}

Expr *expr_member(Arena *arena, Expr *target, Id *member) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_MEMBER;
    e->as.member_expr.target = target;
    e->as.member_expr.member = member;
    return e;
}

Expr *expr_undefined(Arena *arena) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_UNDEFINED;
    return e;
}

Expr *expr_type(Arena *arena, Type *type_value) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_TYPE;
    e->as.type_expr.type_value = type_value;
    e->type = NULL; // A type expression has type `type`
    return e;
}

Expr *expr_call(Arena *arena, Expr *callee, ExprList *args) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_CALL;
    e->as.call_expr.callee = callee;
    e->as.call_expr.args = args;
    return e;
}

Expr *expr_string(Arena *arena, const char* value, isize length) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_STRING;
    e->as.string_expr.value = value;
    e->as.string_expr.length = length;
    return e;
}

Expr *expr_array_literal(Arena *arena, ExprList *elements) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_ARRAY_LITERAL;
    e->as.array_literal_expr.elements = elements;
    return e;
}

Expr *expr_char_literal(Arena *arena, unsigned char value) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_CHAR;
    e->as.char_expr.value = value;
    return e;
}

Expr *expr_range(Arena *arena, Expr *start, Expr *end, bool inclusive) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind               = EXPR_RANGE;
    e->as.range_expr.start     = start;
    e->as.range_expr.end       = end;
    e->as.range_expr.inclusive = inclusive;
    return e;
}

Expr *expr_index(Arena *arena, Expr *target, Expr *index) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_INDEX;
    e->as.index_expr.target = target;
    e->as.index_expr.index  = index;
    return e;
}

Expr *expr_move(Arena *arena, Expr *expr) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_MOVE;
    e->as.move_expr.expr = expr;
    return e;
}

Expr *expr_mut(Arena *arena, Expr *expr) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_MUT;
    e->as.mut_expr.expr = expr;
    return e;
}

Expr *expr_addr(Arena *arena, Expr *expr) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_ADDR;
    e->as.addr_expr.expr = expr;
    return e;
}

Expr *expr_deref(Arena *arena, Expr *expr) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_DEREF;
    e->as.deref_expr.expr = expr;
    return e;
}

Expr *expr_cast(Arena *arena, Expr *expr, Type *target_type) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_CAST;
    e->as.cast_expr.expr = expr;
    e->as.cast_expr.target_type = target_type;
    e->type = target_type;
    return e;
}

ExprMatchCase *expr_match_case(Arena *arena, ExprList *patterns, Expr *body) {
    ExprMatchCase *node = arena_push_aligned(arena, ExprMatchCase);
    node->patterns = patterns;
    node->body = body;
    node->next = NULL;
    return node;
}

Expr *expr_match(Arena *arena, Expr *value, ExprMatchCase *cases, bool is_borrowed) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_MATCH;
    e->as.match_expr.value = value;
    e->as.match_expr.cases = cases;
    e->as.match_expr.is_borrowed = is_borrowed;
    return e;
}

Expr *expr_anon_struct(Arena *arena, DeclList *fields) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_ANON_STRUCT;
    e->as.anon_struct_expr.fields = fields;
    e->type = NULL;
    return e;
}

Expr *expr_anon_enum(Arena *arena, Variant *variants) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_ANON_ENUM;
    e->as.anon_enum_expr.variants = variants;
    e->type = NULL;
    return e;
}

Decl *decl_type_alias(Arena *arena, Id *name, Expr *expr) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->attributes = NULL;
    d->is_private = false;
    d->defining_module = NULL;
    d->kind = DECL_TYPE_ALIAS;
    d->as.type_alias_decl.name = name;
    d->as.type_alias_decl.expr = expr;
    d->as.type_alias_decl.constraints = NULL;
    return d;
}

Expr *expr_builtin(Arena *arena, BuiltinKind kind) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_BUILTIN;
    e->as.builtin_expr.builtin_kind = kind;
    e->as.builtin_expr.arg = NULL;
    return e;
}

Expr *expr_builtin_arg(Arena *arena, BuiltinKind kind, Expr *arg) {
    Expr *e = expr_builtin(arena, kind);
    e->as.builtin_expr.arg = arg;
    return e;
}

Expr *expr_builtin_assume_aligned(Arena *arena, Expr *ptr, isize align) {
    Expr *e = expr_builtin_arg(arena, BUILTIN_ASSUME_ALIGNED, ptr);
    e->as.builtin_expr.align = align;
    return e;
}

#endif /* AST_H */
