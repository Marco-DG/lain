// cmin.c — a MINIMAL SAFE-C FRONT END for Lain-IR.  (REBUILD.md Stage VI / E0.6)
//
// ★ WHY THIS EXISTS. E0.2 says the IR is sovereign: no primitive encodes front-end-specific
// policy. Until now that was an ARGUMENT — a litmus I re-ran by hand whenever the lattice
// grew ("could safe-C and safe-Fortran lower to this?"). This makes it a TEST. A second,
// non-Lain front end lowers a C subset straight to Lain-IR and gets the same guarantees from
// the same analyses: bounds, overflow, definite assignment, use-after-move, division.
//
// The discipline that makes it evidence rather than decoration:
//   · it includes ONLY `ir/*` and `analysis/*` — no ast.h, no sema.h, no Lain parser. If it
//     needed one of those, sovereignty would be false and the build would say so.
//   · it does its own lexing and parsing. Sharing Lain's would prove nothing.
//   · anything it cannot model becomes `ir_opaque`, never a guess — the same fail-closed rule
//     the Lain lowering follows.
//
// The C subset: `int` and `int[N]`, functions with int params, locals, assignment, `+ - * /`,
// comparisons, `if`/`else`, `while`, `return`, array indexing, calls. Deliberately small —
// the point is whether the IR needs anything Lain-shaped, and that question is answered by
// the first hundred lines, not by C's full grammar.
//
//   gcc -std=c99 -o /tmp/cmin src/frontends/cmin/cmin.c -I src && /tmp/cmin file.c
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "ir/emit_c.h"
#include "analysis/linearity.h"
#include "analysis/borrow.h"
#include "analysis/definite_init.h"
#include "analysis/vra.h"
#include "analysis/report.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ── lexer ────────────────────────────────────────────────────────────────────
typedef enum { T_EOF, T_INT, T_ID, T_NUM, T_PUNCT, T_IF, T_ELSE, T_WHILE, T_RETURN } TKind;
typedef struct { TKind k; const char *s; int len; long long num; } Tok;

static const char *cm_src;
static const char *cm_base;     // start of the buffer, for line numbers
static Tok cm_tok;

// Source positions are not optional: an engine that cannot say WHERE is not one anyone can
// run, and the IR stamps every instruction from `ir_cur_line`. A second front end has to feed
// that just as the Lain lowering does — which is itself part of what this exercise tests.
static void cm_set_pos(const char *at) {
    isize line = 1, col = 1;
    for (const char *p = cm_base; p && p < at; p++) { if (*p=='\n') { line++; col = 1; } else col++; }
    ir_cur_line = line; ir_cur_col = col;
}

static bool cm_kw(const char *s, int len, const char *kw) {
    return (int)strlen(kw)==len && strncmp(s,kw,(size_t)len)==0;
}
static void cm_next(void) {
    while (*cm_src) {
        if (isspace((unsigned char)*cm_src)) { cm_src++; continue; }
        if (cm_src[0]=='/' && cm_src[1]=='/') { while (*cm_src && *cm_src!='\n') cm_src++; continue; }
        break;
    }
    if (!*cm_src) { cm_tok.k = T_EOF; cm_tok.len = 0; return; }
    cm_set_pos(cm_src);
    if (isdigit((unsigned char)*cm_src)) {
        char *end; cm_tok.num = strtoll(cm_src, &end, 0);
        cm_tok.k = T_NUM; cm_tok.s = cm_src; cm_tok.len = (int)(end - cm_src); cm_src = end; return;
    }
    if (isalpha((unsigned char)*cm_src) || *cm_src=='_') {
        const char *b = cm_src;
        while (isalnum((unsigned char)*cm_src) || *cm_src=='_') cm_src++;
        cm_tok.s = b; cm_tok.len = (int)(cm_src - b);
        if      (cm_kw(b,cm_tok.len,"int"))    cm_tok.k = T_INT;
        else if (cm_kw(b,cm_tok.len,"if"))     cm_tok.k = T_IF;
        else if (cm_kw(b,cm_tok.len,"else"))   cm_tok.k = T_ELSE;
        else if (cm_kw(b,cm_tok.len,"while"))  cm_tok.k = T_WHILE;
        else if (cm_kw(b,cm_tok.len,"return")) cm_tok.k = T_RETURN;
        else                                   cm_tok.k = T_ID;
        return;
    }
    // two-character operators first, so `<=` does not lex as `<`
    static const char *two[] = { "==","!=","<=",">=", NULL };
    for (int i=0; two[i]; i++)
        if (cm_src[0]==two[i][0] && cm_src[1]==two[i][1]) {
            cm_tok.k=T_PUNCT; cm_tok.s=cm_src; cm_tok.len=2; cm_src+=2; return;
        }
    cm_tok.k=T_PUNCT; cm_tok.s=cm_src; cm_tok.len=1; cm_src++;
}
static bool cm_is(const char *p) { return cm_tok.k==T_PUNCT && cm_kw(cm_tok.s,cm_tok.len,p); }
static bool cm_eat(const char *p) { if (cm_is(p)) { cm_next(); return true; } return false; }
static void cm_expect(const char *p) {
    if (!cm_eat(p)) { fprintf(stderr, "cmin: expected '%s'\n", p); exit(2); }
}

