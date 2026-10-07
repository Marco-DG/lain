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

// Registry of generic type instances → their concrete type arguments, so
// inference can unify a `Foo(T)` parameter pattern against a `Foo_i32` argument
// (an instance is a flat TYPE_SIMPLE name that does not otherwise carry its args).
typedef struct MonoInst {
    Id   *name;                       // e.g. "Option_i32"
    Decl *tmpl, *inst;                // the template, and the instance made from it (I.172)
    Type *args[MONO_MAX_TPARAMS];     // in type-parameter order
    int   n;
    struct MonoInst *next;
} MonoInst;
static MonoInst *g_mono_insts = NULL;

static void mono_record_inst(Id *name, Decl *tmpl, Decl *inst, Type **args, int n) {
    MonoInst *m = arena_push_aligned(sema_arena, MonoInst);
    m->name = name; m->tmpl = tmpl; m->inst = inst; m->n = n < MONO_MAX_TPARAMS ? n : MONO_MAX_TPARAMS;
    for (int i = 0; i < m->n; i++) m->args[i] = args[i];
    m->next = g_mono_insts; g_mono_insts = m;
}
static MonoInst *mono_find_inst(Id *name) {
    for (MonoInst *m = g_mono_insts; m; m = m->next) if (mono_id_eq(m->name, name)) return m;
    return NULL;
}
static MonoInst *mono_find_inst_decl(Decl *inst) {
    for (MonoInst *m = g_mono_insts; m; m = m->next) if (m->inst == inst) return m;
    return NULL;
}

// Return the substituted type: a TYPE_SIMPLE whose name is a type-param becomes
// the concrete type; composite types are rewritten in place (the clone is
// freshly allocated, so mutating its element/param slots is safe — interned
// TYPE_SIMPLE leaves are never mutated, only replaced when matched).
static Type *mono_subst_type(Type *t, SubstCtx *ctx) {
    if (!t) return t;
    // A const-generic parameter in a LENGTH: `data u8[N]` in `Buf(4)` is `data u8[4]`, and a
    // bound `src u8[<= N]` is `src u8[<= 4]`, exactly as if the literal had been written.
    if ((t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) && t->array_len < 0 && t->size_expr &&
        t->size_expr->kind == EXPR_IDENTIFIER && t->size_expr->as.identifier_expr.id) {
        for (int i = 0; i < ctx->n; i++) {
            Type *c = ctx->concretes[i];
            if (!c || c->kind != TYPE_CONST || !mono_id_eq(t->size_expr->as.identifier_expr.id, ctx->names[i]))
                continue;
            if (t->size_relop != TOKEN_EQUAL_EQUAL) t->size_expr = expr_literal(sema_arena, (long long)c->array_len);
            else { t->array_len = c->array_len; t->size_expr = NULL; }
            break;
        }
    }
    if (t->kind == TYPE_SIMPLE && t->base_type) {
        for (int i = 0; i < ctx->n; i++)
            if (mono_id_eq(t->base_type, ctx->names[i]))
                return ctx->concretes[i];
        // Type-application `Option(T)` → substitute within the type arguments
        // (leaves e.g. `Option(i32)`, which the caller then resolves to Option_i32).
        for (TypeList *ta = t->type_args; ta; ta = ta->next)
            ta->type = mono_subst_type(ta->type, ctx);
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
        case EXPR_IDENTIFIER: {
            // A bare identifier naming a type parameter — used as a type argument
            // in the body, e.g. `Option(T).Some(x)` — becomes the concrete type.
            Id *nm = e->as.identifier_expr.id;
            for (int i = 0; i < ctx->n; i++)
                if (mono_id_eq(nm, ctx->names[i])) {
                    e->kind = EXPR_TYPE;
                    e->as.type_expr.type_value = ctx->concretes[i];
                    break;
                }
            break;
        }
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
        case EXPR_MATCH:
            mono_subst_expr(e->as.match_expr.value, ctx);
            for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) mono_subst_expr(p->expr, ctx);
                mono_subst_expr(c->body, ctx);
            }
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
// ★ D-17: AN INSTANCE IS KEYED BY THE TYPE, NOT BY HOW IT WAS SPELLED. `int` IS `i32` (and
// `float` is `f32`, `usize`/`isize` are `u64`/`i64` — the IR's name table lowers them to the
// same width and sign), but the mangled name came from the argument AS WRITTEN, so
// `Option(int)` and `Option(i32)` were two instances and two incompatible types in one program:
// `func f() Option(int) { return Option(i32).Some(1) }` was refused with E012.
static const char *mono_canonical_scalar(const Id *n) {
    if (!n) return NULL;
    static const char *alias[][2] = { {"int","i32"}, {"float","f32"}, {"usize","u64"},
                                      {"isize","i64"}, {NULL,NULL} };
    for (int i = 0; alias[i][0]; i++)
        if ((size_t)n->length == strlen(alias[i][0]) &&
            memcmp(n->name, alias[i][0], (size_t)n->length) == 0) return alias[i][1];
    return NULL;
}

// ★ A REFINEMENT ALIAS AS A TYPE ARGUMENT IS ITS BASE AND ITS WHOLE REFINEMENT. `Option(Small)`
// with `type Small = u8 < 200` was `Option_Small` in a signature and `Option_u8` in an expression
// (the expression read the alias's base and dropped its refinement), so no value of the one could
// be returned as the other; and the refinement is the niche (200..255 is free). The interval and
// the excluded value are read by refine_apply_clauses (ast.h), as lowering reads them, through a
// chain of aliases (`type Tiny = Small < 100`), from the base scalar's range.
static bool mono_alias_refinement(Id *n, Id **base, int64_t *lo, int64_t *hi,
                                  bool *has_ne, int64_t *ne, int depth) {
    if (!n || depth > 16 || n->length <= 0 || n->length >= 224) return false;
    char nb[224]; snprintf(nb, sizeof nb, "%.*s", (int)n->length, n->name);
    Symbol *sym = sema_lookup(nb);
    Decl *d = sym ? sym->decl : NULL;
    if (!d || d->kind != DECL_TYPE_ALIAS || !d->as.type_alias_decl.constraints) return false;
    Expr *bx = d->as.type_alias_decl.expr;                    // the base: `u8` in `u8 < 200`
    Id *bn = !bx ? NULL : bx->kind == EXPR_IDENTIFIER ? bx->as.identifier_expr.id
           : (bx->kind == EXPR_TYPE && bx->as.type_expr.type_value) ? bx->as.type_expr.type_value->base_type : NULL;
    if (!bn) return false;
    if (!mono_alias_refinement(bn, base, lo, hi, has_ne, ne, depth + 1)) {
        signed char w; bool sg;
        ast_parse_int_width(bn->name, bn->length, &w, &sg);
        if (w <= 0) return false;
        if (sg) { *lo = w >= 64 ? INT64_MIN : -(1LL << (w - 1)); *hi = w >= 64 ? INT64_MAX : (1LL << (w - 1)) - 1; }
        else    { *lo = 0;                                        *hi = w >= 64 ? INT64_MAX : (1LL << w) - 1; }
        *base = bn;
    }
    refine_apply_clauses(d->as.type_alias_decl.constraints, lo, hi, has_ne, ne);
    return true;
}
// ★ NO NAME IS WRITTEN PAST ITS BUFFER (I.172, the stopgap; chain 5 replaces the names). Every
// caller did `off += snprintf(buf + off, cap - off, ...)`: snprintf returns what it WOULD have
// written, so after one truncation `cap - off` wrapped and the next write was told it had a huge
// buffer. An append that does not fit now stops at the end of the buffer and sets mono_name_cut;
// the instantiation that built the name refuses (mono_refuse_cut).
static bool mono_name_cut = false;
static void mono_cat(char *buf, size_t cap, int *off, const char *fmt, ...) {
    if (*off < 0 || (size_t)*off >= cap) { mono_name_cut = true; return; }
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + *off, cap - (size_t)*off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - (size_t)*off) { mono_name_cut = true; *off = (int)cap - 1; return; }
    *off += n;
}
static void mono_cat_int(char *buf, size_t cap, int *off, int64_t v) {      // `-5` → `m5`
    if (v < 0) mono_cat(buf, cap, off, "m%llu", (unsigned long long)(-(v + 1)) + 1ULL);
    else       mono_cat(buf, cap, off, "%lld", (long long)v);
}

