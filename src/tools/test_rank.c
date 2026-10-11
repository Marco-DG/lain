// src/tools/test_rank.c — the untrusted ranking-function synthesis (src/solver/rank.h, PE.5),
// linear and lexicographic, on hand-built loops, each answer checked from outside: a ranking function FOUND is evaluated by
// brute force over a box at every point the path allows (f(pre) >= 0 and f(pre) - f(post) >= 1),
// which shares nothing with the solver; a loop with NO linear ranking function must say so, with
// the solver's certificate of it.
//
//   gcc -std=c99 -O2 -o /tmp/test_rank src/tools/test_rank.c -I src && /tmp/test_rank
#include <stdio.h>
#include <stdlib.h>
#include "solver/rank.h"

static char region[1 << 24];
static int fails = 0;

static int64_t jv(const JInt *x) {
    uint64_t m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(int64_t)m : (int64_t)m;
}

// A path as rows of `nv` coefficients, an operator and a constant: sum a_v z_v (<= | =) b.
typedef struct { int nv; int n; int64_t a[8][8]; int eq[8]; int64_t b[8]; } Path;
static void row(Path *p, const int64_t *a, int eq, int64_t b) {
    for (int v = 0; v < p->nv; v++) p->a[p->n][v] = a[v];
    p->eq[p->n] = eq; p->b[p->n] = b; p->n++;
}
static JVc *to_vc(const Path *p, const char **names) {
    JVc *vc = calloc(1, sizeof *vc);
    vc->vars = names; vc->nvars = p->nv; vc->nhyps = p->n; vc->hyps = calloc((size_t)p->n, sizeof(JHyp));
    vc->id.fn = "t"; vc->id.irhash = "0"; vc->id.point = "bb0"; vc->id.kind = "rank"; vc->id.ordinal = "0";
    for (int r = 0; r < p->n; r++) {
        JCons *c = &vc->hyps[r].cons; c->t = calloc((size_t)p->nv, sizeof(JTerm));
        for (int v = 0; v < p->nv; v++) if (p->a[r][v]) { c->t[c->nt].var = v; ls_to_j(p->a[r][v], &c->t[c->nt].coef); c->nt++; }
        c->op = p->eq[r] ? J_EQ : J_LE; ls_to_j(p->b[r], &c->rhs);
        vc->hyps[r].origin = "path";
    }
    return vc;
}
static bool holds(const Path *p, const int64_t *z) {
    for (int r = 0; r < p->n; r++) {
        int64_t s = 0; for (int v = 0; v < p->nv; v++) s += p->a[r][v] * z[v];
        if (p->eq[r] ? s != p->b[r] : s > p->b[r]) return false;
    }
    return true;
}
// every point of [-B, B]^nv the path allows: f(pre) >= 0 and f(pre) - f(post) >= 1
static bool ranks(const Path *p, const LRank *R, const int *pre, const int *post, int np, int B, long *pts) {
    int64_t z[8]; int nv = p->nv;
    for (int v = 0; v < nv; v++) z[v] = -B;
    for (;;) {
        if (holds(p, z)) {
            int64_t fp = jv(&R->c0), fq = jv(&R->c0);
            for (int i = 0; i < np; i++) { fp += jv(&R->coef[i]) * z[pre[i]]; fq += jv(&R->coef[i]) * z[post[i]]; }
            if (fp < 0 || fp - fq < 1) return false;
            (*pts)++;
        }
        int v = 0;
        while (v < nv && ++z[v] > B) { z[v] = -B; v++; }
        if (v == nv) return true;
    }
}
static void expect(const char *name, Path *ps, int npaths, const char **names, const int *pre, const int *post, int np, bool want_found) {
    const JVc *vcs[4];
    for (int k = 0; k < npaths; k++) vcs[k] = to_vc(&ps[k], names);
    JWork lim = { 100000, 4096, 4096 };
    LRank R = lia_rank(vcs, npaths, pre, post, np, lim, region, sizeof region);
    bool ok;
    long pts = 0;
    if (want_found) {
        ok = R.k == LIA_RANK_FOUND;
        for (int k = 0; k < npaths && ok; k++) ok = ranks(&ps[k], &R, pre, post, np, 6, &pts);
    } else ok = R.k == LIA_RANK_NONE && R.none.body.n > 0;
    printf("  %-58s %s", name, ok ? "ok" : "WRONG");
    if (R.k == LIA_RANK_FOUND) {
        printf("  f =");
        for (int i = 0; i < np; i++) if (jv(&R.coef[i])) printf(" %+lld*%s", (long long)jv(&R.coef[i]), names[pre[i]]);
        printf(" %+lld  (checked at %ld points)", (long long)jv(&R.c0), pts);
    } else printf("  (%s)", R.k == LIA_RANK_NONE ? "no linear ranking function, certified" : "stopped or unsupported");
    printf("\n");
    if (!ok) fails++;
}

