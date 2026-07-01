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
    bool is_ptr_guard; // true when index is a pointer and container is its array
    struct InGuardEntry *next;
} InGuardEntry;

static InGuardEntry *sema_in_guards = NULL;

static bool expr_struct_equal(Expr *a, Expr *b); // defined later in sema.h
static bool sema_is_affine_assign(Stmt *s, Id **out_var, long long *out_step); // defined below
static void sema_push_in_guards(Expr *cond);
static bool sema_is_in_guarded(Expr *index, Expr *container);
static bool sema_is_ptr_in_guarded(Expr *ptr, Expr *arr);

// L3: pointer monotone table — pointers whose upper bound is dead inside while loops.
// When p is monotone non-increasing and was initialized at a valid index into arr,
// the upper-bound check `p < arr + arr_len` is always true → dead code in emit.
typedef struct PtrMonotoneEntry {
    Id *ptr_id;           // the pointer variable (p1, p2, out)
    Expr *arr_expr;       // the array it belongs to
    struct PtrMonotoneEntry *next;
} PtrMonotoneEntry;
static PtrMonotoneEntry *sema_ptr_monotone = NULL;

#include "sema/scope.h"
#include "sema/resolve.h"
#include "sema/typecheck.h"
#include "sema/linearity.h"
#include "sema/niche.h"

Type *current_return_type = NULL;
Decl *current_function_decl = NULL;
const char *current_module_path = NULL;
DeclList *sema_decls = NULL;
Arena *sema_arena = NULL;
RangeTable *sema_ranges = NULL;
bool sema_in_unsafe_block = false;
bool sema_walk_phase = false;
bool sema_dump_niche = false;  // set by main from args.dump_niche

/*─────────────────────────────────────────────────────────────────╗
│ Public entry: call this before emit                             │
╚─────────────────────────────────────────────────────────────────*/

// L3: recursive body scanner for affine assignments.
// Collects x = x ± c patterns across nested if/else branches.
// If a variable has updates with DIFFERENT signs (both + and -) it is not monotone
// and is excluded. Variables with SAME-sign updates only are registered.
#define MAX_AFFINE_L3 32
static void l3_scan_affine(StmtList *body, Id **vars, long long *steps,
                            Range *inits, int *n, int max, RangeTable *ranges) {
    for (StmtList *b = body; b; b = b->next) {
        Stmt *s = b->stmt;
        if (!s) continue;
        Id *v = NULL; long long step = 0;
        if (sema_is_affine_assign(s, &v, &step)) {
            // Check for conflict: same var with opposite sign
            bool conflict = false;
            for (int i = 0; i < *n; i++) {
                if (vars[i] && vars[i]->length == v->length &&
                    memcmp(vars[i]->name, v->name, v->length) == 0) {
                    // Same var seen before — conflict if sign differs
                    if ((steps[i] > 0) != (step > 0)) {
                        vars[i] = NULL; // mark as conflicted (not monotone)
                    }
                    conflict = true; break;
                }
            }
            if (!conflict && *n < max) {
                vars[*n] = v; steps[*n] = step;
                inits[*n] = ranges ? range_get(ranges, v) : range_unknown();
                (*n)++;
            }
        } else if (s->kind == STMT_IF) {
            l3_scan_affine(s->as.if_stmt.then_body, vars, steps, inits, n, max, ranges);
            l3_scan_affine(s->as.if_stmt.else_branch, vars, steps, inits, n, max, ranges);
        }
    }
}

// L3: walk the while condition and mark `l3_upper_dead` on pointer in-guards
// where L3 proves the upper bound (ptr < arr + arr_len) is always true.
static void l3_mark_dead_upper_bounds(Expr *cond, PtrMonotoneEntry *monotone) {
    if (!cond || cond->kind != EXPR_BINARY) return;
    if (cond->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        l3_mark_dead_upper_bounds(cond->as.binary_expr.left, monotone);
        l3_mark_dead_upper_bounds(cond->as.binary_expr.right, monotone);
    } else if (cond->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;
        if (lhs && lhs->type && lhs->type->kind == TYPE_POINTER &&
            lhs->kind == EXPR_IDENTIFIER) {
            Id *pid = lhs->as.identifier_expr.id;
            for (PtrMonotoneEntry *e = monotone; e; e = e->next) {
                if (e->ptr_id->length == pid->length &&
                    memcmp(e->ptr_id->name, pid->name, pid->length) == 0 &&
                    expr_struct_equal(e->arr_expr, rhs)) {
                    cond->as.binary_expr.l3_upper_dead = true;
                    break;
                }
            }
        }
    }
}

