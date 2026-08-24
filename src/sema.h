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
    bool is_ptr_guard;      // true when index is a pointer and container is its array
    bool is_backward_guard; // true when guard is `p > arr` (backward iteration)
    struct InGuardEntry *next;
} InGuardEntry;

static InGuardEntry *sema_in_guards = NULL;

static bool expr_struct_equal(Expr *a, Expr *b); // defined later in sema.h
static bool sema_is_affine_assign(Stmt *s, Id **out_var, long long *out_step); // defined below
static void sema_push_in_guards(Expr *cond);
static bool sema_is_in_guarded(Expr *index, Expr *container);

// Nullable narrowing: `if x { … }` / `if x != nil { … }` proves x non-nil inside
// the branch, so ?T narrows to T there (deref/pass/return-safe). Mirrors the
// in-guard mechanism — a scoped stack of variables proven present. Declared here
// (before the sub-header includes) so check_conversion in typecheck.h can query it.
typedef struct NarrowEntry { Expr *var; struct NarrowEntry *next; } NarrowEntry;
static NarrowEntry *sema_narrows = NULL;
static void sema_push_narrows(Expr *cond, bool negated);
static bool sema_is_narrowed(Expr *e);

// Defined in sema/niche.h (included after monomorph.h) — forward-declared here so
// union_lower() in monomorph.h can enforce "zero-cost or reject" on the anonymous
// enum it synthesizes for `T | markers`.
static bool niche_enum_is_zero_cost(struct EnumDecl *e);

// Union (`T | markers`) construction coercion — defined below (after the includes);
// forward-declared here so the call-argument check in typecheck.h can reach it.
static void sema_union_coerce(Expr **slot, Type *target);

// If `t` is a union enum (`T | markers`), the payload type T (the `some`
// variant's field); else NULL. Forward-declared so check_conversion (typecheck.h)
// can narrow a proven-present union to its value type.
static Type *union_payload_type(Type *t);

// The lowered anonymous enum backing a `T | markers` union type (or NULL).
// Forward-declared so the `try`/`else` inference in typecheck.h can enumerate a
// union's markers for the ⊆ propagation check and stash the enum for emit.
static Decl *find_union_enum(Type *t);

// L3: pointer monotone table — pointers whose upper bound is dead inside while loops.
// When p is monotone non-increasing and was initialized at a valid index into arr,
// the upper-bound check `p < arr + arr_len` is always true → dead code in emit.
typedef struct PtrMonotoneEntry {
    Id *ptr_id;           // the pointer variable (p1, p2, out)
    Expr *arr_expr;       // the array it belongs to
    Expr *init_idx;       // the index k in `var p in arr = &arr[k]` (for lower bound proof)
    bool unconditional;   // true if p = p-1 only at top level (not in if/else)
    struct PtrMonotoneEntry *next;
} PtrMonotoneEntry;
static PtrMonotoneEntry *sema_ptr_monotone = NULL;

// L3 init-index table: records the initialization index for ptr in arr variables.
typedef struct PtrInitIdxEntry {
    Id *ptr_id;       // mangled name
    Expr *init_idx;   // the k in &arr[k]
    struct PtrInitIdxEntry *next;
} PtrInitIdxEntry;
static PtrInitIdxEntry *sema_ptr_init_idx = NULL;

#include "sema/scope.h"
#include "sema/resolve.h"
#include "sema/typecheck.h"
#include "sema/monomorph.h"
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
bool sema_addr_of_context = false; // set by EXPR_ADDR to relax &arr[len] in bounds check
bool sema_dump_niche = false;      // set by main from args.dump_niche (D-Niche re-land)
bool sema_dump_effects = false;    // set by main from args.dump_effects (F3.3 effect row)

/*─────────────────────────────────────────────────────────────────╗
│ Union (`T | markers`) construction coercion                      │
│ At a boundary whose target is a union enum, rewrite the value    │
│ into normal enum construction — a bare marker → `U.marker`, a    │
│ payload value → `U.__payload(value)` — so sema + emit handle it  │
│ via the existing enum path (the constructors are niche-optimized).│
╚─────────────────────────────────────────────────────────────────*/
static Decl *find_union_enum(Type *t) {
    if (!t) return NULL;
    while (t->kind == TYPE_COMPTIME && t->element_type) t = t->element_type;
    if (t->kind == TYPE_UNION) t = mono_resolve_type_apps(t);  // lower a raw union → its enum
    if (t->kind != TYPE_SIMPLE || !t->base_type) return NULL;
    const char *n = t->base_type->name; isize nl = t->base_type->length;
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_ENUM || !d->as.enum_decl.is_union) continue;
        Id *en = d->as.enum_decl.type_name;
        if (en && en->length == nl && memcmp(en->name, n, (size_t)nl) == 0) return d;
    }
    return NULL;
}

static Type *union_payload_type(Type *t) {
    Decl *U = find_union_enum(t);
    if (!U) return NULL;
    Variant *sv = U->as.enum_decl.variants;   // `some` payload variant is first
    if (sv && sv->fields && sv->fields->decl && sv->fields->decl->kind == DECL_VARIABLE)
        return sv->fields->decl->as.variable_decl.type;
    return NULL;
}

// If `e` (post-resolve) refers to one of union U's markers, return that marker
// variant's name (matched by the mangled-name suffix `..._<marker>`).
static Id *union_match_marker(Decl *U, Expr *e) {
    if (!U || !e || e->kind != EXPR_IDENTIFIER) return NULL;
    if (e->decl != U) return NULL;                 // resolve bound it to this enum's variant
    const char *nm = e->as.identifier_expr.id->name; isize nl = e->as.identifier_expr.id->length;
    for (Variant *v = U->as.enum_decl.variants; v; v = v->next) {
        if (v->fields) continue;                   // skip the `some` payload variant
        isize vl = v->name->length;
        if (nl > vl && nm[nl - vl - 1] == '_' &&
            memcmp(nm + nl - vl, v->name->name, (size_t)vl) == 0)
            return v->name;
    }
    return NULL;
}

// The value of a `T | markers` union is reached through the `else` arm of a
// `case`: when EVERY marker is explicitly matched by a non-`else` arm, the
// `else` arm provably covers only the payload, so the scrutinee narrows to the
// value type T there. Returns true when that narrowing is sound. (If a marker
// is left to `else`, narrowing would be unsound — `else` could be that marker —
// so we return false and the value stays a union, i.e. E063 on bare use.)
static bool union_else_covers_payload(Expr *value, StmtMatchCase *cases) {
    if (!value || value->kind != EXPR_IDENTIFIER || !value->type) return false;
    Type *t = value->type;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    Decl *U = find_union_enum(t);
    if (!U) return false;
    for (Variant *v = U->as.enum_decl.variants; v; v = v->next) {
        if (v->fields || !v->name) continue;         // skip the payload variant
        bool covered = false;
        for (StmtMatchCase *c = cases; c && !covered; c = c->next) {
            if (!c->patterns) continue;              // the `else` arm itself doesn't cover markers
            for (ExprList *p = c->patterns; p; p = p->next)
                if (pattern_matches_variant(p->expr, v->name)) { covered = true; break; }
        }
        if (!covered) return false;
    }
    return true;
}

static void sema_union_coerce(Expr **slot, Type *target) {
    if (!slot || !*slot || !target) return;
    // A union-returning call's result type may still be a raw TYPE_UNION (its
    // type_func was cached before signature lowering) — lower both sides so a
    // same-union assignment matches instead of mis-firing the `some` wrap.
    if (target->kind == TYPE_UNION) target = mono_resolve_type_apps(target);
    if ((*slot)->type && (*slot)->type->kind == TYPE_UNION)
        (*slot)->type = mono_resolve_type_apps((*slot)->type);
    Decl *U = find_union_enum(target);
    if (!U) return;
    Expr *e = *slot;
    Type *uty = type_simple(sema_arena, U->as.enum_decl.type_name);
    // Payload-marker construction: `Marker(args)` where Marker is a fielded variant
    // of U (not the internal `__payload`) → rewrite to U.Marker(args). Placed before
    // the guards so a bare-marker call, however it resolved, is normalized to the
    // canonical constructor (consistent naming with the enum's own definition).
    if (e->kind == EXPR_CALL && e->as.call_expr.callee &&
        e->as.call_expr.callee->kind == EXPR_IDENTIFIER) {
        // The resolver has already mangled the callee to `<module>_<enum>_<Marker>`
        // (module-prefixed), which mismatches the synthetic enum's unprefixed
        // constructor def. Match a fielded marker by the `_<Marker>` suffix and
        // re-build the call as U.Marker(args) — the same expr_member path the
        // nullary markers use, which emits the unprefixed, def-consistent name.
        Id *cn = e->as.call_expr.callee->as.identifier_expr.id;
        for (Variant *v = U->as.enum_decl.variants; v; v = v->next) {
            if (!v->fields) continue;
            if (v->name->length == 9 && memcmp(v->name->name, "__payload", 9) == 0) continue;
            isize vl = v->name->length;
            if (cn->length > vl && cn->name[cn->length - vl - 1] == '_' &&
                memcmp(cn->name + cn->length - vl, v->name->name, (size_t)vl) == 0) {
                Expr *tgt = expr_type(sema_arena, uty); tgt->decl = U;
                Expr *pw = expr_member(sema_arena, tgt, v->name);
                Expr *call = expr_call(sema_arena, pw, e->as.call_expr.args);
                sema_infer_expr(call);
                *slot = call;
                return;
            }
        }
    }
    if (e->type && core_identical(e->type, target)) return;   // already exactly this union
    if (e->type && find_union_enum(e->type)) return;          // already SOME union value — don't re-wrap
    Id *marker = union_match_marker(U, e);
    if (marker) {                                             // bare marker → U.marker
        Expr *tgt = expr_type(sema_arena, uty); tgt->decl = U;   // member handler keys on target->decl
        Expr *m = expr_member(sema_arena, tgt, marker);
        sema_infer_expr(m);
        *slot = m;
        return;
    }
    // payload value → U.__payload(value) — ONLY when e is exactly the payload
    // type. Anything else (a union value, a type mismatch, or untyped) is left to
    // check_conversion so it can pass a same-union assignment or report the error.
    Variant *sv = U->as.enum_decl.variants;   // the payload variant is first
    Type *payload = (sv && sv->fields && sv->fields->decl && sv->fields->decl->kind == DECL_VARIABLE)
                    ? sv->fields->decl->as.variable_decl.type : NULL;
    if (!e->type || !payload || !types_equal_exact(e->type, payload)) return;
    Expr *tgt = expr_type(sema_arena, uty); tgt->decl = U;
    Expr *pw = expr_member(sema_arena, tgt, sv->name);   // the payload variant, by its own name
    Expr *call = expr_call(sema_arena, pw, expr_list(sema_arena, e));
    sema_infer_expr(call);
    *slot = call;
}

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

