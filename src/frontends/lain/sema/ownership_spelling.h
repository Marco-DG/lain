#ifndef SEMA_OWNERSHIP_SPELLING_H
#define SEMA_OWNERSHIP_SPELLING_H
// ownership_spelling.h — what survived src/sema/linearity.h.
//
// ★ THE OLD OWNERSHIP CHECKER IS GONE (2,301 lines). Linearity, moves, borrows and definite
// assignment are the sovereign engine's — src/analysis/linearity.h, borrow.h,
// definite_init.h, over the IR — and have been the default since 2026-09-08.
//
// Three things in that file were NOT the checker, and dropping them would have been a silent
// loss rather than a cleanup. They are here because they are about the SOURCE TEXT, and the
// IR cannot see source text:
//
//   [E007] `mov` at a call site   a rule about how the argument is WRITTEN. The IR is
//   [E017] `var` at a call site   byte-identical either way, so no IR analysis can ever
//                                 raise them, and standing the pass down would have deleted
//                                 them with nothing to notice.
//   Q-008 struct field `mov`      the same, one level up: every linear field of a struct or
//                                 enum literal must be spelled `mov`.
//   sema_type_is_linear           the front end asks this while typing, before any IR exists.
//
// That is the whole seam rule, applied to a deletion instead of a suppression: a check may be
// removed only where the new engine RAISES the obligation, and these three it cannot.
#include "../ast.h"

static bool sema_type_is_linear(Type *t);

// Use the robust recursive check
#define is_type_move(t) sema_type_is_linear(t)

// Q-008: enforce that every field of a struct/variant whose type is linear
// must be annotated `mov`. Returns true if any error was emitted.
static bool sema_check_struct_field_mov(Decl *d) {
    bool had_error = false;
    if (!d) return false;
    if (d->kind == DECL_STRUCT) {
        for (DeclList *f = d->as.struct_decl.fields; f; f = f->next) {
            if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
            Type *ft = f->decl->as.variable_decl.type;
            if (!ft) continue;
            // If the field's type is linear and the field itself is not annotated `mov`...
            if (ft->mode != MODE_OWNED && sema_type_is_linear(ft)) {
                Id *fname = f->decl->as.variable_decl.name;
                Id *sname = d->as.struct_decl.name;
                fprintf(stderr,
                    "[E083] Error Ln %li, Col %li: field '%.*s' in struct '%.*s' has linear type but is missing `mov` annotation. Add `mov` to the field declaration.\n",
                    f->decl->line, f->decl->col,
                    (int)(fname ? fname->length : 0), fname ? fname->name : "?",
                    (int)(sname ? sname->length : 0), sname ? sname->name : "?");
                had_error = true;
            }
        }
    } else if (d->kind == DECL_ENUM) {
        for (Variant *v = d->as.enum_decl.variants; v; v = v->next) {
            for (DeclList *f = v->fields; f; f = f->next) {
                if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
                Type *ft = f->decl->as.variable_decl.type;
                if (!ft) continue;
                if (ft->mode != MODE_OWNED && sema_type_is_linear(ft)) {
                    Id *fname = f->decl->as.variable_decl.name;
                    Id *vname = v->name;
                    fprintf(stderr,
                        "[E083] Error Ln %li, Col %li: field '%.*s' in variant '%.*s' has linear type but is missing `mov` annotation.\n",
                        f->decl->line, f->decl->col,
                        (int)(fname ? fname->length : 0), fname ? fname->name : "?",
                        (int)(vname ? vname->length : 0), vname ? vname->name : "?");
                    had_error = true;
                }
            }
        }
    }
    return had_error;
}