static void mono_mangle_type(Type *t, char *buf, size_t cap) {
    int o = 0; buf[0] = '\0';
    if (!t) { mono_cat(buf, cap, &o, "?"); return; }
    switch (t->kind) {
        case TYPE_SIMPLE: {
            Id *rb = NULL; int64_t rlo = 0, rhi = 0, rne = 0; bool rhas_ne = false;
            if (!t->type_args && mono_alias_refinement(t->base_type, &rb, &rlo, &rhi, &rhas_ne, &rne, 0)) {
                const char *bc = mono_canonical_scalar(rb);
                if (bc) mono_cat(buf, cap, &o, "%s_r", bc);
                else    mono_cat(buf, cap, &o, "%.*s_r", (int)rb->length, rb->name);
                mono_cat_int(buf, cap, &o, rlo);
                mono_cat(buf, cap, &o, "_");
                mono_cat_int(buf, cap, &o, rhi);
                if (rhas_ne) { mono_cat(buf, cap, &o, "_ne"); mono_cat_int(buf, cap, &o, rne); }
                break;
            }
            const char *canon = mono_canonical_scalar(t->base_type);
            if (canon) { mono_cat(buf, cap, &o, "%s", canon); break; }
            mono_cat(buf, cap, &o, "%.*s", t->base_type ? (int)t->base_type->length : 1,
                     t->base_type ? t->base_type->name : "?");
            break;
        }
        case TYPE_POINTER: {
            char inner[128]; mono_mangle_type(t->element_type, inner, sizeof inner);
            mono_cat(buf, cap, &o, "ptr_%s", inner); break;
        }
        case TYPE_ARRAY: {      // the length is part of the type: u8[4] and u8[8] are two instances
            char inner[128]; mono_mangle_type(t->element_type, inner, sizeof inner);
            if (t->array_len > 0) mono_cat(buf, cap, &o, "arr%lld_%s", (long long)t->array_len, inner);
            else mono_cat(buf, cap, &o, "arr_%s", inner);
            break;
        }
        case TYPE_CONST: mono_cat(buf, cap, &o, "%lld", (long long)t->array_len); break;
        default: mono_cat(buf, cap, &o, "t%d", (int)t->kind); break;
    }
}

// ★ AN INSTANCE IS ITS TEMPLATE AND ITS ARGUMENTS, NOT ITS NAME (I.172, the stopgap). Names are
// not injective: every function-pointer argument mangled to `t7` (`Box(*func(u8) u8)` was the
// instance `Box(*func(i32) i32)` had made, its field typed i32 -> i32), as did a sentinel slice,
// a union or a vector; `*var T` and `*T` share `ptr_T`; and `_` joins names that may hold one
// (`Pair(My_Type, X)`, `Pair(My, Type_X)`). Where a name is already taken, the arguments it was
// made from are compared structurally, as the names mean them (a refinement alias by its base
// and refinement), and a mismatch is refused, not reused.
static bool mono_arg_same(Type *a, Type *b, int depth) {
    if (depth > 32) return false;
    while (a && a->kind == TYPE_COMPTIME) a = a->element_type;
    while (b && b->kind == TYPE_COMPTIME) b = b->element_type;
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TYPE_SIMPLE: {
            Id *ab = NULL, *bb = NULL; int64_t al = 0, ah = 0, an = 0, bl = 0, bh = 0, bn = 0; bool ane = false, bne = false;
            bool ar = !a->type_args && mono_alias_refinement(a->base_type, &ab, &al, &ah, &ane, &an, 0);
            bool br = !b->type_args && mono_alias_refinement(b->base_type, &bb, &bl, &bh, &bne, &bn, 0);
            if (ar != br) return false;
            if (ar) {
                const char *ac = mono_canonical_scalar(ab), *bc = mono_canonical_scalar(bb);
                bool base_same = (ac && bc) ? strcmp(ac, bc) == 0
                               : (!ac && !bc && mono_id_eq(ab, bb));
                return base_same && al == bl && ah == bh && ane == bne && (!ane || an == bn);
            }
            const char *ac = mono_canonical_scalar(a->base_type), *bc = mono_canonical_scalar(b->base_type);
            if (ac || bc) {                   // `usize` is `u64`, as the name says
                const Id *on = ac ? b->base_type : a->base_type; const char *c = ac ? ac : bc;
                if (ac && bc) return strcmp(ac, bc) == 0;
                return on && (size_t)on->length == strlen(c) && memcmp(on->name, c, (size_t)on->length) == 0;
            }
            if (!mono_id_eq(a->base_type, b->base_type)) return false;
            TypeList *x = a->type_args, *y = b->type_args;
            for (; x && y; x = x->next, y = y->next) if (!mono_arg_same(x->type, y->type, depth + 1)) return false;
            return !x && !y;
        }
        case TYPE_POINTER:
            return a->pointee_mutable == b->pointee_mutable && mono_arg_same(a->element_type, b->element_type, depth + 1);
        case TYPE_ARRAY:
            if (a->size_expr || b->size_expr) {   // a sized slice is the same only at one literal bound
                if (!a->size_expr || !b->size_expr || a->size_relop != b->size_relop ||
                    a->size_expr->kind != EXPR_LITERAL || b->size_expr->kind != EXPR_LITERAL ||
                    a->size_expr->as.literal_expr.value != b->size_expr->as.literal_expr.value) return false;
            }
            return a->array_len == b->array_len && a->has_sentinel == b->has_sentinel &&
                   mono_arg_same(a->element_type, b->element_type, depth + 1);
        case TYPE_SLICE:
            return a->has_sentinel == b->has_sentinel && mono_arg_same(a->element_type, b->element_type, depth + 1);
        case TYPE_VECTOR:
            return a->array_len == b->array_len && mono_arg_same(a->element_type, b->element_type, depth + 1);
        case TYPE_CONST:
            return a->array_len == b->array_len;
        case TYPE_META:
            return true;
        case TYPE_FUNC: {
            if (a->func_effects != b->func_effects) return false;
            TypeList *x = a->func_params, *y = b->func_params;
            for (; x && y; x = x->next, y = y->next)       // `mov X` and `X` pass differently
                if ((x->type && y->type && x->type->mode != y->type->mode) || !mono_arg_same(x->type, y->type, depth + 1)) return false;
            return !x && !y && mono_arg_same(a->element_type, b->element_type, depth + 1);
        }
        case TYPE_UNION: {
            IdList *x = a->union_markers, *y = b->union_markers;
            for (; x && y; x = x->next, y = y->next) {
                if (!mono_id_eq(x->id, y->id)) return false;
                DeclList *f = x->fields, *g = y->fields;      // a payload marker's fields
                for (; f && g; f = f->next, g = g->next) {
                    if (!f->decl || !g->decl || f->decl->kind != DECL_VARIABLE || g->decl->kind != DECL_VARIABLE) return false;
                    if (!mono_id_eq(f->decl->as.variable_decl.name, g->decl->as.variable_decl.name) ||
                        !mono_arg_same(f->decl->as.variable_decl.type, g->decl->as.variable_decl.type, depth + 1)) return false;
                }
                if (f || g) return false;
            }
            return !x && !y && mono_arg_same(a->element_type, b->element_type, depth + 1);
        }
        default:
            return false;                 // a kind not compared is never assumed the same (fail closed)
    }
}
static bool mono_args_same(Type **a, int na, Type **b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++) if (!mono_arg_same(a[i], b[i], 0)) return false;
    return true;
}
// Where the type being resolved is written (mono_resolve_type_apps_at), or the call that
// instantiates; every refusal below names it.
static isize mono_at_line = 0, mono_at_col = 0;
static void mono_refuse_cut(const char *what) {
    if (!mono_name_cut) return;
    fprintf(stderr, "[E124] Error Ln %li, Col %li: the name of this instantiation of '%s' is longer "
            "than the compiler keeps; this is a known limitation (I.172).\n",
            (long)mono_at_line, (long)mono_at_col, what);
    diagnostic_show_line(mono_at_line, mono_at_col);
    exit(1);
}
// The name `raw` is taken by `existing`: reuse it only when it is this template's instance at
// these arguments. A name taken by a declaration that is no instance (`type Box_i32`) is I.168.
static void mono_check_existing(Decl *existing, Decl *tmpl, Type **args, int n, const char *what, const char *raw) {
    MonoInst *mi = mono_find_inst_decl(existing);
    if (mi && mi->tmpl == tmpl && mono_args_same(mi->args, mi->n, args, n)) return;
    if (mi)
        fprintf(stderr, "[E124] Error Ln %li, Col %li: two instantiations of '%s' have the same name "
                "'%s'; this is a known limitation (I.172).\n", (long)mono_at_line, (long)mono_at_col, what, raw);
    else
        fprintf(stderr, "[E124] Error Ln %li, Col %li: this instantiation of '%s' is named '%s', and a "
                "declaration has that name; this is a known limitation (I.168).\n",
                (long)mono_at_line, (long)mono_at_col, what, raw);
    diagnostic_show_line(mono_at_line, mono_at_col);
    exit(1);
}
// The arguments bound in `ctx`, in the template's type-parameter order (inference binds them in
// the order the fields name them). A template with more parameters than are kept is refused.
static int mono_ordered_args(DeclList *tparams, SubstCtx *ctx, Type **out, const char *what) {
    int n = 0;
    for (DeclList *tp = tparams; tp; tp = tp->next) {
        if (n >= MONO_MAX_TPARAMS) {
            fprintf(stderr, "[E124] Error Ln %li, Col %li: '%s' has more than %d type parameters; "
                    "this is a known limitation (I.172).\n", (long)mono_at_line, (long)mono_at_col, what, MONO_MAX_TPARAMS);
            diagnostic_show_line(mono_at_line, mono_at_col);
            exit(1);
        }
        Type *c = NULL;
        for (int j = 0; j < ctx->n; j++) if (mono_id_eq(ctx->names[j], tp->decl->as.variable_decl.name)) c = ctx->concretes[j];
        out[n++] = c;
    }
    return n;
}
// Persist a byte range as a NUL-terminated string in the sema arena (Id stores
// the pointer, so mangled names must outlive the local buffer).
static char *mono_dup(const char *s, size_t n) {
    char *p = arena_push_many(sema_arena, char, (isize)(n + 1));
    memcpy(p, s, n); p[n] = '\0';
    return p;
}