// L3: check if ptr_id is ONLY decremented at the top level of body (not in if/else).
// Unconditionally decremented pointers decrement every iteration, not conditionally.
static bool l3_is_unconditional_decrement(StmtList *body, Id *pid) {
    bool found_top = false, found_nested = false;
    for (StmtList *b = body; b; b = b->next) {
        Stmt *s = b->stmt;
        if (!s) continue;
        Id *v = NULL; long long step = 0;
        if (sema_is_affine_assign(s, &v, &step) && step < 0 &&
            v && v->length == pid->length && memcmp(v->name, pid->name, v->length) == 0) {
            found_top = true;
        } else if (s->kind == STMT_IF) {
            // Scan then + else branches for nested decrements
            for (StmtList *tb = s->as.if_stmt.then_body; tb; tb = tb->next) {
                Id *v2 = NULL; long long s2 = 0;
                if (sema_is_affine_assign(tb->stmt, &v2, &s2) && s2 < 0 &&
                    v2 && v2->length == pid->length && memcmp(v2->name, pid->name, v2->length) == 0)
                    found_nested = true;
            }
            for (StmtList *eb = s->as.if_stmt.else_branch; eb; eb = eb->next) {
                Id *v2 = NULL; long long s2 = 0;
                if (sema_is_affine_assign(eb->stmt, &v2, &s2) && s2 < 0 &&
                    v2 && v2->length == pid->length && memcmp(v2->name, pid->name, v2->length) == 0)
                    found_nested = true;
            }
        }
    }
    return found_top && !found_nested;
}

// Auto-measure: check if body contains p = p - k (ptr decrement) for a given ptr id.
static bool body_has_ptr_decrement(StmtList *body, Id *pid) {
    for (StmtList *l = body; l; l = l->next) {
        Stmt *s = l->stmt;
        if (!s) continue;
        if (s->kind == STMT_ASSIGN) {
            Expr *tgt = s->as.assign_stmt.target;
            Expr *val = s->as.assign_stmt.expr;
            if (tgt && tgt->kind == EXPR_IDENTIFIER) {
                Id *tid = tgt->as.identifier_expr.id;
                if (tid->length == pid->length && memcmp(tid->name, pid->name, pid->length) == 0) {
                    if (val && val->kind == EXPR_BINARY && val->as.binary_expr.op == TOKEN_MINUS) {
                        Expr *vlhs = val->as.binary_expr.left;
                        if (vlhs && vlhs->kind == EXPR_IDENTIFIER) {
                            Id *vid = vlhs->as.identifier_expr.id;
                            if (vid->length == pid->length && memcmp(vid->name, pid->name, pid->length) == 0)
                                return true;
                        }
                    }
                }
            }
        }
        if (s->kind == STMT_IF) {
            if (body_has_ptr_decrement(s->as.if_stmt.then_body, pid)) return true;
            if (body_has_ptr_decrement(s->as.if_stmt.else_branch, pid)) return true;
        }
    }
    return false;
}

// P0 (memory safety): the affine post-loop recap assumes the loop runs EXACTLY
// `end-start` times and steps each affine var once per iteration. That is false
// when the body can exit early or skip iterations: `break`/`continue`/`return`
// invalidate the iteration count (a var reaches a SMALLER final value than the
// recap computes → the compiler then "proves" an out-of-bounds index safe —
// ASan-confirmed). Return true if the recap must be discarded (fall back to the
// sound widened range). Recursing into nested loops is a safe over-approximation.
static bool loop_body_defeats_affine_recap(StmtList *body) {
    for (StmtList *b = body; b; b = b->next) {
        Stmt *s = b->stmt;
        if (!s) continue;
        switch (s->kind) {
            case STMT_BREAK: case STMT_CONTINUE: case STMT_RETURN: return true;
            case STMT_IF:
                if (loop_body_defeats_affine_recap(s->as.if_stmt.then_body)) return true;
                if (loop_body_defeats_affine_recap(s->as.if_stmt.else_branch)) return true;
                break;
            case STMT_MATCH:
                for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next)
                    if (loop_body_defeats_affine_recap(c->body)) return true;
                break;
            case STMT_UNSAFE:
                if (loop_body_defeats_affine_recap(s->as.unsafe_stmt.body)) return true;
                break;
            case STMT_FOR:
                if (loop_body_defeats_affine_recap(s->as.for_stmt.body)) return true;
                break;
            case STMT_WHILE:
                if (loop_body_defeats_affine_recap(s->as.while_stmt.body)) return true;
                break;
            default: break;
        }
    }
    return false;
}

// L3 lower bound: mark `l3_lower_dead` on pointer in-guards where the lower-bound
// check `ptr >= arr_base` is provably always true, enabling GCC memcpy detection.
//
// Strategy A (algebraic): for ptr P with init index k_P and others with sum k_others:
//   eval(k_P - k_others, ranges).min >= 0 → lower bound dead.
//   Covers: out.init = (m+n-1) vs p1.init+p2.init = (m-1)+(n-1) → diff = 1 >= 0 ✓
//
// Strategy B (co-decrement): if ALL monotone ptrs in condition decrement unconditionally
//   (every iteration) → they stay in sync → lower bounds are mutually implied → dead.
//   Covers second loop: p2 and out both unconditionally decrement together.
static void l3_mark_dead_lower_bounds(Expr *cond, StmtList *body) {
    if (!cond) return;

    // Collect all `p in arr` pointer in-guards in this condition
    #define MAX_PTR_CONDS 8
    Expr *cond_ptrs[MAX_PTR_CONDS]; int n_conds = 0;
    // Recursive collection from AND chain
    Expr *stack[32]; int top = 0; stack[top++] = cond;
    while (top > 0) {
        Expr *e = stack[--top];
        if (!e || e->kind != EXPR_BINARY) continue;
        if (e->as.binary_expr.op == TOKEN_KEYWORD_AND) {
            if (top < 30) { stack[top++] = e->as.binary_expr.left; stack[top++] = e->as.binary_expr.right; }
        } else if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) {
            Expr *lhs = e->as.binary_expr.left;
            if (lhs && lhs->type && lhs->type->kind == TYPE_POINTER && n_conds < MAX_PTR_CONDS)
                cond_ptrs[n_conds++] = e; // the whole `p in arr` expr
        }
    }
    if (n_conds == 0) return;

    // Strategy B: co-decrement — all ptrs unconditionally decremented
    bool all_unconditional = true;
    for (int i = 0; i < n_conds; i++) {
        Expr *lhs = cond_ptrs[i]->as.binary_expr.left;
        if (lhs->kind != EXPR_IDENTIFIER) { all_unconditional = false; break; }
        Id *pid = lhs->as.identifier_expr.id;
        if (!l3_is_unconditional_decrement(body, pid)) { all_unconditional = false; break; }
    }
    if (all_unconditional && n_conds >= 2) {
        // All pointers decrement every iteration → they stay in sync.
        // Mark lower bounds dead ONLY for pointers in MUTABLE arrays (output pointers).
        // Read-only input pointers (const arr) retain their lower-bound check as the
        // true termination condition — eliminating them would cause infinite loops.
        for (int i = 0; i < n_conds; i++) {
            Expr *rhs = cond_ptrs[i]->as.binary_expr.right;
            if (!rhs) continue;
            Type *arr_ty = rhs->type ? sema_unwrap_type(rhs->type) : NULL;
            // Mutable array param (var *T[]): its associated pointer is "output"
            // and its lower bound is implied by the input pointer's condition.
            bool is_mutable_arr = arr_ty && arr_ty->mode == MODE_MUTABLE;
            if (is_mutable_arr)
                cond_ptrs[i]->as.binary_expr.l3_lower_dead = true;
        }
        return;
    }

    // Strategy A: algebraic — for each ptr P, check k_P - sum(k_others) >= 0
    for (int i = 0; i < n_conds; i++) {
        Expr *lhs = cond_ptrs[i]->as.binary_expr.left;
        if (lhs->kind != EXPR_IDENTIFIER) continue;
        Id *pid = lhs->as.identifier_expr.id;

        // Find init_idx for this pointer
        Expr *my_idx = NULL;
        for (PtrInitIdxEntry *pie = sema_ptr_init_idx; pie; pie = pie->next) {
            if (pie->ptr_id->length == pid->length &&
                memcmp(pie->ptr_id->name, pid->name, pid->length) == 0) {
                my_idx = pie->init_idx; break;
            }
        }
        if (!my_idx) continue;

        // Build sum of OTHER ptrs' init indices
        Expr *sum_others = NULL;
        for (int j = 0; j < n_conds; j++) {
            if (j == i) continue;
            Expr *other_lhs = cond_ptrs[j]->as.binary_expr.left;
            if (other_lhs->kind != EXPR_IDENTIFIER) continue;
            Id *opid = other_lhs->as.identifier_expr.id;
            Expr *other_idx = NULL;
            for (PtrInitIdxEntry *pie = sema_ptr_init_idx; pie; pie = pie->next) {
                if (pie->ptr_id->length == opid->length &&
                    memcmp(pie->ptr_id->name, opid->name, opid->length) == 0) {
                    other_idx = pie->init_idx; break;
                }
            }
            if (!other_idx) { sum_others = NULL; break; } // can't build sum
            if (!sum_others) sum_others = other_idx;
            else sum_others = expr_binary(sema_arena, TOKEN_PLUS, sum_others, other_idx);
        }
        // Single-pointer loop: the lower bound is the termination condition itself — never dead.
        // Multi-pointer: skip if we couldn't build sum_others for this pointer.
        if (!sum_others) continue;

        // Evaluate diff = my_idx - sum_others
        Expr *diff_expr = expr_binary(sema_arena, TOKEN_MINUS, my_idx, sum_others);
        Range diff = sema_eval_range(diff_expr, sema_ranges);
        if (diff.known && diff.min >= 0) {
            // Lower bound provably dead: ptr.offset >= other_offsets_sum >= 0
            cond_ptrs[i]->as.binary_expr.l3_lower_dead = true;
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
                                        // init_idx: look up from sema_ptr_init_idx
                                        entry->init_idx = NULL;
                                        for (PtrInitIdxEntry *pie = sema_ptr_init_idx; pie; pie = pie->next) {
                                            if (pie->ptr_id->length == pid->length &&
                                                memcmp(pie->ptr_id->name, pid->name, pid->length) == 0) {
                                                entry->init_idx = pie->init_idx; break;
                                            }
                                        }
                                        // unconditional: decrement only at top level?
                                        entry->unconditional = l3_is_unconditional_decrement(body, pid);
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
        case EXPR_BINARY:
            // Structural equality of compound expressions (e.g. `i + 15`), so an
            // in-guard `(i + 15) in arr` matches the access `arr[i + 15]`.
            return a->as.binary_expr.op == b->as.binary_expr.op &&
                   expr_struct_equal(a->as.binary_expr.left,  b->as.binary_expr.left) &&
                   expr_struct_equal(a->as.binary_expr.right, b->as.binary_expr.right);
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
// The while body currently under termination analysis — set by
// sema_verify_bounded_while so assignment_direction can ask "is this step
// expression loop-invariant?" (no variable it reads is assigned in the body).
static StmtList *g_term_loop_body = NULL;

// A step expression is loop-invariant if every identifier it reads is unmodified
// anywhere in the loop body: then its VRA range at loop entry is valid on EVERY
// iteration, which is what makes a VRA-derived step sign sound. Conservative:
// only literals, identifiers, and arithmetic over them count (calls, indexing,
// and member access are treated as non-invariant).
static bool expr_is_loop_invariant(Expr *e, StmtList *body) {
    if (!e) return true;
    switch (e->kind) {
        case EXPR_LITERAL:    return true;
        case EXPR_IDENTIFIER: return !sema_body_writes_id(body, e->as.identifier_expr.id);
        case EXPR_BINARY:     return expr_is_loop_invariant(e->as.binary_expr.left,  body) &&
                                     expr_is_loop_invariant(e->as.binary_expr.right, body);
        case EXPR_UNARY:      return expr_is_loop_invariant(e->as.unary_expr.right, body);
        default:              return false;
    }
}

// Direction a measure variable moves on `target = rhs`: +1 increases, -1
// decreases, 0 unknown. The caller multiplies by the variable's polarity to
// decide whether the MEASURE decreased.
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

    // VRA-backed step: `target = target ± E` (or `E + target`) where E is a
    // LOOP-INVARIANT expression VRA proves strictly signed (|E| >= 1). Sound:
    // an invariant E holds its entry range every iteration, and a strict step
    // forces strict monotonicity. Extends the frontier from constant steps to
    // variable/computed steps like `i = i + k` (k proven >= 1), which the
    // syntactic patterns above reject. A step whose range straddles 0 (could be
    // a no-op) or that reads a body-mutated variable falls through to 0.
    if (op == TOKEN_PLUS || op == TOKEN_MINUS) {
        Expr *step = NULL;
        if (expr_struct_equal(left, target))                        step = right;
        else if (op == TOKEN_PLUS && expr_struct_equal(right, target)) step = left;
        if (step && g_term_loop_body && expr_is_loop_invariant(step, g_term_loop_body)) {
            Range r = sema_eval_range(step, sema_ranges);
            if (r.known && r.min >= 1) return (op == TOKEN_PLUS) ? +1 : -1;
            if (r.known && r.max <= -1) return (op == TOKEN_PLUS) ? -1 : +1;
        }
    }

    return 0;
}

// Scan a block: does the measure strictly decrease on EVERY path?
// Returns: +1 if the block guarantees a decrease on all paths & none increase,
//          -1 if some path changes the measure the wrong way (conflict),
//           0 if no conflict but not every path is guaranteed to decrease.
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
                // Soundness: the measure must decrease on EVERY path, so an `if`
                // guarantees a decrease only if BOTH branches decrease. A missing
                // `else` scans to 0, so a one-armed `if` never guarantees it.
                int rt = measure_scan_body(s->as.if_stmt.then_body, vars, nvar);
                if (rt < 0) return -1;
                int re = measure_scan_body(s->as.if_stmt.else_branch, vars, nvar);
                if (re < 0) return -1;
                if (rt > 0 && re > 0) found_decrease = true;
                break;
            }
            case STMT_WHILE: {
                // A nested loop may run zero times: it cannot GUARANTEE a decrease
                // of the outer measure, only conflict (increase).
                int r = measure_scan_body(s->as.while_stmt.body, vars, nvar);
                if (r < 0) return -1;
                break;
            }
            case STMT_FOR: {
                int r = measure_scan_body(s->as.for_stmt.body, vars, nvar);
                if (r < 0) return -1;
                break;
            }
            case STMT_MATCH: {
                // Match is exhaustive: exactly one case runs, so it guarantees a
                // decrease only if EVERY case decreases.
                bool all_dec = true, any = false;
                for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                    any = true;
                    int r = measure_scan_body(c->body, vars, nvar);
                    if (r < 0) return -1;
                    if (r <= 0) all_dec = false;
                }
                if (any && all_dec) found_decrease = true;
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

    g_term_loop_body = s->as.while_stmt.body;   // for invariant-step VRA queries
    int result = measure_scan_body(s->as.while_stmt.body, vars, nvar);
    g_term_loop_body = NULL;
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

