// src/tools/test_judge_format.c — unit test of the judge's text form (src/judge/text.h).
//
// The text is the interface the solver, third-party checkers and the hand-written kernel tests
// meet at, so it is tested from both sides:
//   1. canonical texts (judge_interfaces.md's worked examples, a disjunction, a case inside a
//      case, 512-bit constants, a model) parse, and print back BYTE FOR BYTE;
//   2. their parsed structures hold the values the text states (a parser and printer that drop
//      the same field would still round-trip);
//   3. a structure built by hand prints the expected text (the printer alone);
//   4. every malformed text in a table is refused, each with its EXPECTED reason (a refusal for another
//      reason is a failure: the test would pass while the rule it names is broken);
//   5. a mutation property: thousands of one-edit mutants of the canonical texts; every mutant
//      the parser accepts prints back exactly as it stands, since the form is canonical;
//   6. memory: a region too small at every size up to the one that suffices is refused with
//      "out of memory", never an abort, and the first size that suffices keeps sufficing;
//   7. the SMT-LIB printer's output for two VCs, golden.
//
// Build & run (run_ir_tests.sh does this):
//   gcc -std=c99 -o /tmp/test_judge_format src/tools/test_judge_format.c -I src && /tmp/test_judge_format
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "judge/text.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ── canonical texts ───────────────────────────────────────────────────────────────────────────
#define MAX512 "13407807929942597099574024998205846127479365820592393377723561443721764030073546976801874298166903427690031858186486050853753882811946569946433649006084095"
#define OVER512 "13407807929942597099574024998205846127479365820592393377723561443721764030073546976801874298166903427690031858186486050853753882811946569946433649006084096"

// A bound under its guard: `a[i]` under `i < a.len` (judge_interfaces §4, first example).
static const char *VC_GUARD =
    "vc f 9c1e bb2.3 bounds.hi 0\n"
    "  var %v3\n"
    "  var %v8\n"
    "  hyp h1 type %v3 usize : -1*%v3 <= 0\n"
    "  hyp h2 objsize %v8 : 1*%v8 <= 9223372036854775807\n"
    "  hyp h3 guard bb1->bb2 : 1*%v3 + -1*%v8 <= -1\n"
    "  goal 1*%v3 + -1*%v8 <= -1\n"
    "end\n";
static const char *CERT_GUARD =
    "cert f 9c1e bb2.3 bounds.hi 0 v1\n"
    "  f1 = lin 1*h3 + 1*g\n"
    "  close f1\n"
    "end\n";
// The model the same bound has under `i <= a.len` (§4's not-implied example).
static const char *MODEL_GUARD =
    "model f 9c1e bb2.3 bounds.hi 0 v1\n"
    "  %v3 = 5\n"
    "  %v8 = 5\n"
    "end\n";
// Parity, by two cuts, from 2x - 2y = 1 (§4, second example).
static const char *VC_PARITY =
    "vc parity 0 bb0.1 assume 0\n"
    "  var x\n"
    "  var y\n"
    "  hyp h1 def %v1 add : 2*x + -2*y = 1\n"
    "  goal 0 <= -1\n"
    "end\n";
static const char *CERT_PARITY =
    "cert parity 0 bb0.1 assume 0 v1\n"
    "  f1 = lin 1*h1\n"
    "  f2 = rnd f1\n"
    "  f3 = lin -1*h1\n"
    "  f4 = rnd f3\n"
    "  f5 = lin 1*f2 + 1*f4\n"
    "  close f5\n"
    "end\n";
// Branch and bound: 2x <= 1 and 2x >= 1 (§4, third example).
static const char *VC_BB =
    "vc bb 0 bb0.1 assume 0\n"
    "  var x\n"
    "  hyp h1 def %v1 : 2*x <= 1\n"
    "  hyp h2 def %v2 : -2*x <= -1\n"
    "  goal 0 <= -1\n"
    "end\n";
static const char *CERT_BB =
    "cert bb 0 bb0.1 assume 0 v1\n"
    "  split s1 : 1*x <= 0\n"
    "  le:\n"
    "    f1 = lin 1*h2 + 2*s1\n"
    "    close f1\n"
    "  gt:\n"
    "    f2 = lin 1*h1 + 2*s1\n"
    "    close f2\n"
    "end\n";
// Disjunctions (a disjunct of two constraints), an atom, 512-bit constants, an empty sum and a
// `!=` goal. The parser checks syntax only, so the certificate need not be valid: it nests a
// case inside branch 2 of a three-branch case, where only the LEVEL of the outer `3:` tells it
// from a third branch of the inner case.
static const char *VC_DISJ =
    "vc g 77ab bb3.2 divzero 0\n"
    "  var %v1\n"
    "  var %v2\n"
    "  var atom.mul(%v1,%v2)\n"
    "  hyp h1 path bb3 : or { 1*%v1 <= -1 , 1*%v2 = 4 | 1*%v1 = 0 | -1*%v1 <= -1 }\n"
    "  hyp h2 path bb2 : or { 1*%v2 <= 0 | -1*%v2 <= -1 }\n"
    "  hyp h3 lemma L1 atom.mul(%v1,%v2) : 1*%v1 + 1*%v2 + -1*atom.mul(%v1,%v2) <= " MAX512 "\n"
    "  hyp h4 const : 0 <= 0\n"
    "  hyp h5 type atom.mul(%v1,%v2) : -" MAX512 "*atom.mul(%v1,%v2) <= -" MAX512 "\n"
    "  goal 1*%v2 != 0\n"
    "end\n";
