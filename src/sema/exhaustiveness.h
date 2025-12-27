#ifndef SEMA_EXHAUSTIVENESS_H
#define SEMA_EXHAUSTIVENESS_H

/*
 * Exhaustiveness Checking for Lain Match Statements
 * 
 * Ensures that match statements cover all possible cases.
 * Supports:
 * - Enum types: all variants must be covered (or have else:)
 * - Bool types: true and false must be covered (or have else:)
 * - Integer types: must have else: (infinite domain)
 */

#include "../ast.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

// Debug flag
#ifndef SEMA_EXHAUSTIVENESS_DEBUG
#define SEMA_EXHAUSTIVENESS_DEBUG 0
#endif

#if SEMA_EXHAUSTIVENESS_DEBUG
#define EXHAUST_DBG(fmt, ...) fprintf(stderr, "[exhaust] " fmt "\n", ##__VA_ARGS__)
#else
#define EXHAUST_DBG(fmt, ...) do {} while(0)
#endif

// Forward declaration for symbol lookup (from resolve.h)
extern DeclList *sema_decls;

/*───────────────────────────────────────────────────────────────────╗
│ Helper Functions                                                    │
╚───────────────────────────────────────────────────────────────────*/

// Check if a match statement has an else case (catch-all)
static bool match_has_else_case(StmtMatchCase *cases) {
    for (StmtMatchCase *c = cases; c; c = c->next) {
        if (c->pattern == NULL) {
            EXHAUST_DBG("found else case");
            return true;
        }
    }
    return false;
}

// Find enum declaration by type name
static Decl *find_enum_decl(Type *vtype) {
    if (!vtype || vtype->kind != TYPE_SIMPLE || !vtype->base_type) {
        return NULL;
    }
    
    const char *type_name = vtype->base_type->name;
    int type_len = vtype->base_type->length;
    
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        if (!dl->decl || dl->decl->kind != DECL_ENUM) continue;
        
        Id *enum_name = dl->decl->as.enum_decl.type_name;
        if (!enum_name) continue;

        // 1. Exact match
        if (enum_name->length == type_len &&
            strncmp(enum_name->name, type_name, type_len) == 0) {
            return dl->decl;
        }

        // 2. Suffix match (handle mangled names like module_Enum)
        if (type_len > enum_name->length + 1) {
            const char *suffix_start = type_name + (type_len - enum_name->length);
            if (*(suffix_start - 1) == '_' &&
                strncmp(suffix_start, enum_name->name, enum_name->length) == 0) {
                return dl->decl;
            }
        }
    }
    return NULL;
}

// Check if a pattern matches an enum variant by name
// Handles mangled names like "module_Type_Variant" matching variant "Variant"
static bool pattern_matches_variant(Expr *pattern, Id *variant) {
    if (!pattern || !variant) return false;
    
    // Pattern should be an identifier or a call (constructor)
    Id *pat_id = NULL;
    
    if (pattern->kind == EXPR_IDENTIFIER) {
        pat_id = pattern->as.identifier_expr.id;
    } else if (pattern->kind == EXPR_CALL) {
        // Constructor pattern: Variant(...)
        // The callee should be the variant name
        Expr *callee = pattern->as.call_expr.callee;
        if (callee->kind == EXPR_IDENTIFIER) {
            pat_id = callee->as.identifier_expr.id;
        } else if (callee->kind == EXPR_MEMBER) {
             // Handle Shape.Circle(...)
             pat_id = callee->as.member_expr.member;
        }
    }
    
    if (!pat_id || !pat_id->name || !variant->name) return false;
    
    // First try exact match
    if (pat_id->length == variant->length &&
        strncmp(pat_id->name, variant->name, variant->length) == 0) {
        return true;
    }
    
    // Try suffix match: pattern ends with "_Variant"
    // e.g., "tests_enums_Color_Red" ends with "_Red"
    if (pat_id->length > variant->length + 1) {
        const char *suffix_start = pat_id->name + (pat_id->length - variant->length);
        // Check if character before suffix is '_'
        if (*(suffix_start - 1) == '_' &&
            strncmp(suffix_start, variant->name, variant->length) == 0) {
            return true;
        }
    }
    
    return false;
}

