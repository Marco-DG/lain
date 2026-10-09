#ifndef LAIN_JUDGE_TEXT_H
#define LAIN_JUDGE_TEXT_H
// text.h — the normative text form of the VC, the certificate and the model
// (judge_interfaces.md §2-§4): a printer, a parser, and an SMT-LIB printer for phase 1a's
// offline harness. Judge side only: the solver includes format.h, never this file.
//
// CANONICAL. Each structure has exactly one text, and the parser accepts nothing else, so
// print(parse(t)) == t, byte for byte, for every t it accepts:
//   - indentation is two spaces per level and is SIGNIFICANT (a case's branch labels are told
//     from an enclosing case's by their level);
//   - tokens are separated by one space; no tab, CR, trailing space or empty line; printable
//     ASCII only; every line, the last included, ends with a newline;
//   - integers are decimal with no leading zero and no `-0`, of at most 512 bits (J_LIMBS); the
//     format can state a longer one, and it is refused here as the kernel would refuse it;
//   - a sum lists its variables in declaration order, once each, with nonzero coefficients;
//     the empty sum is `0`;
//   - a variable name contains no `|` and no `\`, so it is a quoted SMT-LIB symbol as it stands.
// The parser checks SYNTAX and NAMES: every name a term uses is declared. Whether a certificate's
// references are in scope and its numbers unique, whether a multiplier has the right sign, and
// whether a case has one branch per disjunct, are the kernel's checks, not this file's.
//
// Grammar (<cons> := <sum> <op> <int>;  <sum> := 0 | <int>*<name> [ + <int>*<name> ]...):
//   vc <id>                                <id> := <function> <irhash> <point> <kind> <ordinal>
//     var <name>                           one per variable, all before the first hypothesis
//     hyp h<k> <origin> : <cons>           <op> is <= or =; numbered h1, h2, ... in order
//     hyp h<k> <origin> : or { <cons> , <cons> | <cons> }        two or more disjuncts
//     goal <cons>                          <op> is <= or != (an equality goal is two VCs)
//   end
//   cert <id> v1
//     f<k> = lin <int>*<ref> [ + <int>*<ref> ]...   <ref> := h<k> | g | f<k> | s<k> | s<k>.<n>
//     f<k> = rnd f<j>
//     close f<k>
//     split s<k> : <cons>                  <op> is <=; then `le:` and `gt:` at the split's level,
//     le:                                  each followed by its branch one level deeper
//       ...
//     gt:
//       ...
//     case s<k> : h<j>                     then `1:`, `2:`, ... at the case's level, likewise
//     1:
//       ...
//   end
//   model <id> v1
//     <name> = <int>                       one per variable of the VC, in declaration order
//   end
// A branch, the certificate's body included, is a list of steps ending in its one terminal step:
// `close`, `split` or `case`. An origin is one or more tokens, none of them `:`.
#include <stdio.h>
#include <string.h>
#include "judge/format.h"
#include "judge/mem.h"

// ── JInt on the text path: decimal conversion and nothing else ──────────────────────────────────
// x = x*10 + d on the magnitude; false if it no longer fits J_LIMBS.
static bool jtext_mul10_add(JInt *x, uint32_t d) {
    uint64_t carry = d;
    for (int i = 0; i < x->n; i++) {
        uint64_t t = (uint64_t)x->limb[i] * 10u + carry;
        x->limb[i] = (uint32_t)t; carry = t >> 32;
    }
    if (carry) { if (x->n >= J_LIMBS) return false; x->limb[x->n++] = (uint32_t)carry; }
    return true;
}
// x = x / 10 on the magnitude; returns the remainder.
static uint32_t jtext_div10(JInt *x) {
    uint64_t rem = 0;
    for (int i = x->n - 1; i >= 0; i--) {
        uint64_t cur = (rem << 32) | x->limb[i];
        x->limb[i] = (uint32_t)(cur / 10u); rem = cur % 10u;
    }
    while (x->n && x->limb[x->n - 1] == 0) x->n--;
    return (uint32_t)rem;
}
static void jint_print(const JInt *x, FILE *o) {
    if (x->n == 0) { fputc('0', o); return; }
    JInt t = *x; char buf[J_LIMBS * 10 + 2]; int k = 0;
    while (t.n) buf[k++] = (char)('0' + jtext_div10(&t));
    if (x->neg) fputc('-', o);
    while (k) fputc(buf[--k], o);
}
static bool jint_parse(const char *s, size_t len, JInt *x) {
    memset(x, 0, sizeof *x);
    size_t i = 0; bool neg = false;
    if (i < len && s[i] == '-') { neg = true; i++; }
    if (i >= len) return false;
    if (s[i] == '0' && len - i > 1) return false;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        if (!jtext_mul10_add(x, (uint32_t)(s[i] - '0'))) return false;
    }
    if (neg && x->n == 0) return false;
    x->neg = neg;
    return true;
}

