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
    Type *args[MONO_MAX_TPARAMS];
    int   n;
    struct MonoInst *next;
} MonoInst;
static MonoInst *g_mono_insts = NULL;

static void mono_record_inst(Id *name, Type **args, int n) {
    MonoInst *m = arena_push_aligned(sema_arena, MonoInst);
    m->name = name; m->n = n < MONO_MAX_TPARAMS ? n : MONO_MAX_TPARAMS;
    for (int i = 0; i < m->n; i++) m->args[i] = args[i];
    m->next = g_mono_insts; g_mono_insts = m;
}
static MonoInst *mono_find_inst(Id *name) {
    for (MonoInst *m = g_mono_insts; m; m = m->next) if (mono_id_eq(m->name, name)) return m;
    return NULL;
}

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
static Type *mono_arg_to_type(Expr *e) {
    if (!e) return NULL;
    if (e->kind == EXPR_TYPE) return e->as.type_expr.type_value;
    if (e->kind == EXPR_DEREF) {
        Type *inner = mono_arg_to_type(e->as.deref_expr.expr);
        return inner ? type_pointer(sema_arena, inner) : NULL;
    }
    return NULL;
}

// Structurally unify a parameter's type pattern against a concrete argument type,
// binding any type-param names it mentions (T, *T[], etc.).
static void mono_unify(Type *pat, Type *arg, SubstCtx *ctx, Id **tp, int ntp) {
    if (!pat || !arg) return;
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
    char rawbuf[256];
    snprintf(rawbuf, sizeof rawbuf, "%.*s%s", (int)base->length, base->name, suffix);
    Symbol *existing = sema_lookup(rawbuf);
    if (existing) return existing->decl;
    char *raw = mono_dup(rawbuf, strlen(rawbuf));
    Id *inst_id = id(sema_arena, (isize)strlen(raw), raw);
    char tmplraw[224]; snprintf(tmplraw, sizeof tmplraw, "%.*s", (int)base->length, base->name);
    Symbol *tsym = sema_lookup(tmplraw);
    char cnamebuf[288]; snprintf(cnamebuf, sizeof cnamebuf, "%s%s", tsym ? tsym->c_name : rawbuf, suffix);
    char *cname = mono_dup(cnamebuf, strlen(cnamebuf));

    // 1) clone + rename + drop the header (the shell).
    Decl *inst = clone_decl(sema_arena, tmpl);
    if (inst->kind == DECL_STRUCT) { inst->as.struct_decl.name = inst_id; inst->as.struct_decl.type_params = NULL; }
    else                          { inst->as.enum_decl.type_name = inst_id; inst->as.enum_decl.type_params = NULL; }

    // 2) register + append BEFORE specializing fields (breaks self-reference).
    Type *ity = type_simple(sema_arena, inst_id);
    sema_insert_global(raw, cname, ity, inst, false);
    mono_record_inst(inst_id, ctx->concretes, ctx->n);   // for inference: Foo_i32 → [i32]
    DeclList *node = decl_list(sema_arena, inst);
    DeclList *tail = sema_decls; while (tail && tail->next) tail = tail->next;
    if (tail) tail->next = node; else sema_decls = node;

    // 3) specialize each field: substitute the type params, then resolve any
    //    nested generic type-application (`inner Option(T)` → Option_i32).
    if (inst->kind == DECL_STRUCT) {
        for (DeclList *f = inst->as.struct_decl.fields; f; f = f->next)
            if (f->decl && f->decl->kind == DECL_VARIABLE)
                f->decl->as.variable_decl.type =
                    mono_resolve_type_apps(mono_subst_type(f->decl->as.variable_decl.type, ctx));
    } else if (inst->kind == DECL_ENUM) {
        for (Variant *v = inst->as.enum_decl.variants; v; v = v->next)
            for (DeclList *f = v->fields; f; f = f->next)
                if (f->decl && f->decl->kind == DECL_VARIABLE)
                    f->decl->as.variable_decl.type =
                        mono_resolve_type_apps(mono_subst_type(f->decl->as.variable_decl.type, ctx));
    }
    return inst;
}

