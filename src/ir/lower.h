// src/ir/lower.h — AST → IR lowering (Phase 1.2).
//
// Walks the frontend's TYPED AST (after sema_resolve_module populates ->type) and
// emits the IR of build.h. Mutable and immutable locals both become stack slots
// (alloca/load/store) — the memory form; mem2reg (a later pass) promotes them to SSA.
// This is the ONLY place source names are read; downstream everything is value
// identity. Coverage is the core subset (grown toward the Phase-1 gate); unhandled
// constructs emit a typed `const 0` placeholder so lowering completes and the CFG is
// inspectable. Not wired into main.c.
#ifndef LAIN_IR_LOWER_H
#define LAIN_IR_LOWER_H

#include "build.h"

// ── local environment (the name→storage map — the AST/IR boundary) ───────────
typedef struct IrLocal {
    Id      *name;
    IrValue *slot;        // alloca address for a scalar local (load/store), OR the
                          // element-base pointer for an aggregate local
    IrValue *param;       // param value (read directly; NULL if it's a slot local)
    bool     aggregate;   // array/slice/struct: `slot` is the base, read it directly
    struct IrLocal *next;
} IrLocal;

typedef struct {
    IrFunc   *f;
    IrBlock  *cur;         // block currently being filled
    Arena    *a;
    Decl     *fdecl;       // the function being lowered (for its own return ensures)
    IrLocal  *locals;
    bool      unsafe;      // inside an `unsafe` block (elem_ptr etc. become unchecked)
    // innermost loop targets, for break/continue
    IrBlock  *loop_head, *loop_exit;
    DeclList *globals;     // module top-level decls (for global-constant references)
    int       const_depth; // recursion guard for cyclic constant initializers
    // struct-type memo: cache the IrType per struct decl so a self-referential
    // field (a pointer back to the struct) returns the in-progress node instead of
    // recursing forever.
    Decl     *scache_decl[64];
    IrType   *scache_type[64];
    int       scache_n;
} LowerCtx;

// A block's terminator is "set" once lowering has given it one. A freshly-memset
// block has kind==IR_TERM_BR (0) with a==NULL, which is the unset sentinel.
static bool ir_is_set_term(IrBlock *b) {
    return b->term.kind != IR_TERM_BR || b->term.a != NULL;
}

static IrLocal *ir_env_find(LowerCtx *c, Id *name) {
    for (IrLocal *l = c->locals; l; l = l->next)
        if (l->name && name && l->name->length == name->length &&
            strncmp(l->name->name, name->name, (size_t)name->length) == 0) return l;
    return NULL;
}
static void ir_env_add(LowerCtx *c, Id *name, IrValue *slot, IrValue *param) {
    IrLocal *l = arena_push_aligned(c->a, IrLocal);
    l->name = name; l->slot = slot; l->param = param; l->aggregate = false;
    l->next = c->locals; c->locals = l;
}
// A module-level immutable constant `NAME T = expr` (scalar) referenced by name.
// sema qualifies a reference as `<defining_module>_<name>` (Q-018), while the decl
// keeps the bare name — so match both the bare and the module-qualified spelling.
static Decl *ir_find_global_const(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_VARIABLE) continue;
        if (!dc->as.variable_decl.init || dc->as.variable_decl.is_mutable) continue;
        Id *dn = dc->as.variable_decl.name;
        if (!dn) continue;
        if (dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0)
            return dc;                                   // bare match
        const char *mod = dc->defining_module;           // `<mod>_<name>` match
        if (mod) {
            size_t ml = strlen(mod);
            if ((size_t)name->length == ml + 1 + (size_t)dn->length &&
                strncmp(name->name, mod, ml) == 0 && name->name[ml] == '_' &&
                strncmp(name->name + ml + 1, dn->name, (size_t)dn->length) == 0)
                return dc;
        }
    }
    return NULL;
}
// "aggregate" here = decays to an element-base pointer read directly from its slot
// (a fixed array). A SLICE and a STRUCT are first-class values living in a normal
// slot (alloca + load/store by value); their fields/elements are reached by GEP.
static bool ir_type_is_agg(IrType *t) {
    return t && t->kind==IRT_ARRAY;
}

// A module-level struct declaration by (bare) name.
static Decl *ir_find_struct_decl(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_STRUCT) continue;
        Id *dn = dc->as.struct_decl.name;
        if (dn && dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0) return dc;
    }
    return NULL;
}
// A type alias `type Name = <base> [refinement…]` by (bare) name.
static Decl *ir_find_type_alias(LowerCtx *c, Id *name) {
    if (!name) return NULL;
    for (DeclList *d = c->globals; d; d = d->next) {
        Decl *dc = d->decl;
        if (!dc || dc->kind != DECL_TYPE_ALIAS) continue;
        Id *dn = dc->as.type_alias_decl.name;
        if (dn && dn->length == name->length &&
            strncmp(dn->name, name->name, (size_t)name->length) == 0) return dc;
    }
    return NULL;
}
// The alias's *runtime* base type — the leftmost leaf of the RHS (refinement
// constraints like `!= 0` are for the VRA, not the representation).
static IrType *ir_lower_type(LowerCtx *c, Type *t);              // fwd
static bool ir_name_int(const char *nm, int len, int *bits, bool *sgn);  // fwd
static IrType *ir_resolve_alias_base(LowerCtx *c, Decl *ad) {
    Expr *rhs = ad->as.type_alias_decl.expr;
    while (rhs && rhs->kind == EXPR_BINARY) rhs = rhs->as.binary_expr.left;  // leftmost leaf
    if (!rhs) return NULL;
    if (rhs->kind == EXPR_TYPE && rhs->as.type_expr.type_value)   // sema-resolved base type
        return ir_lower_type(c, rhs->as.type_expr.type_value);
    if (rhs->kind == EXPR_IDENTIFIER) {
        Id *nm = rhs->as.identifier_expr.id; int b; bool s;
        if (nm->length==4 && strncmp(nm->name,"bool",4)==0) return ir_type_bool(c->a);
        if (ir_name_int(nm->name, (int)nm->length, &b, &s)) return ir_type_int(c->a, b, s);
    }
    if (rhs->type) return ir_lower_type(c, rhs->type);           // any other resolved expr
    return NULL;
}
// Index of field `m` in an IRT_STRUCT (uses the IR type's own field table — no AST).
static int ir_field_index(IrType *st, Id *m, IrType **fty) {
    if (!st || st->kind != IRT_STRUCT || !m) return -1;
    for (int i = 0; i < st->n_fields; i++) {
        IrName *fn = st->field_names[i];   // IR-owned; compare against the AST member id
        if (fn && fn->length == m->length &&
            strncmp(fn->name, m->name, (size_t)m->length) == 0) {
            if (fty) *fty = st->fields[i];
            return i;
        }
    }
    return -1;
}