static const char *CERT_DISJ =
    "cert g 77ab bb3.2 divzero 0 v1\n"
    "  f1 = lin 3*h4\n"
    "  case s1 : h1\n"
    "  1:\n"
    "    f2 = lin 1*s1.2 + 1*g\n"
    "    close f2\n"
    "  2:\n"
    "    case s2 : h2\n"
    "    1:\n"
    "      f3 = lin 1*s2.1 + 0*f1\n"
    "      close f3\n"
    "    2:\n"
    "      split s3 : 1*%v1 + -1*atom.mul(%v1,%v2) <= " MAX512 "\n"
    "      le:\n"
    "        close f1\n"
    "      gt:\n"
    "        f4 = lin -" MAX512 "*s3\n"
    "        close f4\n"
    "  3:\n"
    "    f5 = lin 1*s1.1 + 1*h5\n"
    "    close f5\n"
    "end\n";

// ── helpers ───────────────────────────────────────────────────────────────────────────────────
static char *read_back(FILE *f) {
    long n; fflush(f); fseek(f, 0, SEEK_END); n = ftell(f); rewind(f);
    char *s = malloc((size_t)n + 1); size_t got = fread(s, 1, (size_t)n, f); s[got] = 0; fclose(f);
    return s;
}
static char *print_vc(const JVc *v) { FILE *f = tmpfile(); jvc_print(v, f); return read_back(f); }
static char *print_cert(const JCert *c, const JVc *v) { FILE *f = tmpfile(); jcert_print(c, v, f); return read_back(f); }
static char *print_model(const JModel *m, const JVc *v) { FILE *f = tmpfile(); jmodel_print(m, v, f); return read_back(f); }
static char *print_smt(const JVc *v) { FILE *f = tmpfile(); jvc_print_smtlib(v, f); return read_back(f); }
// `s` with the first `a` replaced by `b` (which must occur).
static char *sub(const char *s, const char *a, const char *b) {
    const char *at = strstr(s, a);
    if (!at) { printf("  test bug: `%s` not in the base text\n", a); exit(2); }
    size_t n = strlen(s) - strlen(a) + strlen(b);
    char *r = malloc(n + 1);
    size_t pre = (size_t)(at - s);
    memcpy(r, s, pre); strcpy(r + pre, b); strcat(r, at + strlen(a));
    return r;
}
static int64_t small(const JInt *x) {        // a JInt that fits in 63 bits, as an int64
    uint64_t m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(int64_t)m : (int64_t)m;
}
static void set_int(JInt *x, int64_t v) {
    memset(x, 0, sizeof *x);
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    x->neg = v < 0;
    while (m) { x->limb[x->n++] = (uint32_t)m; m >>= 32; }
}
static bool is_max512(const JInt *x, int neg) {
    if (x->n != J_LIMBS || x->neg != neg) return false;
    for (int i = 0; i < J_LIMBS; i++) if (x->limb[i] != 0xffffffffu) return false;
    return true;
}

static char region[1 << 22];

