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
typedef struct { int mode; int propose; JClaim claim; JRank rank[3]; int32_t nrank; JClaim claims[4]; int32_t nclaims; JGuardClaim gc[2]; int32_t ngc; } Stub;
enum { P_NONE, P_CLAIM, P_FAIL, P_RANK };
static int nins_m(IrBlock *b) { int n = 0; for (IrInstr *i = b->instrs; i; i = i->next) n++; return n; }
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
static bool stub_propose(void *ctx, IrFunc *mod, const JudgeGen *gen, JClaim **out, int32_t *n, JGuardClaim **gc, int32_t *ngc,
                         JRank **rk, int32_t *nrk) {
    Stub *s = ctx; (void)mod; (void)gen;
    *gc = s->ngc ? s->gc : NULL; *ngc = s->ngc;
    *rk = s->nrank ? s->rank : NULL; *nrk = s->nrank;
    if (s->propose == P_FAIL) return false;
    if (s->propose == P_CLAIM) { *out = &s->claim; *n = 1; } else { *out = NULL; *n = 0; }      // P_RANK: ranks only
    if (s->nclaims) { *out = s->claims; *n = s->nclaims; }                                      // several, with the ranks
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
        JInt *x = &r->model->val[i];                         // 32-bit limbs: two fit, more are marked
        unsigned long long u = x->n ? x->limb[0] : 0; if (x->n >= 2) u |= (unsigned long long)x->limb[1] << 32;
        if (x->n > 2) o += snprintf(line + o, sizeof line - (size_t)o, " %s=%s(past 2^64)", r->vc->vars[i], x->neg ? "-" : "");
        else o += snprintf(line + o, sizeof line - (size_t)o, " %s=%s%llu", r->vc->vars[i], x->neg ? "-" : "", u);
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
    // is the stored 10); with `a[i] = -2147483648` between, a[0] may be that value (a fresh version,
    // 10 or the value stored, c26) and its narrow.lo is not implied
    for (int c = 0; c < 2; c++) {
        IrType *ai32 = ir_type_int(&A, 32, true), *u64e = ir_type_int(&A, 64, false);
        IrType *arr = ir_type_new(&A, IRT_ARRAY); arr->elem = ai32; arr->array_len = 4;
        IrFunc *ef = ir_func_new(&A, nm("el"), ai32, IR_FUNC_PURE);
        IrValue *ei = ir_add_param(ef, u64e, nm("i"));
        IrBlock *e0 = ef->entry;
        IrValue *ea = ir_alloca_array(ef, e0, arr);
        for (int k = 0; k < 4; k++)
            ir_store(ef, e0, ir_elem_ptr(ef, e0, ea, ir_const_int(ef, e0, k, u64e), ai32), ir_const_int(ef, e0, 10 * (k + 1), ai32));
        if (c) ir_store(ef, e0, ir_elem_ptr(ef, e0, ea, ei, ai32), ir_const_int(ef, e0, -2147483648LL, ai32));
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

    // a PRODUCT of two variables, `y *? x`'s shape: cast(y i32):i64 * cast(x i32):i64, its narrowing
    // in i64 proved from the corners of the operands' i32 ranges
    {   IrType *mi32 = ir_type_int(&A, 32, true), *mi64 = ir_type_int(&A, 64, true);
        IrFunc *mf = ir_func_new(&A, nm("mq"), mi64, IR_FUNC_PURE);
        IrValue *mx = ir_add_param(mf, mi32, nm("x")), *my = ir_add_param(mf, mi32, nm("y"));
        IrBlock *mb = mf->entry;
        IrInstr *cx = ir_instr(mf, IR_CAST, mi64, 1); cx->operands[0] = mx; ir_emit(mb, cx);
        IrInstr *cy = ir_instr(mf, IR_CAST, mi64, 1); cy->operands[0] = my; ir_emit(mb, cy);
        ir_set_ret(mb, ir_binop(mf, mb, IR_MUL, cx->result, cy->result, mi64));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, "proved bb0.2 narrow.lo") && strstr(recs, "proved bb0.2 narrow.hi") &&
              nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] == 0, "y *? x's product: its narrowing in i64 proved:\n%s", recs);
    }

    // a LOAD by a variable index from a table of cells: `t = [31, 28, 31, 30]; 100 / (t[m] - 27)`
    // proves its divisor nonzero (the value is one of the four); with 27 in the table it is not
    for (int c = 0; c < 2; c++) {
        IrType *ti32 = ir_type_int(&A, 32, true), *tu64 = ir_type_int(&A, 64, false);
        IrType *tarr = ir_type_new(&A, IRT_ARRAY); tarr->elem = ti32; tarr->array_len = 4;
        IrFunc *tf = ir_func_new(&A, nm("tab"), ti32, IR_FUNC_PURE);
        IrValue *tm = ir_add_param(tf, tu64, nm("m"));
        IrBlock *t0 = tf->entry;
        ir_assume(tf, t0, ir_icmp(tf, t0, IR_CMP_ULT, tm, ir_const_int(tf, t0, 4, tu64)));
        IrValue *ta = ir_alloca_array(tf, t0, tarr);
        int vals[4] = { 31, c ? 27 : 28, 31, 30 };
        for (int k = 0; k < 4; k++)
            ir_store(tf, t0, ir_elem_ptr(tf, t0, ta, ir_const_int(tf, t0, k, tu64), ti32), ir_const_int(tf, t0, vals[k], ti32));
        IrValue *tv = ir_load(tf, t0, ir_elem_ptr(tf, t0, ta, tm, ti32), ti32);
        IrValue *td = ir_binop(tf, t0, IR_SUB, tv, ir_const_int(tf, t0, 27, ti32), ti32);
        ir_set_ret(t0, ir_binop(tf, t0, IR_SDIV, ir_const_int(tf, t0, 100, ti32), td, ti32));
        ir_finalize_cfg(tf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(tf, &s, true, sizeof region, err), "judges: %s", err);
        const char *dz = strstr(recs, " divzero"), *ln = dz;    // its record line's first word
        while (ln && ln > recs && ln[-1] != '\n') ln--;
        CHECK(ln && !strncmp(ln, c ? "not-implied " : "proved ", c ? 12 : 7),
              "table read, case %d: the divisor %s:\n%s", c, c ? "may be 0" : "is nonzero", recs);
    }

    // a WAIVED narrowing wraps exactly: y = 200 (i16), x = y as i8 unchecked is -56, so a[x + 60]
    // in an i32[5] is a[4], in bounds; without the wrap row x is any i8
    {   IrType *wi16 = ir_type_int(&A, 16, true), *wi8 = ir_type_int(&A, 8, true), *wi32 = ir_type_int(&A, 32, true), *wu64 = ir_type_int(&A, 64, false);
        IrType *warr = ir_type_new(&A, IRT_ARRAY); warr->elem = wi32; warr->array_len = 5;
        IrFunc *wf = ir_func_new(&A, nm("wrapped"), wi32, IR_FUNC_PURE);
        IrBlock *w0 = wf->entry;
        IrValue *wa = ir_alloca_array(wf, w0, warr);
        for (int k = 0; k < 5; k++) ir_store(wf, w0, ir_elem_ptr(wf, w0, wa, ir_const_int(wf, w0, k, wu64), wi32), ir_const_int(wf, w0, k, wi32));
        IrValue *wy = ir_alloca(wf, w0, wi16);
        ir_store(wf, w0, wy, ir_const_int(wf, w0, 200, wi16));
        IrInstr *wc = ir_instr(wf, IR_CAST, wi8, 1); wc->operands[0] = ir_load(wf, w0, wy, wi16); wc->unchecked = true; ir_emit(w0, wc);
        IrInstr *ww = ir_instr(wf, IR_CAST, wi32, 1); ww->operands[0] = wc->result; ir_emit(w0, ww);
        IrValue *wsum = ir_binop(wf, w0, IR_ADD, ww->result, ir_const_int(wf, w0, 60, wi32), wi32);
        IrInstr *wx = ir_instr(wf, IR_CAST, wu64, 1); wx->operands[0] = wsum; ir_emit(w0, wx);
        ir_set_ret(w0, ir_load(wf, w0, ir_elem_ptr(wf, w0, wa, wx->result, wi32), wi32));
        ir_finalize_cfg(wf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(wf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] == 0 && strstr(recs, "bounds.hi"), "(200 as i8) + 60 indexes a[4]:\n%s", recs);
    }

    // a `!=` INSIDE a join's disjunct: `if x != 0 { r = x } else { r = 1 }; return 100 / r`: the
    // r = x path holds r != 0 only if the arm's `!=` is stated in its disjunct (split in two)
    {   IrType *ni32 = ir_type_int(&A, 32, true);
        IrFunc *nf = ir_func_new(&A, nm("nej"), ni32, IR_FUNC_PURE);
        IrValue *nx = ir_add_param(nf, ni32, nm("x"));
        IrBlock *n0 = nf->entry, *n1 = ir_new_block(nf), *n2 = ir_new_block(nf), *n3 = ir_new_block(nf);
        IrValue *nr = ir_alloca(nf, n0, ni32);
        ir_set_br_cond(n0, ir_icmp(nf, n0, IR_CMP_NE, nx, ir_const_int(nf, n0, 0, ni32)), n1, n2);
        ir_store(nf, n1, nr, nx); ir_set_br(n1, n3);
        ir_store(nf, n2, nr, ir_const_int(nf, n2, 1, ni32)); ir_set_br(n2, n3);
        ir_set_ret(n3, ir_binop(nf, n3, IR_SDIV, ir_const_int(nf, n3, 100, ni32), ir_load(nf, n3, nr, ni32), ni32));
        ir_finalize_cfg(nf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(nf, &s, true, sizeof region, err), "judges: %s", err);
        const char *dz = strstr(recs, " divzero"), *ln = dz;
        while (ln && ln > recs && ln[-1] != '\n') ln--;
        CHECK(ln && !strncmp(ln, "proved ", 7), "the r = x path's x != 0 proves 100 / r:\n%s", recs);
    }

    // a goal of CONSTANTS that is false holds only where its point is unreachable: `r = x << k` with
    // x in [-3, 5] and k in [0, 3]; `if r > 40 { 5 / 0 }` is dead, so its divzero is proved; and a
    // true one (`x / 4`) pulls in no fact at all
    {   IrType *si8 = ir_type_int(&A, 8, true), *su8 = ir_type_int(&A, 8, false);
        IrFunc *df = ir_func_new(&A, nm("dead"), si8, IR_FUNC_PURE);
        IrValue *dx = ir_add_param(df, si8, nm("x")), *dk = ir_add_param(df, su8, nm("k"));
        IrBlock *d0 = df->entry, *d1 = ir_new_block(df), *d2 = ir_new_block(df);
        ir_assume(df, d0, ir_icmp(df, d0, IR_CMP_SGE, dx, ir_const_int(df, d0, -3, si8)));
        ir_assume(df, d0, ir_icmp(df, d0, IR_CMP_SLE, dx, ir_const_int(df, d0, 5, si8)));
        ir_assume(df, d0, ir_icmp(df, d0, IR_CMP_ULE, dk, ir_const_int(df, d0, 3, su8)));
        IrInstr *ck = ir_instr(df, IR_CAST, si8, 1); ck->operands[0] = dk; ir_emit(d0, ck);
        IrValue *dr = ir_binop(df, d0, IR_SHL, dx, ck->result, si8);
        ir_set_br_cond(d0, ir_icmp(df, d0, IR_CMP_SGT, dr, ir_const_int(df, d0, 40, si8)), d1, d2);
        ir_set_ret(d1, ir_binop(df, d1, IR_SDIV, ir_const_int(df, d1, 5, si8), ir_const_int(df, d1, 0, si8), si8));
        ir_set_ret(d2, ir_binop(df, d2, IR_SDIV, dr, ir_const_int(df, d2, 4, si8), si8));
        ir_finalize_cfg(df);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(df, &s, true, sizeof region, err), "judges: %s", err);
        char want[48]; snprintf(want, sizeof want, "proved bb%d.", d1->id);
        CHECK(strstr(recs, want) && strstr(strstr(recs, want), "divzero") && nrec[JREC_NOT_IMPLIED] == 0,
              "the dead branch's 5 / 0 is never reached:\n%s", recs);
    }

    // a constant TABLE read by a variable index: `T = [4, 5, 6]; 100 / (T[i] - 3)` proved; with a 3
    for (int c = 0; c < 2; c++) {
        IrType *ti = ir_type_int(&A, 32, true), *tu = ir_type_int(&A, 64, false);
        IrType *TA = ir_type_new(&A, IRT_ARRAY); TA->elem = ti; TA->array_len = 3;
        IrData *td = arena_push_aligned(&A, IrData); memset(td, 0, sizeof *td);
        td->type = TA; td->n = 3; td->vals = arena_push_many_aligned(&A, int64_t, 3);
        td->vals[0] = 4; td->vals[1] = c ? 3 : 5; td->vals[2] = 6;
        IrFunc *tf = ir_func_new(&A, nm("tab3"), ti, IR_FUNC_PURE);
        IrValue *ti_ = ir_add_param(tf, tu, nm("i"));
        IrBlock *t0 = tf->entry;
        ir_assume(tf, t0, ir_icmp(tf, t0, IR_CMP_ULT, ti_, ir_const_int(tf, t0, 3, tu)));
        IrValue *tb = ir_alloca_array(tf, t0, TA);
        { IrInstr *l = t0->instrs; while (l->next) l = l->next; l->data = td; }
        IrValue *tv = ir_load(tf, t0, ir_elem_ptr(tf, t0, tb, ti_, ti), ti);
        ir_set_ret(t0, ir_binop(tf, t0, IR_SDIV, ir_const_int(tf, t0, 100, ti), ir_binop(tf, t0, IR_SUB, tv, ir_const_int(tf, t0, 3, ti), ti), ti));
        ir_finalize_cfg(tf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(tf, &s, true, sizeof region, err), "judges: %s", err);
        const char *dz = strstr(recs, " divzero"), *ln = dz;
        while (ln && ln > recs && ln[-1] != '\n') ln--;
        CHECK(ln && !strncmp(ln, c ? "not-implied " : "proved ", c ? 12 : 7), "table, case %d:\n%s", c, recs);
    }

    // a remainder below its divisor: `if s.len > 0 { return s[i % s.len] }` proves its bound
    {   IrType *mu8 = ir_type_int(&A, 8, false), *mu64 = ir_type_int(&A, 64, false);
        IrType *msl = ir_type_new(&A, IRT_SLICE); msl->elem = mu8;
        IrFunc *mf = ir_func_new(&A, nm("modix"), mu8, IR_FUNC_PURE);
        IrValue *ms = ir_add_param(mf, msl, nm("s")), *mi = ir_add_param(mf, mu64, nm("i"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
        IrValue *ml = ir_slice_len(mf, m0, ms);
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_UGT, ml, ir_const_int(mf, m0, 0, mu64)), m1, m2);
        IrValue *ml1 = ir_slice_len(mf, m1, ms);
        IrValue *mx = ir_binop(mf, m1, IR_UREM, mi, ml1, mu64);
        ir_set_ret(m1, ir_load(mf, m1, ir_elem_ptr(mf, m1, ir_slice_data(mf, m1, ms, mu8), mx, mu8), mu8));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, mu8));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, "proved bb1.3 bounds.hi") && nrec[JREC_NOT_IMPLIED] == 0, "s[i %% s.len] in bounds:\n%s", recs);
    }

    // the lowest set bit cleared: `if x > 0 { r = x & (x - 1); 100 / (x - r) }` over i32, where
    // x - 1 may be negative by its type alone
    {   IrType *mi32 = ir_type_int(&A, 32, true);
        IrFunc *mf = ir_func_new(&A, nm("lowbit"), mi32, IR_FUNC_PURE);
        IrValue *mx = ir_add_param(mf, mi32, nm("x"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_SGT, mx, ir_const_int(mf, m0, 0, mi32)), m1, m2);
        IrValue *ms = ir_binop(mf, m1, IR_SUB, mx, ir_const_int(mf, m1, 1, mi32), mi32);
        IrValue *mr = ir_binop(mf, m1, IR_AND, mx, ms, mi32);
        IrValue *md = ir_binop(mf, m1, IR_SUB, mx, mr, mi32);
        ir_set_ret(m1, ir_binop(mf, m1, IR_SDIV, ir_const_int(mf, m1, 100, mi32), md, mi32));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, mi32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, " divzero") && !strstr(recs, "divzero not") && nrec[JREC_NOT_IMPLIED] == 0, "x - (x & (x - 1)) nonzero:\n%s", recs);
    }

    // a quotient's guard and a remainder below its dividend, over u64: `if i < n / 2 { 100 / (n - 1) }`
    // (n >= 2) and `x - x % y`
    for (int c = 0; c < 2; c++) {
        IrType *mu64 = ir_type_int(&A, 64, false);
        IrFunc *mf = ir_func_new(&A, nm(c ? "rounddown" : "halfway"), mu64, IR_FUNC_PURE);
        IrValue *mx = ir_add_param(mf, mu64, nm("x")), *my = ir_add_param(mf, mu64, nm("y"));
        IrBlock *m0 = mf->entry;
        char want[48] = "proved bb1.3 divzero";
        if (c) {
            ir_assume(mf, m0, ir_icmp(mf, m0, IR_CMP_UGE, my, ir_const_int(mf, m0, 1, mu64)));
            IrValue *md = ir_binop(mf, m0, IR_SUB, mx, ir_binop(mf, m0, IR_UREM, mx, my, mu64), mu64);
            int at = 0;
            for (IrInstr *q = m0->instrs; q && q->result != md; q = q->next) at++;
            snprintf(want, sizeof want, "proved bb0.%d narrow.lo", at);
            ir_set_ret(m0, md);
        } else {
            IrBlock *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
            IrValue *mq = ir_binop(mf, m0, IR_UDIV, mx, ir_const_int(mf, m0, 2, mu64), mu64);
            ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_ULT, my, mq), m1, m2);
            ir_set_ret(m1, ir_binop(mf, m1, IR_UDIV, ir_const_int(mf, m1, 100, mu64), ir_binop(mf, m1, IR_SUB, mx, ir_const_int(mf, m1, 1, mu64), mu64), mu64));
            ir_set_ret(m2, ir_const_int(mf, m2, 0, mu64));
        }
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, want) && nrec[JREC_NOT_IMPLIED] == 0,
              "%s:\n%s", c ? "x - x % y" : "100 / (n - 1) under i < n / 2", recs);
    }

    // a bit count and its operand, over u32: `if m != 0 { 100 / popcount(m) }`, and `if m != 0 { if
    // m <= 65535 { 100 / (16 - ctz(m)) } }` (ctz(m) <= 15)
    for (int c = 0; c < 2; c++) {
        IrType *mu32 = ir_type_int(&A, 32, false);
        IrFunc *mf = ir_func_new(&A, nm(c ? "lane" : "share"), mu32, IR_FUNC_PURE);
        IrValue *mm = ir_add_param(mf, mu32, nm("m"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf), *m3 = c ? ir_new_block(mf) : m1;
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_NE, mm, ir_const_int(mf, m0, 0, mu32)), m1, m2);
        if (c) ir_set_br_cond(m1, ir_icmp(mf, m1, IR_CMP_ULE, mm, ir_const_int(mf, m1, 65535, mu32)), m3, m2);
        IrValue *mp = ir_bitcount(mf, m3, c ? IR_CTZ : IR_POPCOUNT, mm, mu32);
        IrValue *md = c ? ir_binop(mf, m3, IR_SUB, ir_const_int(mf, m3, 16, mu32), mp, mu32) : mp;
        ir_set_ret(m3, ir_binop(mf, m3, IR_UDIV, ir_const_int(mf, m3, 100, mu32), md, mu32));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, mu32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, " divzero") && !strstr(recs, "not-implied") && nrec[JREC_NOT_IMPLIED] == 0 &&
              (!c || strstr(recs, "proved bb3.2 narrow.lo")), "%s:\n%s", c ? "16 - ctz(m), m in [1, 65535]" : "100 / popcount(m), m != 0", recs);
    }

    // a subslice's elements are the array's: xs = [10, 20, 30, 40, 50], s = xs[1..4] (stored once).
    // (0) 100 / (s[0] + s[2] - 59); (1) s[1] = 7 then 100 / (xs[2] - 6): both proved. (2) `if i < 3 {
    // s[i] = 0; 100 / xs[3] }`: s[i] may be xs[3], so the divisor is NOT shown nonzero
    for (int c = 0; c < 3; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true), *mu64 = ir_type_int(&A, 64, false);
        IrType *ma5 = ir_type_new(&A, IRT_ARRAY); ma5->elem = mi32; ma5->array_len = 5;
        IrType *msl = ir_type_new(&A, IRT_SLICE); msl->elem = mi32;
        IrFunc *mf = ir_func_new(&A, nm(c == 0 ? "subsum" : c == 1 ? "subwrite" : "subany"), mi32, IR_FUNC_PURE);
        IrValue *mi = ir_add_param(mf, mu64, nm("i"));
        IrBlock *m0 = mf->entry, *mb = m0, *m2 = NULL;
        IrValue *xs = ir_alloca_array(mf, m0, ma5);
        for (int k = 0; k < 5; k++)
            ir_store(mf, m0, ir_elem_ptr(mf, m0, xs, ir_const_int(mf, m0, k, mu64), mi32), ir_const_int(mf, m0, 10 * (k + 1), mi32));
        IrInstr *mk = ir_instr(mf, IR_MAKE_SLICE, msl, 2);
        mk->operands[0] = ir_elem_ptr(mf, m0, xs, ir_const_int(mf, m0, 1, mu64), mi32); mk->operands[1] = ir_const_int(mf, m0, 3, mu64);
        ir_emit(m0, mk);
        IrValue *slot = ir_alloca(mf, m0, msl);
        ir_store(mf, m0, slot, mk->result);
        if (c == 2) {
            mb = ir_new_block(mf); m2 = ir_new_block(mf);
            ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_ULT, mi, ir_const_int(mf, m0, 3, mu64)), mb, m2);
            ir_set_ret(m2, ir_const_int(mf, m2, 0, mi32));
        }
        IrValue *d = ir_slice_data(mf, mb, ir_load(mf, mb, slot, msl), mi32), *dv;
        if (c == 0) {
            IrValue *s0 = ir_load(mf, mb, ir_elem_ptr(mf, mb, d, ir_const_int(mf, mb, 0, mu64), mi32), mi32);
            IrValue *s2 = ir_load(mf, mb, ir_elem_ptr(mf, mb, d, ir_const_int(mf, mb, 2, mu64), mi32), mi32);
            dv = ir_binop(mf, mb, IR_SUB, ir_binop(mf, mb, IR_ADD, s0, s2, mi32), ir_const_int(mf, mb, 59, mi32), mi32);
        } else {
            ir_store(mf, mb, ir_elem_ptr(mf, mb, d, c == 1 ? ir_const_int(mf, mb, 1, mu64) : mi, mi32), ir_const_int(mf, mb, c == 1 ? 7 : 0, mi32));
            dv = ir_load(mf, mb, ir_elem_ptr(mf, mb, xs, ir_const_int(mf, mb, c == 1 ? 2 : 3, mu64), mi32), mi32);
            if (c == 1) dv = ir_binop(mf, mb, IR_SUB, dv, ir_const_int(mf, mb, 6, mi32), mi32);
        }
        ir_set_ret(mb, ir_binop(mf, mb, IR_SDIV, ir_const_int(mf, mb, 100, mi32), dv, mi32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        if (c < 2) CHECK(nrec[JREC_NOT_IMPLIED] == 0 && strstr(recs, " divzero"), "%s:\n%s", c ? "s[1] = 7 is xs[2]" : "s[0] + s[2] is xs[1] + xs[3]", recs);
        else CHECK(strstr(recs, "not-implied bb1.") && strstr(strstr(recs, "not-implied bb1."), "divzero"), "s[i] = 0 may write xs[3]:\n%s", recs);
    }

    // an assert of an unsigned comparison with a signed operand >= 0: `if pos < len { assert pos + 1
    // <= len }`, pos a u32, len a u64, pos + 1 in i34
    {   IrType *mu32 = ir_type_int(&A, 32, false), *mu64 = ir_type_int(&A, 64, false), *mi34 = ir_type_int(&A, 34, true);
        IrFunc *mf = ir_func_new(&A, nm("advance"), mu32, IR_FUNC_PURE);
        IrValue *mp = ir_add_param(mf, mu32, nm("pos")), *ml = ir_add_param(mf, mu64, nm("len"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_ULT, mp, ml), m1, m2);
        IrValue *mx = ir_binop(mf, m1, IR_ADD, mp, ir_const_int(mf, m1, 1, ir_type_int(&A, 32, true)), mi34);
        ir_assert(mf, m1, ir_icmp(mf, m1, IR_CMP_ULE, mx, ml));
        ir_set_ret(m1, ir_const_int(mf, m1, 1, mu32));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, mu32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, "proved bb1.") && strstr(recs, "assert.0") && nrec[JREC_NOT_IMPLIED] == 0, "assert pos + 1 <= len:\n%s", recs);
    }

    // a product of two loaded slots, stored 10 and 3, in i32: its narrowing owed (the corners
    // 30, 30 lie in i32) and proved, and 100 / (p - 29) too
    {   IrType *mi32 = ir_type_int(&A, 32, true);
        IrFunc *mf = ir_func_new(&A, nm("slots"), mi32, IR_FUNC_PURE);
        IrBlock *m0 = mf->entry;
        IrValue *sa = ir_alloca(mf, m0, mi32), *sb = ir_alloca(mf, m0, mi32);
        ir_store(mf, m0, sa, ir_const_int(mf, m0, 10, mi32)); ir_store(mf, m0, sb, ir_const_int(mf, m0, 3, mi32));
        IrValue *mp = ir_binop(mf, m0, IR_MUL, ir_load(mf, m0, sa, mi32), ir_load(mf, m0, sb, mi32), mi32);
        ir_set_ret(m0, ir_binop(mf, m0, IR_SDIV, ir_const_int(mf, m0, 100, mi32), ir_binop(mf, m0, IR_SUB, mp, ir_const_int(mf, m0, 29, mi32), mi32), mi32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, " divzero") && !strstr(recs, "outside") && nrec[JREC_NOT_IMPLIED] == 0, "10 * 3 from two slots:\n%s", recs);
    }

    // a written measure with a quotient fixed at the header: `while i < n / 2 decreasing n / 2 - i`
    {   IrFunc *qm = ir_func_new(&A, nm("halfm"), u32t, IR_FUNC_PURE);
        IrValue *qn = ir_add_param(qm, u32t, nm("n"));
        IrBlock *q0 = qm->entry, *q1 = ir_new_block(qm), *q2 = ir_new_block(qm), *q3 = ir_new_block(qm);
        IrValue *qi = ir_alloca(qm, q0, u32t);
        ir_store(qm, q0, qi, ir_const_int(qm, q0, 0, u32t)); ir_set_br(q0, q1);
        IrValue *qh = ir_binop(qm, q1, IR_UDIV, qn, ir_const_int(qm, q1, 2, u32t), u32t);
        ir_set_br_cond(q1, ir_icmp(qm, q1, IR_CMP_ULT, ir_load(qm, q1, qi, u32t), qh), q2, q3);
        IrValue *qli = ir_load(qm, q2, qi, u32t);
        IrValue *qms = ir_binop(qm, q2, IR_SUB, qh, qli, u32t);
        ir_store(qm, q2, qi, ir_binop(qm, q2, IR_ADD, qli, ir_const_int(qm, q2, 1, u32t), u32t));
        ir_set_br(q2, q1);
        ir_set_ret(q3, ir_const_int(qm, q3, 0, u32t));
        ir_finalize_cfg(qm);
        q1->has_measure = true; q1->measure_val = qms;
        int qp = 0;
        for (IrInstr *i = q2->instrs; i; i = i->next) qp++;
        s.propose = P_NONE; s.nrank = 0;
        snprintf(rf, sizeof rf, "proved bb%d.%d measure.fall", q2->id, qp); snprintf(rb, sizeof rb, "proved bb%d.%d measure.bound", q2->id, qp);
        CHECK(run(qm, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, rf) && strstr(recs, rb) && !strstr(recs, "outside"), "decreasing n / 2 - i: proved:\n%s", recs);
    }

    // a float converted to an i16 (P3.3, F2): (0) on the TRUE edges of x >= -32768.0 and x < 32768.0,
    // both narrowings proved; (1) on the FALSE edges of x < -32768.0 and x >= 32768.0, which a NaN
    // passes (float_guard_lets_nan_through_fail), neither
    for (int c = 0; c < 2; c++) {
        IrType *mf64 = ir_type_new(&A, IRT_FLOAT), *mi16 = ir_type_int(&A, 16, true);
        mf64->float_bits = 64;
        IrFunc *mf = ir_func_new(&A, nm(c ? "nanpass" : "bias"), mi16, IR_FUNC_PURE);
        IrValue *mx = ir_add_param(mf, mf64, nm("x"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf), *mz = ir_new_block(mf);
        IrValue *lo = ir_icmp(mf, m0, c ? IR_CMP_SLT : IR_CMP_SGE, mx, ir_const_float(mf, m0, -32768.0, mf64));
        if (c) ir_set_br_cond(m0, lo, mz, m1); else ir_set_br_cond(m0, lo, m1, mz);
        IrValue *hi = ir_icmp(mf, m1, c ? IR_CMP_SGE : IR_CMP_SLT, mx, ir_const_float(mf, m1, 32768.0, mf64));
        if (c) ir_set_br_cond(m1, hi, mz, m2); else ir_set_br_cond(m1, hi, m2, mz);
        IrInstr *cv = ir_instr(mf, IR_CAST, mi16, 1); cv->operands[0] = mx; ir_emit(m2, cv);
        ir_set_ret(m2, cv->result);
        ir_set_ret(mz, ir_const_int(mf, mz, 0, mi16));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        if (!c) CHECK(strstr(recs, "proved bb2.0 narrow.lo") && strstr(recs, "proved bb2.0 narrow.hi") && nrec[JREC_NOT_IMPLIED] == 0,
                      "x as i16 on the true edges:\n%s", recs);
        else CHECK(strstr(recs, "not-implied bb2.0 narrow.lo") && strstr(recs, "not-implied bb2.0 narrow.hi"), "a NaN passes the false edges:\n%s", recs);
    }

    // a product's hull at its point: `if i < h { return i * w }` over u32 with h, w <= 4095 by
    // refinement and i unbounded: the corners 4094 * 4095 fit, the narrowing owed and proved
    {   IrType *mu32 = ir_type_int(&A, 32, false);
        IrFunc *mf = ir_func_new(&A, nm("rowoff"), mu32, IR_FUNC_PURE);
        IrValue *mi = ir_add_param(mf, mu32, nm("i")), *mh = ir_add_param(mf, mu32, nm("h")), *mw = ir_add_param(mf, mu32, nm("w"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
        ir_assume(mf, m0, ir_icmp(mf, m0, IR_CMP_ULE, mh, ir_const_int(mf, m0, 4095, mu32)));
        ir_assume(mf, m0, ir_icmp(mf, m0, IR_CMP_ULE, mw, ir_const_int(mf, m0, 4095, mu32)));
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_ULT, mi, mh), m1, m2);
        ir_set_ret(m1, ir_binop(mf, m1, IR_MUL, mi, mw, mu32));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, mu32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, "proved bb1.0 narrow.hi") && !strstr(recs, "outside") && nrec[JREC_NOT_IMPLIED] == 0, "i * w under i < h:\n%s", recs);
    }

    // a store by a variable index, element by element (c26), under `if i < 4`: (0) a = [10, 20, 30,
    // 40], a[i] = 7, then 100 / a[2]: a[2] is 7 or 30; (1) a = [6, 6, 6, 6], a[i] = 7, then
    // 100 / (a[i] - 6): the element the index names is the 7 just stored; (2) under `if i < 2`,
    // a = [10, 20, 30, 40], a[i] = 0, then 100 / a[2]: i is not 2, so a[2] kept its 30. All proved
    for (int c = 0; c < 3; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true), *mu64 = ir_type_int(&A, 64, false);
        IrType *ma4 = ir_type_new(&A, IRT_ARRAY); ma4->elem = mi32; ma4->array_len = 4;
        IrFunc *mf = ir_func_new(&A, nm(c == 2 ? "storekept" : c ? "storeread" : "storeother"), mi32, IR_FUNC_PURE);
        IrValue *mi = ir_add_param(mf, mu64, nm("i"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
        IrValue *ma = ir_alloca_array(mf, m0, ma4);
        for (int k = 0; k < 4; k++)
            ir_store(mf, m0, ir_elem_ptr(mf, m0, ma, ir_const_int(mf, m0, k, mu64), mi32), ir_const_int(mf, m0, c == 1 ? 6 : 10 * (k + 1), mi32));
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_ULT, mi, ir_const_int(mf, m0, c == 2 ? 2 : 4, mu64)), m1, m2);
        ir_store(mf, m1, ir_elem_ptr(mf, m1, ma, mi, mi32), ir_const_int(mf, m1, c == 2 ? 0 : 7, mi32));
        IrValue *mr = ir_load(mf, m1, ir_elem_ptr(mf, m1, ma, c == 1 ? mi : ir_const_int(mf, m1, 2, mu64), mi32), mi32);
        if (c == 1) mr = ir_binop(mf, m1, IR_SUB, mr, ir_const_int(mf, m1, 6, mi32), mi32);
        ir_set_ret(m1, ir_binop(mf, m1, IR_SDIV, ir_const_int(mf, m1, 100, mi32), mr, mi32));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, mi32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, " divzero") && !strstr(recs, "divzero  %") && nrec[JREC_NOT_IMPLIED] == 0, "%s:\n%s", c == 2 ? "a[2] kept its 30" : c ? "a[i] read back is 7" : "a[2] is 7 or 30", recs);
    }

    // an EACH claim (c27) over a fill loop: a u8[4] stored 0, `while i < 4 { a[i] = 10; i = i + 1 }`,
    // then 100 / (11 - a[2]). Proposed at the header: each(a) <= 10. Its 4 inv.entry and 4 inv.keep
    // VCs proved (a store keeps or writes 10, c26), and the division after the loop; (1) with a[3]
    // stored 20 before the loop, element 3's inv.entry (ordinal 3) alone not implied
    for (int c = 0; c < 2; c++) {
        IrType *mu8 = ir_type_int(&A, 8, false), *mu64 = ir_type_int(&A, 64, false), *mi32 = ir_type_int(&A, 32, true);
        IrType *ma4 = ir_type_new(&A, IRT_ARRAY); ma4->elem = mu8; ma4->array_len = 4;
        IrFunc *mf = ir_func_new(&A, nm("fill"), mi32, IR_FUNC_PURE);
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf), *m3 = ir_new_block(mf);
        IrValue *ma = ir_alloca_array(mf, m0, ma4), *mi = ir_alloca(mf, m0, mu64);
        for (int k = 0; k < 4; k++) ir_store(mf, m0, ir_elem_ptr(mf, m0, ma, ir_const_int(mf, m0, k, mu64), mu8), ir_const_int(mf, m0, c && k == 3 ? 20 : 0, mu8));
        ir_store(mf, m0, mi, ir_const_int(mf, m0, 0, mu64)); ir_set_br(m0, m1);
        ir_set_br_cond(m1, ir_icmp(mf, m1, IR_CMP_ULT, ir_load(mf, m1, mi, mu64), ir_const_int(mf, m1, 4, mu64)), m2, m3);
        IrValue *mli = ir_load(mf, m2, mi, mu64);
        ir_store(mf, m2, ir_elem_ptr(mf, m2, ma, mli, mu8), ir_const_int(mf, m2, 10, mu8));
        ir_store(mf, m2, mi, ir_binop(mf, m2, IR_ADD, mli, ir_const_int(mf, m2, 1, mu64), mu64)); ir_set_br(m2, m1);
        IrValue *ma2 = ir_load(mf, m3, ir_elem_ptr(mf, m3, ma, ir_const_int(mf, m3, 2, mu64), mu8), mu8);
        ir_set_ret(m3, ir_binop(mf, m3, IR_SDIV, ir_const_int(mf, m3, 100, mi32), ir_binop(mf, m3, IR_SUB, ir_const_int(mf, m3, 11, mi32), ma2, mi32), mi32));
        ir_finalize_cfg(mf);
        memset(&s.claim, 0, sizeof s.claim);
        s.claim.blk = m1->id; s.claim.fn = mf; s.claim.nt = 1; s.claim.key[0] = ma->id; s.claim.tk[0] = JTERM_EACH; s.claim.coef[0] = 1; s.claim.rhs = 10;
        s.propose = P_CLAIM; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        if (!c) CHECK(count_in(recs, "inv.keep") == 4 && count_in(recs, "inv.entry") == 4 &&
                      !strstr(recs, "not-implied") && strstr(recs, "divzero") && !strstr(recs, "divzero  "),
                      "each(a) <= 10 over the fill loop, and the division after it:\n%s", recs);
        else CHECK(count_in(recs, "not-implied") == 1 && strstr(recs, "inv.entry 3 "), "a[3] = 20 breaks element 3's inv.entry alone:\n%s", recs);
        s.propose = P_NONE;
    }

    // a GUARDED EACH claim (c28) over a fill loop of an uninitialized u8[8]: `while i < 8 { a[i] = 7;
    // i = i + 1 }`, then 100 / (a[7] - 6). Proposed at the header: (0) -each(a) <= -7 upto(i), `for
    // each k < i, a[k] >= 7`: its inv.entry and inv.keep and the division after the loop proved; (1)
    // -each(a) <= -8 upto(i), which a[i] = 7 breaks: inv.keep not implied
    for (int c = 0; c < 2; c++) {
        IrType *mu8 = ir_type_int(&A, 8, false), *mu64 = ir_type_int(&A, 64, false), *mi32 = ir_type_int(&A, 32, true);
        IrType *ma8 = ir_type_new(&A, IRT_ARRAY); ma8->elem = mu8; ma8->array_len = 8;
        IrFunc *mf = ir_func_new(&A, nm(c ? "prefixbad" : "prefix"), mi32, IR_FUNC_PURE);
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf), *m3 = ir_new_block(mf);
        IrValue *ma = ir_alloca_array(mf, m0, ma8), *mi = ir_alloca(mf, m0, mu64);
        ir_store(mf, m0, mi, ir_const_int(mf, m0, 0, mu64)); ir_set_br(m0, m1);
        ir_set_br_cond(m1, ir_icmp(mf, m1, IR_CMP_ULT, ir_load(mf, m1, mi, mu64), ir_const_int(mf, m1, 8, mu64)), m2, m3);
        IrValue *mli = ir_load(mf, m2, mi, mu64);
        ir_store(mf, m2, ir_elem_ptr(mf, m2, ma, mli, mu8), ir_const_int(mf, m2, 7, mu8));
        ir_store(mf, m2, mi, ir_binop(mf, m2, IR_ADD, mli, ir_const_int(mf, m2, 1, mu64), mu64)); ir_set_br(m2, m1);
        IrValue *ma7 = ir_load(mf, m3, ir_elem_ptr(mf, m3, ma, ir_const_int(mf, m3, 7, mu64), mu8), mu8);
        ir_set_ret(m3, ir_binop(mf, m3, IR_SDIV, ir_const_int(mf, m3, 100, mi32), ir_binop(mf, m3, IR_SUB, ma7, ir_const_int(mf, m3, 6, mi32), mi32), mi32));
        ir_finalize_cfg(mf);
        memset(&s.claim, 0, sizeof s.claim);
        s.claim.blk = m1->id; s.claim.fn = mf; s.claim.nt = 2; s.claim.key[0] = ma->id; s.claim.tk[0] = JTERM_EACH; s.claim.coef[0] = -1;
        s.claim.key[1] = mi->id; s.claim.tk[1] = JTERM_UPTO; s.claim.coef[1] = 1; s.claim.rhs = c ? -8 : -7;
        s.propose = P_CLAIM; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        if (!c) CHECK(count_in(recs, "inv.entry") == 8 && count_in(recs, "inv.keep") == 8 && !strstr(recs, "not-implied") &&
                      strstr(recs, "proved bb3.6 divzero"), "for each k < i, a[k] >= 7, and a[7] - 6 after:\n%s", recs);
        else CHECK(strstr(recs, "not-implied bb2.7 inv.keep"), "a[i] = 7 breaks a[k] >= 8:\n%s", recs);   // every element's
        s.propose = P_NONE;
    }

    // an array marked initialized (`init`, as a comprehension's fill ends) survives a call to an
    // unknown function: a = [10, 20, 30, 40], init a, unknown(), then 100 / a[2], proved (c29)
    {   IrType *mi32 = ir_type_int(&A, 32, true), *mu64 = ir_type_int(&A, 64, false), *mu32 = ir_type_int(&A, 32, false);
        IrType *ma4 = ir_type_new(&A, IRT_ARRAY); ma4->elem = mi32; ma4->array_len = 4;
        IrFunc *mf = ir_func_new(&A, nm("initcall"), mi32, IR_FUNC_PURE);
        IrBlock *m0 = mf->entry;
        IrValue *ma = ir_alloca_array(mf, m0, ma4);
        for (int k = 0; k < 4; k++) ir_store(mf, m0, ir_elem_ptr(mf, m0, ma, ir_const_int(mf, m0, k, mu64), mi32), ir_const_int(mf, m0, 10 * (k + 1), mi32));
        ir_init_fact(mf, m0, ma);
        IrInstr *call = ir_instr(mf, IR_CALL, mu32, 1); call->aux.callee = nm("unknown"); call->operands[0] = ir_const_int(mf, m0, 0, mu32); ir_emit(m0, call);
        IrValue *ma2 = ir_load(mf, m0, ir_elem_ptr(mf, m0, ma, ir_const_int(mf, m0, 2, mu64), mi32), mi32);
        ir_set_ret(m0, ir_binop(mf, m0, IR_SDIV, ir_const_int(mf, m0, 100, mi32), ma2, mi32));
        ir_finalize_cfg(mf);
        s.propose = P_NONE; s.nrank = 0;
        CHECK(run(mf, &s, true, sizeof region, err), "judges: %s", err);
        CHECK(strstr(recs, " divzero") && !strstr(recs, "not-implied"), "a[2] after init and a call:\n%s", recs);
    }

    // a recursion through a POINTER (c31): apply(fp, x) calls fp(x); spin(x) is `if x == 0 { 40 }
    // else { apply(&spin, x - 1) }`. With measures spin 2x and apply 2x + 1, both calls fall: every
    // rec.* proved (apply's call owes spin's measure at its op[1]); with apply's 2x, the call through
    // the pointer does not fall: its rec.fall not implied
    for (int c = 0; c < 2; c++) {
        IrType *fnt = ir_type_new(&A, IRT_FUNC); fnt->elem = u32t;
        IrType *fpt = ir_type_new(&A, IRT_PTR); fpt->elem = fnt;
        IrFunc *ap = ir_func_new(&A, nm("apply"), u32t, IR_FUNC_PURE), *sp = ir_func_new(&A, nm("spin"), u32t, IR_FUNC_PURE);
        IrValue *af = ir_add_param(ap, fpt, nm("f")), *ax = ir_add_param(ap, u32t, nm("x"));
        IrInstr *ic = ir_instr(ap, IR_CALL, u32t, 2); ic->operands[0] = af; ic->operands[1] = ax; ir_emit(ap->entry, ic);
        int aci = nins_m(ap->entry) - 1;
        ir_set_ret(ap->entry, ic->result);
        ir_finalize_cfg(ap);
        IrValue *sx = ir_add_param(sp, u32t, nm("x"));
        IrBlock *s0 = sp->entry, *s1 = ir_new_block(sp), *s2 = ir_new_block(sp);
        ir_set_br_cond(s0, ir_icmp(sp, s0, IR_CMP_EQ, sx, ir_const_int(sp, s0, 0, u32t)), s1, s2);
        ir_set_ret(s1, ir_const_int(sp, s1, 40, u32t));
        IrValue *self = ir_func_ref(sp, s2, nm("spin"), fpt);
        IrValue *xm = ir_binop(sp, s2, IR_SUB, sx, ir_const_int(sp, s2, 1, u32t), u32t);
        IrInstr *dc = ir_instr(sp, IR_CALL, u32t, 2); dc->aux.callee = nm("apply"); dc->operands[0] = self; dc->operands[1] = xm; ir_emit(s2, dc);
        ir_set_ret(s2, dc->result);
        ir_finalize_cfg(sp);
        ap->next = sp;
        s.propose = P_RANK; s.nrank = 2;
        memset(s.rank, 0, sizeof s.rank);
        IrFunc *fs2[2] = { ap, sp }; IrValue *ks2[2] = { ax, sx };
        for (int w = 0; w < 2; w++) { s.rank[w].blk = JRANK_ENTRY; s.rank[w].fn = fs2[w]; s.rank[w].ncomp = 1; s.rank[w].nt = 1; s.rank[w].key[0] = ks2[w]->id;
                                      s.rank[w].coef[0][0] = 2; s.rank[w].c0[0] = w == 0 && c == 0 ? 1 : 0; }
        snprintf(rf, sizeof rf, "%s bb%d.%d rec.fall", c ? "not-implied" : "proved", ap->entry->id, aci);
        CHECK(run(ap, &s, true, sizeof region, err), "judges: %s", err);
        if (!c) CHECK(strstr(recs, rf) && nrec[JREC_NOT_IMPLIED] + nrec[JREC_OUTSIDE] + nrec[JREC_REFUSED] == 0, "apply 2x + 1, spin 2x: all proved:\n%s", recs);
        else CHECK(strstr(recs, rf), "apply 2x: the call through the pointer does not fall:\n%s", recs);
        s.propose = P_NONE; s.nrank = 0;
    }

    // MONOTONE products (c33): m33(a, b, c) is `q = b * c; if a < b { return q - a * c }`, all u32.
    // a < b gives a*c <= q - c, so the subtraction cannot go below 0: proved; each product's own
    // overflow is `outside` (two variables whose corners leave u32)
    {   IrFunc *mf = ir_func_new(&A, nm("m33"), u32t, IR_FUNC_PURE);
        IrValue *ma = ir_add_param(mf, u32t, nm("a")), *mb = ir_add_param(mf, u32t, nm("b")), *mc = ir_add_param(mf, u32t, nm("c"));
        IrBlock *m0 = mf->entry, *m1 = ir_new_block(mf), *m2 = ir_new_block(mf);
        IrValue *mq = ir_binop(mf, m0, IR_MUL, mb, mc, u32t);
        ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_ULT, ma, mb), m1, m2);
        IrValue *mp = ir_binop(mf, m1, IR_MUL, ma, mc, u32t);
        ir_set_ret(m1, ir_binop(mf, m1, IR_SUB, mq, mp, u32t));
        ir_set_ret(m2, ir_const_int(mf, m2, 0, u32t));
        ir_finalize_cfg(mf);
        Stub s33; memset(&s33, 0, sizeof s33);
        CHECK(run(mf, &s33, true, sizeof region, err), "judges: %s", err);
        char w1[48]; snprintf(w1, sizeof w1, "proved bb%d.%d narrow.lo", m1->id, nins_m(m1) - 1);
        CHECK(strstr(recs, w1) && nrec[JREC_OUTSIDE] == 2 && nrec[JREC_NOT_IMPLIED] == 0, "q - a*c >= 0 proved:\n%s", recs);
        (void)mp;
    }

    // a FREE back edge (c35): Fig. 7b, `while x != 0 { if p { x-- } else if q { y--; z = n1 } else
    // { z--; x = n2 } }` over u32, ranked <y, z, x>. (0) free: its four rank.lex VCs proved, though
    // no one component falls on every path; (1) fall 0 (y on every trip): rank.fall not implied;
    // (2) free, the last path without its z-- (it lowers nothing): a rank.lex not implied
    for (int c = 0; c < 3; c++) {
        IrFunc *cf = ir_func_new(&A, nm("cs7b"), u32t, IR_FUNC_PURE);
        IrValue *cxp = ir_add_param(cf, u32t, nm("xp")), *cp = ir_add_param(cf, u32t, nm("p")), *cq = ir_add_param(cf, u32t, nm("q")),
                *cn1 = ir_add_param(cf, u32t, nm("n1")), *cn2 = ir_add_param(cf, u32t, nm("n2"));
        IrBlock *c0 = cf->entry, *c1 = ir_new_block(cf), *c2 = ir_new_block(cf), *c3 = ir_new_block(cf), *c4 = ir_new_block(cf),
                *c5 = ir_new_block(cf), *c6 = ir_new_block(cf), *c7 = ir_new_block(cf), *c8 = ir_new_block(cf);
        IrValue *cx = ir_alloca(cf, c0, u32t), *cy = ir_alloca(cf, c0, u32t), *cz = ir_alloca(cf, c0, u32t);
        ir_store(cf, c0, cx, cxp); ir_store(cf, c0, cy, cxp); ir_store(cf, c0, cz, cxp); ir_set_br(c0, c1);
        IrValue *clx = ir_load(cf, c1, cx, u32t);
        ir_set_br_cond(c1, ir_icmp(cf, c1, IR_CMP_NE, clx, ir_const_int(cf, c1, 0, u32t)), c2, c7);
        ir_set_br_cond(c2, ir_icmp(cf, c2, IR_CMP_NE, cp, ir_const_int(cf, c2, 0, u32t)), c3, c4);
        ir_store(cf, c3, cx, ir_binop(cf, c3, IR_SUB, clx, ir_const_int(cf, c3, 1, u32t), u32t)); ir_set_br(c3, c6);
        ir_set_br_cond(c4, ir_icmp(cf, c4, IR_CMP_NE, cq, ir_const_int(cf, c4, 0, u32t)), c5, c8);
        ir_store(cf, c5, cy, ir_binop(cf, c5, IR_SUB, ir_load(cf, c5, cy, u32t), ir_const_int(cf, c5, 1, u32t), u32t)); ir_store(cf, c5, cz, cn1); ir_set_br(c5, c6);
        if (c != 2) ir_store(cf, c8, cz, ir_binop(cf, c8, IR_SUB, ir_load(cf, c8, cz, u32t), ir_const_int(cf, c8, 1, u32t), u32t));
        ir_store(cf, c8, cx, cn2); ir_set_br(c8, c6);
        ir_set_br(c6, c1);
        ir_set_ret(c7, ir_const_int(cf, c7, 0, u32t));
        ir_finalize_cfg(cf);
        Stub sc; memset(&sc, 0, sizeof sc);
        sc.propose = P_RANK; sc.nrank = 1;
        JRank *rr = &sc.rank[0];
        rr->blk = c1->id; rr->fn = cf; rr->ncomp = 3; rr->nt = 3; rr->key[0] = cy->id; rr->key[1] = cz->id; rr->key[2] = cx->id;
        for (int j = 0; j < 3; j++) rr->coef[j][j] = 1;
        rr->nedge = 1; rr->from[0] = c6->id; rr->fall[0] = c == 1 ? 0 : -1;
        CHECK(run(cf, &sc, true, sizeof region, err), "judges: %s", err);
        char rl[48]; snprintf(rl, sizeof rl, "proved bb%d.0 rank.lex", c6->id);
        char rn[48]; snprintf(rn, sizeof rn, "not-implied bb%d.0 rank.fall", c6->id);
        if (!c) CHECK(count_in(recs, rl) == 4 && !strstr(recs, "rank.fall") && !strstr(recs, "rank.keep"), "free: four rank.lex proved:\n%s", recs);
        else if (c == 1) CHECK(strstr(recs, rn) != NULL, "fall 0: y does not fall on every trip:\n%s", recs);
        else { snprintf(rn, sizeof rn, "not-implied bb%d.0 rank.lex", c6->id);
               CHECK(strstr(recs, rn) != NULL, "a path lowering nothing: a rank.lex not implied:\n%s", recs); }
    }

    // a MAX component (c36): `while x != 0 { if x > 0 { x-- } else { x++ } }` over an i32 cell.
    // (0) max(x, -x): its two rank.fall (one per row) and rank.bound proved; (1) x alone: its
    // rank.fall not implied (the else path raises x); (2) max(x, -x) with the else path x + 2 (-1
    // goes to 1, |x| stays 1): a rank.fall not implied
    for (int c = 0; c < 3; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        IrFunc *af = ir_func_new(&A, nm("abs36"), mi32, IR_FUNC_PURE);
        IrValue *axp = ir_add_param(af, mi32, nm("xp"));
        IrBlock *a0 = af->entry, *a1 = ir_new_block(af), *a2 = ir_new_block(af), *a3 = ir_new_block(af), *a4 = ir_new_block(af),
                *a5 = ir_new_block(af), *a6 = ir_new_block(af);
        IrValue *ax = ir_alloca(af, a0, mi32);
        ir_store(af, a0, ax, axp); ir_set_br(a0, a1);
        IrValue *alx = ir_load(af, a1, ax, mi32);
        ir_set_br_cond(a1, ir_icmp(af, a1, IR_CMP_NE, alx, ir_const_int(af, a1, 0, mi32)), a2, a5);
        ir_set_br_cond(a2, ir_icmp(af, a2, IR_CMP_SGT, alx, ir_const_int(af, a2, 0, mi32)), a3, a4);
        ir_store(af, a3, ax, ir_binop(af, a3, IR_SUB, alx, ir_const_int(af, a3, 1, mi32), mi32)); ir_set_br(a3, a6);
        ir_store(af, a4, ax, ir_binop(af, a4, IR_ADD, alx, ir_const_int(af, a4, c == 2 ? 2 : 1, mi32), mi32)); ir_set_br(a4, a6);
        ir_set_br(a6, a1);
        ir_set_ret(a5, ir_const_int(af, a5, 0, mi32));
        ir_finalize_cfg(af);
        Stub sa; memset(&sa, 0, sizeof sa);
        sa.propose = P_RANK; sa.nrank = 1;
        JRank *rr = &sa.rank[0];
        rr->blk = a1->id; rr->fn = af; rr->nt = 1; rr->key[0] = ax->id; rr->nedge = 1; rr->from[0] = a6->id; rr->fall[0] = 0;
        rr->ncomp = c == 1 ? 1 : 2; rr->coef[0][0] = 1; rr->coef[1][0] = -1; rr->cont[1] = c == 1 ? 0 : 1;
        CHECK(run(af, &sa, true, sizeof region, err), "judges: %s", err);
        char p1[48], pb[48], nf[48];                         // a record names its kind, not its ordinal
        snprintf(p1, sizeof p1, "proved bb%d.0 rank.fall", a6->id);
        snprintf(pb, sizeof pb, "proved bb%d.0 rank.bound", a6->id); snprintf(nf, sizeof nf, "not-implied bb%d.0 rank.fall", a6->id);
        if (c == 0) CHECK(count_in(recs, p1) == 2 && count_in(recs, pb) == 1 && !strstr(recs, nf), "max(x, -x): both falls and the bound proved:\n%s", recs);
        else CHECK(strstr(recs, nf) != NULL, "case %d: a rank.fall not implied:\n%s", c, recs);
    }

    // a MIN component (c37), Piecewise: `while p > 0 and q > 0 { if q < p { q--; p = n1 } else {
    // p--; q = n2 } }` over i32 cells. (0) min(p, q): its two rank.fall and two rank.bound proved;
    // (1) max(p, q): a rank.fall not implied (the arbitrary one rises); (2) min(p, q) with the else
    // path keeping p (p = p): a rank.fall not implied
    for (int c = 0; c < 3; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        IrFunc *pf = ir_func_new(&A, nm("pw37"), mi32, IR_FUNC_PURE);
        IrValue *pp0 = ir_add_param(pf, mi32, nm("p0")), *pq0 = ir_add_param(pf, mi32, nm("q0")),
                *pn1 = ir_add_param(pf, mi32, nm("n1")), *pn2 = ir_add_param(pf, mi32, nm("n2"));
        IrBlock *q0b = pf->entry, *q1 = ir_new_block(pf), *q2 = ir_new_block(pf), *q3 = ir_new_block(pf), *q4 = ir_new_block(pf),
                *q5 = ir_new_block(pf), *q6 = ir_new_block(pf), *q7 = ir_new_block(pf);
        IrValue *pc = ir_alloca(pf, q0b, mi32), *qc = ir_alloca(pf, q0b, mi32);
        ir_store(pf, q0b, pc, pp0); ir_store(pf, q0b, qc, pq0); ir_set_br(q0b, q1);
        IrValue *plp = ir_load(pf, q1, pc, mi32);
        ir_set_br_cond(q1, ir_icmp(pf, q1, IR_CMP_SGT, plp, ir_const_int(pf, q1, 0, mi32)), q2, q7);
        IrValue *plq = ir_load(pf, q2, qc, mi32);
        ir_set_br_cond(q2, ir_icmp(pf, q2, IR_CMP_SGT, plq, ir_const_int(pf, q2, 0, mi32)), q3, q7);
        ir_set_br_cond(q3, ir_icmp(pf, q3, IR_CMP_SLT, plq, plp), q4, q5);
        ir_store(pf, q4, qc, ir_binop(pf, q4, IR_SUB, plq, ir_const_int(pf, q4, 1, mi32), mi32)); ir_store(pf, q4, pc, pn1); ir_set_br(q4, q6);
        ir_store(pf, q5, pc, c == 2 ? plp : ir_binop(pf, q5, IR_SUB, plp, ir_const_int(pf, q5, 1, mi32), mi32)); ir_store(pf, q5, qc, pn2); ir_set_br(q5, q6);
        ir_set_br(q6, q1);
        ir_set_ret(q7, ir_const_int(pf, q7, 0, mi32));
        ir_finalize_cfg(pf);
        Stub sp; memset(&sp, 0, sizeof sp);
        sp.propose = P_RANK; sp.nrank = 1;
        JRank *rr = &sp.rank[0];
        rr->blk = q1->id; rr->fn = pf; rr->nt = 2; rr->key[0] = pc->id; rr->key[1] = qc->id; rr->nedge = 1; rr->from[0] = q6->id; rr->fall[0] = 0;
        rr->ncomp = 2; rr->coef[0][0] = 1; rr->coef[1][1] = 1; rr->cont[1] = c == 1 ? 1 : 2;
        CHECK(run(pf, &sp, true, sizeof region, err), "judges: %s", err);
        char pf_[48], pb[48], nf[48];                        // a record names its kind, not its ordinal
        snprintf(pf_, sizeof pf_, "proved bb%d.0 rank.fall", q6->id); snprintf(pb, sizeof pb, "proved bb%d.0 rank.bound", q6->id);
        snprintf(nf, sizeof nf, "not-implied bb%d.0 rank.fall", q6->id);
        if (c == 0) CHECK(count_in(recs, pf_) == 2 && count_in(recs, pb) == 2 && !strstr(recs, nf), "min(p, q): both falls and both bounds proved:\n%s", recs);
        else CHECK(strstr(recs, nf) != NULL, "case %d: a rank.fall not implied:\n%s", c, recs);
    }

    // a PIECEWISE ranking (c38), UrbanMine's Fig. 3 over i32 cells: `while x != 0 and y > 0 { if x > 0
    // { if p { x--; y = v1 } else { y-- } } else { if q { x++ } else { y--; x = v2 } } }`, regions
    // P (x >= 1, <x, y>) and N (x <= -1, <y, -x>). (0) its rank.cover, rank.region and six rank.lex
    // proved; a trip from x = 1 ending at x = 0 is in no region and owes nothing; (1) P's x-- made
    // x - 3 (P can move to N): rank.region not implied; (2) P's y-- made y = y (that path lowers
    // nothing): a rank.lex not implied; (3) P's guard x >= 2 (x = 1 starts a trip in no region):
    // rank.cover not implied; (4) overlap: `while x > 0 { x-- }`, regions x >= 3 and x >= 1 both
    // <x>, a legal ranking (rho is 1 throughout) refused, since (B) is owed from x >= 3 too
    for (int c = 0; c < 5; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        IrFunc *uf = ir_func_new(&A, nm("um38"), mi32, IR_FUNC_PURE);
        IrValue *ux0 = ir_add_param(uf, mi32, nm("x0")), *uy0 = ir_add_param(uf, mi32, nm("y0")), *up = ir_add_param(uf, mi32, nm("p")),
                *uq = ir_add_param(uf, mi32, nm("q")), *uv1 = ir_add_param(uf, mi32, nm("v1")), *uv2 = ir_add_param(uf, mi32, nm("v2"));
        IrBlock *u0 = uf->entry, *u1 = ir_new_block(uf);
        IrValue *ux = ir_alloca(uf, u0, mi32), *uy = ir_alloca(uf, u0, mi32);
        ir_store(uf, u0, ux, ux0); ir_store(uf, u0, uy, uy0); ir_set_br(u0, u1);
        IrBlock *latch;
        if (c == 4) {                                          // while x > 0 { x-- }
            IrBlock *u2 = ir_new_block(uf), *u3 = ir_new_block(uf);
            IrValue *lx = ir_load(uf, u1, ux, mi32);
            ir_set_br_cond(u1, ir_icmp(uf, u1, IR_CMP_SGT, lx, ir_const_int(uf, u1, 0, mi32)), u2, u3);
            ir_store(uf, u2, ux, ir_binop(uf, u2, IR_SUB, lx, ir_const_int(uf, u2, 1, mi32), mi32)); ir_set_br(u2, u1);
            ir_set_ret(u3, ir_const_int(uf, u3, 0, mi32));
            latch = u2;
        } else {
            IrBlock *u2 = ir_new_block(uf), *u3 = ir_new_block(uf), *u4 = ir_new_block(uf), *u5 = ir_new_block(uf), *u6 = ir_new_block(uf),
                    *u7 = ir_new_block(uf), *u8 = ir_new_block(uf), *u9 = ir_new_block(uf), *u10 = ir_new_block(uf), *u11 = ir_new_block(uf);
            IrValue *lx = ir_load(uf, u1, ux, mi32);
            ir_set_br_cond(u1, ir_icmp(uf, u1, IR_CMP_NE, lx, ir_const_int(uf, u1, 0, mi32)), u2, u9);
            IrValue *ly = ir_load(uf, u2, uy, mi32);
            ir_set_br_cond(u2, ir_icmp(uf, u2, IR_CMP_SGT, ly, ir_const_int(uf, u2, 0, mi32)), u3, u9);
            ir_set_br_cond(u3, ir_icmp(uf, u3, IR_CMP_SGT, lx, ir_const_int(uf, u3, 0, mi32)), u4, u5);
            ir_set_br_cond(u4, ir_icmp(uf, u4, IR_CMP_NE, up, ir_const_int(uf, u4, 0, mi32)), u6, u7);
            ir_store(uf, u6, ux, ir_binop(uf, u6, IR_SUB, lx, ir_const_int(uf, u6, c == 1 ? 3 : 1, mi32), mi32)); ir_store(uf, u6, uy, uv1); ir_set_br(u6, u8);
            ir_store(uf, u7, uy, c == 2 ? ly : ir_binop(uf, u7, IR_SUB, ly, ir_const_int(uf, u7, 1, mi32), mi32)); ir_set_br(u7, u8);
            ir_set_br_cond(u5, ir_icmp(uf, u5, IR_CMP_NE, uq, ir_const_int(uf, u5, 0, mi32)), u10, u11);
            ir_store(uf, u10, ux, ir_binop(uf, u10, IR_ADD, lx, ir_const_int(uf, u10, 1, mi32), mi32)); ir_set_br(u10, u8);
            ir_store(uf, u11, uy, ir_binop(uf, u11, IR_SUB, ly, ir_const_int(uf, u11, 1, mi32), mi32)); ir_store(uf, u11, ux, uv2); ir_set_br(u11, u8);
            ir_set_br(u8, u1);
            ir_set_ret(u9, ir_const_int(uf, u9, 0, mi32));
            latch = u8;
        }
        ir_finalize_cfg(uf);
        Stub su; memset(&su, 0, sizeof su);
        su.propose = P_RANK; su.nrank = 1;
        JRank *rr = &su.rank[0];
        rr->blk = u1->id; rr->fn = uf; rr->nt = 2; rr->key[0] = ux->id; rr->key[1] = uy->id; rr->nedge = 1; rr->from[0] = latch->id; rr->fall[0] = -1;
        if (c == 4) {                                          // x - 3 >= 0, <x> | x - 1 >= 0, <x>
            rr->ncomp = 4; rr->coef[0][0] = 1; rr->c0[0] = -3; rr->coef[1][0] = 1; rr->coef[2][0] = 1; rr->c0[2] = -1; rr->coef[3][0] = 1;
            rr->nreg = 2; rr->reg[0].g0 = 0; rr->reg[0].ng = 1; rr->reg[0].t0 = 1; rr->reg[0].nt = 1;
            rr->reg[1].g0 = 2; rr->reg[1].ng = 1; rr->reg[1].t0 = 3; rr->reg[1].nt = 1;
        } else {
            rr->ncomp = 6;
            rr->coef[0][0] = 1; rr->c0[0] = c == 3 ? -2 : -1; rr->coef[1][0] = 1; rr->coef[2][1] = 1;
            rr->coef[3][0] = -1; rr->c0[3] = -1; rr->coef[4][1] = 1; rr->coef[5][0] = -1;
            rr->nreg = 2; rr->reg[0].g0 = 0; rr->reg[0].ng = 1; rr->reg[0].t0 = 1; rr->reg[0].nt = 2;
            rr->reg[1].g0 = 3; rr->reg[1].ng = 1; rr->reg[1].t0 = 4; rr->reg[1].nt = 2;
        }
        CHECK(run(uf, &su, true, sizeof region, err), "judges: %s", err);
        char pk[48], nk[48];
        const char *kind = c == 0 ? "rank." : c == 1 || c == 4 ? "rank.region" : c == 2 ? "rank.lex" : "rank.cover";
        snprintf(pk, sizeof pk, "proved bb%d.0 rank.", latch->id); snprintf(nk, sizeof nk, "not-implied bb%d.", latch->id);
        char nkk[64]; snprintf(nkk, sizeof nkk, "%s", kind);
        // a record names its point and kind: the latch's not-implied ones, by kind
        int nn = 0, nkind = 0;
        for (const char *l = recs; (l = strstr(l, nk)); l++) { nn++; const char *e = strchr(l, '\n'), *k = strstr(l, nkk); if (k && (!e || k < e)) nkind++; }
        if (c == 0) CHECK(count_in(recs, " rank.") == 8 && count_in(recs, pk) == 8 && nn == 0 && !strstr(recs, "not-implied") && !strstr(recs, "outside"),
                          "Fig. 3: its eight rank VCs proved:\n%s", recs);
        else if (c == 4) CHECK(nn == 1 && nkind == 1 && count_in(recs, "not-implied") == 1, "overlap: rank.region alone not implied:\n%s", recs);
        else CHECK(nkind >= 1, "case %d: %s not implied:\n%s", c, kind, recs);
    }

    // GUARDED return claims (s7), all i32. dbl(a) = a +% a has no bound for every a (it wraps), but under
    // a = 3 it is 6. The caller divides by 7 - dbl(3). (0) dbl(3) <= 6 and >= 6, guarded: certified, the
    // division proved; (1) no claim: not implied; (2) a false one, dbl(3) <= 5: uncertified; (3) dbl(3)'s
    // claims at a call dbl(4), dividing by 8 - dbl(4), which is 0: not applied, not implied; (4) a
    // computed argument, `if k == 3 { 100 / (7 - dbl(k)) }`: the disjunction applies it, proved; (5) a
    // callee that reads a pointer argument, h(x, p) = x + *p, guarded x = 1 -> ret <= 3: nothing about *p
    // is assumed at entry, so it is not implied (uncertified); (6) a return claim of THREE terms, add(a, b)
    // = a + b, ret - a - b <= 0, used at the call `10 / (6 - add(2, 3))`: proved (the call site's term
    // array holds JCLAIM_MAXT terms; it held 2)
    for (int c = 0; c < 7; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        IrFunc *df = ir_func_new(&A, nm(c == 6 ? "add" : c == 5 ? "h" : "dbl"), mi32, IR_FUNC_PURE);
        IrValue *da = ir_add_param(df, mi32, nm("a")), *db = NULL;
        if (c == 6) db = ir_add_param(df, mi32, nm("b"));
        if (c == 5) { IrType *pt = ir_type_new(&A, IRT_PTR); pt->elem = mi32; db = ir_add_param(df, pt, nm("p")); }
        IrInstr *ad = ir_instr(df, IR_ADD, mi32, 2); ad->operands[0] = da;
        ad->operands[1] = c == 6 ? db : c == 5 ? ir_load(df, df->entry, db, mi32) : da;
        ad->wrap = c >= 5 ? IR_WRAP_CHECK : IR_WRAP_MODULAR; ir_emit(df->entry, ad);
        ir_set_ret(df->entry, ad->result);
        ir_finalize_cfg(df);
        IrFunc *mf = NULL;
        if (c != 5) {
            mf = ir_func_new(&A, nm("main"), mi32, IR_FUNC_PURE);
            IrValue *k = ir_add_param(mf, mi32, nm("k"));
            IrBlock *m0 = mf->entry, *m1 = c == 4 ? ir_new_block(mf) : m0, *m2 = c == 4 ? ir_new_block(mf) : NULL;
            if (c == 4) ir_set_br_cond(m0, ir_icmp(mf, m0, IR_CMP_EQ, k, ir_const_int(mf, m0, 3, mi32)), m1, m2);
            IrInstr *call = ir_instr(mf, IR_CALL, mi32, c == 6 ? 2 : 1); call->aux.callee = nm(c == 6 ? "add" : "dbl");
            call->operands[0] = c == 4 ? k : ir_const_int(mf, m1, c == 3 ? 4 : c == 6 ? 2 : 3, mi32);
            if (c == 6) call->operands[1] = ir_const_int(mf, m1, 3, mi32);
            ir_emit(m1, call);
            IrValue *den = ir_binop(mf, m1, IR_SUB, ir_const_int(mf, m1, c == 3 ? 8 : c == 6 ? 6 : 7, mi32), call->result, mi32);
            ir_set_ret(m1, ir_binop(mf, m1, IR_SDIV, ir_const_int(mf, m1, c == 6 ? 10 : 100, mi32), den, mi32));
            if (c == 4) ir_set_ret(m2, ir_const_int(mf, m2, 0, mi32));
            ir_finalize_cfg(mf);
            df->next = mf;
        }
        Stub sg; memset(&sg, 0, sizeof sg);
        if (c != 1) sg.propose = P_RANK;                       // claims only (no rank, no plain claim)
        if (c == 6) {                                          // an ordinary return claim of three terms
            sg.nclaims = 1; JClaim *rc = &sg.claims[0];
            rc->blk = JCLAIM_RET; rc->fn = df; rc->nt = 3; rc->key[0] = JKEY_RET; rc->coef[0] = 1;
            rc->key[1] = da->id; rc->coef[1] = -1; rc->key[2] = db->id; rc->coef[2] = -1; rc->rhs = 0;
        } else if (c != 1) {
            sg.ngc = c == 5 ? 1 : 2;
            for (int q = 0; q < sg.ngc; q++) {
                JGuardClaim *g = &sg.gc[q];
                g->fn = df; g->nt = 1; g->key[0] = JKEY_RET; g->coef[0] = q ? -1 : 1;
                g->rhs = c == 5 ? 3 : q ? -6 : (c == 2 ? 5 : 6);
                g->ng = 1; g->gkey[0] = da->id; g->gval[0] = c == 5 ? 1 : 3;
            }
        }
        CHECK(run(df, &sg, true, sizeof region, err), "judges: %s", err);
        int dz_p = 0, dz_n = 0, unc = count_in(recs, "uncertified"), post_n = 0;
        for (const char *l = recs; *l; ) {
            const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l);
            char v[24] = "", pt[64] = "", kd[64] = "", line[600]; snprintf(line, sizeof line, "%.*s", (int)(n < 599 ? n : 599), l);
            if (sscanf(line, "%23s %63s %63s", v, pt, kd) == 3) {
                if (!strcmp(kd, "divzero")) { if (!strcmp(v, "proved")) dz_p++; else if (!strcmp(v, "not-implied")) dz_n++; }
                if (!strcmp(kd, "post") && !strcmp(v, "not-implied")) post_n++;
            }
            l = e ? e + 1 : l + n;
        }
        if (c == 0 || c == 4 || c == 6) CHECK(dz_p == 1 && dz_n == 0 && unc == 0, "case %d: certified, the division proved:\n%s", c, recs);
        else if (c == 1 || c == 3) CHECK(dz_p == 0 && dz_n == 1 && unc == 0, "case %d: the division not implied:\n%s", c, recs);
        else CHECK(unc > 0 && post_n == 1, "case %d: the guarded claim not implied, the module uncertified:\n%s", c, recs);
        if (c == 5) {                                          // the post VC assumes the guard and nothing of *p
            JMem m = jmem_over(region, sizeof region); JObl *o; int32_t n; char e2[200];
            bool ok = jgen_func(&m, df, df, NULL, 0, sg.gc, 1, NULL, 0, NULL, JGEN_CLAIMS, &o, &n, e2, sizeof e2);
            int guards = 0, about_p = 0;
            for (int32_t i = 0; ok && i < n; i++)
                for (int32_t h = 0; h < o[i].vc.nhyps; h++) {
                    const char *og = o[i].vc.hyps[h].origin;
                    if (og && !strncmp(og, "guard", 5)) guards++;
                    if (og && strstr(og, "contract")) about_p++;
                }
            CHECK(ok && n == 1 && guards == 1 && about_p == 0, "case 5: one post VC, one guard hypothesis, no other assumption (%s, %d VCs, %d guards)",
                  ok ? "ok" : e2, ok ? n : -1, guards);
        }
    }

    // a RETURNED SLICE's length (s14; census_gate's `func go() u8[] { return "xyz" }`): go returns a
    // slice of length 3, the caller narrows len(go()) to an i8. (A) the return claim len(ret) <= 3:
    // certified, the narrowing proved; (B) len(ret) <= 2, false at the return: uncertified; (C) no
    // claim: the narrowing not implied; (D) a GUARDED claim over len(ret): refused
    for (int c = 0; c < 4; c++) {
        IrType *mu8 = ir_type_int(&A, 8, false), *mi8 = ir_type_int(&A, 8, true), *mu64 = ir_type_int(&A, 64, false);
        IrType *a3 = ir_type_new(&A, IRT_ARRAY); a3->elem = mu8; a3->array_len = 3;
        IrType *st = ir_type_new(&A, IRT_SLICE); st->elem = mu8;
        IrFunc *gf = ir_func_new(&A, nm("go"), st, IR_FUNC_PURE);
        IrValue *gx = ir_add_param(gf, mu8, nm("x"));
        IrValue *buf = ir_alloca(gf, gf->entry, a3);
        ir_set_ret(gf->entry, ir_make_slice(gf, gf->entry, buf, ir_const_int(gf, gf->entry, 3, mu64), mu8));
        ir_finalize_cfg(gf);
        IrFunc *mf = ir_func_new(&A, nm("main"), mi8, IR_FUNC_PURE);
        IrInstr *call = ir_instr(mf, IR_CALL, st, 1); call->aux.callee = nm("go");
        call->operands[0] = ir_const_int(mf, mf->entry, 7, mu8); ir_emit(mf->entry, call);
        IrValue *ln = ir_slice_len(mf, mf->entry, call->result);
        IrInstr *nw = ir_instr(mf, IR_CAST, mi8, 1); nw->operands[0] = ln; ir_emit(mf->entry, nw);
        ir_set_ret(mf->entry, nw->result);
        ir_finalize_cfg(mf);
        gf->next = mf;
        Stub sl; memset(&sl, 0, sizeof sl);
        sl.propose = P_RANK;
        if (c == 0 || c == 1) { JClaim *r = &sl.claims[0]; r->blk = JCLAIM_RET; r->fn = gf; r->nt = 1; r->key[0] = JKEY_RET; r->tk[0] = JTERM_LEN;
                                r->coef[0] = 1; r->rhs = c == 0 ? 3 : 2; sl.nclaims = 1; }
        if (c == 3) { JGuardClaim *g = &sl.gc[0]; g->fn = gf; g->nt = 1; g->key[0] = JKEY_RET; g->tk[0] = JTERM_LEN; g->coef[0] = 1; g->rhs = 3;
                      g->ng = 1; g->gkey[0] = gx->id; g->gval[0] = 7; sl.ngc = 1; }
        CHECK(run(gf, &sl, true, sizeof region, err), "judges: %s", err);
        int nhi = 0, phi = 0;
        for (const char *l = recs; *l; ) {
            const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l);
            char v[24] = "", pt[64] = "", kd[64] = "", line[600]; snprintf(line, sizeof line, "%.*s", (int)(n < 599 ? n : 599), l);
            if (sscanf(line, "%23s %63s %63s", v, pt, kd) == 3 && !strcmp(kd, "narrow.hi")) { nhi++; phi += !strcmp(v, "proved"); }
            l = e ? e + 1 : l + n;
        }
        if (c == 0) CHECK(nhi == 1 && phi == 1 && !strstr(recs, "uncertified") && !strstr(recs, "refused"), "len(ret) <= 3: the narrowing proved:\n%s", recs);
        else if (c == 1) CHECK(strstr(recs, "uncertified") != NULL && phi == 0, "len(ret) <= 2: uncertified:\n%s", recs);
        else if (c == 2) CHECK(nhi == 1 && phi == 0 && strstr(recs, "not-implied") != NULL, "no claim: the narrowing not implied:\n%s", recs);
        else CHECK(strstr(recs, "guarded return claim over a length") != NULL, "a guarded claim over len(ret): refused:\n%s", recs);
    }
    printf("  a returned slice's length: return claims on len(ret), owed at the return, stated at the call\n");

    // a return that WRAPS (s8): g(p, q) returns the i32 sum p + q into an i8, waived (`unsafe { return
    // p + q }`), so g(100, 100) is -56. The caller divides by g(100, 100) + 57. (A) the guarded claim ret
    // in [-56, -56]: certified (its post is owed about the landed value), the division proved; (B) ret in
    // [200, 200], true of the sum before it lands and of nothing a caller receives: uncertified
    for (int c = 0; c < 2; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true), *mi8 = ir_type_int(&A, 8, true);
        IrFunc *gf = ir_func_new(&A, nm("g"), mi8, IR_FUNC_PURE);
        IrValue *gp = ir_add_param(gf, mi32, nm("p")), *gq = ir_add_param(gf, mi32, nm("q"));
        IrValue *sum = ir_binop(gf, gf->entry, IR_ADD, gp, gq, mi32);
        ir_set_ret(gf->entry, sum);
        gf->entry->term.unchecked = true;                      // the landing i32 -> i8 is waived: it wraps
        ir_finalize_cfg(gf);
        IrFunc *mf = ir_func_new(&A, nm("main"), mi32, IR_FUNC_PURE);
        IrInstr *call = ir_instr(mf, IR_CALL, mi8, 2); call->aux.callee = nm("g");
        call->operands[0] = ir_const_int(mf, mf->entry, 100, mi32); call->operands[1] = ir_const_int(mf, mf->entry, 100, mi32);
        ir_emit(mf->entry, call);
        IrInstr *wd = ir_instr(mf, IR_CAST, mi32, 1); wd->operands[0] = call->result; ir_emit(mf->entry, wd);
        IrValue *den = ir_binop(mf, mf->entry, IR_ADD, wd->result, ir_const_int(mf, mf->entry, 57, mi32), mi32);
        ir_set_ret(mf->entry, ir_binop(mf, mf->entry, IR_SDIV, ir_const_int(mf, mf->entry, 1000, mi32), den, mi32));
        ir_finalize_cfg(mf);
        gf->next = mf;
        Stub sw; memset(&sw, 0, sizeof sw);
        sw.propose = P_RANK; sw.ngc = 2;
        for (int q = 0; q < 2; q++) {
            JGuardClaim *g = &sw.gc[q];
            g->fn = gf; g->nt = 1; g->key[0] = JKEY_RET; g->coef[0] = q ? -1 : 1;
            g->rhs = c == 0 ? (q ? 56 : -56) : (q ? -200 : 200);
            g->ng = 2; g->gkey[0] = gp->id; g->gval[0] = 100; g->gkey[1] = gq->id; g->gval[1] = 100;
        }
        CHECK(run(gf, &sw, true, sizeof region, err), "judges: %s", err);
        int dz_p = count_in(recs, "proved bb") ? 0 : 0, unc = count_in(recs, "uncertified");
        for (const char *l = recs; *l; ) {
            const char *e = strchr(l, '\n'); size_t n = e ? (size_t)(e - l) : strlen(l);
            char v[24] = "", pt[64] = "", kd[64] = "", line[600]; snprintf(line, sizeof line, "%.*s", (int)(n < 599 ? n : 599), l);
            if (sscanf(line, "%23s %63s %63s", v, pt, kd) == 3 && !strcmp(kd, "divzero") && !strcmp(v, "proved")) dz_p++;
            l = e ? e + 1 : l + n;
        }
        if (c == 0) CHECK(dz_p == 1 && unc == 0, "g(100, 100) = -56: certified, the division proved:\n%s", recs);
        else CHECK(unc > 0, "g(100, 100) = 200 (the sum before it lands): uncertified:\n%s", recs);
    }

    // an ENTRY term (c40). (A) LarrazOliveras' Fig. 1, all i32: `while y >= 1 { x = x - 1; while y < z
    // { x++; z-- }; y = x + y }`. At the inner header x + z - entry(x) - entry(z) <= 0 and its mirror
    // (entry(x) the store x - 1, entry(z) z's outer-header version), rankings z - y (inner) and
    // x + z + 2^32 (outer, bounded by the types): every claim and rank VC proved; (A') no claims: the
    // outer rank.fall not implied. (B) nestedLoop-1's middle loop, all u32: `i = 0; while i < n { j =
    // 0; while j < m { j++; k = i; while k < N { k++ }; i = k }; i++ }`, claims k - i >= 0 at the
    // inner header and i - entry(i) >= 0 at the middle one (entry(i) i's outer-header version),
    // rankings n - i, m - j, N - k: all proved; (B') without the middle claim: the outer's rank.fall
    // not implied
    for (int c = 0; c < 4; c++) {
        IrType *T = c < 2 ? ir_type_int(&A, 32, true) : u32t;
        IrFunc *ef = ir_func_new(&A, nm(c < 2 ? "larraz40" : "nest40"), T, IR_FUNC_PURE);
        Stub se; memset(&se, 0, sizeof se);
        se.propose = P_RANK;
        IrBlock *latch_out, *latch_in, *H1, *H2;
        if (c < 2) {
            IrValue *x0 = ir_add_param(ef, T, nm("x0")), *y0 = ir_add_param(ef, T, nm("y0")), *z0 = ir_add_param(ef, T, nm("z0"));
            IrBlock *e0 = ef->entry, *e1 = ir_new_block(ef), *e2 = ir_new_block(ef), *e3 = ir_new_block(ef), *e4 = ir_new_block(ef),
                    *e5 = ir_new_block(ef), *e6 = ir_new_block(ef);
            IrValue *x = ir_alloca(ef, e0, T), *y = ir_alloca(ef, e0, T), *z = ir_alloca(ef, e0, T);
            ir_store(ef, e0, x, x0); ir_store(ef, e0, y, y0); ir_store(ef, e0, z, z0); ir_set_br(e0, e1);
            ir_set_br_cond(e1, ir_icmp(ef, e1, IR_CMP_SGE, ir_load(ef, e1, y, T), ir_const_int(ef, e1, 1, T)), e2, e6);
            ir_store(ef, e2, x, ir_binop(ef, e2, IR_SUB, ir_load(ef, e2, x, T), ir_const_int(ef, e2, 1, T), T)); ir_set_br(e2, e3);
            ir_set_br_cond(e3, ir_icmp(ef, e3, IR_CMP_SLT, ir_load(ef, e3, y, T), ir_load(ef, e3, z, T)), e4, e5);
            ir_store(ef, e4, x, ir_binop(ef, e4, IR_ADD, ir_load(ef, e4, x, T), ir_const_int(ef, e4, 1, T), T));
            ir_store(ef, e4, z, ir_binop(ef, e4, IR_SUB, ir_load(ef, e4, z, T), ir_const_int(ef, e4, 1, T), T)); ir_set_br(e4, e3);
            ir_store(ef, e5, y, ir_binop(ef, e5, IR_ADD, ir_load(ef, e5, x, T), ir_load(ef, e5, y, T), T)); ir_set_br(e5, e1);
            ir_set_ret(e6, ir_const_int(ef, e6, 0, T));
            ir_finalize_cfg(ef);
            H1 = e1; H2 = e3; latch_out = e5; latch_in = e4;
            for (int q = 0; q < 2 && c == 0; q++) {                // x + z - entry(x) - entry(z), both ways
                JClaim *cl = &se.claims[q]; cl->blk = e3->id; cl->fn = ef; cl->nt = 4;
                cl->key[0] = x->id; cl->key[1] = z->id; cl->key[2] = x->id; cl->key[3] = z->id;
                cl->tk[2] = JTERM_ENTRY; cl->tk[3] = JTERM_ENTRY;
                int sg = q ? -1 : 1; cl->coef[0] = sg; cl->coef[1] = sg; cl->coef[2] = -sg; cl->coef[3] = -sg; cl->rhs = 0;
            }
            se.nclaims = c == 0 ? 2 : 0;
            JRank *ri = &se.rank[0], *ro = &se.rank[1];
            ri->blk = e3->id; ri->fn = ef; ri->ncomp = 1; ri->nt = 2; ri->key[0] = z->id; ri->key[1] = y->id; ri->coef[0][0] = 1; ri->coef[0][1] = -1;
            ri->nedge = 1; ri->from[0] = e4->id;
            ro->blk = e1->id; ro->fn = ef; ro->ncomp = 1; ro->nt = 2; ro->key[0] = x->id; ro->key[1] = z->id; ro->coef[0][0] = 1; ro->coef[0][1] = 1;
            ro->c0[0] = (__int128)1 << 32; ro->nedge = 1; ro->from[0] = e5->id;
            se.nrank = 2;
        } else {
            IrValue *n = ir_add_param(ef, T, nm("n")), *m = ir_add_param(ef, T, nm("m")), *N = ir_add_param(ef, T, nm("N"));
            IrBlock *b0 = ef->entry, *b1 = ir_new_block(ef), *b2 = ir_new_block(ef), *b3 = ir_new_block(ef), *b4 = ir_new_block(ef),
                    *b5 = ir_new_block(ef), *b6 = ir_new_block(ef), *b7 = ir_new_block(ef), *b8 = ir_new_block(ef), *b9 = ir_new_block(ef);
            IrValue *i = ir_alloca(ef, b0, T), *j = ir_alloca(ef, b0, T), *k = ir_alloca(ef, b0, T);
            ir_store(ef, b0, i, ir_const_int(ef, b0, 0, T)); ir_store(ef, b0, j, ir_const_int(ef, b0, 0, T)); ir_store(ef, b0, k, ir_const_int(ef, b0, 0, T));
            ir_set_br(b0, b1);
            ir_set_br_cond(b1, ir_icmp(ef, b1, IR_CMP_ULT, ir_load(ef, b1, i, T), n), b2, b9);              // outer
            ir_store(ef, b2, j, ir_const_int(ef, b2, 0, T)); ir_set_br(b2, b3);
            IrValue *lj = ir_load(ef, b3, j, T);
            ir_set_br_cond(b3, ir_icmp(ef, b3, IR_CMP_ULT, lj, m), b4, b8);                                // middle
            ir_store(ef, b4, j, ir_binop(ef, b4, IR_ADD, lj, ir_const_int(ef, b4, 1, T), T));
            ir_store(ef, b4, k, ir_load(ef, b4, i, T)); ir_set_br(b4, b5);
            IrValue *lk = ir_load(ef, b5, k, T);
            ir_set_br_cond(b5, ir_icmp(ef, b5, IR_CMP_ULT, lk, N), b6, b7);                                // inner
            ir_store(ef, b6, k, ir_binop(ef, b6, IR_ADD, lk, ir_const_int(ef, b6, 1, T), T)); ir_set_br(b6, b5);
            ir_store(ef, b7, i, ir_load(ef, b7, k, T)); ir_set_br(b7, b3);
            ir_store(ef, b8, i, ir_binop(ef, b8, IR_ADD, ir_load(ef, b8, i, T), ir_const_int(ef, b8, 1, T), T)); ir_set_br(b8, b1);
            ir_set_ret(b9, ir_const_int(ef, b9, 0, T));
            ir_finalize_cfg(ef);
            H1 = b1; H2 = b3; latch_out = b8; latch_in = b7;
            JClaim *ci = &se.claims[0], *cm = &se.claims[1];
            ci->blk = b5->id; ci->fn = ef; ci->nt = 2; ci->key[0] = k->id; ci->key[1] = i->id; ci->coef[0] = -1; ci->coef[1] = 1; ci->rhs = 0;   // k - i >= 0
            cm->blk = b3->id; cm->fn = ef; cm->nt = 2; cm->key[0] = i->id; cm->key[1] = i->id; cm->tk[1] = JTERM_ENTRY;
            cm->coef[0] = -1; cm->coef[1] = 1; cm->rhs = 0;                                                                         // i - entry(i) >= 0
            se.nclaims = c == 2 ? 2 : 1;
            JRank *r = se.rank;
            IrBlock *hs[3] = { b1, b3, b5 }, *ls[3] = { b8, b7, b6 }; IrValue *cs[3] = { i, j, k }, *bs[3] = { n, m, N };
            for (int q = 0; q < 3; q++) { r[q].blk = hs[q]->id; r[q].fn = ef; r[q].ncomp = 1; r[q].nt = 2; r[q].key[0] = bs[q]->id; r[q].key[1] = cs[q]->id;
                                          r[q].coef[0][0] = 1; r[q].coef[0][1] = -1; r[q].nedge = 1; r[q].from[0] = ls[q]->id; }
            se.nrank = 3;
        }
        (void)latch_in; (void)H2;
        CHECK(run(ef, &se, true, sizeof region, err), "judges: %s", err);
        // the claims' and rankings' records (the programs' own + and - are unbounded here, their
        // narrowings not implied, and beside the point)
        int ni = 0;
        for (const char *l = recs; (l = strstr(l, "not-implied ")); l++) { const char *e = strchr(l, '\n'), *k1 = strstr(l, " inv."), *k2 = strstr(l, " rank.");
                                                                           if ((k1 && (!e || k1 < e)) || (k2 && (!e || k2 < e))) ni++; }
        char fo[64]; snprintf(fo, sizeof fo, "not-implied bb%d.%d rank.fall", latch_out->id, nins_m(latch_out));
        int nr = c < 2 ? 2 : 3;
        if (c == 0 || c == 2) CHECK(ni == 0 && !strstr(recs, "outside") && !strstr(recs, "uncertified") && count_in(recs, "proved bb") > 0 &&
                                    count_in(recs, " inv.entry") == 2 && count_in(recs, " inv.keep") == 2 && count_in(recs, "rank.fall") == nr && count_in(recs, "rank.bound") == nr,
                                    "case %d: every claim and ranking VC proved:\n%s", c, recs);
        else CHECK(strstr(recs, fo) != NULL, "case %d: without the entry claim, the outer rank.fall not implied:\n%s", c, recs);
        (void)H1;
    }

    // HEAD terms (s11; R3c, simdlex's count_tokens_smart), all u32: `i = 0; while i < n { if m > 5 {
    // i++; while i < n and k > 7 { i++ } } else { i++ } }`, the three exits joining at the latch b7.
    // The ranking n - i falls only if i at the join exceeds i as the trip began, and where the inner
    // loop leaves by `k > 7` its i says nothing of that. (0) claims head(i) - i <= -1 at the inner
    // header b4 and at the join b7: every claim and rank VC proved; (1) none: the rank.fall not
    // implied; (2) head(i) - i <= -2 at the join, false where i rose by one: a hull VC not implied;
    // (3) a HEAD term at the outer header, which no other loop holds, and (4) at the exit, in no
    // loop: the claims refused
    for (int c = 0; c < 5; c++) {
        IrFunc *ef = ir_func_new(&A, nm("head41"), u32t, IR_FUNC_PURE);
        Stub se; memset(&se, 0, sizeof se);
        se.propose = P_RANK;
        IrValue *n = ir_add_param(ef, u32t, nm("n")), *m = ir_add_param(ef, u32t, nm("m")), *kk = ir_add_param(ef, u32t, nm("k"));
        IrBlock *b0 = ef->entry, *b1 = ir_new_block(ef), *b2 = ir_new_block(ef), *b3 = ir_new_block(ef), *b4 = ir_new_block(ef),
                *b5 = ir_new_block(ef), *b6 = ir_new_block(ef), *b7 = ir_new_block(ef), *b8 = ir_new_block(ef), *b9 = ir_new_block(ef);
        IrValue *i = ir_alloca(ef, b0, u32t);
        ir_store(ef, b0, i, ir_const_int(ef, b0, 0, u32t)); ir_set_br(b0, b1);
        ir_set_br_cond(b1, ir_icmp(ef, b1, IR_CMP_ULT, ir_load(ef, b1, i, u32t), n), b2, b8);         // outer
        ir_set_br_cond(b2, ir_icmp(ef, b2, IR_CMP_UGT, m, ir_const_int(ef, b2, 5, u32t)), b3, b6);
        ir_store(ef, b3, i, ir_binop(ef, b3, IR_ADD, ir_load(ef, b3, i, u32t), ir_const_int(ef, b3, 1, u32t), u32t)); ir_set_br(b3, b4);
        IrValue *li = ir_load(ef, b4, i, u32t);
        ir_set_br_cond(b4, ir_icmp(ef, b4, IR_CMP_ULT, li, n), b9, b7);                                // inner
        ir_set_br_cond(b9, ir_icmp(ef, b9, IR_CMP_UGT, kk, ir_const_int(ef, b9, 7, u32t)), b5, b7);   // and k > 7
        ir_store(ef, b5, i, ir_binop(ef, b5, IR_ADD, li, ir_const_int(ef, b5, 1, u32t), u32t)); ir_set_br(b5, b4);
        ir_store(ef, b6, i, ir_binop(ef, b6, IR_ADD, ir_load(ef, b6, i, u32t), ir_const_int(ef, b6, 1, u32t), u32t)); ir_set_br(b6, b7);
        ir_set_br(b7, b1);
        ir_set_ret(b8, ir_const_int(ef, b8, 0, u32t));
        ir_finalize_cfg(ef);
        IrBlock *at[5] = { b7, b7, b7, b1, b8 };
        JClaim *cj = &se.claims[0], *ch = &se.claims[1];
        cj->blk = at[c]->id; cj->fn = ef; cj->nt = 2; cj->key[0] = i->id; cj->key[1] = i->id; cj->tk[0] = JTERM_HEAD;
        cj->coef[0] = 1; cj->coef[1] = -1; cj->rhs = c == 2 ? -2 : -1;                               // head(i) - i <= -1
        *ch = *cj; ch->blk = b4->id; ch->rhs = -1;
        se.nclaims = c == 1 ? 0 : 2;
        JRank *r = se.rank;
        IrBlock *hs[2] = { b1, b4 }, *ls[2] = { b7, b5 };
        for (int q = 0; q < 2; q++) { r[q].blk = hs[q]->id; r[q].fn = ef; r[q].ncomp = 1; r[q].nt = 2; r[q].key[0] = n->id; r[q].key[1] = i->id;
                                      r[q].coef[0][0] = 1; r[q].coef[0][1] = -1; r[q].nedge = 1; r[q].from[0] = ls[q]->id; }
        se.nrank = 2;
        CHECK(run(ef, &se, true, sizeof region, err), "judges: %s", err);
        char fo[64]; snprintf(fo, sizeof fo, "not-implied bb%d.%d rank.fall", b7->id, nins_m(b7));
        const char *ni = strstr(recs, "not-implied"), *nl = ni ? strchr(ni, '\n') : NULL, *hu = ni ? strstr(ni, " hull") : NULL;
        if (c == 0) CHECK(!strstr(recs, "not-implied") && !strstr(recs, "refused") && !strstr(recs, "uncertified") && count_in(recs, " hull") == 3 &&
                          count_in(recs, " inv.entry") == 1 && count_in(recs, " inv.keep") == 1 && count_in(recs, "proved") > 0 &&
                          count_in(recs, "rank.fall") == 2, "case 0: every claim and rank VC proved:\n%s", recs);
        else if (c == 1) CHECK(strstr(recs, fo) != NULL, "case 1: without the HEAD claims, the outer rank.fall not implied:\n%s", recs);
        else if (c == 2) CHECK(hu && nl && hu < nl && !strstr(recs, "refused"), "case 2: head(i) - i <= -2 not implied:\n%s", recs);
        else CHECK(!strstr(recs, "proved") && strstr(recs, "uncertified") && strstr(recs, "refuses the claims") && strstr(recs, "HEAD term"),
                   "case %d: the HEAD term refused:\n%s", c, recs);
    }
    printf("  HEAD terms: i as the trip began, at a join and an inner header (R3c)\n");

    // a FREE edge over a MAX or MIN component (c41). (A) 4NestedWith3Variables, all i32: `while q > 0
    // { q = q + a - 1; (a, b) = (3a - 4b, 4a + 3b) }`, ranked <2^32 - |a| - |b|, q>, the first
    // component the MIN of the four rows 2^32 -+ a -+ b, the edge free: |a| + |b| grows unless
    // (a, b) = 0, where q falls. (0) rank.lex 0 .. 2 proved; (1) the rotation made the reflection
    // (b, a), which keeps |a| + |b| while q may rise: a rank.lex not implied; (2) the MAX of the four
    // rows (2^32 + |a| + |b|, rising): a rank.lex not implied. (B) a region's tuple with a MAX: `while
    // x != 0 and y >= 0 { if y > 0 { if x > 0 { x-- } else { x++ } } else { y = 1 } }`, regions
    // y >= 1 with <max(x, -x)> and -y >= 0 with <y>. (3) its rank.cover, rank.region and four rank.lex
    // proved; (4) the else path x + 2 (-1 goes to 1): a rank.lex not implied. (5) 4NestedWith3Variables
    // ranked as Main Compiler Work's f11 does, TWO groups: <min(2^32 -+ a -+ b), max(q, -q)>: rank.lex
    // 0 .. 2 proved; (6) the second group a MIN (-|q|, rising as q falls): a rank.lex not implied
    for (int c = 0; c < 7; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        Stub sn; memset(&sn, 0, sizeof sn);
        sn.propose = P_RANK; sn.nrank = 1;
        JRank *rr = &sn.rank[0];
        IrFunc *nf;
        IrBlock *latch;
        if (c < 3 || c >= 5) {
            nf = ir_func_new(&A, nm("nest41"), mi32, IR_FUNC_PURE);
            IrValue *a0p = ir_add_param(nf, mi32, nm("a0")), *b0p = ir_add_param(nf, mi32, nm("b0")), *q0p = ir_add_param(nf, mi32, nm("q0"));
            IrBlock *n0 = nf->entry, *n1 = ir_new_block(nf), *n2 = ir_new_block(nf), *n3 = ir_new_block(nf);
            IrValue *na = ir_alloca(nf, n0, mi32), *nb = ir_alloca(nf, n0, mi32), *nq = ir_alloca(nf, n0, mi32);
            ir_store(nf, n0, na, a0p); ir_store(nf, n0, nb, b0p); ir_store(nf, n0, nq, q0p); ir_set_br(n0, n1);
            IrValue *lq = ir_load(nf, n1, nq, mi32);
            ir_set_br_cond(n1, ir_icmp(nf, n1, IR_CMP_SGT, lq, ir_const_int(nf, n1, 0, mi32)), n2, n3);
            IrValue *la = ir_load(nf, n2, na, mi32), *lb = ir_load(nf, n2, nb, mi32);
            IrValue *q1 = ir_binop(nf, n2, IR_SUB, ir_binop(nf, n2, IR_ADD, lq, la, mi32), ir_const_int(nf, n2, 1, mi32), mi32);
            IrValue *k3 = ir_const_int(nf, n2, 3, mi32), *k4 = ir_const_int(nf, n2, 4, mi32);
            IrValue *a1 = ir_binop(nf, n2, IR_SUB, ir_binop(nf, n2, IR_MUL, k3, la, mi32), ir_binop(nf, n2, IR_MUL, k4, lb, mi32), mi32);
            IrValue *b1 = ir_binop(nf, n2, IR_ADD, ir_binop(nf, n2, IR_MUL, k4, la, mi32), ir_binop(nf, n2, IR_MUL, k3, lb, mi32), mi32);
            ir_store(nf, n2, nq, q1); ir_store(nf, n2, na, c == 1 ? lb : a1); ir_store(nf, n2, nb, c == 1 ? la : b1); ir_set_br(n2, n1);
            ir_set_ret(n3, ir_const_int(nf, n3, 0, mi32));
            ir_finalize_cfg(nf);
            latch = n2;
            rr->blk = n1->id; rr->fn = nf; rr->nt = 3; rr->key[0] = na->id; rr->key[1] = nb->id; rr->key[2] = nq->id;
            rr->nedge = 1; rr->from[0] = n2->id; rr->fall[0] = -1; rr->ncomp = c >= 5 ? 6 : 5;
            for (int k = 0; k < 4; k++) {                      // 2^32 - a - b, - a + b, + a - b, + a + b
                rr->coef[k][0] = (k & 2) ? 1 : -1; rr->coef[k][1] = (k & 1) ? 1 : -1; rr->c0[k] = (__int128)1 << 32;
                if (k) rr->cont[k] = c == 2 ? 1 : 2;
            }
            rr->coef[4][2] = 1;                                // q
            if (c >= 5) { rr->coef[5][2] = -1; rr->cont[5] = c == 5 ? 1 : 2; }   // max(q, -q), or min
        } else {
            nf = ir_func_new(&A, nm("reg41"), mi32, IR_FUNC_PURE);
            IrValue *x0p = ir_add_param(nf, mi32, nm("x0")), *y0p = ir_add_param(nf, mi32, nm("y0"));
            IrBlock *r0 = nf->entry, *r1 = ir_new_block(nf), *r2 = ir_new_block(nf), *r3 = ir_new_block(nf), *r4 = ir_new_block(nf),
                    *r5 = ir_new_block(nf), *r6 = ir_new_block(nf), *r7 = ir_new_block(nf), *r8 = ir_new_block(nf), *r9 = ir_new_block(nf);
            IrValue *rx = ir_alloca(nf, r0, mi32), *ry = ir_alloca(nf, r0, mi32);
            ir_store(nf, r0, rx, x0p); ir_store(nf, r0, ry, y0p); ir_set_br(r0, r1);
            IrValue *lx = ir_load(nf, r1, rx, mi32);
            ir_set_br_cond(r1, ir_icmp(nf, r1, IR_CMP_NE, lx, ir_const_int(nf, r1, 0, mi32)), r2, r9);
            IrValue *ly = ir_load(nf, r2, ry, mi32);
            ir_set_br_cond(r2, ir_icmp(nf, r2, IR_CMP_SGE, ly, ir_const_int(nf, r2, 0, mi32)), r3, r9);
            ir_set_br_cond(r3, ir_icmp(nf, r3, IR_CMP_SGT, ly, ir_const_int(nf, r3, 0, mi32)), r4, r7);
            ir_set_br_cond(r4, ir_icmp(nf, r4, IR_CMP_SGT, lx, ir_const_int(nf, r4, 0, mi32)), r5, r6);
            ir_store(nf, r5, rx, ir_binop(nf, r5, IR_SUB, lx, ir_const_int(nf, r5, 1, mi32), mi32)); ir_set_br(r5, r8);
            ir_store(nf, r6, rx, ir_binop(nf, r6, IR_ADD, lx, ir_const_int(nf, r6, c == 4 ? 2 : 1, mi32), mi32)); ir_set_br(r6, r8);
            ir_store(nf, r7, ry, ir_const_int(nf, r7, 1, mi32)); ir_set_br(r7, r8);
            ir_set_br(r8, r1);
            ir_set_ret(r9, ir_const_int(nf, r9, 0, mi32));
            ir_finalize_cfg(nf);
            latch = r8;
            rr->blk = r1->id; rr->fn = nf; rr->nt = 2; rr->key[0] = rx->id; rr->key[1] = ry->id;
            rr->nedge = 1; rr->from[0] = r8->id; rr->fall[0] = -1; rr->ncomp = 5;
            rr->coef[0][1] = 1; rr->c0[0] = -1;                // y - 1 >= 0: <max(x, -x)>
            rr->coef[1][0] = 1; rr->coef[2][0] = -1; rr->cont[2] = 1;
            rr->coef[3][1] = -1;                               // -y >= 0: <y>
            rr->coef[4][1] = 1;
            rr->nreg = 2; rr->reg[0].g0 = 0; rr->reg[0].ng = 1; rr->reg[0].t0 = 1; rr->reg[0].nt = 2;
            rr->reg[1].g0 = 3; rr->reg[1].ng = 1; rr->reg[1].t0 = 4; rr->reg[1].nt = 1;
        }
        CHECK(run(nf, &sn, true, sizeof region, err), "judges: %s", err);
        char pl[48], nl[48];                                  // a record names its kind, not its ordinal
        snprintf(pl, sizeof pl, "proved bb%d.%d rank.", latch->id, nins_m(latch)); snprintf(nl, sizeof nl, "not-implied bb%d.%d rank.lex", latch->id, nins_m(latch));
        int nrk = 0;                                           // every rank.* record, whatever its verdict
        for (const char *l = recs; (l = strstr(l, " rank.")); l++) nrk++;
        if (c == 0 || c == 5) CHECK(count_in(recs, pl) == 3 && nrk == 3 && count_in(recs, "rank.lex") == 3, "case %d, 4NestedWith3Variables: rank.lex 0 .. 2 proved:\n%s", c, recs);
        else if (c == 3) CHECK(count_in(recs, pl) == 6 && nrk == 6 && count_in(recs, "rank.lex") == 4, "a region's max: cover, region and four rank.lex proved:\n%s", recs);
        else CHECK(strstr(recs, nl) != NULL, "case %d: a rank.lex not implied:\n%s", c, recs);
    }

    // a RECURSION's measure of several rows (c42), every call free (rec.lex), all i32. (0) Ex5,
    // `f(x, y)`: y == 0 returns; x == 0 calls f(y, y - 1), else f(y, x - 1); MAX(2x, 2y + 1) + 2^33:
    // both calls' rec.lex 0 and 1 proved; (1) the MIN of the same rows: a rec.lex not implied. (2)
    // RecursiveMultiplication, `mult(n, m)`: m < 0 calls mult(n, -m), m == 0 returns, else mult(n,
    // m - 1); MAX(2m, 1 - 2m): proved; (3) the m < 0 call passing m: not implied (it never ends).
    // (4) Parts, `parts(p, q)`: p <= 0 or q <= 0 return; q > p calls parts(p, p), else parts(p - q,
    // q) and parts(p, q - 1); <p, q>: its three calls' rec.lex 0 .. 2 proved; (5) <p, -q>: not implied
    for (int c = 0; c < 6; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        const char *fnm = c < 2 ? "ex5_42" : c < 4 ? "mult42" : "parts42";
        IrFunc *rf = ir_func_new(&A, nm(fnm), mi32, IR_FUNC_PURE);
        IrValue *x = ir_add_param(rf, mi32, nm("x")), *y = ir_add_param(rf, mi32, nm("y"));
        IrBlock *q0 = rf->entry, *q1 = ir_new_block(rf), *q2 = ir_new_block(rf), *q3 = ir_new_block(rf), *q4 = ir_new_block(rf), *q5 = ir_new_block(rf);
        IrBlock *cb[3]; int ci[3], ncall = 0;
        #define K(b, v) ir_const_int(rf, (b), (v), mi32)
        #define CALL2(b, a0, a1) do { IrValue *a0_ = (a0), *a1_ = (a1); cb[ncall] = (b); ci[ncall] = nins_m(b); \
                                     IrInstr *k_ = ir_instr(rf, IR_CALL, mi32, 2); k_->aux.callee = nm(fnm); \
                                     k_->operands[0] = a0_; k_->operands[1] = a1_; ir_emit((b), k_); ncall++; } while (0)
        if (c < 2) {
            ir_set_br_cond(q0, ir_icmp(rf, q0, IR_CMP_EQ, y, K(q0, 0)), q1, q2);
            ir_set_ret(q1, x);
            ir_set_br_cond(q2, ir_icmp(rf, q2, IR_CMP_EQ, x, K(q2, 0)), q3, q4);
            CALL2(q3, y, ir_binop(rf, q3, IR_SUB, y, K(q3, 1), mi32)); ir_set_ret(q3, K(q3, 0));
            CALL2(q4, y, ir_binop(rf, q4, IR_SUB, x, K(q4, 1), mi32)); ir_set_ret(q4, K(q4, 0));
            ir_set_ret(q5, K(q5, 0));
        } else if (c < 4) {                                    // x is n, y is m
            ir_set_br_cond(q0, ir_icmp(rf, q0, IR_CMP_SLT, y, K(q0, 0)), q1, q2);
            CALL2(q1, x, c == 3 ? y : ir_binop(rf, q1, IR_SUB, K(q1, 0), y, mi32)); ir_set_ret(q1, K(q1, 0));
            ir_set_br_cond(q2, ir_icmp(rf, q2, IR_CMP_EQ, y, K(q2, 0)), q3, q4);
            ir_set_ret(q3, K(q3, 0));
            CALL2(q4, x, ir_binop(rf, q4, IR_SUB, y, K(q4, 1), mi32)); ir_set_ret(q4, K(q4, 0));
            ir_set_ret(q5, K(q5, 0));
        } else {                                               // x is p, y is q
            ir_set_br_cond(q0, ir_icmp(rf, q0, IR_CMP_SLE, x, K(q0, 0)), q1, q2);
            ir_set_ret(q1, K(q1, 1));
            ir_set_br_cond(q2, ir_icmp(rf, q2, IR_CMP_SLE, y, K(q2, 0)), q5, q3);
            ir_set_ret(q5, K(q5, 0));
            IrBlock *q6 = ir_new_block(rf);
            ir_set_br_cond(q3, ir_icmp(rf, q3, IR_CMP_SGT, y, x), q4, q6);
            CALL2(q4, x, x); ir_set_ret(q4, K(q4, 0));
            CALL2(q6, ir_binop(rf, q6, IR_SUB, x, y, mi32), y);
            CALL2(q6, x, ir_binop(rf, q6, IR_SUB, y, K(q6, 1), mi32)); ir_set_ret(q6, K(q6, 0));
        }
        #undef CALL2
        #undef K
        ir_finalize_cfg(rf);
        Stub sr; memset(&sr, 0, sizeof sr);
        sr.propose = P_RANK; sr.nrank = 1;
        JRank *rr = &sr.rank[0];
        rr->blk = JRANK_ENTRY; rr->fn = rf; rr->nt = 2; rr->key[0] = x->id; rr->key[1] = y->id; rr->ncomp = 2;
        const __int128 T33 = (__int128)1 << 33;
        if (c < 2) { rr->coef[0][0] = 2; rr->c0[0] = T33; rr->coef[1][1] = 2; rr->c0[1] = T33 + 1; rr->cont[1] = c == 1 ? 2 : 1; }
        else if (c < 4) { rr->coef[0][1] = 2; rr->coef[1][1] = -2; rr->c0[1] = 1; rr->cont[1] = 1; }
        else { rr->coef[0][0] = 1; rr->coef[1][1] = c == 5 ? -1 : 1; }
        CHECK(run(rf, &sr, true, sizeof region, err), "judges: %s", err);
        int want = c < 4 ? 2 : 3, proved = 0, rec = 0;            // per call: rec.lex 0 .. want - 1
        for (int w = 0; w < ncall; w++) {
            char pl[64]; snprintf(pl, sizeof pl, "proved bb%d.%d rec.lex", cb[w]->id, ci[w]);
            proved += count_in(recs, pl);
        }
        for (const char *l = recs; (l = strstr(l, " rec.")); l++) { const char *b = l; while (b > recs && b[-1] != '\n') b--; if (!strncmp(b, "proved", 6) || !strncmp(b, "not-implied", 11)) rec++; }
        bool good = c == 0 || c == 2 || c == 4;
        if (good) CHECK(proved == ncall * want && rec == ncall * want && !strstr(recs, " rec.fall") && !strstr(recs, " rec.bound"),
                        "case %d: every call's rec.lex 0 .. %d proved (%d of %d):\n%s", c, want - 1, proved, ncall * want, recs);
        else CHECK(strstr(recs, "not-implied") && proved < ncall * want, "case %d: a rec.lex not implied:\n%s", c, recs);
        if (!good) {                                            // the failure is a rec.lex's, not only an overflow's
            bool nl = false;
            for (int w = 0; w < ncall; w++) { char k2[64]; snprintf(k2, sizeof k2, "not-implied bb%d.%d rec.lex", cb[w]->id, ci[w]); nl |= strstr(recs, k2) != NULL; }
            CHECK(nl, "case %d: the rec.lex not implied is the measure's:\n%s", c, recs);
        }
    }

    // a RECURSION's measure in REGIONS (c43), all i32. (0) TwoWay, `tw(t, n)`: n < 0 returns; m = n
    // - 1 where t != 0, else n + 1; calls tw(t, m); regions t >= 1 with <n>, t <= -1 with <n>, t = 0
    // with <2^31 - 1 - n>: its eleven VCs at the call proved; (1) the call passing 1 - t (t = 1 goes
    // to t = 0, a region up): rec.region not implied. (2) RecursiveMultiplication, `mult(n, m)`:
    // regions m >= 0 with <m>, then m <= -1 with <-m> (its one step from region 1 to 0 lowers rho):
    // both calls' six VCs proved; (3) the m < 0 call passing m: a rec.lex not implied
    for (int c = 0; c < 4; c++) {
        IrType *mi32 = ir_type_int(&A, 32, true);
        const char *fnm = c < 2 ? "tw43" : "mult43";
        IrFunc *rf = ir_func_new(&A, nm(fnm), mi32, IR_FUNC_PURE);
        IrValue *x = ir_add_param(rf, mi32, nm("x")), *y = ir_add_param(rf, mi32, nm("y"));
        IrBlock *q0 = rf->entry, *q1 = ir_new_block(rf), *q2 = ir_new_block(rf), *q3 = ir_new_block(rf), *q4 = ir_new_block(rf), *q5 = ir_new_block(rf);
        IrBlock *cb[2]; int ci[2], ncall = 0;
        #define K(b, v) ir_const_int(rf, (b), (v), mi32)
        #define CALL2(b, a0, a1) do { IrValue *a0_ = (a0), *a1_ = (a1); cb[ncall] = (b); ci[ncall] = nins_m(b); \
                                     IrInstr *k_ = ir_instr(rf, IR_CALL, mi32, 2); k_->aux.callee = nm(fnm); \
                                     k_->operands[0] = a0_; k_->operands[1] = a1_; ir_emit((b), k_); ncall++; } while (0)
        if (c < 2) {                                           // x is t, y is n
            IrValue *mc = ir_alloca(rf, q0, mi32);
            ir_set_br_cond(q0, ir_icmp(rf, q0, IR_CMP_SLT, y, K(q0, 0)), q1, q2);
            ir_set_ret(q1, K(q1, 1));
            ir_set_br_cond(q2, ir_icmp(rf, q2, IR_CMP_NE, x, K(q2, 0)), q3, q4);
            ir_store(rf, q3, mc, ir_binop(rf, q3, IR_SUB, y, K(q3, 1), mi32)); ir_set_br(q3, q5);
            ir_store(rf, q4, mc, ir_binop(rf, q4, IR_ADD, y, K(q4, 1), mi32)); ir_set_br(q4, q5);
            IrValue *lm = ir_load(rf, q5, mc, mi32);
            CALL2(q5, c == 1 ? ir_binop(rf, q5, IR_SUB, K(q5, 1), x, mi32) : x, lm); ir_set_ret(q5, K(q5, 0));
        } else {                                               // x is n, y is m
            ir_set_br_cond(q0, ir_icmp(rf, q0, IR_CMP_SLT, y, K(q0, 0)), q1, q2);
            CALL2(q1, x, c == 3 ? y : ir_binop(rf, q1, IR_SUB, K(q1, 0), y, mi32)); ir_set_ret(q1, K(q1, 0));
            ir_set_br_cond(q2, ir_icmp(rf, q2, IR_CMP_EQ, y, K(q2, 0)), q3, q4);
            ir_set_ret(q3, K(q3, 0));
            CALL2(q4, x, ir_binop(rf, q4, IR_SUB, y, K(q4, 1), mi32)); ir_set_ret(q4, K(q4, 0));
            ir_set_ret(q5, K(q5, 0));
        }
        #undef CALL2
        #undef K
        ir_finalize_cfg(rf);
        Stub sr; memset(&sr, 0, sizeof sr);
        sr.propose = P_RANK; sr.nrank = 1;
        JRank *rr = &sr.rank[0];
        rr->blk = JRANK_ENTRY; rr->fn = rf; rr->nt = 2; rr->key[0] = x->id; rr->key[1] = y->id;
        if (c < 2) {
            rr->ncomp = 7; rr->nreg = 3;
            rr->coef[0][0] = 1; rr->c0[0] = -1; rr->coef[1][1] = 1;
            rr->coef[2][0] = -1; rr->c0[2] = -1; rr->coef[3][1] = 1;
            rr->coef[4][0] = 1; rr->coef[5][0] = -1; rr->coef[6][1] = -1; rr->c0[6] = 2147483647;
            rr->reg[0].g0 = 0; rr->reg[0].ng = 1; rr->reg[0].t0 = 1; rr->reg[0].nt = 1;
            rr->reg[1].g0 = 2; rr->reg[1].ng = 1; rr->reg[1].t0 = 3; rr->reg[1].nt = 1;
            rr->reg[2].g0 = 4; rr->reg[2].ng = 2; rr->reg[2].t0 = 6; rr->reg[2].nt = 1;
        } else {
            rr->ncomp = 4; rr->nreg = 2;
            rr->coef[0][1] = 1; rr->coef[1][1] = 1;                // m >= 0: <m>
            rr->coef[2][1] = -1; rr->c0[2] = -1; rr->coef[3][1] = -1;   // m <= -1: <-m>
            rr->reg[0].g0 = 0; rr->reg[0].ng = 1; rr->reg[0].t0 = 1; rr->reg[0].nt = 1;
            rr->reg[1].g0 = 2; rr->reg[1].ng = 1; rr->reg[1].t0 = 3; rr->reg[1].nt = 1;
        }
        CHECK(run(rf, &sr, true, sizeof region, err), "judges: %s", err);
        int want = c < 2 ? 11 : 6, proved = 0, rec = 0;
        for (int w = 0; w < ncall; w++) {
            char pl[64]; snprintf(pl, sizeof pl, "proved bb%d.%d rec.", cb[w]->id, ci[w]);
            proved += count_in(recs, pl);
        }
        for (const char *l = recs; (l = strstr(l, " rec.")); l++) { const char *b = l; while (b > recs && b[-1] != '\n') b--; if (!strncmp(b, "proved", 6) || !strncmp(b, "not-implied", 11)) rec++; }
        bool good = c == 0 || c == 2;
        if (good) CHECK(proved == ncall * want && rec == ncall * want, "case %d: every call's %d VCs proved (%d of %d):\n%s", c, want, proved, ncall * want, recs);
        else {
            char k2[64]; snprintf(k2, sizeof k2, "not-implied bb%d.%d rec.%s", cb[0]->id, ci[0], c == 1 ? "region" : "lex");
            CHECK(strstr(recs, k2) != NULL, "case %d: a rec.%s not implied:\n%s", c, c == 1 ? "region" : "lex", recs);
        }
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
