#ifndef LAIN_SEMA_UNDECLARED_H
#define LAIN_SEMA_UNDECLARED_H
// undeclared.h — E106, "use of undeclared identifier", raised by the FRONT END.
//
// ★ WHY THIS FILE EXISTS. The check used to live in src/emit/expr.h, and flipping the default
// backend is what made that untenable: with the C emitted from the IR the old emitter never
// runs, so an undeclared name was ACCEPTED and emitted as `void v0;` — broken C, and
// prove-or-reject delegated to the C compiler. Name resolution is the front end's job, and
// the backend is the last place that should notice.
//
// ★ WHY IT RUNS HERE AND NOT IN resolve.h. Its old comment gave the reason and it still
// holds: the predicate is only reliable AFTER resolution, monomorphization and UFCS
// desugaring have all finished, because until then a name may legitimately be unbound (a
// template's body, a method call awaiting its receiver). So this is a separate pass over the
// finished AST rather than a check inside the resolver — late in the front end, not in the
// back.
//
// THE PREDICATE IS THE EMITTER'S, VERBATIM: an EXPR_IDENTIFIER with a real source location,
// no type, no decl, not global, and not a pattern binding. Everything else in this file is
// the WALK, and the walk is the part that can go wrong: it must visit what the backend would
// emit and nothing else, or it reports names that are fine (over-rejection) or misses names
// that are not. Where the two could differ the walk is deliberately the PERMISSIVE side —
// pattern positions and un-taken comptime branches are not visited, bindings are
// over-approximated — so a divergence costs a missed diagnostic, never a rejected program.
#include "../ast.h"
#include <stdio.h>

// Case-arm payload bindings currently in scope, exactly as emit/core.h kept them: a binding
// (`case s { Some(v): … v … }`) is a NULL-typed local that no symbol table tracks.
static const char *und_bind_name[256];
static int         und_bind_len[256];
static int         und_bind_depth = 0;
static const char *und_file = NULL;
static int         und_found = 0;

static bool und_is_binding(const char *name, int len) {
    for (int i = 0; i < und_bind_depth; i++)
        if (und_bind_len[i] == len && strncmp(und_bind_name[i], name, len) == 0) return true;
    return false;
}
static void und_push(Id *id) {
    if (!id || und_bind_depth >= 256) return;
    und_bind_name[und_bind_depth] = id->name;
    und_bind_len[und_bind_depth]  = (int)id->length;
    und_bind_depth++;
}

static void und_expr(Expr *e);
static void und_stmt_list(StmtList *l);

// Every identifier a pattern could BIND, over-approximated: an argument position in a call
// pattern (`Some(v)`, `Err(NotFound)`). The emitter distinguishes a binding from a static
// sub-variant by consulting the niche layout; here the distinction does not matter, because
// treating a sub-variant name as bound only makes this check more permissive.
static void und_collect_pattern_bindings(Expr *p) {
    if (!p || p->kind != EXPR_CALL) return;
    for (ExprList *a = p->as.call_expr.args; a; a = a->next) {
        if (!a->expr) continue;
        if (a->expr->kind == EXPR_IDENTIFIER) und_push(a->expr->as.identifier_expr.id);
        else und_collect_pattern_bindings(a->expr);   // nested pattern
    }
}

// A pattern is matched STRUCTURALLY, not evaluated: its head identifier is a variant name and
// its arguments are bindings, neither of which resolves to a value node. Only the shapes the
// backend really evaluates — a range's endpoints, a literal — are walked.
static void und_pattern(Expr *p) {
    if (!p) return;
    if (p->kind == EXPR_IDENTIFIER || p->kind == EXPR_CALL || p->kind == EXPR_MEMBER) return;
    und_expr(p);
}

static void und_match_cases_expr(ExprMatchCase *cs) {
    for (ExprMatchCase *c = cs; c; c = c->next) {
        int saved = und_bind_depth;
        for (ExprList *p = c->patterns; p; p = p->next) und_pattern(p->expr);
        if (c->patterns) und_collect_pattern_bindings(c->patterns->expr);
        und_expr(c->body);
        und_bind_depth = saved;               // bindings leave scope with the arm
    }
}