// ── 1 and 2: round trips, and the values parsed ───────────────────────────────────────────────
static void test_round_trips(void) {
    printf("── round trips ──\n");
    JMem m = jmem_over(region, sizeof region);
    struct { const char *vc, *cert, *name; } P[] = {
        { VC_GUARD, CERT_GUARD, "guard" }, { VC_PARITY, CERT_PARITY, "parity" },
        { VC_BB, CERT_BB, "branch and bound" }, { VC_DISJ, CERT_DISJ, "disjunction, nested case" },
    };
    for (int i = 0; i < 4; i++) {
        JVc v; JCert c;
        JParse ps = jparse_over(&m, P[i].vc);
        bool ok = jvc_parse(&ps, &v);
        CHECK(ok, "%s: VC refused: %s", P[i].name, ps.err);
        if (!ok) continue;
        CHECK(*ps.p == 0, "%s: VC parse stopped before the end", P[i].name);
        char *t = print_vc(&v); CHECK(strcmp(t, P[i].vc) == 0, "%s: VC prints back as\n%s", P[i].name, t); free(t);
        ps = jparse_over(&m, P[i].cert);
        ok = jcert_parse(&ps, &v, &c);
        CHECK(ok, "%s: certificate refused: %s", P[i].name, ps.err);
        if (!ok) continue;
        CHECK(*ps.p == 0, "%s: certificate parse stopped before the end", P[i].name);
        t = print_cert(&c, &v); CHECK(strcmp(t, P[i].cert) == 0, "%s: certificate prints back as\n%s", P[i].name, t); free(t);
        printf("  %s: VC and certificate round-trip\n", P[i].name);
    }

    // a file holds several items: a VC, then its certificate, read from one text
    {
        size_t n = strlen(VC_BB) + strlen(CERT_BB);
        char *both = malloc(n + 1); strcpy(both, VC_BB); strcat(both, CERT_BB);
        JParse ps = jparse_over(&m, both); JVc v; JCert c;
        bool ok = jvc_parse(&ps, &v) && jcert_parse(&ps, &v, &c);
        CHECK(ok && *ps.p == 0, "a VC then its certificate in one text: %s", ps.err);
        if (ok) printf("  one text holding a VC and its certificate parses as two items\n");
        free(both);
    }

    // the values the text states
    {
        JVc v; JParse ps = jparse_over(&m, VC_GUARD);
        if (jvc_parse(&ps, &v)) {
            CHECK(strcmp(v.id.fn, "f") == 0 && strcmp(v.id.irhash, "9c1e") == 0 && strcmp(v.id.point, "bb2.3") == 0
                  && strcmp(v.id.kind, "bounds.hi") == 0 && strcmp(v.id.ordinal, "0") == 0, "the obligation ID");
            CHECK(v.nvars == 2 && strcmp(v.vars[0], "%v3") == 0 && strcmp(v.vars[1], "%v8") == 0, "the variables");
            CHECK(v.nhyps == 3, "three hypotheses, not %d", v.nhyps);
            CHECK(strcmp(v.hyps[0].origin, "type %v3 usize") == 0, "h1's origin is `%s`", v.hyps[0].origin);
            CHECK(strcmp(v.hyps[2].origin, "guard bb1->bb2") == 0, "h3's origin is `%s`", v.hyps[2].origin);
            const JCons *h3 = &v.hyps[2].cons;
            CHECK(!v.hyps[2].disj && h3->op == J_LE && h3->nt == 2 && h3->t[0].var == 0 && small(&h3->t[0].coef) == 1
                  && h3->t[1].var == 1 && small(&h3->t[1].coef) == -1 && small(&h3->rhs) == -1, "h3 is %%v3 - %%v8 <= -1");
            CHECK(small(&v.hyps[1].cons.rhs) == INT64_MAX, "h2's bound is isize.max");
            CHECK(v.goal.op == J_LE && v.goal.nt == 2 && small(&v.goal.rhs) == -1, "the goal");
            JModel mo; ps = jparse_over(&m, MODEL_GUARD);
            bool ok = jmodel_parse(&ps, &v, &mo);
            CHECK(ok, "the model is refused: %s", ps.err);
            if (ok) {
                CHECK(mo.n == 2 && small(&mo.val[0]) == 5 && small(&mo.val[1]) == 5, "the model's values");
                char *t = print_model(&mo, &v); CHECK(strcmp(t, MODEL_GUARD) == 0, "the model prints back as\n%s", t); free(t);
                printf("  model: round-trips, values read\n");
            }
        }
        ps = jparse_over(&m, VC_DISJ);
        if (jvc_parse(&ps, &v)) {
            const JHyp *h1 = &v.hyps[0];
            CHECK(h1->disj && h1->ndis == 3 && h1->dis[0].nc == 2 && h1->dis[1].nc == 1 && h1->dis[2].nc == 1, "h1's shape");
            CHECK(h1->dis[0].c[1].op == J_EQ && h1->dis[0].c[1].t[0].var == 1 && small(&h1->dis[0].c[1].rhs) == 4, "h1's second constraint of disjunct 1 is %%v2 = 4");
            CHECK(h1->dis[2].c[0].op == J_LE && small(&h1->dis[2].c[0].t[0].coef) == -1, "h1's third disjunct");
            CHECK(is_max512(&v.hyps[2].cons.rhs, 0), "h3's constant is 2^512 - 1");
            CHECK(is_max512(&v.hyps[4].cons.rhs, 1) && is_max512(&v.hyps[4].cons.t[0].coef, 1), "h5's constant and coefficient are -(2^512 - 1)");
            CHECK(v.hyps[3].cons.nt == 0 && v.hyps[3].cons.rhs.n == 0, "h4 is the empty sum `0 <= 0`");
            CHECK(v.goal.op == J_NE, "the goal is `!=`");
            JCert c; ps = jparse_over(&m, CERT_DISJ);
            if (jcert_parse(&ps, &v, &c)) {
                CHECK(c.body.n == 2 && c.body.s[1].k == J_CASE && c.body.s[1].hyp == 1 && c.body.s[1].nbr == 3, "the outer case has three branches");
                const JStep *in = &c.body.s[1].br[1].s[0];
                CHECK(in->k == J_CASE && in->s == 2 && in->nbr == 2, "branch 2 holds a case of two branches, not %d", in->nbr);
                const JStep *sp = &in->br[1].s[0];
                CHECK(sp->k == J_SPLIT && sp->split.nt == 2 && sp->split.t[1].var == 2, "the split is over the atom");
                const JMul *mu = &c.body.s[1].br[0].s[0].mul[0];
                CHECK(mu->r.k == J_REF_SC && mu->r.a == 1 && mu->r.b == 2 && small(&mu->m) == 1, "s1.2 is a case fact");
                CHECK(c.body.s[1].br[0].s[0].mul[1].r.k == J_REF_G, "g is the goal's negation");
                CHECK(in->br[0].s[0].mul[1].m.n == 0, "a zero multiplier parses (its sign is the kernel's check)");
                printf("  parsed values: IDs, origins, terms, 512-bit constants, the nesting of cases\n");
            }
        }
    }
}