// ★ A PARAMETER'S OR RETURN'S MODE BELONGS TO THE USE, NOT TO THE TYPE ARGUMENT. `mov x T` is a
// type parameter carrying MODE_OWNED; substituting returned the concrete type as-is, at the
// default mode, so the instance's parameter became a SHARED borrow. The caller's
// `sink(Resource, mov r)` then built a temporary to lend, the instance never owned what it
// consumed, and a correct program was refused with E003 — in the CALLER. `x var T` lost its
// write-back the same way (refused with E009), and a `var T` return its borrow.
//
// Deliberately NOT applied to struct FIELDS: there the obligation is T's own (`Box(T)` is linear
// iff T is), and a `mov v T` field is what permits a box to HOLD a linear T — not a request that
// `Box(i32)` be linear.
static Type *mono_subst_keep_mode(Type *t, SubstCtx *ctx) {
    if (!t) return t;
    OwnershipMode m0 = t->mode;
    Type *r = mono_subst_type(t, ctx);
    if (r && m0 != MODE_SHARED && r->mode != m0) {
        Type *m = arena_push_aligned(sema_arena, Type);
        *m = *r; m->mode = m0;
        return m;
    }
    return r;
}

// ★ A PARAMETER'S TYPE LIVES IN ONE OF TWO PLACES. A destructuring parameter (`mov {v} Box(T)`)
// is a DECL_DESTRUCT, not a DECL_VARIABLE, and every loop in this file asked for the variable
// kind alone. So its type was never resolved (`func open(mov {v} Box(i32))` kept the GENERIC `Box`
// and emitted C gcc rejects), never substituted, and — in a generic function's instance — the
// parameter was DROPPED outright, so `unbox(T type, {v} Box(T))` counted zero value arguments.
// One accessor, used wherever a parameter's type is read or rewritten.
static Type **mono_param_type_slot(Decl *d) {
    if (!d) return NULL;
    if (d->kind == DECL_VARIABLE) return &d->as.variable_decl.type;
    if (d->kind == DECL_DESTRUCT) return &d->as.destruct_decl.type;
    return NULL;
}

// Build a specialized function instance: clone the template, drop the type
// parameters, substitute the type-param names → concrete types in the remaining
// parameter types, the return type, and the body; rename to inst_id.
static Decl *mono_instantiate_function(Decl *tmpl, SubstCtx *ctx, Id *inst_id) {
    Decl *inst = clone_decl(sema_arena, tmpl);
    inst->as.function_decl.name = inst_id;
    DeclList *newp = NULL, *nt = NULL;
    for (DeclList *p = inst->as.function_decl.params; p; p = p->next) {
        Type **slot = mono_param_type_slot(p->decl);
        if (!slot) continue;
        Type *pt = *slot;
        if (pt && pt->kind == TYPE_META) continue;            // drop the type parameter
        *slot = mono_subst_keep_mode(pt, ctx);
        DeclList *node = decl_list(sema_arena, p->decl);
        if (!newp) newp = node; else nt->next = node;
        nt = node;
    }
    inst->as.function_decl.params = newp;
    inst->as.function_decl.return_type = mono_subst_keep_mode(inst->as.function_decl.return_type, ctx);
    for (StmtList *b = inst->as.function_decl.body; b; b = b->next) mono_subst_stmt(b->stmt, ctx);
    return inst;
}

// Bind type-param `name` → concrete in ctx (idempotent; last write wins — the
// instance's own typecheck catches any genuine inconsistency).
static void mono_bind(SubstCtx *ctx, Id *name, Type *concrete) {
    if (!name || !concrete) return;
    for (int i = 0; i < ctx->n; i++)
        if (mono_id_eq(ctx->names[i], name)) return;   // already bound
    if (ctx->n < MONO_MAX_TPARAMS) { ctx->names[ctx->n] = name; ctx->concretes[ctx->n] = concrete; ctx->n++; }
}