// Resolve generic type-applications in a type: `Vec(i32)` in a signature/field
// becomes the concrete instance type. Recurses into nested applications and
// composite types. Returns the (possibly rewritten) type.
// Lower `T | m1 | m2` (TYPE_UNION) to a niche-optimized anonymous enum: one
// payload variant `some { __v: T }` + one empty variant per marker. Deduped by a
// deterministic mangled name so the same union in two signatures shares one enum.
// ZERO-COST MANDATORY: reject (E064) if the markers don't fit T's niche.
static Type *union_lower(Type *u) {
    Type *value = u->element_type;
    char nb[256]; int off = 0;
    char vb[128]; mono_mangle_type(value, vb, sizeof vb);
    off += snprintf(nb + off, sizeof nb - (size_t)off, "__U_%s", vb);
    int nmark = 0;
    for (IdList *m = u->union_markers; m; m = m->next) {
        off += snprintf(nb + off, sizeof nb - (size_t)off, "_%.*s", (int)m->id->length, m->id->name);
        nmark++;
    }
    Symbol *ex = sema_lookup(nb);
    if (ex && ex->decl && ex->decl->kind == DECL_ENUM)
        return type_simple(sema_arena, ex->decl->as.enum_decl.type_name);

    char *raw = mono_dup(nb, strlen(nb));
    Id *ename = id(sema_arena, (isize)strlen(raw), raw);

    // payload variant `some { __v : value }`, then one empty variant per marker.
    Variant *pv = arena_push_aligned(sema_arena, Variant);
    pv->name   = id(sema_arena, 4, "some");
    pv->fields = decl_list(sema_arena, decl_variable(sema_arena, id(sema_arena, 3, "__v"), value));
    pv->next   = NULL;
    Variant *tail = pv;
    for (IdList *m = u->union_markers; m; m = m->next) {
        Variant *mv = arena_push_aligned(sema_arena, Variant);
        mv->name = m->id; mv->fields = NULL; mv->next = NULL;
        tail->next = mv; tail = mv;
    }

    Decl *ed = arena_push_aligned(sema_arena, Decl);
    ed->kind = DECL_ENUM;
    ed->as.enum_decl.type_name   = ename;
    ed->as.enum_decl.variants    = pv;
    ed->as.enum_decl.type_params = NULL;
    ed->as.enum_decl.is_union    = true;

    Type *ity = type_simple(sema_arena, ename);
    sema_insert_global(raw, raw, ity, ed, false);
    DeclList *node = decl_list(sema_arena, ed);
    DeclList *tl = sema_decls; while (tl && tl->next) tl = tl->next;
    if (tl) tl->next = node; else sema_decls = node;

    // Zero-cost mandatory: the markers must fit the value's niche, else reject.
    if (!niche_enum_is_zero_cost(&ed->as.enum_decl)) {
        char vd[128]; type_describe(value, vd, sizeof vd);
        fprintf(stderr, "[E064] Error: the union `%s | ...` cannot be zero-cost — '%s' has no "
                "spare bit-patterns for its %d marker(s). Give the value type niche room "
                "(a refinement like `u8 where < 200`, a pointer, or a slice), or use fewer markers.\n",
                vd, vd, nmark);
        exit(1);
    }
    return ity;
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
            fprintf(stderr, "[E124] Error: '%.*s' is not a generic type.\n",
                    (int)t->base_type->length, t->base_type->name);
            exit(1);
        }
        Decl *tmpl = sym->decl;
        DeclList *tparams = (tmpl->kind == DECL_STRUCT) ? tmpl->as.struct_decl.type_params
                                                        : tmpl->as.enum_decl.type_params;
        SubstCtx ctx; ctx.n = 0; char suffix[224]; int soff = 0; suffix[0] = '\0';
        TypeList *ta = t->type_args;
        for (DeclList *tp = tparams; tp && ta; tp = tp->next, ta = ta->next) {
            mono_bind(&ctx, tp->decl->as.variable_decl.name, ta->type);
            char tb[128]; mono_mangle_type(ta->type, tb, sizeof tb);
            soff += snprintf(suffix + soff, sizeof suffix - (size_t)soff, "_%s", tb);
        }
        Decl *inst = mono_type_instance(tmpl, &ctx, suffix);
        Id *iname = (inst->kind == DECL_STRUCT) ? inst->as.struct_decl.name : inst->as.enum_decl.type_name;
        return type_simple(sema_arena, iname);
    }
    if (t->element_type) t->element_type = mono_resolve_type_apps(t->element_type);
    return t;
}

