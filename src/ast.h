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

    // for TYPE_SLICE:
    const char* sentinel_str;
    isize       sentinel_len;
    bool        sentinel_is_string;
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
    DECL_DESTRUCT,
} DeclKind;

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
    
    bool  is_parameter; // New: true if this is a function parameter
} DeclVariable;


typedef struct EnumDecl {
    Id* type_name;          // Enum name
    IdList* variants;       // List of enum values (should be IdList, not DeclList)
} DeclEnum;

typedef struct StructDecl {
    Id* name;          // Struct name
    DeclList* fields;  // List of fields (should be DeclList)
} DeclStruct;

typedef struct {
    Id*         name;           // Function name
    DeclList*   params;         // Parameters (linked list or array)
    Type*       return_type;    // Changed to Type* to support array types
    StmtList*   body;           // Function body
    bool        is_extern;      // rue for “extern func”
} DeclFunction;

typedef struct {
    Id *module_name;   // contains "foo.bar"
} DeclImport;

typedef struct Decl {
    DeclKind kind;
    union {
        DeclVariable    variable_decl;
        DeclStruct      struct_decl;
        DeclEnum        enum_decl;
        DeclFunction    function_decl;
        DeclImport      import_decl;
        DeclDestruct    destruct_decl;
    } as;
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
    STMT_MATCH_CASE,
    STMT_MATCH,
    STMT_USE,
    STMT_RETURN,
} StmtKind;

typedef struct {
    Id*         name;     // Variable declaration
    Type*       type;     // NULL if no annotation
    Expr*       expr;     // NULL if no init
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
    StmtList *then_branch;  // TODO: rename then_body ?
    StmtList *else_branch;  // possibly NULL, or a single-item list if it’s an “else if”
  } StmtIf;

typedef struct {
    Id       *index_name;  // may be NULL if you wrote `for c in …`
    Id       *value_name;  // always non‐NULL
    Expr     *iterable;
    StmtList *body;
} StmtFor;

typedef struct StmtMatchCase {
    Expr            *pattern;         // NULL for `else`
    StmtList        *body;
    struct StmtMatchCase   *next;
} StmtMatchCase;

typedef struct {
    Expr *value;
    StmtMatchCase *cases;
} StmtMatch;

typedef struct {
    Expr *value;    // the expression to return
} StmtReturn;

typedef struct Stmt {
    StmtKind kind;
    union {
        StmtVar         var_stmt;
        StmtAssign      assign_stmt;
        StmtExpr        expr_stmt;
        StmtIf          if_stmt;
        StmtFor         for_stmt;
        StmtUse         use_stmt;
        StmtMatch       match_stmt;
        StmtReturn      return_stmt;
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
} ExprKind;

typedef struct {
    Expr*       left;   // Left operand
    TokenKind   op;     // Operator
    Expr*       right;  // Right operand
} ExprBinary;

typedef struct {
    TokenKind   op;     // Unary operator
    Expr*       right;  // Right operand
} ExprUnary;

typedef struct {
    Expr*       expr;   // The expression being moved
} ExprMove;

typedef struct {
    Expr*       expr;
} ExprMut; // New


typedef struct {
    Id*         id;   // Identifier name
} ExprIdentifier;

typedef struct {
    int         value;  // Literal value (TODO: extend to support other types)
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
    Expr *target;   // e.g. the `text`
    Expr *index;    // e.g. the `start..cursor` (an ExprRange)
} ExprIndex;

typedef struct Expr {
    ExprKind kind;
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

// Simple names: no element_type, no sentinel
Type *type_simple(Arena *arena, Id *base) {
    Type *t = arena_push_aligned(arena, Type);
    t->kind      = TYPE_SIMPLE;
    t->mode      = MODE_SHARED;  // default ownership
    t->base_type = base;
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
    return t;
}

Type *type_pointer(Arena *arena, Type *element_type) {
    assert(element_type != NULL);
    Type *t = arena_push_aligned(arena, Type);
    t->kind         = TYPE_POINTER;
    t->mode         = MODE_SHARED;  // pointers default to shared
    t->element_type = element_type;
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
    d->kind = DECL_VARIABLE;
    d->as.variable_decl.name = name;
    d->as.variable_decl.type = type;
    d->as.variable_decl.in_field = NULL; // default: no "in" annotation
    d->as.variable_decl.is_parameter = false;
    return d;
}

Decl *decl_function(Arena *arena, Id *name, DeclList *params, Type *return_type, StmtList *body, bool is_extern) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->kind = is_extern ? DECL_EXTERN_FUNCTION : DECL_FUNCTION;
    d->as.function_decl.name = name;
    d->as.function_decl.params = params;
    d->as.function_decl.return_type = return_type;
    d->as.function_decl.body = body;
    d->as.function_decl.is_extern   = is_extern;
    return d;
}

Decl *decl_procedure(Arena *arena, Id *name, DeclList *params, Type *return_type, StmtList *body, bool is_extern) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->kind = is_extern ? DECL_EXTERN_PROCEDURE : DECL_PROCEDURE;
    d->as.function_decl.name = name;
    d->as.function_decl.params = params;
    d->as.function_decl.return_type = return_type;
    d->as.function_decl.body = body;
    d->as.function_decl.is_extern   = is_extern;
    return d;
}

Decl* decl_struct(Arena* arena, Id* name, DeclList* fields) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->kind = DECL_STRUCT;  // FIXED: Use correct enum value
    d->as.struct_decl.name = name;  // FIXED: Correct member
    d->as.struct_decl.fields = fields;  // FIXED: Correct member
    return d;
}

Decl *decl_enum(Arena *arena, Id *type_name, IdList *variants) {
    Decl *d = arena_push_aligned(arena, Decl);
    d->kind = DECL_ENUM;
    d->as.enum_decl.type_name = type_name;
    d->as.enum_decl.variants = variants;
    return d;
}

Decl* decl_import(Arena* arena, Id* module_name) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->kind = DECL_IMPORT;
    return d;
}

Decl* decl_destruct(Arena* arena, IdList* names, Type* type) {
    Decl* d = arena_push_aligned(arena, Decl);
    d->kind = DECL_DESTRUCT;
    d->as.destruct_decl.names = names;
    d->as.destruct_decl.type = type;
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
    s->as.if_stmt.then_branch = then_branch;
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

Stmt *stmt_continue(Arena *arena) {
    Stmt *s = arena_push_aligned(arena, Stmt);
    s->kind = STMT_CONTINUE;
    return s;
}

StmtMatchCase *stmt_match_case(Arena *arena, Expr *pattern, StmtList *body) {
    StmtMatchCase *s = arena_push(arena, StmtMatchCase);
    s->pattern = pattern;    // NULL means `else`
    s->body    = body;
    s->next    = NULL;
    return s;
}


Stmt *stmt_match(Arena *arena, Expr *value, StmtMatchCase *cases) {
    Stmt *s = arena_push(arena, Stmt);
    s->kind = STMT_MATCH;
    s->as.match_stmt.value = value;
    s->as.match_stmt.cases = cases;
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

Expr *expr_literal(Arena *arena, int value) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_LITERAL;
    e->as.literal_expr.value = value;
    return e;
}

Expr *expr_member(Arena *arena, Expr *target, Id *member) {
    Expr *e = arena_push_aligned(arena, Expr);
    e->kind = EXPR_MEMBER;
    e->as.member_expr.target = target;
    e->as.member_expr.member = member;
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


#endif /* AST_H */