// ── lowering context ─────────────────────────────────────────────────────────
// A local is a NAME and the IR slot behind it. That is the whole symbol table: the front end
// owns names, the IR owns values, and nothing crosses.
typedef struct CmLocal { const char *name; int len; IrValue *slot; IrType *ty; struct CmLocal *next; } CmLocal;
typedef struct {
    Arena   *a;
    IrFunc  *f;
    IrBlock *cur;
    CmLocal *locals;
    IrType  *i32;
    IrFunc  *mod;           // functions lowered so far (for call resolution)
} Cm;

static CmLocal *cm_find(Cm *c, const char *n, int len) {
    for (CmLocal *l = c->locals; l; l = l->next)
        if (l->len==len && strncmp(l->name,n,(size_t)len)==0) return l;
    return NULL;
}
static void cm_add(Cm *c, const char *n, int len, IrValue *slot, IrType *ty) {
    CmLocal *l = arena_push_aligned(c->a, CmLocal);
    l->name=n; l->len=len; l->slot=slot; l->ty=ty; l->next=c->locals; c->locals=l;
}

static IrValue *cm_expr(Cm *c);

// primary := NUM | ID | ID '(' args ')' | ID '[' expr ']' | '(' expr ')'
static IrValue *cm_primary(Cm *c) {
    if (cm_tok.k == T_NUM) { long long v = cm_tok.num; cm_next(); return ir_const_int(c->f,c->cur,v,c->i32); }
    if (cm_eat("(")) { IrValue *v = cm_expr(c); cm_expect(")"); return v; }
    if (cm_tok.k == T_ID) {
        const char *n = cm_tok.s; int len = cm_tok.len; cm_next();
        if (cm_is("(")) {                                  // call
            cm_next();
            IrValue *args[8]; int n_args = 0;
            if (!cm_is(")")) { do { if (n_args<8) args[n_args++] = cm_expr(c); } while (cm_eat(",")); }
            cm_expect(")");
            IrInstr *ins = ir_instr(c->f, IR_CALL, c->i32, n_args);
            ins->aux.callee = ir_intern(c->a, n, len);
            for (int i=0;i<n_args;i++) ins->operands[i] = args[i];
            ir_emit(c->cur, ins);
            return ins->result;
        }
        CmLocal *l = cm_find(c, n, len);
        if (!l) return ir_opaque(c->f, c->cur, c->i32, false, "cmin-unknown-name", NULL, 0);
        if (cm_eat("[")) {                                 // a[i]
            IrValue *idx = cm_expr(c); cm_expect("]");
            IrValue *p = ir_elem_ptr(c->f, c->cur, l->slot, idx, c->i32);
            return ir_load(c->f, c->cur, p, c->i32);
        }
        if (l->ty && l->ty->kind == IRT_ARRAY) return l->slot;   // bare array = its base
        return ir_load(c->f, c->cur, l->slot, c->i32);
    }
    return ir_opaque(c->f, c->cur, c->i32, false, "cmin-unhandled-primary", NULL, 0);
}