// ── 3: a structure built by hand prints the expected text ─────────────────────────────────────
static void test_printer_alone(void) {
    printf("── the printer alone ──\n");
    const char *vars[] = { "%v3", "%v8" };
    JTerm h1t[1], h2t[1], h3t[2], gt[2];
    JHyp hyps[3]; memset(hyps, 0, sizeof hyps);
    h1t[0].var = 0; set_int(&h1t[0].coef, -1);
    hyps[0].cons.t = h1t; hyps[0].cons.nt = 1; hyps[0].cons.op = J_LE; set_int(&hyps[0].cons.rhs, 0); hyps[0].origin = "type %v3 usize";
    h2t[0].var = 1; set_int(&h2t[0].coef, 1);
    hyps[1].cons.t = h2t; hyps[1].cons.nt = 1; hyps[1].cons.op = J_LE; set_int(&hyps[1].cons.rhs, INT64_MAX); hyps[1].origin = "objsize %v8";
    h3t[0].var = 0; set_int(&h3t[0].coef, 1); h3t[1].var = 1; set_int(&h3t[1].coef, -1);
    hyps[2].cons.t = h3t; hyps[2].cons.nt = 2; hyps[2].cons.op = J_LE; set_int(&hyps[2].cons.rhs, -1); hyps[2].origin = "guard bb1->bb2";
    memcpy(gt, h3t, sizeof gt);
    JVc v; memset(&v, 0, sizeof v);
    v.id.fn = "f"; v.id.irhash = "9c1e"; v.id.point = "bb2.3"; v.id.kind = "bounds.hi"; v.id.ordinal = "0";
    v.vars = vars; v.nvars = 2; v.hyps = hyps; v.nhyps = 3;
    v.goal.t = gt; v.goal.nt = 2; v.goal.op = J_LE; set_int(&v.goal.rhs, -1);
    char *t = print_vc(&v);
    CHECK(strcmp(t, VC_GUARD) == 0, "a hand-built VC prints as\n%s", t); free(t);

    JMul mul[2]; JStep steps[2]; memset(steps, 0, sizeof steps);
    set_int(&mul[0].m, 1); mul[0].r.k = J_REF_H; mul[0].r.a = 3;
    set_int(&mul[1].m, 1); mul[1].r.k = J_REF_G;
    steps[0].k = J_LIN; steps[0].f = 1; steps[0].mul = mul; steps[0].nmul = 2;
    steps[1].k = J_CLOSE; steps[1].f = 1;
    JCert c; c.id = v.id; c.body.s = steps; c.body.n = 2;
    t = print_cert(&c, &v);
    CHECK(strcmp(t, CERT_GUARD) == 0, "a hand-built certificate prints as\n%s", t); free(t);

    JInt vals[2]; set_int(&vals[0], 5); set_int(&vals[1], 5);
    JModel mo; mo.id = v.id; mo.val = vals; mo.n = 2;
    t = print_model(&mo, &v);
    CHECK(strcmp(t, MODEL_GUARD) == 0, "a hand-built model prints as\n%s", t); free(t);
    printf("  hand-built VC, certificate and model print as the canonical texts\n");
}