// Is e an identifier whose type is a single-marker union (`T | m`)? Only those
// get `if r` narrowing: `if (r)` soundly excludes one sentinel; a multi-marker
// union must be discriminated with `case` (its markers sit at several sentinels).
static bool nn_is_single_marker_var(Expr *e) {
    if (!e || e->kind != EXPR_IDENTIFIER || !e->type) return false;
    Type *t = e->type;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    Decl *U = find_union_enum(t);
    if (U) {
        int markers = 0;
        for (Variant *v = U->as.enum_decl.variants; v; v = v->next)
            if (!v->fields) markers++;
        return markers == 1;
    }
    return false;
}

// Push the variables a condition proves NON-nil for the branch being entered.
// `negated` = we are entering the else-branch (the condition is false there).
//   `if x` / `if x != nil`  -> x present in THEN.
//   `if x == nil { } else`  -> x present in ELSE (negated).
//   `and`/`or` chains combined via De Morgan.
static void sema_push_narrows(Expr *cond, bool negated) {
    if (!cond) return;
    if (cond->kind == EXPR_IDENTIFIER) {
        if (!negated && nn_is_single_marker_var(cond)) {
            NarrowEntry *e = arena_push_aligned(sema_arena, NarrowEntry);
            e->var = cond; e->next = sema_narrows; sema_narrows = e;
        }
        return;
    }
    if (cond->kind != EXPR_BINARY) return;
    TokenKind op = cond->as.binary_expr.op;
    Expr *l = cond->as.binary_expr.left, *r = cond->as.binary_expr.right;
    if (op == TOKEN_KEYWORD_AND && !negated) {
        sema_push_narrows(l, false); sema_push_narrows(r, false);
    } else if (op == TOKEN_KEYWORD_OR && negated) {          // !(a or b) = !a and !b
        sema_push_narrows(l, true); sema_push_narrows(r, true);
    }
}

static bool sema_is_narrowed(Expr *e) {
    if (!e || e->kind != EXPR_IDENTIFIER) return false;
    for (NarrowEntry *n = sema_narrows; n; n = n->next)
        if (expr_struct_equal(e, n->var)) return true;
    return false;
}

