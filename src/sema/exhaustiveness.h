#ifndef SEMA_EXHAUSTIVENESS_H
#define SEMA_EXHAUSTIVENESS_H

/*
 * Exhaustiveness Checking for Lain Match Statements
 * 
 * Ensures that match statements cover all possible cases.
 * For now, requires either:
 * - An `else:` case (catch-all)
 * - OR explicit coverage of all enum variants (when matching on enum)
 */

#include "../ast.h"
#include <stdio.h>
#include <stdbool.h>

// Debug flag
#ifndef SEMA_EXHAUSTIVENESS_DEBUG
#define SEMA_EXHAUSTIVENESS_DEBUG 0
#endif

#if SEMA_EXHAUSTIVENESS_DEBUG
#define EXHAUST_DBG(fmt, ...) fprintf(stderr, "[exhaust] " fmt "\n", ##__VA_ARGS__)
#else
#define EXHAUST_DBG(fmt, ...) do {} while(0)
#endif

/*───────────────────────────────────────────────────────────────────╗
│ Match Exhaustiveness Check                                         │
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

// Count the number of cases (excluding else)
static int match_count_cases(StmtMatchCase *cases) {
    int count = 0;
    for (StmtMatchCase *c = cases; c; c = c->next) {
        if (c->pattern != NULL) {
            count++;
        }
    }
    return count;
}

// Check if a match on integer literals covers common patterns
// Returns true if the match appears exhaustive for integer type
static bool match_check_int_exhaustiveness(Type *value_type, StmtMatchCase *cases) {
    (void)value_type;
    (void)cases;
    // For integer types, we can't prove exhaustiveness without else
    // (infinite domain)
    return match_has_else_case(cases);
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
    
    if (vtype) {
        // For bool type, check if both true and false are covered
        if (vtype->kind == TYPE_SIMPLE && vtype->base_type) {
            const char *type_name = vtype->base_type->name;
            int type_len = vtype->base_type->length;
            
            if (type_len == 4 && strncmp(type_name, "bool", 4) == 0) {
                if (match_check_bool_exhaustiveness(cases)) {
                    EXHAUST_DBG("match on bool is exhaustive");
                    return true;
                }
            }
        }
        
        // TODO: For enum types, check all variants are covered
        // For now, require else for non-bool types
    }
    
    // For integer/other types without else, not exhaustive
    EXHAUST_DBG("match is NOT exhaustive (no else, not complete enum)");
    return false;
}

// Report non-exhaustive match error
static void sema_report_nonexhaustive_match(Stmt *match_stmt) {
    (void)match_stmt;
    fprintf(stderr, "sema error: non-exhaustive match - add an 'else:' case or cover all variants\n");
}

#endif /* SEMA_EXHAUSTIVENESS_H */
