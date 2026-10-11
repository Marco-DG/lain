// src/tools/test_judge_kernel.c — the judge's KERNEL (src/judge/kernel.h) against good and bad
// certificates: the bad-certificate suite. The kernel is the trusted base, so each rule it checks
// is pinned twice, by a certificate it must accept and by one that breaks the rule, which it must
// refuse WITH THAT RULE'S REASON (a refusal for another reason would pass while the rule is gone):
//   1. accepted: judge_interfaces.md's examples (a guard, parity by two cuts, branch and bound), a
//      cut whose constant is negative and odd (floored toward minus infinity; a kernel that
//      truncates toward 0 refuses it), a case, a `!=` goal (its negation is an equality, used with
//      either sign), facts made before a split used in both branches, 512-bit numbers that fit;
//   2. refused, each for its own reason: a close moved into the sibling branch, a number used in
//      both branches, a fact used before it is made, a split fact used in the sibling of its split, a cut of a
//      fact with no variable, a cut by 1, a negative multiplier on an inequality, a sum out of
//      order or with a zero coefficient (built by hand: the parser refuses their text), a number
//      past 512 bits, a close of a fact with a variable or a constant not negative, a disjunction
//      outside a case, a case on a plain hypothesis or with too few branches, a case fact its
//      disjunct lacks, a hypothesis the VC lacks, a step after a terminal step and a branch with
//      no terminal step (by hand), an equality goal (by hand);
//   3. memory: a region too small at every size up to the one that suffices refuses with "out of
//      memory", never an abort.
//
// Build & run (run_ir_tests.sh does this):
//   gcc -std=c99 -o /tmp/test_judge_kernel src/tools/test_judge_kernel.c -I src && /tmp/test_judge_kernel
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "judge/format.h"
#include "judge/mem.h"
#include "judge/text.h"
#include "judge/kernel.h"

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char region[1 << 22];

typedef void (*Mutate)(JVc *v, JCert *c);
// Parse `vc` and `cert`, apply `mut` to the structures when given, and run the kernel. `want` NULL:
// it must accept; else it must refuse with a reason containing `want`.
static void run(const char *name, const char *vc, const char *cert, Mutate mut, const char *want) {
    JMem m = jmem_over(region, sizeof region);
    JParse ps = jparse_over(&m, vc); JVc v;
    if (!jvc_parse(&ps, &v)) { CHECK(0, "%s: the VC does not parse: %s", name, ps.err); return; }
    JParse pc = jparse_over(&m, cert); JCert c;
    if (!jcert_parse(&pc, &v, &c)) { CHECK(0, "%s: the certificate does not parse: %s", name, pc.err); return; }
    if (mut) mut(&v, &c);
    char why[256] = "";
    bool ok = jkernel_check(&m, &v, &c, why, sizeof why);
    if (!want) CHECK(ok, "%s: refused (%s), must be accepted", name, why);
    else CHECK(!ok && strstr(why, want), "%s: %s, must be refused for \"%s\"", name, ok ? "accepted" : why, want);
}

static const char *GUARD =
    "vc f 9c1e bb2.3 bounds.hi 0\n  var %v3\n  var %v8\n  hyp h1 type %v3 usize : -1*%v3 <= 0\n"
    "  hyp h2 objsize %v8 : 1*%v8 <= 9223372036854775807\n  hyp h3 guard bb1->bb2 : 1*%v3 + -1*%v8 <= -1\n"
    "  goal 1*%v3 + -1*%v8 <= -1\nend\n";
static const char *PARITY = "vc parity 0 bb0.1 assume 0\n  var x\n  var y\n  hyp h1 def %v1 add : 2*x + -2*y = 1\n  goal 0 <= -1\nend\n";
static const char *BB = "vc bb 0 bb0.1 assume 0\n  var x\n  hyp h1 def %v1 : 2*x <= 1\n  hyp h2 def %v2 : -2*x <= -1\n  goal 0 <= -1\nend\n";
static const char *FLOOR = "vc fl 0 bb0.1 assume 0\n  var x\n  hyp h1 a : 2*x <= -1\n  hyp h2 b : -1*x <= 0\n  goal 0 <= -1\nend\n";
static const char *CASE =
    "vc cs 0 bb0.1 assume 0\n  var x\n  hyp h1 j : or { 1*x <= 0 | -1*x <= -5 }\n  hyp h2 k : -1*x <= -1\n"
    "  hyp h3 l : 1*x <= 3\n  goal 0 <= -1\nend\n";
