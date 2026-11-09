// src/tools/test_finder_forms.c — how the claim proposer (src/solver/finder.h, untrusted) tells two
// claims apart and which claim shapes it proposes at all, for the term kinds whose identity is not
// their key alone: s31's GHOST (a product of a header's versions, named by its first factor's cell
// and told apart by its index, targ: y*y and y*y*y share key y). A ghost compared by key alone made
// two products one claim (a bound on y*y taken as already proposed by y*y*y's), and `y + y*y` was
// filtered out as one cell named twice, which gen.h's jg_claim_shape accepts.
//
//   gcc -std=c99 -o /tmp/test_finder_forms src/tools/test_finder_forms.c -I src && /tmp/test_finder_forms
#include <stdio.h>
#include <string.h>
#include "ir/ir.h"
#include "solver/finder.h"

static int fails = 0;
static void check(const char *what, bool got, bool want) {
    printf("  %-58s %s\n", what, got == want ? "ok" : "FAILED");
    if (got != want) fails++;
}
static JClaim mk(int nt, const int32_t *key, const uint8_t *tk, const int32_t *targ, __int128 rhs) {
    JClaim c; memset(&c, 0, sizeof c);
    c.blk = 3; c.nt = nt; c.rhs = rhs;
    for (int k = 0; k < nt; k++) { c.key[k] = key[k]; c.tk[k] = tk[k]; c.targ[k] = targ[k]; c.coef[k] = 1; }
    return c;
}
int main(void) {
    int32_t ky[1] = { 7 }, kyy[2] = { 7, 7 }, t0[2] = { 0, 0 }, t1[1] = { 1 }, t01[2] = { 0, 1 }, t00[2] = { 0, 0 };
    uint8_t g[1] = { JTERM_GHOST }, cg[2] = { JTERM_VALUE, JTERM_GHOST }, gg[2] = { JTERM_GHOST, JTERM_GHOST }, cc[2] = { JTERM_VALUE, JTERM_VALUE };
    JClaim sq = mk(1, ky, g, t0, 10), cube = mk(1, ky, g, t1, 10), sq2 = mk(1, ky, g, t0, 10);
    printf("── same_form / fd_claim_same: a GHOST is its index ──\n");
    check("y*y and y*y*y (one key, two indices) differ", same_form(&sq, &cube), false);
    check("y*y and y*y are one form", same_form(&sq, &sq2), true);
    check("y*y and y*y*y are not the same claim", fd_claim_same(&sq, &cube), false);
    check("y*y <= 10 twice is the same claim", fd_claim_same(&sq, &sq2), true);
    printf("── fd_claim_shape_ok: as gen.h's jg_claim_shape ──\n");
    JClaim yg = mk(2, kyy, cg, t0, 10), two = mk(2, kyy, gg, t01, 10), twice = mk(2, kyy, gg, t00, 10), cell2 = mk(2, kyy, cc, t0, 10);
    check("y + y*y: a cell and its ghost", fd_claim_shape_ok(&yg), true);
    check("y*y + y*y*y: two ghosts of one key", fd_claim_shape_ok(&two), true);
    check("y*y + y*y: one ghost twice (gen.h refuses it)", fd_claim_shape_ok(&twice), false);
    check("y + y: one cell twice", fd_claim_shape_ok(&cell2), false);
    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
