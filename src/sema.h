#ifndef SEMA_H
#define SEMA_H

// Diagnostic globals (defined before sub-header includes so they can call diagnostic_show_line)
const char *sema_source_text = NULL;
const char *sema_source_file = NULL;

static void diagnostic_show_line(isize line, isize col) {
    if (!sema_source_text || !sema_source_file) return;
    if (line <= 0) return;

    const char *p = sema_source_text;
    isize cur_line = 1;
    while (*p && cur_line < line) {
        if (*p == '\n') cur_line++;
        p++;
    }
    if (!*p && cur_line < line) return;

    const char *line_start = p;
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;

    int line_len = (int)(line_end - line_start);
    int line_num_width = 1;
    { isize tmp = line; while (tmp >= 10) { line_num_width++; tmp /= 10; } }

    fprintf(stderr, "  --> %s:%li:%li\n", sema_source_file, (long)line, (long)col);
    fprintf(stderr, " %*s |\n", line_num_width, "");
    fprintf(stderr, " %li | %.*s\n", (long)line, line_len, line_start);
    fprintf(stderr, " %*s | ", line_num_width, "");
    isize caret_pos = (col > 0) ? col - 1 : 0;
    for (isize i = 0; i < caret_pos; i++) {
        if (i < line_len && line_start[i] == '\t')
            fputc('\t', stderr);
        else
            fputc(' ', stderr);
    }
    fprintf(stderr, "^\n");
}

// In-guard table (declared before sub-header includes so typecheck.h can use them)
typedef struct InGuardEntry {
    Expr *index;
    Expr *container;
    struct InGuardEntry *next;
} InGuardEntry;

static InGuardEntry *sema_in_guards = NULL;

static void sema_push_in_guards(Expr *cond);
static bool sema_is_in_guarded(Expr *index, Expr *container);

#include "sema/scope.h"
#include "sema/resolve.h"
#include "sema/typecheck.h"
#include "sema/linearity.h"

Type *current_return_type = NULL;
Decl *current_function_decl = NULL;
const char *current_module_path = NULL;
DeclList *sema_decls = NULL;
Arena *sema_arena = NULL;
RangeTable *sema_ranges = NULL;
bool sema_in_unsafe_block = false;
bool sema_walk_phase = false;

/*─────────────────────────────────────────────────────────────────╗
│ Public entry: call this before emit                             │
╚─────────────────────────────────────────────────────────────────*/

// Helper to widen variables modified in a loop to unknown
static void sema_widen_loop(StmtList *body, RangeTable *t) {
    for (StmtList *l = body; l; l = l->next) {
        Stmt *s = l->stmt;
        if (!s) continue;
        switch (s->kind) {
            case STMT_ASSIGN:
                if (s->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
                     range_set(t, s->as.assign_stmt.target->as.identifier_expr.id, range_unknown());
                }
                break;
            case STMT_IF:
                sema_widen_loop(s->as.if_stmt.then_branch, t);
                sema_widen_loop(s->as.if_stmt.else_branch, t);
                break;
            case STMT_FOR:
                sema_widen_loop(s->as.for_stmt.body, t);
                break;
            case STMT_WHILE:
                sema_widen_loop(s->as.while_stmt.body, t);
                break;
            case STMT_MATCH:
                 for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                     sema_widen_loop(c->body, t);
                 }
                 break;
            default: break;
        }
    }
}

/*─────────────────────────────────────────────────────────────────────────────╗
│ Bounded-while termination verification                                       │
│ Self-contained: does NOT use VRA range table (which can't track struct fields)│
╚─────────────────────────────────────────────────────────────────────────────*/

// Structural equality for expressions (IDENTIFIER, MEMBER, LITERAL)
static bool expr_struct_equal(Expr *a, Expr *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case EXPR_IDENTIFIER:
            return a->as.identifier_expr.id->length == b->as.identifier_expr.id->length &&
                   strncmp(a->as.identifier_expr.id->name,
                           b->as.identifier_expr.id->name,
                           a->as.identifier_expr.id->length) == 0;
        case EXPR_MEMBER:
            return expr_struct_equal(a->as.member_expr.target, b->as.member_expr.target) &&
                   a->as.member_expr.member->length == b->as.member_expr.member->length &&
                   strncmp(a->as.member_expr.member->name,
                           b->as.member_expr.member->name,
                           a->as.member_expr.member->length) == 0;
        case EXPR_LITERAL:
            return a->as.literal_expr.value == b->as.literal_expr.value;
        default:
            return false;
    }
}

// --- Non-negativity: condition implies measure >= 0 ---

// Extract a comparison (a < b, a > b, etc.) from a possibly conjunctive condition.
// Tries both sides of `and`. Returns true if a relevant comparison was found.
typedef struct { Expr *lo; Expr *hi; bool strict; } MeasureCmp;

