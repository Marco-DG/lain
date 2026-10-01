// static_eval.h — COMPILE-TIME EVALUATION BY THE PROGRAM'S OWN SEMANTICS (DECIDE-W step 2).
//
// A module constant whose initialiser CALLS a function is computed at compile time and replaced by
// its value before the module is lowered, so every use, every analysis and the emitted C see a
// constant, exactly as if the programmer had written the literal:
//
//   CTYPE u8[256] = [classify(c) for c in 0..256]   classify runs 256 times; the table is data
//   F20 u32 = fib(20)                                 the C reads 6765
//
// The value comes from the IR interpreter (src/ir/interp.h), the one definition of what a program
// means, so it is the value the same call computes at run time. Nothing runs unless it is PROVEN:
// every function the initialiser can reach must have the EMPTY effect row (pure and total) and pass
// every analysis, and the interpreter checks every discharged proof again as it runs. Anything else
// is E136. Evaluation is bounded by a STEP budget, not time, so a build is deterministic.
//
// A compile-time expression can only mention module-level names, so all of this happens at MODULE
// level, between the front end and lowering, where none of the front end's per-function state is
// live: no re-entrancy, no rewrite of the front end (local/internal/design/comptime_endgame_v2.md).
#ifndef LAIN_STATIC_EVAL_H
#define LAIN_STATIC_EVAL_H

// ~1.2 s at the measured ~117 ns per IR operation (Handwriting, M2). LAIN_STATIC_STEPS raises it.
#define SE_STEPS_DEFAULT 10000000LL

static bool se_has_call(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_CALL: return true;
        case EXPR_BINARY: return se_has_call(e->as.binary_expr.left) || se_has_call(e->as.binary_expr.right);
        case EXPR_UNARY: return se_has_call(e->as.unary_expr.right);
        case EXPR_CAST: return se_has_call(e->as.cast_expr.expr);
        case EXPR_INDEX: return se_has_call(e->as.index_expr.target) || se_has_call(e->as.index_expr.index);
        case EXPR_MEMBER: return se_has_call(e->as.member_expr.target);
        case EXPR_ARRAY_LITERAL:
            for (ExprList *l = e->as.array_literal_expr.elements; l; l = l->next) if (se_has_call(l->expr)) return true;
            return false;
        case EXPR_ARRAY_COMPREHENSION: return se_has_call(e->as.array_comprehension_expr.body);
        default: return false;
    }
}

// `proven` and the walk in se_prove are sized by the module, so no function a constant reaches can
// escape the checks by being the 513th.
typedef struct SeCtx {
    IrFunc *mod; int nfuncs; IrFunc **proven; int nproven; IrFunc **work;
    const char *file; long long budget; Arena *a; DeclList *program;
} SeCtx;
typedef enum { SE_OK, SE_DEFER, SE_FAIL } SeProof;

// Lower the module as it stands now: a constant folded since the last lowering is a literal in it.
static void se_lower(SeCtx *x) {
    x->mod = ir_lower_module(x->program, x->a);
    lin_mod = x->mod; bor_loan_mod = x->mod; vra_mod = x->mod;
    x->nfuncs = 0; for (IrFunc *h = x->mod; h; h = h->next) x->nfuncs++;
    x->nfuncs += 1;                                       // the thunk itself
    x->proven = arena_push_many_aligned(x->a, IrFunc *, (size_t)x->nfuncs);
    x->work = arena_push_many_aligned(x->a, IrFunc *, (size_t)x->nfuncs);
    x->nproven = 0;
}

static IrFunc *se_find(IrFunc *mod, const IrName *n) {
    for (IrFunc *h = mod; h; h = h->next)
        if (h->name && n && h->name->length == n->length && memcmp(h->name->name, n->name, (size_t)n->length) == 0) return h;
    return NULL;
}
static void se_err_at(Decl *d, Expr *at) {
    isize ln = at && at->line ? at->line : d->line, cl = at && at->line ? at->col : d->col;
    fprintf(stderr, "[E136] Error Ln %li, Col %li: ", (long)ln, (long)cl);
}