// Check if all enum variants are covered
static bool match_check_enum_exhaustiveness(Decl *enum_decl, StmtMatchCase *cases) {
    if (!enum_decl) return false;
    
    Variant *variants = enum_decl->as.enum_decl.variants;
    
    // For each variant, check if there's a matching case
    for (Variant *v = variants; v; v = v->next) {
        if (!v->name) continue;
        
        EXHAUST_DBG("checking variant '%.*s'", (int)v->name->length, v->name->name);
        
        bool variant_covered = false;
        for (StmtMatchCase *c = cases; c; c = c->next) {
            if (c->pattern == NULL) {
                // else case covers everything
                variant_covered = true;
                break;
            }
            
            if (pattern_matches_variant(c->pattern, v->name)) {
                variant_covered = true;
                break;
            }
        }
        
        if (!variant_covered) {
            EXHAUST_DBG("enum variant '%.*s' not covered", 
                       (int)v->name->length, v->name->name);
            return false;
        }
    }
    
    EXHAUST_DBG("all enum variants covered");
    return true;
}

// Check if a match on a boolean covers both true and false
static bool match_check_bool_exhaustiveness(StmtMatchCase *cases) {
    bool has_true = false;
    bool has_false = false;
    
    for (StmtMatchCase *c = cases; c; c = c->next) {
        if (c->pattern == NULL) {
            return true;  // else covers everything
        }
        // Check if pattern is a literal true/false
        if (c->pattern->kind == EXPR_LITERAL) {
            if (c->pattern->as.literal_expr.value != 0) {
                has_true = true;
            } else {
                has_false = true;
            }
        }
    }
    
    return has_true && has_false;
}

/*───────────────────────────────────────────────────────────────────╗
│ Main Exhaustiveness Check                                          │
╚───────────────────────────────────────────────────────────────────*/

// Main exhaustiveness check function
// Returns true if the match is exhaustive, false otherwise
static bool sema_check_match_exhaustive(Stmt *match_stmt) {
    if (!match_stmt || match_stmt->kind != STMT_MATCH) {
        return true;  // Not a match, nothing to check
    }
    
    Expr *value = match_stmt->as.match_stmt.value;
    StmtMatchCase *cases = match_stmt->as.match_stmt.cases;
    
    if (!cases) {
        EXHAUST_DBG("match has no cases!");
        return false;  // Empty match is not exhaustive
    }
    
    // Quick check: if there's an else case, always exhaustive
    if (match_has_else_case(cases)) {
        EXHAUST_DBG("match is exhaustive (has else)");
        return true;
    }
    
    // Check based on the type of the matched value
    Type *vtype = value ? value->type : NULL;
    
    if (vtype && vtype->kind == TYPE_SIMPLE && vtype->base_type) {
        const char *type_name = vtype->base_type->name;
        int type_len = vtype->base_type->length;
        
        // Check for bool
        if (type_len == 4 && strncmp(type_name, "bool", 4) == 0) {
            if (match_check_bool_exhaustiveness(cases)) {
                EXHAUST_DBG("match on bool is exhaustive");
                return true;
            }
        }
        
        // Check for enum type
        Decl *enum_decl = find_enum_decl(vtype);
        if (enum_decl) {
            if (match_check_enum_exhaustiveness(enum_decl, cases)) {
                return true;
            }
            // Fall through to error - enum not fully covered
        }
    }
    
    // For integer/other types without else, not exhaustive
    EXHAUST_DBG("match is NOT exhaustive (no else, not complete coverage)");
    return false;
}

// Report non-exhaustive match error
static void sema_report_nonexhaustive_match(Stmt *match_stmt) {
    (void)match_stmt;
    fprintf(stderr, "sema error: non-exhaustive match - add an 'else:' case or cover all variants\n");
}

#endif /* SEMA_EXHAUSTIVENESS_H */