static bool measure_extract_cmp(Expr *cond, Expr *measure, MeasureCmp *out) {
    if (!cond) return false;
    if (cond->kind == EXPR_BINARY) {
        TokenKind op = cond->as.binary_expr.op;
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;

        // Handle `and` conjunctions — try both sides
        if (op == TOKEN_KEYWORD_AND) {
            if (measure_extract_cmp(lhs, measure, out)) return true;
            if (measure_extract_cmp(rhs, measure, out)) return true;
            return false;
        }

        // Normalize to lo < hi or lo <= hi
        switch (op) {
            case TOKEN_ANGLE_BRACKET_LEFT:         // a < b
                out->lo = lhs; out->hi = rhs; out->strict = true; return true;
            case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:   // a <= b
                out->lo = lhs; out->hi = rhs; out->strict = false; return true;
            case TOKEN_ANGLE_BRACKET_RIGHT:        // a > b => b < a
                out->lo = rhs; out->hi = lhs; out->strict = true; return true;
            case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:  // a >= b => b <= a
                out->lo = rhs; out->hi = lhs; out->strict = false; return true;
            default: break;
        }
    }
    return false;
}

static bool sema_verify_measure_nonneg(Expr *cond, Expr *measure) {
    // Handle 'and' conjunctions — try both sides
    if (cond && cond->kind == EXPR_BINARY && cond->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        if (sema_verify_measure_nonneg(cond->as.binary_expr.left, measure)) return true;
        if (sema_verify_measure_nonneg(cond->as.binary_expr.right, measure)) return true;
    }

    // Handle 'in' condition: idx in arr, measure = arr.len - idx
    // idx in arr => idx < arr.len => arr.len - idx >= 1 > 0
    if (cond && cond->kind == EXPR_BINARY && cond->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        Expr *idx = cond->as.binary_expr.left;
        Expr *arr = cond->as.binary_expr.right;
        if (measure->kind == EXPR_BINARY && measure->as.binary_expr.op == TOKEN_MINUS) {
            Expr *m_hi = measure->as.binary_expr.left;
            Expr *m_lo = measure->as.binary_expr.right;
            if (expr_struct_equal(m_lo, idx) &&
                m_hi->kind == EXPR_MEMBER &&
                m_hi->as.member_expr.member->length == 3 &&
                strncmp(m_hi->as.member_expr.member->name, "len", 3) == 0 &&
                expr_struct_equal(m_hi->as.member_expr.target, arr)) {
                return true;
            }
        }
    }

    MeasureCmp cmp;
    if (!measure_extract_cmp(cond, measure, &cmp)) return false;

    // Pattern: measure == hi - lo  (condition gives lo < hi => hi - lo > 0)
    if (measure->kind == EXPR_BINARY && measure->as.binary_expr.op == TOKEN_MINUS) {
        if (expr_struct_equal(measure->as.binary_expr.left, cmp.hi) &&
            expr_struct_equal(measure->as.binary_expr.right, cmp.lo)) {
            return true;
        }
    }

    // Pattern: measure is a single variable/expr that equals hi, and lo is literal >= 0
    // e.g.  while n > 0 : n   =>  lo=0, hi=n, strict, measure=n
    if (expr_struct_equal(measure, cmp.hi) && cmp.strict) {
        if (cmp.lo->kind == EXPR_LITERAL && cmp.lo->as.literal_expr.value >= 0) return true;
    }
    if (expr_struct_equal(measure, cmp.hi) && !cmp.strict) {
        if (cmp.lo->kind == EXPR_LITERAL && cmp.lo->as.literal_expr.value >= 0) return true;
    }

    return false;
}

// --- Strict decrease: body assignments decrease the measure ---

typedef struct { Expr *var; int polarity; } MeasureVar;
#define MAX_MEASURE_VARS 8

// Extract variables and their polarities from a measure expression.
// b - a  =>  b(+1), a(-1).     x  =>  x(+1).
static int measure_extract_vars(Expr *m, MeasureVar *out, int max) {
    if (!m || max <= 0) return 0;

    if (m->kind == EXPR_IDENTIFIER || m->kind == EXPR_MEMBER) {
        out[0].var = m;
        out[0].polarity = +1;
        return 1;
    }

    if (m->kind == EXPR_BINARY) {
        if (m->as.binary_expr.op == TOKEN_MINUS) {
            int n = measure_extract_vars(m->as.binary_expr.left, out, max);
            int old_n = n;
            n += measure_extract_vars(m->as.binary_expr.right, out + n, max - n);
            for (int i = old_n; i < n; i++) out[i].polarity *= -1;
            return n;
        }
        if (m->as.binary_expr.op == TOKEN_PLUS) {
            int n = measure_extract_vars(m->as.binary_expr.left, out, max);
            n += measure_extract_vars(m->as.binary_expr.right, out + n, max - n);
            return n;
        }
    }

    return 0;
}

static int measure_find_var(Expr *target, MeasureVar *vars, int nvar) {
    for (int i = 0; i < nvar; i++) {
        if (expr_struct_equal(target, vars[i].var)) return i;
    }
    return -1;
}

// Determine direction: does `target = rhs` increase (+1) or decrease (-1) target?
// Only handles: target = target + K, target = target - K, K + target  (K literal > 0)
static int assignment_direction(Expr *target, Expr *rhs) {
    if (!rhs || rhs->kind != EXPR_BINARY) return 0;

    TokenKind op  = rhs->as.binary_expr.op;
    Expr *left    = rhs->as.binary_expr.left;
    Expr *right   = rhs->as.binary_expr.right;

    // target = target + K  or  target = target - K
    if (expr_struct_equal(left, target) && right->kind == EXPR_LITERAL) {
        int64_t k = right->as.literal_expr.value;
        if (op == TOKEN_PLUS  && k > 0) return +1;
        if (op == TOKEN_PLUS  && k < 0) return -1;
        if (op == TOKEN_MINUS && k > 0) return -1;
        if (op == TOKEN_MINUS && k < 0) return +1;
    }
    // target = K + target
    if (expr_struct_equal(right, target) && left->kind == EXPR_LITERAL && op == TOKEN_PLUS) {
        int64_t k = left->as.literal_expr.value;
        if (k > 0) return +1;
        if (k < 0) return -1;
    }

    return 0;
}