// ── 4: malformed texts, each refused for its own reason ───────────────────────────────────────
enum { K_VC, K_CERT, K_MODEL };
static int nrejected = 0, ncalls = 0;
static void reject(int kind, const char *base_vc, const char *text, const char *why, const char *label) {
    JMem m = jmem_over(region, sizeof region);
    JVc v; JCert c; JModel mo; bool ok;
    ncalls++;
    if (kind == K_VC) {
        JParse ps = jparse_over(&m, text);
        ok = jvc_parse(&ps, &v);
        CHECK(!ok, "%s: accepted", label);
        if (!ok) CHECK(strncmp(ps.err, "line ", 5) == 0 && strstr(ps.err, why), "%s: refused as `%s`, expected `%s`", label, ps.err, why);
        if (!ok && strstr(ps.err, why)) nrejected++;
        return;
    }
    JParse pv = jparse_over(&m, base_vc);
    if (!jvc_parse(&pv, &v)) { CHECK(0, "%s: base VC refused: %s", label, pv.err); return; }
    JParse ps = jparse_over(&m, text);
    ok = kind == K_CERT ? jcert_parse(&ps, &v, &c) : jmodel_parse(&ps, &v, &mo);
    CHECK(!ok, "%s: accepted", label);
    if (!ok) CHECK(strncmp(ps.err, "line ", 5) == 0 && strstr(ps.err, why), "%s: refused as `%s`, expected `%s`", label, ps.err, why);
    if (!ok && strstr(ps.err, why)) nrejected++;
}
static void test_rejections(void) {
    printf("── malformed texts ──\n");
    const char *G = VC_GUARD, *D = VC_DISJ;
    // layout
    reject(K_VC, 0, sub(G, "  var %v3", "\tvar %v3"), "printable ASCII", "a tab");
    reject(K_VC, 0, sub(G, "var %v3\n", "var %v3 \n"), "one space", "a trailing space");
    reject(K_VC, 0, sub(G, "var %v3", "var  %v3"), "one space", "two spaces between tokens");
    reject(K_VC, 0, sub(G, "  var %v3", "   var %v3"), "two spaces per level", "odd indentation");
    reject(K_VC, 0, sub(G, "end\n", "end\r\n"), "printable ASCII", "a CR");
    { char *t = sub(G, "end\n", "end"); reject(K_VC, 0, t, "does not end with a newline", "no final newline"); }
    reject(K_VC, 0, sub(G, "  var %v8\n", "\n  var %v8\n"), "empty line", "an empty line");
    reject(K_VC, 0, sub(G, "  var %v8\n", "    var %v8\n"), "one level deep", "a VC line two levels deep");
    reject(K_VC, 0, sub(G, "vc f 9c1e bb2.3 bounds.hi 0", "vc f 9c1e bb2.3 bounds.hi"), "a VC starts", "a header of four fields");
    reject(K_VC, 0, sub(G, "vc f", "  vc f"), "a VC starts", "an indented header");
    reject(K_VC, 0, "", "expected a VC", "an empty text");
    // numbers
    reject(K_VC, 0, sub(G, "<= -1\n  goal", "<= -01\n  goal"), "canonical decimal", "a leading zero");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "-1*%v3 <= -0"), "canonical decimal", "-0");
    reject(K_VC, 0, sub(G, "9223372036854775807", OVER512), "at most 512 bits", "2^512");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "-1*%v3 <= 1x"), "canonical decimal", "a constant with a letter");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "0*%v3 <= 0"), "nonzero", "a zero coefficient");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "+1*%v3 <= 0"), "nonzero canonical", "a `+` sign");
    // sums and names
    reject(K_VC, 0, sub(G, "1*%v8 <= 9223", "1*%v9 <= 9223"), "does not declare", "an undeclared variable");
    reject(K_VC, 0, sub(G, "goal 1*%v3 + -1*%v8", "goal -1*%v8 + 1*%v3"), "declaration order", "variables out of order");
    reject(K_VC, 0, sub(G, "goal 1*%v3 + -1*%v8", "goal 1*%v3 + 1*%v3"), "declaration order", "a variable twice in a sum");
    reject(K_VC, 0, sub(G, "goal 1*%v3 + -1*%v8", "goal 1*%v3 -1*%v8"), "joined by", "terms without ` + `");
    reject(K_VC, 0, sub(G, "goal 1*%v3 + -1*%v8", "goal 1*%v3 - 1*%v8"), "joined by", "terms joined by ` - `");
    reject(K_VC, 0, sub(G, "goal 1*%v3 + -1*%v8 <= -1", "goal %v3 <= -1"), "a term is", "a term without a coefficient");
    reject(K_VC, 0, sub(G, "var %v8", "var %v3"), "declared twice", "a variable declared twice");
    reject(K_VC, 0, sub(G, "var %v8", "var a|b"), "no `|`", "a `|` in a name");
    reject(K_VC, 0, sub(G, "var %v8", "var a\\b"), "no `|`", "a `\\` in a name");
    reject(K_VC, 0, sub(G, "  var %v8\n", "  var %v8 x\n"), "`var <name>`", "a var line of three tokens");
    reject(K_VC, 0, sub(G, "  hyp h2", "  var %v9\n  hyp h2"), "before the first hypothesis", "a variable after a hypothesis");
    // hypotheses
    reject(K_VC, 0, sub(G, "hyp h2", "hyp h3"), "in order", "a hypothesis out of order");
    reject(K_VC, 0, sub(G, "hyp h2", "hyp h02"), "hyp h<k>", "a hypothesis number with a leading zero");
    reject(K_VC, 0, sub(G, "hyp h1 type %v3 usize :", "hyp h1 :"), "has an origin", "a hypothesis without an origin");
    reject(K_VC, 0, sub(G, "usize : -1", "usize -1"), "has an origin", "a hypothesis without ` : `");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "-1*%v3 < 0"), "`<=` or `=`", "`<` in a hypothesis");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "-1*%v3 >= 0"), "`<=` or `=`", "`>=` in a hypothesis");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "-1*%v3 != 0"), "`<=` or `=`", "`!=` in a hypothesis");
    reject(K_VC, 0, sub(G, "-1*%v3 <= 0", "<= 0"), "a constraint is", "a constraint of two tokens");
    reject(K_VC, 0, sub(D, "or { 1*%v2 <= 0 | -1*%v2 <= -1 }", "or { 1*%v2 <= 0 }"), "two or more", "a disjunction of one");
    reject(K_VC, 0, sub(D, "or { 1*%v2 <= 0 | -1*%v2 <= -1 }", "or { | -1*%v2 <= -1 }"), "a constraint is", "an empty disjunct");
    reject(K_VC, 0, sub(D, "or { 1*%v2 <= 0 | -1*%v2 <= -1 }", "or { 1*%v2 <= 0 , | -1*%v2 <= -1 }"), "a constraint is", "an empty conjunct");
    reject(K_VC, 0, sub(D, "or { 1*%v2 <= 0 | -1*%v2 <= -1 }", "or { 1*%v2 <= 0 | -1*%v2 <= -1"), "a disjunction is", "a disjunction without `}`");
    reject(K_VC, 0, sub(D, "or { 1*%v2 <= 0 | -1*%v2 <= -1 }", "or 1*%v2 <= 0 | -1*%v2 <= -1 }"), "a disjunction is", "a disjunction without `{`");
    // goal and end
    reject(K_VC, 0, sub(G, "  goal 1*%v3 + -1*%v8 <= -1\n", ""), "has a goal", "no goal");
    reject(K_VC, 0, sub(G, "  goal", "  goal 0 <= 0\n  goal"), "the last line", "two goals");
    reject(K_VC, 0, sub(G, "goal 1*%v3 + -1*%v8 <= -1", "goal 1*%v3 + -1*%v8 = -1"), "`<=` or `!=`", "an equality goal");
    reject(K_VC, 0, sub(G, "end\n", ""), "ends with `end`", "no end");
    reject(K_VC, 0, sub(G, "end\n", "end x\n"), "alone", "end with a token");
    reject(K_VC, 0, sub(G, "  goal", "  assert 0 <= 0\n  goal"), "`var`, `hyp` or `goal`", "an unknown line");
    // certificates
    const char *C = CERT_GUARD, *B = CERT_BB;
    reject(K_CERT, G, sub(C, "  close f1\n", "  close f1\n  close f1\n"), "followed by `end`", "a step after the terminal step");
    reject(K_CERT, G, sub(C, "  close f1\n", ""), "steps end in", "a body without a terminal step");
    reject(K_CERT, G, "cert f 9c1e bb2.3 bounds.hi 0 v1\nend\n", "steps end in", "an empty body");
    reject(K_CERT, G, sub(C, " v1\n", "\n"), "a certificate starts", "a header without v1");
    reject(K_CERT, G, sub(C, " v1\n", " v2\n"), "a certificate starts", "a header with v2");
    reject(K_CERT, G, sub(C, "1*h3", "1*x3"), "a reference is", "a reference x3");
    reject(K_CERT, G, sub(C, "1*h3", "1*h03"), "a reference is", "a reference h03");
    reject(K_CERT, G, sub(C, "1*h3", "1*s1.0"), "a reference is", "a reference s1.0");
    reject(K_CERT, G, sub(C, "1*h3", "1*h1.2"), "a reference is", "a reference h1.2");
    reject(K_CERT, G, sub(C, "1*h3", "01*h3"), "canonical", "a multiplier 01");
    reject(K_CERT, G, sub(C, "1*h3 + 1*g", "1*h3 - 1*g"), "joined by", "a combination joined by `-`");
    reject(K_CERT, G, sub(C, "f1 = lin 1*h3 + 1*g", "f1 = lin"), "a combination is", "an empty combination");
    reject(K_CERT, G, sub(C, "f1 = lin", "f0 = lin"), "a step is", "a fact f0");
    reject(K_CERT, G, sub(C, "f1 = lin", "f1 = sum"), "`f<k> = lin", "an unknown derivation");
    reject(K_CERT, G, sub(C, "f1 = lin 1*h3 + 1*g", "f1 = rnd 2*f1"), "a cut is", "a cut with a multiplier");
    reject(K_CERT, G, sub(C, "close f1", "close f1 f2"), "a close is", "a close of two facts");
    reject(K_CERT, G, sub(C, "close f1", "close h1"), "a close is", "a close of a hypothesis");
    reject(K_CERT, VC_BB, sub(B, "  gt:\n    f2 = lin 1*h1 + 2*s1\n    close f2\n", ""), "`le:` and `gt:`", "a split without gt");
    reject(K_CERT, VC_BB, sub(sub(B, "le:", "LE:"), "gt:", "le:"), "`le:` and `gt:`", "a split's labels swapped");
    reject(K_CERT, VC_BB, sub(B, "    f1 = lin 1*h2 + 2*s1", "  f1 = lin 1*h2 + 2*s1"), "one level below its label", "a branch at its label's level");
    reject(K_CERT, VC_BB, sub(B, "  le:", "    le:"), "`le:` and `gt:`", "a label one level too deep");
    reject(K_CERT, VC_BB, sub(B, "split s1 : 1*x <= 0", "split s1 : 1*x = 0"), "the operator here is `<=`", "a split on an equality");
    reject(K_CERT, VC_BB, sub(B, "split s1 : 1*x <= 0", "split s1 : 1*z <= 0"), "does not declare", "a split over an undeclared variable");
    reject(K_CERT, VC_BB, sub(B, "split s1 :", "split s1"), "a split is", "a split without ` : `");
    reject(K_CERT, D, "cert g 77ab bb3.2 divzero 0 v1\n  case s1 : h1\nend\n", "followed by its branches", "a case without branches");
    reject(K_CERT, D, sub(CERT_DISJ, "  case s1 : h1", "  case s1 : h1 h2"), "a case is", "a case of two hypotheses");
    reject(K_CERT, D, sub(CERT_DISJ, "  2:\n    case", "  3:\n    case"), "followed by `end`", "a case's labels out of order");
    // models
    const char *M = MODEL_GUARD;
    reject(K_MODEL, G, sub(M, "  %v8 = 5\n", ""), "every variable", "a model missing a variable");
    reject(K_MODEL, G, sub(sub(M, "%v3 = 5", "%vX = 5"), "%v8 = 5", "%v3 = 5"), "every variable", "a model out of order");
    reject(K_MODEL, G, sub(M, "  %v8 = 5\n", "  %v8 = 5\n  %v9 = 1\n"), "ends with `end`", "a model with an extra variable");
    reject(K_MODEL, G, sub(M, "%v8 = 5", "%v8 = -0"), "canonical decimal", "a model value -0");
    reject(K_MODEL, G, sub(M, " v1\n", " v2\n"), "a model starts", "a model header with v2");
    reject(K_MODEL, G, sub(M, "%v8 = 5", "%v8 5"), "every variable", "a model line without `=`");
    CHECK(nrejected == ncalls, "%d of %d malformed texts refused for their expected reason", nrejected, ncalls);
    printf("  %d of %d malformed texts refused, each with its expected reason\n", nrejected, ncalls);
}