static IrValue *cm_mul(Cm *c) {
    IrValue *x = cm_primary(c);
    for (;;) {
        if (cm_eat("*")) x = ir_binop(c->f,c->cur,IR_MUL,x,cm_primary(c),c->i32);
        else if (cm_eat("/")) x = ir_binop(c->f,c->cur,IR_SDIV,x,cm_primary(c),c->i32);
        else if (cm_eat("%")) x = ir_binop(c->f,c->cur,IR_SREM,x,cm_primary(c),c->i32);
        else return x;
    }
}
static IrValue *cm_addsub(Cm *c) {
    IrValue *x = cm_mul(c);
    for (;;) {
        if (cm_eat("+")) x = ir_binop(c->f,c->cur,IR_ADD,x,cm_mul(c),c->i32);
        else if (cm_eat("-")) x = ir_binop(c->f,c->cur,IR_SUB,x,cm_mul(c),c->i32);
        else return x;
    }
}
static IrValue *cm_expr(Cm *c) {
    IrValue *x = cm_addsub(c);
    for (;;) {
        IrCmp k;
        if      (cm_is("<"))  k = IR_CMP_SLT;
        else if (cm_is(">"))  k = IR_CMP_SGT;
        else if (cm_is("<=")) k = IR_CMP_SLE;
        else if (cm_is(">=")) k = IR_CMP_SGE;
        else if (cm_is("==")) k = IR_CMP_EQ;
        else if (cm_is("!=")) k = IR_CMP_NE;
        else return x;
        cm_next();
        x = ir_icmp(c->f, c->cur, k, x, cm_addsub(c));
    }
}

static void cm_block(Cm *c);

// stmt := 'int' ID ['[' NUM ']'] ['=' expr] ';' | ID '=' expr ';' | ID '[' expr ']' '=' expr ';'
//       | 'if' '(' expr ')' stmt ['else' stmt] | 'while' '(' expr ')' stmt
//       | 'return' [expr] ';' | '{' stmt* '}' | expr ';'
static void cm_stmt(Cm *c) {
    if (cm_is("{")) { cm_block(c); return; }
    if (cm_tok.k == T_INT) {
        cm_next();
        const char *n = cm_tok.s; int len = cm_tok.len; cm_next();
        if (cm_eat("[")) {                                  // int a[N];
            long long n_elem = cm_tok.num; cm_next(); cm_expect("]");
            IrType *arr = ir_type_new(c->a, IRT_ARRAY); arr->elem = c->i32; arr->array_len = n_elem;
            IrValue *slot = ir_alloca_array(c->f, c->cur, arr);
            cm_add(c, n, len, slot, arr);
            cm_expect(";");
            return;
        }
        IrValue *slot = ir_alloca(c->f, c->cur, c->i32);
        cm_add(c, n, len, slot, c->i32);
        if (cm_eat("=")) ir_store(c->f, c->cur, slot, cm_expr(c));
        cm_expect(";");
        return;
    }
    if (cm_tok.k == T_IF) {
        cm_next(); cm_expect("("); IrValue *cond = cm_expr(c); cm_expect(")");
        IrBlock *t = ir_new_block(c->f), *e = ir_new_block(c->f), *j = ir_new_block(c->f);
        ir_set_br_cond(c->cur, cond, t, e);
        c->cur = t; cm_stmt(c); if (c->cur->term.kind==IR_TERM_BR && !c->cur->term.a) ir_set_br(c->cur, j);
        if (c->cur->term.kind == IR_TERM_BR && c->cur->term.a == NULL) ir_set_br(c->cur, j);
        if (!c->cur->term.a && c->cur->term.kind != IR_TERM_RET) ir_set_br(c->cur, j);
        c->cur = e;
        if (cm_tok.k == T_ELSE) { cm_next(); cm_stmt(c); }
        if (!c->cur->term.a && c->cur->term.kind != IR_TERM_RET) ir_set_br(c->cur, j);
        c->cur = j;
        return;
    }
    if (cm_tok.k == T_WHILE) {
        cm_next();
        IrBlock *head = ir_new_block(c->f), *body = ir_new_block(c->f), *exit = ir_new_block(c->f);
        ir_set_br(c->cur, head);
        c->cur = head; head->is_loop_header = true;
        cm_expect("("); IrValue *cond = cm_expr(c); cm_expect(")");
        ir_set_br_cond(c->cur, cond, body, exit);
        c->cur = body; cm_stmt(c);
        if (!c->cur->term.a && c->cur->term.kind != IR_TERM_RET) ir_set_br(c->cur, head);
        c->cur = exit;
        return;
    }
    if (cm_tok.k == T_RETURN) {
        cm_next();
        IrValue *v = cm_is(";") ? NULL : cm_expr(c);
        cm_expect(";");
        ir_set_ret(c->cur, v);
        c->cur = ir_new_block(c->f);           // anything after a return is its own dead block
        return;
    }
    if (cm_tok.k == T_ID) {
        const char *n = cm_tok.s; int len = cm_tok.len;
        const char *save_src = cm_src; Tok save_tok = cm_tok;
        cm_next();
        CmLocal *l = cm_find(c, n, len);
        if (l && cm_eat("[")) {                             // a[i] = e;
            IrValue *idx = cm_expr(c); cm_expect("]");
            if (cm_eat("=")) {
                IrValue *p = ir_elem_ptr(c->f, c->cur, l->slot, idx, c->i32);
                ir_store(c->f, c->cur, p, cm_expr(c));
                cm_expect(";");
                return;
            }
        } else if (l && cm_eat("=")) {                      // x = e;
            ir_store(c->f, c->cur, l->slot, cm_expr(c));
            cm_expect(";");
            return;
        }
        cm_src = save_src; cm_tok = save_tok;               // not an assignment: an expression
    }
    (void)cm_expr(c);
    cm_expect(";");
}