// Scan body for assignments to measure variables.
// Returns: +1 if at least one decreases & none conflict, -1 if any conflict, 0 if none found.
static int measure_scan_body(StmtList *body, MeasureVar *vars, int nvar) {
    bool found_decrease = false;
    for (StmtList *l = body; l; l = l->next) {
        Stmt *s = l->stmt;
        if (!s) continue;
        switch (s->kind) {
            case STMT_ASSIGN: {
                int idx = measure_find_var(s->as.assign_stmt.target, vars, nvar);
                if (idx >= 0) {
                    int dir = assignment_direction(s->as.assign_stmt.target, s->as.assign_stmt.expr);
                    if (dir == 0) return -1;  /* unknown change to measure var */
                    int effect = vars[idx].polarity * dir;
                    if (effect > 0) return -1; /* measure increases — conflict */
                    found_decrease = true;
                }
                break;
            }
            case STMT_IF: {
                int r = measure_scan_body(s->as.if_stmt.then_branch, vars, nvar);
                if (r < 0) return -1;
                if (r > 0) found_decrease = true;
                r = measure_scan_body(s->as.if_stmt.else_branch, vars, nvar);
                if (r < 0) return -1;
                if (r > 0) found_decrease = true;
                break;
            }
            case STMT_WHILE: {
                int r = measure_scan_body(s->as.while_stmt.body, vars, nvar);
                if (r < 0) return -1;
                if (r > 0) found_decrease = true;
                break;
            }
            case STMT_FOR: {
                int r = measure_scan_body(s->as.for_stmt.body, vars, nvar);
                if (r < 0) return -1;
                if (r > 0) found_decrease = true;
                break;
            }
            case STMT_MATCH: {
                for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                    int r = measure_scan_body(c->body, vars, nvar);
                    if (r < 0) return -1;
                    if (r > 0) found_decrease = true;
                }
                break;
            }
            case STMT_UNSAFE: {
                int r = measure_scan_body(s->as.unsafe_stmt.body, vars, nvar);
                if (r < 0) return -1;
                if (r > 0) found_decrease = true;
                break;
            }
            default: break;
        }
    }
    return found_decrease ? +1 : 0;
}

// Top-level verification for a bounded while loop
static void sema_verify_bounded_while(Stmt *s) {
    Expr *cond    = s->as.while_stmt.cond;
    Expr *measure = s->as.while_stmt.measure;
    if (!measure) return;

    // Check 1: condition implies measure >= 0
    if (!sema_verify_measure_nonneg(cond, measure)) {
        fprintf(stderr, "[E080] Error Ln %li, Col %li: cannot verify that the termination measure "
                "is non-negative when the loop condition holds.\n"
                "  Hint: use 'while a < b : b - a { ... }' so the condition implies the measure is positive.\n",
                s->line, s->col);
        diagnostic_show_line(s->line, s->col);
        exit(1);
    }

    // Check 2: body strictly decreases the measure
    MeasureVar vars[MAX_MEASURE_VARS];
    int nvar = measure_extract_vars(measure, vars, MAX_MEASURE_VARS);
    if (nvar == 0) {
        fprintf(stderr, "[E081] Error Ln %li, Col %li: cannot extract variables from termination measure.\n"
                "  Hint: the measure must reference identifiers or struct fields.\n",
                s->line, s->col);
        diagnostic_show_line(s->line, s->col);
        exit(1);
    }

    int result = measure_scan_body(s->as.while_stmt.body, vars, nvar);
    if (result <= 0) {
        fprintf(stderr, "[E082] Error Ln %li, Col %li: cannot verify that the termination measure "
                "strictly decreases on each iteration.\n"
                "  Hint: the loop body must contain an assignment that decreases the measure "
                "(e.g., 'pos += 1' when measure is 'size - pos').\n",
                s->line, s->col);
        diagnostic_show_line(s->line, s->col);
        exit(1);
    }
}

/*─────────────────────────────────────────────────────────────────────────────╗
│ In-guard table: function definitions (type + global declared before includes)│
╚─────────────────────────────────────────────────────────────────────────────*/

static void sema_push_in_guards(Expr *cond) {
    if (!cond || cond->kind != EXPR_BINARY) return;
    if (cond->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        InGuardEntry *e = arena_push_aligned(sema_arena, InGuardEntry);
        e->index = cond->as.binary_expr.left;
        e->container = cond->as.binary_expr.right;
        e->next = sema_in_guards;
        sema_in_guards = e;
    } else if (cond->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        sema_push_in_guards(cond->as.binary_expr.left);
        sema_push_in_guards(cond->as.binary_expr.right);
    }
}

static bool sema_is_in_guarded(Expr *index, Expr *container) {
    for (InGuardEntry *e = sema_in_guards; e; e = e->next) {
        if (expr_struct_equal(e->index, index) &&
            expr_struct_equal(e->container, container))
            return true;
    }
    return false;
}

