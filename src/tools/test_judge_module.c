// src/tools/test_judge_module.c — the judge's ENTRY POINT (src/judge/judge.h) over a hand-built
// module, with the untrusted callbacks stubbed, so each way an answer can be wrong reaches it:
//   1. honest answers (the real solver): a guarded division `proved`, an unguarded one
//      `not-implied` with its model, checked (A6);
//   2. a certificate the kernel refuses (a sign byte of 2) is a `solver-bug`, never `proved`;
//   3. a model at which the goal holds, and one a hypothesis is false at, are `solver-bug`;
//   4. a budget answer is `budget` with the solver's counters; no solver at all proves nothing;
//   5. claims: a true return claim is certified (its VCs are records too, before the
//      obligations), and the obligations are decided; a false one makes every obligation of the
//      module `uncertified`, naming the claim's VC; a claim the generator refuses does the same;
//      a proposer that fails is a `no-claims` record, and the module is judged without claims;
//   6. a function the generator refuses (a phi) is one `refused` record, and the rest is judged;
//   7. the same module judged twice gives the same records; a region too small is `err`, never
//      an abort.
//
// Build & run (run_ir_tests.sh does this):
//   gcc -std=c99 -o /tmp/test_judge_module src/tools/test_judge_module.c -I src && /tmp/test_judge_module
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include <string.h>
#include "judge/judge.h"
#include "judge/text.h"
#include "solver/lia.h"      // the honest answers: the untrusted solver, as the compiler will call it

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Arena A;
static char region[1 << 24], work[1 << 22];
static IrName *nm(const char *s) { return ir_intern(&A, s, (isize)strlen(s)); }
static IrType *u32t;

// ── the stub callbacks ──────────────────────────────────────────────────────────────────────
enum { S_HONEST, S_CORRUPT, S_GOAL_MODEL, S_HYP_MODEL, S_BUDGET };
typedef struct { int mode; int propose; JClaim claim; JRank rank[2]; int32_t nrank; } Stub;
enum { P_NONE, P_CLAIM, P_FAIL, P_RANK };
static JInt jv(int64_t v) { JInt x; memset(&x, 0, sizeof x); uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v; x.neg = v < 0; while (u) { x.limb[x.n++] = (uint32_t)u; u >>= 32; } return x; }
static JInt model_vals[64];
static void corrupt(JSteps *b, bool *done) {          // the first combination anywhere: a sign byte 2
    for (int32_t i = 0; b && i < b->n && !*done; i++) {
        if (b->s[i].k == J_LIN && b->s[i].nmul) { b->s[i].mul[0].m.neg = 2; *done = true; }
        for (int32_t k = 0; k < b->s[i].nbr && !*done; k++) corrupt(&b->s[i].br[k], done);
    }
}
static bool stub_solve(void *ctx, const JVc *vc, JWork lim, JAnswer *out) {
    Stub *s = ctx;
    *out = lia_solve(vc, lim, work, sizeof work);
    switch (s->mode) {
        case S_CORRUPT: { bool done = false; if (out->k == J_ANS_CERT) corrupt(&out->cert.body, &done); break; }
        case S_GOAL_MODEL: case S_HYP_MODEL: {
            // a model of the solver's own making: every variable 7 (100 / 7 is fine: the goal
            // holds there), or 2^40 (past a u32: a type hypothesis is false there)
            if (vc->nvars > 64) break;
            for (int32_t i = 0; i < vc->nvars; i++) model_vals[i] = jv(s->mode == S_GOAL_MODEL ? 7 : (int64_t)1 << 40);
            out->k = J_ANS_MODEL; out->model.id = vc->id; out->model.val = model_vals; out->model.n = vc->nvars;
            break;
        }
        case S_BUDGET: out->k = J_ANS_BUDGET; out->used.pivot = 11; out->used.node = 22; out->used.leaf = 33; break;
        default: break;
    }
    return true;
}
static bool stub_propose(void *ctx, IrFunc *mod, const JudgeGen *gen, JClaim **out, int32_t *n, JRank **rk, int32_t *nrk) {
    Stub *s = ctx; (void)mod; (void)gen;
    *rk = s->nrank ? s->rank : NULL; *nrk = s->nrank;
    if (s->propose == P_FAIL) return false;
    if (s->propose == P_CLAIM) { *out = &s->claim; *n = 1; } else { *out = NULL; *n = 0; }      // P_RANK: ranks only
    return true;
}
// ── the records, as text, in order ─────────────────────────────────────────────────────────
static char recs[1 << 16]; static size_t nrec_text; static int nrec[9];
static void sink(void *ctx, const JRecord *r) {
    (void)ctx;
    static const char *kn[] = { "proved", "not-implied", "outside", "budget", "capacity", "solver-bug", "uncertified", "refused", "no-claims" };
    nrec[r->k]++;
    char line[600]; int o = snprintf(line, sizeof line, "%s %s %s %s", kn[r->k], r->id ? r->id->point : "-", r->id ? r->id->kind : "-", r->reason ? r->reason : "");
    if (r->k == JREC_NOT_IMPLIED && r->model) for (int32_t i = 0; i < r->model->n && o < 500; i++) {
        JInt *x = &r->model->val[i]; long long v = x->n ? (long long)x->limb[0] : 0; if (x->neg) v = -v;
        o += snprintf(line + o, sizeof line - (size_t)o, " %s=%lld", r->vc->vars[i], v);
    }
    if (r->k == JREC_BUDGET) snprintf(line + o, sizeof line - (size_t)o, " used=%llu,%llu,%llu", (unsigned long long)r->used.pivot, (unsigned long long)r->used.node, (unsigned long long)r->used.leaf);
    size_t l = strlen(line);
    if (nrec_text + l + 2 < sizeof recs) { memcpy(recs + nrec_text, line, l); recs[nrec_text + l] = '\n'; nrec_text += l + 1; recs[nrec_text] = 0; }
}
static bool run(IrFunc *mod, Stub *s, bool solver, size_t rsize, char *err) {
    nrec_text = 0; recs[0] = 0; memset(nrec, 0, sizeof nrec);
    JudgeCalls calls = { s, solver ? stub_solve : NULL, s->propose == P_NONE ? NULL : stub_propose };
    JudgeLimits lim; lim.lim.pivot = 100000; lim.lim.node = 4096; lim.lim.leaf = 4096;
    JMem m = jmem_over(region, rsize);
    return judge_module(mod, &lim, &calls, sink, NULL, &m, err, 200);
}