// ── 5: every mutant the parser accepts prints back exactly ────────────────────────────────────
static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint32_t next_rand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static void test_mutants(void) {
    printf("── one-edit mutants ──\n");
    const char *alpha = " \n0123456789-*+:=<>!.|{},hfsgxv%";
    struct { int kind; const char *vc, *text; } B[] = {
        { K_VC, 0, VC_GUARD }, { K_VC, 0, VC_PARITY }, { K_VC, 0, VC_BB }, { K_VC, 0, VC_DISJ },
        { K_CERT, VC_GUARD, CERT_GUARD }, { K_CERT, VC_PARITY, CERT_PARITY }, { K_CERT, VC_BB, CERT_BB },
        { K_CERT, VC_DISJ, CERT_DISJ }, { K_MODEL, VC_GUARD, MODEL_GUARD },
    };
    int accepted = 0, changed = 0, refused = 0, total = 0;
    for (int b = 0; b < 9; b++) {
        size_t len = strlen(B[b].text);
        for (int k = 0; k < 3000; k++) {
            char *t = malloc(len + 3);
            size_t at = next_rand() % len; uint32_t op = next_rand() % 4;
            char ch = alpha[next_rand() % strlen(alpha)];
            if (op == 0) { strcpy(t, B[b].text); t[at] = ch; }                                        // replace
            else if (op == 1) { memcpy(t, B[b].text, at); strcpy(t + at, B[b].text + at + 1); }      // delete
            else if (op == 2) { memcpy(t, B[b].text, at); t[at] = ch; strcpy(t + at + 1, B[b].text + at); }  // insert
            else { strcpy(t, B[b].text); if (at + 1 < len) { char c = t[at]; t[at] = t[at + 1]; t[at + 1] = c; } }  // swap
            JMem m = jmem_over(region, sizeof region);
            JVc v; JCert c; JModel mo; bool ok; char *back = 0;
            JParse ps;
            if (B[b].kind == K_VC) {
                ps = jparse_over(&m, t); ok = jvc_parse(&ps, &v) && *ps.p == 0;
                if (ok) back = print_vc(&v);
            } else {
                JParse pv = jparse_over(&m, B[b].vc); jvc_parse(&pv, &v);
                ps = jparse_over(&m, t);
                ok = (B[b].kind == K_CERT ? jcert_parse(&ps, &v, &c) : jmodel_parse(&ps, &v, &mo)) && *ps.p == 0;
                if (ok) back = B[b].kind == K_CERT ? print_cert(&c, &v) : print_model(&mo, &v);
            }
            total++;
            if (ok) {
                accepted++; changed += strcmp(t, B[b].text) != 0;
                CHECK(strcmp(back, t) == 0, "an accepted mutant prints back differently:\n%s---\n%s", t, back);
            } else {
                refused++;
                // a parse that stops early with no reason (trailing text after an item) is the
                // one refusal without `err`; anything else names its line
                CHECK(ps.err[0] == 0 ? *ps.p != 0 : strncmp(ps.err, "line ", 5) == 0, "a refusal without a reason");
            }
            free(back); free(t);
        }
    }
    CHECK(changed > 1000 && refused > 1000, "the mutants do not reach both sides: %d accepted changed texts, %d refused", changed, refused);
    printf("  %d mutants: %d accepted (%d of them changed texts), each printing back as it stands;\n"
           "  %d refused, each with a line and a reason\n", total, accepted, changed, refused);
}