/* walk_stmt: type inference + range analysis walk over a single statement.
   Formerly a GCC nested function inside sema_resolve_module; refactored to
   file-level static for C99/Clang/MSVC portability. */
static void walk_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR:
            sema_infer_expr(s->as.var_stmt.expr);
            // Infer variable type from initializer if missing
            if (!s->as.var_stmt.type && s->as.var_stmt.expr) {
                if (s->as.var_stmt.expr->kind == EXPR_UNDEFINED) {
                    fprintf(stderr, "Error: Cannot infer type for variable initialized with 'undefined'. Explicit type annotation required.\n");
                    exit(1);
                }
                s->as.var_stmt.type = s->as.var_stmt.expr->type;
            }

            if (sema_ranges && s->as.var_stmt.expr) {
                Range r = sema_eval_range(s->as.var_stmt.expr, sema_ranges);
                range_set(sema_ranges, s->as.var_stmt.name, r);
            }
            break;
        case STMT_IF: {
            sema_infer_expr(s->as.if_stmt.cond);

            // Save state
            RangeEntry *old_head = sema_ranges->head;
            ConstraintEntry *old_constraints = sema_ranges->constraints;
            InGuardEntry *old_guards = sema_in_guards;

            // Apply condition for THEN branch
            sema_apply_constraint(s->as.if_stmt.cond, sema_ranges);
            sema_push_in_guards(s->as.if_stmt.cond);

            sema_push_scope();
            for (StmtList *b = s->as.if_stmt.then_branch; b; b = b->next)
                walk_stmt(b->stmt);
            sema_pop_scope();

            // Restore state (pop constraints + in-guards from THEN)
            sema_ranges->head = old_head;
            sema_ranges->constraints = old_constraints;
            sema_in_guards = old_guards;

            // Apply negated condition for ELSE branch (no in-guards — negated 'in' proves nothing)
            sema_apply_negated_constraint(s->as.if_stmt.cond, sema_ranges);

            sema_push_scope();
            for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next)
                walk_stmt(b->stmt);
            sema_pop_scope();

            // Restore state again
            sema_ranges->head = old_head;
            sema_ranges->constraints = old_constraints;
            break;
        }
        case STMT_FOR: {
            sema_infer_expr(s->as.for_stmt.iterable);
            // Range Analysis: Loop index
            Range end_range = range_unknown();
            if (sema_ranges && s->as.for_stmt.iterable->kind == EXPR_RANGE && s->as.for_stmt.index_name) {
                Range start = sema_eval_range(s->as.for_stmt.iterable->as.range_expr.start, sema_ranges);
                end_range = sema_eval_range(s->as.for_stmt.iterable->as.range_expr.end, sema_ranges);
                if (start.known && end_range.known) {
                    Range r = range_make(start.min, end_range.max - 1);
                    range_set(sema_ranges, s->as.for_stmt.index_name, r);
                }
            }
            
            // Widen modified variables BEFORE body
            if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);
            
            sema_push_scope();
            for (StmtList *b = s->as.for_stmt.body; b; b = b->next)
                walk_stmt(b->stmt);
            sema_pop_scope();
                
            // Widen modified variables AFTER body
            if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);
            
            // Preserve loop index value at exit: for i in start..end → i == end after loop
            if (sema_ranges && s->as.for_stmt.index_name && end_range.known) {
                range_set(sema_ranges, s->as.for_stmt.index_name, end_range);
            }
            break;
        }
        case STMT_WHILE:
            sema_infer_expr(s->as.while_stmt.cond);
            if (s->as.while_stmt.measure) {
                sema_infer_expr(s->as.while_stmt.measure);
                sema_verify_bounded_while(s);
            }

            // Widen modified variables BEFORE body
            if (sema_ranges) sema_widen_loop(s->as.while_stmt.body, sema_ranges);

            {   // Apply condition constraints + in-guards for body
                RangeEntry *old_head = sema_ranges ? sema_ranges->head : NULL;
                ConstraintEntry *old_constraints = sema_ranges ? sema_ranges->constraints : NULL;
                InGuardEntry *old_guards = sema_in_guards;

                if (sema_ranges) sema_apply_constraint(s->as.while_stmt.cond, sema_ranges);
                sema_push_in_guards(s->as.while_stmt.cond);

                sema_push_scope();
                for (StmtList *b = s->as.while_stmt.body; b; b = b->next)
                    walk_stmt(b->stmt);
                sema_pop_scope();

                if (sema_ranges) {
                    sema_ranges->head = old_head;
                    sema_ranges->constraints = old_constraints;
                }
                sema_in_guards = old_guards;
            }

            // Widen modified variables AFTER body
            if (sema_ranges) sema_widen_loop(s->as.while_stmt.body, sema_ranges);
            break;
        case STMT_ASSIGN:
            sema_infer_expr(s->as.assign_stmt.expr);
            if (sema_ranges && s->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
                Expr *rhs = s->as.assign_stmt.expr;
                Id *lhs_id = s->as.assign_stmt.target->as.identifier_expr.id;
                
                // 1. Update Range
                Range r = sema_eval_range(rhs, sema_ranges);
                range_set(sema_ranges, lhs_id, r);
                
                // 2. Linear Constraints: x = y + c
                if (rhs->kind == EXPR_BINARY) {
                    TokenKind op = rhs->as.binary_expr.op;
                    Expr *rl = rhs->as.binary_expr.left;
                    Expr *rr = rhs->as.binary_expr.right;
                    
                    // x = y + c
                    if (op == TOKEN_PLUS && rl->kind == EXPR_IDENTIFIER && rr->kind == EXPR_LITERAL) {
                        int64_t c = rr->as.literal_expr.value;
                        constraint_add(sema_ranges, lhs_id, rl->as.identifier_expr.id, c);
                        constraint_add(sema_ranges, rl->as.identifier_expr.id, lhs_id, -c);
                    }
                    // x = c + y
                    else if (op == TOKEN_PLUS && rl->kind == EXPR_LITERAL && rr->kind == EXPR_IDENTIFIER) {
                        int64_t c = rl->as.literal_expr.value;
                        constraint_add(sema_ranges, lhs_id, rr->as.identifier_expr.id, c);
                        constraint_add(sema_ranges, rr->as.identifier_expr.id, lhs_id, -c);
                    }
                    // x = y - c
                    else if (op == TOKEN_MINUS && rl->kind == EXPR_IDENTIFIER && rr->kind == EXPR_LITERAL) {
                        int64_t c = rr->as.literal_expr.value;
                        constraint_add(sema_ranges, lhs_id, rl->as.identifier_expr.id, -c);
                        constraint_add(sema_ranges, rl->as.identifier_expr.id, lhs_id, c);
                    }
                }
                // x = y
                else if (rhs->kind == EXPR_IDENTIFIER) {
                    constraint_add(sema_ranges, lhs_id, rhs->as.identifier_expr.id, 0);
                    constraint_add(sema_ranges, rhs->as.identifier_expr.id, lhs_id, 0);
                }
            }
            break;
        case STMT_EXPR:
            sema_infer_expr(s->as.expr_stmt.expr);
            break;
        case STMT_RETURN:
            sema_infer_expr(s->as.return_stmt.value);
            // Check Post-Contracts
            if (current_function_decl && current_function_decl->as.function_decl.post_contracts) {
                Range ret_range = sema_eval_range(s->as.return_stmt.value, sema_ranges);
                
                for (ExprList *post = current_function_decl->as.function_decl.post_contracts; post; post = post->next) {
                    int result = sema_check_post_condition(post->expr, ret_range, sema_ranges);
                    
                    if (result == 0) {
                        fprintf(stderr, "Error: Post-condition violation. Return value cannot satisfy contract.\n");
                        exit(1);
                    }
                }
            }
            
            // Check equation-style return constraints: func f() int >= 0
            if (current_function_decl && current_function_decl->as.function_decl.return_constraints) {
                Range ret_range = sema_eval_range(s->as.return_stmt.value, sema_ranges);
                
                for (ExprList *rc = current_function_decl->as.function_decl.return_constraints; rc; rc = rc->next) {
                    int result = sema_check_post_condition(rc->expr, ret_range, sema_ranges);
                    
                    if (result == 0) {
                        fprintf(stderr, "Error: Return constraint violation. Return value does not satisfy type constraint.\n");
                        exit(1);
                    }
                }
            }
            break;
        case STMT_MATCH:
            sema_infer_expr(s->as.match_stmt.value);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                sema_push_scope();
                for (ExprList *p = c->patterns; p; p = p->next) {
                    sema_infer_expr(p->expr);
                }
                for (StmtList *b = c->body; b; b = b->next)
                    walk_stmt(b->stmt);
                sema_pop_scope();
            }
            break;
        case STMT_UNSAFE: {
            bool old_unsafe = sema_in_unsafe_block;
            sema_in_unsafe_block = true;
            sema_push_scope();
            for (StmtList *b = s->as.unsafe_stmt.body; b; b = b->next) {
                walk_stmt(b->stmt);
            }
            sema_pop_scope();
            sema_in_unsafe_block = old_unsafe;
            break;
        }
        case STMT_DEFER:
            walk_stmt(s->as.defer_stmt.stmt);
            break;
        case STMT_COMPTIME_IF: {
            // F-049: the taken branch must be walked so VRA/bounds run on it.
            // resolve_stmt evaluated the condition and marked is_taken.
            StmtList *branch = s->as.comptime_if_stmt.is_taken
                ? s->as.comptime_if_stmt.then_branch
                : s->as.comptime_if_stmt.else_branch;
            sema_push_scope();
            for (StmtList *b = branch; b; b = b->next) walk_stmt(b->stmt);
            sema_pop_scope();
            break;
        }
        default: break;
    }
}