// ── type bridge: AST Type → IrType (core cases) ──────────────────────────────
static bool ir_name_int(const char *nm, int len, int *bits, bool *sgn) {
    if (len >= 2 && (nm[0]=='i'||nm[0]=='u')) {
        int b=0; bool ok=true;
        for (int k=1;k<len;k++){ if(nm[k]<'0'||nm[k]>'9'){ok=false;break;} b=b*10+(nm[k]-'0'); }
        if (ok && b>=1 && b<=64){ *bits=b; *sgn=(nm[0]=='i'); return true; }
    }
    struct { const char*n; int b; bool s; } al[] = {
        {"usize",64,false},{"isize",64,true},{"int",32,true},{NULL,0,false} };
    for (int i=0; al[i].n; i++)
        if ((int)strlen(al[i].n)==len && strncmp(nm,al[i].n,len)==0){ *bits=al[i].b; *sgn=al[i].s; return true; }
    return false;
}
static IrType *ir_lower_type(LowerCtx *c, Type *t) {
    if (!t) return ir_type_new(c->a, IRT_UNIT);
    switch (t->kind) {
        case TYPE_SIMPLE: {
            if (t->base_type) {
                int len=(int)t->base_type->length; const char *nm=t->base_type->name;
                if (len==4 && strncmp(nm,"bool",4)==0) return ir_type_bool(c->a);
                int b; bool s;
                if (t->int_width_cache>0) return ir_type_int(c->a, t->int_width_cache, t->int_signed_cache);
                if (ir_name_int(nm,len,&b,&s)) return ir_type_int(c->a, b, s);
            }
            // a type alias → its runtime base type (refinements are the VRA's concern)
            Decl *ad = ir_find_type_alias(c, t->base_type);
            if (ad && c->const_depth < 32) {
                c->const_depth++;
                IrType *r = ir_resolve_alias_base(c, ad);
                c->const_depth--;
                if (r) return r;
            }
            // a named struct → a self-contained IRT_STRUCT (lowered field table)
            Decl *sd = ir_find_struct_decl(c, t->base_type);
            if (sd) {
                for (int i=0;i<c->scache_n;i++) if (c->scache_decl[i]==sd) return c->scache_type[i];
                IrType *r = ir_type_new(c->a, IRT_STRUCT);
                Id *snm = sd->as.struct_decl.name;                    // intern the struct name
                if (snm) r->sname = ir_intern(c->a, snm->name, snm->length);
                if (c->scache_n < 64) { c->scache_decl[c->scache_n]=sd;
                                        c->scache_type[c->scache_n]=r; c->scache_n++; }
                int nf=0; for (DeclList *fl=sd->as.struct_decl.fields; fl; fl=fl->next)
                    if (fl->decl && fl->decl->kind==DECL_VARIABLE) nf++;
                r->n_fields = nf;
                r->fields       = arena_push_many_aligned(c->a, IrType*, nf>0?nf:1);
                r->field_names  = arena_push_many_aligned(c->a, IrName*, nf>0?nf:1);
                int i=0; for (DeclList *fl=sd->as.struct_decl.fields; fl; fl=fl->next) {
                    if (!fl->decl || fl->decl->kind!=DECL_VARIABLE) continue;
                    r->fields[i]      = ir_lower_type(c, fl->decl->as.variable_decl.type);
                    Id *fnm = fl->decl->as.variable_decl.name;
                    r->field_names[i] = fnm ? ir_intern(c->a, fnm->name, fnm->length) : NULL;
                    i++;
                }
                return r;
            }
            return ir_type_new(c->a, IRT_STRUCT);   // unresolved named type — opaque
        }
        case TYPE_ARRAY: {
            IrType *el = ir_lower_type(c, t->element_type);
            if (t->array_len >= 0) { IrType *r=ir_type_new(c->a,IRT_ARRAY); r->elem=el; r->array_len=t->array_len; return r; }
            IrType *r=ir_type_new(c->a,IRT_SLICE); r->elem=el; return r;   // dynamic → slice
        }
        case TYPE_SLICE: { IrType *r=ir_type_new(c->a,IRT_SLICE); r->elem=ir_lower_type(c,t->element_type); return r; }
        case TYPE_POINTER:{ IrType *r=ir_type_new(c->a,IRT_PTR); r->elem=ir_lower_type(c,t->element_type); r->ptr_mut=t->pointee_mutable; return r; }
        default: return ir_type_new(c->a, IRT_UNIT);
    }
}

// forward
static IrValue *ir_lower_expr(LowerCtx *c, Expr *e);
static void     ir_lower_stmts(LowerCtx *c, StmtList *body);

// map an AST comparison token to an IrCmp given signedness. false ⇒ not a comparison.
static bool ir_tok_cmp(TokenKind op, bool sgn, IrCmp *out) {
    switch (op) {
        case TOKEN_ANGLE_BRACKET_LEFT:        *out = sgn?IR_CMP_SLT:IR_CMP_ULT; return true;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  *out = sgn?IR_CMP_SLE:IR_CMP_ULE; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT:       *out = sgn?IR_CMP_SGT:IR_CMP_UGT; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: *out = sgn?IR_CMP_SGE:IR_CMP_UGE; return true;
        default: return false;
    }
}

// B4-lite (dependent lengths): a slice/dynamic-array param whose type carries a length
// constraint `i32[m]` / `i32[>= n]` / `i32[out.len]` becomes an entry
// `assume(slice_len(p) relop <expr>)` — connecting the runtime length to the symbol.
// Run AFTER all params are in scope so the length expr (another param) resolves.
// Lower a refinement RHS robustly — the refinement/size exprs are NOT type-checked by
// sema (they come through untyped), so a literal gets the fallback type and `x.len` is
// resolved by the LOWERED value's IR kind, never the (missing) AST type.
static IrValue *ir_lower_refinement_rhs(LowerCtx *c, Expr *rhs, IrType *fallback_ty) {
    if (!rhs) return NULL;
    if (rhs->kind==EXPR_LITERAL)
        return ir_const_int(c->f, c->cur, rhs->as.literal_expr.value, fallback_ty);
    if (rhs->kind==EXPR_MEMBER && rhs->as.member_expr.member && rhs->as.member_expr.member->length==3
        && strncmp(rhs->as.member_expr.member->name,"len",3)==0) {
        IrValue *tv = ir_lower_expr(c, rhs->as.member_expr.target);
        return (tv && tv->type && tv->type->kind==IRT_SLICE) ? ir_slice_len(c->f, c->cur, tv) : NULL;
    }
    return ir_lower_expr(c, rhs);
}

static void ir_lower_slice_len_refinement(LowerCtx *c, IrValue *pv, Type *pty) {
    if (!pv || !pty || pty->kind!=TYPE_ARRAY || pty->array_len>=0 || !pty->size_expr) return;
    IrValue *L  = ir_slice_len(c->f, c->cur, pv);
    IrValue *rv = ir_lower_refinement_rhs(c, pty->size_expr, ir_type_int(c->a,64,false));
    if (!rv || !rv->type || rv->type->kind!=IRT_INT) return;
    if (pty->size_relop == TOKEN_EQUAL_EQUAL) {           // len == expr
        ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_ULE, L, rv));
        ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_UGE, L, rv));
    } else { IrCmp cmp;
        if (ir_tok_cmp(pty->size_relop, false, &cmp))     // len relop expr (len is usize)
            ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, L, rv));
    }
}

// B2 (contracts): a callee's return refinement `result OP rhs` (`func f(..) usize <= m`)
// becomes a post-call `assume(v OP <that>)` — the caller LEARNS the ensures. rhs may be a
// constant or one of the callee's params, resolved to the matching call argument.
static void ir_lower_return_ensures(LowerCtx *c, Decl *callee, IrValue *v, IrInstr *call) {
    if (!callee || callee->kind==DECL_STRUCT || !v || !v->type || v->type->kind!=IRT_INT) return;
    for (ExprList *rc = callee->as.function_decl.return_constraints; rc; rc = rc->next) {
        Expr *con = rc->expr;
        if (!con || con->kind!=EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        IrValue *rv = NULL;
        if (rhs && rhs->kind==EXPR_LITERAL) rv = ir_const_int(c->f, c->cur, rhs->as.literal_expr.value, v->type);
        else if (rhs && rhs->kind==EXPR_IDENTIFIER) {
            int idx=0; Id *rn=rhs->as.identifier_expr.id;
            for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
                if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
                Id *pn=p->decl->as.variable_decl.name;
                if (pn && rn && pn->length==rn->length && strncmp(pn->name,rn->name,(size_t)pn->length)==0) {
                    if (idx < call->n_operands) rv = call->operands[idx];
                    break;
                }
            }
        }
        IrCmp cmp;
        if (rv && ir_tok_cmp(con->as.binary_expr.op, v->type->is_signed, &cmp))
            ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, v, rv));
    }
}

