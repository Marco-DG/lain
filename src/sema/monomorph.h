#ifndef SEMA_MONOMORPH_H
#define SEMA_MONOMORPH_H

// ─────────────────────────────────────────────────────────────────────────────
// Duck-typed monomorphization (Grail Plan Phase 1 — re-land).
//
// A type parameter is an ordinary dependent parameter whose type is the
// meta-type `type` (TYPE_META). A function/type carrying one is a generic
// TEMPLATE. At each call with concrete type arguments we produce a specialized
// instance (clone + substitute the type-param name → the concrete type), give
// it a mangled name (`max_i32`), register it, and append it to the global decl
// list — where the per-function loop (which iterates that same list) reaches it
// and runs the FULL normal pipeline on the concrete instance. Fixpoint falls
// out of append-during-iteration + dedup; instances are byte-identical to
// hand-written C (zero cost). This is the duck-typed re-land: each instance is
// checked concretely, so there is no parametric-linearity obligation.
// ─────────────────────────────────────────────────────────────────────────────

#define MONO_MAX_TPARAMS 8

// decl_is_generic_template() lives in ast.h (pure AST predicate, needed by emit too).

static bool mono_id_eq(Id *a, Id *b) {
    return a && b && a->length == b->length &&
           strncmp(a->name, b->name, (size_t)a->length) == 0;
}

// Substitution context: type-param names → concrete types.
typedef struct {
    Id   *names[MONO_MAX_TPARAMS];
    Type *concretes[MONO_MAX_TPARAMS];
    int   n;
} SubstCtx;

// Return the substituted type: a TYPE_SIMPLE whose name is a type-param becomes
// the concrete type; composite types are rewritten in place (the clone is
// freshly allocated, so mutating its element/param slots is safe — interned
// TYPE_SIMPLE leaves are never mutated, only replaced when matched).
static Type *mono_subst_type(Type *t, SubstCtx *ctx) {
    if (!t) return t;
    if (t->kind == TYPE_SIMPLE && t->base_type) {
        for (int i = 0; i < ctx->n; i++)
            if (mono_id_eq(t->base_type, ctx->names[i]))
                return ctx->concretes[i];
        return t;
    }
    if (t->element_type) t->element_type = mono_subst_type(t->element_type, ctx);
    if (t->kind == TYPE_FUNC)
        for (TypeList *p = t->func_params; p; p = p->next)
            p->type = mono_subst_type(p->type, ctx);
    return t;
}

// Forward decls for the body walkers (mutually recursive).
static void mono_subst_expr(Expr *e, SubstCtx *ctx);
static void mono_subst_stmt(Stmt *s, SubstCtx *ctx);

static void mono_subst_expr(Expr *e, SubstCtx *ctx) {
    if (!e) return;
    if (e->type) e->type = mono_subst_type(e->type, ctx);
    switch (e->kind) {
        case EXPR_TYPE:
            e->as.type_expr.type_value = mono_subst_type(e->as.type_expr.type_value, ctx);
            break;
        case EXPR_BINARY:
            mono_subst_expr(e->as.binary_expr.left, ctx);
            mono_subst_expr(e->as.binary_expr.right, ctx);
            break;
        case EXPR_UNARY:
            mono_subst_expr(e->as.unary_expr.right, ctx);
            break;
        case EXPR_CALL:
            mono_subst_expr(e->as.call_expr.callee, ctx);
            for (ExprList *a = e->as.call_expr.args; a; a = a->next)
                mono_subst_expr(a->expr, ctx);
            break;
        case EXPR_MEMBER:
            mono_subst_expr(e->as.member_expr.target, ctx);
            break;
        case EXPR_INDEX:
            mono_subst_expr(e->as.index_expr.target, ctx);
            mono_subst_expr(e->as.index_expr.index, ctx);
            break;
        case EXPR_CAST:
            e->as.cast_expr.target_type = mono_subst_type(e->as.cast_expr.target_type, ctx);
            mono_subst_expr(e->as.cast_expr.expr, ctx);
            break;
        case EXPR_MUT:
            mono_subst_expr(e->as.mut_expr.expr, ctx);
            break;
        default: break;
    }
}