// Resolve type-applications across a function's signature (param + return types).
static void mono_resolve_signature(Decl *d) {
    if (!d || d->kind != DECL_FUNCTION) return;
    for (DeclList *p = d->as.function_decl.params; p; p = p->next)
        if (p->decl && p->decl->kind == DECL_VARIABLE)
            p->decl->as.variable_decl.type = mono_resolve_type_apps(p->decl->as.variable_decl.type);
    d->as.function_decl.return_type = mono_resolve_type_apps(d->as.function_decl.return_type);
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
        for (int i = 0; i < ntp; i++, a = a->next) {
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
    for (int i = 0; i < ntp; i++) {
        Type *concrete = NULL;
        for (int j = 0; j < ctx.n; j++) if (mono_id_eq(ctx.names[j], tp_names[i])) concrete = ctx.concretes[j];
        char tb[128]; mono_mangle_type(concrete, tb, sizeof tb);
        soff += snprintf(suffix + soff, sizeof suffix - (size_t)soff, "_%s", tb);
    }

    call->as.call_expr.args = field_args;
    Decl *inst = mono_type_instance(tmpl, &ctx, suffix);
    Type *ity = type_simple(sema_arena, inst->as.struct_decl.name);
    callee->decl = inst;
    callee->type = ity;
    if (callee->kind == EXPR_TYPE) callee->as.type_expr.type_value = ity;
    else                           callee->as.identifier_expr.id = inst->as.struct_decl.name;
    return true;
}

// Generic enum reference `Option(i32)` (all args are type args) → instantiate
// Option_i32 and rewrite the call node into an EXPR_TYPE for the instance, so a
// following `.Some(5)` / `.None` resolves as a variant of the concrete enum.
static bool mono_ref_generic_enum(Expr *call, Decl *tmpl) {
    DeclList *tparams = tmpl->as.enum_decl.type_params;
    Id *base = tmpl->as.enum_decl.type_name;
    SubstCtx ctx; ctx.n = 0; char suffix[224]; int soff = 0; suffix[0] = '\0';
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
        soff += snprintf(suffix + soff, sizeof suffix - (size_t)soff, "_%s", tb);
        a = a->next;
    }
    Decl *inst = mono_type_instance(tmpl, &ctx, suffix);
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
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pt = p->decl->as.variable_decl.type;
        if (pt && pt->kind == TYPE_META) { if (ntp < MONO_MAX_TPARAMS) tp_names[ntp++] = p->decl->as.variable_decl.name; }
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
            mono_unify(vparams[i]->decl->as.variable_decl.type, argt, &ctx, tp_names, ntp);
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
    for (int i = 0; i < ntp; i++) {
        Type *concrete = NULL;
        for (int j = 0; j < ctx.n; j++) if (mono_id_eq(ctx.names[j], tp_names[i])) concrete = ctx.concretes[j];
        char tb[128]; mono_mangle_type(concrete, tb, sizeof tb);
        soff += snprintf(suffix + soff, sizeof suffix - (size_t)soff, "_%s", tb);
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
        // Resolve the instance's signature NOW (`Option(T)` → Option_i32) so a
        // caller inferring this call's result type sees the concrete type even
        // though the instance is appended after (and processed later than) it.
        mono_resolve_signature(inst);
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