// Reinterpret an explicit type-argument expression as a Type. A plain type name
// resolves to EXPR_TYPE; a pointer type-arg `*T` parses as EXPR_DEREF wrapping
// the element type (recursively for `**T`). Returns NULL if `e` is not a type.
static Type *resolve_type_alias(Type *t);         // typecheck.h: peels a type alias
static Type *mono_arg_to_type(Expr *e) {
    if (!e) return NULL;
    if (e->kind == EXPR_TYPE) return e->as.type_expr.type_value;
    // An alias of a type that is not a struct or an enum (`type Quad = u8[4]`) stays an
    // identifier in expression position; as a type argument it is the type it names.
    // Read off the alias's own declaration: G(Quad) is then the same instance as G(u8[4]).
    if (e->kind == EXPR_IDENTIFIER && e->decl && e->decl->kind == DECL_TYPE_ALIAS) {
        // A REFINEMENT alias is not its base: the argument is the alias, whose refinement the
        // instance carries (and its name, mono_mangle_type), as in a signature.
        if (e->decl->as.type_alias_decl.constraints && e->decl->as.type_alias_decl.name)
            return type_simple(sema_arena, e->decl->as.type_alias_decl.name);
        Expr *rx = e->decl->as.type_alias_decl.expr;
        return rx && rx != e ? mono_arg_to_type(rx) : NULL;
    }
    // `u8[4]` written as an argument parses as an index into the type: an array of that length.
    if (e->kind == EXPR_INDEX && e->as.index_expr.index && e->as.index_expr.index->kind == EXPR_LITERAL
        && e->as.index_expr.index->as.literal_expr.value > 0) {
        Type *el = mono_arg_to_type(e->as.index_expr.target);
        return el ? type_array(sema_arena, el, (isize)e->as.index_expr.index->as.literal_expr.value) : NULL;
    }
    // `usize` / `isize` are builtin types but deliberately NOT rewritten to type-values by the
    // resolver, so they stay legal as binding names (tests/errors/near_type_name_var_pass.ln).
    // In a type-argument position, a name that is NOT bound is the type: `Box(usize, 5)` was
    // refused with "expects a leading type argument". A local named `usize` still wins.
    if (e->kind == EXPR_IDENTIFIER && !e->decl && e->as.identifier_expr.id) {
        Id *n = e->as.identifier_expr.id;
        bool is_sz = n->length == 5 && (memcmp(n->name, "usize", 5) == 0 || memcmp(n->name, "isize", 5) == 0);
        if (is_sz) {
            char raw[6]; memcpy(raw, n->name, 5); raw[5] = '\0';
            if (!sema_lookup(raw)) return type_simple(sema_arena, n);
        }
    }
    if (e->kind == EXPR_DEREF) {
        Type *inner = mono_arg_to_type(e->as.deref_expr.expr);
        return inner ? type_pointer(sema_arena, inner) : NULL;
    }
    return NULL;
}

// Structurally unify a parameter's type pattern against a concrete argument type,
// binding any type-param names it mentions (T, *T[], etc.).
static Type *mono_const_type(long long v) {
    Type *c = arena_push_aligned(sema_arena, Type);
    memset(c, 0, sizeof *c);
    c->kind = TYPE_CONST; c->array_len = (isize)v; c->size_expr = expr_literal(sema_arena, v);
    return c;
}
static void mono_unify(Type *pat, Type *arg, SubstCtx *ctx, Id **tp, int ntp) {
    if (!pat || !arg) return;
    // A const-generic LENGTH is inferred from an argument of known length: the field
    // `data u8[N]` against `[1, 2, 3, 4]` (a `u8[4]`) binds N = 4.
    if (pat->kind == TYPE_ARRAY && pat->array_len < 0 && pat->size_expr &&
        pat->size_expr->kind == EXPR_IDENTIFIER && pat->size_expr->as.identifier_expr.id &&
        arg->kind == TYPE_ARRAY && arg->array_len >= 0) {
        Id *nm = pat->size_expr->as.identifier_expr.id;
        for (int i = 0; i < ntp; i++)
            if (mono_id_eq(nm, tp[i])) {
                bool bound = false;
                for (int j = 0; j < ctx->n; j++) if (mono_id_eq(ctx->names[j], nm)) bound = true;
                if (!bound) mono_bind(ctx, nm, mono_const_type((long long)arg->array_len));
                break;
            }
    }
    if (pat->kind == TYPE_SIMPLE && pat->base_type) {
        for (int i = 0; i < ntp; i++)
            if (mono_id_eq(pat->base_type, tp[i])) { mono_bind(ctx, pat->base_type, arg); return; }
        // Type-application pattern `Foo(T..)` vs an instance argument `Foo_i32`:
        // recover the instance's concrete type args and unify positionally.
        if (pat->type_args && arg->kind == TYPE_SIMPLE && arg->base_type) {
            MonoInst *info = mono_find_inst(arg->base_type);
            if (info) {
                int i = 0;
                for (TypeList *pa = pat->type_args; pa && i < info->n; pa = pa->next, i++)
                    mono_unify(pa->type, info->args[i], ctx, tp, ntp);
            }
        }
        return;   // concrete type in the pattern → nothing to bind
    }
    if (pat->kind == TYPE_FUNC && arg->kind == TYPE_FUNC) {
        TypeList *pp = pat->func_params, *ap = arg->func_params;
        while (pp && ap) { mono_unify(pp->type, ap->type, ctx, tp, ntp); pp = pp->next; ap = ap->next; }
    }
    if (pat->element_type && arg->element_type)   // pointer/array element, or fn-ptr return
        mono_unify(pat->element_type, arg->element_type, ctx, tp, ntp);
}

// Clone + specialize a generic struct/enum: substitute the type parameters in
// every field (and, for enums, every variant field) type; rename; drop the
// Get-or-create the concrete instance of a generic type for the bound type args
// (dedup via the symbol table). The instance SHELL is cloned, renamed, and
// registered BEFORE its fields are specialized, so a self-referential generic
// (`Node(T){ next *Node(T) }`) dedups to the in-progress instance instead of
// recursing forever. Appended to the decl list, where the per-decl passes + emit
// reach it. Returns the instance decl.
static Decl *mono_type_instance(Decl *tmpl, SubstCtx *ctx, const char *suffix) {
    Id *base = (tmpl->kind == DECL_STRUCT) ? tmpl->as.struct_decl.name : tmpl->as.enum_decl.type_name;
    char what[128]; snprintf(what, sizeof what, "%.*s", (int)base->length, base->name);
    Type *oargs[MONO_MAX_TPARAMS];
    int on = mono_ordered_args(tmpl->kind == DECL_STRUCT ? tmpl->as.struct_decl.type_params
                                                         : tmpl->as.enum_decl.type_params, ctx, oargs, what);
    char rawbuf[256]; int ro = 0; rawbuf[0] = '\0';
    mono_cat(rawbuf, sizeof rawbuf, &ro, "%.*s%s", (int)base->length, base->name, suffix);
    mono_refuse_cut(what);                  // the suffix the caller built, and this name
    Symbol *existing = sema_lookup(rawbuf);
    if (existing) {
        mono_check_existing(existing->decl, tmpl, oargs, on, what, rawbuf);
        return existing->decl;
    }
    char *raw = mono_dup(rawbuf, strlen(rawbuf));
    Id *inst_id = id(sema_arena, (isize)strlen(raw), raw);
    char tmplraw[224]; snprintf(tmplraw, sizeof tmplraw, "%.*s", (int)base->length, base->name);
    Symbol *tsym = sema_lookup(tmplraw);
    char cnamebuf[288]; int co = 0; cnamebuf[0] = '\0';
    mono_cat(cnamebuf, sizeof cnamebuf, &co, "%s%s", tsym ? tsym->c_name : rawbuf, suffix);
    mono_refuse_cut(what);
    char *cname = mono_dup(cnamebuf, strlen(cnamebuf));

    // 1) clone + rename + drop the header (the shell).
    Decl *inst = clone_decl(sema_arena, tmpl);
    if (inst->kind == DECL_STRUCT) { inst->as.struct_decl.name = inst_id; inst->as.struct_decl.type_params = NULL; }
    else                          { inst->as.enum_decl.type_name = inst_id; inst->as.enum_decl.type_params = NULL; }

    // 2) register + append BEFORE specializing fields (breaks self-reference).
    Type *ity = type_simple(sema_arena, inst_id);
    sema_insert_global(raw, cname, ity, inst, false);
    mono_record_inst(inst_id, tmpl, inst, oargs, on);   // for inference: Foo_i32 → [i32]
    DeclList *node = decl_list(sema_arena, inst);
    DeclList *tail = sema_decls; while (tail && tail->next) tail = tail->next;
    if (tail) tail->next = node; else sema_decls = node;

    // 3) specialize each field: substitute the type params, then resolve any
    //    nested generic type-application (`inner Option(T)` → Option_i32).
    if (inst->kind == DECL_STRUCT) {
        for (DeclList *f = inst->as.struct_decl.fields; f; f = f->next)
            if (f->decl && f->decl->kind == DECL_VARIABLE)
                f->decl->as.variable_decl.type =
                    mono_resolve_type_apps_at(mono_subst_type(f->decl->as.variable_decl.type, ctx), f->decl->line, f->decl->col);
    } else if (inst->kind == DECL_ENUM) {
        for (Variant *v = inst->as.enum_decl.variants; v; v = v->next)
            for (DeclList *f = v->fields; f; f = f->next)
                if (f->decl && f->decl->kind == DECL_VARIABLE)
                    f->decl->as.variable_decl.type =
                        mono_resolve_type_apps_at(mono_subst_type(f->decl->as.variable_decl.type, ctx), f->decl->line, f->decl->col);
    }
    return inst;
}