// ── 6: a small region is a refusal, never an abort ────────────────────────────────────────────
static void test_memory(void) {
    printf("── memory ──\n");
    long first = -1; int oom = 0, bad = 0;
    // each size gets a buffer of exactly that size, so an allocation past its end is an ASan
    // error as well as a `cur` beyond `end`
    for (size_t size = 0; size <= 64 * 1024; size += 8) {
        char *buf = malloc(size ? size : 1);
        JMem m = jmem_over(buf, size);
        JVc v; JCert c;
        JParse ps = jparse_over(&m, VC_DISJ);
        bool ok = jvc_parse(&ps, &v);
        if (ok) { ps = jparse_over(&m, CERT_DISJ); ok = jcert_parse(&ps, &v, &c); }
        if (m.cur > m.end) bad++;
        free(buf);
        if (ok) { if (first < 0) first = (long)size; continue; }
        if (strstr(ps.err, "out of memory")) { oom++; if (first >= 0) bad++; }
        else bad++;
    }
    CHECK(first > 0 && oom > 0, "the first region that suffices is %ld bytes, after %d too small", first, oom);
    CHECK(bad == 0, "%d sizes allocated past their region, were refused for a reason other than memory, or were refused after a smaller size sufficed", bad);
    printf("  %d region sizes too small, each refused as out of memory; %ld bytes and up suffice\n", oom, first);
}