static void sema_push_in_guards(Expr *cond) {
    if (!cond || cond->kind != EXPR_BINARY) return;
    if (cond->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;
        InGuardEntry *e = arena_push_aligned(sema_arena, InGuardEntry);
        e->index = lhs;
        e->container = rhs;
        e->is_ptr_guard = (lhs && lhs->type && lhs->type->kind == TYPE_POINTER);
        e->is_backward_guard = false;
        e->next = sema_in_guards;
        sema_in_guards = e;
    } else if (cond->as.binary_expr.op == TOKEN_ANGLE_BRACKET_RIGHT) {
        // `p > arr_ptr` — backward pointer guard: proves p-1 >= arr_ptr
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;
        if (lhs && lhs->type && lhs->type->kind == TYPE_POINTER) {
            InGuardEntry *e = arena_push_aligned(sema_arena, InGuardEntry);
            e->index = lhs;
            e->container = rhs;
            e->is_ptr_guard = true;
            e->is_backward_guard = true;
            e->next = sema_in_guards;
            sema_in_guards = e;
        }
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

// True iff `e` syntactically references the variable `var` anywhere within it.
static bool expr_references_id(Expr *e, Id *var) {
    if (!e || !var) return false;
    switch (e->kind) {
        case EXPR_IDENTIFIER:
            return e->as.identifier_expr.id &&
                   e->as.identifier_expr.id->length == var->length &&
                   strncmp(e->as.identifier_expr.id->name, var->name, (size_t)var->length) == 0;
        case EXPR_BINARY:
            return expr_references_id(e->as.binary_expr.left,  var) ||
                   expr_references_id(e->as.binary_expr.right, var);
        case EXPR_UNARY:
            return expr_references_id(e->as.unary_expr.right, var);
        case EXPR_MEMBER:
            return expr_references_id(e->as.member_expr.target, var);
        case EXPR_INDEX:
            return expr_references_id(e->as.index_expr.target, var) ||
                   expr_references_id(e->as.index_expr.index,  var);
        case EXPR_CAST:
            return expr_references_id(e->as.cast_expr.expr, var);
        default:
            return false;
    }
}

// SOUNDNESS: mutating `var` makes any in-guard whose index OR container references
// it stale — `while i in a { i = huge; a[i] }` must NOT keep the `i in a` guard and
// read out of bounds. Unlink every such guard from the active list.
static void sema_invalidate_in_guards(Id *var) {
    InGuardEntry **pp = &sema_in_guards;
    while (*pp) {
        InGuardEntry *e = *pp;
        if (expr_references_id(e->index, var) || expr_references_id(e->container, var))
            *pp = e->next;              // unlink the stale guard
        else
            pp = &e->next;
    }
}

// Does `body` (recursively) assign to any identifier that `guard` references? Used
// to invalidate OUTER guards after a NESTED branch that may mutate the guarded
// variable — the same-scope unlink above is undone by a branch's scope-restore.
static bool stmtlist_assigns_referenced(struct StmtList *body, Expr *guard);
static bool stmt_assigns_referenced(Stmt *s, Expr *guard) {
    if (!s || !guard) return false;
    switch (s->kind) {
        case STMT_ASSIGN:
            return s->as.assign_stmt.target &&
                   s->as.assign_stmt.target->kind == EXPR_IDENTIFIER &&
                   expr_references_id(guard, s->as.assign_stmt.target->as.identifier_expr.id);
        case STMT_IF:
            return stmtlist_assigns_referenced(s->as.if_stmt.then_body, guard) ||
                   stmtlist_assigns_referenced(s->as.if_stmt.else_branch, guard);
        case STMT_WHILE:
            return stmtlist_assigns_referenced(s->as.while_stmt.body, guard);
        case STMT_FOR:
            return stmtlist_assigns_referenced(s->as.for_stmt.body, guard);
        case STMT_UNSAFE:
            return stmtlist_assigns_referenced(s->as.unsafe_stmt.body, guard);
        default:
            return false;
    }
}
static bool stmtlist_assigns_referenced(struct StmtList *body, Expr *guard) {
    for (struct StmtList *b = body; b; b = b->next)
        if (stmt_assigns_referenced(b->stmt, guard)) return true;
    return false;
}
// Invalidate every in-guard a nested `body` may have staled by assigning a variable
// the guard references. Called after a branch/loop body's scope is restored.
static void sema_invalidate_guards_for_body(struct StmtList *body) {
    InGuardEntry **pp = &sema_in_guards;
    while (*pp) {
        InGuardEntry *e = *pp;
        if (stmtlist_assigns_referenced(body, e->index) ||
            stmtlist_assigns_referenced(body, e->container))
            *pp = e->next;
        else
            pp = &e->next;
    }
}

// SOUNDNESS (constraint counterpart of the in-guard unlink above): a relational
// constraint `v1 - v2 <= k` — e.g. `i < n` recorded from a `while i < n` guard —
// goes STALE the instant `v1` or `v2` is reassigned. The bounds prover chains
// such a constraint with the endpoint's range to prove a fixed-array index, so a
// stale `i < n` after `i = i +% 1000` would wrongly prove an out-of-range `a[i]`.
// Unlink every constraint mentioning the mutated variable (matched by name, as
// the constraint table stores non-interned Ids). Same-scope mutation site.
static void sema_invalidate_constraints(Id *var) {
    if (!sema_ranges || !var) return;
    ConstraintEntry **pp = &sema_ranges->constraints;
    while (*pp) {
        ConstraintEntry *c = *pp;
        bool hits =
            (c->v1 && c->v1->length == var->length &&
             strncmp(c->v1->name, var->name, (size_t)var->length) == 0) ||
            (c->v2 && c->v2->length == var->length &&
             strncmp(c->v2->name, var->name, (size_t)var->length) == 0);
        if (hits) *pp = c->next;   // unlink the stale constraint
        else      pp = &c->next;
    }
}
// Invalidate constraints a nested `body` may have staled by assigning either
// endpoint (mirrors sema_invalidate_guards_for_body; a branch's scope-restore
// would otherwise re-expose a constraint the branch invalidated).
static void sema_invalidate_constraints_for_body(struct StmtList *body) {
    if (!sema_ranges) return;
    Expr probe; memset(&probe, 0, sizeof probe); probe.kind = EXPR_IDENTIFIER;
    ConstraintEntry **pp = &sema_ranges->constraints;
    while (*pp) {
        ConstraintEntry *c = *pp;
        probe.as.identifier_expr.id = c->v1;
        bool staled = c->v1 && stmtlist_assigns_referenced(body, &probe);
        if (!staled && c->v2) {
            probe.as.identifier_expr.id = c->v2;
            staled = stmtlist_assigns_referenced(body, &probe);
        }
        if (staled) *pp = c->next;
        else        pp = &c->next;
    }
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
                // Strip MODE_MUTABLE from inferred type: `var x = var_param`
                // gives x the VALUE type, not the reference type. Also strip the
                // on-type refinement: a variable's type is its declared capacity,
                // not the initializer's interval (`var i = 1` is an i32, not {ν=1}
                // — else a later `i = 9` would "violate" it). The init value's
                // range still travels via the range table.
                Type *inferred = s->as.var_stmt.expr->type;
                if (inferred && (inferred->mode == MODE_MUTABLE || inferred->refine.known)) {
                    Type *stripped = arena_push_aligned(sema_arena, Type);
                    *stripped = *inferred;
                    if (stripped->mode == MODE_MUTABLE) stripped->mode = MODE_SHARED;
                    stripped->refine.known = false;
                    inferred = stripped;
                }
                s->as.var_stmt.type = inferred;
            }

            // F4 (spec audit): full type-compatibility enforcement at
            // STMT_VAR is non-trivial — requires refinement-alias
            // resolution + float literal polymorphism + integer literal
            // polymorphism. Deferred. See internal/ai_analysis/
            // spec_audit_2026_05_14.md §F4.

            sema_union_coerce(&s->as.var_stmt.expr, s->as.var_stmt.type);  // `T | markers` construction
            if (sema_ranges && s->as.var_stmt.expr) {
                Range r = sema_eval_range(s->as.var_stmt.expr, sema_ranges);
                // Q-002 Phase 5: overflow-at-boundary check (var declaration).
                if (s->as.var_stmt.type) {
                    char vname_buf[128];
                    int vn_len = s->as.var_stmt.name->length < 127
                                 ? (int)s->as.var_stmt.name->length : 127;
                    memcpy(vname_buf, s->as.var_stmt.name->name, vn_len);
                    vname_buf[vn_len] = '\0';
                    check_conversion(s->as.var_stmt.expr->type, s->as.var_stmt.type, r,
                        s->as.var_stmt.expr, s->line, s->col,
                        "initialization of variable", vname_buf);
                    // Reject multidimensional fixed arrays (nested `T[N][M]`): the
                    // backend emits an undeclared `Fixed_` type name (broken C), and
                    // the dimension nesting is reversed vs C. Use a flat `T[m*n]`
                    // indexed `a[i*n + j]`.
                    if (s->as.var_stmt.type->kind == TYPE_ARRAY &&
                        s->as.var_stmt.type->element_type &&
                        s->as.var_stmt.type->element_type->kind == TYPE_ARRAY) {
                        fprintf(stderr, "[E100] Error Ln %li, Col %li: multidimensional arrays are "
                            "not supported — use a flat array 'T[m*n]' indexed 'a[i*n + j]'.\n",
                            (long)s->line, (long)s->col);
                        diagnostic_show_line(s->line, s->col);
                        exit(1);
                    }
                    // P2/S4: an array literal's element count must match a fixed
                    // array length (was a gap: u8[3] = [1,2] and u8[2] = [1,2,3]
                    // compiled, emitting bad C with missing/excess initializers).
                    if (s->as.var_stmt.type->kind == TYPE_ARRAY
                        && s->as.var_stmt.type->array_len >= 0
                        && s->as.var_stmt.expr->kind == EXPR_ARRAY_LITERAL) {
                        long count = 0;
                        for (ExprList *el = s->as.var_stmt.expr->as.array_literal_expr.elements;
                             el; el = el->next) count++;
                        if (count != (long)s->as.var_stmt.type->array_len) {
                            fprintf(stderr, "[E012] Error Ln %li, Col %li: array literal has %ld "
                                "element(s) but '%s' has fixed length %lld.\n",
                                (long)s->line, (long)s->col, count, vname_buf,
                                (long long)s->as.var_stmt.type->array_len);
                            diagnostic_show_line(s->line, s->col);
                            exit(1);
                        }
                    }
                    // P2/S3: an array literal's literal elements must fit the target
                    // array's element type (was a gap: u8[3] = [1,2,300] compiled).
                    if (s->as.var_stmt.type->kind == TYPE_ARRAY
                        && s->as.var_stmt.type->element_type
                        && is_integer_type(s->as.var_stmt.type->element_type)
                        && s->as.var_stmt.expr->kind == EXPR_ARRAY_LITERAL) {
                        Type *et = s->as.var_stmt.type->element_type;
                        for (ExprList *el = s->as.var_stmt.expr->as.array_literal_expr.elements;
                             el; el = el->next) {
                            if (el->expr) {
                                // Literal → exact point range; otherwise the VRA range
                                // (unknown → check skips, conservative).
                                Range er = (el->expr->kind == EXPR_LITERAL)
                                    ? (Range){ el->expr->as.literal_expr.value,
                                               el->expr->as.literal_expr.value, true }
                                    : sema_eval_range(el->expr, sema_ranges);
                                check_conversion(el->expr->type, et, er, el->expr,
                                    el->expr->line, el->expr->col, "array element", vname_buf);
                            }
                        }
                    }
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

                // var x = y ± k  (y a plain local/param identifier, NOT .len):
                // seed the difference constraint x = y ± k so a later `y < x` /
                // `x > y` is provable. Mirrors the x.len case above and the
                // STMT_ASSIGN path (`x = y + c`); without it `var x = y + 1` could
                // not discharge a `b < a` precondition (range_linear_pass proved
                // only via the general-branch fail-open). Sound: a var-decl is a
                // fresh name (no stale constraint on it), and a later reassignment
                // of x or y invalidates the constraint (sema_invalidate_constraints).
                {
                    Expr *init = s->as.var_stmt.expr;
                    Id   *n_id = s->as.var_stmt.name;
                    Id      *vref   = NULL;
                    int64_t  vdelta = 0;
                    if (init && init->kind == EXPR_BINARY &&
                        (init->as.binary_expr.op == TOKEN_PLUS ||
                         init->as.binary_expr.op == TOKEN_MINUS) &&
                        init->as.binary_expr.left &&
                        init->as.binary_expr.left->kind == EXPR_IDENTIFIER &&
                        init->as.binary_expr.right &&
                        init->as.binary_expr.right->kind == EXPR_LITERAL) {
                        /* var x = y + k  /  var x = y - k */
                        vref   = init->as.binary_expr.left->as.identifier_expr.id;
                        int64_t k = (int64_t)init->as.binary_expr.right->as.literal_expr.value;
                        vdelta = (init->as.binary_expr.op == TOKEN_PLUS) ? k : -k;
                    } else if (init && init->kind == EXPR_BINARY &&
                               init->as.binary_expr.op == TOKEN_PLUS &&
                               init->as.binary_expr.left &&
                               init->as.binary_expr.left->kind == EXPR_LITERAL &&
                               init->as.binary_expr.right &&
                               init->as.binary_expr.right->kind == EXPR_IDENTIFIER) {
                        /* var x = k + y */
                        vref   = init->as.binary_expr.right->as.identifier_expr.id;
                        vdelta = (int64_t)init->as.binary_expr.left->as.literal_expr.value;
                    } else if (init && init->kind == EXPR_IDENTIFIER) {
                        /* var x = y */
                        vref = init->as.identifier_expr.id; vdelta = 0;
                    }
                    if (vref && n_id && sema_ranges &&
                        !(vref->length == n_id->length &&
                          memcmp(vref->name, n_id->name, (size_t)vref->length) == 0)) {
                        /* x = y + delta  ↔  x - y ≤ delta ; y - x ≤ -delta */
                        constraint_add(sema_ranges, n_id, vref,  vdelta);
                        constraint_add(sema_ranges, vref, n_id, -vdelta);
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

            // Auto-infer pointer in-guard from `&arr[k]` initializer.
            // `var p = &arr[k]` carries the same safety guarantee as
            // `var p in arr = &arr[k]` — the init bounds check already proved
            // p is within arr at declaration time.
            if (!s->as.var_stmt.in_expr &&
                s->as.var_stmt.type && s->as.var_stmt.type->kind == TYPE_POINTER &&
                s->as.var_stmt.expr && s->as.var_stmt.expr->kind == EXPR_ADDR &&
                s->as.var_stmt.expr->as.addr_expr.expr &&
                s->as.var_stmt.expr->as.addr_expr.expr->kind == EXPR_INDEX) {
                s->as.var_stmt.in_expr =
                    s->as.var_stmt.expr->as.addr_expr.expr->as.index_expr.target;
            }

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

                // L3 lower bound: record init index from &arr[k] initializer
                Expr *init = s->as.var_stmt.expr;
                if (init && init->kind == EXPR_ADDR &&
                    init->as.addr_expr.expr &&
                    init->as.addr_expr.expr->kind == EXPR_INDEX) {
                    Expr *idx = init->as.addr_expr.expr->as.index_expr.index;
                    PtrInitIdxEntry *pie = arena_push_aligned(sema_arena, PtrInitIdxEntry);
                    pie->ptr_id = resolved_id;
                    pie->init_idx = idx;
                    pie->next = sema_ptr_init_idx;
                    sema_ptr_init_idx = pie;
                }
            }
            break;
        case STMT_IF: {
            sema_infer_expr(s->as.if_stmt.cond);

            // Save state
            RangeEntry *old_head = sema_ranges->head;
            ConstraintEntry *old_constraints = sema_ranges->constraints;
            InGuardEntry *old_guards = sema_in_guards;
            NarrowEntry *old_narrows = sema_narrows;

            // Apply condition for THEN branch
            sema_apply_constraint(s->as.if_stmt.cond, sema_ranges);
            sema_push_in_guards(s->as.if_stmt.cond);
            sema_push_narrows(s->as.if_stmt.cond, false);   // ?T narrows to T in THEN

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
            sema_narrows = old_narrows;

            // Apply negated condition for ELSE branch (no in-guards — negated 'in' proves nothing)
            sema_apply_negated_constraint(s->as.if_stmt.cond, sema_ranges);
            sema_push_narrows(s->as.if_stmt.cond, true);    // else: x present when cond was `x == nil`

            sema_push_scope();
            for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next)
                walk_stmt(b->stmt);
            sema_pop_scope();
            sema_narrows = old_narrows;

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
                // Nullable guard clause: `if x == nil { return }` — the trailing
                // code runs only when the cond was false, so x is present below.
                sema_push_narrows(s->as.if_stmt.cond, true);
            } else {
                // Normal: restore state to what it was before the if.
                sema_ranges->head = old_head;
                sema_ranges->constraints = old_constraints;
            }
            // SOUNDNESS: either branch may have mutated a guarded variable, and the
            // scope restore above re-added the outer guards — re-invalidate any guard
            // whose variable a branch assigns (`while i in a { if c { i = huge } a[i] }`).
            sema_invalidate_guards_for_body(s->as.if_stmt.then_body);
            sema_invalidate_guards_for_body(s->as.if_stmt.else_branch);
            sema_invalidate_constraints_for_body(s->as.if_stmt.then_body);
            sema_invalidate_constraints_for_body(s->as.if_stmt.else_branch);
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
                // VRA: seed the counter's lower bound whenever `start` is known,
                // even if `end` is not constant (`for i in 0..n` / `0..a.len`).
                // Previously the whole range was dropped unless BOTH ends were
                // known, discarding the always-true `i >= start` premise that
                // every arithmetic-index / reverse-iteration proof needs.
                // Safe: check_value_fits_type treats max within 4096 of INT64_MAX
                // as unbounded, so no false overflow; the counter is overwritten
                // post-loop.
                if (start_range.known) {
                    int64_t hi = end_range.known ? end_range.max - 1 : INT64_MAX;
                    range_set(sema_ranges, iter_var, range_make(start_range.min, hi));
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

            // SOUNDNESS: the loop body may have mutated an OUTER-guarded variable —
            // invalidate any guard whose variable the body assigns.
            sema_invalidate_guards_for_body(s->as.for_stmt.body);
            sema_invalidate_constraints_for_body(s->as.for_stmt.body);

            // Widen modified variables AFTER body
            if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);

            // S15 (VRA L3): post-loop affine recap.
            // For each affine var, compute final range from init + step * iter_count.
            // Iter count = end - start (loop iterates exactly that many times).
            //
            // P0: this exact recap is SOUND only when the loop body cannot exit
            // early or skip iterations, and steps each var exactly once. A
            // break/continue/return, or the same var stepped twice at top level,
            // makes the real final value differ from the recap — which then lets
            // the bounds checker "prove" an out-of-bounds index (ASan-confirmed).
            // When unsafe, discard the recap: sema_widen_loop already set a sound
            // (wider) range for these vars above.
            bool recap_unsafe = loop_body_defeats_affine_recap(s->as.for_stmt.body);
            for (int i = 0; !recap_unsafe && i < n_affine; i++)      // same var stepped twice
                for (int j = i + 1; j < n_affine; j++)
                    if (affine_vars[i] && affine_vars[j] &&
                        affine_vars[i]->length == affine_vars[j]->length &&
                        strncmp(affine_vars[i]->name, affine_vars[j]->name,
                                affine_vars[i]->length) == 0) { recap_unsafe = true; break; }
            if (!recap_unsafe && sema_ranges && n_affine > 0 && start_range.known && end_range.known) {
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

            // Preserve loop index value at exit. A clean `for i in start..end`
            // exits with i == end; but with a break the index can be anywhere in
            // [start, end], so widen it conservatively rather than claim `end`.
            if (sema_ranges && iter_var && end_range.known) {
                if (recap_unsafe && start_range.known)
                    range_set(sema_ranges, iter_var, range_make(start_range.min, end_range.max));
                else
                    range_set(sema_ranges, iter_var, end_range);
            }
            #undef MAX_AFFINE
            break;
        }
        case STMT_WHILE:
            sema_infer_expr(s->as.while_stmt.cond);

            // Auto-infer `decreasing` when measure is absent and all conditions are
            // pointer-in-arr guards (p in arr) or backward pointer guards (p > arr_ptr).
            // Synthesizes measure as sum of (p - &arr[0]) or (p - arr_ptr).
            if (!s->as.while_stmt.measure && current_function_decl &&
                current_function_decl->kind == DECL_FUNCTION) {
                // Collect pointer conditions from AND-chain
                Expr *ptr_exprs[8]; Expr *arr_exprs[8];
                bool  is_backward[8]; int n_ptrs = 0;
                {
                    Expr *stk[16]; int top = 0; stk[top++] = s->as.while_stmt.cond;
                    while (top > 0) {
                        Expr *ce = stk[--top];
                        if (!ce || ce->kind != EXPR_BINARY) continue;
                        if (ce->as.binary_expr.op == TOKEN_KEYWORD_AND && top < 14) {
                            stk[top++] = ce->as.binary_expr.left;
                            stk[top++] = ce->as.binary_expr.right;
                        } else if (ce->as.binary_expr.op == TOKEN_KEYWORD_IN) {
                            Expr *lhs = ce->as.binary_expr.left;
                            if (lhs && lhs->type && lhs->type->kind == TYPE_POINTER && n_ptrs < 8) {
                                ptr_exprs[n_ptrs] = lhs;
                                arr_exprs[n_ptrs] = ce->as.binary_expr.right;
                                is_backward[n_ptrs] = false;
                                n_ptrs++;
                            }
                        } else if (ce->as.binary_expr.op == TOKEN_ANGLE_BRACKET_RIGHT) {
                            // p > arr_ptr — backward guard
                            Expr *lhs = ce->as.binary_expr.left;
                            Expr *rhs = ce->as.binary_expr.right;
                            if (lhs && lhs->type && lhs->type->kind == TYPE_POINTER && n_ptrs < 8) {
                                ptr_exprs[n_ptrs] = lhs;
                                arr_exprs[n_ptrs] = rhs;
                                is_backward[n_ptrs] = true;
                                n_ptrs++;
                            }
                        }
                    }
                }
                // Build measure from decreasing pointers
                Expr *synth_measure = NULL;
                for (int i = 0; i < n_ptrs; i++) {
                    Expr *ptr = ptr_exprs[i];
                    if (ptr->kind != EXPR_IDENTIFIER) continue;
                    Id *pid = ptr->as.identifier_expr.id;
                    if (!body_has_ptr_decrement(s->as.while_stmt.body, pid)) continue;
                    // Build the subtracted base: &arr[0] for forward, arr_ptr for backward
                    Expr *base_ptr;
                    if (is_backward[i]) {
                        // arr_exprs[i] is already a pointer — use directly as `p - arr_ptr`
                        base_ptr = arr_exprs[i];
                    } else {
                        // Build &arr[0]
                        Expr *zero = arena_push_aligned(sema_arena, Expr);
                        memset(zero, 0, sizeof(Expr));
                        zero->kind = EXPR_LITERAL; zero->line = s->line; zero->col = s->col;
                        Expr *aidx = arena_push_aligned(sema_arena, Expr);
                        memset(aidx, 0, sizeof(Expr));
                        aidx->kind = EXPR_INDEX;
                        aidx->as.index_expr.target = arr_exprs[i];
                        aidx->as.index_expr.index = zero;
                        aidx->line = s->line; aidx->col = s->col;
                        Expr *aaddr = arena_push_aligned(sema_arena, Expr);
                        memset(aaddr, 0, sizeof(Expr));
                        aaddr->kind = EXPR_ADDR;
                        aaddr->as.addr_expr.expr = aidx;
                        aaddr->line = s->line; aaddr->col = s->col;
                        base_ptr = aaddr;
                    }
                    // p - base_ptr
                    Expr *term = arena_push_aligned(sema_arena, Expr);
                    memset(term, 0, sizeof(Expr));
                    term->kind = EXPR_BINARY;
                    term->as.binary_expr.op = TOKEN_MINUS;
                    term->as.binary_expr.left = ptr;
                    term->as.binary_expr.right = base_ptr;
                    term->line = s->line; term->col = s->col;
                    if (!synth_measure) {
                        synth_measure = term;
                    } else {
                        Expr *plus = arena_push_aligned(sema_arena, Expr);
                        memset(plus, 0, sizeof(Expr));
                        plus->kind = EXPR_BINARY;
                        plus->as.binary_expr.op = TOKEN_PLUS;
                        plus->as.binary_expr.left = synth_measure;
                        plus->as.binary_expr.right = term;
                        plus->line = s->line; plus->col = s->col;
                        synth_measure = plus;
                    }
                }
                if (synth_measure) {
                    s->as.while_stmt.measure = synth_measure;
                } else {
                    // No pointer decrements found: emit E011
                    fprintf(stderr, "[E011] Error Ln %li, Col %li: 'while' loops without a termination "
                            "measure are not allowed in pure function '%.*s'. "
                            "Add 'decreasing <measure>' or use 'proc'.\n",
                            s->line, s->col,
                            (int)current_function_decl->as.function_decl.name->length,
                            current_function_decl->as.function_decl.name->name);
                    diagnostic_show_line(s->line, s->col);
                    exit(1);
                }
            }

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
            if (sema_ranges)
                l3_mark_dead_lower_bounds(s->as.while_stmt.cond,
                                          s->as.while_stmt.body);

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

            // SOUNDNESS: the loop body may have mutated an OUTER-guarded variable —
            // invalidate any guard whose variable the body assigns.
            sema_invalidate_guards_for_body(s->as.while_stmt.body);
            sema_invalidate_constraints_for_body(s->as.while_stmt.body);
            break;
        case STMT_ASSIGN:
            sema_infer_expr(s->as.assign_stmt.expr);
            sema_infer_expr(s->as.assign_stmt.target);

            // COHERENCE (§2.9): a raw pointer `*T` is the unsafe/interop tool — it
            // is not borrow-checked, and mutating a struct field through it in SAFE
            // code silently emitted `const T*` C that gcc rejects. Writing a field
            // through a pointer outside `unsafe` is rejected here, pointing at the
            // idiomatic in-place mutation: a `var` (mutable borrow) parameter. (Index
            // writes `out[i]=…` through sized array/slice OUTPUT params stay valid —
            // only field writes `p.f=…` through a `*T` pointer are caught.)
            if (!sema_in_unsafe_block &&
                s->as.assign_stmt.target->kind == EXPR_MEMBER) {
                Expr *base = s->as.assign_stmt.target->as.member_expr.target;
                if (base && base->type && base->type->kind == TYPE_POINTER) {
                    Id *bid = base->kind == EXPR_IDENTIFIER ? base->as.identifier_expr.id : NULL;
                    fprintf(stderr, "[E009] Error Ln %li, Col %li: cannot mutate a field through a raw pointer "
                            "`*T`%s%.*s%s in safe code — `*T` is the unsafe/interop tool and is not borrow-checked. "
                            "For in-place mutation take a `var` (mutable borrow) parameter, e.g. `proc f(var x T)`, "
                            "or wrap the write in an `unsafe` block.\n",
                            s->line, s->col,
                            bid ? " (`" : "", bid ? (int)bid->length : 0, bid ? bid->name : "", bid ? "`)" : "");
                    diagnostic_show_line(s->line, s->col);
                    exit(1);
                }
            }

            // SOUNDNESS: reassigning an identifier invalidates any in-guard that
            // references it (index or container), so a stale `i in a` cannot keep
            // proving `a[i]` after `i` has changed. The same applies to a relational
            // constraint (`i < n`): a stale one would let the bounds prover chain it
            // to prove an out-of-range `a[i]` after `i` is mutated.
            if (s->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
                sema_invalidate_in_guards(s->as.assign_stmt.target->as.identifier_expr.id);
                sema_invalidate_constraints(s->as.assign_stmt.target->as.identifier_expr.id);
            }

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
                                // G5: enforce the field's refinement constraints on
                                // REASSIGNMENT too (`c.pct = 200` must satisfy pct's
                                // `>= 0 and <= 100`), not just at construction.
                                if (sf->decl->as.variable_decl.constraints && sema_ranges &&
                                    !sema_in_unsafe_block) {
                                    Expr *frhs = s->as.assign_stmt.expr;
                                    Range r = (frhs && frhs->kind == EXPR_LITERAL)
                                        ? (Range){ frhs->as.literal_expr.value, frhs->as.literal_expr.value, true }
                                        : (frhs ? sema_eval_range(frhs, sema_ranges) : range_unknown());
                                    if (r.known) {
                                        for (ExprList *fc = sf->decl->as.variable_decl.constraints; fc; fc = fc->next) {
                                            if (!fc->expr || fc->expr->kind != EXPR_BINARY) continue;
                                            Expr *crhs = fc->expr->as.binary_expr.right;
                                            if (!crhs || crhs->kind != EXPR_LITERAL) continue;
                                            long long k = crhs->as.literal_expr.value;
                                            bool fits = true;
                                            switch (fc->expr->as.binary_expr.op) {
                                                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  fits = (r.max <= k); break;
                                                case TOKEN_ANGLE_BRACKET_LEFT:        fits = (r.max <  k); break;
                                                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: fits = (r.min >= k); break;
                                                case TOKEN_ANGLE_BRACKET_RIGHT:       fits = (r.min >  k); break;
                                                case TOKEN_EQUAL_EQUAL:               fits = (r.min == k && r.max == k); break;
                                                case TOKEN_BANG_EQUAL:                fits = (r.min > k || r.max < k); break;
                                                default: break;
                                            }
                                            if (!fits) {
                                                fprintf(stderr, "[E086] Error Ln %li, Col %li: assignment to field "
                                                    "'%.*s' value range [%lld, %lld] violates its refinement "
                                                    "constraint.\n", (long)s->line, (long)s->col,
                                                    (int)fn->length, fn->name, (long long)r.min, (long long)r.max);
                                                diagnostic_show_line(s->line, s->col);
                                                exit(1);
                                            }
                                        }
                                    }
                                }
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
                sema_union_coerce(&s->as.assign_stmt.expr, tgt->type);  // `T | markers` construction
                check_conversion(s->as.assign_stmt.expr->type, tgt->type, r,
                    s->as.assign_stmt.expr, s->line, s->col, ctx, label);
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
            sema_union_coerce(&s->as.return_stmt.value, current_return_type);  // `T | markers` construction
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
                check_conversion(s->as.return_stmt.value->type, current_return_type, r,
                    s->as.return_stmt.value, s->line, s->col, "return from function", buf);
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
                    // PROVE-OR-REJECT: the compiler TRUSTS a return refinement to
                    // narrow every caller's VRA, so the body must PROVABLY satisfy
                    // it. `result == 1` means proven; 0 (violated) or -1 (unknown,
                    // e.g. unbounded/wrapping) must both be rejected — a refinement
                    // it can't prove is a lie that defeats callers' bounds proofs.
                    if (result != 1 && !sema_in_unsafe_block) {
                        fprintf(stderr, "[E086] Error Ln %li, Col %li: return value cannot be proven "
                            "to satisfy the function's return refinement (range [%lld, %lld]). Constrain "
                            "the inputs or narrow the value so VRA can prove it.\n",
                            (long)s->line, (long)s->col,
                            (long long)ret_range.min, (long long)ret_range.max);
                        diagnostic_show_line(s->line, s->col);
                        exit(1);
                    }
                }
            }
            break;
        case STMT_MATCH: {
            sema_infer_expr(s->as.match_stmt.value);
            // Union scrutinee: reaching the value is the `else` arm, which narrows
            // the scrutinee to the value type — but only when every marker is
            // matched explicitly (else the `else` arm could BE a marker).
            bool else_narrows = union_else_covers_payload(s->as.match_stmt.value,
                                                          s->as.match_stmt.cases);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                sema_push_scope();
                NarrowEntry *old_narrows = sema_narrows;
                if (else_narrows && c->patterns == NULL) {   // the `else` arm
                    NarrowEntry *ne = arena_push_aligned(sema_arena, NarrowEntry);
                    ne->var = s->as.match_stmt.value;
                    ne->next = sema_narrows; sema_narrows = ne;
                }
                for (ExprList *p = c->patterns; p; p = p->next) {
                    sema_infer_expr(p->expr);
                }
                for (StmtList *b = c->body; b; b = b->next)
                    walk_stmt(b->stmt);
                sema_narrows = old_narrows;
                sema_pop_scope();
            }
            break;
        }
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
        case EXPR_TRY:  proc_w130_visit_expr(e->as.try_expr.operand); break;
        case EXPR_ELSE: proc_w130_visit_expr(e->as.else_expr.operand); proc_w130_visit_expr(e->as.else_expr.arm); break;
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

/*──────────────────────────────────────────────────────────────────╗
│ F3.3 effect inference (DIRECT effects; E1)                        │
│ Generalizes the W130 walker from a boolean "has effects" to the   │
│ EffectSet bitset. Unlike W130 it never early-returns — it unions  │
│ every effect. Transitive callee propagation is E2.                │
╚──────────────────────────────────────────────────────────────────*/
static EffectSet g_eff_acc;
static Decl     *g_eff_self;
static void eff_visit_expr(Expr *e);
static void eff_visit_stmt(Stmt *s);
static void eff_visit_list(StmtList *l) { for (; l; l = l->next) eff_visit_stmt(l->stmt); }
static EffectSet effect_full(Decl *d);   // E2: transitive, memoized

static void eff_visit_expr(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_CALL: {
            Expr *callee = e->as.call_expr.callee;
            if (callee && callee->kind == EXPR_IDENTIFIER && callee->as.identifier_expr.id &&
                callee->as.identifier_expr.id->length == 5 &&
                memcmp(callee->as.identifier_expr.id->name, "panic", 5) == 0) {
                g_eff_acc |= EFFECT_RAISES;
            } else if (callee && callee->decl) {
                // E2: a call carries the callee's WHOLE effect set (transitive).
                // effect_full resolves extern func -> {}, extern proc -> IO,
                // func/proc -> their inferred effects, and a recursion cycle ->
                // Diverge (via the in-progress guard).
                g_eff_acc |= effect_full(callee->decl);
            }
            eff_visit_expr(callee);
            for (ExprList *a = e->as.call_expr.args; a; a = a->next) eff_visit_expr(a->expr);
            break;
        }
        case EXPR_IDENTIFIER:
            if (e->is_global && e->decl && e->decl->kind == DECL_VARIABLE &&
                e->decl->as.variable_decl.is_mutable) g_eff_acc |= EFFECT_WRITE;
            break;
        case EXPR_BINARY: eff_visit_expr(e->as.binary_expr.left); eff_visit_expr(e->as.binary_expr.right); break;
        case EXPR_UNARY:  eff_visit_expr(e->as.unary_expr.right); break;
        case EXPR_MEMBER: eff_visit_expr(e->as.member_expr.target); break;
        case EXPR_INDEX:  eff_visit_expr(e->as.index_expr.target); eff_visit_expr(e->as.index_expr.index); break;
        case EXPR_RANGE:  eff_visit_expr(e->as.range_expr.start); eff_visit_expr(e->as.range_expr.end); break;
        case EXPR_MOVE:   eff_visit_expr(e->as.move_expr.expr); break;
        case EXPR_MUT:    eff_visit_expr(e->as.mut_expr.expr); break;
        case EXPR_CAST:   eff_visit_expr(e->as.cast_expr.expr); break;
        // try/else add no effect of their own (a recoverable error is a value,
        // not an effect — F3.4); just recurse so the operand/arm's effects count.
        case EXPR_TRY:    eff_visit_expr(e->as.try_expr.operand); break;
        case EXPR_ELSE:   eff_visit_expr(e->as.else_expr.operand); eff_visit_expr(e->as.else_expr.arm); break;
        case EXPR_MATCH:
            eff_visit_expr(e->as.match_expr.value);
            for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) eff_visit_expr(p->expr);
                eff_visit_expr(c->body);
            }
            break;
        case EXPR_ARRAY_LITERAL:
            for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next) eff_visit_expr(el->expr);
            break;
        default: break;
    }
}