/*─────────────────────────────────────────────────────────────────╗
│ F-020: mutual recursion detection among DECL_FUNCTION            │
╚─────────────────────────────────────────────────────────────────*/

// Walk expression; for every EXPR_CALL to a DECL_FUNCTION, call visit(callee_decl).
static void mrec_walk_expr(Expr *e, void (*visit)(Decl *));
static void mrec_walk_stmt_list(StmtList *list, void (*visit)(Decl *));

static void mrec_walk_stmt(Stmt *s, void (*visit)(Decl *)) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR: mrec_walk_expr(s->as.var_stmt.expr, visit); break;
        case STMT_ASSIGN:
            mrec_walk_expr(s->as.assign_stmt.target, visit);
            mrec_walk_expr(s->as.assign_stmt.expr, visit);
            break;
        case STMT_EXPR: mrec_walk_expr(s->as.expr_stmt.expr, visit); break;
        case STMT_RETURN: mrec_walk_expr(s->as.return_stmt.value, visit); break;
        case STMT_IF:
            mrec_walk_expr(s->as.if_stmt.cond, visit);
            mrec_walk_stmt_list(s->as.if_stmt.then_branch, visit);
            mrec_walk_stmt_list(s->as.if_stmt.else_branch, visit);
            break;
        case STMT_FOR:
            mrec_walk_expr(s->as.for_stmt.iterable, visit);
            mrec_walk_stmt_list(s->as.for_stmt.body, visit);
            break;
        case STMT_WHILE:
            mrec_walk_expr(s->as.while_stmt.cond, visit);
            if (s->as.while_stmt.measure) mrec_walk_expr(s->as.while_stmt.measure, visit);
            mrec_walk_stmt_list(s->as.while_stmt.body, visit);
            break;
        case STMT_MATCH:
            mrec_walk_expr(s->as.match_stmt.value, visit);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) mrec_walk_expr(p->expr, visit);
                mrec_walk_stmt_list(c->body, visit);
            }
            break;
        case STMT_UNSAFE: mrec_walk_stmt_list(s->as.unsafe_stmt.body, visit); break;
        case STMT_DEFER:  mrec_walk_stmt(s->as.defer_stmt.stmt, visit); break;
        case STMT_COMPTIME_IF:
            mrec_walk_stmt_list(s->as.comptime_if_stmt.then_branch, visit);
            mrec_walk_stmt_list(s->as.comptime_if_stmt.else_branch, visit);
            break;
        default: break;
    }
}