// B2 (ensures, CALLEE side — the soundness dual of ir_lower_return_ensures): at each
// `return e` in a function declaring `result OP rhs`, emit `assert(e OP rhs)`. The callee
// must PROVE its own postcondition; only that discharge licenses the caller's post-call
// `assume`. Without this, the caller would trust a front-end ensures the IR never checks —
// a lying/sketchy front-end could then prove a false bound from the returned value. rhs is
// a literal or one of the callee's own params (scalar, in scope).
static void ir_lower_return_ensures_assert(LowerCtx *c, IrValue *v) {
    Decl *fn = c->fdecl;
    if (!fn || fn->kind==DECL_STRUCT || !v || !v->type || v->type->kind!=IRT_INT) return;
    for (ExprList *rc = fn->as.function_decl.return_constraints; rc; rc = rc->next) {
        Expr *con = rc->expr;
        if (!con || con->kind!=EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        IrValue *rv = NULL;
        if (rhs && rhs->kind==EXPR_LITERAL) rv = ir_const_int(c->f, c->cur, rhs->as.literal_expr.value, v->type);
        else if (rhs && rhs->kind==EXPR_IDENTIFIER) {
            IrLocal *l = ir_env_find(c, rhs->as.identifier_expr.id);
            rv = (l && l->param) ? l->param : NULL;   // scalar param only (fail-closed otherwise)
        }
        IrCmp cmp;
        if (rv && rv->type && rv->type->kind==IRT_INT &&
            ir_tok_cmp(con->as.binary_expr.op, v->type->is_signed, &cmp))
            ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, v, rv));
    }
}

// Resolve a contract RHS at a call site, substituting a callee param name for the
// matching call argument: a literal (typed `ty`), `m` → the arg for m, `a.len` →
// slice_len(arg for a).
static IrValue *ir_resolve_contract_rhs(LowerCtx *c, Decl *callee, IrInstr *call, Expr *rhs, IrType *ty) {
    if (!rhs) return NULL;
    if (rhs->kind==EXPR_LITERAL) return ir_const_int(c->f, c->cur, rhs->as.literal_expr.value, ty);
    Id *nm=NULL; bool is_len=false;
    if (rhs->kind==EXPR_IDENTIFIER) nm=rhs->as.identifier_expr.id;
    else if (rhs->kind==EXPR_MEMBER && rhs->as.member_expr.member && rhs->as.member_expr.member->length==3
             && strncmp(rhs->as.member_expr.member->name,"len",3)==0
             && rhs->as.member_expr.target && rhs->as.member_expr.target->kind==EXPR_IDENTIFIER) {
        nm=rhs->as.member_expr.target->as.identifier_expr.id; is_len=true;
    }
    if (!nm) return NULL;
    int idx=0;
    for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        Id *pn=p->decl->as.variable_decl.name;
        if (pn && pn->length==nm->length && strncmp(pn->name,nm->name,(size_t)pn->length)==0) {
            if (idx >= call->n_operands) return NULL;
            IrValue *av = call->operands[idx];
            if (is_len) return (av && av->type && av->type->kind==IRT_SLICE) ? ir_slice_len(c->f,c->cur,av) : NULL;
            return av;
        }
    }
    return NULL;
}
// B2 (requires): a callee's param refinements become call-site `assert`s the CALLER
// must discharge — closing the contract soundly (the callee's entry `assume` is then
// justified). Reported as VRA_PRECOND (a distinct obligation class).
static void ir_lower_call_requires(LowerCtx *c, Decl *callee, IrInstr *call) {
    if (!callee || callee->kind==DECL_STRUCT) return;
    int idx=0;
    for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        if (idx >= call->n_operands) break;
        IrValue *arg = call->operands[idx];
        if (!arg || !arg->type || arg->type->kind!=IRT_INT) continue;
        for (ExprList *cn=p->decl->as.variable_decl.constraints; cn; cn=cn->next) {
            Expr *con=cn->expr;
            if (!con || con->kind!=EXPR_BINARY) continue;
            IrCmp cmp;
            if (!ir_tok_cmp(con->as.binary_expr.op, arg->type->is_signed, &cmp)) continue;
            IrValue *rv = ir_resolve_contract_rhs(c, callee, call, con->as.binary_expr.right, arg->type);
            if (rv && rv->type && rv->type->kind==IRT_INT)
                ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, arg, rv));
        }
        // in_field dual: callee `pos in text` ⇒ assert arg_pos < len(arg_text) (licenses the
        // callee's entry `assume(pos < len(text))`, closing the contract soundly).
        Id *inf = p->decl->as.variable_decl.in_field;
        if (inf) {
            int jdx=0;
            for (DeclList *q=callee->as.function_decl.params; q; q=q->next, jdx++) {
                if (!q->decl || q->decl->kind!=DECL_VARIABLE) continue;
                Id *qn=q->decl->as.variable_decl.name;
                if (qn && qn->length==inf->length && strncmp(qn->name,inf->name,(size_t)qn->length)==0) {
                    if (jdx < call->n_operands) {
                        IrValue *aarr = call->operands[jdx];
                        if (aarr && aarr->type && aarr->type->kind==IRT_SLICE)
                            ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_ULT, arg, ir_slice_len(c->f,c->cur,aarr)));
                    }
                    break;
                }
            }
        }
    }
}

// map an AST binary token to an IR op given signedness; returns true if arithmetic/bitwise.
static bool ir_bin_op(TokenKind t, bool sgn, IrOp *op, IrWrapMode *wrap) {
    *wrap = IR_WRAP_CHECK;
    switch (t) {
        case TOKEN_PLUS: *op=IR_ADD; return true;
        case TOKEN_MINUS:*op=IR_SUB; return true;
        case TOKEN_ASTERISK: *op=IR_MUL; return true;
        case TOKEN_SLASH:  *op= sgn?IR_SDIV:IR_UDIV; return true;
        case TOKEN_PERCENT:*op= sgn?IR_SREM:IR_UREM; return true;
        case TOKEN_AMPERSAND:*op=IR_AND; return true;
        case TOKEN_PIPE:     *op=IR_OR;  return true;
        case TOKEN_CARET:    *op=IR_XOR; return true;
        case TOKEN_SHIFT_LEFT: *op=IR_SHL; return true;
        case TOKEN_SHIFT_RIGHT:*op= sgn?IR_ASHR:IR_LSHR; return true;
        case TOKEN_PLUS_PERCENT: *op=IR_ADD; *wrap=IR_WRAP_MODULAR; return true;
        case TOKEN_MINUS_PERCENT:*op=IR_SUB; *wrap=IR_WRAP_MODULAR; return true;
        case TOKEN_ASTERISK_PERCENT: *op=IR_MUL; *wrap=IR_WRAP_MODULAR; return true;
        default: return false;
    }
}
static bool ir_cmp_op(TokenKind t, bool sgn, IrCmp *c) {
    switch (t) {
        case TOKEN_EQUAL_EQUAL: *c=IR_CMP_EQ; return true;
        case TOKEN_BANG_EQUAL:  *c=IR_CMP_NE; return true;
        case TOKEN_ANGLE_BRACKET_LEFT:       *c= sgn?IR_CMP_SLT:IR_CMP_ULT; return true;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: *c= sgn?IR_CMP_SLE:IR_CMP_ULE; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT:      *c= sgn?IR_CMP_SGT:IR_CMP_UGT; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:*c= sgn?IR_CMP_SGE:IR_CMP_UGE; return true;
        default: return false;
    }
}

// Recursively resolve a callee-scope length expression at a CALL SITE into caller values —
// each callee param name is substituted by the matching call argument (ident / `x.len` via
// ir_resolve_contract_rhs; +/-/* recurse). NULL if any leaf is unresolvable → fail-closed:
// no call-site length assert is emitted, so the callee's entry length-assume goes unlicensed
// and the chain simply won't fully verify (sound: we never trust an unproven length).
static IrValue *ir_resolve_len_expr(LowerCtx *c, Decl *callee, IrInstr *call, Expr *e, IrType *ty) {
    if (!e) return NULL;
    if (e->kind==EXPR_BINARY) {
        IrOp op; IrWrapMode wrap;
        if (!ir_bin_op(e->as.binary_expr.op, false, &op, &wrap)) return NULL;
        IrValue *l = ir_resolve_len_expr(c, callee, call, e->as.binary_expr.left, ty);
        IrValue *r = ir_resolve_len_expr(c, callee, call, e->as.binary_expr.right, ty);
        if (!l || !r) return NULL;
        return ir_binop(c->f, c->cur, op, l, r, ty);
    }
    return ir_resolve_contract_rhs(c, callee, call, e, ty);   // literal / param ident / x.len
}