static void eff_visit_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR: eff_visit_expr(s->as.var_stmt.expr); break;
        case STMT_ASSIGN:
            eff_visit_expr(s->as.assign_stmt.target);
            eff_visit_expr(s->as.assign_stmt.expr);
            break;
        case STMT_EXPR:   eff_visit_expr(s->as.expr_stmt.expr); break;
        case STMT_RETURN: eff_visit_expr(s->as.return_stmt.value); break;
        case STMT_IF:
            eff_visit_expr(s->as.if_stmt.cond);
            eff_visit_list(s->as.if_stmt.then_body);
            eff_visit_list(s->as.if_stmt.else_branch);
            break;
        case STMT_FOR:
            eff_visit_expr(s->as.for_stmt.iterable);
            eff_visit_list(s->as.for_stmt.body);
            break;
        case STMT_WHILE:
            if (!s->as.while_stmt.measure) g_eff_acc |= EFFECT_DIVERGE;  // unbounded while
            eff_visit_expr(s->as.while_stmt.cond);
            if (s->as.while_stmt.measure) eff_visit_expr(s->as.while_stmt.measure);
            eff_visit_list(s->as.while_stmt.body);
            break;
        case STMT_MATCH:
            eff_visit_expr(s->as.match_stmt.value);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) eff_visit_list(c->body);
            break;
        case STMT_UNSAFE: eff_visit_list(s->as.unsafe_stmt.body); break;
        case STMT_DEFER:  eff_visit_stmt(s->as.defer_stmt.stmt); break;
        default: break;
    }
}

