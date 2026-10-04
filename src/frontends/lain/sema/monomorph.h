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

static void mono_mangle_type(Type *t, char *buf, size_t cap) {
    if (!t) { snprintf(buf, cap, "?"); return; }
    switch (t->kind) {
        case TYPE_SIMPLE: {
            const char *canon = mono_canonical_scalar(t->base_type);
            if (canon) { snprintf(buf, cap, "%s", canon); break; }
            snprintf(buf, cap, "%.*s", t->base_type ? (int)t->base_type->length : 1,
                     t->base_type ? t->base_type->name : "?");
            break;
        }
        case TYPE_POINTER: {
            char inner[128]; mono_mangle_type(t->element_type, inner, sizeof inner);
            snprintf(buf, cap, "ptr_%s", inner); break;
        }
        case TYPE_ARRAY: {      // the length is part of the type: u8[4] and u8[8] are two instances
            char inner[128]; mono_mangle_type(t->element_type, inner, sizeof inner);
            if (t->array_len > 0) snprintf(buf, cap, "arr%lld_%s", (long long)t->array_len, inner);
            else snprintf(buf, cap, "arr_%s", inner);
            break;
        }
        case TYPE_CONST: snprintf(buf, cap, "%lld", (long long)t->array_len); break;
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
    int off = 0;
    char vb[128]; mono_mangle_type(value, vb, sizeof vb);
    off += snprintf(nb + off, cap - (size_t)off, "__U_%s", vb);
    for (IdList *m = u->union_markers; m; m = m->next) {
        off += snprintf(nb + off, cap - (size_t)off, "_%.*s", (int)m->id->length, m->id->name);
        // Payload markers mangle their field types too, so `E{line u32}` and
        // `E{col u16}` are distinct unions (no dedup collision).
        for (DeclList *f = m->fields; f; f = f->next) {
            if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
            char fb[128]; mono_mangle_type(f->decl->as.variable_decl.type, fb, sizeof fb);
            off += snprintf(nb + off, cap - (size_t)off, "_%s", fb);
        }
    }
}

static Type *union_lower(Type *u) {
    Type *value = u->element_type;
    char nb[256];
    union_mangled_name(u, nb, sizeof nb);
    Symbol *ex = sema_lookup(nb);
    if (ex && ex->decl && ex->decl->kind == DECL_ENUM)
        return type_simple(sema_arena, ex->decl->as.enum_decl.type_name);

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
// keeps it.
static isize mono_at_line = 0, mono_at_col = 0;
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
            soff += snprintf(suffix + soff, sizeof suffix - (size_t)soff, "_%s", tb);
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
    callee->as.identifier_expr.via_qualifier = true;  // synthesized instance: exempt from glob-retirement
    callee->decl = inst;
    callee->type = NULL;
    call->as.call_expr.args = new_args;
    return true;
}

#endif // SEMA_MONOMORPH_H