static const char *CASE3 =
    "vc c3 0 bb0.1 assume 0\n  var x\n  hyp h1 j : or { 1*x <= 0 | -1*x <= -5 | 1*x <= 1 , -1*x <= -1 }\n  hyp h2 k : -1*x <= -2\n"
    "  hyp h3 l : 1*x <= 3\n  goal 0 <= -1\nend\n";
static const char *NE = "vc ne 0 bb0.1 divzero 0\n  var x\n  hyp h1 a : 1*x <= 2\n  goal 1*x != 3\nend\n";
static const char *BIG =
    "vc big 0 bb0.1 assume 0\n  var x\n"
    "  hyp h1 a : 1606938044258990275541962092341162602522202993782792835301376*x <= -1\n"
    "  hyp h2 b : -1*x <= 0\n  goal 0 <= -1\nend\n";

// by hand: what the parser cannot express
static void swap_split_terms(JVc *v, JCert *c) { (void)v; JCons *s = &c->body.s[0].split; JTerm t = s->t[0]; s->t[0] = s->t[1]; s->t[1] = t; }
static void zero_split_coef(JVc *v, JCert *c) { (void)v; memset(&c->body.s[0].split.t[1].coef, 0, sizeof(JInt)); }
static void close_not_last(JVc *v, JCert *c) { (void)v; c->body.n = 1; c->body.s[0].k = J_CLOSE; c->body.s[0].f = 1; c->body.n = 2; }
static void no_terminal(JVc *v, JCert *c) { (void)v; c->body.n = 1; }
static void eq_goal(JVc *v, JCert *c) { (void)c; v->goal.op = J_EQ; }