// E2: the TRANSITIVE effect set of a function — its direct effects unioned with
// every callee's effects — memoized on the decl. An `extern func` is trusted pure
// ({}), an `extern proc` is opaque (IO), and a recursion cycle yields Diverge via
// the in-progress guard. EFFECT_WRITE is reserved for mutation of mutable GLOBAL
// state (a hidden side effect); mutation of a `var` parameter is NOT an effect —
// it is declared in the signature, exclusive, and referentially transparent
// (spec §12), so a `func` that only mutates a var param is pure & total.
static EffectSet effect_full(Decl *d) {
    if (!d) return 0;
    if (d->kind == DECL_EXTERN_FUNCTION) return 0;             // trusted pure (I-016)
    if (d->kind == DECL_EXTERN_PROCEDURE) return EFFECT_IO;    // opaque external effect
    if (d->kind != DECL_FUNCTION && d->kind != DECL_PROCEDURE) return 0;
    if (d->as.function_decl.effects_done) return d->as.function_decl.effects;
    if (d->as.function_decl.effects_in_progress) return EFFECT_DIVERGE;  // recursion cycle
    d->as.function_decl.effects_in_progress = true;

    EffectSet saved = g_eff_acc; Decl *saved_self = g_eff_self;
    g_eff_acc = 0; g_eff_self = d;
    eff_visit_list(d->as.function_decl.body);
    EffectSet result = g_eff_acc;
    g_eff_acc = saved; g_eff_self = saved_self;

    d->as.function_decl.effects = result;
    d->as.function_decl.effects_done = true;
    d->as.function_decl.effects_in_progress = false;
    return result;
}