static bool sema_type_is_linear(Type *t) {
    if (!t) return false;
    // 1. Explicit ownership override
    if (t->mode == MODE_OWNED) return true;

    // 2. Aggregate types: recursive check
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE || t->kind == TYPE_POINTER) {
        // Pointers/Slices/Arrays: only linear if mode is OWNED (checked above) 
        // OR if element type is linear?
        // - Array[T] where T is linear -> Array is linear (must consume)
        // - Pointer[T] -> Pointer is copyable unless mode=OWNED. T's linearity doesn't affect pointer linearity.
        // - Slice[T] -> Slice is struct {ptr, len}. If T is linear? 
        //   Lain slices are views. They don't own T.
        //   Arrays [N]T OWN the elements. So matching Rust: [T; N] is linear if T is linear.
        if (t->kind == TYPE_ARRAY) {
            return sema_type_is_linear(t->element_type);
        }
        return false;
    }

    // 3. Simple ID (Structs/Enums)
    if (t->kind == TYPE_SIMPLE) {
        // Resolve underlying declaration
        extern Symbol *sema_lookup(const char *name); // Forward decl from sema.h
        
        char buf[256];
        if (t->base_type->length >= sizeof(buf)) return false;
        memcpy(buf, t->base_type->name, t->base_type->length);
        buf[t->base_type->length] = '\0';
        
        Symbol *sym = sema_lookup(buf);
        if (!sym || !sym->decl) return false;

        if (sym->decl->kind == DECL_STRUCT) {
            // Check all fields
            for (DeclList *f = sym->decl->as.struct_decl.fields; f; f = f->next) {
                if (f->decl->kind == DECL_VARIABLE) {
                    if (sema_type_is_linear(f->decl->as.variable_decl.type)) {
                        return true; // Found a linear field -> Struct is linear
                    }
                }
            }
        }
        if (sym->decl->kind == DECL_ENUM) {
            // Check all variants: if ANY variant has a linear field, the enum is linear
            for (Variant *v = sym->decl->as.enum_decl.variants; v; v = v->next) {
                for (DeclList *f = v->fields; f; f = f->next) {
                    if (f->decl->kind == DECL_VARIABLE) {
                        if (sema_type_is_linear(f->decl->as.variable_decl.type)) {
                            return true; // Found a linear field in a variant -> Enum is linear
                        }
                    }
                }
            }
        }
    }

    return false;
}

/* ---------- helpers to find function decl robustly ---------- */

/* Try to find a function Decl by a mangled-or-raw name.
   Accepts either "module_fn" or "fn" and matches Decl->as.function_decl.name (raw name). */