// Lexicographic: for each path k with component j = fall[k], at every point of the box it allows,
// every earlier component is non-negative and does not rise, and component j is non-negative and
// falls by at least 1.
static int64_t fval(const JInt *coef, const JInt *c0, const int *idx, int np, const int64_t *z) {
    int64_t f = jv(c0);
    for (int i = 0; i < np; i++) f += jv(&coef[i]) * z[idx[i]];
    return f;
}
static bool lex_ranks(const Path *p, const LRankLex *R, int k, const int *pre, const int *post, int np, int B, long *pts) {
    int64_t z[8]; int nv = p->nv, j = R->fall[k];
    if (j < 0) return false;
    for (int v = 0; v < nv; v++) z[v] = -B;
    for (;;) {
        if (holds(p, z)) {
            for (int l = 0; l <= j; l++) {
                int64_t a = fval(R->coef[l], &R->c0[l], pre, np, z), b = fval(R->coef[l], &R->c0[l], post, np, z);
                if (a < 0) return false;
                if (l < j ? b > a : a - b < 1) return false;
            }
            (*pts)++;
        }
        int v = 0;
        while (v < nv && ++z[v] > B) { z[v] = -B; v++; }
        if (v == nv) return true;
    }
}
static void expect_lex(const char *name, Path *ps, int npaths, const char **names, const int *pre, const int *post, int np,
                       int want_comps, int B) {
    const JVc *vcs[4];
    for (int k = 0; k < npaths; k++) vcs[k] = to_vc(&ps[k], names);
    JWork lim = { 200000, 4096, 4096 };
    LRankLex R = lia_rank_lex(vcs, npaths, pre, post, np, lim, region, sizeof region);
    long pts = 0;
    bool ok = want_comps == 0 ? R.k == LIA_RANK_NONE : (R.k == LIA_RANK_FOUND && R.ncomp == want_comps);
    for (int k = 0; k < npaths && ok && want_comps > 0; k++) ok = lex_ranks(&ps[k], &R, k, pre, post, np, B, &pts);
    printf("  %-58s %s", name, ok ? "ok" : "WRONG");
    if (R.k == LIA_RANK_FOUND) {
        printf("  (");
        for (int j = 0; j < R.ncomp; j++) {
            printf("%s", j ? ", " : "");
            for (int i = 0; i < np; i++) if (jv(&R.coef[j][i])) printf("%+lld*%s", (long long)jv(&R.coef[j][i]), names[pre[i]]);
            if (jv(&R.c0[j])) printf("%+lld", (long long)jv(&R.c0[j]));
        }
        printf(")  checked at %ld points", pts);
    } else printf("  (%s)", R.k == LIA_RANK_NONE ? "no lexicographic ranking" : "stopped or unsupported");
    printf("\n");
    if (!ok) fails++;
}