// C (dependent-length requires — the soundness dual of ir_lower_slice_len_refinement): at a
// call, for each callee param declared as a sized slice `T[expr]`, assert
// `len(arg) relop resolve(expr)` (equality ⇒ both directions, matching the entry assume).
// This licenses the callee's entry `assume(len == expr)`; without it the callee would trust
// that the caller passed a correctly-sized buffer — a front-end that let a short slice
// through would make the callee prove a false `out[i]` bound. Fail-closed if unresolvable.
static void ir_lower_call_slice_len_requires(LowerCtx *c, Decl *callee, IrInstr *call) {
    if (!callee || callee->kind==DECL_STRUCT) return;
    int idx=0;
    for (DeclList *p=callee->as.function_decl.params; p; p=p->next, idx++) {
        if (!p->decl || p->decl->kind!=DECL_VARIABLE) continue;
        if (idx >= call->n_operands) break;
        Type *pty = p->decl->as.variable_decl.type;
        if (!pty || pty->kind!=TYPE_ARRAY || pty->array_len>=0 || !pty->size_expr) continue;
        IrValue *arg = call->operands[idx];
        if (!arg || !arg->type || arg->type->kind!=IRT_SLICE) continue;
        IrValue *L  = ir_slice_len(c->f, c->cur, arg);
        IrValue *rv = ir_resolve_len_expr(c, callee, call, pty->size_expr, ir_type_int(c->a,64,false));
        if (!rv || !rv->type || rv->type->kind!=IRT_INT) continue;  // fail-closed
        if (pty->size_relop == TOKEN_EQUAL_EQUAL) {                 // len == expr ⇒ both dirs
            ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_UGE, L, rv));
            ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_ULE, L, rv));
        } else { IrCmp cmp;
            if (ir_tok_cmp(pty->size_relop, false, &cmp))
                ir_assert(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, L, rv));
        }
    }
}

// address of an lvalue (identifier slot / index / member) — for assignment + index.
static IrValue *ir_lower_addr(LowerCtx *c, Expr *e) {
    if (e->kind == EXPR_IDENTIFIER) {
        IrLocal *l = ir_env_find(c, e->as.identifier_expr.id);
        if (l && l->slot) return l->slot;
    }
    if (e->kind == EXPR_INDEX) {
        IrValue *base = ir_lower_expr(c, e->as.index_expr.target);
        IrValue *idx  = ir_lower_expr(c, e->as.index_expr.index);
        IrType  *elem = ir_lower_type(c, e->type);
        if (base->type && base->type->kind == IRT_SLICE)
            base = ir_slice_data(c->f, c->cur, base, elem);   // index through .data
        IrValue *p = ir_elem_ptr(c->f, c->cur, base, idx, elem);
        p->line = e->line; p->col = e->col;
        c->cur->instrs_tail->unchecked = c->unsafe;   // elem_ptr just emitted
        return p;
    }
    if (e->kind == EXPR_MEMBER) {
        IrValue *base = ir_lower_addr(c, e->as.member_expr.target);   // struct address
        IrType  *sty  = ir_lower_type(c, e->as.member_expr.target->type);
        IrType  *fty  = NULL;
        int idx = ir_field_index(sty, e->as.member_expr.member, &fty);
        if (idx >= 0) return ir_field_ptr(c->f, c->cur, base, idx, fty);
    }
    // other lvalues: not yet lowered — infaithful placeholder slot
    c->f->incomplete = true;
    return ir_alloca(c->f, c->cur, ir_lower_type(c, e->type));
}