// F3.3 --dump-effects: print a function's inferred effect row.
static void sema_print_effects(Decl *d) {
    Id *n = d->as.function_decl.name;
    const char *kind = (d->kind == DECL_FUNCTION) ? "func" : "proc";
    EffectSet e = d->as.function_decl.effects;
    fprintf(stderr, "[effects] %s %.*s : {", kind, n ? (int)n->length : 1, n ? n->name : "?");
    const char *sep = "";
    if (e & EFFECT_WRITE)   { fprintf(stderr, "%sWrite",   sep); sep = ", "; }
    if (e & EFFECT_DIVERGE) { fprintf(stderr, "%sDiverge", sep); sep = ", "; }
    if (e & EFFECT_RAISES)  { fprintf(stderr, "%sRaises",  sep); sep = ", "; }
    if (e & EFFECT_IO)      { fprintf(stderr, "%sIO",      sep); sep = ", "; }
    if (e & EFFECT_ALLOC)   { fprintf(stderr, "%sAlloc",   sep); sep = ", "; }
    fprintf(stderr, "}%s\n", e == 0 ? "  (pure & total)" : "");
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

// ── Return-path completeness ─────────────────────────────────────────────────
// A non-void function must not fall off the end without returning a value (that
// is C undefined behavior). "Always exits" = every path leaves via `return` or a
// diverging call (`panic`). Structurally analogous to measure_scan_body's
// per-path analysis (if → both branches, match → all cases).
static bool sema_stmt_always_exits(Stmt *s);

static bool sema_stmts_always_exit(StmtList *list) {
    for (StmtList *l = list; l; l = l->next)
        if (sema_stmt_always_exits(l->stmt)) return true;
    return false;
}

static bool sema_call_diverges(Expr *e) {
    if (!e || e->kind != EXPR_CALL) return false;
    Expr *callee = e->as.call_expr.callee;
    return callee && callee->kind == EXPR_IDENTIFIER && callee->as.identifier_expr.id &&
           callee->as.identifier_expr.id->length == 5 &&
           memcmp(callee->as.identifier_expr.id->name, "panic", 5) == 0;
}

static bool sema_stmt_always_exits(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
        case STMT_RETURN: return true;
        case STMT_EXPR:   return sema_call_diverges(s->as.expr_stmt.expr);
        case STMT_IF:
            // Needs an else, and both branches must exit.
            return s->as.if_stmt.else_branch &&
                   sema_stmts_always_exit(s->as.if_stmt.then_body) &&
                   sema_stmts_always_exit(s->as.if_stmt.else_branch);
        case STMT_MATCH: {
            // All arms must exit (exhaustiveness is enforced separately, E014).
            if (!s->as.match_stmt.cases) return false;
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next)
                if (!sema_stmts_always_exit(c->body)) return false;
            return true;
        }
        case STMT_UNSAFE: return sema_stmts_always_exit(s->as.unsafe_stmt.body);
        default: return false;   // loops / plain statements: no guarantee
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

// Resolve a TYPE_SIMPLE name to its struct/enum Decl (or NULL).
static Decl *sema_decl_for_type_name(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return NULL;
    if ((size_t)t->base_type->length >= 256) return NULL;
    char buf[256];
    memcpy(buf, t->base_type->name, t->base_type->length);
    buf[t->base_type->length] = '\0';
    Symbol *sym = sema_lookup(buf);
    if (sym && sym->decl && (sym->decl->kind == DECL_STRUCT || sym->decl->kind == DECL_ENUM))
        return sym->decl;
    return NULL;
}

// Does aggregate `d` transitively contain `target` BY VALUE — through nominal or
// fixed-array fields, NOT pointers/slices (which break the size cycle)? A
// by-value cycle is infinite size and crashes the layout / is_linear walks.
static bool sema_aggregate_reaches_by_value(Decl *d, Decl *target, Decl **seen, int nseen) {
    if (!d || nseen >= 128) return false;
    for (int i = 0; i < nseen; i++) if (seen[i] == d) return false;
    seen[nseen] = d;
    #define _FIELD_STEP(FT) do { \
        Type *ft = (FT); \
        while (ft && ft->kind == TYPE_ARRAY && ft->array_len > 0) ft = ft->element_type; \
        Decl *fd = sema_decl_for_type_name(ft); \
        if (fd) { if (fd == target) return true; \
                  if (sema_aggregate_reaches_by_value(fd, target, seen, nseen + 1)) return true; } \
    } while (0)
    if (d->kind == DECL_STRUCT) {
        for (DeclList *f = d->as.struct_decl.fields; f; f = f->next)
            if (f->decl && f->decl->kind == DECL_VARIABLE) _FIELD_STEP(f->decl->as.variable_decl.type);
    } else if (d->kind == DECL_ENUM) {
        for (Variant *v = d->as.enum_decl.variants; v; v = v->next)
            for (DeclList *f = v->fields; f; f = f->next)
                if (f->decl && f->decl->kind == DECL_VARIABLE) _FIELD_STEP(f->decl->as.variable_decl.type);
    }
    #undef _FIELD_STEP
    return false;
}

// Reject a struct/enum that contains itself by value (infinite size) — must be
// indirected through a pointer. Prevents an infinite-recursion crash in the
// layout and is-linear walks.
static void sema_check_no_value_cycle(Decl *d) {
    if (!d || (d->kind != DECL_STRUCT && d->kind != DECL_ENUM)) return;
    Decl *seen[128];
    if (sema_aggregate_reaches_by_value(d, d, seen, 0)) {
        Id *nm = (d->kind == DECL_STRUCT) ? d->as.struct_decl.name : d->as.enum_decl.type_name;
        int nl = nm ? (int)nm->length : 0;
        fprintf(stderr, "[E104] Error Ln %li, Col %li: type '%.*s' contains itself by value "
            "(infinite size) — a recursive field must go through a pointer (e.g. '*%.*s').\n",
            (long)d->line, (long)d->col, nl, nm ? nm->name : "", nl, nm ? nm->name : "");
        diagnostic_show_line(d->line, d->col);
        exit(1);
    }
}

