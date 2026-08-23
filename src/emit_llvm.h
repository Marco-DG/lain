#ifndef EMIT_LLVM_H
#define EMIT_LLVM_H

// ── Lain-IR → LLVM-IR seam (Phase 1, first vertebra) ─────────────────────────
// The whole reason Lain beats C: it carries PROOFS the C type system erases. This
// emitter lowers a Lain-integer subset to LLVM-IR and injects Lain's proven param
// ranges as `@llvm.assume`, so the optimizer deletes what the proof makes dead —
// assembly no transpiler-to-C can reach. Scope today: integer funcs, +/-/*, the
// comparisons, `if <cond> { return } ...`, `return`. Everything else is skipped
// with a comment (C stays the portable fallback target). This is a demonstration
// of the seam, not yet the full backend; aggressive proofs are gated on Phase-0
// trust (see internal/design/GRAIL_BACKEND.md).

#include <stdio.h>
#include "ast.h"

static FILE *llf;
static int   ll_tmp;          // SSA temp counter
static int   ll_lbl;          // block-label counter

static void ll_type(Type *t, char *out, size_t n) {
    int bits; bool sgn;
    if (t && parse_iN_uN(t, &bits, &sgn)) { snprintf(out, n, "i%d", bits); return; }
    // aliases (Pct = i32 ...) resolve to an integer width; bool → i1; else i32.
    long long lo, hi;
    if (t && type_integer_range(t, &lo, &hi)) { snprintf(out, n, "i32"); return; }
    snprintf(out, n, "i32");
}

// Emit any needed instructions for `e`; write its SSA operand into `out`.
static void ll_expr(Expr *e, char *out, size_t n) {
    if (!e) { snprintf(out, n, "0"); return; }
    switch (e->kind) {
      case EXPR_LITERAL:
        snprintf(out, n, "%lld", (long long)e->as.literal_expr.value);
        return;
      case EXPR_IDENTIFIER:
        snprintf(out, n, "%%%.*s", (int)e->as.identifier_expr.id->length,
                 e->as.identifier_expr.id->name);
        return;
      case EXPR_BINARY: {
        char a[64], b[64];
        ll_expr(e->as.binary_expr.left,  a, sizeof a);
        ll_expr(e->as.binary_expr.right, b, sizeof b);
        const char *op = NULL, *icmp = NULL;
        switch (e->as.binary_expr.op) {
          case TOKEN_PLUS:     op = "add";  break;
          case TOKEN_MINUS:    op = "sub";  break;
          case TOKEN_ASTERISK: op = "mul";  break;
          case TOKEN_ANGLE_BRACKET_RIGHT:        icmp = "sgt"; break;
          case TOKEN_ANGLE_BRACKET_LEFT:         icmp = "slt"; break;
          case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:  icmp = "sge"; break;
          case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:   icmp = "sle"; break;
          case TOKEN_EQUAL_EQUAL:                icmp = "eq";  break;
          case TOKEN_BANG_EQUAL:                 icmp = "ne";  break;
          default: break;
        }
        int t = ll_tmp++;
        if (icmp) fprintf(llf, "  %%t%d = icmp %s i32 %s, %s\n", t, icmp, a, b);
        else if (op) fprintf(llf, "  %%t%d = %s i32 %s, %s\n", t, op, a, b);
        else fprintf(llf, "  %%t%d = add i32 %s, 0 ; unsupported binop\n", t, a);
        snprintf(out, n, "%%t%d", t);
        return;
      }
      default:
        fprintf(llf, "  ; unsupported expr kind %d\n", e->kind);
        snprintf(out, n, "0");
        return;
    }
}

// Emit statements; returns true if the block was terminated (a `ret`).
static bool ll_stmts(StmtList *body);