int main(void) {
    printf("── ranking functions (src/solver/rank.h) ──\n");
    {   // while i < n { i = i + 1 }:  z = (i, n, i')
        const char *nm[] = { "i", "n", "i'" }; Path p = { .nv = 3 };
        row(&p, (int64_t[]){ 1, -1, 0 }, 0, -1); row(&p, (int64_t[]){ -1, 0, 1 }, 1, 1);
        int pre[] = { 0, 1 }, post[] = { 2, 1 };
        expect("while i < n { i = i + 1 }", &p, 1, nm, pre, post, 2, true);
    }
    {   // while x > 0 { x = x - y }, y >= 1:  z = (x, y, x')
        const char *nm[] = { "x", "y", "x'" }; Path p = { .nv = 3 };
        row(&p, (int64_t[]){ -1, 0, 0 }, 0, -1); row(&p, (int64_t[]){ 0, -1, 0 }, 0, -1);
        row(&p, (int64_t[]){ -1, 1, 1 }, 1, 0);
        int pre[] = { 0, 1 }, post[] = { 2, 1 };
        expect("while x > 0 { x = x - y }, y >= 1", &p, 1, nm, pre, post, 2, true);
    }
    {   // while x > 0 { x = x + 1 }: none
        const char *nm[] = { "x", "x'" }; Path p = { .nv = 2 };
        row(&p, (int64_t[]){ -1, 0 }, 0, -1); row(&p, (int64_t[]){ -1, 1 }, 1, 1);
        int pre[] = { 0 }, post[] = { 1 };
        expect("while x > 0 { x = x + 1 } (does not terminate)", &p, 1, nm, pre, post, 1, false);
    }
    {   // while i < n { i += 1 or i += 2 }: two paths
        const char *nm[] = { "i", "n", "i'" }; Path p[2] = { { .nv = 3 }, { .nv = 3 } };
        row(&p[0], (int64_t[]){ 1, -1, 0 }, 0, -1); row(&p[0], (int64_t[]){ -1, 0, 1 }, 1, 1);
        row(&p[1], (int64_t[]){ 1, -1, 0 }, 0, -1); row(&p[1], (int64_t[]){ -1, 0, 1 }, 1, 2);
        int pre[] = { 0, 1 }, post[] = { 2, 1 };
        expect("while i < n { i += 1 | i += 2 } (two paths)", p, 2, nm, pre, post, 2, true);
    }
    {   // while i > 0 and j > 0 { i -= 1; j += 1 }:  z = (i, j, i', j')
        const char *nm[] = { "i", "j", "i'", "j'" }; Path p = { .nv = 4 };
        row(&p, (int64_t[]){ -1, 0, 0, 0 }, 0, -1); row(&p, (int64_t[]){ 0, -1, 0, 0 }, 0, -1);
        row(&p, (int64_t[]){ -1, 0, 1, 0 }, 1, -1); row(&p, (int64_t[]){ 0, -1, 0, 1 }, 1, 1);
        int pre[] = { 0, 1 }, post[] = { 2, 3 };
        expect("while i > 0 and j > 0 { i -= 1; j += 1 }", &p, 1, nm, pre, post, 2, true);
    }
    {   // while i > -5 { i = i - 1 }: f must be i + 4 or more, so the bound's constant is needed
        const char *nm[] = { "i", "i'" }; Path p = { .nv = 2 };
        row(&p, (int64_t[]){ -1, 0 }, 0, 4); row(&p, (int64_t[]){ -1, 1 }, 1, -1);
        int pre[] = { 0 }, post[] = { 1 };
        expect("while i > -5 { i = i - 1 } (needs the constant c0)", &p, 1, nm, pre, post, 1, true);
    }
    {   // while i > 0 { if j > 0 { j -= 1 } else { i -= 1; j = n } }, n >= 0: lexicographic only
        const char *nm[] = { "i", "j", "n", "i'", "j'" }; Path p[2] = { { .nv = 5 }, { .nv = 5 } };
        row(&p[0], (int64_t[]){ -1, 0, 0, 0, 0 }, 0, -1); row(&p[0], (int64_t[]){ 0, -1, 0, 0, 0 }, 0, -1);
        row(&p[0], (int64_t[]){ 0, -1, 0, 0, 1 }, 1, -1); row(&p[0], (int64_t[]){ -1, 0, 0, 1, 0 }, 1, 0);
        row(&p[0], (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0);
        row(&p[1], (int64_t[]){ -1, 0, 0, 0, 0 }, 0, -1); row(&p[1], (int64_t[]){ 0, 1, 0, 0, 0 }, 0, 0);
        row(&p[1], (int64_t[]){ -1, 0, 0, 1, 0 }, 1, -1); row(&p[1], (int64_t[]){ 0, 0, -1, 0, 1 }, 1, 0);
        row(&p[1], (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0);
        int pre[] = { 0, 1, 2 }, post[] = { 3, 4, 2 };
        expect("nested countdown (needs a lexicographic ranking)", p, 2, nm, pre, post, 3, false);
    }
    printf("── lexicographic ranking functions ──\n");
    {   // the nested countdown: (i, j)
        const char *nm[] = { "i", "j", "n", "i'", "j'" }; Path p[2] = { { .nv = 5 }, { .nv = 5 } };
        row(&p[0], (int64_t[]){ -1, 0, 0, 0, 0 }, 0, -1); row(&p[0], (int64_t[]){ 0, -1, 0, 0, 0 }, 0, -1);
        row(&p[0], (int64_t[]){ 0, -1, 0, 0, 1 }, 1, -1); row(&p[0], (int64_t[]){ -1, 0, 0, 1, 0 }, 1, 0);
        row(&p[0], (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0);
        row(&p[1], (int64_t[]){ -1, 0, 0, 0, 0 }, 0, -1); row(&p[1], (int64_t[]){ 0, 1, 0, 0, 0 }, 0, 0);
        row(&p[1], (int64_t[]){ -1, 0, 0, 1, 0 }, 1, -1); row(&p[1], (int64_t[]){ 0, 0, -1, 0, 1 }, 1, 0);
        row(&p[1], (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0);
        int pre[] = { 0, 1, 2 }, post[] = { 3, 4, 2 };
        expect_lex("nested countdown: two components", p, 2, nm, pre, post, 3, 2, 6);
    }
    {   // three levels: if j > 0 { if k > 0 { k -= 1 } else { j -= 1; k = n } } else { i -= 1; j = n }
        const char *nm[] = { "i", "j", "k", "n", "i'", "j'", "k'" }; Path p[3] = { { .nv = 7 }, { .nv = 7 }, { .nv = 7 } };
        // P1: i >= 1, j >= 1, k >= 1, k' = k - 1, j' = j, i' = i, n >= 0
        row(&p[0], (int64_t[]){ -1, 0, 0, 0, 0, 0, 0 }, 0, -1); row(&p[0], (int64_t[]){ 0, -1, 0, 0, 0, 0, 0 }, 0, -1);
        row(&p[0], (int64_t[]){ 0, 0, -1, 0, 0, 0, 0 }, 0, -1); row(&p[0], (int64_t[]){ 0, 0, -1, 0, 0, 0, 1 }, 1, -1);
        row(&p[0], (int64_t[]){ 0, -1, 0, 0, 0, 1, 0 }, 1, 0);  row(&p[0], (int64_t[]){ -1, 0, 0, 0, 1, 0, 0 }, 1, 0);
        row(&p[0], (int64_t[]){ 0, 0, 0, -1, 0, 0, 0 }, 0, 0);
        // P2: i >= 1, j >= 1, k <= 0, j' = j - 1, k' = n, i' = i, n >= 0
        row(&p[1], (int64_t[]){ -1, 0, 0, 0, 0, 0, 0 }, 0, -1); row(&p[1], (int64_t[]){ 0, -1, 0, 0, 0, 0, 0 }, 0, -1);
        row(&p[1], (int64_t[]){ 0, 0, 1, 0, 0, 0, 0 }, 0, 0);   row(&p[1], (int64_t[]){ 0, -1, 0, 0, 0, 1, 0 }, 1, -1);
        row(&p[1], (int64_t[]){ 0, 0, 0, -1, 0, 0, 1 }, 1, 0);  row(&p[1], (int64_t[]){ -1, 0, 0, 0, 1, 0, 0 }, 1, 0);
        row(&p[1], (int64_t[]){ 0, 0, 0, -1, 0, 0, 0 }, 0, 0);
        // P3: i >= 1, j <= 0, i' = i - 1, j' = n, k' = k, n >= 0
        row(&p[2], (int64_t[]){ -1, 0, 0, 0, 0, 0, 0 }, 0, -1); row(&p[2], (int64_t[]){ 0, 1, 0, 0, 0, 0, 0 }, 0, 0);
        row(&p[2], (int64_t[]){ -1, 0, 0, 0, 1, 0, 0 }, 1, -1); row(&p[2], (int64_t[]){ 0, 0, 0, -1, 0, 1, 0 }, 1, 0);
        row(&p[2], (int64_t[]){ 0, 0, -1, 0, 0, 0, 1 }, 1, 0);  row(&p[2], (int64_t[]){ 0, 0, 0, -1, 0, 0, 0 }, 0, 0);
        int pre[] = { 0, 1, 2, 3 }, post[] = { 4, 5, 6, 3 };
        expect_lex("three-level countdown: three components", p, 3, nm, pre, post, 4, 3, 3);
    }
    {   // a loop a linear function ranks: one component
        const char *nm[] = { "i", "n", "i'" }; Path p = { .nv = 3 };
        row(&p, (int64_t[]){ 1, -1, 0 }, 0, -1); row(&p, (int64_t[]){ -1, 0, 1 }, 1, 1);
        int pre[] = { 0, 1 }, post[] = { 2, 1 };
        expect_lex("while i < n { i = i + 1 }: one component", &p, 1, nm, pre, post, 2, 1, 6);
    }
    {   // does not terminate: none
        const char *nm[] = { "x", "x'" }; Path p = { .nv = 2 };
        row(&p, (int64_t[]){ -1, 0 }, 0, -1); row(&p, (int64_t[]){ -1, 1 }, 1, 1);
        int pre[] = { 0 }, post[] = { 1 };
        expect_lex("while x > 0 { x = x + 1 }: none", &p, 1, nm, pre, post, 1, 0, 6);
    }
    printf(fails ? "rank: %d WRONG\n" : "rank: all passed\n", fails);
    return fails ? 1 : 0;
}
