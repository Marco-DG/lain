#ifndef SEMA_SCOPE_H
#define SEMA_SCOPE_H

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../ast.h"

extern Arena *sema_arena;

// Helper: strdup using arena (no free needed)
static char *arena_strdup(Arena *a, const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = arena_push_many_aligned(a, char, len);
    memcpy(copy, s, len);
    return copy;
}

// A builtin type name (iN/uN for N∈[1,64], plus the named scalars/aliases) is a
// first-class type-VALUE in Lain (`Vec(i32)`, `max(i32, …)`) and is resolved by
// the identifier-fallback in resolve.h, never entered in the symbol table. Binding
// such a name as a variable/parameter therefore cannot shadow the type: any later
// read of the name resolves back to the type-value and emits broken C
// ("unhandled expression type"). Reject the binding cleanly instead — like `end`,
// a reserved type name is not a legal identifier to bind.
static bool is_reserved_type_name(const char *raw) {
    size_t rl = strlen(raw);
    if (rl >= 2 && rl <= 3 && (raw[0] == 'i' || raw[0] == 'u')) {
        int bits = 0; bool all_digits = true;
        for (size_t k = 1; k < rl; k++) {
            if (raw[k] < '0' || raw[k] > '9') { all_digits = false; break; }
            bits = bits * 10 + (raw[k] - '0');
        }
        if (all_digits && bits >= 1 && bits <= 64) return true;
    }
    // Exactly the names the resolve.h identifier-fallback rewrites to a type-value
    // (empirically the set that miscompiles when bound and later read). usize/isize/
    // void/char are NOT rewritten there — they bind and read correctly — so binding
    // them stays legal; `type` is already a reserved keyword rejected at parse time.
    static const char *names[] = {
        "int", "float", "bool", "string", "f32", "f64", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strcmp(raw, names[i]) == 0) return true;
    return false;
}

/*
  A two‐table symbol‐scope implementation:

   • sema_globals[]: holds every top‐level symbol (enums, structs, functions, globals).
   • sema_locals[]:  a fresh table for each function; holds that function’s params + var‐decls.

  Lookup always checks sema_locals first, then sema_globals.
  sema_clear_locals() is called at function‐entry and function‐exit.
  sema_clear_globals() is called once at program startup (or module‐reload).
*/

#define SEMA_BUCKET_COUNT 4096

typedef struct Symbol {
    char      *name;    // raw identifier, e.g. "lexeme"
    char      *c_name;  // mangled C identifier, e.g. "main_match_keyword_lexeme"
    Type      *type;    // AST’s Type* for this symbol (NULL if not yet known)
    Decl      *decl;    // The declaration (NULL for locals defined via STMT_VAR)
    bool       is_global; // True if defined in global scope
    bool       is_mutable; // True if mutable (var)
    struct Symbol *next;
} Symbol;

// ── the two tables ───────────────────────────────────────────────────────────
static Symbol *sema_globals[SEMA_BUCKET_COUNT];
static Symbol *sema_locals [SEMA_BUCKET_COUNT];

// ── hash function (unchanged) ───────────────────────────────────────────────
static unsigned sema_hash(const char *s) {
    unsigned long h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s++;
    }
    return (unsigned)(h % SEMA_BUCKET_COUNT);
}

// ── insert into the global symbol‐table ──────────────────────────────────────
static void sema_insert_global(const char *raw, const char *cname, Type *ty, Decl *decl, bool is_mutable) {
    unsigned idx = sema_hash(raw);
    Symbol *sym = arena_push_aligned(sema_arena, Symbol);
    sym->name   = arena_strdup(sema_arena, raw);
    sym->c_name = arena_strdup(sema_arena, cname);
    sym->type   = ty;
    sym->decl   = decl;
    sym->is_global = true;
    sym->is_mutable = is_mutable;
    sym->next   = sema_globals[idx];
    sema_globals[idx] = sym;
}

// ── insert into the local symbol‐table ───────────────────────────────────────
static void sema_insert_local(const char *raw, const char *cname, Type *ty, Decl *decl, bool is_mutable) {
    unsigned idx = sema_hash(raw);
    Symbol *sym = arena_push_aligned(sema_arena, Symbol);
    sym->name   = arena_strdup(sema_arena, raw);
    sym->c_name = arena_strdup(sema_arena, cname);
    sym->type   = ty;
    sym->decl   = decl; 
    sym->is_global = false;
    sym->is_mutable = is_mutable;
    sym->next   = sema_locals[idx];
    sema_locals[idx] = sym;
}

// ── lookup (locals first → globals) ─────────────────────────────────────────
static Symbol *sema_lookup(const char *raw) {
    unsigned idx = sema_hash(raw);
    // 1) check locals
    for (Symbol *sym = sema_locals[idx]; sym; sym = sym->next) {
        if (strcmp(sym->name, raw) == 0) {
            return sym;
        }
    }
    // 2) fallback to globals
    for (Symbol *sym = sema_globals[idx]; sym; sym = sym->next) {
        if (strcmp(sym->name, raw) == 0) {
            return sym;
        }
    }
    return NULL;
}

// ── clear out only the global table (call this once at program start) ───────
static void sema_clear_globals(void) {
    // Arena handles deallocation — just reset bucket pointers
    for (int i = 0; i < SEMA_BUCKET_COUNT; i++) {
        sema_globals[i] = NULL;
    }
}

// ── clear out only the local table (call this at function‐entry/function‐exit) ─
static void sema_clear_locals(void) {
    // Arena handles deallocation — just reset bucket pointers
    for (int i = 0; i < SEMA_BUCKET_COUNT; i++) {
        sema_locals[i] = NULL;
    }
}

// ── block scoping via push/pop ──────────────────────────────────────────────
// ScopeFrame saves the head pointers of every bucket in sema_locals.
// When popped, all symbols added since the push are effectively removed.

typedef struct ScopeFrame {
    Symbol *saved_locals[SEMA_BUCKET_COUNT];
    struct ScopeFrame *parent;
} ScopeFrame;

static ScopeFrame *scope_stack = NULL;

static void sema_push_scope(void) {
    // F-017: allocate on the sema arena (freed in bulk at end of compilation).
    // The comment that used to say "allocate on the arena" now matches reality.
    ScopeFrame *frame = arena_push_aligned(sema_arena, ScopeFrame);
    memcpy(frame->saved_locals, sema_locals, sizeof(sema_locals));
    frame->parent = scope_stack;
    scope_stack = frame;
}

static void sema_pop_scope(void) {
    if (!scope_stack) return;
    ScopeFrame *frame = scope_stack;
    memcpy(sema_locals, frame->saved_locals, sizeof(sema_locals));
    scope_stack = frame->parent;
    // No free: arena owns the memory.
}

#endif // SEMA_SCOPE_H