static void sema_check_no_mutual_recursion(DeclList *decls) {
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;
        // Skip generic templates — only their concrete instances are checked.
        if (decl_is_generic_template(d)) continue;

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
                sema_check_no_value_cycle(dl->decl);  // reject infinite-size before any recursive walk
                if (sema_check_struct_field_mov(dl->decl)) any_error = true;
            }
        }
        if (any_error) exit(1);
    }

    // D-Niche (re-land): precompute niche layout for every enum, emit a
    // best-effort W120 when the sentinel pool was insufficient, and dump the
    // decision when --dump-niche is set. Codegen (emit/*) recomputes per enum,
    // so this loop is diagnostic; it runs after monomorphization below only for
    // W120 coverage of generic instances — non-generic enums are covered here.
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (dl->decl && dl->decl->kind == DECL_ENUM &&
            !decl_is_generic_template(dl->decl)) {
            NicheLayout layout = niche_compute_layout(&dl->decl->as.enum_decl);
            niche_emit_w120(&dl->decl->as.enum_decl, &layout);
            if (sema_dump_niche) niche_dump_layout(&dl->decl->as.enum_decl, &layout);
        }
    }

    // Pre-pass: resolve EVERY function signature (lowering `T | markers` return
    // and param types to their niche'd enum) before any body is inferred — so a
    // call to a union-returning function reads the lowered enum, not a raw
    // TYPE_UNION. mono_resolve_signature is idempotent, so the loop below re-runs
    // it harmlessly.
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || decl_is_generic_template(d)) continue;
        if (d->kind == DECL_FUNCTION || d->kind == DECL_PROCEDURE) {
            mono_resolve_signature(d);
        } else if (d->kind == DECL_STRUCT) {   // lower `T | markers` / Vec(i32) field types
            for (DeclList *f = d->as.struct_decl.fields; f; f = f->next)
                if (f->decl && f->decl->kind == DECL_VARIABLE)
                    f->decl->as.variable_decl.type = mono_resolve_type_apps(f->decl->as.variable_decl.type);
        } else if (d->kind == DECL_ENUM) {
            for (Variant *v = d->as.enum_decl.variants; v; v = v->next)
                for (DeclList *f = v->fields; f; f = f->next)
                    if (f->decl && f->decl->kind == DECL_VARIABLE)
                        f->decl->as.variable_decl.type = mono_resolve_type_apps(f->decl->as.variable_decl.type);
        }
    }

    // 2) For each function: resolve → infer → linearity → clear locals
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d) continue;
        // Top-level constant: resolve + type its initializer (all globals were
        // registered in the first pass, so cross-references between constants and
        // arrays like CTYPE resolve here).
        if (d->kind == DECL_VARIABLE) {
            if (d->as.variable_decl.init) {
                sema_clear_locals();
                sema_resolve_expr(d->as.variable_decl.init);
                sema_infer_expr(d->as.variable_decl.init);
            }
            continue;
        }
        if (d->kind != DECL_FUNCTION && d->kind != DECL_PROCEDURE) continue;
        // Generic templates are never processed directly — only their concrete
        // monomorphized instances (appended to this same list) are.
        if (decl_is_generic_template(d)) continue;
        // Resolve generic type-applications in the signature (`Vec(i32)` params/
        // returns) — for original functions and appended instances alike.
        mono_resolve_signature(d);

        sema_clear_locals();

        // P0 soundness (name-keyed leak): capture the VRA fact-table state BEFORE
        // this function seeds its parameter refinements, so those constraints are
        // rolled back before the next function. `sema_ranges` is a single shared
        // table; seeding (`i < a.len` -> `i - __len_a <= -1`) happens in the param
        // loop and during resolve, and the old snapshot was taken at the walk —
        // TOO LATE, so a param refinement leaked by name into a later same-named
        // parameter and let its `a[i]` read out of bounds (confirmed via ASan).
        // range_set/constraint_add prepend, so resetting head/constraints fully
        // isolates. (Facts are still live through this function's walk below,
        // which runs before the restore.)
        RangeEntry *__fn_pre_head = sema_ranges ? sema_ranges->head : NULL;
        ConstraintEntry *__fn_pre_cons = sema_ranges ? sema_ranges->constraints : NULL;

        // Reject duplicate parameter names (ambiguous — C would redefine the
        // symbol; the second shadows the first with no diagnostic otherwise).
        for (DeclList *pa = d->as.function_decl.params; pa; pa = pa->next) {
            if (!pa->decl || pa->decl->kind != DECL_VARIABLE) continue;
            Id *na = pa->decl->as.variable_decl.name;
            if (!na) continue;
            for (DeclList *pb = pa->next; pb; pb = pb->next) {
                if (!pb->decl || pb->decl->kind != DECL_VARIABLE) continue;
                Id *nb = pb->decl->as.variable_decl.name;
                if (nb && nb->length == na->length &&
                    strncmp(na->name, nb->name, na->length) == 0) {
                    fprintf(stderr, "[E013] Error Ln %li, Col %li: duplicate parameter name '%.*s'.\n",
                        (long)d->line, (long)d->col, (int)na->length, na->name);
                    diagnostic_show_line(d->line, d->col);
                    exit(1);
                }
            }
        }

        // Reject a dependent array size that is a bare identifier referencing
        // neither a parameter nor an in-scope name (e.g. `func f(a i32[n])` with
        // free n) — the backend emits an undeclared `n`. Sizes may reference
        // other parameters (`n`, `a.len`); use `T[]` for an unsized slice.
        for (DeclList *sp = d->as.function_decl.params; sp; sp = sp->next) {
            if (!sp->decl || sp->decl->kind != DECL_VARIABLE) continue;
            Type *spt = sp->decl->as.variable_decl.type;
            if (!spt || spt->kind != TYPE_ARRAY || !spt->size_expr ||
                spt->size_expr->kind != EXPR_IDENTIFIER) continue;
            Id *sid = spt->size_expr->as.identifier_expr.id;
            if (!sid) continue;
            bool bound = false;
            for (DeclList *pp = d->as.function_decl.params; pp && !bound; pp = pp->next) {
                if (pp->decl && pp->decl->kind == DECL_VARIABLE) {
                    Id *pn = pp->decl->as.variable_decl.name;
                    if (pn && pn->length == sid->length &&
                        strncmp(pn->name, sid->name, pn->length) == 0) bound = true;
                }
            }
            if (!bound && (size_t)sid->length < 128) {
                char nb[128];
                memcpy(nb, sid->name, sid->length); nb[sid->length] = '\0';
                if (sema_lookup(nb)) bound = true;
            }
            if (!bound) {
                fprintf(stderr, "[E100] Error Ln %li, Col %li: dependent array size '%.*s' is "
                    "not a parameter or in scope — use 'T[]' for an unsized slice or add a "
                    "length parameter.\n",
                    (long)d->line, (long)d->col, (int)sid->length, sid->name);
                diagnostic_show_line(d->line, d->col);
                exit(1);
            }
        }

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

                // Seed the VRA range of every fixed-width integer parameter with
                // its TYPE range. This is what makes overflow prove-or-reject the
                // default: an unconstrained iN/uN param now carries [T_min, T_max]
                // instead of "unknown", so arithmetic on it (e.g. `a + b` of two
                // i32s → [2*i32_min, 2*i32_max]) has a concrete range that provably
                // can exceed the result type and is rejected unless the inputs are
                // constrained, a wrapping/saturating op is used, or an `as` widening
                // cast is applied. (i64/isize seed to the full i64 range, which the
                // UNBOUNDED_WINDOW treats as no-info — i64 has no headroom for VRA to
                // detect overflow, so i64 arithmetic is unchecked, as before.)
                if (sema_ranges && pty) {
                    int bits; bool sgn;
                    if (parse_iN_uN(pty, &bits, &sgn)) {
                        long long tlo, thi;
                        if (type_integer_range(pty, &tlo, &thi)) {
                            range_set(sema_ranges, pid,
                                      range_make((int64_t)tlo, (int64_t)thi));
                        }
                    } else if (pty->kind == TYPE_SIMPLE && pty->base_type) {
                        isize pl = pty->base_type->length;
                        if (pl == 5 && memcmp(pty->base_type->name, "usize", 5) == 0) {
                            range_set(sema_ranges, pid, range_make(0, INT64_MAX));
                        } else if (pl == 5 && memcmp(pty->base_type->name, "isize", 5) == 0) {
                            range_set(sema_ranges, pid, range_make(INT64_MIN, INT64_MAX));
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

                        // G9: dynamic array `i usize in a` — tie `i < a.len` via a
                        // difference constraint against the synthetic __len_a var
                        // (registered by the __len_PARAM seeding for array params
                        // processed earlier). Previously only fixed arrays tightened,
                        // so `a[i]` on a plain slice was rejected E085.
                        if (arr_type->kind == TYPE_ARRAY && arr_type->array_len == -1) {
                            char key[272]; int klen = 6 + (int)arr_id->length;
                            if (klen < (int)sizeof(key)) {
                                memcpy(key, "__len_", 6);
                                memcpy(key + 6, arr_id->name, arr_id->length);
                                Id *len_id = NULL;
                                for (RangeEntry *re = sema_ranges->head; re; re = re->next)
                                    if (re->var && re->var->length == klen &&
                                        strncmp(re->var->name, key, klen) == 0) { len_id = re->var; break; }
                                if (len_id) constraint_add(sema_ranges, param_id, len_id, -1);
                            }
                        }
                    }
                }
                
                // Apply equation-style constraints: b int != 0, x int >= 0 and <= 100
                if (p->decl->as.variable_decl.constraints && sema_ranges) {
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        sema_apply_constraint(c->expr, sema_ranges);
                    }
                }

                // Alias-typed param: seed VRA from the refinement alias's own
                // constraints too (they live on the alias decl, not the param),
                // rewriting each as `param <op> bound`. Lets a bounded-alias
                // index prove in-bounds (`a[i]` with `i SmallIdx` = i32 0..9).
                if (sema_ranges) {
                    ExprList *ac = alias_constraints_for(p->decl->as.variable_decl.type);
                    for (ExprList *c = ac; c; c = c->next) {
                        if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
                        Expr pe; memset(&pe, 0, sizeof(pe));
                        pe.kind = EXPR_IDENTIFIER; pe.as.identifier_expr.id = pid;
                        Expr synth; memset(&synth, 0, sizeof(synth));
                        synth.kind = EXPR_BINARY;
                        synth.as.binary_expr.op = c->expr->as.binary_expr.op;
                        synth.as.binary_expr.left = &pe;
                        synth.as.binary_expr.right = c->expr->as.binary_expr.right;
                        sema_apply_constraint(&synth, sema_ranges);
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
        current_return_type = mono_resolve_type_apps(d->as.function_decl.return_type);
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
        InGuardEntry *__fn_old_guards = sema_in_guards;
        NarrowEntry *__fn_old_narrows = sema_narrows;
        sema_walk_phase = true;
        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next)
            walk_stmt(sl->stmt);
        sema_walk_phase = false;
        // Restore to the PRE-function baseline (captured before param seeding, so
        // this function's parameter refinements + resolve/walk facts are all
        // rolled back — no leak into the next same-named function).
        if (sema_ranges) {
            sema_ranges->head = __fn_pre_head;
            sema_ranges->constraints = __fn_pre_cons;
        }
        sema_in_guards = __fn_old_guards;
        sema_narrows = __fn_old_narrows;

        // 2.c.i) Return-path completeness: a non-void function must return (or
        // diverge) on every path — never fall off the end (C UB).
        if (d->as.function_decl.return_type &&
            !sema_stmts_always_exit(d->as.function_decl.body)) {
            fprintf(stderr, "[E018] Error Ln %li, Col %li: function '%.*s' can reach the end "
                "without returning a value.\n"
                "       Every path must end in a `return` (or a diverging call such as panic).\n",
                (long)d->line, (long)d->col,
                (int)d->as.function_decl.name->length, d->as.function_decl.name->name);
            diagnostic_show_line(d->line, d->col);
            exit(1);
        }

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

    // 4) W130 + F3.3 effect inference. Run AFTER per-function resolve so Expr.decl
    //    is populated everywhere (needed for callee-kind effect detection).
    for (DeclList *dl = decls; dl; dl = dl->next) {
        if (!dl->decl) continue;
        if (dl->decl->kind == DECL_FUNCTION || dl->decl->kind == DECL_PROCEDURE) {
            EffectSet ef = effect_full(dl->decl);   // transitive; memoized + stored
            if (sema_dump_effects) sema_print_effects(dl->decl);
            // E3 (consistency net): a `func` is pure (no IO) and total (no
            // Diverge). If the effect row infers either, its guarantees are
            // violated — a hole the func checks (E011) should already catch.
            if (dl->decl->kind == DECL_FUNCTION && (ef & (EFFECT_IO | EFFECT_DIVERGE))) {
                Id *n = dl->decl->as.function_decl.name;
                fprintf(stderr, "[E011] Error Ln %li, Col %li: `func` '%.*s' has a forbidden "
                    "effect (%s) — a func must be pure and total. Declare it `proc`.\n",
                    (long)dl->decl->line, (long)dl->decl->col,
                    n ? (int)n->length : 1, n ? n->name : "?",
                    (ef & EFFECT_IO) ? "IO" : "Diverge");
                diagnostic_show_line(dl->decl->line, dl->decl->col);
                exit(1);
            }
        }
        if (dl->decl->kind == DECL_PROCEDURE) {
            sema_check_proc_eligibility(dl->decl);
        }
    }
}

// Optional: destroy/reset global state
static void sema_destroy(void) {
    sema_clear_globals();
}

#endif // SEMA_H