static void cm_block(Cm *c) {
    cm_expect("{");
    CmLocal *saved = c->locals;                 // C block scoping, the front end's own business
    while (!cm_is("}") && cm_tok.k != T_EOF) cm_stmt(c);
    cm_expect("}");
    c->locals = saved;
}

// func := 'int' ID '(' ['int' ID {',' 'int' ID}] ')' block
static IrFunc *cm_function(Arena *a, IrFunc *mod) {
    if (cm_tok.k != T_INT) { fprintf(stderr, "cmin: expected 'int'\n"); exit(2); }
    cm_next();
    const char *fn = cm_tok.s; int fl = cm_tok.len; cm_next();
    IrType *i32 = ir_type_int(a, 32, true);
    IrFunc *f = ir_func_new(a, ir_intern(a, fn, fl), i32, IR_FUNC_PROC);
    Cm c = {0}; c.a=a; c.f=f; c.cur=f->entry; c.i32=i32; c.mod=mod;
    cm_expect("(");
    // `(void)` is an EMPTY parameter list, not a parameter named `void`. Taking it literally
    // gave main a parameter the emitter does not declare, and the generated C did not compile.
    if (cm_tok.k == T_ID && cm_kw(cm_tok.s, cm_tok.len, "void")) cm_next();
    if (!cm_is(")")) {
        do {
            bool arr = false;
            if (cm_tok.k == T_INT) cm_next();
            const char *pn = cm_tok.s; int pl = cm_tok.len; cm_next();
            if (cm_eat("[")) { cm_expect("]"); arr = true; }     // int a[] — a pointer parameter
            IrType *pt = i32;
            if (arr) { pt = ir_type_new(a, IRT_PTR); pt->elem = i32; pt->ptr_mut = true; }
            IrValue *pv = ir_add_param(f, pt, ir_intern(a, pn, pl));
            if (arr) cm_add(&c, pn, pl, pv, pt);              // the pointer IS the base
            else { IrValue *slot = ir_alloca(f, c.cur, i32);   // give a scalar param a home slot
                   ir_store(f, c.cur, slot, pv);
                   cm_add(&c, pn, pl, slot, i32); }
        } while (cm_eat(","));
    }
    cm_expect(")");
    cm_block(&c);
    if (c.cur->term.kind == IR_TERM_BR && !c.cur->term.a) ir_set_ret(c.cur, NULL);
    ir_finalize_cfg(f);
    return f;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.c> [--emit-c]\n", argv[0]); return 2; }
    bool emit = (argc >= 3 && strcmp(argv[2], "--emit-c") == 0);
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { fprintf(stderr, "cmin: cannot open %s\n", argv[1]); return 2; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, fp); buf[n] = 0; fclose(fp);

    Arena a = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 512);
    cm_src = buf; cm_base = buf; cm_next();
    IrFunc *head = NULL, *tail = NULL;
    while (cm_tok.k != T_EOF) {
        IrFunc *f = cm_function(&a, head);
        if (!head) head = tail = f; else { tail->next = f; tail = f; }
    }
    if (emit) { ir_emit_module_c(head, stdout, &a); return 0; }

    // ★ THE POINT: the SAME analyses, unchanged, over IR that no Lain code produced.
    lin_mod = head; bor_loan_mod = head; vra_mod = head;
    int found = 0;
    for (IrFunc *f = head; f; f = f->next) {
        if (f->incomplete) { fprintf(stderr, "note: '%.*s' not fully modelled (%s)\n",
                                     (int)f->name->length, f->name->name,
                                     f->incomplete_why ? f->incomplete_why : "?"); continue; }
        found += ir_report_findings(f, head, argv[1], true);
    }
    fprintf(stderr, found ? "cmin: %d finding(s)\n" : "cmin: no findings\n", found);
    return found ? 1 : 0;
}