// Resolve generic type-applications in a type: `Vec(i32)` in a signature/field
// becomes the concrete instance type. Recurses into nested applications and
// composite types. Returns the (possibly rewritten) type.
// Lower `T | m1 | m2` (TYPE_UNION) to a niche-optimized anonymous enum: one
// payload variant `some { __v: T }` + one empty variant per marker. Deduped by a
// deterministic mangled name so the same union in two signatures shares one enum.
// ZERO-COST MANDATORY: the IR refuses (E064) a layout that needs a tag (ir_emit_layout_report).
// The synthesized enum's name: the value type and every marker (with a payload marker's field
// types). It IS the union's identity, so lowering finds the enum by this name too (I.106).
static void union_mangled_name(Type *u, char *nb, size_t cap) {
    Type *value = u->element_type;
    int off = 0; nb[0] = '\0';
    char vb[128]; mono_mangle_type(value, vb, sizeof vb);
    mono_cat(nb, cap, &off, "__U_%s", vb);
    for (IdList *m = u->union_markers; m; m = m->next) {
        mono_cat(nb, cap, &off, "_%.*s", (int)m->id->length, m->id->name);
        // Payload markers mangle their field types too, so `E{line u32}` and
        // `E{col u16}` are distinct unions (no dedup collision).
        for (DeclList *f = m->fields; f; f = f->next) {
            if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
            char fb[128]; mono_mangle_type(f->decl->as.variable_decl.type, fb, sizeof fb);
            mono_cat(nb, cap, &off, "_%s", fb);
        }
    }
}

// The union `u` is the one the enum `ed` was synthesized for: the same value type, the same
// markers in the same order, each with the same payload fields (name and type). The name alone
// said so for `T | m` and `U | m` whenever T and U mangled alike (I.172).
static bool union_is_lowered_as(Decl *ed, Type *u) {
    if (!ed || ed->kind != DECL_ENUM || !ed->as.enum_decl.is_union) return false;
    Variant *pv = ed->as.enum_decl.variants;
    if (!pv || !pv->fields || !pv->fields->decl) return false;
    if (!mono_arg_same(pv->fields->decl->as.variable_decl.type, u->element_type, 0)) return false;
    Variant *v = pv->next; IdList *m = u->union_markers;
    for (; v && m; v = v->next, m = m->next) {
        if (!mono_id_eq(v->name, m->id)) return false;
        DeclList *x = v->fields, *y = m->fields;
        for (; x && y; x = x->next, y = y->next) {
            if (!x->decl || !y->decl || x->decl->kind != DECL_VARIABLE || y->decl->kind != DECL_VARIABLE) return false;
            if (!mono_id_eq(x->decl->as.variable_decl.name, y->decl->as.variable_decl.name)) return false;
            if (!mono_arg_same(x->decl->as.variable_decl.type, y->decl->as.variable_decl.type, 0)) return false;
        }
        if (x || y) return false;
    }
    return !v && !m;
}

static Type *union_lower(Type *u) {
    Type *value = u->element_type;
    char nb[256];
    mono_name_cut = false;
    union_mangled_name(u, nb, sizeof nb);
    mono_refuse_cut("a union");
    Symbol *ex = sema_lookup(nb);
    if (ex) {
        if (!union_is_lowered_as(ex->decl, u)) {
            fprintf(stderr, "[E124] Error Ln %li, Col %li: two different unions have the same name '%s'; "
                    "this is a known limitation (I.172).\n", (long)mono_at_line, (long)mono_at_col, nb);
            diagnostic_show_line(mono_at_line, mono_at_col);
            exit(1);
        }
        return type_simple(sema_arena, ex->decl->as.enum_decl.type_name);
    }

    char *raw = mono_dup(nb, strlen(nb));
    Id *ename = id(sema_arena, (isize)strlen(raw), raw);

    // payload variant `__payload { __v : value }`, then one empty variant per
    // marker. The payload variant's name is a compiler-internal detail: user
    // code never spells it — the value is reached with the `else` arm of a
    // `case` (which narrows the scrutinee to the value type), never `some(v)`.
    Variant *pv = arena_push_aligned(sema_arena, Variant);
    pv->name   = id(sema_arena, 9, "__payload");
    pv->fields = decl_list(sema_arena, decl_variable(sema_arena, id(sema_arena, 3, "__v"), value));
    pv->next   = NULL;
    Variant *tail = pv;
    for (IdList *m = u->union_markers; m; m = m->next) {
        Variant *mv = arena_push_aligned(sema_arena, Variant);
        mv->name = m->id; mv->fields = m->fields; mv->next = NULL;   // payload fields (or NULL)
        tail->next = mv; tail = mv;
    }

    Decl *ed = arena_push_aligned(sema_arena, Decl);
    ed->kind = DECL_ENUM;
    ed->as.enum_decl.type_name   = ename;
    ed->as.enum_decl.variants    = pv;
    ed->as.enum_decl.type_params = NULL;
    ed->as.enum_decl.is_union    = true;
    ed->as.enum_decl.is_ordered  = false;

    Type *ity = type_simple(sema_arena, ename);
    sema_insert_global(raw, raw, ity, ed, false);
    DeclList *node = decl_list(sema_arena, ed);
    DeclList *tl = sema_decls; while (tl && tl->next) tl = tl->next;
    if (tl) tl->next = node; else sema_decls = node;

    // Zero cost is MANDATORY for a union whose markers carry no payload (E064), and whether a
    // layout achieves it is ir/layout.h's answer, asked once the union is an IR type
    // (ir_emit_layout_report). Asking sema/niche.h here was a second answer to the same
    // question, and two answers are how D-62 happened.
    return ity;
}

// The struct/enum type an alias names, resolved — or NULL when `n` is not an alias of one.
static Type *mono_resolve_type_apps(Type *t);
// Where the type being resolved is written. A Type carries no position, so E124 in a type
// (`g G(7)`, `Plain(i32)`) said only "Error:"; each caller that resolves a written type names the
// construct it is written in (the parameter, the field, the statement), and the recursion inside
// keeps it. (mono_at_line and mono_at_col are declared with the name refusals, which read them.)
static Type *mono_resolve_type_apps_at(Type *t, isize line, isize col) {
    isize sl = mono_at_line, sc = mono_at_col;
    if (line > 0) { mono_at_line = line; mono_at_col = col; }
    Type *r = mono_resolve_type_apps(t);
    mono_at_line = sl; mono_at_col = sc;
    return r;
}
static void sema_bind_const_names(Expr *e, int depth);   // sema.h: names in a constant, bound
static Type *mono_alias_target(Id *n) {
    if (!n || n->length >= 224) return NULL;
    char nb[224]; snprintf(nb, sizeof nb, "%.*s", (int)n->length, n->name);
    Symbol *sym = sema_lookup(nb);
    if (!sym || !sym->decl || sym->decl->kind != DECL_TYPE_ALIAS || !sym->type) return NULL;
    Type *at = sym->type;
    if (at->kind != TYPE_SIMPLE || !at->base_type) return NULL;
    if (at->type_args) at = mono_resolve_type_apps(at);
    if (!at || !at->base_type || at->base_type->length >= 224) return NULL;
    char tb[224]; snprintf(tb, sizeof tb, "%.*s", (int)at->base_type->length, at->base_type->name);
    Symbol *ts = sema_lookup(tb);
    if (!ts || !ts->decl || (ts->decl->kind != DECL_STRUCT && ts->decl->kind != DECL_ENUM)) return NULL;
    if (decl_is_generic_template(ts->decl)) return NULL;
    return at;
}