static Decl *find_function_decl_by_mangled_or_raw(const char *mangled) {
    if (!mangled) return NULL;
    
    // Quick pass: if the string exactly equals a raw name, use it.
    for (ModuleNode *mn = loaded_modules; mn; mn = mn->next) {
        for (DeclList *dl = mn->decls; dl; dl = dl->next) {
            Decl *d = dl->decl;
            if (!d) continue;
            // Also check procedures and externs since they can take linear args
            if (d->kind != DECL_FUNCTION && 
                d->kind != DECL_EXTERN_FUNCTION) continue;
            
            Id *fid = d->as.function_decl.name;
            if (!fid) continue;
            if (strncmp(mangled, fid->name, fid->length) == 0 && mangled[fid->length] == '\0') return d;
        }
    }
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;
        Id *fid = d->as.function_decl.name;
        if (!fid) continue;
        if (strncmp(mangled, fid->name, fid->length) == 0 && mangled[fid->length] == '\0') return d;
    }

    // Otherwise, try to match "<module>_<raw>" by suffix:
    size_t mlen = strlen(mangled);
    for (ModuleNode *mn = loaded_modules; mn; mn = mn->next) {
        for (DeclList *dl = mn->decls; dl; dl = dl->next) {
            Decl *d = dl->decl;
            if (!d) continue;
            if (d->kind != DECL_FUNCTION && 
                d->kind != DECL_EXTERN_FUNCTION) continue;
            
            Id *fid = d->as.function_decl.name;
            if (!fid) continue;
            size_t rlen = (size_t)fid->length;
            if (rlen + 1 <= mlen && mangled[mlen - rlen - 1] == '_') {
                // compare suffix
                if (strncmp(mangled + (mlen - rlen), fid->name, rlen) == 0) {
                    return d;
                }
            }
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────────────────
   CALL-SITE SPELLING  —  [E007] `mov`, [E017] `var`

   These two are not ownership analysis. They are surface rules: an owned parameter must be
   handed its argument with `mov` written at the call site, and a mutable parameter with
   `var`. The IR lowers `f(a)` and `f(mov a)` to the same instructions, so no IR analysis can
   recover the distinction — which is exactly why the rule belongs to the front end and has to
   keep running when the sovereign analyses take over ownership (`--engine=ir`).

   The walk does NOTHING else: no tables, no state, no other diagnostic. Duplicating the two
   checks is deliberate; sharing them with the pass above would mean running the pass above.
   ───────────────────────────────────────────────────────────────────────────────────────── */

static void spell_walk_expr(Expr *e);
static void spell_walk_stmt(Stmt *s);

static void spell_walk_stmt_list(StmtList *l) {
    for (StmtList *sl = l; sl; sl = sl->next) spell_walk_stmt(sl->stmt);
}

/* The owner NAME behind an argument, or NULL when the argument is not a named place
   (a literal, a call result, a temporary): those cannot be moved FROM, so the rule
   does not apply to them. Mirrors the same unwrapping the linearity pass does. */
static Id *spell_owner_of(Expr *arg) {
    if (!arg) return NULL;
    if (arg->kind == EXPR_IDENTIFIER) return arg->as.identifier_expr.id;
    if (arg->kind == EXPR_MEMBER) {
        Expr *head = arg->as.member_expr.target;
        while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
        return (head && head->kind == EXPR_IDENTIFIER) ? head->as.identifier_expr.id : NULL;
    }
    if (arg->kind == EXPR_MUT || arg->kind == EXPR_MOVE) {
        Expr *in = (arg->kind == EXPR_MUT) ? arg->as.mut_expr.expr : arg->as.move_expr.expr;
        return (in && in->kind == EXPR_IDENTIFIER) ? in->as.identifier_expr.id : NULL;
    }
    return NULL;
}

static void spell_check_call(Expr *e) {
    Expr *callee = e->as.call_expr.callee;
    Decl *fn = NULL;
    if (callee && callee->decl &&
        (callee->decl->kind == DECL_FUNCTION))
        fn = callee->decl;
    if (!fn && callee && callee->kind == EXPR_IDENTIFIER && callee->as.identifier_expr.id) {
        Id *cid = callee->as.identifier_expr.id;
        char buf[256];
        int  n = cid->length < 255 ? (int)cid->length : 255;
        memcpy(buf, cid->name, (size_t)n); buf[n] = '\0';
        fn = find_function_decl_by_mangled_or_raw(buf);
    }
    if (!fn || (fn->kind != DECL_FUNCTION)) return;

    DeclList *params = fn->as.function_decl.params;
    ExprList *args   = e->as.call_expr.args;
    for (; params && args; params = params->next, args = args->next) {
        // NOT gated on DECL_VARIABLE: a DESTRUCTURING parameter (`mov {handle} File`) has
        // no binding name, and gating on the kind skipped exactly the consuming functions
        // this rule exists for.
        if (!params->decl) continue;
        Type *pty = params->decl->as.variable_decl.type;
        Expr *arg = args->expr;
        Id   *own = spell_owner_of(arg);
        if (!pty || !arg || !own) continue;

        if (pty->mode == MODE_OWNED && arg->kind != EXPR_MOVE) {
            fprintf(stderr, "[E007] Error Ln %li, Col %li: moving linear variable '%.*s' "
                    "requires explicit 'mov' at the call site.\n",
                    (long)e->line, (long)e->col, (int)own->length, own->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
        if (pty->mode == MODE_MUTABLE && arg->kind != EXPR_MUT) {
            fprintf(stderr, "[E017] Error Ln %li, Col %li: passing '%.*s' to a mutable "
                    "('var') parameter requires explicit 'var' at the call site.\n",
                    (long)e->line, (long)e->col, (int)own->length, own->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
}

static void spell_walk_expr(Expr *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_CALL:
        spell_check_call(e);
        spell_walk_expr(e->as.call_expr.callee);
        for (ExprList *a = e->as.call_expr.args; a; a = a->next) spell_walk_expr(a->expr);
        break;
    case EXPR_BINARY: spell_walk_expr(e->as.binary_expr.left);
                      spell_walk_expr(e->as.binary_expr.right); break;
    case EXPR_UNARY:  spell_walk_expr(e->as.unary_expr.right); break;
    case EXPR_MEMBER: spell_walk_expr(e->as.member_expr.target); break;
    case EXPR_INDEX:  spell_walk_expr(e->as.index_expr.target);
                      spell_walk_expr(e->as.index_expr.index); break;
    case EXPR_CAST:   spell_walk_expr(e->as.cast_expr.expr); break;
    case EXPR_MOVE:   spell_walk_expr(e->as.move_expr.expr); break;
    case EXPR_MUT:    spell_walk_expr(e->as.mut_expr.expr); break;
    case EXPR_TRY:    spell_walk_expr(e->as.try_expr.operand); break;
    case EXPR_ELSE:   spell_walk_expr(e->as.else_expr.operand);
                      spell_walk_expr(e->as.else_expr.arm); break;
    case EXPR_MATCH:
        spell_walk_expr(e->as.match_expr.value);
        for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
            for (ExprList *pl = c->patterns; pl; pl = pl->next) spell_walk_expr(pl->expr);
            spell_walk_expr(c->body);
        }
        break;
    default: break;
    }
}

static void spell_walk_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
    case STMT_VAR:    spell_walk_expr(s->as.var_stmt.expr); break;
    case STMT_ASSIGN: spell_walk_expr(s->as.assign_stmt.target);
                      spell_walk_expr(s->as.assign_stmt.expr); break;
    case STMT_EXPR:   spell_walk_expr(s->as.expr_stmt.expr); break;
    case STMT_RETURN: spell_walk_expr(s->as.return_stmt.value); break;
    case STMT_IF:     spell_walk_expr(s->as.if_stmt.cond);
                      spell_walk_stmt_list(s->as.if_stmt.then_body);
                      spell_walk_stmt_list(s->as.if_stmt.else_branch); break;
    case STMT_FOR:    spell_walk_expr(s->as.for_stmt.iterable);
                      spell_walk_stmt_list(s->as.for_stmt.body); break;
    case STMT_WHILE:  spell_walk_expr(s->as.while_stmt.cond);
                      spell_walk_stmt_list(s->as.while_stmt.body); break;
    case STMT_MATCH:
        spell_walk_expr(s->as.match_stmt.value);
        for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
            for (ExprList *pl = c->patterns; pl; pl = pl->next) spell_walk_expr(pl->expr);
            spell_walk_stmt_list(c->body);
        }
        break;
    case STMT_UNSAFE: spell_walk_stmt_list(s->as.unsafe_stmt.body); break;
    case STMT_DEFER:  spell_walk_stmt(s->as.defer_stmt.stmt); break;
    default: break;
    }
}

static void sema_check_call_spelling(Decl *d) {
    if (!d || (d->kind != DECL_FUNCTION)) return;
    spell_walk_stmt_list(d->as.function_decl.body);
}


// The front end's per-function hook. It used to be a 2,000-line ownership pass with the
// spelling walk bolted on the front under `g_suppress_ownership`; the pass is gone and the
// walk is all that is left, so the seam goes with it — there is nothing left to suppress.
static void sema_check_function_linearity(Decl *d) { sema_check_call_spelling(d); }


#endif // SEMA_OWNERSHIP_SPELLING_H