static IrValue *ir_lower_expr(LowerCtx *c, Expr *e) {
    if (!e) return ir_const_int(c->f, c->cur, 0, ir_type_int(c->a,32,true));
    IrType *ty = ir_lower_type(c, e->type);
    switch (e->kind) {
        case EXPR_LITERAL:
            return ir_const_int(c->f, c->cur, e->as.literal_expr.value, ty);
        case EXPR_CHAR:
            return ir_const_int(c->f, c->cur, e->as.char_expr.value, ty);
        case EXPR_STRING: {
            IrValue *data = ir_str_const(c->f, c->cur, e->as.string_expr.value,
                                         (int32_t)e->as.string_expr.length);
            if (ty && ty->kind == IRT_SLICE) {   // u8[:0] context → a fat {data,len}
                IrValue *ln = ir_const_int(c->f, c->cur, e->as.string_expr.length,
                                           ir_type_int(c->a,64,false));
                return ir_make_slice(c->f, c->cur, data, ln, ty->elem);
            }
            return data;   // fixed u8[N:0] / pointer context: the data pointer
        }
        case EXPR_IDENTIFIER: {
            IrLocal *l = ir_env_find(c, e->as.identifier_expr.id);
            if (l && l->param) return l->param;
            if (l && l->aggregate) return l->slot;   // array/slice base pointer, read directly
            if (l && l->slot)  return ir_load(c->f, c->cur, l->slot,
                                              l->slot->type->elem ? l->slot->type->elem : ty);
            // not a local/param: a module-level constant folds to its initializer
            if (c->const_depth < 32) {
                Decl *g = ir_find_global_const(c, e->as.identifier_expr.id);
                if (g) { c->const_depth++;
                         IrValue *v = ir_lower_expr(c, g->as.variable_decl.init);
                         c->const_depth--; return v; }
            }
            c->f->incomplete = true;                     // truly unresolved (e.g. global array)
            return ir_const_int(c->f, c->cur, 0, ty);
        }
        case EXPR_BINARY: {
            Expr *L=e->as.binary_expr.left, *R=e->as.binary_expr.right;
            // `i in container` — a valid-index guard: 0 ≤ i < container.len. Lowered to
            // `i < len` (the ≥ 0 half comes from i's type/flow); this makes it a real
            // icmp so guard refinement and the termination check both engage.
            if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) {
                IrValue *a = ir_lower_expr(c, L);
                IrValue *len;
                if (R->type && R->type->kind==TYPE_ARRAY && R->type->array_len>=0)
                    len = ir_const_int(c->f, c->cur, R->type->array_len, ir_type_int(c->a,64,false));
                else len = ir_slice_len(c->f, c->cur, ir_lower_expr(c, R));
                return ir_icmp(c->f, c->cur, IR_CMP_ULT, a, len);
            }
            // Short-circuit `and` / `or`: the right operand must NOT be evaluated when
            // the left already decides the result (correctness — it may guard a deref/
            // index), so this needs real control flow, not a bitwise op. The br_cond on
            // the left's condition also lets the octagon refine the eval-right block.
            if (e->as.binary_expr.op==TOKEN_KEYWORD_AND || e->as.binary_expr.op==TOKEN_KEYWORD_OR) {
                bool is_and = (e->as.binary_expr.op==TOKEN_KEYWORD_AND);
                IrType *bt = ir_type_bool(c->a);
                IrValue *rcell = ir_alloca(c->f, c->cur, bt);
                IrValue *xa = ir_lower_expr(c, L);
                IrBlock *evb=ir_new_block(c->f), *sk=ir_new_block(c->f), *jn=ir_new_block(c->f);
                if (is_and) ir_set_br_cond(c->cur, xa, evb, sk);   // and: eval R only if L true
                else        ir_set_br_cond(c->cur, xa, sk, evb);   // or:  eval R only if L false
                c->cur = evb; ir_store(c->f, c->cur, rcell, ir_lower_expr(c, R)); ir_set_br(c->cur, jn);
                c->cur = sk;  ir_store(c->f, c->cur, rcell, ir_const_int(c->f,c->cur,is_and?0:1,bt)); ir_set_br(c->cur, jn);
                c->cur = jn;  return ir_load(c->f, c->cur, rcell, bt);
            }
            // Signedness comes from the OPERANDS, not e->type: a comparison's result
            // type is bool, so deriving from it would mistag every `i < len` as signed.
            Type *sty = (L->type && L->type->kind==TYPE_SIMPLE && L->type->int_width_cache>0) ? L->type
                      : (R->type && R->type->kind==TYPE_SIMPLE && R->type->int_width_cache>0) ? R->type
                      : e->type;
            bool sgn = !(sty && sty->kind==TYPE_SIMPLE && sty->int_width_cache>0 && !sty->int_signed_cache);
            IrValue *x = ir_lower_expr(c,L), *y = ir_lower_expr(c,R);
            IrOp op; IrWrapMode wrap; IrCmp cmp;
            if (ir_cmp_op(e->as.binary_expr.op, sgn, &cmp)) return ir_icmp(c->f,c->cur,cmp,x,y);
            if (ir_bin_op(e->as.binary_expr.op, sgn, &op, &wrap)) {
                IrValue *r = ir_binop(c->f,c->cur,op,x,y,ty);
                c->cur->instrs_tail->wrap = wrap;
                return r;
            }
            // an unhandled binary operator — infaithful, so fail closed.
            c->f->incomplete = true;
            return ir_binop(c->f,c->cur,IR_AND,x,y,ir_type_bool(c->a));
        }
        case EXPR_UNARY: {
            IrValue *x = ir_lower_expr(c, e->as.unary_expr.right);
            if (e->as.unary_expr.op == TOKEN_MINUS) { IrInstr *ins=ir_instr(c->f,IR_NEG,ty,1); ins->operands[0]=x; ir_emit(c->cur,ins); return ins->result; }
            IrInstr *ins=ir_instr(c->f,IR_BNOT,ty,1); ins->operands[0]=x; ir_emit(c->cur,ins); return ins->result;
        }
        case EXPR_INDEX: {
            Expr *idxe = e->as.index_expr.index;
            if (idxe && idxe->kind == EXPR_RANGE) {   // subslice: xs[lo..hi] → a slice value
                IrType *u64t  = ir_type_int(c->a,64,false);
                IrValue *tv   = ir_lower_expr(c, e->as.index_expr.target);
                IrType  *selem= (ty && ty->elem) ? ty->elem : ir_type_int(c->a,8,false);
                bool src_slice= tv->type && tv->type->kind==IRT_SLICE;
                IrValue *srclen = src_slice ? ir_slice_len(c->f,c->cur,tv)
                    : (e->as.index_expr.target->type && e->as.index_expr.target->type->kind==TYPE_ARRAY
                        ? ir_const_int(c->f,c->cur, e->as.index_expr.target->type->array_len, u64t) : NULL);
                IrValue *data0= src_slice ? ir_slice_data(c->f,c->cur,tv,selem) : tv;
                IrValue *lo   = idxe->as.range_expr.start ? ir_lower_expr(c, idxe->as.range_expr.start)
                                                          : ir_const_int(c->f,c->cur,0,u64t);
                IrValue *hi   = idxe->as.range_expr.end   ? ir_lower_expr(c, idxe->as.range_expr.end)
                              : (srclen ? srclen : ir_const_int(c->f,c->cur,0,u64t));
                IrValue *nd   = ir_elem_ptr(c->f,c->cur, data0, lo, selem);
                IrValue *len  = ir_binop(c->f,c->cur, IR_SUB, hi, lo, u64t);
                if (idxe->as.range_expr.inclusive)
                    len = ir_binop(c->f,c->cur, IR_ADD, len, ir_const_int(c->f,c->cur,1,u64t), u64t);
                return ir_make_slice(c->f,c->cur, nd, len, selem);
            }
            IrValue *addr = ir_lower_addr(c, e);
            return ir_load(c->f, c->cur, addr, ty);
        }
        case EXPR_MEMBER: {
            Id *m = e->as.member_expr.member;
            Expr *tgt = e->as.member_expr.target;
            Type *tst = tgt ? tgt->type : NULL;
            if (m && m->length==3 && strncmp(m->name,"len",3)==0) {
                if (tst && tst->kind==TYPE_ARRAY && tst->array_len>=0)
                    return ir_const_int(c->f, c->cur, tst->array_len, ty);   // fixed array .len = N
                // otherwise decide by the LOWERED target's IR type — robust to untyped
                // refinement/size exprs (e.g. `src.len - 1` in `i32[src.len - 1]`).
                IrValue *s = ir_lower_expr(c, tgt);
                if (s && s->type && s->type->kind==IRT_SLICE) return ir_slice_len(c->f, c->cur, s);
                if (s && s->type && s->type->kind==IRT_ARRAY)
                    return ir_const_int(c->f, c->cur, s->type->array_len, ty);
                // (fall through: a struct field literally named `len`)
            }
            // struct field read: load through the field address
            IrType *sty = ir_lower_type(c, tst);
            IrType *fty = NULL;
            if (ir_field_index(sty, m, &fty) >= 0) {
                IrValue *addr = ir_lower_addr(c, e);
                return ir_load(c->f, c->cur, addr, fty ? fty : ty);
            }
            c->f->incomplete = true;                     // unresolved member access
            return ir_const_int(c->f, c->cur, 0, ty);
        }
        case EXPR_CALL: {
            Decl *callee = e->as.call_expr.callee ? e->as.call_expr.callee->decl : NULL;
            int n=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next) n++;
            // `Point(1, 2)` is struct construction, not a call: build a struct value
            // from the positional field args (in declaration order).
            if (callee && callee->kind == DECL_STRUCT && ty && ty->kind==IRT_STRUCT) {
                IrValue **fs = arena_push_many_aligned(c->a, IrValue*, n>0?n:1);
                int k=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next,k++)
                    fs[k] = ir_lower_expr(c, a->expr);
                return ir_struct_new(c->f, c->cur, ty, fs, n);
            }
            IrInstr *ins = ir_instr(c->f, IR_CALL, (e->type && e->type->kind!=TYPE_SIMPLE) || (ty->kind!=IRT_UNIT) ? ty : NULL, n);
            Id *cnm = callee ? callee->as.function_decl.name : NULL;   // intern the callee name
            if (!cnm && e->as.call_expr.callee && e->as.call_expr.callee->kind==EXPR_IDENTIFIER)
                cnm = e->as.call_expr.callee->as.identifier_expr.id;   // declless builtin (e.g. `panic`)
            ins->aux.callee = cnm ? ir_intern(c->a, cnm->name, cnm->length) : NULL;
            DeclList *pp = callee ? callee->as.function_decl.params : NULL;
            int i=0;
            for (ExprList *a=e->as.call_expr.args; a; a=a->next,i++) {
                IrValue *av = ir_lower_expr(c, a->expr);
                // a fixed array decays to a slice when the callee expects one
                if (pp && pp->decl && pp->decl->kind==DECL_VARIABLE) {
                    IrType *ptype = ir_lower_type(c, pp->decl->as.variable_decl.type);
                    if (ptype && ptype->kind==IRT_SLICE && av->type && av->type->kind==IRT_PTR) {
                        int64_t ne = (a->expr->type && a->expr->type->kind==TYPE_ARRAY) ? a->expr->type->array_len : 0;
                        IrValue *ln = ir_const_int(c->f, c->cur, ne, ir_type_int(c->a,64,false));
                        av = ir_make_slice(c->f, c->cur, av, ln, ptype->elem);
                    }
                }
                ins->operands[i] = av;
                if (pp) pp = pp->next;
            }
            ir_lower_call_requires(c, callee, ins);   // contracts: prove the callee's scalar preconditions
            ir_lower_call_slice_len_requires(c, callee, ins);  // …and its sized-slice length preconditions
            ir_emit(c->cur, ins);
            if (ins->result) ir_lower_return_ensures(c, callee, ins->result, ins);  // contracts: learn the ensures
            return ins->result ? ins->result : ir_const_int(c->f,c->cur,0,ir_type_int(c->a,32,true));
        }
        case EXPR_CAST: {
            IrValue *x = ir_lower_expr(c, e->as.cast_expr.expr);
            IrInstr *ins = ir_instr(c->f, IR_CAST, ir_lower_type(c, e->as.cast_expr.target_type), 1);
            ins->operands[0]=x; ins->aux.cast_kind=IR_CAST_BITCAST; ir_emit(c->cur,ins);
            return ins->result;
        }
        case EXPR_MOVE: {                               // `mov x`: read the value, then INVALIDATE
            Expr *src = e->as.move_expr.expr;            // the source slot (linearity: use-after-move)
            IrValue *v = ir_lower_expr(c, src);
            if (src && src->kind==EXPR_IDENTIFIER) {
                IrLocal *l = ir_env_find(c, src->as.identifier_expr.id);
                if (l && l->slot) ir_consume(c->f, c->cur, l->slot);   // slot is now moved-from
            }
            return v;
        }
        case EXPR_MUT: {                                // `var lv`: a mutable borrow
            Expr *inner = e->as.mut_expr.expr;
            IrType *it = inner ? ir_lower_type(c, inner->type) : NULL;
            if (it && it->kind==IRT_STRUCT) return ir_lower_addr(c, inner);  // pass its address
            return ir_lower_expr(c, inner);
        }
        case EXPR_ADDR:                                 // &lvalue
            return ir_lower_addr(c, e->as.addr_expr.expr);
        case EXPR_DEREF: {                              // *ptr
            IrValue *p = ir_lower_expr(c, e->as.deref_expr.expr);
            return ir_load(c->f, c->cur, p, ty);
        }
        default:
            c->f->incomplete = true;                     // unhandled expr → infaithful placeholder
            return ir_const_int(c->f, c->cur, 0, ty);
    }
}