int main(void) {
    printf("── accepted ──\n");
    run("a guard", GUARD, "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h3 + 1*g\n  close f1\nend\n", NULL, NULL);
    run("parity, by two cuts", PARITY,
        "cert parity 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1\n  f2 = rnd f1\n  f3 = lin -1*h1\n  f4 = rnd f3\n  f5 = lin 1*f2 + 1*f4\n  close f5\nend\n", NULL, NULL);
    run("branch and bound", BB,
        "cert bb 0 bb0.1 assume 0 v1\n  split s1 : 1*x <= 0\n  le:\n    f1 = lin 1*h2 + 2*s1\n    close f1\n  gt:\n    f2 = lin 1*h1 + 2*s1\n    close f2\nend\n", NULL, NULL);
    run("a cut floored toward minus infinity", FLOOR,
        "cert fl 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1\n  f2 = rnd f1\n  f3 = lin 1*f2 + 1*h2\n  close f3\nend\n", NULL, NULL);
    run("a case", CASE,
        "cert cs 0 bb0.1 assume 0 v1\n  case s1 : h1\n  1:\n    f1 = lin 1*s1.1 + 1*h2\n    close f1\n  2:\n    f2 = lin 1*s1.1 + 1*h3\n    close f2\nend\n", NULL, NULL);
    run("a case of three, one disjunct of two constraints", CASE3,
        "cert c3 0 bb0.1 assume 0 v1\n  case s1 : h1\n  1:\n    f1 = lin 1*s1.1 + 1*h2\n    close f1\n  2:\n    f2 = lin 1*s1.1 + 1*h3\n    close f2\n"
        "  3:\n    f3 = lin 1*s1.1 + 1*h2\n    close f3\nend\n", NULL, NULL);
    run("a `!=` goal, its negation used with a negative multiplier", NE,
        "cert ne 0 bb0.1 divzero 0 v1\n  f1 = lin 1*h1 + -1*g\n  close f1\nend\n", NULL, NULL);
    run("a fact made before a split, used in both branches", BB,
        "cert bb 0 bb0.1 assume 0 v1\n  f9 = lin 1*h2\n  split s1 : 1*x <= 0\n  le:\n    f1 = lin 1*f9 + 2*s1\n    close f1\n  gt:\n    f2 = lin 1*h1 + 2*s1\n    close f2\nend\n", NULL, NULL);
    run("200-bit coefficients that fit", BIG,
        "cert big 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1\n  f2 = rnd f1\n  f3 = lin 1*f2 + 1*h2\n  close f3\nend\n", NULL, NULL);

    printf("── refused, each for its reason ──\n");
    run("a close moved into the sibling branch", BB,
        "cert bb 0 bb0.1 assume 0 v1\n  split s1 : 1*x <= 0\n  le:\n    f1 = lin 1*h2 + 2*s1\n    close f1\n  gt:\n    f2 = lin 1*h1 + 2*s1\n    close f1\nend\n", NULL, "not in scope");
    run("a number used in both branches", BB,
        "cert bb 0 bb0.1 assume 0 v1\n  split s1 : 1*x <= 0\n  le:\n    f1 = lin 1*h2 + 2*s1\n    close f1\n  gt:\n    f1 = lin 1*h1 + 2*s1\n    close f1\nend\n", NULL, "used twice");
    run("a fact used before it is made", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*f2\n  f2 = lin 1*h3 + 1*g\n  close f2\nend\n", NULL, "not in scope");
    run("a split fact used in the sibling of a nested split", BB,
        "cert bb 0 bb0.1 assume 0 v1\n  split s1 : 1*x <= 0\n  le:\n    split s2 : 1*x <= -5\n    le:\n      f1 = lin 1*h2 + 2*s1\n      close f1\n    gt:\n      f2 = lin 1*h2 + 2*s1\n      close f2\n"
        "  gt:\n    f3 = lin 1*h1 + 2*s2\n    close f3\nend\n", NULL, "not in scope");
    run("a cut of a fact with no variable", BB,
        "cert bb 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1 + 1*h2\n  f2 = rnd f1\n  close f2\nend\n", NULL, "no variable");
    run("a cut by 1", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h3\n  f2 = rnd f1\n  f3 = lin 1*f2 + 1*g\n  close f3\nend\n", NULL, "cut by 1");
    run("a negative multiplier on an inequality", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin -1*h3 + 1*g\n  close f1\nend\n", NULL, "negative multiplier");
    run("a sum out of order (by hand)", PARITY,
        "cert parity 0 bb0.1 assume 0 v1\n  split s1 : 1*x + 1*y <= 0\n  le:\n    f1 = lin 1*h1\n    close f1\n  gt:\n    f2 = lin 1*h1\n    close f2\nend\n", swap_split_terms, "not strictly ascending");
    run("a zero coefficient (by hand)", PARITY,
        "cert parity 0 bb0.1 assume 0 v1\n  split s1 : 1*x + 1*y <= 0\n  le:\n    f1 = lin 1*h1\n    close f1\n  gt:\n    f2 = lin 1*h1\n    close f2\nend\n", zero_split_coef, "zero");
    run("a number past 512 bits", BIG,
        "cert big 0 bb0.1 assume 0 v1\n  f1 = lin 1606938044258990275541962092341162602522202993782792835301376*h1 + 1*h2\n  f2 = lin 1606938044258990275541962092341162602522202993782792835301376*f1 + 1*h2\n"
        "  f3 = lin 1606938044258990275541962092341162602522202993782792835301376*f2 + 1*h2\n  close f3\nend\n", NULL, "beyond 512 bits");
    {   // a product that passes the limb count's first test and still carries past 512 bits, and a
        // sum past 512 bits: overflow refuses, it never wraps
        static char vcb[1024], ceb[1024];
        snprintf(vcb, sizeof vcb, "vc ov 0 bb0.1 assume 0\n  var x\n  hyp h1 a : 497323236409786642155382248146820840100456150797347717440463976893159497012533375533055*x <= 0\n  hyp h2 b : 13407807929942597099574024998205846127479365820592393377723561443721764030073546976801874298166903427690031858186486050853753882811946569946433649006084095*x <= 0\n  goal 0 <= -1\nend\n");
        snprintf(ceb, sizeof ceb, "cert ov 0 bb0.1 assume 0 v1\n  f1 = lin 115792089237316195423570985008687907853269984665640564039457584007913129639935*h1\n  close f1\nend\n");
        run("a product carried past 512 bits", vcb, ceb, NULL, "beyond 512 bits");
        snprintf(ceb, sizeof ceb, "cert ov 0 bb0.1 assume 0 v1\n  f1 = lin 1*h2 + 1*h2\n  close f1\nend\n");
        run("a sum past 512 bits", vcb, ceb, NULL, "beyond 512 bits");
    }
    run("a close of a fact with a variable", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h1 + 1*g\n  close f1\nend\n", NULL, "still has a variable");
    run("a close of a constant not negative", PARITY,
        "cert parity 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1\n  f3 = lin -1*h1\n  f5 = lin 1*f1 + 1*f3\n  close f5\nend\n", NULL, "not negative");
    run("a disjunction outside a case", CASE,
        "cert cs 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1 + 1*h2\n  close f1\nend\n", NULL, "outside a `case`");
    run("a case on a plain hypothesis", CASE,
        "cert cs 0 bb0.1 assume 0 v1\n  case s1 : h2\n  1:\n    f1 = lin 1*s1.1 + 1*h3\n    close f1\nend\n", NULL, "not a disjunction");
    run("a case with too few branches", CASE3,
        "cert c3 0 bb0.1 assume 0 v1\n  case s1 : h1\n  1:\n    f1 = lin 1*s1.1 + 1*h2\n    close f1\n  2:\n    f2 = lin 1*s1.1 + 1*h3\n    close f2\nend\n", NULL, "one per disjunct");
    run("a case fact its disjunct lacks", CASE,
        "cert cs 0 bb0.1 assume 0 v1\n  case s1 : h1\n  1:\n    f1 = lin 1*s1.2 + 1*h2\n    close f1\n  2:\n    f2 = lin 1*s1.1 + 1*h3\n    close f2\nend\n", NULL, "its disjunct does not have");
    run("a hypothesis the VC lacks", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h4 + 1*g\n  close f1\nend\n", NULL, "does not have");
    run("a step after a terminal step (by hand)", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h3 + 1*g\n  close f1\nend\n", close_not_last, "after a terminal step");
    run("a branch with no terminal step (by hand)", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h3 + 1*g\n  close f1\nend\n", no_terminal, "does not end");
    run("an equality goal (by hand)", GUARD,
        "cert f 9c1e bb2.3 bounds.hi 0 v1\n  f1 = lin 1*h3 + 1*g\n  close f1\nend\n", eq_goal, "equality");

    printf("── memory ──\n");
    {   // the parity certificate in regions of every size up to one that suffices
        static char parsed[1 << 16];
        JMem pm = jmem_over(parsed, sizeof parsed);
        JParse ps = jparse_over(&pm, PARITY); JVc v; bool vok = jvc_parse(&ps, &v);
        JParse pc = jparse_over(&pm, "cert parity 0 bb0.1 assume 0 v1\n  f1 = lin 1*h1\n  f2 = rnd f1\n  f3 = lin -1*h1\n  f4 = rnd f3\n  f5 = lin 1*f2 + 1*f4\n  close f5\nend\n");
        JCert c; bool cok = vok && jcert_parse(&pc, &v, &c);
        CHECK(cok, "the parity certificate parses");
        size_t enough = 0; int oomok = 1;
        for (size_t n = 0; cok && n <= sizeof region; n += 16) {
            JMem m = jmem_over(region, n); char why[256] = "";
            if (jkernel_check(&m, &v, &c, why, sizeof why)) { enough = n; break; }
            if (!strstr(why, "out of memory")) { oomok = 0; CHECK(0, "a region of %zu bytes refuses for \"%s\", not memory", n, why); break; }
        }
        CHECK(enough > 0 && oomok, "a region too small refuses with \"out of memory\" until one suffices (%zu)", enough);
        for (size_t n = enough; enough && n < enough + 256; n += 16) {
            JMem m = jmem_over(region, n); char why[256];
            CHECK(jkernel_check(&m, &v, &c, why, sizeof why), "a larger region (%zu) still suffices", n);
        }
    }
    printf("%s\n", failures ? "test_judge_kernel: FAILED" : "test_judge_kernel: all passed");
    return failures ? 1 : 0;
}