static Type *mono_resolve_type_apps(Type *t) {
    if (!t) return t;
    if (t->kind == TYPE_UNION) {                 // `T | markers` → niche'd anonymous enum
        t->element_type = mono_resolve_type_apps(t->element_type);
        return union_lower(t);
    }
    if (t->kind == TYPE_SIMPLE && t->type_args && t->base_type) {
        for (TypeList *ta = t->type_args; ta; ta = ta->next) ta->type = mono_resolve_type_apps(ta->type);
        char nb[224]; snprintf(nb, sizeof nb, "%.*s", (int)t->base_type->length, t->base_type->name);
        Symbol *sym = sema_lookup(nb);
        if (!sym || !sym->decl || !decl_is_generic_template(sym->decl) ||
            (sym->decl->kind != DECL_STRUCT && sym->decl->kind != DECL_ENUM)) {
            fprintf(stderr, "[E124] Error Ln %li, Col %li: '%.*s' is not a generic type.\n",
                    (long)mono_at_line, (long)mono_at_col, (int)t->base_type->length, t->base_type->name);
            exit(1);
        }
        Decl *tmpl = sym->decl;
        DeclList *tparams = (tmpl->kind == DECL_STRUCT) ? tmpl->as.struct_decl.type_params
                                                        : tmpl->as.enum_decl.type_params;
        SubstCtx ctx; ctx.n = 0; char suffix[224]; int soff = 0; suffix[0] = '\0';
        mono_name_cut = false;
        TypeList *ta = t->type_args;
        for (DeclList *tp = tparams; tp && ta; tp = tp->next, ta = ta->next) {
            Type *pty = tp->decl->as.variable_decl.type;
            Type *arg = ta->type;
            Id *pnm = tp->decl->as.variable_decl.name;
            if (pty && pty->kind != TYPE_META) {
                // ★ A CONST-GENERIC PARAMETER TAKES A VALUE: `type Buf(N usize)` is instantiated
                // by `Buf(4)` or by a module constant, `Buf(SIZE)`, evaluated as a module assert
                // is (DECIDE-O). Every spelling was refused, by the parser ("Expected type name")
                // or here, while const generics were documented as shipped.
                Expr *ve = NULL;
                if (arg && arg->kind == TYPE_CONST) ve = arg->size_expr;
                else if (arg && arg->kind == TYPE_SIMPLE && arg->base_type && !arg->type_args) {
                    ve = expr_identifier(sema_arena, arg->base_type);
                    sema_bind_const_names(ve, 0);
                }
                bool lay = false; __int128 v = 0;
                if (!ve || !sa_is_const(ve, &lay) || lay || !sa_eval(ve, &v) || v < 0 || v > (__int128)INT64_MAX) {
                    fprintf(stderr, "[E124] Error Ln %li, Col %li: the argument for '%.*s' of '%.*s' must be a "
                            "non-negative constant: a literal or a module constant.\n",
                            (long)mono_at_line, (long)mono_at_col, (int)(pnm ? pnm->length : 0), pnm ? pnm->name : "",
                            (int)t->base_type->length, t->base_type->name);
                    exit(1);
                }
                Type *c = arena_push_aligned(sema_arena, Type);
                memset(c, 0, sizeof *c);
                c->kind = TYPE_CONST; c->array_len = (isize)v; c->size_expr = expr_literal(sema_arena, (long long)v);
                arg = c; ta->type = c;
            } else if (arg && arg->kind == TYPE_CONST) {
                fprintf(stderr, "[E124] Error Ln %li, Col %li: '%.*s' of '%.*s' is a type parameter, and %lld is a value.\n",
                        (long)mono_at_line, (long)mono_at_col, (int)(pnm ? pnm->length : 0), pnm ? pnm->name : "",
                        (int)t->base_type->length, t->base_type->name,
                        arg->size_expr && arg->size_expr->kind == EXPR_LITERAL ? (long long)arg->size_expr->as.literal_expr.value : 0LL);
                exit(1);
            } else if (arg && arg->kind == TYPE_SIMPLE && !arg->type_args) {
                // An alias of an ARRAY type is the array, so `g G(Quad)` names the instance that
                // `G(Quad)` and `G(u8[4])` build in an expression. (Other aliases keep their name:
                // a refinement alias is not its base type.)
                Type *pa = resolve_type_alias(arg);
                if (pa && pa->kind == TYPE_ARRAY && pa->array_len > 0) { arg = pa; ta->type = pa; }
            }
            mono_bind(&ctx, pnm, arg);
            char tb[128]; mono_mangle_type(arg, tb, sizeof tb);
            mono_cat(suffix, sizeof suffix, &soff, "_%s", tb);
        }
        Decl *inst = mono_type_instance(tmpl, &ctx, suffix);
        Id *iname = (inst->kind == DECL_STRUCT) ? inst->as.struct_decl.name : inst->as.enum_decl.type_name;
        return type_simple(sema_arena, iname);
    }
    // ★ D-18: AN ALIAS OF A STRUCT OR ENUM IS THAT TYPE. `type OptInt = Option(i32)` declared a
    // name that worked nowhere the type did: as a parameter (`o OptInt`) a `case` on it was
    // "non-exhaustive" because nothing mapped the name to the enum, and as a constructor
    // (`OptInt.Some(42)`) it fell to direct field access (E125). Resolve it to the type it names
    // — the use's mode kept. A REFINEMENT alias (`type Small = i32 >= 0 …`) names no struct or
    // enum and is untouched: its constraints live on the alias and are read from there.
    if (t->kind == TYPE_SIMPLE && !t->type_args && t->base_type) {
        Type *at = mono_alias_target(t->base_type);
        if (at) {
            if (at->mode == t->mode) return at;
            Type *m = arena_push_aligned(sema_arena, Type);
            *m = *at; m->mode = t->mode;
            return m;
        }
    }
    if (t->element_type) t->element_type = mono_resolve_type_apps(t->element_type);
    return t;
}

// Resolve type-applications across a function's signature (param + return types).
static void mono_resolve_signature(Decl *d) {
    if (!d || (d->kind != DECL_FUNCTION)) return;
    for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
        Type **slot = mono_param_type_slot(p->decl);
        if (slot) *slot = mono_resolve_type_apps_at(*slot, p->decl->line, p->decl->col);
    }
    d->as.function_decl.return_type = mono_resolve_type_apps_at(d->as.function_decl.return_type, d->line, d->col);
}