static void mrec_walk_stmt_list(StmtList *list, void (*visit)(Decl *)) {
    for (StmtList *l = list; l; l = l->next) mrec_walk_stmt(l->stmt, visit);
}

static void mrec_walk_expr(Expr *e, void (*visit)(Decl *)) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_CALL: {
            Expr *callee = e->as.call_expr.callee;
            if (callee && callee->decl && callee->decl->kind == DECL_FUNCTION) {
                visit(callee->decl);
            }
            mrec_walk_expr(callee, visit);
            for (ExprList *a = e->as.call_expr.args; a; a = a->next) mrec_walk_expr(a->expr, visit);
            break;
        }
        case EXPR_BINARY:
            mrec_walk_expr(e->as.binary_expr.left, visit);
            mrec_walk_expr(e->as.binary_expr.right, visit);
            break;
        case EXPR_UNARY: mrec_walk_expr(e->as.unary_expr.right, visit); break;
        case EXPR_MEMBER: mrec_walk_expr(e->as.member_expr.target, visit); break;
        case EXPR_INDEX:
            mrec_walk_expr(e->as.index_expr.target, visit);
            mrec_walk_expr(e->as.index_expr.index, visit);
            break;
        case EXPR_RANGE:
            mrec_walk_expr(e->as.range_expr.start, visit);
            mrec_walk_expr(e->as.range_expr.end, visit);
            break;
        case EXPR_MOVE: mrec_walk_expr(e->as.move_expr.expr, visit); break;
        case EXPR_MUT:  mrec_walk_expr(e->as.mut_expr.expr, visit); break;
        case EXPR_CAST: mrec_walk_expr(e->as.cast_expr.expr, visit); break;
        case EXPR_MATCH:
            mrec_walk_expr(e->as.match_expr.value, visit);
            for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) mrec_walk_expr(p->expr, visit);
                mrec_walk_expr(c->body, visit);
            }
            break;
        case EXPR_ARRAY_LITERAL:
            for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next)
                mrec_walk_expr(el->expr, visit);
            break;
        default: break;
    }
}

// Cycle detection via DFS. For simplicity we reject only cycles where every
// edge goes through a DECL_FUNCTION (pure func). Procedures are allowed to
// recurse mutually because P4 does not constrain them.
#define MREC_MAX_NODES 256
static Decl *mrec_stack[MREC_MAX_NODES];
static int mrec_stack_len;
static Decl *mrec_visited[MREC_MAX_NODES];
static int mrec_visited_len;
static Decl *mrec_found_cycle_start = NULL;
static Decl *mrec_found_cycle_end = NULL;

static bool mrec_in_stack(Decl *d) {
    for (int i = 0; i < mrec_stack_len; i++) if (mrec_stack[i] == d) return true;
    return false;
}
static bool mrec_in_visited(Decl *d) {
    for (int i = 0; i < mrec_visited_len; i++) if (mrec_visited[i] == d) return true;
    return false;
}

static Decl *mrec_current_source = NULL;

static void mrec_edge_visit(Decl *callee) {
    if (mrec_found_cycle_start) return;
    if (!callee || callee->kind != DECL_FUNCTION) return;
    if (mrec_in_stack(callee)) {
        // Cycle found.
        mrec_found_cycle_start = callee;
        mrec_found_cycle_end = mrec_current_source;
        return;
    }
    if (mrec_in_visited(callee)) return;
    if (mrec_visited_len >= MREC_MAX_NODES) return;
    if (mrec_stack_len >= MREC_MAX_NODES) return;
    // DFS descent
    mrec_visited[mrec_visited_len++] = callee;
    mrec_stack[mrec_stack_len++] = callee;
    Decl *saved = mrec_current_source;
    mrec_current_source = callee;
    mrec_walk_stmt_list(callee->as.function_decl.body, mrec_edge_visit);
    mrec_current_source = saved;
    mrec_stack_len--;
}