// ── printing ──────────────────────────────────────────────────────────────────────────────────
static void jtext_indent(int depth, FILE *o) { for (int k = 0; k < depth; k++) fputs("  ", o); }
static void jid_print(const JId *id, FILE *o) { fprintf(o, "%s %s %s %s %s", id->fn, id->irhash, id->point, id->kind, id->ordinal); }
static void jcons_print(const JCons *c, const char *const *vars, FILE *o) {
    if (c->nt == 0) fputc('0', o);
    for (int i = 0; i < c->nt; i++) {
        if (i) fputs(" + ", o);
        jint_print(&c->t[i].coef, o); fputc('*', o); fputs(vars[c->t[i].var], o);
    }
    fputs(c->op == J_LE ? " <= " : c->op == J_EQ ? " = " : " != ", o);
    jint_print(&c->rhs, o);
}
static void jvc_print(const JVc *v, FILE *o) {
    fputs("vc ", o); jid_print(&v->id, o); fputc('\n', o);
    for (int i = 0; i < v->nvars; i++) fprintf(o, "  var %s\n", v->vars[i]);
    for (int i = 0; i < v->nhyps; i++) {
        const JHyp *h = &v->hyps[i];
        fprintf(o, "  hyp h%d %s : ", i + 1, h->origin);
        if (!h->disj) jcons_print(&h->cons, v->vars, o);
        else {
            fputs("or { ", o);
            for (int d = 0; d < h->ndis; d++) {
                if (d) fputs(" | ", o);
                for (int c = 0; c < h->dis[d].nc; c++) {
                    if (c) fputs(" , ", o);
                    jcons_print(&h->dis[d].c[c], v->vars, o);
                }
            }
            fputs(" }", o);
        }
        fputc('\n', o);
    }
    fputs("  goal ", o); jcons_print(&v->goal, v->vars, o); fputc('\n', o);
    fputs("end\n", o);
}
static void jref_print(const JRef *r, FILE *o) {
    switch (r->k) {
        case J_REF_H: fprintf(o, "h%d", r->a); break;
        case J_REF_G: fputc('g', o); break;
        case J_REF_F: fprintf(o, "f%d", r->a); break;
        case J_REF_S: fprintf(o, "s%d", r->a); break;
        case J_REF_SC: fprintf(o, "s%d.%d", r->a, r->b); break;
    }
}
// A split's fact is written over the VC's variables, so printing a certificate needs its VC.
static void jsteps_print(const JSteps *b, const JVc *v, int depth, FILE *o) {
    for (int i = 0; i < b->n; i++) {
        const JStep *s = &b->s[i];
        jtext_indent(depth, o);
        switch (s->k) {
            case J_LIN:
                fprintf(o, "f%d = lin ", s->f);
                for (int m = 0; m < s->nmul; m++) {
                    if (m) fputs(" + ", o);
                    jint_print(&s->mul[m].m, o); fputc('*', o); jref_print(&s->mul[m].r, o);
                }
                fputc('\n', o);
                break;
            case J_RND: fprintf(o, "f%d = rnd f%d\n", s->f, s->of); break;
            case J_CLOSE: fprintf(o, "close f%d\n", s->f); break;
            case J_SPLIT: case J_CASE:
                if (s->k == J_SPLIT) { fprintf(o, "split s%d : ", s->s); jcons_print(&s->split, v->vars, o); fputc('\n', o); }
                else fprintf(o, "case s%d : h%d\n", s->s, s->hyp);
                for (int k = 0; k < s->nbr; k++) {
                    jtext_indent(depth, o);
                    if (s->k == J_SPLIT) fputs(k == 0 ? "le:\n" : "gt:\n", o);
                    else fprintf(o, "%d:\n", k + 1);
                    jsteps_print(&s->br[k], v, depth + 1, o);
                }
                break;
        }
    }
}
static void jcert_print(const JCert *c, const JVc *v, FILE *o) {
    fputs("cert ", o); jid_print(&c->id, o); fputs(" v1\n", o);
    jsteps_print(&c->body, v, 1, o);
    fputs("end\n", o);
}
static void jmodel_print(const JModel *m, const JVc *v, FILE *o) {
    fputs("model ", o); jid_print(&m->id, o); fputs(" v1\n", o);
    for (int i = 0; i < m->n; i++) { fprintf(o, "  %s = ", v->vars[i]); jint_print(&m->val[i], o); fputc('\n', o); }
    fputs("end\n", o);
}