// L3 pointer monotone analysis: detect `p = p - 1` for pointer vars.
// Registers p in sema_ptr_monotone so the emit can skip the upper-bound check.
static void l3_register_ptr_monotone(StmtList *body) {
    for (StmtList *b = body; b; b = b->next) {
        Stmt *s = b->stmt;
        if (!s) continue;
        if (s->kind == STMT_ASSIGN) {
            Expr *tgt = s->as.assign_stmt.target;
            Expr *rhs = s->as.assign_stmt.expr;
            // Pattern: p = p - 1 where p is TYPE_POINTER
            if (tgt && tgt->kind == EXPR_IDENTIFIER &&
                tgt->type && tgt->type->kind == TYPE_POINTER &&
                rhs && rhs->kind == EXPR_BINARY &&
                rhs->as.binary_expr.op == TOKEN_MINUS) {
                Expr *l = rhs->as.binary_expr.left;
                Expr *r = rhs->as.binary_expr.right;
                // l must be same identifier as tgt, r must be literal 1
                if (l && l->kind == EXPR_IDENTIFIER && r && r->kind == EXPR_LITERAL &&
                    r->as.literal_expr.value > 0) {
                    Id *pid = tgt->as.identifier_expr.id;
                    Id *lid = l->as.identifier_expr.id;
                    if (pid->length == lid->length &&
                        memcmp(pid->name, lid->name, pid->length) == 0) {
                        // Find this pointer's associated array from active in-guards
                        for (InGuardEntry *ig = sema_in_guards; ig; ig = ig->next) {
                            if (!ig->is_ptr_guard) continue;
                            if (ig->index && ig->index->kind == EXPR_IDENTIFIER) {
                                Id *gid = ig->index->as.identifier_expr.id;
                                if (gid->length == pid->length &&
                                    memcmp(gid->name, pid->name, pid->length) == 0) {
                                    // Found: p is monotone non-increasing in arr
                                    // Check not already registered
                                    bool dup = false;
                                    for (PtrMonotoneEntry *e = sema_ptr_monotone; e; e = e->next) {
                                        if (e->ptr_id->length == pid->length &&
                                            memcmp(e->ptr_id->name, pid->name, pid->length) == 0) {
                                            dup = true; break;
                                        }
                                    }
                                    if (!dup) {
                                        PtrMonotoneEntry *entry = arena_push_aligned(sema_arena, PtrMonotoneEntry);
                                        entry->ptr_id = pid;
                                        entry->arr_expr = ig->container;
                                        entry->next = sema_ptr_monotone;
                                        sema_ptr_monotone = entry;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        } else if (s->kind == STMT_IF) {
            l3_register_ptr_monotone(s->as.if_stmt.then_body);
            l3_register_ptr_monotone(s->as.if_stmt.else_branch);
        }
    }
}

// Helper to widen variables modified in a loop to unknown
// S15 (VRA L3): affine pattern detection.
// Returns true if the statement is `x = x + c` or `x = x - c` (literal c),
// setting *out_var, *out_step (positive for +, negative for -).
static bool sema_is_affine_assign(Stmt *s, Id **out_var, long long *out_step) {
    if (!s || s->kind != STMT_ASSIGN) return false;
    Expr *t = s->as.assign_stmt.target;
    Expr *e = s->as.assign_stmt.expr;
    if (!t || !e || t->kind != EXPR_IDENTIFIER || e->kind != EXPR_BINARY) return false;
    TokenKind op = e->as.binary_expr.op;
    if (op != TOKEN_PLUS && op != TOKEN_MINUS) return false;
    Expr *l = e->as.binary_expr.left;
    Expr *r = e->as.binary_expr.right;
    if (!l || !r) return false;
    // Pattern: x = x +/- LIT, or x = LIT + x (commutative for +).
    Id *vt = t->as.identifier_expr.id;
    bool match_xc = (l->kind == EXPR_IDENTIFIER
        && l->as.identifier_expr.id->length == vt->length
        && memcmp(l->as.identifier_expr.id->name, vt->name, vt->length) == 0
        && r->kind == EXPR_LITERAL);
    bool match_cx = (op == TOKEN_PLUS
        && r->kind == EXPR_IDENTIFIER
        && r->as.identifier_expr.id->length == vt->length
        && memcmp(r->as.identifier_expr.id->name, vt->name, vt->length) == 0
        && l->kind == EXPR_LITERAL);
    if (!match_xc && !match_cx) return false;
    long long c = match_xc ? l->as.literal_expr.value /* but we want r */ : 0;
    if (match_xc) c = r->as.literal_expr.value;
    else c = l->as.literal_expr.value;
    *out_var = vt;
    *out_step = (op == TOKEN_MINUS) ? -c : c;
    return true;
}

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
                sema_widen_loop(s->as.if_stmt.then_body, t);
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
    if (!measure) return false;

    // Handle 'and' conjunctions — try each part of the condition
    if (cond && cond->kind == EXPR_BINARY && cond->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        if (sema_verify_measure_nonneg(cond->as.binary_expr.left, measure)) return true;
        if (sema_verify_measure_nonneg(cond->as.binary_expr.right, measure)) return true;
    }

    // Sum measure: prove each addend >= 0 independently using the full condition.
    // Handles: (p1 - &arr1[0]) + (p2 - &arr2[0]) with condition (p1 in arr1) and (p2 in arr2)
    if (measure->kind == EXPR_BINARY && measure->as.binary_expr.op == TOKEN_PLUS) {
        if (sema_verify_measure_nonneg(cond, measure->as.binary_expr.left) &&
            sema_verify_measure_nonneg(cond, measure->as.binary_expr.right)) {
            return true;
        }
    }

    // Handle 'in' condition: idx in arr, measure = arr.len - idx
    // idx in arr => idx < arr.len => arr.len - idx >= 1 > 0
    if (cond && cond->kind == EXPR_BINARY && cond->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        Expr *idx = cond->as.binary_expr.left;
        Expr *arr = cond->as.binary_expr.right;
        if (measure->kind == EXPR_BINARY && measure->as.binary_expr.op == TOKEN_MINUS) {
            Expr *m_hi = measure->as.binary_expr.left;
            Expr *m_lo = measure->as.binary_expr.right;
            // Index in-guard: arr.len - idx
            if (expr_struct_equal(m_lo, idx) &&
                m_hi->kind == EXPR_MEMBER &&
                m_hi->as.member_expr.member->length == 3 &&
                strncmp(m_hi->as.member_expr.member->name, "len", 3) == 0 &&
                expr_struct_equal(m_hi->as.member_expr.target, arr)) {
                return true;
            }
            // Pointer in-guard: ptr in arr, measure = ptr - &arr[0]
            // ptr - &arr[0] >= 0 when ptr in arr (ptr >= arr base)
            if (idx && idx->type && idx->type->kind == TYPE_POINTER) {
                if (expr_struct_equal(m_hi, idx)) {
                    // m_lo should be &arr[0] (EXPR_ADDR of arr[0])
                    if (m_lo->kind == EXPR_ADDR &&
                        m_lo->as.addr_expr.expr &&
                        m_lo->as.addr_expr.expr->kind == EXPR_INDEX) {
                        Expr *base = m_lo->as.addr_expr.expr->as.index_expr.target;
                        if (expr_struct_equal(base, arr)) return true;
                    }
                }
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
// p - &arr[0]  =>  p(+1) only (addr term treated as constant base).
static int measure_extract_vars(Expr *m, MeasureVar *out, int max) {
    if (!m || max <= 0) return 0;

    if (m->kind == EXPR_IDENTIFIER || m->kind == EXPR_MEMBER) {
        out[0].var = m;
        out[0].polarity = +1;
        return 1;
    }

    if (m->kind == EXPR_BINARY) {
        if (m->as.binary_expr.op == TOKEN_MINUS) {
            // Special case: ptr - &arr[0] — treat the EXPR_ADDR as a constant.
            // Only the pointer variable (left side) is tracked in the measure.
            Expr *rhs_m = m->as.binary_expr.right;
            if (rhs_m && rhs_m->kind == EXPR_ADDR) {
                // ptr - &arr[0]: only ptr contributes, addr is constant
                return measure_extract_vars(m->as.binary_expr.left, out, max);
            }
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
                int r = measure_scan_body(s->as.if_stmt.then_body, vars, nvar);
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
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;
        InGuardEntry *e = arena_push_aligned(sema_arena, InGuardEntry);
        e->index = lhs;
        e->container = rhs;
        // Pointer in-guard: left is a pointer type (TYPE_POINTER)
        e->is_ptr_guard = (lhs && lhs->type && lhs->type->kind == TYPE_POINTER);
        e->next = sema_in_guards;
        sema_in_guards = e;
    } else if (cond->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        sema_push_in_guards(cond->as.binary_expr.left);
        sema_push_in_guards(cond->as.binary_expr.right);
    }
}

static bool sema_is_in_guarded(Expr *index, Expr *container) {
    for (InGuardEntry *e = sema_in_guards; e; e = e->next) {
        if (e->is_ptr_guard) continue;
        if (expr_struct_equal(e->index, index) &&
            expr_struct_equal(e->container, container))
            return true;
    }
    return false;
}

// Check if a POINTER `ptr` is guarded within `arr` (pointer `in` guard).
static bool sema_is_ptr_in_guarded(Expr *ptr, Expr *arr) {
    for (InGuardEntry *e = sema_in_guards; e; e = e->next) {
        if (!e->is_ptr_guard) continue;
        if (expr_struct_equal(e->index, ptr) &&
            expr_struct_equal(e->container, arr))
            return true;
    }
    return false;
}

// Push persistent InGuardEntries for each "field Type in container" annotation
// in a struct definition. Call this when a variable or parameter of a struct
// type enters scope. `var_name_id` is the variable/parameter name Id.
static void sema_push_struct_field_guards(Id *var_name_id, Type *var_ty) {
    if (!var_ty || var_ty->kind != TYPE_SIMPLE || !var_ty->base_type) return;
    char sname[256];
    int snlen = (int)var_ty->base_type->length;
    if (snlen >= (int)sizeof(sname)) return;
    memcpy(sname, var_ty->base_type->name, snlen);
    sname[snlen] = '\0';
    Symbol *ssym = sema_lookup(sname);
    if (!ssym || !ssym->decl || ssym->decl->kind != DECL_STRUCT) return;
    for (DeclList *sf = ssym->decl->as.struct_decl.fields; sf; sf = sf->next) {
        if (!sf->decl || sf->decl->kind != DECL_VARIABLE) continue;
        Id *in_fld = sf->decl->as.variable_decl.in_field;
        if (!in_fld) continue;
        // Synthetic EXPR_IDENTIFIER for the variable
        Expr *ve = arena_push_aligned(sema_arena, Expr);
        memset(ve, 0, sizeof(Expr));
        ve->kind = EXPR_IDENTIFIER;
        ve->as.identifier_expr.id = var_name_id;
        // EXPR_MEMBER: var.field (the index)
        Expr *fidx = arena_push_aligned(sema_arena, Expr);
        memset(fidx, 0, sizeof(Expr));
        fidx->kind = EXPR_MEMBER;
        fidx->as.member_expr.target = ve;
        fidx->as.member_expr.member = sf->decl->as.variable_decl.name;
        // EXPR_MEMBER: var.container
        Expr *fcnt = arena_push_aligned(sema_arena, Expr);
        memset(fcnt, 0, sizeof(Expr));
        fcnt->kind = EXPR_MEMBER;
        fcnt->as.member_expr.target = ve;
        fcnt->as.member_expr.member = in_fld;
        // Push the guard
        InGuardEntry *ig = arena_push_aligned(sema_arena, InGuardEntry);
        ig->index = fidx;
        ig->container = fcnt;
        ig->next = sema_in_guards;
        sema_in_guards = ig;
    }
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
                // Strip MODE_MUTABLE from inferred type: `var x = var_param`
                // gives x the VALUE type, not the reference type.
                Type *inferred = s->as.var_stmt.expr->type;
                if (inferred && inferred->mode == MODE_MUTABLE) {
                    Type *stripped = arena_push_aligned(sema_arena, Type);
                    *stripped = *inferred;
                    stripped->mode = MODE_SHARED;
                    inferred = stripped;
                }
                s->as.var_stmt.type = inferred;
            }

            // F4 (spec audit): full type-compatibility enforcement at
            // STMT_VAR is non-trivial — requires refinement-alias
            // resolution + float literal polymorphism + integer literal
            // polymorphism. Deferred. See internal/ai_analysis/
            // spec_audit_2026_05_14.md §F4.

            if (sema_ranges && s->as.var_stmt.expr) {
                Range r = sema_eval_range(s->as.var_stmt.expr, sema_ranges);
                // Q-002 Phase 5: overflow-at-boundary check (var declaration).
                if (s->as.var_stmt.type) {
                    char vname_buf[128];
                    int vn_len = s->as.var_stmt.name->length < 127
                                 ? (int)s->as.var_stmt.name->length : 127;
                    memcpy(vname_buf, s->as.var_stmt.name->name, vn_len);
                    vname_buf[vn_len] = '\0';
                    check_value_fits_type(r, s->as.var_stmt.type, s->line, s->col,
                        "initialization of variable", vname_buf);
                }
                // S2 (VRA L1 auto-sizing): intersect source range with the
                // declared type's range. If the source range is unknown or
                // wider, the var's range falls back to the type's bounds.
                if (s->as.var_stmt.type) {
                    long long tlo, thi;
                    if (type_integer_range(s->as.var_stmt.type, &tlo, &thi)) {
                        if (!r.known) {
                            r = range_make(tlo, thi);
                        } else {
                            long long mn = r.min < tlo ? tlo : r.min;
                            long long mx = r.max > thi ? thi : r.max;
                            if (mn <= mx) r = range_make(mn, mx);
                        }
                    }
                }

                // Q-002 refinement type alias propagation:
                // If the variable's type is a TYPE_SIMPLE pointing to a
                // type alias with constraints (e.g., `type Pressure = int >= 0 and <= 1000`),
                // apply each constraint to the source range, narrowing it
                // and emitting E086 on violation.
                if (sema_ranges && s->as.var_stmt.type
                    && s->as.var_stmt.type->kind == TYPE_SIMPLE
                    && s->as.var_stmt.type->base_type
                    && !sema_in_unsafe_block && r.known) {
                    char tnam[256];
                    isize tl = s->as.var_stmt.type->base_type->length;
                    if (tl < (isize)sizeof(tnam)) {
                        memcpy(tnam, s->as.var_stmt.type->base_type->name, tl);
                        tnam[tl] = '\0';
                        Symbol *tsym = sema_lookup(tnam);
                        if (tsym && tsym->decl && tsym->decl->kind == DECL_TYPE_ALIAS
                            && tsym->decl->as.type_alias_decl.constraints) {
                            for (ExprList *c = tsym->decl->as.type_alias_decl.constraints; c; c = c->next) {
                                if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
                                TokenKind op = c->expr->as.binary_expr.op;
                                Expr *rhs = c->expr->as.binary_expr.right;
                                if (!rhs || rhs->kind != EXPR_LITERAL) continue;
                                long long k = rhs->as.literal_expr.value;
                                bool fits = true;
                                switch (op) {
                                    case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: fits = (r.max <= k); break;  // <=
                                    case TOKEN_ANGLE_BRACKET_LEFT:        fits = (r.max <  k); break;  // <
                                    case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: fits = (r.min >= k); break;  // >=
                                    case TOKEN_ANGLE_BRACKET_RIGHT:       fits = (r.min >  k); break;  // >
                                    case TOKEN_EQUAL_EQUAL:               fits = (r.min == k && r.max == k); break;
                                    case TOKEN_BANG_EQUAL:                fits = (r.min > k || r.max < k); break;
                                    default: fits = true; break;
                                }
                                if (!fits) {
                                    fprintf(stderr,
                                        "[E086] Error Ln %li, Col %li: assignment to '%.*s' violates refinement constraint of type alias '%s': value range [%lld, %lld] does not satisfy the alias constraint.\n",
                                        s->line, s->col,
                                        (int)s->as.var_stmt.name->length, s->as.var_stmt.name->name,
                                        tnam, (long long)r.min, (long long)r.max);
                                    exit(1);
                                }
                            }
                        }
                    }
                }

                range_set(sema_ranges, s->as.var_stmt.name, r);

                // If the initializer is x.len or x.len ± k, register the equality
                // (or affine relationship) between n and __len_x as difference constraints
                // so the Omega Test can cancel terms like (n - i - 1 < n).
                //
                //   var n = x.len      → n = __len_x  (constraints: n-__len_x≤0, __len_x-n≤0)
                //   var n = x.len - k  → n = __len_x - k
                //                        (constraints: n-__len_x≤-k, __len_x-n≤k)
                //   var n = x.len + k  → n = __len_x + k
                //                        (constraints: n-__len_x≤k, __len_x-n≤-k)
                {
                    Expr *init = s->as.var_stmt.expr;
                    Id *n_id   = s->as.var_stmt.name;

                    /* Extract (ref, delta) such that init == ref.len + delta */
                    Id      *ref   = NULL;
                    int64_t  delta = 0;

                    if (init && init->kind == EXPR_MEMBER &&
                        init->as.member_expr.member &&
                        init->as.member_expr.member->length == 3 &&
                        memcmp(init->as.member_expr.member->name, "len", 3) == 0 &&
                        init->as.member_expr.target &&
                        init->as.member_expr.target->kind == EXPR_IDENTIFIER) {
                        /* var n = x.len */
                        ref   = init->as.member_expr.target->as.identifier_expr.id;
                        delta = 0;
                    } else if (init && init->kind == EXPR_BINARY &&
                               (init->as.binary_expr.op == TOKEN_PLUS ||
                                init->as.binary_expr.op == TOKEN_MINUS) &&
                               init->as.binary_expr.left &&
                               init->as.binary_expr.left->kind == EXPR_MEMBER &&
                               init->as.binary_expr.left->as.member_expr.member &&
                               init->as.binary_expr.left->as.member_expr.member->length == 3 &&
                               memcmp(init->as.binary_expr.left->as.member_expr.member->name,
                                      "len", 3) == 0 &&
                               init->as.binary_expr.left->as.member_expr.target &&
                               init->as.binary_expr.left->as.member_expr.target->kind == EXPR_IDENTIFIER &&
                               init->as.binary_expr.right &&
                               init->as.binary_expr.right->kind == EXPR_LITERAL) {
                        /* var n = x.len ± k */
                        ref = init->as.binary_expr.left->as.member_expr.target
                                  ->as.identifier_expr.id;
                        int64_t k = (int64_t)init->as.binary_expr.right->as.literal_expr.value;
                        delta = (init->as.binary_expr.op == TOKEN_PLUS) ? k : -k;
                    }

                    if (ref && sema_ranges) {
                        char lk[272]; int lklen = 6 + (int)ref->length;
                        if (lklen < (int)sizeof(lk)) {
                            memcpy(lk, "__len_", 6);
                            memcpy(lk + 6, ref->name, ref->length);
                            Id *len_id = NULL;
                            for (RangeEntry *re = sema_ranges->head; re; re = re->next) {
                                if ((int)re->var->length == lklen &&
                                    memcmp(re->var->name, lk, lklen) == 0) {
                                    len_id = re->var; break;
                                }
                            }
                            if (len_id) {
                                /* n = __len_x + delta
                                   ↔  n - __len_x ≤  delta
                                      __len_x - n ≤ -delta  */
                                constraint_add(sema_ranges, n_id,   len_id,  delta);
                                constraint_add(sema_ranges, len_id, n_id,   -delta);
                            }
                        }
                    }
                }

                // Register __len_VAR for local sized-slice variables (e.g. var s = arr[lo..hi]).
                // This lets subsequent accesses s[i] use the constraint/interval prover.
                Type *sv_ty = s->as.var_stmt.type;
                if (sv_ty && sv_ty->kind == TYPE_ARRAY && sv_ty->array_len == -1 && sv_ty->size_expr) {
                    Id *vname = s->as.var_stmt.name;
                    char lk[272]; int lklen = 6 + (int)vname->length;
                    if (lklen < (int)sizeof(lk)) {
                        memcpy(lk, "__len_", 6);
                        memcpy(lk + 6, vname->name, vname->length);
                        char *stored = arena_push_many(sema_arena, char, lklen);
                        memcpy(stored, lk, lklen);
                        Id *len_id = arena_push_aligned(sema_arena, Id);
                        len_id->length = lklen; len_id->name = stored;
                        Range len_r = sema_eval_range(sv_ty->size_expr, sema_ranges);
                        if (!len_r.known) len_r = range_make(0, INT64_MAX);
                        range_set(sema_ranges, len_id, len_r);
                    }
                }
            }

            // Struct field invariants: push persistent in-guards so that
            // accesses like `l.text[l.pos]` are bounds-proven automatically.
            sema_push_struct_field_guards(s->as.var_stmt.name, s->as.var_stmt.type);

            // Local pointer invariant: `var p *T in arr = &arr[k]`
            // Push a pointer in-guard so `*p` is safe when `p in arr` is checked.
            if (s->as.var_stmt.in_expr) {
                // Use the RESOLVED (mangled) Id so that comparisons with assignment
                // targets (which use the mangled name after sema_resolve_expr) work.
                char raw_ptr[256];
                int rlen = s->as.var_stmt.name->length < 255 ? s->as.var_stmt.name->length : 255;
                memcpy(raw_ptr, s->as.var_stmt.name->name, rlen); raw_ptr[rlen] = '\0';
                Symbol *psym = sema_lookup(raw_ptr);
                const char *cname_ptr = psym ? psym->c_name : raw_ptr;
                Id *resolved_id = arena_push_aligned(sema_arena, Id);
                resolved_id->length = strlen(cname_ptr);
                resolved_id->name = cname_ptr;

                Expr *p_ve = arena_push_aligned(sema_arena, Expr);
                memset(p_ve, 0, sizeof(Expr));
                p_ve->kind = EXPR_IDENTIFIER;
                p_ve->as.identifier_expr.id = resolved_id; // mangled name
                p_ve->type = s->as.var_stmt.type;
                InGuardEntry *ig = arena_push_aligned(sema_arena, InGuardEntry);
                ig->index = p_ve;
                ig->container = s->as.var_stmt.in_expr;
                ig->is_ptr_guard = true;
                ig->next = sema_in_guards;
                sema_in_guards = ig;
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
            for (StmtList *b = s->as.if_stmt.then_body; b; b = b->next)
                walk_stmt(b->stmt);
            sema_pop_scope();

            // Detect early-return-only then-branch: if THEN ends with a
            // return statement, the code after the if-stmt is reachable
            // only when the condition was false. Propagate the negated
            // condition to the trailing scope (no else-branch case).
            bool then_returns = false;
            {
                StmtList *last = NULL;
                for (StmtList *b = s->as.if_stmt.then_body; b; b = b->next) last = b;
                if (last && last->stmt && last->stmt->kind == STMT_RETURN) {
                    then_returns = true;
                }
            }

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

            if (then_returns) {
                // Keep the negated constraints for the rest of the
                // function. Also, if the original cond was `!(E)`, then
                // the negation is `E` — push E's in-guards so subsequent
                // bounds checks can use them.
                Expr *cond = s->as.if_stmt.cond;
                if (cond && cond->kind == EXPR_UNARY &&
                    cond->as.unary_expr.op == TOKEN_BANG &&
                    cond->as.unary_expr.right) {
                    sema_push_in_guards(cond->as.unary_expr.right);
                }
            } else {
                // Normal: restore state to what it was before the if.
                sema_ranges->head = old_head;
                sema_ranges->constraints = old_constraints;
            }
            break;
        }
        case STMT_FOR: {
            sema_infer_expr(s->as.for_stmt.iterable);
            // Range Analysis: Loop index
            Range end_range = range_unknown();
            Range start_range = range_unknown();
            // Iteration variable for `for V in start..end` is value_name.
            // (index_name is set only for the dual form `for I, V in ...`.)
            Id *iter_var = s->as.for_stmt.index_name
                ? s->as.for_stmt.index_name
                : s->as.for_stmt.value_name;
            if (sema_ranges && s->as.for_stmt.iterable->kind == EXPR_RANGE && iter_var) {
                start_range = sema_eval_range(s->as.for_stmt.iterable->as.range_expr.start, sema_ranges);
                end_range   = sema_eval_range(s->as.for_stmt.iterable->as.range_expr.end, sema_ranges);
                if (start_range.known && end_range.known) {
                    Range r = range_make(start_range.min, end_range.max - 1);
                    range_set(sema_ranges, iter_var, r);
                }
            }

            // S15 (VRA L3): collect affine updates in the body before widening.
            // For each `x = x + c` or `x = x - c`, capture init range and step
            // so we can compute post-loop range precisely.
            #define MAX_AFFINE 16
            Id   *affine_vars[MAX_AFFINE]; long long affine_steps[MAX_AFFINE];
            Range affine_inits[MAX_AFFINE]; int n_affine = 0;
            if (sema_ranges) {
                for (StmtList *b = s->as.for_stmt.body; b; b = b->next) {
                    Id *v = NULL; long long step = 0;
                    if (sema_is_affine_assign(b->stmt, &v, &step) && n_affine < MAX_AFFINE) {
                        affine_vars[n_affine]  = v;
                        affine_steps[n_affine] = step;
                        affine_inits[n_affine] = range_get(sema_ranges, v);
                        n_affine++;
                    }
                }
            }

            // Widen modified variables BEFORE body
            if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);

            // Sized-slice constraint injection: add symbolic "iter_var < end" constraint
            // scoped to the body so it doesn't pollute post-loop analysis.
            ConstraintEntry *__for_old_constraints = sema_ranges ? sema_ranges->constraints : NULL;
            if (sema_ranges && iter_var &&
                s->as.for_stmt.iterable->kind == EXPR_RANGE &&
                s->as.for_stmt.iterable->as.range_expr.end) {
                Expr *end_expr = s->as.for_stmt.iterable->as.range_expr.end;
                if (end_expr->kind == EXPR_MEMBER &&
                    end_expr->as.member_expr.target->kind == EXPR_IDENTIFIER &&
                    end_expr->as.member_expr.member->length == 3 &&
                    strncmp(end_expr->as.member_expr.member->name, "len", 3) == 0) {
                    // for i in 0..obj.len → i - __len_obj <= -1 inside body
                    Id *obj_id = end_expr->as.member_expr.target->as.identifier_expr.id;
                    char key[272];
                    int klen = 6 + (int)obj_id->length;
                    if (klen < (int)sizeof(key)) {
                        memcpy(key, "__len_", 6);
                        memcpy(key + 6, obj_id->name, obj_id->length);
                        Id *len_id = NULL;
                        for (RangeEntry *re = sema_ranges->head; re; re = re->next) {
                            if (re->var->length == klen &&
                                strncmp(re->var->name, key, klen) == 0) {
                                len_id = re->var;
                                break;
                            }
                        }
                        if (len_id) constraint_add(sema_ranges, iter_var, len_id, -1);
                    }
                } else if (end_expr->kind == EXPR_IDENTIFIER) {
                    // for i in 0..n → i - n <= -1 inside body
                    Id *n_id = end_expr->as.identifier_expr.id;
                    constraint_add(sema_ranges, iter_var, n_id, -1);
                }
            }

            sema_push_scope();
            for (StmtList *b = s->as.for_stmt.body; b; b = b->next)
                walk_stmt(b->stmt);
            sema_pop_scope();

            // Restore constraint scope: the symbolic bound only holds inside the body
            if (sema_ranges) sema_ranges->constraints = __for_old_constraints;

            // Widen modified variables AFTER body
            if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);

            // S15 (VRA L3): post-loop affine recap.
            // For each affine var, compute final range from init + step * iter_count.
            // Iter count = end - start (loop iterates exactly that many times).
            if (sema_ranges && n_affine > 0 && start_range.known && end_range.known) {
                long long iter_min = end_range.min - start_range.max;
                long long iter_max = end_range.max - start_range.min;
                if (iter_min < 0) iter_min = 0;  // empty-loop case
                if (iter_max < 0) iter_max = 0;
                for (int i = 0; i < n_affine; i++) {
                    if (!affine_inits[i].known) continue;
                    long long step = affine_steps[i];
                    long long delta_min, delta_max;
                    if (step >= 0) {
                        delta_min = step * iter_min;
                        delta_max = step * iter_max;
                    } else {
                        delta_min = step * iter_max;
                        delta_max = step * iter_min;
                    }
                    Range r = range_make(
                        affine_inits[i].min + delta_min,
                        affine_inits[i].max + delta_max);
                    range_set(sema_ranges, affine_vars[i], r);
                }
            }

            // Preserve loop index value at exit: for i in start..end → i == end after loop
            if (sema_ranges && iter_var && end_range.known) {
                range_set(sema_ranges, iter_var, end_range);
            }
            #undef MAX_AFFINE
            break;
        }
        case STMT_WHILE:
            sema_infer_expr(s->as.while_stmt.cond);
            if (s->as.while_stmt.measure) {
                sema_infer_expr(s->as.while_stmt.measure);
                sema_verify_bounded_while(s);
            }

            // L3 affine analysis for WHILE loops.
            // Collect monotone variables before widening so we can recover
            // their upper/lower bounds after the conservative widen.
            Id   *wl3_vars[MAX_AFFINE_L3]; long long wl3_steps[MAX_AFFINE_L3];
            Range wl3_inits[MAX_AFFINE_L3]; int n_wl3 = 0;
            if (sema_ranges) {
                l3_scan_affine(s->as.while_stmt.body,
                               wl3_vars, wl3_steps, wl3_inits, &n_wl3, MAX_AFFINE_L3,
                               sema_ranges);
            }

            // L3 pointer monotone: register pointers that only decrement.
            // Then mark the while condition's `p in arr` expressions with dead upper bounds.
            l3_register_ptr_monotone(s->as.while_stmt.body);
            l3_mark_dead_upper_bounds(s->as.while_stmt.cond, sema_ptr_monotone);

            // Widen modified variables BEFORE body (conservative)
            if (sema_ranges) sema_widen_loop(s->as.while_stmt.body, sema_ranges);

            // L3 apply: re-establish monotone bounds after widen.
            // Non-increasing vars: upper bound = initial max (preserved throughout).
            // Non-decreasing vars: lower bound = initial min (preserved throughout).
            if (sema_ranges) {
                for (int i = 0; i < n_wl3; i++) {
                    if (!wl3_vars[i]) continue; // conflicted (not monotone)
                    if (!wl3_inits[i].known) continue;
                    Range cur = range_get(sema_ranges, wl3_vars[i]);
                    if (wl3_steps[i] < 0) {
                        // Monotone non-increasing: x ≤ initial_max throughout loop
                        // Re-apply upper bound after widen
                        if (!cur.known || cur.max > wl3_inits[i].max) {
                            Range r = cur.known
                                ? range_make(cur.min, wl3_inits[i].max)
                                : range_make(INT64_MIN, wl3_inits[i].max);
                            r.known = true;
                            range_set(sema_ranges, wl3_vars[i], r);
                        }
                    } else if (wl3_steps[i] > 0) {
                        // Monotone non-decreasing: x ≥ initial_min throughout loop
                        if (!cur.known || cur.min < wl3_inits[i].min) {
                            Range r = cur.known
                                ? range_make(wl3_inits[i].min, cur.max)
                                : range_make(wl3_inits[i].min, INT64_MAX);
                            r.known = true;
                            range_set(sema_ranges, wl3_vars[i], r);
                        }
                    }
                }
            }

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
            sema_infer_expr(s->as.assign_stmt.target);

            // E121: struct field invariant violation check.
            // If LHS is `obj.field` and `field` has `in_field = container`,
            // verify that the RHS value is in range of `obj.container`.
            if (s->as.assign_stmt.target->kind == EXPR_MEMBER) {
                Expr *obj = s->as.assign_stmt.target->as.member_expr.target;
                Id   *fld_name = s->as.assign_stmt.target->as.member_expr.member;
                if (obj && fld_name && obj->type && obj->type->kind == TYPE_SIMPLE && obj->type->base_type) {
                    char sn[256];
                    int snl = (int)obj->type->base_type->length;
                    if (snl < (int)sizeof(sn)) {
                        memcpy(sn, obj->type->base_type->name, snl);
                        sn[snl] = '\0';
                        Symbol *ss = sema_lookup(sn);
                        if (ss && ss->decl && ss->decl->kind == DECL_STRUCT) {
                            for (DeclList *sf = ss->decl->as.struct_decl.fields; sf; sf = sf->next) {
                                if (!sf->decl || sf->decl->kind != DECL_VARIABLE) continue;
                                Id *fn = sf->decl->as.variable_decl.name;
                                if (!fn || fn->length != fld_name->length ||
                                    strncmp(fn->name, fld_name->name, fn->length) != 0) continue;
                                Id *in_fld = sf->decl->as.variable_decl.in_field;
                                if (!in_fld) break; // field found but no invariant
                                // Build synthetic EXPR_MEMBER for obj.container
                                Expr *cnt_expr = arena_push_aligned(sema_arena, Expr);
                                memset(cnt_expr, 0, sizeof(Expr));
                                cnt_expr->kind = EXPR_MEMBER;
                                cnt_expr->as.member_expr.target = obj;
                                cnt_expr->as.member_expr.member = in_fld;
                                // Find container field type for the bounds check
                                Type *cnt_type = NULL;
                                for (DeclList *cf = ss->decl->as.struct_decl.fields; cf; cf = cf->next) {
                                    if (!cf->decl || cf->decl->kind != DECL_VARIABLE) continue;
                                    Id *cfn = cf->decl->as.variable_decl.name;
                                    if (cfn && cfn->length == in_fld->length &&
                                        strncmp(cfn->name, in_fld->name, cfn->length) == 0) {
                                        cnt_type = cf->decl->as.variable_decl.type;
                                        break;
                                    }
                                }
                                // Run bounds check: rhs must be in [0, obj.container.len)
                                if (cnt_type && sema_ranges && !sema_in_unsafe_block) {
                                    // Temporarily push the container in-guard so sema_is_in_guarded
                                    // doesn't double-fire; directly call sema_check_bounds.
                                    // Override error message to E121.
                                    // We can't easily override, so use the VRA in-guard mechanism:
                                    // push (rhs, cnt_expr) and check; if not proven, emit E121.
                                    Expr *rhs = s->as.assign_stmt.expr;
                                    bool proven = sema_is_in_guarded(rhs, cnt_expr);
                                    if (!proven && sema_ranges) {
                                        // Try VRA: check rhs < cnt_type length
                                        Range r = sema_eval_range(rhs, sema_ranges);
                                        bool ok = false;
                                        if (cnt_type->kind == TYPE_ARRAY && cnt_type->array_len >= 0) {
                                            // fixed-size container
                                            if (r.known && r.min >= 0 && r.max < cnt_type->array_len) ok = true;
                                        }
                                        // For dynamic slice: conservatively accept (we can't easily prove without
                                        // the synthetic __len_ var for member access — future work).
                                        if (cnt_type->kind == TYPE_ARRAY && cnt_type->array_len == -1) ok = true;
                                        if (!ok && cnt_type->kind == TYPE_ARRAY && cnt_type->array_len >= 0) {
                                            fprintf(stderr,
                                                "[E121] Error Ln %li, Col %li: assignment to '%.*s' may violate struct invariant "
                                                "'%.*s in %.*s': value range [%lld, %lld] is not proven to be in [0, %lld).\n",
                                                s->line, s->col,
                                                (int)fld_name->length, fld_name->name,
                                                (int)fld_name->length, fld_name->name,
                                                (int)in_fld->length, in_fld->name,
                                                (long long)r.min, (long long)r.max,
                                                (long long)cnt_type->array_len);
                                            diagnostic_show_line(s->line, s->col);
                                            exit(1);
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }

            // Q-002 Phase 5: overflow-at-boundary check (assignment).
            // Covers all assign target shapes: identifier / field / index.
            if (sema_ranges && s->as.assign_stmt.target && s->as.assign_stmt.target->type) {
                Range r = sema_eval_range(s->as.assign_stmt.expr, sema_ranges);
                Expr *tgt = s->as.assign_stmt.target;
                char label[160];
                const char *ctx = "assignment to";
                if (tgt->kind == EXPR_IDENTIFIER && tgt->as.identifier_expr.id) {
                    int n = (int)tgt->as.identifier_expr.id->length;
                    if (n > 159) n = 159;
                    memcpy(label, tgt->as.identifier_expr.id->name, n);
                    label[n] = '\0';
                } else if (tgt->kind == EXPR_MEMBER && tgt->as.member_expr.member) {
                    int n = (int)tgt->as.member_expr.member->length;
                    if (n > 150) n = 150;
                    label[0] = '.';
                    memcpy(label + 1, tgt->as.member_expr.member->name, n);
                    label[n + 1] = '\0';
                    ctx = "assignment to field";
                } else if (tgt->kind == EXPR_INDEX) {
                    label[0] = '\0';
                    ctx = "assignment to indexed element";
                } else {
                    label[0] = '\0';
                }
                check_value_fits_type(r, tgt->type, s->line, s->col, ctx, label);
            }
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
            // Q-002 Phase 5: overflow-at-boundary check (return).
            if (sema_ranges && s->as.return_stmt.value && current_return_type) {
                Range r = sema_eval_range(s->as.return_stmt.value, sema_ranges);
                const char *fname = "?";
                int fn_len = 1;
                if (current_function_decl && current_function_decl->as.function_decl.name) {
                    fname = current_function_decl->as.function_decl.name->name;
                    fn_len = (int)current_function_decl->as.function_decl.name->length;
                }
                char buf[160];
                if (fn_len > 159) fn_len = 159;
                memcpy(buf, fname, fn_len);
                buf[fn_len] = '\0';
                check_value_fits_type(r, current_return_type, s->line, s->col,
                    "return from function", buf);
            }
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
                ? s->as.comptime_if_stmt.then_body
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
            mrec_walk_stmt_list(s->as.if_stmt.then_body, visit);
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
            mrec_walk_stmt_list(s->as.comptime_if_stmt.then_body, visit);
            mrec_walk_stmt_list(s->as.comptime_if_stmt.else_branch, visit);
            break;
        default: break;
    }
}

static void mrec_walk_stmt_list(StmtList *list, void (*visit)(Decl *)) {
    for (StmtList *l = list; l; l = l->next) mrec_walk_stmt(l->stmt, visit);
}

/*─────────────────────────────────────────────────────────────────╗
│ W130: proc-could-be-func suggestion                              │
│                                                                  │
│ A `proc` is eligible to become `func` when its body uses no       │
│ external side effect: no proc calls, no extern-proc calls, no    │
│ panic, no unbounded while (without `decreasing`), no read or     │
│ write of mutable globals. The compiler scans every proc; for    │
│ those eligible, emits W130 with the suggestion.                  │
╚─────────────────────────────────────────────────────────────────*/

static bool proc_w130_eligible;       // false on any violation
static Decl *proc_w130_self;          // decl being analyzed (to detect self-recursion)
static bool proc_w130_has_while_no_measure;  // while w/o decreasing seen

static void proc_w130_visit_expr(Expr *e);
static void proc_w130_visit_stmt(Stmt *s);
static void proc_w130_visit_stmt_list(StmtList *list) {
    for (StmtList *l = list; l; l = l->next) proc_w130_visit_stmt(l->stmt);
}

static void proc_w130_visit_expr(Expr *e) {
    if (!e || !proc_w130_eligible) return;
    switch (e->kind) {
        case EXPR_CALL: {
            Expr *callee = e->as.call_expr.callee;
            // Detect `panic("...")` by name.
            if (callee && callee->kind == EXPR_IDENTIFIER &&
                callee->as.identifier_expr.id &&
                callee->as.identifier_expr.id->length == 5 &&
                memcmp(callee->as.identifier_expr.id->name, "panic", 5) == 0) {
                proc_w130_eligible = false;
                return;
            }
            if (callee && callee->decl) {
                DeclKind k = callee->decl->kind;
                if (k == DECL_PROCEDURE || k == DECL_EXTERN_PROCEDURE) {
                    proc_w130_eligible = false; return;
                }
                if (callee->decl == proc_w130_self) {
                    // Self-recursion: func disallows it.
                    proc_w130_eligible = false; return;
                }
            } else if (callee && callee->kind == EXPR_IDENTIFIER) {
                // Unresolved callee — could be an extern proc or
                // anything else with side effects. Be conservative:
                // do NOT suggest func when we can't prove purity.
                proc_w130_eligible = false; return;
            }
            proc_w130_visit_expr(callee);
            for (ExprList *a = e->as.call_expr.args; a; a = a->next)
                proc_w130_visit_expr(a->expr);
            break;
        }
        case EXPR_IDENTIFIER: {
            if (e->is_global && e->decl && e->decl->kind == DECL_VARIABLE &&
                e->decl->as.variable_decl.is_mutable) {
                proc_w130_eligible = false;
            }
            break;
        }
        case EXPR_BINARY:
            proc_w130_visit_expr(e->as.binary_expr.left);
            proc_w130_visit_expr(e->as.binary_expr.right); break;
        case EXPR_UNARY: proc_w130_visit_expr(e->as.unary_expr.right); break;
        case EXPR_MEMBER: proc_w130_visit_expr(e->as.member_expr.target); break;
        case EXPR_INDEX:
            proc_w130_visit_expr(e->as.index_expr.target);
            proc_w130_visit_expr(e->as.index_expr.index); break;
        case EXPR_RANGE:
            proc_w130_visit_expr(e->as.range_expr.start);
            proc_w130_visit_expr(e->as.range_expr.end); break;
        case EXPR_MOVE: proc_w130_visit_expr(e->as.move_expr.expr); break;
        case EXPR_MUT:  proc_w130_visit_expr(e->as.mut_expr.expr); break;
        case EXPR_CAST: proc_w130_visit_expr(e->as.cast_expr.expr); break;
        case EXPR_MATCH:
            proc_w130_visit_expr(e->as.match_expr.value);
            for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) proc_w130_visit_expr(p->expr);
                proc_w130_visit_expr(c->body);
            }
            break;
        case EXPR_ARRAY_LITERAL:
            for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next)
                proc_w130_visit_expr(el->expr);
            break;
        default: break;
    }
}

static void proc_w130_visit_stmt(Stmt *s) {
    if (!s || !proc_w130_eligible) return;
    switch (s->kind) {
        case STMT_VAR: proc_w130_visit_expr(s->as.var_stmt.expr); break;
        case STMT_ASSIGN:
            // Writing to a mutable global counts as side effect.
            if (s->as.assign_stmt.target &&
                s->as.assign_stmt.target->kind == EXPR_IDENTIFIER &&
                s->as.assign_stmt.target->is_global &&
                s->as.assign_stmt.target->decl &&
                s->as.assign_stmt.target->decl->kind == DECL_VARIABLE &&
                s->as.assign_stmt.target->decl->as.variable_decl.is_mutable) {
                proc_w130_eligible = false;
                return;
            }
            proc_w130_visit_expr(s->as.assign_stmt.target);
            proc_w130_visit_expr(s->as.assign_stmt.expr);
            break;
        case STMT_EXPR: proc_w130_visit_expr(s->as.expr_stmt.expr); break;
        case STMT_RETURN: proc_w130_visit_expr(s->as.return_stmt.value); break;
        case STMT_IF:
            proc_w130_visit_expr(s->as.if_stmt.cond);
            proc_w130_visit_stmt_list(s->as.if_stmt.then_body);
            proc_w130_visit_stmt_list(s->as.if_stmt.else_branch);
            break;
        case STMT_FOR:
            proc_w130_visit_expr(s->as.for_stmt.iterable);
            proc_w130_visit_stmt_list(s->as.for_stmt.body);
            break;
        case STMT_WHILE:
            if (!s->as.while_stmt.measure) {
                // Unbounded while → not pure-eligible.
                proc_w130_has_while_no_measure = true;
                proc_w130_eligible = false;
                return;
            }
            proc_w130_visit_expr(s->as.while_stmt.cond);
            proc_w130_visit_expr(s->as.while_stmt.measure);
            proc_w130_visit_stmt_list(s->as.while_stmt.body);
            break;
        case STMT_MATCH:
            proc_w130_visit_expr(s->as.match_stmt.value);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) proc_w130_visit_expr(p->expr);
                proc_w130_visit_stmt_list(c->body);
            }
            break;
        case STMT_UNSAFE: proc_w130_visit_stmt_list(s->as.unsafe_stmt.body); break;
        case STMT_DEFER: proc_w130_visit_stmt(s->as.defer_stmt.stmt); break;
        default: break;
    }
}

static bool sema_w130_silent = false;  // suppress for stdlib if needed

static void sema_check_proc_eligibility(Decl *d) {
    if (!d || d->kind != DECL_PROCEDURE) return;
    if (sema_w130_silent) return;
    proc_w130_eligible = true;
    proc_w130_self = d;
    proc_w130_has_while_no_measure = false;
    proc_w130_visit_stmt_list(d->as.function_decl.body);
    if (proc_w130_eligible) {
        Id *n = d->as.function_decl.name;
        // Skip if name is "main" — entrypoint must remain a proc (it returns
        // i32 exit code and signals "this is the program start").
        if (n && n->length == 4 && memcmp(n->name, "main", 4) == 0) return;
        fprintf(stderr,
            "[W130] '%.*s' is declared `proc` but has no observable side\n"
            "       effect: it could be `func`. Consider downgrading to\n"
            "       `func` for clearer intent.\n",
            (int)n->length, n->name);
    }
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

    // Q-008: enforce `mov` on every linear field of every struct/enum.
    {
        bool any_error = false;
        for (DeclList *dl = decls; dl; dl = dl->next) {
            if (!dl->decl) continue;
            if (dl->decl->kind == DECL_STRUCT || dl->decl->kind == DECL_ENUM) {
                if (sema_check_struct_field_mov(dl->decl)) any_error = true;
            }
        }
        if (any_error) exit(1);
    }

    // D-Niche M2+M5: precompute niche layout for every enum, emit
    // a best-effort W120 warning when the pool was insufficient,
    // and dump the decision when --dump-niche is set.
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl && dl->decl->kind == DECL_ENUM) {
            NicheLayout layout = niche_compute_layout(&dl->decl->as.enum_decl);
            niche_emit_w120(&dl->decl->as.enum_decl, &layout);
            if (sema_dump_niche) {
                niche_dump_layout(&dl->decl->as.enum_decl, &layout);
            }
        }
    }


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

                // Seed VRA range for UNSIGNED integer parameters only.
                // uN / usize params are always >= 0; seeding [0, max_uN] lets the prover
                // use non-negativity (and tight upper bounds for small types like u1/u8)
                // without requiring an explicit >= 0 annotation.
                //
                // We do NOT seed signed types (iN): they carry no new information beyond
                // "could be anything", and seeding [-2^(N-1), 2^(N-1)-1] would make
                // arithmetic on two i32 params produce a range that violates the return
                // type's bounds, triggering false E086 errors on otherwise valid functions.
                if (sema_ranges && pty) {
                    int bits; bool sgn;
                    if (parse_iN_uN(pty, &bits, &sgn) && !sgn) {
                        /* uN: range is [0, 2^N - 1]  (or [0, INT64_MAX] for N >= 63) */
                        long long tlo, thi;
                        if (type_integer_range(pty, &tlo, &thi)) {
                            range_set(sema_ranges, pid,
                                      range_make((int64_t)tlo, (int64_t)thi));
                        }
                    } else if (pty->kind == TYPE_SIMPLE && pty->base_type) {
                        /* usize: also unsigned */
                        isize pl = pty->base_type->length;
                        if (pl == 5 && memcmp(pty->base_type->name, "usize", 5) == 0) {
                            range_set(sema_ranges, pid, range_make(0, INT64_MAX));
                        }
                    }
                }

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

                // Sized slices: for every dynamic-length array parameter (including plain
                // i32[]), register a synthetic __len_PARAM entry in the VRA range table.
                // This lets the for-loop constraint injector and the bounds checker use
                // symbolic proof even when the interval is wide ([0, INT64_MAX]).
                if (sema_ranges && pty->kind == TYPE_ARRAY && pty->array_len == -1) {
                    char lenkey[272];
                    int lklen = 6 + (int)pid->length;
                    if (lklen < (int)sizeof(lenkey)) {
                        memcpy(lenkey, "__len_", 6);
                        memcpy(lenkey + 6, pid->name, pid->length);
                        // Allocate persistent storage for the key in the sema arena
                        char *stored = arena_push_many(sema_ranges->arena, char, lklen);
                        memcpy(stored, lenkey, lklen);
                        Id *len_id = arena_push_aligned(sema_ranges->arena, Id);
                        len_id->length = lklen;
                        len_id->name   = stored;
                        range_set(sema_ranges, len_id, range_make(0, INT64_MAX));

                        // If a size_expr is given with equality, add constraint linking
                        // this parameter's length to the referenced expression.
                        if (pty->size_expr && pty->size_relop == TOKEN_EQUAL_EQUAL) {
                            if (pty->size_expr->kind == EXPR_MEMBER &&
                                pty->size_expr->as.member_expr.target->kind == EXPR_IDENTIFIER &&
                                pty->size_expr->as.member_expr.member->length == 3 &&
                                strncmp(pty->size_expr->as.member_expr.member->name, "len", 3) == 0) {
                                // a i32[out.len] → a.len == out.len (__len_a == __len_out)
                                Id *ref_id = pty->size_expr->as.member_expr.target->as.identifier_expr.id;
                                char rkey[272];
                                int rklen = 6 + (int)ref_id->length;
                                if (rklen < (int)sizeof(rkey)) {
                                    memcpy(rkey, "__len_", 6);
                                    memcpy(rkey + 6, ref_id->name, ref_id->length);
                                    // Find the already-registered __len_REF Id
                                    Id *ref_len_id = NULL;
                                    for (RangeEntry *re = sema_ranges->head; re; re = re->next) {
                                        if (re->var->length == rklen &&
                                            strncmp(re->var->name, rkey, rklen) == 0) {
                                            ref_len_id = re->var;
                                            break;
                                        }
                                    }
                                    if (ref_len_id) {
                                        constraint_add(sema_ranges, len_id, ref_len_id, 0);
                                        constraint_add(sema_ranges, ref_len_id, len_id, 0);
                                    }
                                }
                            } else if (pty->size_expr->kind == EXPR_IDENTIFIER) {
                                // out i32[n] → out.len == n (__len_out == n)
                                Id *n_id = pty->size_expr->as.identifier_expr.id;
                                constraint_add(sema_ranges, len_id, n_id, 0);
                                constraint_add(sema_ranges, n_id, len_id, 0);
                            } else if (pty->size_expr->kind == EXPR_BINARY) {
                            // Arithmetic size_expr: out i32[a.len + b.len] or i32[src.len - k].
                            // Derive monotone constraints that the constraint prover can chain.
                            TokenKind binop = pty->size_expr->as.binary_expr.op;
                            Expr *se_lhs = pty->size_expr->as.binary_expr.left;
                            Expr *se_rhs = pty->size_expr->as.binary_expr.right;
                            // Helper: if E is EXPR_MEMBER(x.len), find __len_x and return its Id.
                            #define FIND_LEN_ID(E, OUT_ID) do { \
                                if ((E)->kind == EXPR_MEMBER && \
                                    (E)->as.member_expr.member->length == 3 && \
                                    strncmp((E)->as.member_expr.member->name, "len", 3) == 0 && \
                                    (E)->as.member_expr.target->kind == EXPR_IDENTIFIER) { \
                                    Id *_ref = (E)->as.member_expr.target->as.identifier_expr.id; \
                                    char _rk[272]; int _rkl = 6 + (int)_ref->length; \
                                    if (_rkl < (int)sizeof(_rk)) { \
                                        memcpy(_rk, "__len_", 6); \
                                        memcpy(_rk + 6, _ref->name, _ref->length); \
                                        for (RangeEntry *_re = sema_ranges->head; _re; _re = _re->next) { \
                                            if (_re->var->length == _rkl && \
                                                strncmp(_re->var->name, _rk, _rkl) == 0) \
                                            { (OUT_ID) = _re->var; break; } \
                                        } \
                                    } \
                                } \
                            } while(0)
                            if (binop == TOKEN_PLUS) {
                                // out i32[a.len + b.len]:
                                //   a.len ≤ out.len → __len_a - __len_out <= 0
                                //   b.len ≤ out.len → __len_b - __len_out <= 0
                                Id *la_id = NULL; FIND_LEN_ID(se_lhs, la_id);
                                Id *lb_id = NULL; FIND_LEN_ID(se_rhs, lb_id);
                                if (la_id) constraint_add(sema_ranges, la_id, len_id, 0);
                                if (lb_id) constraint_add(sema_ranges, lb_id, len_id, 0);
                            } else if (binop == TOKEN_MINUS && se_rhs->kind == EXPR_LITERAL) {
                                // out i32[src.len - k]:
                                //   out.len = src.len - k → out.len - src.len <= -k
                                int64_t k = se_rhs->as.literal_expr.value;
                                Id *ls_id = NULL; FIND_LEN_ID(se_lhs, ls_id);
                                if (ls_id) constraint_add(sema_ranges, len_id, ls_id, -k);
                            }
                            #undef FIND_LEN_ID
                            }  // closes else if (EXPR_BINARY)
                        }      // closes if (TOKEN_EQUAL_EQUAL)
                        if (pty->size_expr &&
                                   (pty->size_relop == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL ||
                                    pty->size_relop == TOKEN_ANGLE_BRACKET_RIGHT)) {
                            // arr i32[>= n] → arr.len >= n → n - __len_arr <= 0
                            if (pty->size_expr->kind == EXPR_IDENTIFIER) {
                                Id *n_id = pty->size_expr->as.identifier_expr.id;
                                int64_t delta = (pty->size_relop == TOKEN_ANGLE_BRACKET_RIGHT) ? -1 : 0;
                                constraint_add(sema_ranges, n_id, len_id, delta);
                            } else if (pty->size_expr->kind == EXPR_LITERAL) {
                                // arr i32[> k] / arr i32[>= k] with literal k:
                                // register a concrete lower bound on __len_arr.
                                int64_t k = pty->size_expr->as.literal_expr.value;
                                int64_t min_len = k + (pty->size_relop == TOKEN_ANGLE_BRACKET_RIGHT ? 1 : 0);
                                range_set(sema_ranges, len_id, range_make(min_len, INT64_MAX));
                            }
                        }
                    }
                }
                // Struct-typed parameter: push field invariants as in-guards
                // so the callee proves `l.text[l.pos]` safe without explicit guard.
                sema_push_struct_field_guards(pid, pty);
            }
            param_idx++;
        }

        // 2.b) Name resolution
        current_return_type = d->as.function_decl.return_type;
        current_function_decl = d; // Set current function
        // Q-018: use the decl's defining_module if known so that cross-module
        // visibility checks within imported function bodies see the correct
        // owning module. Fallback to top-level module_path.
        current_module_path = d->defining_module ? d->defining_module : module_path;

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
        // Snapshot sema_ranges state so any constraints added during walk
        // (e.g. by post-`if then-returns` propagation) don't leak to the
        // next function's resolve.
        RangeEntry *__fn_old_head = sema_ranges ? sema_ranges->head : NULL;
        ConstraintEntry *__fn_old_cons = sema_ranges ? sema_ranges->constraints : NULL;
        InGuardEntry *__fn_old_guards = sema_in_guards;
        sema_walk_phase = true;
        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next)
            walk_stmt(sl->stmt);
        sema_walk_phase = false;
        if (sema_ranges) {
            sema_ranges->head = __fn_old_head;
            sema_ranges->constraints = __fn_old_cons;
        }
        sema_in_guards = __fn_old_guards;

        // 2.d) Linearity check: run function-level linearity checker
        // NOTE: sema_check_function_linearity must run while sema_locals still
        // exist (so it can trust that implicit locals were created by resolve).
        // We keep current_function_decl set so the linearity pass can inspect
        // the function's parameters (Sprint 5 step D).
        sema_check_function_linearity(d);

        current_return_type = NULL;
        current_function_decl = NULL;
        current_module_path = NULL;

        // 2.e) Clear locals after all passes
        sema_clear_locals();
    }

    // 3) F-020: detect mutual recursion involving pure functions.
    // Direct recursion is already rejected in typecheck.h; here we catch
    // indirect cycles (f -> g -> f) that break the P4 termination guarantee.
    sema_check_no_mutual_recursion(decls);

    // 4) W130: every `proc` whose body has no side effect could be `func`.
    // Run AFTER per-function resolve so Expr.decl is populated everywhere.
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl && dl->decl->kind == DECL_PROCEDURE) {
            sema_check_proc_eligibility(dl->decl);
        }
    }
}

// Optional: destroy/reset global state
static void sema_destroy(void) {
    sema_clear_globals();
}

#endif // SEMA_H