static void mono_subst_stmt(Stmt *s, SubstCtx *ctx) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR:
            if (s->as.var_stmt.type) s->as.var_stmt.type = mono_subst_type(s->as.var_stmt.type, ctx);
            mono_subst_expr(s->as.var_stmt.expr, ctx);
            break;
        case STMT_ASSIGN:
            mono_subst_expr(s->as.assign_stmt.target, ctx);
            mono_subst_expr(s->as.assign_stmt.expr, ctx);
            break;
        case STMT_RETURN:
            mono_subst_expr(s->as.return_stmt.value, ctx);
            break;
        case STMT_EXPR:
            mono_subst_expr(s->as.expr_stmt.expr, ctx);
            break;
        case STMT_IF:
            mono_subst_expr(s->as.if_stmt.cond, ctx);
            for (StmtList *b = s->as.if_stmt.then_body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
            for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
            break;
        case STMT_WHILE:
            mono_subst_expr(s->as.while_stmt.cond, ctx);
            mono_subst_expr(s->as.while_stmt.measure, ctx);
            for (StmtList *b = s->as.while_stmt.body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
            break;
        case STMT_FOR:
            mono_subst_expr(s->as.for_stmt.iterable, ctx);
            for (StmtList *b = s->as.for_stmt.body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
            break;
        case STMT_MATCH:
            mono_subst_expr(s->as.match_stmt.value, ctx);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next)
                for (StmtList *b = c->body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
            break;
        case STMT_UNSAFE:
            for (StmtList *b = s->as.unsafe_stmt.body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
            break;
        default: break;
    }
}

// Append one type argument's mangling to buf (a stable, valid C identifier
// fragment): i32→"i32", *u8→"ptr_u8", T[]→"arr_i32", user Foo→"Foo".
static void mono_mangle_type(Type *t, char *buf, size_t cap) {
    if (!t) { snprintf(buf, cap, "?"); return; }
    switch (t->kind) {
        case TYPE_SIMPLE:
            snprintf(buf, cap, "%.*s", t->base_type ? (int)t->base_type->length : 1,
                     t->base_type ? t->base_type->name : "?");
            break;
        case TYPE_POINTER: {
            char inner[128]; mono_mangle_type(t->element_type, inner, sizeof inner);
            snprintf(buf, cap, "ptr_%s", inner); break;
        }
        case TYPE_ARRAY: {
            char inner[128]; mono_mangle_type(t->element_type, inner, sizeof inner);
            snprintf(buf, cap, "arr_%s", inner); break;
        }
        default: snprintf(buf, cap, "t%d", (int)t->kind); break;
    }
}

// Persist a byte range as a NUL-terminated string in the sema arena (Id stores
// the pointer, so mangled names must outlive the local buffer).
static char *mono_dup(const char *s, size_t n) {
    char *p = arena_push_many(sema_arena, char, (isize)(n + 1));
    memcpy(p, s, n); p[n] = '\0';
    return p;
}

// Build a specialized function instance: clone the template, drop the type
// parameters, substitute the type-param names → concrete types in the remaining
// parameter types, the return type, and the body; rename to inst_id.
static Decl *mono_instantiate_function(Decl *tmpl, SubstCtx *ctx, Id *inst_id) {
    Decl *inst = clone_decl(sema_arena, tmpl);
    inst->as.function_decl.name = inst_id;
    DeclList *newp = NULL, *nt = NULL;
    for (DeclList *p = inst->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pt = p->decl->as.variable_decl.type;
        if (pt && pt->kind == TYPE_META) continue;            // drop the type parameter
        p->decl->as.variable_decl.type = mono_subst_type(pt, ctx);
        DeclList *node = decl_list(sema_arena, p->decl);
        if (!newp) newp = node; else nt->next = node;
        nt = node;
    }
    inst->as.function_decl.params = newp;
    inst->as.function_decl.return_type = mono_subst_type(inst->as.function_decl.return_type, ctx);
    for (StmtList *b = inst->as.function_decl.body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
    return inst;
}

// Detect + rewrite a call to a generic function with explicit type arguments.
// Returns true iff `call` was a generic call (now rewritten to its instance).
static bool sema_monomorphize_call(Expr *call) {
    Expr *callee = call->as.call_expr.callee;
    if (!callee || callee->kind != EXPR_IDENTIFIER || !callee->decl) return false;
    Decl *tmpl = callee->decl;
    if (tmpl->kind != DECL_FUNCTION || !decl_is_generic_template(tmpl)) return false;

    Id *base = tmpl->as.function_decl.name;
    char suffix[224]; int soff = 0; suffix[0] = '\0';
    SubstCtx ctx; ctx.n = 0;
    ExprList *new_args = NULL, *na_tail = NULL;

    DeclList *p = tmpl->as.function_decl.params;
    ExprList *a = call->as.call_expr.args;
    for (; p && a; p = p->next, a = a->next) {
        Type *pt = (p->decl && p->decl->kind == DECL_VARIABLE) ? p->decl->as.variable_decl.type : NULL;
        if (pt && pt->kind == TYPE_META) {
            Expr *ta = a->expr;
            if (ta->kind != EXPR_TYPE || !ta->as.type_expr.type_value) {
                fprintf(stderr, "[E124] Error Ln %li, Col %li: type parameter '%.*s' expects a type argument.\n",
                        (long)call->line, (long)call->col,
                        (int)p->decl->as.variable_decl.name->length, p->decl->as.variable_decl.name->name);
                diagnostic_show_line(call->line, call->col); exit(1);
            }
            if (ctx.n < MONO_MAX_TPARAMS) {
                ctx.names[ctx.n] = p->decl->as.variable_decl.name;
                ctx.concretes[ctx.n] = ta->as.type_expr.type_value;
                ctx.n++;
            }
            char tb[128]; mono_mangle_type(ta->as.type_expr.type_value, tb, sizeof tb);
            soff += snprintf(suffix + soff, sizeof suffix - (size_t)soff, "_%s", tb);
        } else {
            ExprList *node = arena_push(sema_arena, ExprList);
            node->expr = a->expr; node->next = NULL;
            if (!new_args) new_args = node; else na_tail->next = node;
            na_tail = node;
        }
    }
    if (p || a) {
        fprintf(stderr, "[E124] Error Ln %li, Col %li: wrong number of arguments to generic '%.*s'.\n",
                (long)call->line, (long)call->col, (int)base->length, base->name);
        diagnostic_show_line(call->line, call->col); exit(1);
    }

    // Mangled raw name: base ⧺ suffix (e.g. "max_i32"). Dedup via the symbol table.
    char rawbuf[256];
    snprintf(rawbuf, sizeof rawbuf, "%.*s%s", (int)base->length, base->name, suffix);
    Symbol *existing = sema_lookup(rawbuf);
    Decl *inst = existing ? existing->decl : NULL;
    Id *inst_id;
    if (existing) {
        inst_id = inst->as.function_decl.name;
    } else {
        char *raw = mono_dup(rawbuf, strlen(rawbuf));
        inst_id = id(sema_arena, (isize)strlen(raw), raw);
        // cname = template's cname ⧺ suffix (e.g. "mod_max" → "mod_max_i32").
        char tmplraw[224];
        snprintf(tmplraw, sizeof tmplraw, "%.*s", (int)base->length, base->name);
        Symbol *tsym = sema_lookup(tmplraw);
        char cnamebuf[288];
        snprintf(cnamebuf, sizeof cnamebuf, "%s%s", tsym ? tsym->c_name : rawbuf, suffix);
        char *cname = mono_dup(cnamebuf, strlen(cnamebuf));
        inst = mono_instantiate_function(tmpl, &ctx, inst_id);
        sema_insert_global(raw, cname, inst->as.function_decl.return_type, inst, false);
        DeclList *node = decl_list(sema_arena, inst);
        DeclList *tail = sema_decls; while (tail && tail->next) tail = tail->next;
        if (tail) tail->next = node; else sema_decls = node;
    }

    // Rewrite the call to target the concrete instance.
    callee->as.identifier_expr.id = inst_id;
    callee->decl = inst;
    callee->type = NULL;
    call->as.call_expr.args = new_args;
    return true;
}

#endif // SEMA_MONOMORPH_H