// Lower a boolean condition as a branch cur→tb (true) / cur→fb (false), expanding
// short-circuit `and`/`or` into NESTED guards. This keeps the octagon refining BOTH
// operands along the taken path — a materialized bool cell (the expression lowering of
// `A and B`) hides the numeric refinement from the analysis, so `if a<n and b<n {arr[a]}`
// wouldn't prove. (Guard context only; the expression form still uses the bool cell.)
static void ir_lower_cond_br(LowerCtx *c, Expr *cond, IrBlock *tb, IrBlock *fb) {
    if (cond && cond->kind==EXPR_BINARY &&
        (cond->as.binary_expr.op==TOKEN_KEYWORD_AND || cond->as.binary_expr.op==TOKEN_KEYWORD_OR)) {
        bool is_and = cond->as.binary_expr.op==TOKEN_KEYWORD_AND;
        IrBlock *mid = ir_new_block(c->f);
        if (is_and) ir_lower_cond_br(c, cond->as.binary_expr.left, mid, fb);  // A false ⇒ short-circuit to fb
        else        ir_lower_cond_br(c, cond->as.binary_expr.left, tb, mid);  // A true  ⇒ short-circuit to tb
        c->cur = mid;
        ir_lower_cond_br(c, cond->as.binary_expr.right, tb, fb);
        return;
    }
    ir_set_br_cond(c->cur, ir_lower_expr(c, cond), tb, fb);
}