// SMT-LIB 2, logic QF_LIA, for phase 1a's offline harness: the VC holds iff `hyps and not goal`
// is unsat. Variables are quoted symbols |…|, so `%s6@bb3.4` and `atom.mul(%v3,%v7)` stand as
// they are; each hypothesis's origin follows it as a comment.
static void jsmt_int(const JInt *x, FILE *o) {
    if (!x->neg) { jint_print(x, o); return; }
    JInt m = *x; m.neg = 0;
    fputs("(- ", o); jint_print(&m, o); fputc(')', o);
}
static void jsmt_cons(const JCons *c, const char *const *vars, FILE *o) {
    fputs(c->op == J_NE ? "(not (= " : c->op == J_EQ ? "(= " : "(<= ", o);
    if (c->nt == 0) fputc('0', o);
    if (c->nt > 1) fputs("(+", o);
    for (int i = 0; i < c->nt; i++) {
        if (c->nt > 1) fputc(' ', o);
        fputs("(* ", o); jsmt_int(&c->t[i].coef, o); fprintf(o, " |%s|)", vars[c->t[i].var]);
    }
    if (c->nt > 1) fputc(')', o);
    fputc(' ', o); jsmt_int(&c->rhs, o);
    fputs(c->op == J_NE ? "))" : ")", o);
}
static void jvc_print_smtlib(const JVc *v, FILE *o) {
    fputs("; vc ", o); jid_print(&v->id, o); fputs("\n(set-logic QF_LIA)\n", o);
    for (int i = 0; i < v->nvars; i++) fprintf(o, "(declare-fun |%s| () Int)\n", v->vars[i]);
    for (int i = 0; i < v->nhyps; i++) {
        const JHyp *h = &v->hyps[i];
        fputs("(assert ", o);
        if (!h->disj) jsmt_cons(&h->cons, v->vars, o);
        else {
            fputs("(or", o);
            for (int d = 0; d < h->ndis; d++) {
                fputs(" (and", o);
                for (int c = 0; c < h->dis[d].nc; c++) { fputc(' ', o); jsmt_cons(&h->dis[d].c[c], v->vars, o); }
                fputc(')', o);
            }
            fputc(')', o);
        }
        fprintf(o, ") ; h%d %s\n", i + 1, h->origin);
    }
    fputs("(assert (not ", o); jsmt_cons(&v->goal, v->vars, o); fputs(")) ; g\n(check-sat)\n", o);
}

// ── parsing ───────────────────────────────────────────────────────────────────────────────────
// The parser reads a NUL-terminated text from `p`, one item at a time (a file may hold many).
// Every refusal leaves the line number and the reason in `err`, and the output unspecified: a
// caller must not use a structure whose parse failed. Running out of the caller's region is a
// refusal like any other.
typedef struct { const char *s; size_t n; } JTok;
typedef struct {
    JMem *m;
    const char *p;              // the next line
    int line;                   // lines taken so far
    int cur;                    // the line last looked at, which a refusal names
    char err[200];
    JTok *tok; int ntok;        // the line last looked at: its tokens and level
    int depth;
} JParse;

