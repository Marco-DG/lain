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
// want: LIA_RANK_FOUND (checked by brute force), _NONE (with its certificate) or _STOP; by: the
// search that must answer (0 the Farkas LP, 1 the samples), -1 for a STOP. `size` is the region.
static void expect_in(const char *name, Path *ps, int npaths, const char **names, const int *pre, const int *post, int np,
                      LRankKind want, int by, size_t size) {
    const JVc *vcs[4];
    for (int k = 0; k < npaths; k++) vcs[k] = to_vc(&ps[k], names);
    JWork lim = { 100000, 4096, 4096 };
    LRank R = lia_rank(vcs, npaths, pre, post, np, lim, region, size);
    bool ok = R.k == want && (by < 0 || R.by == by);
    long pts = 0;
    if (ok && want == LIA_RANK_FOUND)                  // the layout the finder reads: coef[np] is c0 again
        ok = jv(&R.coef[np]) == jv(&R.c0) && R.coef[np].n == R.c0.n;
    if (ok && want == LIA_RANK_FOUND)
        for (int k = 0; k < npaths && ok; k++) ok = ranks(&ps[k], &R, pre, post, np, 6, &pts);
    if (ok && want == LIA_RANK_NONE) ok = R.none.body.n > 0;
    printf("  %-58s %s", name, ok ? "ok" : "WRONG");
    if (R.k == LIA_RANK_FOUND) {
        printf("  f =");
        for (int i = 0; i < np; i++) if (jv(&R.coef[i])) printf(" %+lld*%s", (long long)jv(&R.coef[i]), names[pre[i]]);
        printf(" %+lld  (checked at %ld points)", (long long)jv(&R.c0), pts);
    } else printf("  (%s)", R.k == LIA_RANK_NONE ? "no linear ranking function, certified" : "stopped or unsupported");
    if (R.k == LIA_RANK_FOUND || R.k == LIA_RANK_NONE) printf(" [%s]", R.by ? "by samples" : "by the Farkas LP");
    printf("\n");
    if (!ok) fails++;
}
static void expect(const char *name, Path *ps, int npaths, const char **names, const int *pre, const int *post, int np, bool want_found) {
    expect_in(name, ps, npaths, names, pre, post, np, want_found ? LIA_RANK_FOUND : LIA_RANK_NONE, 0, sizeof region);
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

// PE.7: every relation lia_relations proposes must not rise on any path, at every point of the
// box the path allows; and the one the loop is written for must be among them.
static bool rel_holds(const Path *ps, int npaths, const LRel *r, const int *pre, const int *post, int B, long *pts) {
    for (int k = 0; k < npaths; k++) {
        const Path *p = &ps[k]; int64_t z[8]; int nv = p->nv;
        for (int v = 0; v < nv; v++) z[v] = -B;
        for (;;) {
            if (holds(p, z)) {
                int64_t d = r->ct * (z[post[r->t]] - z[pre[r->t]]) + r->cj * (z[post[r->j]] - z[pre[r->j]]);
                if (d > 0) return false;
                (*pts)++;
            }
            int v = 0;
            while (v < nv && ++z[v] > B) { z[v] = -B; v++; }
            if (v == nv) break;
        }
    }
    return true;
}
static void expect_rel(const char *name, Path *ps, int npaths, const char **names, const int *pre, const int *post, int np,
                       int want_t, int want_j, int64_t want_ct, int64_t want_cj) {
    const JVc *vcs[4];
    for (int k = 0; k < npaths; k++) vcs[k] = to_vc(&ps[k], names);
    JWork lim = { 100000, 4096, 4096 };
    LRel out[16];
    int32_t n = lia_relations(vcs, npaths, pre, post, np, lim, region, sizeof region, out, 16);
    bool ok = true, found = want_t < 0; long pts = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!rel_holds(ps, npaths, &out[i], pre, post, 9, &pts)) ok = false;
        if (out[i].t == want_t && out[i].j == want_j && out[i].ct == want_ct && out[i].cj == want_cj) found = true;
    }
    for (int32_t i = 0; want_t < 0 && i < n; i++) if (out[i].j == want_j) ok = false;   // never as the counter
    printf("  %-58s %s ", name, ok && found ? "ok" : "WRONG");
    for (int32_t i = 0; i < n; i++)
        printf(" [%+lld*%s %+lld*%s]", (long long)out[i].ct, names[pre[out[i].t]], (long long)out[i].cj, names[pre[out[i].j]]);
    printf("  (checked at %ld points)\n", pts);
    if (!(ok && found)) fails++;
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
    // ── by counterexamples (rk_cegis, s2): the same loops in a region too small for the Farkas
    // tableau, which grows with every path's facts together (four paths of eight rows: about 400
    // KB), while each path's own solve needs a few KB. SMALL is that region.
    const size_t SMALL = 1 << 17;
    {   // Gothenburg_v2-1's shape: while x >= 0 or y >= 0 { x = x + a - b - 1; y = y + b - a - 1 },
        // a - b = 1 kept, x < 0 a header fact: x stays, y falls by 2 (or 3); the x >= 0 paths are empty.
        // z = (x, y, x', y')
        const char *nm[] = { "x", "y", "x'", "y'" }; Path p[4] = { { .nv = 4 }, { .nv = 4 }, { .nv = 4 }, { .nv = 4 } };
        for (int k = 0; k < 4; k++) {
            if (k < 2) row(&p[k], (int64_t[]){ 0, -1, 0, 0 }, 0, 0);                  // y >= 0
            else { row(&p[k], (int64_t[]){ -1, 0, 0, 0 }, 0, 0); row(&p[k], (int64_t[]){ 0, 1, 0, 0 }, 0, -1); }   // x >= 0, y < 0
            row(&p[k], (int64_t[]){ 1, 0, 0, 0 }, 0, -1);                             // x <= -1, the header's
            row(&p[k], (int64_t[]){ -1, 0, 1, 0 }, 1, 0);                             // x' = x
            row(&p[k], (int64_t[]){ 0, -1, 0, 1 }, 1, k % 2 ? -3 : -2);               // y' = y - 2 | y - 3
            row(&p[k], (int64_t[]){ -1, 0, 0, 0 }, 0, 1000); row(&p[k], (int64_t[]){ 0, 1, 0, 0 }, 0, 1000);
            row(&p[k], (int64_t[]){ 0, -1, 0, 0 }, 0, 1000);                          // boxes, as types give
        }
        int pre[] = { 0, 1 }, post[] = { 2, 3 };
        expect_in("Gothenburg_v2-1's shape, by the Farkas LP", p, 4, nm, pre, post, 2, LIA_RANK_FOUND, 0, sizeof region);
        expect_in("Gothenburg_v2-1's shape, too big for it: by samples", p, 4, nm, pre, post, 2, LIA_RANK_FOUND, 1, SMALL);
    }
    {   // x rises on two paths and falls on two, inside [0, 10]: no linear ranking, and the samples
        // show it (a fall of +1 needs c <= -1, of -1 needs c >= 1). z = (x, x')
        const char *nm[] = { "x", "x'" }; Path p[4] = { { .nv = 2 }, { .nv = 2 }, { .nv = 2 }, { .nv = 2 } };
        for (int k = 0; k < 4; k++) {
            row(&p[k], (int64_t[]){ -1, 0 }, 0, 0); row(&p[k], (int64_t[]){ 1, 0 }, 0, 10);
            row(&p[k], (int64_t[]){ -1, 1 }, 1, k % 2 ? -1 : 1);
            for (int b = 1; b <= 5; b++) row(&p[k], (int64_t[]){ 0, 1 }, 0, 10 + b);  // the post's box, five times
        }
        int pre[] = { 0 }, post[] = { 1 };
        expect_in("x += 1 | x -= 1 in [0, 10], by the Farkas LP: none", p, 4, nm, pre, post, 1, LIA_RANK_NONE, 0, sizeof region);
        expect_in("x += 1 | x -= 1 in [0, 10], by samples: none", p, 4, nm, pre, post, 1, LIA_RANK_NONE, 1, SMALL);
    }
    {   // while i > -5 { i = i - 1 }: f = i + 4, and only a BOUND sample (i = -4 under f = i) makes
        // the search raise c0
        const char *nm[] = { "i", "i'" }; Path p[4] = { { .nv = 2 }, { .nv = 2 }, { .nv = 2 }, { .nv = 2 } };
        for (int k = 0; k < 4; k++) {
            row(&p[k], (int64_t[]){ -1, 0 }, 0, 4); row(&p[k], (int64_t[]){ -1, 1 }, 1, -1);
            for (int b = 1; b <= 6; b++) row(&p[k], (int64_t[]){ 1, 0 }, 0, 100 * b);  // i < 100*b, redundant
        }
        int pre[] = { 0 }, post[] = { 1 };
        expect_in("while i > -5 { i = i - 1 }, by samples (needs c0)", p, 4, nm, pre, post, 1, LIA_RANK_FOUND, 1, SMALL);
    }
    {   // while x > 0 { x = x + 1 }, unbounded: the Farkas LP says none; the samples only raise c0,
        // round after round, and the search must STOP, never answer a candidate
        const char *nm[] = { "x", "x'" }; Path p[4] = { { .nv = 2 }, { .nv = 2 }, { .nv = 2 }, { .nv = 2 } };
        for (int k = 0; k < 4; k++) {
            row(&p[k], (int64_t[]){ -1, 0 }, 0, -1); row(&p[k], (int64_t[]){ -1, 1 }, 1, 1);
            for (int b = 1; b <= 6; b++) row(&p[k], (int64_t[]){ -1, 0 }, 0, b);    // x > -b, redundant
        }
        int pre[] = { 0 }, post[] = { 1 };
        expect_in("x = x + 1 under x > 0, by the Farkas LP: none", p, 4, nm, pre, post, 1, LIA_RANK_NONE, 0, sizeof region);
        expect_in("x = x + 1 under x > 0, by samples: STOP", p, 4, nm, pre, post, 1, LIA_RANK_STOP, -1, SMALL);
    }
    printf("── relations a loop keeps (PE.7) ──\n");
    {   // for i < n { s = s + e }, e in [1, 3]:  z = (s, i, n, e, s', i')
        const char *nm[] = { "s", "i", "n", "e", "s'", "i'" }; Path p = { .nv = 6 };
        row(&p, (int64_t[]){ 0, 1, -1, 0, 0, 0 }, 0, -1);
        row(&p, (int64_t[]){ 0, 0, 0, -1, 0, 0 }, 0, -1); row(&p, (int64_t[]){ 0, 0, 0, 1, 0, 0 }, 0, 3);
        row(&p, (int64_t[]){ -1, 0, 0, -1, 1, 0 }, 1, 0); row(&p, (int64_t[]){ 0, -1, 0, 0, 0, 1 }, 1, 1);
        int pre[] = { 0, 1, 2 }, post[] = { 4, 5, 2 };
        expect_rel("running total s += e, e in [1,3], i += 1: s - 3*i", &p, 1, nm, pre, post, 3, 0, 1, 1, -3);
    }
    {   // the Zune loop, scaled: days falls by 4 (leap) or 3 while year rises by 1:  z = (days, year, days', year')
        const char *nm[] = { "days", "year", "days'", "year'" }; Path p[2] = { { .nv = 4 }, { .nv = 4 } };
        row(&p[0], (int64_t[]){ -1, 0, 0, 0 }, 0, -5); row(&p[0], (int64_t[]){ -1, 0, 1, 0 }, 1, -4); row(&p[0], (int64_t[]){ 0, -1, 0, 1 }, 1, 1);
        row(&p[1], (int64_t[]){ -1, 0, 0, 0 }, 0, -4); row(&p[1], (int64_t[]){ -1, 0, 1, 0 }, 1, -3); row(&p[1], (int64_t[]){ 0, -1, 0, 1 }, 1, 1);
        int pre[] = { 0, 1 }, post[] = { 2, 3 };
        expect_rel("Zune, scaled: days -= 4 | 3, year += 1: 3*year + days", p, 2, nm, pre, post, 2, 1, 0, 3, 1);
    }
    {   // a counter that rises on one path and falls on the other: no relation with it
        const char *nm[] = { "s", "j", "s'", "j'" }; Path p[2] = { { .nv = 4 }, { .nv = 4 } };
        row(&p[0], (int64_t[]){ -1, 0, 1, 0 }, 1, 1); row(&p[0], (int64_t[]){ 0, -1, 0, 1 }, 1, 1);
        row(&p[1], (int64_t[]){ -1, 0, 1, 0 }, 1, 1); row(&p[1], (int64_t[]){ 0, -1, 0, 1 }, 1, -1);
        int pre[] = { 0, 1 }, post[] = { 2, 3 };
        expect_rel("j += 1 | j -= 1: never a relation's counter", p, 2, nm, pre, post, 2, -1, 1, 0, 0);
    }
    {   // a total that rises by up to 7 beside a counter stepping by 2: 2*s - 7*i
        const char *nm[] = { "s", "i", "e", "s'", "i'" }; Path p = { .nv = 5 };
        row(&p, (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0); row(&p, (int64_t[]){ 0, 0, 1, 0, 0 }, 0, 7);
        row(&p, (int64_t[]){ -1, 0, -1, 1, 0 }, 1, 0); row(&p, (int64_t[]){ 0, -1, 0, 0, 1 }, 1, 2);
        int pre[] = { 0, 1 }, post[] = { 3, 4 };
        expect_rel("s += e, e in [0,7], i += 2: 2*s - 7*i", &p, 1, nm, pre, post, 2, 0, 1, 2, -7);
    }
    {   // two paths: i += 2 while s rises by at most 1, or i += 3 while s rises by up to 5: the ratio
        // 2*5/3 is fractional and must round UP (2*s - 4*i; -3*i would rise by 1 on the second path)
        const char *nm[] = { "s", "i", "e", "s'", "i'" }; Path p[2] = { { .nv = 5 }, { .nv = 5 } };
        row(&p[0], (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0); row(&p[0], (int64_t[]){ 0, 0, 1, 0, 0 }, 0, 1);
        row(&p[0], (int64_t[]){ -1, 0, -1, 1, 0 }, 1, 0); row(&p[0], (int64_t[]){ 0, -1, 0, 0, 1 }, 1, 2);
        row(&p[1], (int64_t[]){ 0, 0, -1, 0, 0 }, 0, 0); row(&p[1], (int64_t[]){ 0, 0, 1, 0, 0 }, 0, 5);
        row(&p[1], (int64_t[]){ -1, 0, -1, 1, 0 }, 1, 0); row(&p[1], (int64_t[]){ 0, -1, 0, 0, 1 }, 1, 3);
        int pre[] = { 0, 1 }, post[] = { 3, 4 };
        expect_rel("i += 2 (s +<= 1) | i += 3 (s +<= 5): 2*s - 4*i, rounded up", p, 2, nm, pre, post, 2, 0, 1, 2, -4);
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
