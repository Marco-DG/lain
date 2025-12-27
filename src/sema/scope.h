#ifndef SEMA_SCOPE_H
#define SEMA_SCOPE_H

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../ast.h"

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
static void sema_insert_global(const char *raw, const char *cname, Type *ty, Decl *decl) {
    unsigned idx = sema_hash(raw);
    Symbol *sym = malloc(sizeof *sym);
    sym->name   = strdup(raw);
    sym->c_name = strdup(cname);
    sym->type   = ty;
    sym->decl   = decl;
    sym->is_global = true;
    // Globals are mutable by default? Or immutable?
    // In Lain, globals are usually 'var' or 'const'. 
    // If decl is DECL_VARIABLE, we can check.
    // But for now let's assume globals are mutable if they are variables.
    sym->is_mutable = true; 
    sym->next   = sema_globals[idx];
    sema_globals[idx] = sym;
}

// ── insert into the local symbol‐table ───────────────────────────────────────
static void sema_insert_local(const char *raw, const char *cname, Type *ty, Decl *decl, bool is_mutable) {
    unsigned idx = sema_hash(raw);
    Symbol *sym = malloc(sizeof *sym);
    sym->name   = strdup(raw);
    sym->c_name = strdup(cname);
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
    for (int i = 0; i < SEMA_BUCKET_COUNT; i++) {
        Symbol *sym = sema_globals[i];
        while (sym) {
            Symbol *next = sym->next;
            free(sym->name);
            free(sym->c_name);
            free(sym);
            sym = next;
        }
        sema_globals[i] = NULL;
    }
}

// ── clear out only the local table (call this at function‐entry/function‐exit) ─
static void sema_clear_locals(void) {
    for (int i = 0; i < SEMA_BUCKET_COUNT; i++) {
        Symbol *sym = sema_locals[i];
        while (sym) {
            Symbol *next = sym->next;
            free(sym->name);
            free(sym->c_name);
            free(sym);
            sym = next;
        }
        sema_locals[i] = NULL;
    }
}

#endif // SEMA_SCOPE_H