static JParse jparse_over(JMem *m, const char *text) {
    JParse ps; memset(&ps, 0, sizeof ps); ps.m = m; ps.p = text; return ps;
}
static bool jp_fail(JParse *ps, const char *why) {
    if (!ps->err[0]) snprintf(ps->err, sizeof ps->err, "line %d: %s", ps->cur, why);
    return false;
}
static bool jt_is(JTok t, const char *w) { return strlen(w) == t.n && memcmp(t.s, w, t.n) == 0; }
static char *jp_dup(JParse *ps, JTok t) {
    char *d = JMEM_NEW(ps->m, char, t.n + 1);
    if (d) { memcpy(d, t.s, t.n); d[t.n] = 0; }
    return d;
}
// Look at the next line without taking it: its level and its tokens. False at the end of the
// text with `err` empty, or with `err` set if the line is not canonical. *next is where the
// line after it starts.
static bool jp_peek(JParse *ps, const char **next) {
    const char *s = ps->p;
    ps->cur = ps->line + 1;
    if (!*s) return false;
    const char *e = s; while (*e && *e != '\n') e++;
    if (!*e) return jp_fail(ps, "the last line does not end with a newline");
    size_t sp = 0; while (s + sp < e && s[sp] == ' ') sp++;
    if (s + sp == e) return jp_fail(ps, "an empty line");
    if (sp % 2) return jp_fail(ps, "indentation is two spaces per level");
    int n = 1;
    for (const char *c = s + sp; c < e; c++) {
        if (*c < 0x20 || *c > 0x7e) return jp_fail(ps, "a character that is not printable ASCII (a tab, a CR, ...)");
        if (*c == ' ') {
            if (c[1] == ' ' || c + 1 == e) return jp_fail(ps, "tokens are separated by one space, with none at the end");
            n++;
        }
    }
    ps->tok = JMEM_NEW(ps->m, JTok, n);
    if (!ps->tok) return jp_fail(ps, "out of memory (the caller's region is full)");
    ps->ntok = 0;
    for (const char *c = s + sp; c < e; ) {
        const char *b = c; while (c < e && *c != ' ') c++;
        ps->tok[ps->ntok].s = b; ps->tok[ps->ntok].n = (size_t)(c - b); ps->ntok++;
        if (c < e) c++;
    }
    ps->depth = (int)(sp / 2);
    *next = e + 1;
    return true;
}
static void jp_take(JParse *ps, const char *next) { ps->p = next; ps->line++; }
// Take the next line, which must be at `depth`; `what` names it in the refusal.
static bool jp_expect(JParse *ps, int depth, const char *what) {
    const char *next;
    if (!jp_peek(ps, &next)) return ps->err[0] ? false : jp_fail(ps, what);
    if (ps->depth != depth) return jp_fail(ps, what);
    jp_take(ps, next);
    return true;
}
// `<letter><decimal>`, as in h12, f3, s2: no leading zero, at least 1, at most 10^9.
static bool jt_numbered(JTok t, char c, int32_t *out) {
    if (t.n < 2 || t.n > 11 || t.s[0] != c || t.s[1] == '0') return false;
    int64_t v = 0;
    for (size_t i = 1; i < t.n; i++) {
        if (t.s[i] < '0' || t.s[i] > '9') return false;
        v = v * 10 + (t.s[i] - '0');
    }
    if (v > 1000000000) return false;
    *out = (int32_t)v; return true;
}
static bool jp_id(JParse *ps, const JTok *t, JId *id) {
    id->fn = jp_dup(ps, t[0]); id->irhash = jp_dup(ps, t[1]); id->point = jp_dup(ps, t[2]);
    id->kind = jp_dup(ps, t[3]); id->ordinal = jp_dup(ps, t[4]);
    if (!id->fn || !id->irhash || !id->point || !id->kind || !id->ordinal) return jp_fail(ps, "out of memory (the caller's region is full)");
    return true;
}
static int32_t jp_var_find(const JVc *v, const char *s, size_t n, int32_t from) {
    for (int32_t i = from; i < v->nvars; i++) if (strlen(v->vars[i]) == n && memcmp(v->vars[i], s, n) == 0) return i;
    return -1;
}
enum { JP_OP_LE = 1, JP_OP_EQ = 2, JP_OP_NE = 4 };
// `<sum> <op> <int>` from t[0..n), over the VC's variables; `ops` says which operators may stand.
static bool jp_cons(JParse *ps, const JTok *t, int n, const JVc *v, int ops, JCons *c) {
    memset(c, 0, sizeof *c);
    if (n < 3) return jp_fail(ps, "a constraint is `<sum> <op> <int>`");
    JTok op = t[n - 2];
    if (jt_is(op, "<=") && (ops & JP_OP_LE)) c->op = J_LE;
    else if (jt_is(op, "=") && (ops & JP_OP_EQ)) c->op = J_EQ;
    else if (jt_is(op, "!=") && (ops & JP_OP_NE)) c->op = J_NE;
    else return jp_fail(ps, ops == JP_OP_LE ? "the operator here is `<=`"
                            : ops == (JP_OP_LE | JP_OP_EQ) ? "the operator here is `<=` or `=`"
                            : "the operator here is `<=` or `!=`");
    if (!jint_parse(t[n - 1].s, t[n - 1].n, &c->rhs)) return jp_fail(ps, "a constant is a canonical decimal of at most 512 bits");
    int nsum = n - 2;
    if (nsum == 1 && jt_is(t[0], "0")) return true;                    // the empty sum
    if (nsum % 2 == 0) return jp_fail(ps, "a sum's terms are joined by ` + `");
    c->nt = (nsum + 1) / 2;
    c->t = JMEM_NEW(ps->m, JTerm, c->nt);
    if (!c->t) return jp_fail(ps, "out of memory (the caller's region is full)");
    for (int i = 0; i < c->nt; i++) {
        JTok w = t[2 * i];
        if (i && !jt_is(t[2 * i - 1], "+")) return jp_fail(ps, "a sum's terms are joined by ` + `");
        const char *star = memchr(w.s, '*', w.n);
        if (!star) return jp_fail(ps, "a term is `<int>*<name>`");
        if (!jint_parse(w.s, (size_t)(star - w.s), &c->t[i].coef) || c->t[i].coef.n == 0)
            return jp_fail(ps, "a term's coefficient is a nonzero canonical decimal of at most 512 bits");
        const char *nm = star + 1; size_t nl = w.n - (size_t)(star - w.s) - 1;
        int32_t from = i ? c->t[i - 1].var + 1 : 0;
        int32_t k = jp_var_find(v, nm, nl, from);
        if (k < 0) return jp_fail(ps, jp_var_find(v, nm, nl, 0) >= 0
                                      ? "a sum names its variables in declaration order, once each"
                                      : "a term names a variable the VC does not declare");
        c->t[i].var = k;
    }
    return true;
}
static bool jp_name_ok(JTok t) {
    for (size_t i = 0; i < t.n; i++) if (t.s[i] == '|' || t.s[i] == '\\') return false;
    return true;
}