static void ir_lower_stmt(LowerCtx *c, Stmt *s) {
    if (!s || ir_is_set_term(c->cur)) return;   // dead code after a terminator
    switch (s->kind) {
        case STMT_VAR: {
            IrType *slot_ty = s->as.var_stmt.type ? ir_lower_type(c, s->as.var_stmt.type)
                            : (s->as.var_stmt.expr ? ir_lower_type(c, s->as.var_stmt.expr->type)
                                                   : ir_type_int(c->a,32,true));
            if (ir_type_is_agg(slot_ty)) {
                IrValue *agg = slot_ty->kind==IRT_ARRAY ? ir_alloca_array(c->f, c->cur, slot_ty)
                                                        : ir_alloca(c->f, c->cur, slot_ty);
                IrLocal *l = arena_push_aligned(c->a, IrLocal);
                l->name = s->as.var_stmt.name; l->slot = agg; l->param = NULL;
                l->aggregate = true; l->next = c->locals; c->locals = l;
                // array-literal initializer: store each element at its index
                Expr *init = s->as.var_stmt.expr;
                if (init && init->kind == EXPR_ARRAY_LITERAL && slot_ty->kind==IRT_ARRAY) {
                    int k = 0;
                    for (ExprList *el = init->as.array_literal_expr.elements; el; el = el->next, k++) {
                        IrValue *idx = ir_const_int(c->f, c->cur, k, ir_type_int(c->a,64,false));
                        IrValue *p   = ir_elem_ptr(c->f, c->cur, agg, idx, slot_ty->elem);
                        ir_store(c->f, c->cur, p, ir_lower_expr(c, el->expr));
                    }
                } else if (init && init->kind == EXPR_ARRAY_COMPREHENSION && slot_ty->kind==IRT_ARRAY) {
                    // [ body for i in range ] ⇒ i=lo; while i<hi { agg[i]=body; i+=1 }
                    Expr *rng=init->as.array_comprehension_expr.range, *bod=init->as.array_comprehension_expr.body;
                    IrType *ity=ir_type_int(c->a,64,false);
                    bool isr = rng && rng->kind==EXPR_RANGE;
                    IrValue *icell=ir_alloca(c->f,c->cur,ity);
                    ir_store(c->f,c->cur,icell, (isr&&rng->as.range_expr.start)?ir_lower_expr(c,rng->as.range_expr.start):ir_const_int(c->f,c->cur,0,ity));
                    ir_env_add(c, init->as.array_comprehension_expr.idx, icell, NULL);
                    IrBlock *head=ir_new_block(c->f),*bb=ir_new_block(c->f),*ex=ir_new_block(c->f);
                    ir_set_br(c->cur,head); c->cur=head;
                    IrValue *iv=ir_load(c->f,head,icell,ity);
                    IrValue *hi=(isr&&rng->as.range_expr.end)?ir_lower_expr(c,rng->as.range_expr.end):ir_const_int(c->f,head,slot_ty->array_len,ity);
                    ir_set_br_cond(head, ir_icmp(c->f,head,(isr&&rng->as.range_expr.inclusive)?IR_CMP_ULE:IR_CMP_ULT,iv,hi), bb, ex);
                    c->cur=bb;
                    IrValue *iv2=ir_load(c->f,bb,icell,ity);
                    ir_store(c->f,bb, ir_elem_ptr(c->f,bb,agg,iv2,slot_ty->elem), ir_lower_expr(c,bod));
                    IrValue *ci=ir_load(c->f,c->cur,icell,ity);
                    ir_store(c->f,c->cur,icell, ir_binop(c->f,c->cur,IR_ADD,ci,ir_const_int(c->f,c->cur,1,ity),ity));
                    ir_set_br(c->cur,head); c->cur=ex;
                } else if (init) {
                    c->f->incomplete = true;   // some other aggregate init we don't model — fail closed
                }
                break;
            }
            IrValue *slot = ir_alloca(c->f, c->cur, slot_ty);
            if (s->as.var_stmt.expr) ir_store(c->f, c->cur, slot, ir_lower_expr(c, s->as.var_stmt.expr));
            ir_env_add(c, s->as.var_stmt.name, slot, NULL);
            break;
        }
        case STMT_ASSIGN: {
            IrValue *addr = ir_lower_addr(c, s->as.assign_stmt.target);
            ir_store(c->f, c->cur, addr, ir_lower_expr(c, s->as.assign_stmt.expr));
            break;
        }
        case STMT_EXPR: (void)ir_lower_expr(c, s->as.expr_stmt.expr); break;
        case STMT_RETURN: {
            IrValue *rv = s->as.return_stmt.value ? ir_lower_expr(c, s->as.return_stmt.value) : NULL;
            if (rv) ir_lower_return_ensures_assert(c, rv);   // callee proves its own ensures
            ir_set_ret(c->cur, rv);
            break;
        }
        case STMT_IF: {
            IrBlock *tb = ir_new_block(c->f), *jb = ir_new_block(c->f);
            IrBlock *eb = s->as.if_stmt.else_branch ? ir_new_block(c->f) : jb;
            ir_lower_cond_br(c, s->as.if_stmt.cond, tb, eb);   // expands `and`/`or` (refinement-preserving)
            c->cur = tb; ir_lower_stmts(c, s->as.if_stmt.then_body);
            if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, jb);
            if (s->as.if_stmt.else_branch) {
                c->cur = eb; ir_lower_stmts(c, s->as.if_stmt.else_branch);
                if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, jb);
            }
            c->cur = jb;
            break;
        }
        case STMT_WHILE: {
            IrBlock *head = ir_new_block(c->f), *body = ir_new_block(c->f), *exit = ir_new_block(c->f);
            ir_set_br(c->cur, head);
            c->cur = head;
            IrValue *cond = ir_lower_expr(c, s->as.while_stmt.cond);
            ir_set_br_cond(head, cond, body, exit);
            IrBlock *oh=c->loop_head, *oe=c->loop_exit; c->loop_head=head; c->loop_exit=exit;
            c->cur = body; ir_lower_stmts(c, s->as.while_stmt.body);
            if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, head);
            c->loop_head=oh; c->loop_exit=oe;
            c->cur = exit;
            break;
        }
        case STMT_FOR: {
            Expr *it = s->as.for_stmt.iterable;
            Id *vn = s->as.for_stmt.value_name, *xn = s->as.for_stmt.index_name;
            IrType *usz = ir_type_int(c->a, 64, false);
            if (it && it->kind == EXPR_RANGE) {
                // for i in lo..hi { body }  ⇒  i=lo; while i <(=) hi { body; i=i+1 }
                Expr *lo_e=it->as.range_expr.start, *hi_e=it->as.range_expr.end;
                IrType *ity = lo_e ? ir_lower_type(c, lo_e->type) : usz;
                if (!ity || ity->kind!=IRT_INT) ity = usz;
                IrValue *icell = ir_alloca(c->f, c->cur, ity);
                ir_store(c->f, c->cur, icell, lo_e ? ir_lower_expr(c, lo_e) : ir_const_int(c->f,c->cur,0,ity));
                ir_env_add(c, vn, icell, NULL);                 // i reads/writes its cell
                IrBlock *head=ir_new_block(c->f), *body=ir_new_block(c->f), *exit=ir_new_block(c->f);
                ir_set_br(c->cur, head); c->cur = head;
                IrValue *iv = ir_load(c->f, head, icell, ity);
                IrValue *hi = hi_e ? ir_lower_expr(c, hi_e) : ir_const_int(c->f,head,0,ity);
                IrValue *cond = ir_icmp(c->f, head, it->as.range_expr.inclusive?IR_CMP_ULE:IR_CMP_ULT, iv, hi);
                ir_set_br_cond(head, cond, body, exit);
                IrBlock *oh=c->loop_head, *oe=c->loop_exit; c->loop_head=head; c->loop_exit=exit;
                c->cur = body; ir_lower_stmts(c, s->as.for_stmt.body);
                if (!ir_is_set_term(c->cur)) {
                    IrValue *ci = ir_load(c->f, c->cur, icell, ity);
                    ir_store(c->f, c->cur, icell, ir_binop(c->f,c->cur,IR_ADD,ci,ir_const_int(c->f,c->cur,1,ity),ity));
                    ir_set_br(c->cur, head);
                }
                c->loop_head=oh; c->loop_exit=oe; c->cur = exit;
                break;
            }
            // for v in arr { body }  ⇒  i=0; while i<arr.len { v=arr[i]; body; i=i+1 }
            IrValue *av = ir_lower_expr(c, it);
            IrType  *aty = it ? ir_lower_type(c, it->type) : NULL;
            IrType  *elem = aty && aty->elem ? aty->elem : ir_type_int(c->a,32,true);
            bool is_slice = av->type && av->type->kind==IRT_SLICE;
            int64_t alen = (it && it->type && it->type->kind==TYPE_ARRAY) ? it->type->array_len : -1;
            IrValue *icell = ir_alloca(c->f, c->cur, usz);
            ir_store(c->f, c->cur, icell, ir_const_int(c->f,c->cur,0,usz));
            IrValue *vcell = ir_alloca(c->f, c->cur, elem);
            ir_env_add(c, vn, vcell, NULL);
            if (xn) ir_env_add(c, xn, icell, NULL);
            IrBlock *head=ir_new_block(c->f), *body=ir_new_block(c->f), *exit=ir_new_block(c->f);
            ir_set_br(c->cur, head); c->cur = head;
            IrValue *iv = ir_load(c->f, head, icell, usz);
            IrValue *len = is_slice ? ir_slice_len(c->f, head, av)
                         : ir_const_int(c->f, head, alen>=0?alen:0, usz);
            ir_set_br_cond(head, ir_icmp(c->f,head,IR_CMP_ULT,iv,len), body, exit);
            IrBlock *oh=c->loop_head, *oe=c->loop_exit; c->loop_head=head; c->loop_exit=exit;
            c->cur = body;
            IrValue *iv2 = ir_load(c->f, body, icell, usz);
            IrValue *dat = is_slice ? ir_slice_data(c->f, body, av, elem) : av;
            IrValue *ep  = ir_elem_ptr(c->f, body, dat, iv2, elem);
            ir_store(c->f, body, vcell, ir_load(c->f, body, ep, elem));   // v = arr[i]
            ir_lower_stmts(c, s->as.for_stmt.body);
            if (!ir_is_set_term(c->cur)) {
                IrValue *ci = ir_load(c->f, c->cur, icell, usz);
                ir_store(c->f, c->cur, icell, ir_binop(c->f,c->cur,IR_ADD,ci,ir_const_int(c->f,c->cur,1,usz),usz));
                ir_set_br(c->cur, head);
            }
            c->loop_head=oh; c->loop_exit=oe; c->cur = exit;
            break;
        }
        case STMT_MATCH: {
            // Integer/char match on literal + range patterns, lowered to an if-chain.
            // Enum/ADT matches need tag+payload modeling ⇒ fail closed until then.
            Expr *val = s->as.match_stmt.value;
            Type *vt = val ? val->type : NULL;
            if (!(vt && vt->kind==TYPE_SIMPLE && vt->int_width_cache>0)) { c->f->incomplete=true; break; }
            IrValue *v = ir_lower_expr(c, val);
            IrBlock *join = ir_new_block(c->f);
            StmtMatchCase *elsec = NULL;
            for (StmtMatchCase *cs = s->as.match_stmt.cases; cs; cs = cs->next) {
                if (!cs->patterns) { elsec = cs; continue; }
                IrBlock *body = ir_new_block(c->f);
                for (ExprList *p = cs->patterns; p; p = p->next) {   // OR of this case's patterns
                    Expr *pe = p->expr;
                    IrBlock *nxt = ir_new_block(c->f);
                    if (pe->kind == EXPR_RANGE) {
                        Expr *loe=pe->as.range_expr.start, *hie=pe->as.range_expr.end;
                        IrBlock *hitest = ir_new_block(c->f);
                        if (loe){ IrValue *ge=ir_icmp(c->f,c->cur,IR_CMP_SGE,v,ir_lower_expr(c,loe)); ir_set_br_cond(c->cur,ge,hitest,nxt); }
                        else ir_set_br(c->cur, hitest);
                        c->cur = hitest;
                        if (hie){ IrCmp cc=pe->as.range_expr.inclusive?IR_CMP_SLE:IR_CMP_SLT; IrValue *le=ir_icmp(c->f,c->cur,cc,v,ir_lower_expr(c,hie)); ir_set_br_cond(c->cur,le,body,nxt); }
                        else ir_set_br(c->cur, body);
                    } else {
                        IrValue *eq=ir_icmp(c->f,c->cur,IR_CMP_EQ,v,ir_lower_expr(c,pe));
                        ir_set_br_cond(c->cur,eq,body,nxt);
                    }
                    c->cur = nxt;
                }
                IrBlock *ftblk = c->cur;   // where control lands if no pattern matched
                c->cur = body; ir_lower_stmts(c, cs->body);
                if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, join);
                c->cur = ftblk;
            }
            if (elsec) ir_lower_stmts(c, elsec->body);
            if (!ir_is_set_term(c->cur)) ir_set_br(c->cur, join);
            c->cur = join;
            break;
        }
        case STMT_BREAK:    if (c->loop_exit) ir_set_br(c->cur, c->loop_exit); break;
        case STMT_CONTINUE: if (c->loop_head) ir_set_br(c->cur, c->loop_head); break;
        case STMT_UNSAFE: { bool o=c->unsafe; c->unsafe=true; ir_lower_stmts(c, s->as.unsafe_stmt.body); c->unsafe=o; break; }
        default: c->f->incomplete = true; break;   // enum-match/defer/use — TODO (fail closed)
    }
}
static void ir_lower_stmts(LowerCtx *c, StmtList *body) {
    for (StmtList *b = body; b && !ir_is_set_term(c->cur); b = b->next) ir_lower_stmt(c, b->stmt);
}