// Everything the thunk can reach must be pure, total and proven BEFORE it runs.
// QUIET: a non-empty effect row is not reported but DEFERRED. A function lowered before a table it
// reads was folded still holds the table's unmodelled comprehension, an opaque whose row is
// everything; folding the table and lowering again removes it. Nothing else depends on the order:
// a panic, a function pointer, a failed analysis are reported at once.
static SeProof se_prove(SeCtx *x, IrFunc *thunk, Decl *d, Expr *at, bool quiet) {
    Id *nm = d->as.variable_decl.name;
    IrFunc **work = x->work; int nw = 0, done = 0; work[nw++] = thunk;
    const IrName *undef = NULL;       // a callee with no body: named only if no row names its caller
    while (done < nw) {
        IrFunc *f = work[done++];
        if (f->is_extern) continue;
        for (IrBlock *b = f->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next) {
                if (i->op != IR_CALL && i->op != IR_FUNC_REF) continue;
                if (i->op == IR_CALL && !i->aux.callee) {          // through a pointer: unknown callee
                    se_err_at(d, at);
                    fprintf(stderr, "the initialiser of constant '%.*s' calls through a function pointer, "
                            "so what it runs cannot be proven pure and total before it runs.\n",
                            nm ? (int)nm->length : 1, nm ? nm->name : "?");
                    return SE_FAIL;
                }
                IrFunc *g = se_find(x->mod, i->aux.callee);
                if (!g) { if (!undef) undef = i->aux.callee; continue; }   // the `panic` builtin
                bool seen = false; for (int k = 0; k < nw; k++) if (work[k] == g) seen = true;
                if (!seen) work[nw++] = g;                         // at most once each: fits nfuncs
            }
    }
    // A CYCLE: lowering inlines a module constant's initialiser where it is read, so a function the
    // initialiser reaches that holds code at the initialiser's own position READS the constant
    // being defined. (Left alone it is a function calling itself, reported as `diverge`, or the
    // table's opaque.)
    for (int k = 1; k < nw; k++) {
        IrFunc *g = work[k];
        if (g->is_extern) continue;
        for (IrBlock *b = g->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next) {
                if (!at || i->line != at->line || i->col != at->col) continue;
                if (quiet) return SE_DEFER;
                Decl *gd = (Decl *)g->src_decl;
                Id *gn = (gd && gd->kind == DECL_FUNCTION) ? gd->as.function_decl.name : NULL;
                se_err_at(d, at);
                fprintf(stderr, "constant '%.*s' is computed by '%.*s', which reads '%.*s' itself.\n",
                        nm ? (int)nm->length : 1, nm ? nm->name : "?",
                        gn ? (int)gn->length : (int)g->name->length, gn ? gn->name : g->name->name,
                        nm ? (int)nm->length : 1, nm ? nm->name : "?");
                diagnostic_show_line(at->line, at->col);
                return SE_FAIL;
            }
    }
    // An unmodelled construct is reported as itself (E100, as the main pipeline does), not as the
    // effect row of everything that an unknown value implies.
    for (int k = 0; k < nw; k++) {
        if (work[k]->is_extern) continue;
        for (IrBlock *b = work[k]->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next) {
                if (i->op != IR_OPAQUE) continue;
                if (quiet) return SE_DEFER;
                fprintf(stderr, "[E100] Error Ln %lld, Col %lld: this construct is not supported by the code "
                        "generator yet (%s), and the initialiser of constant '%.*s' reaches it.\n",
                        (long long)i->line, (long long)i->col, i->aux.opaque.why ? i->aux.opaque.why : "?",
                        nm ? (int)nm->length : 1, nm ? nm->name : "?");
                if (i->line) diagnostic_show_line(i->line, i->col);
                return SE_FAIL;
            }
    }
    // The effect row of each function reached: the first that is not empty is named, by the name
    // the programmer wrote (the IR's is module-qualified).
    for (int k = 1; k < nw; k++) {
        IrFunc *g = work[k];
        IrEffect e = ir_effects(g, x->mod);
        if (e & (IR_EFFECT_DIVERGE | IR_EFFECT_RAISES | IR_EFFECT_IO | IR_EFFECT_ALLOC | IR_EFFECT_UNMODELLED_WRITE)) {
            if (quiet) return SE_DEFER;
            Decl *gd = (Decl *)g->src_decl;
            Id *gn = (gd && (gd->kind == DECL_FUNCTION || gd->kind == DECL_EXTERN_FUNCTION)) ? gd->as.function_decl.name : NULL;
            char row[64]; int rl = 0; row[0] = 0;
            const char *bits[4] = { e & IR_EFFECT_IO ? "io" : NULL, e & IR_EFFECT_DIVERGE ? "diverge" : NULL,
                                    e & IR_EFFECT_RAISES ? "raises" : NULL, e & IR_EFFECT_ALLOC ? "alloc" : NULL };
            for (int q = 0; q < 4; q++) if (bits[q]) rl += snprintf(row + rl, sizeof row - (size_t)rl, "%s%s", rl ? ", " : "", bits[q]);
            se_err_at(d, at);
            fprintf(stderr, "the initialiser of constant '%.*s' reaches '%.*s', whose effects are {%s}, so "
                    "it cannot run at compile time. Only a function whose effect row is empty (pure and total) does.\n",
                    nm ? (int)nm->length : 1, nm ? nm->name : "?",
                    gn ? (int)gn->length : (int)g->name->length, gn ? gn->name : g->name->name, row);
            diagnostic_show_line(at && at->line ? at->line : d->line, at && at->line ? at->col : d->col);
            return SE_FAIL;
        }
    }
    if (undef) {                       // `panic` charges `raises` to its caller, so this is a backstop
        se_err_at(d, at);
        fprintf(stderr, "the initialiser of constant '%.*s' reaches '%.*s', which may panic, so it cannot run "
                "at compile time. Only a function whose effect row is empty does.\n",
                nm ? (int)nm->length : 1, nm ? nm->name : "?", (int)undef->length, undef->name);
        return SE_FAIL;
    }
    // Every analysis, once per function (the main pipeline would refuse the same program anyway).
    for (int k = 0; k < nw; k++) {
        IrFunc *g = work[k]; bool already = g->is_extern;          // an extern has no body to analyse
        for (int j = 0; j < x->nproven; j++) if (x->proven[j] == g) already = true;
        if (already) continue;
        if (ir_report_findings(g, x->mod, x->file, true)) return SE_FAIL;
        if (g != thunk) x->proven[x->nproven++] = g;
    }
    return SE_OK;
}