static void sema_check_no_mutual_recursion(DeclList *decls) {
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;
        // Skip generic (comptime) functions — substituted before use.
        bool is_generic = false;
        for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
            if (p->decl && p->decl->kind == DECL_VARIABLE &&
                p->decl->as.variable_decl.type &&
                p->decl->as.variable_decl.type->kind == TYPE_COMPTIME) {
                is_generic = true;
                break;
            }
        }
        if (is_generic) continue;

        // Fresh state per root
        mrec_stack_len = 0;
        mrec_visited_len = 0;
        mrec_found_cycle_start = NULL;
        mrec_found_cycle_end = NULL;

        mrec_visited[mrec_visited_len++] = d;
        mrec_stack[mrec_stack_len++] = d;
        mrec_current_source = d;
        mrec_walk_stmt_list(d->as.function_decl.body, mrec_edge_visit);
        mrec_stack_len = 0;

        if (mrec_found_cycle_start && mrec_found_cycle_start != d) {
            // Cycle not rooted at d: we'll report it when iterating reaches the root.
            // Skip this one and let the canonical entry raise the error.
            continue;
        }
        if (mrec_found_cycle_start == d) {
            fprintf(stderr,
                    "[E011] Error Ln %li, Col %li: pure function '%.*s' participates in mutual recursion (via '%.*s'). "
                    "Mutual recursion breaks the termination guarantee of 'func'.\n",
                    (long)d->line, (long)d->col,
                    (int)d->as.function_decl.name->length, d->as.function_decl.name->name,
                    mrec_found_cycle_end ? (int)mrec_found_cycle_end->as.function_decl.name->length : 0,
                    mrec_found_cycle_end ? mrec_found_cycle_end->as.function_decl.name->name : "?");
            diagnostic_show_line(d->line, d->col);
            exit(1);
        }
    }
}