// Emit `assume(param OP const)` for a scalar param's refinement (`n u32 < 4096`).
// This is where a front-end refinement ENTERS the IR as a verifiable fact — the IR
// then owns it; no analysis re-reads the AST. (Sovereignty: lowering is the adapter.)
static void ir_lower_param_refinements(LowerCtx *c, IrValue *pv, Type *pty, Decl *pdecl) {
    if (!pv || !pv->type || pv->type->kind != IRT_INT || !pdecl) return;
    (void)pty;
    bool sgn = pv->type->is_signed;
    for (ExprList *cn = pdecl->as.variable_decl.constraints; cn; cn = cn->next) {
        Expr *con = cn->expr;
        if (!con || con->kind != EXPR_BINARY) continue;
        Expr *rhs = con->as.binary_expr.right;
        IrCmp cmp;
        if (!ir_tok_cmp(con->as.binary_expr.op, sgn, &cmp)) continue;   // skip == / !=
        IrValue *rv = ir_lower_refinement_rhs(c, rhs, pv->type);   // literal / a.len / param
        if (rv && rv->type && rv->type->kind==IRT_INT)
            ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, cmp, pv, rv));
    }
    // `pos usize in text` — a valid-index refinement (pos < len(text)). in_field names the
    // array param; the ≥0 half is pv's usize type. (Call-site dual in ir_lower_call_requires.)
    Id *inf = pdecl->as.variable_decl.in_field;
    if (inf) {
        IrLocal *la = ir_env_find(c, inf);
        IrValue *av = la ? (la->param ? la->param : la->slot) : NULL;
        IrValue *len = NULL;
        if (av && av->type && av->type->kind==IRT_SLICE) len = ir_slice_len(c->f, c->cur, av);
        else if (av && av->type && av->type->kind==IRT_ARRAY)
            len = ir_const_int(c->f, c->cur, av->type->array_len, ir_type_int(c->a,64,false));
        if (len) ir_assume(c->f, c->cur, ir_icmp(c->f, c->cur, IR_CMP_ULT, pv, len));
    }
}

// ── entry: lower one function ────────────────────────────────────────────────
IrFunc *ir_lower_function(Decl *fn, DeclList *globals, Arena *a) {
    IrType tmp; LowerCtx cc = {0}; cc.a = a; cc.globals = globals; (void)tmp;
    Id *fnm = fn->as.function_decl.name;
    IrFunc *f = ir_func_new(a, fnm ? ir_intern(a, fnm->name, fnm->length) : NULL,
                            NULL, fn->kind==DECL_FUNCTION ? IR_FUNC_PURE : IR_FUNC_PROC);
    f->src_decl = fn;   // opaque provenance (void*) — the IR never derefs it
    cc.fdecl = fn;      // for callee-side return-ensures asserts
    cc.f = f; cc.cur = f->entry;
    f->ret_type = ir_lower_type(&cc, fn->as.function_decl.return_type);
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type   *pty = p->decl->as.variable_decl.type;
        IrType *pt  = ir_lower_type(&cc, pty);
        Id     *pnm = p->decl->as.variable_decl.name;
        IrName *pin = pnm ? ir_intern(a, pnm->name, pnm->length) : NULL;   // IR-owned param name
        if (pt->kind == IRT_STRUCT && pty && pty->mode == MODE_MUTABLE) {
            // a `var` struct param is a mutable borrow — a pointer to the caller's
            // storage, so field writes propagate. The pointer *is* the slot.
            IrType *ptr = ir_type_new(cc.a, IRT_PTR); ptr->elem = pt; ptr->ptr_mut = true;
            IrValue *pv = ir_add_param(f, ptr, pin);
            ir_env_add(&cc, pnm, pv, NULL);
        } else if (pt->kind == IRT_STRUCT) {
            // a by-value struct param: materialize to a slot so its fields address
            IrValue *pv = ir_add_param(f, pt, pin);
            IrValue *slot = ir_alloca(f, cc.cur, pt);
            ir_store(f, cc.cur, slot, pv);
            ir_env_add(&cc, pnm, slot, NULL);
        } else {
            IrValue *pv = ir_add_param(f, pt, pin);
            ir_env_add(&cc, pnm, NULL, pv);
        }
    }
    // second pass (all params now in scope, so RHS `m`/`a.len` resolve): scalar refinements
    // (`n < 4096`, `i < a.len`) and dependent-length constraints (`out i32[m]`) → entry assumes.
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pty = p->decl->as.variable_decl.type;
        IrLocal *l = ir_env_find(&cc, p->decl->as.variable_decl.name);
        IrValue *pv = l ? l->param : NULL;
        if (pv && pv->type && pv->type->kind==IRT_INT)
            ir_lower_param_refinements(&cc, pv, pty, p->decl);
        if (pv && pty && pty->kind==TYPE_ARRAY && pty->array_len<0 && pty->size_expr)
            ir_lower_slice_len_refinement(&cc, pv, pty);
    }
    ir_lower_stmts(&cc, fn->as.function_decl.body);
    if (!ir_is_set_term(cc.cur))
        ir_set_ret(cc.cur, NULL);   // implicit unit return / end of proc
    ir_finalize_cfg(f);
    return f;
}

// Lower a whole program to a module IrFunc list: every func/proc body, PLUS a
// declaration-only stub (is_extern, no body) for each extern func/proc so an
// interprocedural pass (effects, and later borrow/linearity) can classify calls to them
// WITHOUT consulting the AST — the module is self-contained. Returns the list head.
static IrFunc *ir_lower_module(DeclList *program, Arena *a) {
    IrFunc *head=NULL, *tail=NULL;
    for (DeclList *d = program; d; d = d->next) {
        if (!d->decl) continue;
        IrFunc *f = NULL;
        DeclKind k = d->decl->kind;
        if ((k==DECL_FUNCTION || k==DECL_PROCEDURE) && d->decl->as.function_decl.body) {
            f = ir_lower_function(d->decl, program, a);
        } else if (k==DECL_EXTERN_FUNCTION || k==DECL_EXTERN_PROCEDURE) {
            Id *nm = d->decl->as.function_decl.name;
            f = ir_func_new(a, nm ? ir_intern(a, nm->name, nm->length) : NULL, NULL,
                            k==DECL_EXTERN_FUNCTION ? IR_FUNC_PURE : IR_FUNC_PROC);
            f->is_extern = true; f->src_decl = d->decl;
        }
        if (f) { if (!head) head=tail=f; else { tail->next=f; tail=f; } }
    }
    return head;
}

#endif // LAIN_IR_LOWER_H