// The interpreter's value as the literal the programmer could have written, typed as the CONSTANT
// (`ty`); NULL when it is not an integer or a bool, or does not fit a literal (then the initialiser
// is left to run at each use: the same value, since everything it reaches is pure and total). The
// value is read by the type it was COMPUTED in (`rt`, the thunk's return type: the bits are two's
// complement and the type says how to read them), and then checked against the constant's type
// exactly as a literal initialiser is (sema_check_one_const_fits: `X u16 = big()` is refused when
// big() is 70000 and accepted when it is 7, which a proof of the narrowing could not tell apart).
static Expr *se_literal_of(IVal *v, IrType *rt, Type *ty, Expr *at, Decl *d, const char *what) {
    if (v->k != IV_INT || !rt || (rt->kind != IRT_INT && rt->kind != IRT_BOOL)) return NULL;
    __int128 x = it_wrap_bits((__int128)v->i, it_bits(rt), it_signed(rt));
    if (x < (__int128)INT64_MIN || x > (__int128)INT64_MAX) return NULL;
    Expr *lit = expr_literal(sema_arena, (int64_t)x);
    lit->type = ty; lit->line = at->line; lit->col = at->col;
    if (ty && is_bool_type(ty)) lit->as.literal_expr.is_bool = true;
    Id *nm = d->as.variable_decl.name;
    char name[256]; snprintf(name, sizeof name, "%.*s", nm ? (int)nm->length : 1, nm ? nm->name : "?");
    sema_check_one_const_fits(ty, lit, d, what, name);         // exits with E086 when it does not fit
    return lit;
}

// The per-index thunk's parameter carries the comprehension's RANGE as a refinement
// (`lo <= i < hi`), which lowering turns into an entry assumption: without it `i` is any i32, and
// a body like `cls(i)` with `cls(c usize)` cannot prove the conversion.
static Decl *se_thunk(Decl *d, Expr *body, Type *rt, Id *param, int64_t lo, int64_t hi) {
    Id *nm = d->as.variable_decl.name;
    DeclList *params = NULL;
    if (param) {
        Decl *pd = decl_variable(sema_arena, param, get_builtin_i32_type());
        pd->as.variable_decl.is_parameter = true; pd->line = body->line; pd->col = body->col;
        Expr *lo_v = expr_literal(sema_arena, lo), *hi_v = expr_literal(sema_arena, hi);
        lo_v->type = hi_v->type = get_builtin_i32_type();      // lowering types a literal by its type
        Expr *lo_c = expr_binary(sema_arena, TOKEN_ANGLE_BRACKET_RIGHT_EQUAL, expr_identifier(sema_arena, param), lo_v);
        Expr *hi_c = expr_binary(sema_arena, TOKEN_ANGLE_BRACKET_LEFT, expr_identifier(sema_arena, param), hi_v);
        pd->as.variable_decl.constraints = expr_list(sema_arena, lo_c);
        pd->as.variable_decl.constraints->next = expr_list(sema_arena, hi_c);
        params = decl_list(sema_arena, pd);
    }
    char tn[300]; int tl = snprintf(tn, sizeof tn, "__static_%.*s", nm ? (int)nm->length : 1, nm ? nm->name : "k");
    char *heap = arena_push_many_aligned(sema_arena, char, (size_t)tl + 1); memcpy(heap, tn, (size_t)tl + 1);
    Stmt *ret = stmt_return(sema_arena, body); ret->line = body->line; ret->col = body->col;
    Decl *th = decl_function(sema_arena, id(sema_arena, tl, heap), params, rt, stmt_list(sema_arena, ret), false, false);
    th->line = d->line; th->col = d->col; th->defining_module = d->defining_module;
    return th;
}