// Generic struct construction. Type args may be explicit and leading
// (`Pair(i32, 3, 5)`) or inferred from the field values (`Pair(3, 5)` — unify
// each field's type pattern against its argument). Instantiates Pair_i32 and
// rewrites the call to construct the concrete struct.
static bool mono_construct_generic_struct(Expr *call, Decl *tmpl) {
    Expr *callee = call->as.call_expr.callee;
    DeclList *tparams = tmpl->as.struct_decl.type_params;
    DeclList *fields  = tmpl->as.struct_decl.fields;
    Id *base = tmpl->as.struct_decl.name;

    Id *tp_names[MONO_MAX_TPARAMS]; int ntp = 0;
    for (DeclList *tp = tparams; tp; tp = tp->next)
        if (ntp < MONO_MAX_TPARAMS) tp_names[ntp++] = tp->decl->as.variable_decl.name;
    int nf = 0;    for (DeclList *f = fields; f; f = f->next) nf++;
    int nargs = 0; for (ExprList *a = call->as.call_expr.args; a; a = a->next) nargs++;

    SubstCtx ctx; ctx.n = 0;
    ExprList *field_args;

    if (nargs == ntp + nf) {
        // Explicit: leading `ntp` args are the type arguments.
        ExprList *a = call->as.call_expr.args;
        DeclList *tpd = tparams;
        for (int i = 0; i < ntp; i++, a = a->next, tpd = tpd ? tpd->next : NULL) {
            Type *pty = (tpd && tpd->decl) ? tpd->decl->as.variable_decl.type : NULL;
            if (pty && pty->kind != TYPE_META) {          // a VALUE parameter: `Buf(4, [..])`
                bool lay = false; __int128 v = 0;
                sema_bind_const_names(a->expr, 0);
                if (!sa_is_const(a->expr, &lay) || lay || !sa_eval(a->expr, &v) || v < 0 || v > (__int128)INT64_MAX) {
                    fprintf(stderr, "[E124] Error Ln %li, Col %li: the argument for '%.*s' of '%.*s' must be a "
                            "non-negative constant: a literal or a module constant.\n", (long)call->line,
                            (long)call->col, (int)tp_names[i]->length, tp_names[i]->name, (int)base->length, base->name);
                    diagnostic_show_line(call->line, call->col); exit(1);
                }
                mono_bind(&ctx, tp_names[i], mono_const_type((long long)v));
                continue;
            }
            Type *ta = mono_arg_to_type(a->expr);
            if (!ta) {
                fprintf(stderr, "[E124] Error Ln %li, Col %li: generic type '%.*s' expects a leading type argument.\n",
                        (long)call->line, (long)call->col, (int)base->length, base->name);
                diagnostic_show_line(call->line, call->col); exit(1);
            }
            mono_bind(&ctx, tp_names[i], ta);
        }
        field_args = a;
    } else if (nargs == nf) {
        // Inferred: unify each field's type pattern against its argument.
        ExprList *a = call->as.call_expr.args;
        for (DeclList *f = fields; f && a; f = f->next, a = a->next) {
            sema_infer_expr(a->expr);
            if (f->decl && f->decl->kind == DECL_VARIABLE)
                mono_unify(f->decl->as.variable_decl.type, a->expr->type, &ctx, tp_names, ntp);
        }
        for (int i = 0; i < ntp; i++) {
            bool bound = false;
            for (int j = 0; j < ctx.n; j++) if (mono_id_eq(ctx.names[j], tp_names[i])) bound = true;
            if (!bound) {
                fprintf(stderr, "[E124] Error Ln %li, Col %li: cannot infer type parameter '%.*s' of '%.*s' "
                        "from the field values — pass it explicitly.\n",
                        (long)call->line, (long)call->col, (int)tp_names[i]->length, tp_names[i]->name,
                        (int)base->length, base->name);
                diagnostic_show_line(call->line, call->col); exit(1);
            }
        }
        field_args = call->as.call_expr.args;
    } else {
        fprintf(stderr, "[E124] Error Ln %li, Col %li: wrong number of arguments to construct generic '%.*s' "
                "(expected %d field values, with %d type argument(s) explicit or inferred).\n",
                (long)call->line, (long)call->col, (int)base->length, base->name, nf, ntp);
        diagnostic_show_line(call->line, call->col); exit(1);
    }

    char suffix[224]; int soff = 0; suffix[0] = '\0';
    mono_name_cut = false;
    for (int i = 0; i < ntp; i++) {
        Type *concrete = NULL;
        for (int j = 0; j < ctx.n; j++) if (mono_id_eq(ctx.names[j], tp_names[i])) concrete = ctx.concretes[j];
        char tb[128]; mono_mangle_type(concrete, tb, sizeof tb);
        mono_cat(suffix, sizeof suffix, &soff, "_%s", tb);
    }

    call->as.call_expr.args = field_args;
    isize sl = mono_at_line, sc = mono_at_col;
    mono_at_line = call->line; mono_at_col = call->col;
    Decl *inst = mono_type_instance(tmpl, &ctx, suffix);
    mono_at_line = sl; mono_at_col = sc;
    Type *ity = type_simple(sema_arena, inst->as.struct_decl.name);
    callee->decl = inst;
    callee->type = ity;
    if (callee->kind == EXPR_TYPE) callee->as.type_expr.type_value = ity;
    else {
        callee->as.identifier_expr.id = inst->as.struct_decl.name;
        callee->as.identifier_expr.via_qualifier = true;  // synthesized instance: exempt from glob-retirement
    }
    return true;
}

// Generic enum reference `Option(i32)` (all args are type args) → instantiate
// Option_i32 and rewrite the call node into an EXPR_TYPE for the instance, so a
// following `.Some(5)` / `.None` resolves as a variant of the concrete enum.
static bool mono_ref_generic_enum(Expr *call, Decl *tmpl) {
    DeclList *tparams = tmpl->as.enum_decl.type_params;
    Id *base = tmpl->as.enum_decl.type_name;
    SubstCtx ctx; ctx.n = 0; char suffix[224]; int soff = 0; suffix[0] = '\0';
    mono_name_cut = false;
    ExprList *a = call->as.call_expr.args;
    for (DeclList *tp = tparams; tp; tp = tp->next) {
        Type *ta = a ? mono_arg_to_type(a->expr) : NULL;
        if (!ta) {
            fprintf(stderr, "[E124] Error Ln %li, Col %li: generic type '%.*s' expects a type argument.\n",
                    (long)call->line, (long)call->col, (int)base->length, base->name);
            diagnostic_show_line(call->line, call->col); exit(1);
        }
        mono_bind(&ctx, tp->decl->as.variable_decl.name, ta);
        char tb[128]; mono_mangle_type(ta, tb, sizeof tb);
        mono_cat(suffix, sizeof suffix, &soff, "_%s", tb);
        a = a->next;
    }
    isize sl = mono_at_line, sc = mono_at_col;
    mono_at_line = call->line; mono_at_col = call->col;
    Decl *inst = mono_type_instance(tmpl, &ctx, suffix);
    mono_at_line = sl; mono_at_col = sc;
    Type *ity = type_simple(sema_arena, inst->as.enum_decl.type_name);
    call->kind = EXPR_TYPE;
    call->as.type_expr.type_value = ity;
    call->decl = inst;
    call->type = ity;
    return true;
}