static void und_expr(Expr *e) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_IDENTIFIER:
            if (e->line > 0 && e->type == NULL && e->decl == NULL && !e->is_global &&
                !und_is_binding(e->as.identifier_expr.id->name,
                                (int)e->as.identifier_expr.id->length)) {
                fprintf(stderr, "[E106] Error Ln %li, Col %li: use of undeclared identifier '%.*s'.\n",
                        (long)e->line, (long)e->col,
                        (int)e->as.identifier_expr.id->length,
                        e->as.identifier_expr.id->name);
                if (und_file)
                    fprintf(stderr, "  --> %s:%li:%li\n", und_file, (long)e->line, (long)e->col);
                und_found++;
            }
            break;
        case EXPR_BINARY:
            und_expr(e->as.binary_expr.left); und_expr(e->as.binary_expr.right); break;
        case EXPR_UNARY:  und_expr(e->as.unary_expr.right); break;
        case EXPR_MEMBER: und_expr(e->as.member_expr.target); break;
        case EXPR_CALL:
            und_expr(e->as.call_expr.callee);
            for (ExprList *a = e->as.call_expr.args; a; a = a->next) und_expr(a->expr);
            break;
        case EXPR_INDEX:
            und_expr(e->as.index_expr.target); und_expr(e->as.index_expr.index); break;
        case EXPR_RANGE:
            und_expr(e->as.range_expr.start); und_expr(e->as.range_expr.end); break;
        case EXPR_MOVE:  und_expr(e->as.move_expr.expr); break;
        case EXPR_MUT:   und_expr(e->as.mut_expr.expr); break;
        case EXPR_CAST:  und_expr(e->as.cast_expr.expr); break;
        case EXPR_ADDR:  und_expr(e->as.addr_expr.expr); break;
        case EXPR_DEREF: und_expr(e->as.deref_expr.expr); break;
        case EXPR_TRY:   und_expr(e->as.try_expr.operand); break;
        case EXPR_ELSE:
            und_expr(e->as.else_expr.operand); und_expr(e->as.else_expr.arm); break;
        case EXPR_BUILTIN:
            und_expr(e->as.builtin_expr.arg);
            und_expr(e->as.builtin_expr.arg2);
            und_expr(e->as.builtin_expr.arg3);
            break;
        case EXPR_ARRAY_LITERAL:
            for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next)
                und_expr(el->expr);
            break;
        case EXPR_ARRAY_COMPREHENSION: {
            int saved = und_bind_depth;
            und_expr(e->as.array_comprehension_expr.range);
            und_push(e->as.array_comprehension_expr.idx);   // `idx` is bound inside the body
            und_expr(e->as.array_comprehension_expr.body);
            und_bind_depth = saved;
            break;
        }
        case EXPR_ANON_STRUCT:
            for (DeclList *f = e->as.anon_struct_expr.fields; f; f = f->next)
                if (f->decl && f->decl->kind == DECL_VARIABLE)
                    und_expr(f->decl->as.variable_decl.init);
            break;
        case EXPR_MATCH:
            und_expr(e->as.match_expr.value);
            und_match_cases_expr(e->as.match_expr.cases);
            break;
        default: break;   // literals, EXPR_TYPE, EXPR_ANON_ENUM: no value identifiers
    }
}

static void und_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        // `in_expr` is an invariant annotation, not code: nothing is emitted for it.
        case STMT_VAR:    und_expr(s->as.var_stmt.expr); break;
        case STMT_ASSIGN:
            und_expr(s->as.assign_stmt.target); und_expr(s->as.assign_stmt.expr); break;
        case STMT_EXPR:   und_expr(s->as.expr_stmt.expr); break;
        case STMT_RETURN: und_expr(s->as.return_stmt.value); break;
        case STMT_ASSERT: und_expr(s->as.assert_stmt.cond); break;
        case STMT_DEFER:  und_stmt(s->as.defer_stmt.stmt); break;
        case STMT_UNSAFE: und_stmt_list(s->as.unsafe_stmt.body); break;
        case STMT_IF:
            und_expr(s->as.if_stmt.cond);
            und_stmt_list(s->as.if_stmt.then_body);
            und_stmt_list(s->as.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            // The measure is a proof obligation, not code — `decreasing` emits nothing.
            und_expr(s->as.while_stmt.cond);
            und_stmt_list(s->as.while_stmt.body);
            break;
        case STMT_FOR: {
            int saved = und_bind_depth;
            und_expr(s->as.for_stmt.iterable);
            und_push(s->as.for_stmt.index_name);
            und_push(s->as.for_stmt.value_name);
            und_stmt_list(s->as.for_stmt.body);
            und_bind_depth = saved;
            break;
        }
        // STMT_MATCH_CASE never appears as a statement in a list — it is reached through
        // STMT_MATCH's `cases`, the same as in ast_clone.h. Reading `match_stmt` for it would
        // be reading a union member nothing set.
        case STMT_MATCH:
            und_expr(s->as.match_stmt.value);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                int saved = und_bind_depth;
                for (ExprList *p = c->patterns; p; p = p->next) und_pattern(p->expr);
                if (c->patterns) und_collect_pattern_bindings(c->patterns->expr);
                und_stmt_list(c->body);
                und_bind_depth = saved;
            }
            break;
        case STMT_COMPTIME_IF:
            // Only the taken branch reaches the backend; the other is not this target's code.
            if (!s->as.comptime_if_stmt.evaluated) break;
            und_stmt_list(s->as.comptime_if_stmt.is_taken
                            ? s->as.comptime_if_stmt.then_body
                            : s->as.comptime_if_stmt.else_branch);
            break;
        default: break;   // break, continue, use
    }
}

static void und_stmt_list(StmtList *l) {
    for (; l; l = l->next) und_stmt(l->stmt);
}

// Walk everything the backend would emit. Returns the number of undeclared names found.
// A generic TEMPLATE is skipped for the same reason the emitter skips it: only its instances
// are code, and its body mentions type parameters that are bound per instance.
static int sema_check_undeclared(DeclList *decls, const char *file) {
    und_file = file; und_found = 0; und_bind_depth = 0;
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || decl_is_generic_template(d)) continue;
        if (d->kind == DECL_FUNCTION || d->kind == DECL_PROCEDURE) {
            if (d->as.function_decl.is_extern) continue;
            und_stmt_list(d->as.function_decl.body);
        } else if (d->kind == DECL_VARIABLE) {
            und_expr(d->as.variable_decl.init);
        }
    }
    return und_found;
}

#endif // LAIN_SEMA_UNDECLARED_H