// Run one thunk; a value, or NULL after an E136 (err set) or when not representable (err unset).
// *left is the constant's remaining budget: ONE budget per constant, however many elements it has.
static Expr *se_run(SeCtx *x, Decl *d, IrFunc *thunk, IVal *arg, Type *ty, Expr *at, bool *err, long long *left,
                    const char *what) {
    IVal out; memset(&out, 0, sizeof out);
    long long used = 0;
    int st = ir_interpret_call(thunk, x->mod, x->file, arg, arg ? 1 : 0, &out, *left, &used);
    *left -= used;
    Id *nm = d->as.variable_decl.name;
    if (st == 98) return NULL;                                     // not modelled: runs at each use
    if (st != 0) {
        se_err_at(d, at);
        if (st == 97) fprintf(stderr, "constant '%.*s' took more than %lld steps to compute "
                              "(LAIN_STATIC_STEPS raises the limit).\n", nm ? (int)nm->length : 1, nm ? nm->name : "?", x->budget);
        else fprintf(stderr, "constant '%.*s' could not be computed at compile time (see above).\n",
                     nm ? (int)nm->length : 1, nm ? nm->name : "?");
        *err = true; return NULL;
    }
    return se_literal_of(&out, thunk->ret_type, ty, at, d, what);
}

// A module constant is computed here when its initialiser calls a function, or when it is a
// comprehension the front end could not expand into a literal (its evaluator stops at a cast,
// `[(i * 100) as% u8 for i in 0..4]`, and the comprehension was then E100, unmodelled).
static bool se_pending(Decl *d) {
    if (!d || d->kind != DECL_VARIABLE || d->as.variable_decl.is_mutable || !d->as.variable_decl.init) return false;
    return se_has_call(d->as.variable_decl.init) || d->as.variable_decl.init->kind == EXPR_ARRAY_COMPREHENSION;
}

