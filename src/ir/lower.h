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
    IrFunc  *f;
    IrBlock *cur;         // block currently being filled
    Arena   *a;
    IrLocal *locals;
    bool     unsafe;      // inside an `unsafe` block (elem_ptr etc. become unchecked)
    // innermost loop targets, for break/continue
    IrBlock *loop_head, *loop_exit;
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
static bool ir_type_is_agg(IrType *t) {
    return t && (t->kind==IRT_ARRAY || t->kind==IRT_SLICE || t->kind==IRT_STRUCT);
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
            return ir_type_new(c->a, IRT_STRUCT);   // named struct/enum/alias (opaque for now)
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
        IrValue *p = ir_elem_ptr(c->f, c->cur, base, idx, elem);
        p->line = e->line; p->col = e->col;
        c->cur->instrs_tail->unchecked = c->unsafe;   // elem_ptr just emitted
        return p;
    }
    // member / other lvalues: not yet lowered — placeholder slot
    return ir_alloca(c->f, c->cur, ir_lower_type(c, e->type));
}

static IrValue *ir_lower_expr(LowerCtx *c, Expr *e) {
    if (!e) return ir_const_int(c->f, c->cur, 0, ir_type_int(c->a,32,true));
    IrType *ty = ir_lower_type(c, e->type);
    switch (e->kind) {
        case EXPR_LITERAL:
            return ir_const_int(c->f, c->cur, e->as.literal_expr.value, ty);
        case EXPR_IDENTIFIER: {
            IrLocal *l = ir_env_find(c, e->as.identifier_expr.id);
            if (l && l->param) return l->param;
            if (l && l->aggregate) return l->slot;   // array/slice base pointer, read directly
            if (l && l->slot)  return ir_load(c->f, c->cur, l->slot,
                                              l->slot->type->elem ? l->slot->type->elem : ty);
            return ir_const_int(c->f, c->cur, 0, ty);   // unresolved (e.g. global) — placeholder
        }
        case EXPR_BINARY: {
            Expr *L=e->as.binary_expr.left, *R=e->as.binary_expr.right;
            bool sgn = !(e->type && e->type->kind==TYPE_SIMPLE && e->type->int_width_cache>0 && !e->type->int_signed_cache);
            IrValue *x = ir_lower_expr(c,L), *y = ir_lower_expr(c,R);
            IrOp op; IrWrapMode wrap; IrCmp cmp;
            if (ir_cmp_op(e->as.binary_expr.op, sgn, &cmp)) return ir_icmp(c->f,c->cur,cmp,x,y);
            if (ir_bin_op(e->as.binary_expr.op, sgn, &op, &wrap)) {
                IrValue *r = ir_binop(c->f,c->cur,op,x,y,ty);
                c->cur->instrs_tail->wrap = wrap;
                return r;
            }
            // `and`/`or` (logical) — non-short-circuit lowering for pure operands (TODO: CFG).
            IrValue *r = ir_binop(c->f,c->cur,IR_AND,x,y,ir_type_bool(c->a));
            return r;
        }
        case EXPR_UNARY: {
            IrValue *x = ir_lower_expr(c, e->as.unary_expr.right);
            if (e->as.unary_expr.op == TOKEN_MINUS) { IrInstr *ins=ir_instr(c->f,IR_NEG,ty,1); ins->operands[0]=x; ir_emit(c->cur,ins); return ins->result; }
            IrInstr *ins=ir_instr(c->f,IR_BNOT,ty,1); ins->operands[0]=x; ir_emit(c->cur,ins); return ins->result;
        }
        case EXPR_INDEX: {
            IrValue *addr = ir_lower_addr(c, e);
            return ir_load(c->f, c->cur, addr, ty);
        }
        case EXPR_MEMBER: {
            Id *m = e->as.member_expr.member;
            if (m && m->length==3 && strncmp(m->name,"len",3)==0) {
                IrValue *s = ir_lower_expr(c, e->as.member_expr.target);
                return ir_slice_len(c->f, c->cur, s);
            }
            // struct field read — placeholder (field index lowering TODO)
            return ir_const_int(c->f, c->cur, 0, ty);
        }
        case EXPR_CALL: {
            Decl *callee = e->as.call_expr.callee ? e->as.call_expr.callee->decl : NULL;
            int n=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next) n++;
            IrInstr *ins = ir_instr(c->f, IR_CALL, (e->type && e->type->kind!=TYPE_SIMPLE) || (ty->kind!=IRT_UNIT) ? ty : NULL, n);
            ins->aux.callee = callee;
            int i=0; for (ExprList *a=e->as.call_expr.args; a; a=a->next,i++) ins->operands[i]=ir_lower_expr(c,a->expr);
            ir_emit(c->cur, ins);
            return ins->result ? ins->result : ir_const_int(c->f,c->cur,0,ir_type_int(c->a,32,true));
        }
        case EXPR_CAST: {
            IrValue *x = ir_lower_expr(c, e->as.cast_expr.expr);
            IrInstr *ins = ir_instr(c->f, IR_CAST, ir_lower_type(c, e->as.cast_expr.target_type), 1);
            ins->operands[0]=x; ins->aux.cast_kind=IR_CAST_BITCAST; ir_emit(c->cur,ins);
            return ins->result;
        }
        default:
            return ir_const_int(c->f, c->cur, 0, ty);   // unhandled expr → placeholder
    }
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
        case STMT_RETURN:
            ir_set_ret(c->cur, s->as.return_stmt.value ? ir_lower_expr(c, s->as.return_stmt.value) : NULL);
            break;
        case STMT_IF: {
            IrValue *cond = ir_lower_expr(c, s->as.if_stmt.cond);
            IrBlock *tb = ir_new_block(c->f), *jb = ir_new_block(c->f);
            IrBlock *eb = s->as.if_stmt.else_branch ? ir_new_block(c->f) : jb;
            ir_set_br_cond(c->cur, cond, tb, eb);
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
        case STMT_BREAK:    if (c->loop_exit) ir_set_br(c->cur, c->loop_exit); break;
        case STMT_CONTINUE: if (c->loop_head) ir_set_br(c->cur, c->loop_head); break;
        case STMT_UNSAFE: { bool o=c->unsafe; c->unsafe=true; ir_lower_stmts(c, s->as.unsafe_stmt.body); c->unsafe=o; break; }
        default: break;   // for/match/defer/use — TODO
    }
}
static void ir_lower_stmts(LowerCtx *c, StmtList *body) {
    for (StmtList *b = body; b && !ir_is_set_term(c->cur); b = b->next) ir_lower_stmt(c, b->stmt);
}

// ── entry: lower one function ────────────────────────────────────────────────
IrFunc *ir_lower_function(Decl *fn, Arena *a) {
    IrType tmp; LowerCtx cc = {0}; cc.a = a; (void)tmp;
    IrFunc *f = ir_func_new(a, fn->as.function_decl.name,
                            NULL, fn->kind==DECL_FUNCTION ? IR_FUNC_PURE : IR_FUNC_PROC);
    f->src_decl = fn;
    cc.f = f; cc.cur = f->entry;
    f->ret_type = ir_lower_type(&cc, fn->as.function_decl.return_type);
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        IrType *pt = ir_lower_type(&cc, p->decl->as.variable_decl.type);
        IrValue *pv = ir_add_param(f, pt, p->decl->as.variable_decl.name);
        ir_env_add(&cc, p->decl->as.variable_decl.name, NULL, pv);
    }
    ir_lower_stmts(&cc, fn->as.function_decl.body);
    if (!ir_is_set_term(cc.cur))
        ir_set_ret(cc.cur, NULL);   // implicit unit return / end of proc
    ir_finalize_cfg(f);
    return f;
}

#endif // LAIN_IR_LOWER_H