// Detect + rewrite a call to a generic function. Type arguments may be passed
// explicitly (`max(i32, 3, 5)`) or inferred from the value arguments
// (`max(3, 5)`). Returns true iff `call` was a generic call (now rewritten).
static bool sema_monomorphize_call(Expr *call) {
    Expr *callee = call->as.call_expr.callee;
    if (!callee || (callee->kind != EXPR_IDENTIFIER && callee->kind != EXPR_TYPE) || !callee->decl)
        return false;
    Decl *tmpl = callee->decl;
    if (!decl_is_generic_template(tmpl)) return false;
    if (tmpl->kind == DECL_STRUCT) return mono_construct_generic_struct(call, tmpl);
    if (tmpl->kind == DECL_ENUM) {
        // Only a reference to the enum's OWN name is a type-application
        // (`Option(i32)`). A variant pattern/constructor like `Some(v)` also
        // carries the enum decl but must NOT be treated as `Enum(typeargs)`.
        bool is_enum_ref = (callee->kind == EXPR_TYPE) ||
            (callee->kind == EXPR_IDENTIFIER &&
             mono_id_eq(callee->as.identifier_expr.id, tmpl->as.enum_decl.type_name));
        return is_enum_ref ? mono_ref_generic_enum(call, tmpl) : false;
    }
    if (tmpl->kind != DECL_FUNCTION) return false;

    Id *base = tmpl->as.function_decl.name;

    // Partition params into type params (meta) and value params, in order.
    Id   *tp_names[MONO_MAX_TPARAMS]; int ntp = 0;
    DeclList *vparams[64]; int nvp = 0;
    for (DeclList *p = tmpl->as.function_decl.params; p; p = p->next) {
        Type **slot = mono_param_type_slot(p->decl);
        if (!slot) continue;
        Type *pt = *slot;
        if (pt && pt->kind == TYPE_META && p->decl->kind == DECL_VARIABLE) {
            if (ntp < MONO_MAX_TPARAMS) tp_names[ntp++] = p->decl->as.variable_decl.name;
        }
        else if (nvp < 64) vparams[nvp++] = p;
    }

    // Count arguments.
    int nargs = 0; for (ExprList *a = call->as.call_expr.args; a; a = a->next) nargs++;

    SubstCtx ctx; ctx.n = 0;
    ExprList *new_args = NULL, *na_tail = NULL;

    if (nargs == ntp + nvp) {
        // Explicit mode: leading `ntp` args are the type arguments.
        ExprList *a = call->as.call_expr.args;
        for (int i = 0; i < ntp; i++, a = a->next) {
            Type *ta = mono_arg_to_type(a->expr);
            if (!ta) {
                fprintf(stderr, "[E124] Error Ln %li, Col %li: type parameter '%.*s' expects a type argument.\n",
                        (long)call->line, (long)call->col, (int)tp_names[i]->length, tp_names[i]->name);
                diagnostic_show_line(call->line, call->col); exit(1);
            }
            mono_bind(&ctx, tp_names[i], ta);
        }
        for (; a; a = a->next) {           // remaining args are the value args
            ExprList *node = arena_push(sema_arena, ExprList);
            node->expr = a->expr; node->next = NULL;
            if (!new_args) new_args = node; else na_tail->next = node;
            na_tail = node;
        }
    } else if (nargs == nvp) {
        // Inferred mode: type arguments omitted → infer from the value args.
        ExprList *a = call->as.call_expr.args;
        for (int i = 0; i < nvp && a; i++, a = a->next) {
            sema_infer_expr(a->expr);      // ensure the arg has a type to unify against
            Type *argt = a->expr->type;
            // A bare function name passed as an argument has no fn-ptr type yet;
            // synthesize it so a `*func(T) U` parameter can bind T and U.
            if ((!argt || argt->kind != TYPE_FUNC) && a->expr->decl) {
                Type *ft = fnptr_type_of_decl(a->expr->decl);
                if (ft) argt = ft;
            }
            mono_unify(*mono_param_type_slot(vparams[i]->decl), argt, &ctx, tp_names, ntp);
            ExprList *node = arena_push(sema_arena, ExprList);
            node->expr = a->expr; node->next = NULL;
            if (!new_args) new_args = node; else na_tail->next = node;
            na_tail = node;
        }
        // Every type parameter must have been inferred.
        for (int i = 0; i < ntp; i++) {
            bool bound = false;
            for (int j = 0; j < ctx.n; j++) if (mono_id_eq(ctx.names[j], tp_names[i])) bound = true;
            if (!bound) {
                fprintf(stderr, "[E124] Error Ln %li, Col %li: cannot infer type parameter '%.*s' of '%.*s' "
                        "from the arguments — pass it explicitly.\n",
                        (long)call->line, (long)call->col, (int)tp_names[i]->length, tp_names[i]->name,
                        (int)base->length, base->name);
                diagnostic_show_line(call->line, call->col); exit(1);
            }
        }
    } else {
        fprintf(stderr, "[E124] Error Ln %li, Col %li: wrong number of arguments to generic '%.*s' "
                "(expected %d value arguments, with %d type argument(s) explicit or inferred).\n",
                (long)call->line, (long)call->col, (int)base->length, base->name, nvp, ntp);
        diagnostic_show_line(call->line, call->col); exit(1);
    }

    // Build the mangled suffix in type-parameter order (stable across explicit/inferred).
    char suffix[224]; int soff = 0; suffix[0] = '\0';
    mono_name_cut = false;
    Type *oargs[MONO_MAX_TPARAMS];
    for (int i = 0; i < ntp; i++) {
        Type *concrete = NULL;
        for (int j = 0; j < ctx.n; j++) if (mono_id_eq(ctx.names[j], tp_names[i])) concrete = ctx.concretes[j];
        oargs[i] = concrete;
        char tb[128]; mono_mangle_type(concrete, tb, sizeof tb);
        mono_cat(suffix, sizeof suffix, &soff, "_%s", tb);
    }

    // Mangled raw name: base ⧺ suffix (e.g. "max_i32"). Dedup via the symbol table.
    char what[128]; snprintf(what, sizeof what, "%.*s", (int)base->length, base->name);
    isize sl = mono_at_line, sc = mono_at_col;
    mono_at_line = call->line; mono_at_col = call->col;
    char rawbuf[256]; int ro = 0; rawbuf[0] = '\0';
    mono_cat(rawbuf, sizeof rawbuf, &ro, "%.*s%s", (int)base->length, base->name, suffix);
    mono_refuse_cut(what);
    Symbol *existing = sema_lookup(rawbuf);
    if (existing) mono_check_existing(existing->decl, tmpl, oargs, ntp, what, rawbuf);
    Decl *inst = existing ? existing->decl : NULL;
    Id *inst_id;
    if (existing) {
        // ★ THE SYMBOL IS KEYED BY THE RAW NAME, so the rewritten call must carry the raw name.
        // This used the instance DECL's name, which by the second call has been qualified to its
        // C name (`mod_ident_i32`) — a name the symbol table does not know — so the call's type
        // inferred to nothing, and `var b = ident(i32, 4)` after `var a = ident(i32, 3)` left
        // `b` without a type and later "undeclared" (E106). Calling a generic function twice at
        // the same type argument, with an unannotated binding, never worked.
        char *raw2 = mono_dup(rawbuf, strlen(rawbuf));
        inst_id = id(sema_arena, (isize)strlen(raw2), raw2);
    } else {
        char *raw = mono_dup(rawbuf, strlen(rawbuf));
        inst_id = id(sema_arena, (isize)strlen(raw), raw);
        // cname = template's cname ⧺ suffix (e.g. "mod_max" → "mod_max_i32").
        char tmplraw[224];
        snprintf(tmplraw, sizeof tmplraw, "%.*s", (int)base->length, base->name);
        Symbol *tsym = sema_lookup(tmplraw);
        char cnamebuf[288]; int co = 0; cnamebuf[0] = '\0';
        mono_cat(cnamebuf, sizeof cnamebuf, &co, "%s%s", tsym ? tsym->c_name : rawbuf, suffix);
        mono_refuse_cut(what);
        char *cname = mono_dup(cnamebuf, strlen(cnamebuf));
        inst = mono_instantiate_function(tmpl, &ctx, inst_id);
        mono_record_inst(inst_id, tmpl, inst, oargs, ntp);
        // Resolve the instance's signature NOW (`Option(T)` → Option_i32) so a
        // caller inferring this call's result type sees the concrete type even
        // though the instance is appended after (and processed later than) it.
        mono_resolve_signature(inst);
        sema_insert_global(raw, cname, inst->as.function_decl.return_type, inst, false);
        DeclList *node = decl_list(sema_arena, inst);
        DeclList *tail = sema_decls; while (tail && tail->next) tail = tail->next;
        if (tail) tail->next = node; else sema_decls = node;
    }
    mono_at_line = sl; mono_at_col = sc;

    // Rewrite the call to target the concrete instance.
    callee->as.identifier_expr.id = inst_id;
    callee->as.identifier_expr.via_qualifier = true;  // synthesized instance: exempt from glob-retirement
    callee->decl = inst;
    callee->type = NULL;
    call->as.call_expr.args = new_args;
    return true;
}

#endif // SEMA_MONOMORPH_H