static bool jvc_parse(JParse *ps, JVc *v) {
    memset(v, 0, sizeof *v);
    const char *next;
    if (!jp_peek(ps, &next)) return ps->err[0] ? false : jp_fail(ps, "expected a VC");
    if (ps->depth != 0 || ps->ntok != 6 || !jt_is(ps->tok[0], "vc"))
        return jp_fail(ps, "a VC starts `vc <function> <irhash> <point> <kind> <ordinal>`");
    if (!jp_id(ps, ps->tok + 1, &v->id)) return false;
    jp_take(ps, next);
    int32_t cvars = 0, chyps = 0;
    bool goal = false;
    for (;;) {
        if (!jp_peek(ps, &next)) return ps->err[0] ? false : jp_fail(ps, "a VC ends with `end`");
        const JTok *t = ps->tok; int n = ps->ntok;
        if (ps->depth == 0) {
            if (n != 1 || !jt_is(t[0], "end")) return jp_fail(ps, "a VC ends with `end`, alone on its line");
            if (!goal) return jp_fail(ps, "a VC has a goal");
            jp_take(ps, next);
            return true;
        }
        if (ps->depth != 1) return jp_fail(ps, "a VC's lines are one level deep");
        if (goal) return jp_fail(ps, "the goal is the last line before `end`");
        jp_take(ps, next);
        if (jt_is(t[0], "var")) {
            if (n != 2) return jp_fail(ps, "a variable is `var <name>`");
            if (v->nhyps) return jp_fail(ps, "variables are declared before the first hypothesis");
            if (!jp_name_ok(t[1])) return jp_fail(ps, "a variable's name contains no `|` and no `\\`");
            if (jp_var_find(v, t[1].s, t[1].n, 0) >= 0) return jp_fail(ps, "a variable is declared twice");
            if (v->nvars == cvars) {
                int32_t nc = cvars ? cvars * 2 : 8;
                const char **nv = JMEM_NEW(ps->m, const char *, nc);
                if (!nv) return jp_fail(ps, "out of memory (the caller's region is full)");
                if (v->nvars) memcpy(nv, v->vars, sizeof *nv * (size_t)v->nvars);
                v->vars = nv; cvars = nc;
            }
            if (!(v->vars[v->nvars] = jp_dup(ps, t[1]))) return jp_fail(ps, "out of memory (the caller's region is full)");
            v->nvars++;
        } else if (jt_is(t[0], "hyp")) {
            int32_t k;
            if (n < 2 || !jt_numbered(t[1], 'h', &k)) return jp_fail(ps, "a hypothesis is `hyp h<k> <origin> : <fact>`");
            if (k != v->nhyps + 1) return jp_fail(ps, "hypotheses are numbered h1, h2, ... in order");
            int colon = -1;
            for (int i = 2; i < n; i++) if (jt_is(t[i], ":")) { colon = i; break; }
            if (colon < 3) return jp_fail(ps, "a hypothesis has an origin, then ` : ` and its fact");
            if (v->nhyps == chyps) {
                int32_t nc = chyps ? chyps * 2 : 8;
                JHyp *nh = JMEM_NEW(ps->m, JHyp, nc);
                if (!nh) return jp_fail(ps, "out of memory (the caller's region is full)");
                if (v->nhyps) memcpy(nh, v->hyps, sizeof *nh * (size_t)v->nhyps);
                v->hyps = nh; chyps = nc;
            }
            JHyp *h = &v->hyps[v->nhyps]; memset(h, 0, sizeof *h);
            JTok o = { t[2].s, (size_t)(t[colon - 1].s + t[colon - 1].n - t[2].s) };
            if (!(h->origin = jp_dup(ps, o))) return jp_fail(ps, "out of memory (the caller's region is full)");
            const JTok *f = t + colon + 1; int fn = n - colon - 1;
            if (fn >= 1 && jt_is(f[0], "or")) {
                if (fn < 3 || !jt_is(f[1], "{") || !jt_is(f[fn - 1], "}"))
                    return jp_fail(ps, "a disjunction is `or { <cons> , <cons> | <cons> }`");
                int nd = 1;
                for (int i = 2; i < fn - 1; i++) if (jt_is(f[i], "|")) nd++;
                if (nd < 2) return jp_fail(ps, "a disjunction has two or more disjuncts");
                h->disj = true; h->ndis = nd;
                if (!(h->dis = JMEM_NEW(ps->m, JConj, nd))) return jp_fail(ps, "out of memory (the caller's region is full)");
                // disjuncts end at `|` or the closing `}`; constraints within one at `,`
                for (int d = 0, i = 2; d < nd; d++) {
                    int end = i; while (end < fn - 1 && !jt_is(f[end], "|")) end++;
                    int nc = 1;
                    for (int j = i; j < end; j++) if (jt_is(f[j], ",")) nc++;
                    h->dis[d].nc = nc;
                    if (!(h->dis[d].c = JMEM_NEW(ps->m, JCons, nc))) return jp_fail(ps, "out of memory (the caller's region is full)");
                    for (int c = 0, cs = i; c < nc; c++) {
                        int ce = cs; while (ce < end && !jt_is(f[ce], ",")) ce++;
                        if (!jp_cons(ps, f + cs, ce - cs, v, JP_OP_LE | JP_OP_EQ, &h->dis[d].c[c])) return false;
                        cs = ce + 1;
                    }
                    i = end + 1;
                }
            } else if (!jp_cons(ps, f, fn, v, JP_OP_LE | JP_OP_EQ, &h->cons)) return false;
            v->nhyps++;
        } else if (jt_is(t[0], "goal")) {
            if (!jp_cons(ps, t + 1, n - 1, v, JP_OP_LE | JP_OP_NE, &v->goal)) return false;
            goal = true;
        } else return jp_fail(ps, "a VC's line is `var`, `hyp` or `goal`");
    }
}

