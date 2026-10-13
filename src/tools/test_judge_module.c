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
typedef struct { int mode; int propose; JClaim claim; JRank rank[3]; int32_t nrank; } Stub;
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
static int count_in(const char *all, const char *key) { int n = 0; for (const char *p = all; (p = strstr(p, key)); p++) n++; return n; }
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
    CHECK(nrec[JREC_NO_CLAIMS] == 1 && nrec[JREC_OUTSIDE] == 1 && strstr(recs, "rank.fall no ranking function") && !strstr(recs, "rank.bound"),
          "a proposer that fails hands over no ranks:\n%s", recs);
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

    // a WRITTEN measure (`while i < n decreasing m`): `decreasing n - i` proved; `decreasing i`,
    // which rises, not implied (E082 under authority); a ranking claim proposed there is not read
    IrFunc *hm = ir_func_new(&A, nm("hm"), u32t, IR_FUNC_PURE);
    IrValue *mn = ir_add_param(hm, u32t, nm("n"));
    IrBlock *m0 = hm->entry, *m1 = ir_new_block(hm), *m2 = ir_new_block(hm), *m3 = ir_new_block(hm);
    IrValue *mi = ir_alloca(hm, m0, u32t);
    ir_store(hm, m0, mi, ir_const_int(hm, m0, 0, u32t)); ir_set_br(m0, m1);
    ir_set_br_cond(m1, ir_icmp(hm, m1, IR_CMP_ULT, ir_load(hm, m1, mi, u32t), mn), m2, m3);
    IrValue *mli = ir_load(hm, m2, mi, u32t);
    IrValue *meas_ok = ir_binop(hm, m2, IR_SUB, mn, mli, u32t);             // n - i: falls
    ir_store(hm, m2, mi, ir_binop(hm, m2, IR_ADD, mli, ir_const_int(hm, m2, 1, u32t), u32t));
    ir_set_br(m2, m1);
    ir_set_ret(m3, ir_const_int(hm, m3, 0, u32t));
    ir_finalize_cfg(hm);
    m1->has_measure = true; m1->measure_val = meas_ok;
    int mp = 0;
    for (IrInstr *i = m2->instrs; i; i = i->next) mp++;
    s.propose = P_RANK; s.nrank = 1;                                       // a proposal, which rises: not read
    memset(&s.rank[0], 0, sizeof s.rank[0]);
    s.rank[0].blk = m1->id; s.rank[0].fn = hm; s.rank[0].ncomp = 1; s.rank[0].nt = 1; s.rank[0].key[0] = mi->id;
    s.rank[0].coef[0][0] = 1; s.rank[0].nedge = 1; s.rank[0].from[0] = m2->id;
    snprintf(rf, sizeof rf, "proved bb%d.%d measure.fall", m2->id, mp); snprintf(rb, sizeof rb, "proved bb%d.%d measure.bound", m2->id, mp);
    CHECK(run(hm, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && strstr(recs, rb) && !strstr(recs, "rank.") && !strstr(recs, "not-implied"), "decreasing n - i: proved, the proposal not read:\n%s", recs);
    m1->measure_val = mli;                                                 // decreasing i: it rises
    snprintf(rf, sizeof rf, "not-implied bb%d.%d measure.fall", m2->id, mp);
    CHECK(run(hm, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && nrec[JREC_SOLVER_BUG] == 0, "decreasing i, which rises: measure.fall not implied:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;

    // a RECURSION: r(n) calls r(n - 1) past `n == 0`. With the measure n, rec.fall and rec.bound
    // proved; with 2 - n, which rises as n falls, rec.fall not implied; with none, outside
    IrFunc *rf_ = ir_func_new(&A, nm("r"), u32t, IR_FUNC_PURE);
    IrValue *rn = ir_add_param(rf_, u32t, nm("n"));
    IrBlock *r0 = rf_->entry, *r1 = ir_new_block(rf_), *r2 = ir_new_block(rf_);
    ir_set_br_cond(r0, ir_icmp(rf_, r0, IR_CMP_EQ, rn, ir_const_int(rf_, r0, 0, u32t)), r1, r2);
    ir_set_ret(r1, ir_const_int(rf_, r1, 0, u32t));
    IrValue *rm = ir_binop(rf_, r2, IR_SUB, rn, ir_const_int(rf_, r2, 1, u32t), u32t);
    int rci = 0;
    for (IrInstr *i = r2->instrs; i; i = i->next) rci++;
    IrInstr *rc = ir_instr(rf_, IR_CALL, u32t, 1); rc->aux.callee = nm("r"); rc->operands[0] = rm; ir_emit(r2, rc);
    ir_set_ret(r2, rc->result);
    ir_finalize_cfg(rf_);
    CHECK(run(rf_, &s, true, sizeof region, err) && strstr(recs, "rec.fall no measure proposed"), "no measure: outside:\n%s", recs);
    s.propose = P_RANK; s.nrank = 1;
    memset(&s.rank[0], 0, sizeof s.rank[0]);
    s.rank[0].blk = JRANK_ENTRY; s.rank[0].fn = rf_; s.rank[0].ncomp = 1; s.rank[0].nt = 1; s.rank[0].key[0] = rn->id; s.rank[0].coef[0][0] = 1;
    snprintf(rf, sizeof rf, "proved bb%d.%d rec.fall", r2->id, rci); snprintf(rb, sizeof rb, "proved bb%d.%d rec.bound", r2->id, rci);
    CHECK(run(rf_, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && strstr(recs, rb) && nrec[JREC_OUTSIDE] == 0, "the measure n: proved:\n%s", recs);
    s.rank[0].coef[0][0] = -1; s.rank[0].c0[0] = 2;                        // 2 - n: rises as n falls
    snprintf(rf, sizeof rf, "not-implied bb%d.%d rec.fall", r2->id, rci);
    CHECK(run(rf_, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && nrec[JREC_SOLVER_BUG] == 0, "2 - n: rec.fall not implied:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;

    // spec 12's CUT, judged: a(n) -> b(n - 1) and c(n) -> a(n - 1) past `n == 0`, b(n) -> c(n), the
    // measure n each. With no call listed, b -> c's rec.fall is not implied (n - n <= -1); with it
    // listed, rec.le there, and all proved (a's and c's n - 1 owe narrow.lo and .hi besides); with
    // every call listed, all three refused
    IrFunc *cf[3]; IrValue *cn[3]; IrBlock *cbk[3]; int cci[3];
    const char *cnm[3] = { "a", "b", "c" }, *cto[3] = { "b", "c", "a" };
    for (int w = 0; w < 3; w++) {
        IrFunc *h = cf[w] = ir_func_new(&A, nm(cnm[w]), u32t, IR_FUNC_PURE);
        cn[w] = ir_add_param(h, u32t, nm("n"));
        IrValue *arg = cn[w]; IrBlock *at = h->entry;
        if (w != 1) {                                                      // a and c: n - 1 past n == 0
            IrBlock *h1 = ir_new_block(h), *h2 = ir_new_block(h);
            ir_set_br_cond(h->entry, ir_icmp(h, h->entry, IR_CMP_EQ, cn[w], ir_const_int(h, h->entry, 0, u32t)), h1, h2);
            ir_set_ret(h1, ir_const_int(h, h1, 0, u32t));
            arg = ir_binop(h, h2, IR_SUB, cn[w], ir_const_int(h, h2, 1, u32t), u32t); at = h2;
        }
        cci[w] = 0; for (IrInstr *i = at->instrs; i; i = i->next) cci[w]++;
        IrInstr *c = ir_instr(h, IR_CALL, u32t, 1); c->aux.callee = nm(cto[w]); c->operands[0] = arg; ir_emit(at, c);
        ir_set_ret(at, c->result); cbk[w] = at;
        ir_finalize_cfg(h);
    }
    cf[0]->next = cf[1]; cf[1]->next = cf[2];
    s.propose = P_RANK; s.nrank = 3;
    memset(s.rank, 0, sizeof s.rank);
    for (int w = 0; w < 3; w++) { s.rank[w].blk = JRANK_ENTRY; s.rank[w].fn = cf[w]; s.rank[w].ncomp = 1; s.rank[w].nt = 1; s.rank[w].key[0] = cn[w]->id; s.rank[w].coef[0][0] = 1; }
    snprintf(rf, sizeof rf, "not-implied bb%d.%d rec.fall", cbk[1]->id, cci[1]);
    CHECK(run(cf[0], &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && nrec[JREC_NOT_IMPLIED] == 1 && nrec[JREC_PROVED] == 9, "no call listed: b -> c does not fall:\n%s", recs);
    s.rank[1].nle = 1; s.rank[1].le_blk[0] = cbk[1]->id; s.rank[1].le_idx[0] = cci[1];
    snprintf(rf, sizeof rf, "proved bb%d.%d rec.le", cbk[1]->id, cci[1]);
    CHECK(run(cf[0], &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, rf) && nrec[JREC_PROVED] == 10 &&
          nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] + nrec[JREC_REFUSED] == 0, "b -> c listed: the cut holds, all proved:\n%s", recs);
    for (int w = 0; w < 3; w += 2) { s.rank[w].nle = 1; s.rank[w].le_blk[0] = cbk[w]->id; s.rank[w].le_idx[0] = cci[w]; }
    CHECK(run(cf[0], &s, true, sizeof region, err), "judges: %s", err);
    CHECK(nrec[JREC_REFUSED] == 3 && nrec[JREC_PROVED] == 0 && strstr(recs, "close a cycle"), "every call listed: the three refused:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;

    // a FIXED condition: `x = n; while 0 != 0 { x = x + 1 }` with i32 x. The body is never entered,
    // so x + 1's narrow.hi is proved (before, not implied at x = 2^31 - 1), and a constant ranking
    // claim proves the loop ends: its fall, 0 <= -1, holds on the dead back edge
    IrFunc *kf = ir_func_new(&A, nm("k"), u32t, IR_FUNC_PURE);
    IrType *i32t = ir_type_int(&A, 32, true);
    IrValue *kn = ir_add_param(kf, i32t, nm("n"));
    IrBlock *k0 = kf->entry, *k1 = ir_new_block(kf), *k2 = ir_new_block(kf), *k3 = ir_new_block(kf);
    IrValue *kx = ir_alloca(kf, k0, i32t);
    ir_store(kf, k0, kx, kn); ir_set_br(k0, k1);
    ir_set_br_cond(k1, ir_icmp(kf, k1, IR_CMP_NE, ir_const_int(kf, k1, 0, i32t), ir_const_int(kf, k1, 0, i32t)), k2, k3);
    IrValue *klx = ir_load(kf, k2, kx, i32t);
    ir_store(kf, k2, kx, ir_binop(kf, k2, IR_ADD, klx, ir_const_int(kf, k2, 1, i32t), i32t));
    ir_set_br(k2, k1);
    ir_set_ret(k3, ir_const_int(kf, k3, 0, u32t));
    ir_finalize_cfg(kf);
    s.propose = P_RANK; s.nrank = 1;
    memset(&s.rank[0], 0, sizeof s.rank[0]);
    s.rank[0].blk = k1->id; s.rank[0].fn = kf; s.rank[0].ncomp = 1; s.rank[0].nt = 0; s.rank[0].nedge = 1; s.rank[0].from[0] = k2->id;
    CHECK(run(kf, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, "proved") && strstr(recs, "narrow.hi") && strstr(recs, "rank.fall") && strstr(recs, "rank.bound") &&
          nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] + nrec[JREC_REFUSED] + nrec[JREC_SOLVER_BUG] == 0,
          "while 0 != 0: the body's narrowing and the loop's end proved:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;

    // a ranking claim over a cell the loop only READS: `i = 0; c = n; while i < c { i = i + 1 }`
    // with c - i, proved (the term c is the header's version of c, the one every trip reads)
    IrFunc *rf2 = ir_func_new(&A, nm("rr"), u32t, IR_FUNC_PURE);
    IrValue *rn2 = ir_add_param(rf2, u32t, nm("n"));
    IrBlock *rb0 = rf2->entry, *rb1 = ir_new_block(rf2), *rb2 = ir_new_block(rf2), *rb3 = ir_new_block(rf2);
    IrValue *qi = ir_alloca(rf2, rb0, u32t), *qc = ir_alloca(rf2, rb0, u32t);
    ir_store(rf2, rb0, qi, ir_const_int(rf2, rb0, 0, u32t)); ir_store(rf2, rb0, qc, rn2); ir_set_br(rb0, rb1);
    IrValue *qli = ir_load(rf2, rb1, qi, u32t), *qlc = ir_load(rf2, rb1, qc, u32t);
    ir_set_br_cond(rb1, ir_icmp(rf2, rb1, IR_CMP_ULT, qli, qlc), rb2, rb3);
    IrValue *qli2 = ir_load(rf2, rb2, qi, u32t);
    ir_store(rf2, rb2, qi, ir_binop(rf2, rb2, IR_ADD, qli2, ir_const_int(rf2, rb2, 1, u32t), u32t));
    ir_set_br(rb2, rb1);
    ir_set_ret(rb3, ir_const_int(rf2, rb3, 0, u32t));
    ir_finalize_cfg(rf2);
    s.propose = P_RANK; s.nrank = 1;
    memset(&s.rank[0], 0, sizeof s.rank[0]);
    s.rank[0].blk = rb1->id; s.rank[0].fn = rf2; s.rank[0].ncomp = 1; s.rank[0].nt = 2; s.rank[0].nedge = 1; s.rank[0].from[0] = rb2->id;
    s.rank[0].key[0] = qc->id; s.rank[0].coef[0][0] = 1; s.rank[0].key[1] = qi->id; s.rank[0].coef[0][1] = -1;
    CHECK(run(rf2, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, "rank.fall") && strstr(recs, "rank.bound") &&
          nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] + nrec[JREC_REFUSED] + nrec[JREC_SOLVER_BUG] == 0,
          "c - i over a cell only read: proved:\n%s", recs);
    s.propose = P_NONE; s.nrank = 0;

    // an UNSIGNED comparison of a signed operand, split on its sign: `if x <u y { 100 / y }` with
    // x an i64 that may be negative and y a u64: y is not 0 on either side of x's sign
    IrType *s64 = ir_type_int(&A, 64, true), *u64 = ir_type_int(&A, 64, false);
    IrFunc *uf = ir_func_new(&A, nm("u"), u64, IR_FUNC_PURE);
    IrValue *ux = ir_add_param(uf, s64, nm("x")), *uy = ir_add_param(uf, u64, nm("y"));
    IrBlock *u0 = uf->entry, *u1 = ir_new_block(uf), *u2 = ir_new_block(uf);
    ir_set_br_cond(u0, ir_icmp(uf, u0, IR_CMP_ULT, ux, uy), u1, u2);
    ir_set_ret(u1, ir_binop(uf, u1, IR_UDIV, ir_const_int(uf, u1, 100, u64), uy, u64));
    ir_set_ret(u2, ir_const_int(uf, u2, 0, u64));
    ir_finalize_cfg(uf);
    CHECK(run(uf, &s, true, sizeof region, err), "judges: %s", err);
    CHECK(strstr(recs, "proved bb1.1 divzero") && nrec[JREC_NOT_IMPLIED] + nrec[JREC_SOLVER_BUG] == 0,
          "x <u y with x an i64: 100 / y proved:\n%s", recs);

    // a WRITTEN function measure (I.206), a ghost over the entry parameters, owed as written:
    // (a) f(n, k) decreasing k, recursing on f(n - 1, k): rec.fall not implied (I.206's ERROR);
    // (b) down(k) decreasing k, on down(k - 1) past k <= 0: proved (its VERIFY); (c) g(lo, hi)
    // decreasing hi - lo over u32, on g(lo + 1, hi) past lo >= hi: the subtraction well-defined at
    // the entry and at the arguments (rec.def), and all proved; (d) h(n) decreasing n + 1 over i32:
    // n + 1 overflows at 2^31 - 1, rec.def not implied; (e) decreasing 2 * n over i32: it falls by
    // 2, but 2 * n overflows at the entry and at the arguments, two rec.def not implied; (f) k of
    // type `i32 >= 0` decreasing k - 1: at the arguments k - 2 may be -1, outside the refinement
    // but well-defined (a refinement is no part of the arithmetic), so all proved
    IrType *ki32 = ir_type_int(&A, 32, true), *kref = arena_push_aligned(&A, IrType);
    *kref = *ki32; kref->has_refine = true; kref->refine_lo = 0; kref->refine_hi = 2147483647;
    for (int c = 0; c < 6; c++) {
        IrType *T = c == 2 ? u32t : c == 5 ? kref : i32t;
        IrFunc *wf = ir_func_new(&A, nm("w"), T, IR_FUNC_PURE);
        IrValue *p0 = ir_add_param(wf, T, nm("p")), *p1 = c == 0 || c == 2 ? ir_add_param(wf, T, nm("q")) : NULL;
        IrBlock *w0 = wf->entry, *w1 = ir_new_block(wf), *w2 = ir_new_block(wf);
        IrValue *stop = c == 2 ? ir_icmp(wf, w0, IR_CMP_UGE, p0, p1) : ir_icmp(wf, w0, IR_CMP_SLE, c == 0 ? p0 : p0, ir_const_int(wf, w0, 0, T));
        ir_set_br_cond(w0, stop, w1, w2);
        ir_set_ret(w1, ir_const_int(wf, w1, 0, T));
        IrValue *a0 = ir_binop(wf, w2, c == 2 ? IR_ADD : IR_SUB, p0, ir_const_int(wf, w2, 1, T), T);
        int wci = 0; for (IrInstr *i = w2->instrs; i; i = i->next) wci++;
        IrInstr *wc = ir_instr(wf, IR_CALL, T, p1 ? 2 : 1); wc->aux.callee = nm("w"); wc->operands[0] = a0; if (p1) wc->operands[1] = p1;
        ir_emit(w2, wc); ir_set_ret(w2, wc->result);
        wf->has_decreasing = true; wf->measure_blk = ir_new_ghost_block(wf);
        IrBlock *gb = wf->measure_blk;
        wf->measure_val = c == 0 ? p1 : c == 1 ? p0 : c == 2 ? ir_binop(wf, gb, IR_SUB, p1, p0, T)
                        : c == 3 ? ir_binop(wf, gb, IR_ADD, p0, ir_const_int(wf, gb, 1, T), T)
                        : c == 5 ? ir_binop(wf, gb, IR_SUB, p0, ir_const_int(wf, gb, 1, T), T)
                                 : ir_binop(wf, gb, IR_MUL, ir_const_int(wf, gb, 2, T), p0, T);
        ir_finalize_cfg(wf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(wf, &s, true, sizeof region, err), "judges: %s", err);
        char want[64];
        if (c == 0) snprintf(want, sizeof want, "not-implied bb%d.%d rec.fall", w2->id, wci);
        else if (c == 3 || c == 4) snprintf(want, sizeof want, "not-implied bb%d.%d rec.def", w2->id, wci);
        else snprintf(want, sizeof want, "proved bb%d.%d rec.%s", w2->id, wci, c == 2 || c == 5 ? "def" : "fall");
        bool clean = nrec[JREC_OUTSIDE] + nrec[JREC_REFUSED] + nrec[JREC_SOLVER_BUG] == 0;
        CHECK(strstr(recs, want) && clean && nrec[JREC_NOT_IMPLIED] == (c == 0 || c == 4 ? 2 : c == 3 ? 1 : 0) &&
              (c != 4 || strstr(recs, "proved bb2.2 rec.fall")) &&
              (c != 2 || (count_in(recs, "proved") == nrec[JREC_PROVED] && strstr(recs, "rec.fall") && strstr(recs, "rec.bound"))),
              "written measure case %d (%s):\n%s", c, want, recs);
    }

    // ELEMENT cells: `var a i32[4] = [10, 20, 30, 40]; return a[0] - 10` proves its narrowing (a[0]
    // is the stored 10); with `a[i] = 7` between, a[0] is fresh and its narrow.lo is not implied
    for (int c = 0; c < 2; c++) {
        IrType *ai32 = ir_type_int(&A, 32, true), *u64e = ir_type_int(&A, 64, false);
        IrType *arr = ir_type_new(&A, IRT_ARRAY); arr->elem = ai32; arr->array_len = 4;
        IrFunc *ef = ir_func_new(&A, nm("el"), ai32, IR_FUNC_PURE);
        IrValue *ei = ir_add_param(ef, u64e, nm("i"));
        IrBlock *e0 = ef->entry;
        IrValue *ea = ir_alloca_array(ef, e0, arr);
        for (int k = 0; k < 4; k++)
            ir_store(ef, e0, ir_elem_ptr(ef, e0, ea, ir_const_int(ef, e0, k, u64e), ai32), ir_const_int(ef, e0, 10 * (k + 1), ai32));
        if (c) ir_store(ef, e0, ir_elem_ptr(ef, e0, ea, ei, ai32), ir_const_int(ef, e0, 7, ai32));
        IrValue *e00 = ir_load(ef, e0, ir_elem_ptr(ef, e0, ea, ir_const_int(ef, e0, 0, ai32), ai32), ai32);
        ir_set_ret(e0, ir_binop(ef, e0, IR_SUB, e00, ir_const_int(ef, e0, 10, ai32), ai32));
        ir_finalize_cfg(ef);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(ef, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(c ? strstr(recs, "narrow.lo  %v") != NULL && strstr(strstr(recs, "narrow.lo  %v") - 30, "not-implied") != NULL &&
                  strstr(recs, "@havoc") != NULL                // a[i] = 7 also owes i <= 3, not implied either
                : nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] == 0 && nrec[JREC_PROVED] > 0,
              "element cells, case %d:\n%s", c, recs);
    }

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