// ── 7: SMT-LIB ────────────────────────────────────────────────────────────────────────────────
static void test_smtlib(void) {
    printf("── SMT-LIB ──\n");
    JMem m = jmem_over(region, sizeof region);
    JVc v; JParse ps = jparse_over(&m, VC_GUARD);
    if (!jvc_parse(&ps, &v)) { CHECK(0, "VC refused"); return; }
    char *t = print_smt(&v);
    const char *want =
        "; vc f 9c1e bb2.3 bounds.hi 0\n"
        "(set-logic QF_LIA)\n"
        "(declare-fun |%v3| () Int)\n"
        "(declare-fun |%v8| () Int)\n"
        "(assert (<= (* (- 1) |%v3|) 0)) ; h1 type %v3 usize\n"
        "(assert (<= (* 1 |%v8|) 9223372036854775807)) ; h2 objsize %v8\n"
        "(assert (<= (+ (* 1 |%v3|) (* (- 1) |%v8|)) (- 1))) ; h3 guard bb1->bb2\n"
        "(assert (not (<= (+ (* 1 |%v3|) (* (- 1) |%v8|)) (- 1)))) ; g\n"
        "(check-sat)\n";
    CHECK(strcmp(t, want) == 0, "the guard VC's SMT-LIB is\n%s", t); free(t);
    ps = jparse_over(&m, VC_DISJ);
    if (!jvc_parse(&ps, &v)) { CHECK(0, "VC refused"); return; }
    t = print_smt(&v);
    const char *want2 =
        "; vc g 77ab bb3.2 divzero 0\n"
        "(set-logic QF_LIA)\n"
        "(declare-fun |%v1| () Int)\n"
        "(declare-fun |%v2| () Int)\n"
        "(declare-fun |atom.mul(%v1,%v2)| () Int)\n"
        "(assert (or (and (<= (* 1 |%v1|) (- 1)) (= (* 1 |%v2|) 4)) (and (= (* 1 |%v1|) 0)) (and (<= (* (- 1) |%v1|) (- 1))))) ; h1 path bb3\n"
        "(assert (or (and (<= (* 1 |%v2|) 0)) (and (<= (* (- 1) |%v2|) (- 1))))) ; h2 path bb2\n"
        "(assert (<= (+ (* 1 |%v1|) (* 1 |%v2|) (* (- 1) |atom.mul(%v1,%v2)|)) " MAX512 ")) ; h3 lemma L1 atom.mul(%v1,%v2)\n"
        "(assert (<= 0 0)) ; h4 const\n"
        "(assert (<= (* (- " MAX512 ") |atom.mul(%v1,%v2)|) (- " MAX512 "))) ; h5 type atom.mul(%v1,%v2)\n"
        "(assert (not (not (= (* 1 |%v2|) 0)))) ; g\n"
        "(check-sat)\n";
    CHECK(strcmp(t, want2) == 0, "the disjunctive VC's SMT-LIB is\n%s", t); free(t);
    printf("  two VCs print as the expected SMT-LIB\n");
}

int main(void) {
    test_round_trips();
    test_printer_alone();
    test_rejections();
    test_mutants();
    test_memory();
    test_smtlib();
    printf("%s\n", failures ? "test_judge_format: FAILED" : "test_judge_format: all passed");
    return failures ? 1 : 0;
}