int main(void) {
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 256);
    u32t = ir_type_int(&A, 32, false);
    // f(x, y): if y != 0 { return 100 / y } else { return 100 / x }
    IrFunc *f = ir_func_new(&A, nm("f"), u32t, IR_FUNC_PURE);
    IrValue *x = ir_add_param(f, u32t, nm("x")), *y = ir_add_param(f, u32t, nm("y"));
    IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
    ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_NE, y, ir_const_int(f, b0, 0, u32t)), b1, b2);
    IrValue *q1 = ir_binop(f, b1, IR_UDIV, ir_const_int(f, b1, 100, u32t), y, u32t);
    ir_set_ret(b1, q1);
    IrValue *q2 = ir_binop(f, b2, IR_UDIV, ir_const_int(f, b2, 100, u32t), x, u32t);
    ir_set_ret(b2, q2);
    ir_finalize_cfg(f);
    (void)q1; (void)q2;
    char err[200];
    char p1[16], p2[16];
    snprintf(p1, sizeof p1, "bb%d.", b1->id); snprintf(p2, sizeof p2, "bb%d.", b2->id);

    printf("── honest answers ──\n");
    Stub s; memset(&s, 0, sizeof s);
    CHECK(run(f, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_PROVED] == 1 && nrec[JREC_NOT_IMPLIED] == 1, "the guarded division proved, the other not implied:\n%s", recs);
    char want[64]; snprintf(want, sizeof want, "not-implied %s", p2);
    CHECK(strstr(recs, want) && strstr(recs, " %v0=0"), "x = 0 refutes 100 / x, and the model says so:\n%s", recs);
    char honest[1 << 16]; snprintf(honest, sizeof honest, "%s", recs);
    CHECK(run(f, &s, true, sizeof region, err) && !strcmp(recs, honest), "judged twice, the same records:\n%s\n%s", honest, recs);

    printf("── answers the judge cannot show ──\n");
    s.mode = S_CORRUPT;
    CHECK(run(f, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_PROVED] == 0 && nrec[JREC_SOLVER_BUG] == 1 && strstr(recs, "not canonical"), "a certificate the kernel refuses is a solver bug:\n%s", recs);
    s.mode = S_GOAL_MODEL;
    CHECK(run(f, &s, true, sizeof region, err) && nrec[JREC_SOLVER_BUG] == 2 && strstr(recs, "the goal holds"), "a model where the goal holds is a solver bug:\n%s", recs);
    s.mode = S_HYP_MODEL;
    CHECK(run(f, &s, true, sizeof region, err) && nrec[JREC_SOLVER_BUG] == 2 && strstr(recs, "hypothesis is false"), "a model a hypothesis is false at is a solver bug:\n%s", recs);
    s.mode = S_BUDGET;
    CHECK(run(f, &s, true, sizeof region, err) && nrec[JREC_BUDGET] == 2 && strstr(recs, "used=11,22,33"), "a budget answer, with its counters:\n%s", recs);
    s.mode = S_HONEST;
    CHECK(run(f, &s, false, sizeof region, err) && nrec[JREC_PROVED] == 0 && nrec[JREC_BUDGET] == 2, "no solver proves nothing:\n%s", recs);

    printf("── claims ──\n");
    s.propose = P_CLAIM;
    memset(&s.claim, 0, sizeof s.claim);
    s.claim.blk = JCLAIM_RET; s.claim.fn = f; s.claim.nt = 1; s.claim.key[0] = JKEY_RET; s.claim.coef[0] = 1; s.claim.rhs = 4294967295;   // a u32: true
    CHECK(run(f, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_PROVED] == 3 && nrec[JREC_NOT_IMPLIED] == 1 && nrec[JREC_UNCERTIFIED] == 0, "a true claim: its two post VCs proved, then the obligations:\n%s", recs);
    CHECK(!strncmp(recs, "proved", 6) && strstr(recs, " post\n") < strstr(recs, " divzero"), "the claim's VCs come first:\n%s", recs);
    s.claim.rhs = 5;                                                       // at most 5: false (100 / 1 = 100)
    CHECK(run(f, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_UNCERTIFIED] == 2 && nrec[JREC_PROVED] + nrec[JREC_NOT_IMPLIED] <= 1 && strstr(recs, "is not certified"),
          "a false claim makes every obligation uncertified, naming the claim's VC:\n%s", recs);
    s.claim.rhs = 4294967295; s.claim.nt = JCLAIM_MAXT + 1;                // a claim the generator refuses
    CHECK(run(f, &s, true, sizeof region, err) && nrec[JREC_UNCERTIFIED] == 2 && strstr(recs, "refuses the claims"), "a claim the generator refuses, likewise:\n%s", recs);
    s.claim.nt = 1; s.propose = P_FAIL;
    CHECK(run(f, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_NO_CLAIMS] == 1 && !strncmp(recs, "no-claims", 9) && nrec[JREC_PROVED] == 1 && nrec[JREC_NOT_IMPLIED] == 1,
          "a proposer that fails is said, and the module judged without claims:\n%s", recs);
    s.propose = P_NONE;

    printf("── a loop's termination ──\n");
    // h(n): i = 0; while i < n { i = i + 1 }. Owed termination: `outside` with no ranking claim,
    // both VCs proved with n - i, and a ranking claim that rises is not implied, with a model
    IrFunc *h = ir_func_new(&A, nm("h"), u32t, IR_FUNC_PURE);
    IrValue *hn = ir_add_param(h, u32t, nm("n"));
    IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h), *h3 = ir_new_block(h);
    IrValue *hi = ir_alloca(h, h0, u32t);
    ir_store(h, h0, hi, ir_const_int(h, h0, 0, u32t)); ir_set_br(h0, h1);
    ir_set_br_cond(h1, ir_icmp(h, h1, IR_CMP_ULT, ir_load(h, h1, hi, u32t), hn), h2, h3);
    ir_store(h, h2, hi, ir_binop(h, h2, IR_ADD, ir_load(h, h2, hi, u32t), ir_const_int(h, h2, 1, u32t), u32t));
    ir_set_br(h2, h1);
    ir_set_ret(h3, ir_const_int(h, h3, 0, u32t));
    ir_finalize_cfg(h);
    CHECK(run(h, &s, true, sizeof region, err) && nrec[JREC_OUTSIDE] == 1 && strstr(recs, "no ranking function"), "no ranking claim: outside:\n%s", recs);
    s.propose = P_RANK; s.nrank = 1;
    memset(&s.rank[0], 0, sizeof s.rank[0]);
    s.rank[0].blk = h1->id; s.rank[0].fn = h; s.rank[0].ncomp = 1; s.rank[0].nt = 2;
    s.rank[0].key[0] = hi->id; s.rank[0].key[1] = hn->id; s.rank[0].coef[0][0] = -1; s.rank[0].coef[0][1] = 1;   // n - i
    s.rank[0].nedge = 1; s.rank[0].from[0] = h2->id; s.rank[0].fall[0] = 0;
    int hp = 0;
    for (IrInstr *i = h2->instrs; i; i = i->next) hp++;
    char rf[48], rb[48];
    snprintf(rf, sizeof rf, "proved bb%d.%d rank.fall", h2->id, hp); snprintf(rb, sizeof rb, "proved bb%d.%d rank.bound", h2->id, hp);
    CHECK(run(h, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_OUTSIDE] == 0 && strstr(recs, rf) && strstr(recs, rb) && !strstr(recs, "not-implied") && nrec[JREC_UNCERTIFIED] == 0,
          "n - i: rank.fall and rank.bound proved:\n%s", recs);
    s.propose = P_FAIL;                                                    // a failed proposer's ranks are dropped
    CHECK(run(h, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_NO_CLAIMS] == 1 && nrec[JREC_OUTSIDE] == 1 && !strstr(recs, "rank."), "a proposer that fails hands over no ranks:\n%s", recs);
    s.propose = P_RANK;
    s.rank[0].coef[0][0] = 1; s.rank[0].coef[0][1] = 0;                    // i: rises, never falls
    CHECK(run(h, &s, true, sizeof region, err), "judges: %s", err);
    snprintf(rf, sizeof rf, "not-implied bb%d.%d rank.fall", h2->id, hp);
    CHECK(strstr(recs, rf) && nrec[JREC_NOT_IMPLIED] == 1 && nrec[JREC_SOLVER_BUG] == 0, "a ranking claim that rises: rank.fall not implied:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;
    // a WRAPPING step, `i = i +% 1` on u8 under `i < n`: the add's two cases, and the guard rules
    // out the wrapped one, so n - i falls (Main Compiler Work's census: 7 such loops)
    IrType *u8t = ir_type_int(&A, 8, false);
    IrFunc *w = ir_func_new(&A, nm("w"), u32t, IR_FUNC_PURE);
    IrValue *wn = ir_add_param(w, u8t, nm("n"));
    IrBlock *w0 = w->entry, *w1 = ir_new_block(w), *w2 = ir_new_block(w), *w3 = ir_new_block(w);
    IrValue *wi = ir_alloca(w, w0, u8t);
    ir_store(w, w0, wi, ir_const_int(w, w0, 0, u8t)); ir_set_br(w0, w1);
    ir_set_br_cond(w1, ir_icmp(w, w1, IR_CMP_ULT, ir_load(w, w1, wi, u8t), wn), w2, w3);
    IrValue *wone = ir_const_int(w, w2, 1, ir_type_int(&A, 32, true));     // an i32 literal, as lowering makes it
    IrValue *ws = ir_binop(w, w2, IR_ADD, ir_load(w, w2, wi, u8t), wone, u8t);
    w2->instrs_tail->wrap = IR_WRAP_MODULAR;
    ir_store(w, w2, wi, ws); ir_set_br(w2, w1);
    ir_set_ret(w3, ir_const_int(w, w3, 0, u32t));
    ir_finalize_cfg(w);
    s.propose = P_RANK; s.nrank = 1;
    memset(&s.rank[0], 0, sizeof s.rank[0]);
    s.rank[0].blk = w1->id; s.rank[0].fn = w; s.rank[0].ncomp = 1; s.rank[0].nt = 2;
    s.rank[0].key[0] = wi->id; s.rank[0].key[1] = wn->id; s.rank[0].coef[0][0] = -1; s.rank[0].coef[0][1] = 1;   // n - i
    s.rank[0].nedge = 1; s.rank[0].from[0] = w2->id; s.rank[0].fall[0] = 0;
    int wp = 0;
    for (IrInstr *i = w2->instrs; i; i = i->next) wp++;
    snprintf(rf, sizeof rf, "proved bb%d.%d rank.fall", w2->id, wp);
    CHECK(run(w, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && nrec[JREC_SOLVER_BUG] == 0, "i +%% 1 under i < n: n - i falls:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;

    printf("── a function the generator refuses, a region too small ──\n");
    IrFunc *g = ir_func_new(&A, nm("g"), u32t, IR_FUNC_PURE);
    IrValue *z = ir_add_param(g, u32t, nm("z"));
    ir_set_ret(g->entry, ir_binop(g, g->entry, IR_UDIV, ir_const_int(g, g->entry, 100, u32t), z, u32t));
    ir_finalize_cfg(g);
    g->entry->phis = ir_instr(g, IR_PHI, u32t, 0);                         // the generator reads none
    f->next = g;
    CHECK(run(f, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_REFUSED] == 1 && strstr(recs, "a phi") && nrec[JREC_PROVED] == 1, "g refused, f judged:\n%s", recs);
    f->next = NULL;
    CHECK(!run(f, &s, true, 64, err) && strstr(err, "full"), "a region too small: err, not an abort (\"%s\")", err);

    printf("%s\n", failures ? "test_judge_module: FAILED" : "test_judge_module: all passed");
    return failures ? 1 : 0;
}