static void sema_resolve_module(DeclList *decls, const char *module_path,
                                Arena *arena) {
    sema_arena = arena;
    sema_decls = decls;
    sema_ranges = range_table_new(arena);

    // Set source text for diagnostics
    ModuleNode *mod = find_module(module_path);
    if (mod) {
        sema_source_text = mod->source_text;
        sema_source_file = mod->source_file;
    }

    // 1) Clear old globals + insert top-level decls
    sema_clear_globals();
    sema_build_scope(decls, module_path);

    // 2) For each function: resolve → infer → linearity → clear locals
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d) continue;
        if (d->kind != DECL_FUNCTION && d->kind != DECL_PROCEDURE) continue;

        sema_clear_locals();

        // 2.a) Insert parameters into locals
        int param_idx = 0;
        for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
            if (p->decl->kind == DECL_DESTRUCT) {
                // 1) Generate hidden name "_param_N"
                char hidden_name[32];
                snprintf(hidden_name, sizeof(hidden_name), "_param_%d", param_idx);
                
                DeclDestruct *dd = &p->decl->as.destruct_decl;

                // 2) Insert hidden parameter
                sema_insert_local(hidden_name, hidden_name, dd->type, p->decl, false);

                // 3) Resolve struct type to find fields
                Decl *struct_decl = NULL;
                // Simple linear search in sema_decls for the struct
                // (Optimization: could use a hash map, but this is fine for now)
                if (dd->type->kind == TYPE_SIMPLE) {
                    for (DeclList *g = sema_decls; g; g = g->next) {
                        if (g->decl->kind == DECL_STRUCT) {
                            Id *sname = g->decl->as.struct_decl.name;
                            if (sname->length == dd->type->base_type->length &&
                                strncmp(sname->name, dd->type->base_type->name, sname->length) == 0) {
                                struct_decl = g->decl;
                                break;
                            }
                        }
                    }
                }

                if (!struct_decl) {
                    fprintf(stderr, "Error: Could not resolve struct type for destructuring\n");
                    exit(1);
                }

                // 4) For each destructured name, find field type and insert local
                for (IdList *n = dd->names; n; n = n->next) {
                    Type *field_type = NULL;
                    for (DeclList *f = struct_decl->as.struct_decl.fields; f; f = f->next) {
                        Id *fname = f->decl->as.variable_decl.name;
                        if (fname->length == n->id->length &&
                            strncmp(fname->name, n->id->name, fname->length) == 0) {
                            field_type = f->decl->as.variable_decl.type;
                            break;
                        }
                    }

                    if (!field_type) {
                        fprintf(stderr, "Error: Field '%.*s' not found in struct '%.*s'\n", 
                                (int)n->id->length, n->id->name,
                                (int)dd->type->base_type->length, dd->type->base_type->name);
                        exit(1);
                    }

                    // Insert local variable (e.g. "text" -> u8[:0])
                    // Emit will generate "u8[:0] text = _param_N.text;"
                    char raw_field[256];
                    int L = n->id->length < (int)sizeof(raw_field) - 1 ? n->id->length : (int)sizeof(raw_field) - 1;
                    memcpy(raw_field, n->id->name, L);
                    raw_field[L] = '\0';
                    
                    sema_insert_local(raw_field, raw_field, field_type, NULL, false); // Destructured fields don't have a Decl
                }

            } else {
                Id *pid = p->decl->as.variable_decl.name;
                Type *pty = p->decl->as.variable_decl.type;

                char rawp[256];
                int L = pid->length < (int)sizeof(rawp) - 1 ? pid->length
                                                             : (int)sizeof(rawp) - 1;
                memcpy(rawp, pid->name, L);
                rawp[L] = '\0';

                sema_insert_local(rawp, rawp, pty, p->decl, false);
                
                // Handle 'in' constraint: param int in arr
                // Desugars to: param >= 0 and param < arr.len
                if (p->decl->as.variable_decl.in_field && sema_ranges) {
                    Id *arr_id = p->decl->as.variable_decl.in_field;
                    Id *param_id = pid;
                    
                    // Find the array parameter to get its length
                    Type *arr_type = NULL;
                    for (DeclList *arr_p = d->as.function_decl.params; arr_p; arr_p = arr_p->next) {
                        if (arr_p->decl->kind == DECL_VARIABLE) {
                            Id *aname = arr_p->decl->as.variable_decl.name;
                            if (aname->length == arr_id->length &&
                                strncmp(aname->name, arr_id->name, aname->length) == 0) {
                                arr_type = arr_p->decl->as.variable_decl.type;
                                break;
                            }
                        }
                    }
                    
                    if (arr_type) {
                        // Apply range: param >= 0
                        Range r = range_make(0, INT64_MAX);
                        
                        // If array has known length (fixed-size), tighten upper bound
                        if (arr_type->kind == TYPE_ARRAY && arr_type->array_len >= 0) {
                            r = range_make(0, arr_type->array_len - 1);
                        }
                        
                        range_set(sema_ranges, param_id, r);
                    }
                }
                
                // Apply equation-style constraints: b int != 0, x int >= 0 and <= 100
                if (p->decl->as.variable_decl.constraints && sema_ranges) {
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        sema_apply_constraint(c->expr, sema_ranges);
                    }
                }
            }
            param_idx++;
        }

        // 2.b) Name resolution
        current_return_type = d->as.function_decl.return_type;
        current_function_decl = d; // Set current function
        current_module_path = module_path;

        // Apply Pre-Contracts to Range Table
        if (sema_ranges) {
            for (ExprList *pre = d->as.function_decl.pre_contracts; pre; pre = pre->next) {
                sema_resolve_expr(pre->expr);
                sema_infer_expr(pre->expr);
                sema_apply_constraint(pre->expr, sema_ranges);
            }
            
            // Also apply inline parameter constraints (e.g., `b int != 0`)
            // to the function body's range table, so VRA can see them.
            for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
                if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.constraints) {
                    Id *param_name = p->decl->as.variable_decl.name;
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        if (c->expr->kind == EXPR_BINARY) {
                            // Build a synthetic constraint expr: param_name <op> rhs
                            // The constraint's LHS is the parameter itself — apply
                            // it by evaluating against the current range table.
                            Expr synth;
                            synth.kind = EXPR_BINARY;
                            synth.as.binary_expr.op = c->expr->as.binary_expr.op;
                            // LHS: create an identifier expr for the parameter
                            Expr lhs_id;
                            lhs_id.kind = EXPR_IDENTIFIER;
                            lhs_id.as.identifier_expr.id = param_name;
                            lhs_id.type = NULL;
                            synth.as.binary_expr.left = &lhs_id;
                            synth.as.binary_expr.right = c->expr->as.binary_expr.right;
                            sema_apply_constraint(&synth, sema_ranges);
                        }
                    }
                }
            }
        }

        // Resolve Post-Contracts
        // Inject 'result' variable for resolution
        if (d->as.function_decl.post_contracts) {
             // We inject "result" as a local variable so it can be resolved.
             // It will remain in the scope for the body, which is acceptable.
             // If the user shadows it, the inner "result" will be used in the body,
             // but the contracts are already resolved to this outer "result".
             sema_insert_local("result", "result", d->as.function_decl.return_type, NULL, false);
             
             for (ExprList *post = d->as.function_decl.post_contracts; post; post = post->next) {
                 sema_resolve_expr(post->expr);
                 sema_infer_expr(post->expr);
             }
        }

        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next) {
            sema_resolve_stmt(sl->stmt);
        }

        // 2.c) Type inference + bounds checking
        // (walk_stmt is defined as a static function above sema_resolve_module)
        sema_walk_phase = true;
        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next)
            walk_stmt(sl->stmt);
        sema_walk_phase = false;

        current_return_type = NULL;
        current_function_decl = NULL;
        current_module_path = NULL;

        // 2.d) Linearity check: run function-level linearity checker
        // NOTE: sema_check_function_linearity must run while sema_locals still
        // exist (so it can trust that implicit locals were created by resolve).
        sema_check_function_linearity(d);

        // 2.e) Clear locals after all passes
        sema_clear_locals();
    }

    // 3) F-020: detect mutual recursion involving pure functions.
    // Direct recursion is already rejected in typecheck.h; here we catch
    // indirect cycles (f -> g -> f) that break the P4 termination guarantee.
    sema_check_no_mutual_recursion(decls);
}

// Optional: destroy/reset global state
static void sema_destroy(void) {
    sema_clear_globals();
}

#endif // SEMA_H