// Fold one constant. SE_OK: folded, or left to run at each use (a value with no literal, or one the
// interpreter does not model: the same value either way, since all it reaches is pure and total).
// SE_DEFER: quiet, and the effect check failed (see se_prove). SE_FAIL: reported.
static SeProof se_fold_one(SeCtx *x, Decl *d, bool quiet, int *folded) {
    Expr *init = d->as.variable_decl.init;
    Type *ty = d->as.variable_decl.type ? d->as.variable_decl.type : init->type;
    bool err = false;
    long long left = x->budget;
    if (init->kind == EXPR_ARRAY_COMPREHENSION) {                // one thunk, run per index
        Expr *body = init->as.array_comprehension_expr.body, *rg = init->as.array_comprehension_expr.range;
        Type *et = ty && ty->element_type ? ty->element_type : body->type;   // the constant's elements
        // The front end admits only literal bounds here (typecheck: "range bounds must be integer literals").
        int64_t lo = rg->as.range_expr.start->as.literal_expr.value, hi = rg->as.range_expr.end->as.literal_expr.value;
        if (rg->as.range_expr.inclusive) hi++;
        IrFunc *th = ir_lower_function(se_thunk(d, body, body->type, init->as.array_comprehension_expr.idx, lo, hi), x->program, x->a);
        SeProof pr = se_prove(x, th, d, init, quiet);
        if (pr != SE_OK) return pr;
        ExprList *head = NULL, **tail = &head;
        for (int64_t k = lo; k < hi; k++) {
            IVal arg; memset(&arg, 0, sizeof arg); arg.k = IV_INT; arg.i = (uint64_t)k;
            Expr *lit = se_run(x, d, th, &arg, et, init, &err, &left, "an element of constant");
            if (err) return SE_FAIL;
            if (!lit) return SE_OK;                                  // left as it is
            *tail = expr_list(sema_arena, lit); tail = &(*tail)->next;
        }
        Expr *arr = expr_array_literal(sema_arena, head);
        arr->type = ty; arr->line = init->line; arr->col = init->col;
        d->as.variable_decl.init = arr; (*folded)++;
        return SE_OK;
    }
    if (init->kind == EXPR_ARRAY_LITERAL) {                          // each element that calls
        Type *et = ty && ty->element_type ? ty->element_type : NULL;
        for (ExprList *l = init->as.array_literal_expr.elements; l; l = l->next) {
            if (!se_has_call(l->expr)) continue;
            IrFunc *th = ir_lower_function(se_thunk(d, l->expr, l->expr->type, NULL, 0, 0), x->program, x->a);
            SeProof pr = se_prove(x, th, d, l->expr, quiet);
            if (pr != SE_OK) return pr;
            Expr *lit = se_run(x, d, th, NULL, et ? et : l->expr->type, l->expr, &err, &left, "an element of constant");
            if (err) return SE_FAIL;
            if (lit) l->expr = lit;
        }
        (*folded)++;
        return SE_OK;
    }
    IrFunc *th = ir_lower_function(se_thunk(d, init, init->type, NULL, 0, 0), x->program, x->a);   // a scalar
    SeProof pr = se_prove(x, th, d, init, quiet);
    if (pr != SE_OK) return pr;
    Expr *lit = se_run(x, d, th, NULL, ty, init, &err, &left, "constant");
    if (err) return SE_FAIL;
    if (lit) { d->as.variable_decl.init = lit; (*folded)++; }
    return SE_OK;
}

// Evaluate every module constant whose initialiser calls a function. Returns the number of errors.
// ORDER: a constant may need another (`N u32 = count()` where `count` reads `CTYPE`), in either
// source order. Rather than a dependency walk over the AST (a second, approximate model of what
// lowering reads), the IR decides: QUIET rounds fold whatever proves and lower again after each
// round that folded something; when a round makes no progress, the constants still pending fail for
// a reason of their own, and one LOUD round reports it. A cycle (`A = f()` where `f` reads `A`) is
// refused there by the analyses: `f` calls itself with nothing decreasing.
static int ir_static_eval_module(DeclList *program, const char *file, Arena *a) {
    int npending = 0;
    for (DeclList *dl = program; dl; dl = dl->next) if (se_pending(dl->decl)) npending++;
    if (!npending) return 0;
    SeCtx x; memset(&x, 0, sizeof x);
    x.file = file; x.budget = SE_STEPS_DEFAULT; x.a = a; x.program = program;
    const char *bs = getenv("LAIN_STATIC_STEPS"); if (bs) x.budget = atoll(bs);
    Decl **pend = arena_push_many_aligned(a, Decl *, (size_t)npending);
    int np = 0;
    for (DeclList *dl = program; dl; dl = dl->next) if (se_pending(dl->decl)) pend[np++] = dl->decl;
    int errors = 0, folded = 0, rounds = 0;
    bool stale = true;
    for (;;) {
        if (stale) { se_lower(&x); stale = false; }
        rounds++;
        int before = folded, left = 0;
        for (int k = 0; k < np; k++) {
            if (!pend[k]) continue;
            SeProof pr = se_fold_one(&x, pend[k], true, &folded);
            if (pr == SE_DEFER) { left++; continue; }
            if (pr == SE_FAIL) errors++;
            pend[k] = NULL;
        }
        if (folded > before) stale = true;
        if (!left || folded == before) break;
    }
    // The loud round. After its first failure the rest run quiet again: a constant that needs the
    // one that failed would report the failed table as an unmodelled construct, a consequence named
    // as a cause. They still count, and a failure of their own (SE_FAIL) is still printed.
    for (int k = 0; k < np; k++) {
        if (!pend[k]) continue;
        if (stale) { se_lower(&x); stale = false; }
        SeProof pr = se_fold_one(&x, pend[k], errors > 0, &folded);
        if (pr != SE_OK) errors++;
    }
    if (getenv("LAIN_STATIC_EVAL_TRACE"))
        fprintf(stderr, "static-eval: %d constant(s) computed at compile time, %d round(s)\n", folded, rounds);
    return errors;
}

#endif // LAIN_STATIC_EVAL_H