static bool jp_ref(JParse *ps, JTok t, JRef *r) {
    memset(r, 0, sizeof *r);
    const char *why = "a reference is h<k>, g, f<k>, s<k> or s<k>.<n>";
    if (jt_is(t, "g")) { r->k = J_REF_G; return true; }
    const char *dot = memchr(t.s, '.', t.n);
    if (dot) {
        JTok a = { t.s, (size_t)(dot - t.s) };
        char nb[12]; size_t bl = t.n - a.n - 1;
        if (bl == 0 || bl > 10) return jp_fail(ps, why);
        nb[0] = 'n'; memcpy(nb + 1, dot + 1, bl);
        JTok b = { nb, bl + 1 };
        if (!jt_numbered(a, 's', &r->a) || !jt_numbered(b, 'n', &r->b)) return jp_fail(ps, why);
        r->k = J_REF_SC; return true;
    }
    if (jt_numbered(t, 'h', &r->a)) { r->k = J_REF_H; return true; }
    if (jt_numbered(t, 'f', &r->a)) { r->k = J_REF_F; return true; }
    if (jt_numbered(t, 's', &r->a)) { r->k = J_REF_S; return true; }
    return jp_fail(ps, why);
}
// One branch at `depth`: steps up to and including its terminal step.
static bool jp_steps(JParse *ps, const JVc *v, int depth, JSteps *out) {
    const char *ends = "a branch is steps ending in `close`, `split` or `case`, one level below its label";
    int32_t cap = 4; out->n = 0;
    if (!(out->s = JMEM_NEW(ps->m, JStep, cap))) return jp_fail(ps, "out of memory (the caller's region is full)");
    for (;;) {
        const char *next;
        if (!jp_peek(ps, &next)) return ps->err[0] ? false : jp_fail(ps, depth == 1 ? "a certificate's steps end in `close`, `split` or `case`" : ends);
        if (ps->depth != depth) return jp_fail(ps, depth == 1 ? "a certificate's steps end in `close`, `split` or `case`" : ends);
        jp_take(ps, next);
        if (out->n == cap) {
            JStep *ns = JMEM_NEW(ps->m, JStep, cap * 2);
            if (!ns) return jp_fail(ps, "out of memory (the caller's region is full)");
            memcpy(ns, out->s, sizeof *ns * (size_t)out->n); out->s = ns; cap *= 2;
        }
        JStep *s = &out->s[out->n++]; memset(s, 0, sizeof *s);
        const JTok *t = ps->tok; int n = ps->ntok;
        if (n >= 3 && jt_is(t[1], "=") && jt_numbered(t[0], 'f', &s->f)) {
            if (jt_is(t[2], "rnd")) {
                if (n != 4 || !jt_numbered(t[3], 'f', &s->of)) return jp_fail(ps, "a cut is `f<k> = rnd f<j>`");
                s->k = J_RND; continue;
            }
            if (!jt_is(t[2], "lin")) return jp_fail(ps, "a derived fact is `f<k> = lin ...` or `f<k> = rnd f<j>`");
            int nm = n - 3;
            if (nm < 1 || nm % 2 == 0) return jp_fail(ps, "a combination is `f<k> = lin <int>*<ref> + ...`");
            s->k = J_LIN; s->nmul = (nm + 1) / 2;
            if (!(s->mul = JMEM_NEW(ps->m, JMul, s->nmul))) return jp_fail(ps, "out of memory (the caller's region is full)");
            for (int i = 0; i < s->nmul; i++) {
                JTok w = t[3 + 2 * i];
                if (i && !jt_is(t[2 + 2 * i], "+")) return jp_fail(ps, "a combination's terms are joined by ` + `");
                const char *star = memchr(w.s, '*', w.n);
                if (!star || !jint_parse(w.s, (size_t)(star - w.s), &s->mul[i].m))
                    return jp_fail(ps, "a combination's term is `<int>*<ref>`, the int canonical and of at most 512 bits");
                JTok r = { star + 1, w.n - (size_t)(star - w.s) - 1 };
                if (!jp_ref(ps, r, &s->mul[i].r)) return false;
            }
            continue;
        }
        if (jt_is(t[0], "close")) {
            if (n != 2 || !jt_numbered(t[1], 'f', &s->f)) return jp_fail(ps, "a close is `close f<k>`");
            s->k = J_CLOSE;
            return true;
        }
        if (jt_is(t[0], "split")) {
            if (n < 4 || !jt_numbered(t[1], 's', &s->s) || !jt_is(t[2], ":")) return jp_fail(ps, "a split is `split s<k> : <sum> <= <int>`");
            if (!jp_cons(ps, t + 3, n - 3, v, JP_OP_LE, &s->split)) return false;
            s->k = J_SPLIT; s->nbr = 2;
            if (!(s->br = JMEM_NEW(ps->m, JSteps, 2))) return jp_fail(ps, "out of memory (the caller's region is full)");
            for (int k = 0; k < 2; k++) {
                const char *lab = k == 0 ? "le:" : "gt:";
                if (!jp_expect(ps, depth, "a split is followed by `le:` and `gt:`, each at its level")) return false;
                if (ps->ntok != 1 || !jt_is(ps->tok[0], lab)) return jp_fail(ps, "a split is followed by `le:` and `gt:`, each at its level");
                if (!jp_steps(ps, v, depth + 1, &s->br[k])) return false;
            }
            return true;
        }
        if (jt_is(t[0], "case")) {
            if (n != 4 || !jt_numbered(t[1], 's', &s->s) || !jt_is(t[2], ":") || !jt_numbered(t[3], 'h', &s->hyp))
                return jp_fail(ps, "a case is `case s<k> : h<j>`");
            s->k = J_CASE;
            int32_t bcap = 4;
            if (!(s->br = JMEM_NEW(ps->m, JSteps, bcap))) return jp_fail(ps, "out of memory (the caller's region is full)");
            for (;;) {
                char lab[16]; snprintf(lab, sizeof lab, "%d:", s->nbr + 1);
                if (!jp_peek(ps, &next)) { if (ps->err[0]) return false; break; }
                if (ps->depth != depth || ps->ntok != 1 || !jt_is(ps->tok[0], lab)) break;
                jp_take(ps, next);
                if (s->nbr == bcap) {
                    JSteps *nb = JMEM_NEW(ps->m, JSteps, bcap * 2);
                    if (!nb) return jp_fail(ps, "out of memory (the caller's region is full)");
                    memcpy(nb, s->br, sizeof *nb * (size_t)s->nbr); s->br = nb; bcap *= 2;
                }
                if (!jp_steps(ps, v, depth + 1, &s->br[s->nbr])) return false;
                s->nbr++;
            }
            if (s->nbr == 0) return jp_fail(ps, "a case is followed by its branches `1:`, `2:`, ..., each at its level");
            return true;
        }
        return jp_fail(ps, "a step is `f<k> = lin ...`, `f<k> = rnd f<j>`, `split`, `case` or `close`");
    }
}
// A certificate for the VC `v` (its split facts are written over v's variables).
static bool jcert_parse(JParse *ps, const JVc *v, JCert *c) {
    memset(c, 0, sizeof *c);
    if (!jp_expect(ps, 0, "a certificate starts `cert <function> <irhash> <point> <kind> <ordinal> v1`")) return false;
    if (ps->ntok != 7 || !jt_is(ps->tok[0], "cert") || !jt_is(ps->tok[6], "v1")) {
        return jp_fail(ps, "a certificate starts `cert <function> <irhash> <point> <kind> <ordinal> v1`");
    }
    if (!jp_id(ps, ps->tok + 1, &c->id)) return false;
    if (!jp_steps(ps, v, 1, &c->body)) return false;
    if (!jp_expect(ps, 0, "a certificate's terminal step is followed by `end`")) return false;
    if (ps->ntok != 1 || !jt_is(ps->tok[0], "end")) return jp_fail(ps, "a certificate's terminal step is followed by `end`");
    return true;
}
// A model for the VC `v`: one line per variable, in v's declaration order.
static bool jmodel_parse(JParse *ps, const JVc *v, JModel *m) {
    memset(m, 0, sizeof *m);
    if (!jp_expect(ps, 0, "a model starts `model <function> <irhash> <point> <kind> <ordinal> v1`")) return false;
    if (ps->ntok != 7 || !jt_is(ps->tok[0], "model") || !jt_is(ps->tok[6], "v1")) {
        return jp_fail(ps, "a model starts `model <function> <irhash> <point> <kind> <ordinal> v1`");
    }
    if (!jp_id(ps, ps->tok + 1, &m->id)) return false;
    if (!(m->val = JMEM_NEW(ps->m, JInt, v->nvars))) return jp_fail(ps, "out of memory (the caller's region is full)");
    for (m->n = 0; m->n < v->nvars; m->n++) {
        if (!jp_expect(ps, 1, "a model gives every variable of its VC a value, in declaration order")) return false;
        const JTok *t = ps->tok;
        if (ps->ntok != 3 || !jt_is(t[1], "=") || !jt_is(t[0], v->vars[m->n])) {
            return jp_fail(ps, "a model gives every variable of its VC a value, in declaration order");
        }
        if (!jint_parse(t[2].s, t[2].n, &m->val[m->n])) return jp_fail(ps, "a value is a canonical decimal of at most 512 bits");
    }
    if (!jp_expect(ps, 0, "a model ends with `end` after its last variable")) return false;
    if (ps->ntok != 1 || !jt_is(ps->tok[0], "end")) return jp_fail(ps, "a model ends with `end` after its last variable");
    return true;
}

#endif // LAIN_JUDGE_TEXT_H