static bool ll_stmt(Stmt *s) {
    if (!s) return false;
    switch (s->kind) {
      case STMT_RETURN: {
        char v[64]; ll_expr(s->as.return_stmt.value, v, sizeof v);
        fprintf(llf, "  ret i32 %s\n", v);
        return true;
      }
      case STMT_IF: {
        // `if cond { then } [else]` — supports the early-return form: then-body
        // ends in `ret`, execution falls through to `cont` for later statements.
        char c[64]; ll_expr(s->as.if_stmt.cond, c, sizeof c);
        int thenL = ll_lbl++, contL = ll_lbl++;
        fprintf(llf, "  br i1 %s, label %%L%d, label %%L%d\n", c, thenL, contL);
        fprintf(llf, "L%d:\n", thenL);
        bool term = ll_stmts(s->as.if_stmt.then_body);
        if (!term) fprintf(llf, "  br label %%L%d\n", contL);
        fprintf(llf, "L%d:\n", contL);
        return false;
      }
      default:
        fprintf(llf, "  ; unsupported stmt kind %d\n", s->kind);
        return false;
    }
}

static bool ll_stmts(StmtList *body) {
    for (StmtList *b = body; b; b = b->next)
        if (ll_stmt(b->stmt)) return true;
    return false;
}

static void ll_func(Decl *d) {
    DeclFunction *f = &d->as.function_decl;
    char rty[16]; ll_type(f->return_type, rty, sizeof rty);
    fprintf(llf, "define %s @%.*s(", rty, (int)f->name->length, f->name->name);
    bool first = true;
    for (DeclList *p = f->params; p; p = p->next) {
        Decl *pv = p->decl; if (!pv || pv->kind != DECL_VARIABLE) continue;
        char pty[16]; ll_type(pv->as.variable_decl.type, pty, sizeof pty);
        fprintf(llf, "%s%s %%%.*s", first ? "" : ", ", pty,
                (int)pv->as.variable_decl.name->length, pv->as.variable_decl.name->name);
        first = false;
    }
    fprintf(llf, ") {\nentry:\n");
    ll_tmp = 0; ll_lbl = 0;
    // Proof-carrying: inject each param's PROVEN range as `assume`. The optimizer
    // exploits it; the assume evaporates. This is the beyond-C payload.
    for (DeclList *p = f->params; p; p = p->next) {
        Decl *pv = p->decl; if (!pv || pv->kind != DECL_VARIABLE) continue;
        Type *pt = pv->as.variable_decl.type;
        long long lo, hi, wlo, whi;
        if (!pt || !type_integer_range(pt, &lo, &hi)) continue;
        int bits; bool sgn;
        Type *base = pt;
        // only inject when the proven range is TIGHTER than the raw integer width
        bool tighter = false;
        if (parse_iN_uN(base, &bits, &sgn)) { /* plain width → not tighter */ }
        else if (type_integer_range(pt, &wlo, &whi)) tighter = (lo > -2147483648LL || hi < 2147483647LL);
        if (!tighter) continue;
        const char *nm_p = pv->as.variable_decl.name->name; int nm_l = (int)pv->as.variable_decl.name->length;
        int t1 = ll_tmp++;
        fprintf(llf, "  %%t%d = icmp sge i32 %%%.*s, %lld\n", t1, nm_l, nm_p, lo);
        fprintf(llf, "  call void @llvm.assume(i1 %%t%d)\n", t1);
        int t2 = ll_tmp++;
        fprintf(llf, "  %%t%d = icmp sle i32 %%%.*s, %lld\n", t2, nm_l, nm_p, hi);
        fprintf(llf, "  call void @llvm.assume(i1 %%t%d)\n", t2);
    }
    bool term = ll_stmts(f->body);
    if (!term) fprintf(llf, "  ret %s 0\n", rty);
    fprintf(llf, "}\n\n");
}

static void emit_llvm(DeclList *program, const char *out_file) {
    llf = fopen(out_file, "w");
    if (!llf) { fprintf(stderr, "emit_llvm: cannot open %s\n", out_file); return; }
    fprintf(llf, "; Lain-IR -> LLVM-IR (proof-carrying seam)\n");
    fprintf(llf, "declare void @llvm.assume(i1)\n\n");
    for (DeclList *dl = program; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (d && d->kind == DECL_FUNCTION) ll_func(d);
        else if (d && d->kind == DECL_PROCEDURE) ll_func(d);  // same shape here
    }
    fclose(llf);
}

#endif /* EMIT_LLVM_H */
