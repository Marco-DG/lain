// src/tools/judge_vc_driver.c — phase 1a of the proof engine's judge: the VCs the generator
// (src/judge/gen.h) states for every obligation of a Lain file, beside VRA's verdict on the same
// instruction. Nothing here decides anything: the judge has no kernel yet. The untrusted solver
// runs only to PROPOSE claims at forward joins and loop headers (propose_claims below), which the
// generator then owes as obligations of their own (`hull`, `inv.entry`, `inv.keep`);
// `--no-claims` proposes none.
//
//   gcc -std=c99 -O2 -o /tmp/jvc src/tools/judge_vc_driver.c -I src
//   /tmp/jvc FILE.ln            each obligation: a header line, then its VC in the text form; a
//                               claim's (`# claim on the edge bbP->bbH`) after the function's
//                               own, and a last line counting the claims proposed and kept
//   --claims                    also each claim kept, as a comment line
//   --no-claims, --no-finder    propose no claim; or none from VRA's header states (PE.4)
//   /tmp/jvc FILE.ln --smt      the same, as SMT-LIB (QF_LIA), for an offline solver
//   /tmp/jvc FILE.ln --stats    one TSV line per obligation: function, point, kind, ordinal,
//                               VRA's verdict, outside or stated, #vars, #hyps
//   /tmp/jvc FILE.ln --check-hyps
//                               run the program on the interpreter and, each time an obligation's
//                               instruction is reached, evaluate every hypothesis of its VC at the
//                               values the program has there. The generator is TRUSTED: a false
//                               hypothesis is a generator bug (or the interpreter's), never noise.
//                               One line per false hypothesis, then a summary line, on STDERR:
//                               the program's own output is on stdout and need not end a line.
//                               A claim's obligation is checked when the program takes its edge,
//                               and so is the CLAIM, its goal: a claim is a fact wherever its
//                               join's version is named (its VCs implied, judge_gate.sh).
//
// VRA's verdict is read from its checks on the same instruction: `proved` when VRA discharged the
// matching half, `refused` when it did not, `none` when VRA states no obligation there.
// A driver, not the judge: it may include src/analysis/ (the include rules bind src/judge/).
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/file.h"
#include "utils/common/system.h"
#include "utils/panic.h"
#include <unistd.h>
#include "frontends/lain/lexer.h"
#include "frontends/lain/parser.h"
#include "frontends/lain/ast.h"
#include "frontends/lain/module.h"
#include "target.h"
#include "frontends/lain/sema.h"
#include "frontends/lain/lower.h"
#include "analysis/vra.h"
#include "ir/emit_c.h"     // interp.h reads ir_iface_mark from it, as main.c includes them
#include "ir/interp.h"
#include "judge/gen.h"
#include "judge/text.h"
#include "solver/lia.h"    // the proposer's side only: propose_claims

// The module name as the compiler makes it (main.c, filepath_to_modname): one dotted segment per
// path component, each an identifier. The program itself is read from the path as given
// (module_root_file). The plainer name vra_driver.c uses made a program that names its own
// module's declarations fail with E105.
static char *drv_modname(Arena *a, const char *path) {
    const char *p = path;
    while ((p[0]=='.' && (p[1]=='/'||p[1]=='\\')) || p[0]=='/' || p[0]=='\\')
        p += (p[0]=='/'||p[0]=='\\') ? 1 : 2;
    size_t n = strlen(p), end = (n>3 && strcmp(p+n-3,".ln")==0) ? n-3 : n;
    char *t = arena_push_many(a, char, 2*end+1);
    size_t o = 0; bool seg_start = true;
    for (size_t i = 0; i < end; i++) {
        char c = p[i];
        if (c == '/' || c == '\\') { t[o++] = '.'; seg_start = true; continue; }
        bool ident = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (seg_start && c >= '0' && c <= '9') t[o++] = '_';
        t[o++] = ident ? c : '_';
        seg_start = false;
    }
    t[o] = '\0';
    return t;
}

// VRA's verdict on the half of an obligation the judge states. VRA keeps one check per
// instruction and kind; its lo_ok/hi_ok split the bounds check, `shift` and `refine` qualify an
// overflow check. A judge kind with no VRA counterpart at this instruction is `none`.
static const char *vra_verdict(Vra *V, JObl *o) {
    const char *k = o->vc.id.kind ? o->vc.id.kind : "";
    for (int i = 0; i < V->nchecks; i++) {
        VraCheck *c = &V->checks[i];
        if (c->at != o->at) continue;
        if (!strcmp(k, "bounds.lo") && c->kind == VRA_BOUNDS) return c->lo_ok || c->ok ? "proved" : "refused";
        if (!strcmp(k, "bounds.hi") && c->kind == VRA_BOUNDS) return c->hi_ok || c->ok ? "proved" : "refused";
        if ((!strcmp(k, "divzero") || !strcmp(k, "divovf")) && c->kind == VRA_DIVZERO) return c->ok ? "proved" : "refused";
        if ((!strncmp(k, "narrow", 6) || !strcmp(k, "shift")) && c->kind == VRA_OVERFLOW) return c->ok ? "proved" : "refused";
    }
    return "none";
}

// ── --check-hyps ───────────────────────────────────────────────────────────────────────────
typedef struct { IrInstr *at; JObl *o; } HK;
static HK *hk; static int64_t nhk;
static long long reached, checked, falsec, unknown, premises, claimchk, claimgoals;
static int hk_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const HK *)a)->at, y = (uintptr_t)((const HK *)b)->at;
    return x < y ? -1 : x > y;
}
// A variable's value at the instruction, or false when it cannot be read (not an integer yet).
static bool hk_var(JObl *o, int32_t x, IVal *v, int nv, __int128 *out) {
    int32_t k = o->vkind[x], key = o->vkey[x];
    if (key < 0 || key >= nv) return false;
    // the obligation's own result does not exist yet: the hook runs before the instruction, and
    // the VC's variable for it is the exact value the instruction is about to settle
    if (k == 0 && o->at && o->at->result && key == o->at->result->id) return false;
    IVal *c = &v[key];
    if (k == 1 || k == 4) {
        // the version current at the point is the cell's content; an older one is the value its
        // store wrote, which the frame still holds (the store and that value dominate the point);
        // kind 4 is a slice cell version's length, read the same way
        if (o->vcur[x]) {
            if (c->k != IV_PTR || !c->p.o || !c->p.o->live) return false;
            c = ip_cell(&c->p, NULL);
        } else {
            if (o->vaux[x] < 0 || o->vaux[x] >= nv) return false;
            c = &v[o->vaux[x]];
        }
    }
    if (k == 2 || k == 4) { if (c->k != IV_SLICE) return false; *out = c->len; return true; }
    if (k == 3) {                                      // a fresh variable: its recipe
        const JRecipe *r = o->vrec ? o->vrec[x] : NULL;
        if (!r || r->den == 0) return false;
        __int128 s = 0;
        for (int i = 0; i < r->n; i++) {
            __int128 y, p;
            if (r->var[i] == x || !hk_var(o, r->var[i], v, nv, &y)) return false;
            if (__builtin_mul_overflow(r->coef[i], y, &p) || __builtin_add_overflow(s, p, &s)) return false;
        }
        if (s % r->den) return false;                  // not exact: the recipe does not apply
        *out = s / r->den; return true;
    }
    if (c->k != IV_INT) return false;
    const IrType *t = o->vtype[x];
    bool uns = t && ((t->kind == IRT_INT && !t->is_signed) || t->kind == IRT_BOOL);
    *out = uns ? (__int128)c->i : (__int128)(int64_t)c->i;
    return true;
}
static __int128 hk_jint(const JInt *x) {             // the generator's constants are below 2^66
    unsigned __int128 m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(__int128)m : (__int128)m;
}
// 1 true, 0 false, -1 not evaluable
static int hk_cons(JObl *o, const JCons *c, IVal *v, int nv) {
    __int128 s = 0;
    for (int i = 0; i < c->nt; i++) {
        __int128 x, p;
        if (!hk_var(o, c->t[i].var, v, nv, &x)) return -1;
        if (__builtin_mul_overflow(hk_jint(&c->t[i].coef), x, &p) || __builtin_add_overflow(s, p, &s)) return -1;
    }
    __int128 r = hk_jint(&c->rhs);
    return c->op == J_LE ? s <= r : c->op == J_EQ ? s == r : s != r;
}
// The predecessor each block was last entered from, per frame (keyed by the frame's values), from
// the interpreter's own transitions: an R7 join's hypothesis is the disjunct of the path taken.
typedef struct { const IVal *v; int32_t blk, pred; } HkLast;
#define HK_LAST (1 << 16)
static HkLast hk_lastpred[HK_LAST];
static uint32_t hk_lp_slot(const IVal *v, int32_t blk) {
    uint64_t x = (uint64_t)(uintptr_t)v * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)blk * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)(x >> 32) & (HK_LAST - 1);
}
static int32_t hk_pred_of(const IVal *v, int32_t blk) {
    const HkLast *e = &hk_lastpred[hk_lp_slot(v, blk)];
    return e->v == v && e->blk == blk ? e->pred : -1;       // a slot another entry took: unknown
}
// Every hypothesis of one obligation, at the values the program has where it is owed.
static void hk_check(JObl *o, IVal *v, int nv) {
    reached++;
    for (int32_t k = 0; k < o->vc.nhyps; k++) {
        const JHyp *y = &o->vc.hyps[k];
        int r;
        // a premise is the antecedent of an implication VC (divovf: x = i64.min ⊨ y != -1),
        // not a fact about the run
        if (!strncmp(y->origin, "premise ", 8)) { premises++; continue; }
        int32_t tp = o->hjoin && o->hjoin[k] >= 0 && o->hpred[k] ? hk_pred_of(v, o->hjoin[k]) : -1;
        int dk = -1;
        for (int d = 0; tp >= 0 && d < y->ndis && dk < 0; d++) if (o->hpred[k][d] == tp) dk = d;
        if (!y->disj) r = hk_cons(o, &y->cons, v, nv);
        else if (dk >= 0) {                                  // the join was entered from that one
            r = 1;
            for (int c = 0; c < y->dis[dk].nc && r != 0; c++) { int t = hk_cons(o, &y->dis[dk].c[c], v, nv); if (t == 0) r = 0; else if (t < 0) r = -1; }
        } else {
            r = 0;
            for (int d = 0; d < y->ndis && r != 1; d++) {
                int all = 1;
                for (int c = 0; c < y->dis[d].nc && all == 1; c++) { int t = hk_cons(o, &y->dis[d].c[c], v, nv); all = t == 1 ? 1 : t; }
                if (all == 1) r = 1; else if (all < 0) r = -1;
            }
        }
        if (r < 0) { unknown++; continue; }
        checked++;
        if (!strncmp(y->origin, "claim ", 6)) claimchk++;   // a claim stated after its join
        if (r == 1) continue;
        falsec++;
        fprintf(stderr, "FALSE %s %s %s %s h%d (%s):", o->vc.id.fn, o->vc.id.point, o->vc.id.kind, o->vc.id.ordinal, k + 1, y->origin);
        for (int32_t x = 0; x < o->vc.nvars; x++) {
            __int128 val;
            if (hk_var(o, x, v, nv, &val)) fprintf(stderr, " %s=%lld", o->vc.vars[x], (long long)val);
        }
        fprintf(stderr, "\n");
    }
}
// A claim's obligations, by the block whose END they are owed at, each on one edge out of it.
typedef struct { IrBlock *from; JObl *o; } HE;
static HE *he; static int64_t nhe; static long long edges;
static int he_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const HE *)a)->from, y = (uintptr_t)((const HE *)b)->from;
    return x < y ? -1 : x > y;
}
// The edge taken is the interpreter's own: the block whose end was last reached, then the block
// entered next in the same frame (ii_on_block), with nothing run between. A record left by a
// block that returned matches no claim (such a block has no edge to owe one on).
static IrBlock *hk_last; static IVal *hk_lastv;
static void hk_on_block(IrFunc *f, IrBlock *to, IVal *v, int nv) {
    (void)f;
    IrBlock *b = hk_last; IVal *bv = hk_lastv;
    hk_last = NULL;
    if (b && bv == v) { HkLast *e = &hk_lastpred[hk_lp_slot(v, to->id)]; e->v = v; e->blk = to->id; e->pred = b->id; }
    if (!b || bv != v || !nhe) return;
    HE key = { b, NULL };
    HE *h = bsearch(&key, he, (size_t)nhe, sizeof *he, he_cmp);
    if (!h) return;
    while (h > he && h[-1].from == b) h--;
    for (; h < he + nhe && h->from == b; h++) {
        if (h->o->to != to) continue;
        edges++; hk_check(h->o, v, nv);
        int r = hk_cons(h->o, &h->o->vc.goal, v, nv);
        if (r < 0) { unknown++; continue; }
        claimgoals++;
        if (r == 1) continue;
        falsec++;
        fprintf(stderr, "FALSE %s %s %s %s goal (the claim, on the edge bb%d->bb%d):", h->o->vc.id.fn, h->o->vc.id.point,
                h->o->vc.id.kind, h->o->vc.id.ordinal, b->id, to->id);
        for (int32_t x = 0; x < h->o->vc.nvars; x++) {
            __int128 val;
            if (hk_var(h->o, x, v, nv, &val)) fprintf(stderr, " %s=%lld", h->o->vc.vars[x], (long long)val);
        }
        fprintf(stderr, "\n");
    }
}
static void hk_on_instr(IrFunc *f, IrBlock *b, IrInstr *i, IVal *v, int nv) {
    (void)f;
    if (!i) { hk_last = b; hk_lastv = v; return; }  // the block's end: its edge is known at the next entry
    if (!nhk) return;
    HK key = { i, NULL };
    HK *h = bsearch(&key, hk, (size_t)nhk, sizeof *hk, hk_cmp);
    if (!h) return;
    while (h > hk && h[-1].at == i) h--;
    for (; h < hk + nhk && h->at == i; h++) hk_check(h->o, v, nv);
}

// ── VRA's converged loop-header states (PE.4's sink), the FINDER's candidates ──────────────────
// Kept per function and header, the first report only (the analysis may run twice).
typedef struct { IrFunc *f; int bid; VraRel *r; int n; } HdrRel;
static HdrRel *hdr; static int nhdr, chdr;
static void hdr_sink(void *ctx, IrFunc *f, int bid, const VraRel *r, int n) {
    (void)ctx;
    for (int i = 0; i < nhdr; i++) if (hdr[i].f == f && hdr[i].bid == bid) return;
    if (nhdr == chdr) {
        int nc = chdr ? 2 * chdr : 16;
        HdrRel *x = realloc(hdr, sizeof *x * (size_t)nc);
        if (!x) return;                                      // a candidate lost: precision only
        hdr = x; chdr = nc;
    }
    VraRel *c = malloc(sizeof *c * (size_t)(n ? n : 1));
    if (!c) return;
    memcpy(c, r, sizeof *c * (size_t)n);
    hdr[nhdr].f = f; hdr[nhdr].bid = bid; hdr[nhdr].r = c; hdr[nhdr].n = n; nhdr++;
}

// ── claims: the PROPOSER (untrusted; phase 1a's stand-in for the finder and solver) ─────────────
// Candidates: at a forward join, each side of each fresh version; at a loop header, each side of
// each header version and of its difference with every other term the header may name
// (jgen_terms). A candidate's bound is the largest value its objective takes over the paths INTO
// the block (lia_maximize, PE.2): on a back edge that is one trip on from the candidates at their
// placeholder 0, a guess like any other here (an entry-only bound measured the same over the
// corpus, claims and time). One query asks first whether the types' extreme is reached, and then
// nothing is claimed. A header's own comparisons
// add candidates with a fixed bound: `i < 2` proposes `i <= 1`, `i <= 2` and `i <= 3`, each side,
// since the bound an induction keeps is the guard's give or take a step. VRA's converged state at
// the header (PE.4) adds its relations as fixed candidates too, those whose terms a claim there may
// name, one of them a version the header makes.
// Nothing here is believed: the generator owes every claim on every edge into its block, back
// edges included, and HOUDINI drops a claim one of those VCs is not implied by, and solves the rest
// again, until none is: what survives is inductive together. Bounds are computed in ROUNDS, each
// under the claims the previous round verified only (a candidate not yet verified may contradict
// another, and a bound computed under it would be vacuous): round 1 under none, then until a
// round verifies nothing new, three at most. A round's verified set contains the last one's.
#define CL_REGION ((size_t)1 << 26)
static char *cl_work;
static const JWork cl_lim = { 100000, 4096, 4096 };    // solve_vc's
static long long cl_proposed, cl_kept, cl_dropped;
typedef struct { JClaim c; __int128 tb; bool fixed; } JCand;
static bool cand_push(JCand **cd, int32_t *n, int32_t *cap, int32_t blk, int32_t k0, int c0, int32_t k1, int c1,
                      const IrType *t0, const IrType *t1) {
    JRange r0, r1;
    if (!jg_range(t0, &r0) || (k1 >= 0 && !jg_range(t1, &r1))) return true;   // not an integer: no candidate
    if (*n == *cap) {
        int32_t nc = *cap ? *cap * 2 : 32;
        JCand *x = realloc(*cd, sizeof *x * (size_t)nc);
        if (!x) return false;
        *cd = x; *cap = nc;
    }
    JCand *c = &(*cd)[(*n)++]; memset(c, 0, sizeof *c);
    c->c.blk = blk; c->c.nt = k1 >= 0 ? 2 : 1;
    c->c.key[0] = k0; c->c.coef[0] = c0; c->tb = c0 > 0 ? r0.hi : -r0.lo;
    if (k1 >= 0) { c->c.key[1] = k1; c->c.coef[1] = c1; c->tb += c1 > 0 ? r1.hi : -r1.lo; }
    return true;
}
static bool same_form(const JClaim *a, const JClaim *b) {
    if (a->blk != b->blk || a->nt != b->nt) return false;
    for (int k = 0; k < a->nt; k++) if (a->key[k] != b->key[k] || a->coef[k] != b->coef[k]) return false;
    return true;
}
// Houdini over cl[0..nc): the claims left, in order, or -1 when the generator refuses them.
static int32_t houdini(IrFunc *f, char *buf, size_t cap, JClaim *cl, int32_t nc, char *err, size_t errcap) {
    for (bool again = nc > 0; again; ) {
        again = false;
        JMem m = jmem_over(buf, cap);
        JObl *o; int32_t n;
        if (!jgen_func(&m, f, cl, nc, JGEN_CLAIMS, &o, &n, err, errcap)) return -1;
        uint8_t *bad = calloc((size_t)nc, 1);
        if (!bad) { snprintf(err, errcap, "out of memory"); return -1; }
        for (int32_t q = 0; q < n; q++)
            if (!bad[o[q].claim] && lia_solve(&o[q].vc, cl_lim, cl_work, CL_REGION).k != J_ANS_CERT) bad[o[q].claim] = 1;
        int32_t keep = 0;
        for (int32_t ci = 0; ci < nc; ci++) if (!bad[ci]) cl[keep++] = cl[ci]; else { cl_dropped++; again = true; }
        nc = keep;
        free(bad);
    }
    return nc;
}
// One block's candidates (the slots sl[i..j), all at one block), into `cd`.
static bool block_candidates(IrFunc *f, char *buf, size_t cap, const JSlot *sl, int32_t i, int32_t j,
                             JCand **cd, int32_t *ncd, int32_t *ccd, char *err, size_t errcap) {
    int32_t H = sl[i].blk, nt = 0;
    bool head = sl[i].head;
    JSlot *tm = NULL;
    *ncd = 0;
    if (head) {
        JMem m = jmem_over(buf, cap);
        JSlot *t0;
        if (!jgen_terms(&m, f, H, &t0, &nt, err, errcap)) return false;
        if (!(tm = malloc(sizeof *tm * (size_t)(nt ? nt : 1)))) goto oom;
        memcpy(tm, t0, sizeof *tm * (size_t)nt);
    }
    for (int32_t k = i; k < j; k++) {
        if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, -1, 0, sl[k].type, NULL) ||
            !cand_push(cd, ncd, ccd, H, sl[k].key, -1, -1, 0, sl[k].type, NULL)) goto oom;
        for (int32_t t = 0; t < nt; t++) {
            if (tm[t].key == sl[k].key) continue;
            bool own = false;                                // two of the header's versions: one pair
            for (int32_t k2 = i; k2 < j; k2++) if (sl[k2].key == tm[t].key) own = true;
            if (own && tm[t].key < sl[k].key) continue;
            if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, tm[t].key, -1, sl[k].type, tm[t].type) ||
                !cand_push(cd, ncd, ccd, H, sl[k].key, -1, tm[t].key, 1, sl[k].type, tm[t].type)) goto oom;
        }
    }
    // a header's comparisons: a header version against a constant or another term, each side at
    // the compared value give or take one (fixed bounds, nothing to maximize)
    IrBlock *hb = NULL;
    for (IrBlock *b = f->blocks; head && b && !hb; b = b->next) if (b->id == H) hb = b;
    for (IrInstr *ic = hb ? hb->instrs : NULL; ic; ic = ic->next) {
        if (ic->op != IR_ICMP || ic->n_operands < 2) continue;
        int32_t key[2] = { -1, -1 }; bool isc[2] = { false, false }; __int128 cv[2] = { 0, 0 };
        const IrType *ty[2] = { NULL, NULL };
        for (int k = 0; k < 2; k++) {
            IrValue *o = ic->operands[k];
            IrInstr *d = NULL;
            for (IrInstr *x = hb->instrs; x && x != ic; x = x->next) if (x->result == o) d = x;
            if (d && d->op == IR_CONST) { isc[k] = true; cv[k] = jg_const(d); continue; }
            int32_t kk = d && d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0] ? d->operands[0]->id : o ? o->id : -1;
            for (int32_t t = 0; t < nt; t++) if (tm[t].key == kk) { key[k] = kk; ty[k] = tm[t].type; }
        }
        for (int k = 0; k < 2; k++) {
            int32_t x = -1;                                  // which header version this side is
            for (int32_t s2 = i; s2 < j; s2++) if (sl[s2].key == key[k]) x = s2;
            if (x < 0 || (!isc[1 - k] && key[1 - k] < 0)) continue;
            for (int d = -1; d <= 1; d++) {
                int32_t before = *ncd;
                bool ok = isc[1 - k]
                    ? cand_push(cd, ncd, ccd, H, key[k], 1, -1, 0, sl[x].type, NULL) && cand_push(cd, ncd, ccd, H, key[k], -1, -1, 0, sl[x].type, NULL)
                    : cand_push(cd, ncd, ccd, H, key[k], 1, key[1 - k], -1, sl[x].type, ty[1 - k]) &&
                      cand_push(cd, ncd, ccd, H, key[k], -1, key[1 - k], 1, sl[x].type, ty[1 - k]);
                if (!ok) goto oom;
                if (*ncd != before + 2) continue;
                (*cd)[before].fixed = (*cd)[before + 1].fixed = true;
                (*cd)[before].c.rhs = isc[1 - k] ? cv[1 - k] + d : d;
                (*cd)[before + 1].c.rhs = isc[1 - k] ? -(cv[1 - k] + d) : d;
            }
        }
    }
    // VRA's relations at this header
    for (int h = 0; head && h < nhdr; h++) {
        if (hdr[h].f != f || hdr[h].bid != H || hdr[h].n == 0) continue;
        int32_t *keys = malloc(sizeof *keys * (size_t)hdr[h].n * 2);
        uint8_t *kind = malloc((size_t)hdr[h].n * 2);
        if (!keys || !kind) { free(keys); free(kind); goto oom; }
        for (int r = 0; r < hdr[h].n; r++) { keys[2 * r] = hdr[h].r[r].a; keys[2 * r + 1] = hdr[h].r[r].b >= 0 ? hdr[h].r[r].b : hdr[h].r[r].a; }
        JMem m = jmem_over(buf, cap);
        if (!jgen_term_kinds(&m, f, H, keys, hdr[h].n * 2, kind, err, errcap)) { free(keys); free(kind); free(tm); return false; }
        for (int r = 0; r < hdr[h].n; r++) {
            const VraRel *v = &hdr[h].r[r];
            if (!kind[2 * r] || !kind[2 * r + 1] || (kind[2 * r] != 2 && kind[2 * r + 1] != 2)) continue;
            if ((v->sa != 1 && v->sa != -1) || (v->b >= 0 && v->sb != 1 && v->sb != -1)) continue;
            const IrType *ta = NULL, *tb = NULL;
            for (int32_t t = 0; t < nt; t++) { if (tm[t].key == v->a) ta = tm[t].type; if (tm[t].key == v->b) tb = tm[t].type; }
            for (int32_t k = i; k < j; k++) { if (sl[k].key == v->a) ta = sl[k].type; if (sl[k].key == v->b) tb = sl[k].type; }
            if (!ta || (v->b >= 0 && !tb)) continue;         // a term whose type the proposer cannot name
            int32_t before = *ncd;
            if (!cand_push(cd, ncd, ccd, H, v->a, v->sa, v->b, v->sb, ta, tb)) { free(keys); free(kind); goto oom; }
            if (*ncd == before + 1) { (*cd)[before].fixed = true; (*cd)[before].c.rhs = v->c; }
        }
        free(keys); free(kind);
    }
    free(tm);
    return true;
oom:
    free(tm); snprintf(err, errcap, "out of memory");
    return false;
}
static JClaim *propose_claims(IrFunc *f, char *buf, size_t cap, int32_t *ncl, char *err, size_t errcap) {
    *ncl = 0; err[0] = 0;
    JMem m = jmem_over(buf, cap);
    JSlot *s0; int32_t ns;
    if (!jgen_slots(&m, f, &s0, &ns, err, errcap) || ns == 0) return NULL;
    JSlot *sl = malloc(sizeof *sl * (size_t)ns);
    JClaim *cl = NULL, *as = NULL;                       // this round's claims; the verified ones
    JCand *cd = NULL;
    int32_t nc = 0, ccl = 0, nv = 0, ncd = 0, ccd = 0;
    if (!sl) goto oom;
    memcpy(sl, s0, sizeof *sl * (size_t)ns);
    for (int round = 0; round < 3; round++) {
        nc = 0;
        if (nv + 1 > ccl) { JClaim *x = realloc(cl, sizeof *x * (size_t)(nv + 1)); if (!x) goto oom; cl = x; ccl = nv + 1; }
        if (nv) memcpy(cl, as, sizeof *cl * (size_t)nv);
        nc = nv;
        for (int32_t i = 0, j; i < ns; i = j) {
            for (j = i; j < ns && sl[j].blk == sl[i].blk; j++) ;
            int32_t H = sl[i].blk;
            if (!block_candidates(f, buf, cap, sl, i, j, &cd, &ncd, &ccd, err, errcap)) goto fail;
            // the verified claims, then this block's candidates: the generator's obligations for them
            JClaim *q2 = malloc(sizeof *q2 * (size_t)(nv + ncd + 1));
            if (!q2) goto oom;
            if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
            for (int32_t c = 0; c < ncd; c++) q2[nv + c] = cd[c].c;
            m = jmem_over(buf, cap);
            JObl *o; int32_t n;
            bool gok = jgen_func(&m, f, q2, nv + ncd, H, &o, &n, err, errcap);
            free(q2);
            if (!gok) goto fail;
            for (int32_t c = 0; c < ncd; c++) {
                JClaim got = cd[c].c;
                if (!cd[c].fixed) {
                    __int128 best = 0;
                    bool ok = true, any = false;
                    for (int32_t q = 0; q < n && ok; q++) {
                        if (o[q].claim != nv + c) continue;
                        JVc v = o[q].vc;
                        jg_int(&v.goal.rhs, cd[c].tb - 1);
                        if (lia_solve(&v, cl_lim, cl_work, CL_REGION).k != J_ANS_CERT) { ok = false; break; }
                        LMax x = lia_maximize(&o[q].vc, o[q].vc.goal.t, o[q].vc.goal.nt, cl_lim, cl_work, CL_REGION);
                        if (x.k == LIA_MAX_EMPTY) continue;
                        if (x.k != LIA_MAX_BOUND) { ok = false; break; }
                        __int128 b = hk_jint(&x.b);
                        if (!any || b > best) best = b;
                        any = true;
                    }
                    if (!ok || !any) continue;
                    got.rhs = best;
                }
                // already verified as tight, or this very candidate already proposed (a tighter one
                // not yet verified does not make it redundant: it may fail)
                bool dup = false;
                for (int32_t v2 = 0; v2 < nv && !dup; v2++) dup = same_form(&as[v2], &got) && as[v2].rhs <= got.rhs;
                for (int32_t v2 = nv; v2 < nc && !dup; v2++) dup = same_form(&cl[v2], &got) && cl[v2].rhs == got.rhs;
                if (dup) continue;
                if (nc == ccl) { JClaim *x = realloc(cl, sizeof *x * (size_t)(ccl * 2)); if (!x) goto oom; cl = x; ccl *= 2; }
                cl[nc++] = got;
                cl_proposed++;
            }
        }
        if (nc == nv) break;                                 // nothing new to verify
        int32_t kept = houdini(f, buf, cap, cl, nc, err, errcap);
        if (kept < 0) goto fail;
        bool grew = kept > nv;
        free(as); as = malloc(sizeof *as * (size_t)(kept ? kept : 1));
        if (!as) goto oom;
        memcpy(as, cl, sizeof *as * (size_t)kept);
        nv = kept;
        if (!grew) break;
    }
    free(sl); free(cd); free(cl);
    cl_kept += nv;
    *ncl = nv;
    if (nv == 0) { free(as); return NULL; }
    return as;
oom:
    snprintf(err, errcap, "out of memory");
fail:
    free(sl); free(cd); free(cl); free(as);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ln> [--smt | --stats | --why | --check-hyps] [--no-claims]\n", argv[0]); return 2; }
    bool smt = false, stats = false, check = false, why = false, claims = true, show = false, finder = true;
    for (int k = 2; k < argc; k++) {
        if (!strcmp(argv[k], "--no-claims")) claims = false;
        if (!strcmp(argv[k], "--claims")) show = true;
        if (!strcmp(argv[k], "--no-finder")) finder = false;
        if (!strcmp(argv[k], "--smt")) smt = true;
        if (!strcmp(argv[k], "--stats")) stats = true;
        if (!strcmp(argv[k], "--check-hyps")) check = true;
        if (!strcmp(argv[k], "--why")) why = true;
    }
    Arena aa = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena sa = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    Arena ia = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    target_init_for(NULL);
    const char *path = argv[1];
    char *mod = drv_modname(&aa, path);
    module_root_file = path;
    DeclList *prog = load_module(&aa, mod);
    if (!prog) { fprintf(stderr, "load failed\n"); return 1; }
    sema_resolve_module(prog, mod, &sa);
    IrFunc *irmod = ir_lower_module(prog, &ia);
    vra_mod = irmod;
    jgen_module = irmod;                               // the generator's callees (its frame rule)
    size_t cap = (size_t)1 << 28, hcap = (size_t)1 << 26;
    char *buf = malloc(cap), *hbuf = malloc(hcap);
    cl_work = malloc(CL_REGION);
    if (!buf || !hbuf || !cl_work) { fprintf(stderr, "out of memory\n"); return 1; }
    int rc = 0;
    JMem m = jmem_over(buf, cap);                      // one region: --check-hyps keeps every VC
    for (IrFunc *f = irmod; f; f = f->next) {
        if (f->is_extern) continue;
        int32_t ncl = 0; char err[200]; err[0] = 0;
        if (claims && finder) {                              // VRA's header states, as candidates
            vra_header_sink = hdr_sink; vra_header_ctx = NULL;
            Vra *VH = vra_analyze(f);
            vra_header_sink = NULL;
            if (VH) vra_free(VH);
        }
        JClaim *cl = claims ? propose_claims(f, hbuf, hcap, &ncl, err, sizeof err) : NULL;
        for (int32_t c = 0; show && c < ncl; c++) {
            printf("# claim %.*s bb%d: %lld*%%%d", (int)f->name->length, f->name->name, cl[c].blk, (long long)cl[c].coef[0], cl[c].key[0]);
            if (cl[c].nt > 1) printf(" + %lld*%%%d", (long long)cl[c].coef[1], cl[c].key[1]);
            JInt r; jg_int(&r, cl[c].rhs); printf(" <= "); jint_print(&r, stdout); printf("\n");
        }
        if (check) {
            JObl *o; int32_t n;
            if (err[0] || !jgen_func(&m, f, cl, ncl, JGEN_ALL, &o, &n, err, sizeof err)) {
                fprintf(stderr, "ERROR %.*s: %s\n", (int)f->name->length, f->name->name, err); rc = 1; free(cl); continue;
            }
            free(cl);
            HK *nh = realloc(hk, sizeof *hk * (size_t)(nhk + n + 1));
            HE *ne = realloc(he, sizeof *he * (size_t)(nhe + n + 1));
            if (!nh || !ne) { fprintf(stderr, "out of memory\n"); return 1; }
            hk = nh; he = ne;
            for (int32_t i = 0; i < n; i++) {
                if (o[i].outside) continue;
                if (o[i].to) { he[nhe].from = o[i].blk; he[nhe].o = &o[i]; nhe++; }
                else { hk[nhk].at = o[i].at; hk[nhk].o = &o[i]; nhk++; }
            }
            continue;
        }
        Vra *V = vra_analyze(f);
        m = jmem_over(buf, cap);
        JObl *o; int32_t n;
        if (err[0] || !jgen_func(&m, f, cl, ncl, JGEN_ALL, &o, &n, err, sizeof err)) {
            printf("%s %.*s: %s\n", stats ? "ERROR\t" : "# generator refused", (int)f->name->length, f->name->name, err);
            rc = 1; free(cl); vra_free(V); continue;
        }
        free(cl);
        for (int32_t i = 0; i < n; i++) {
            const char *vv = o[i].outside ? "-" : vra_verdict(V, &o[i]);
            if (why) {
                // what the goal's variables are: the step that would have to state more about them
                if (o[i].outside) continue;
                printf("%s %s %s %s %s\t", o[i].vc.id.fn, o[i].vc.id.irhash, o[i].vc.id.point, o[i].vc.id.kind, o[i].vc.id.ordinal);
                for (int t = 0; t < o[i].vc.goal.nt; t++) {
                    int32_t x = o[i].vc.goal.t[t].var;
                    const char *what = "?";
                    if (o[i].vkind[x] == 2 || o[i].vkind[x] == 4) what = "len";
                    else if (o[i].vkind[x] == 1) {
                        const char *nmx = o[i].vc.vars[x];
                        what = strstr(nmx, "@head") ? "version.head" : strstr(nmx, "@join") ? "version.join"
                             : strstr(nmx, "@entry") ? "version.entry" : "version.store";
                    } else {
                        IrInstr *d = NULL;
                        for (IrBlock *bb = f->blocks; bb && !d; bb = bb->next)
                            for (IrInstr *ii = bb->instrs; ii; ii = ii->next)
                                if (ii->result && ii->result->id == o[i].vkey[x]) { d = ii; break; }
                        static const char *opn[] = { [IR_CONST]="const", [IR_CAST]="cast", [IR_ADD]="add", [IR_SUB]="sub", [IR_MUL]="mul",
                            [IR_SDIV]="div", [IR_UDIV]="div", [IR_SREM]="rem", [IR_UREM]="rem", [IR_NEG]="neg", [IR_AND]="and", [IR_OR]="or",
                            [IR_XOR]="xor", [IR_SHL]="shl", [IR_LSHR]="lshr", [IR_ASHR]="ashr", [IR_BNOT]="bnot", [IR_CTZ]="ctz", [IR_CLZ]="clz",
                            [IR_POPCOUNT]="popcount", [IR_SIZEOF]="sizeof", [IR_ALIGNOF]="alignof", [IR_ICMP]="icmp", [IR_LOAD]="load",
                            [IR_SLICE_LEN]="slice_len", [IR_CALL]="call", [IR_SUM_TAG]="sum_tag", [IR_SUM_PAYLOAD]="sum_payload",
                            [IR_VEC_MOVEMASK]="movemask", [IR_SEQ_EQ]="seq_eq" };
                        if (!d) what = "param";
                        else if ((size_t)d->op < sizeof opn / sizeof *opn && opn[d->op]) what = opn[d->op];
                        else what = "other-op";
                        if (d && d->op == IR_LOAD) {
                            IrInstr *ad = NULL; IrValue *a0 = d->operands[0];
                            for (IrBlock *bb = f->blocks; bb && !ad; bb = bb->next)
                                for (IrInstr *ii = bb->instrs; ii; ii = ii->next)
                                    if (ii->result == a0) { ad = ii; break; }
                            what = !ad ? "load.param-ptr" : ad->op == IR_ALLOCA ? "load.escaped-local" : ad->op == IR_FIELD_PTR ? "load.field"
                                 : ad->op == IR_ELEM_PTR ? "load.element" : "load.other";
                        }
                    }
                    printf("%s%s", t ? "," : "", what);
                }
                printf("\n");
                continue;
            }
            if (stats) {
                if (o[i].outside) printf("%.*s\tbb%d.%d\toutside\t-\t%s\t%s\n", (int)f->name->length, f->name->name,
                                         o[i].blk->id, o[i].idx, vra_verdict(V, &o[i]), o[i].outside);
                else printf("%s\t%s\t%s\t%s\t%s\tstated\t%d\t%d\n", o[i].vc.id.fn, o[i].vc.id.point, o[i].vc.id.kind,
                            o[i].vc.id.ordinal, vv, o[i].vc.nvars, o[i].vc.nhyps);
                continue;
            }
            if (o[i].outside) {
                printf("# %.*s bb%d.%d outside: %s\n", (int)f->name->length, f->name->name, o[i].blk->id, o[i].idx, o[i].outside);
                continue;
            }
            if (o[i].to) printf("# claim on the edge bb%d->bb%d\n", o[i].blk->id, o[i].to->id);
            else printf("# vra=%s\n", vv);
            if (smt) jvc_print_smtlib(&o[i].vc, stdout); else jvc_print(&o[i].vc, stdout);
        }
        vra_free(V);
    }
    if (check) {
        qsort(hk, (size_t)nhk, sizeof *hk, hk_cmp);
        qsort(he, (size_t)nhe, sizeof *he, he_cmp);
        ii_on_instr = hk_on_instr; ii_on_block = hk_on_block;
        int st = ir_interpret_module(irmod, path);
        fflush(stdout);
        fprintf(stderr, "SUMMARY obligations=%lld reached=%lld hyps-checked=%lld false=%lld not-evaluable=%lld premises=%lld status=%d"
                " claims=%lld claims-dropped=%lld claim-obligations=%lld edges=%lld claim-hyps-checked=%lld claim-goals-checked=%lld\n",
               (long long)nhk, reached, checked, falsec, unknown, premises, st, cl_kept, cl_dropped, (long long)nhe, edges,
               claimchk, claimgoals);
        if (falsec) rc = 3;
    } else if (claims) printf("# claims proposed %lld, kept %lld, dropped %lld\n", cl_proposed, cl_kept, cl_dropped);
    free(buf); free(hbuf); free(cl_work);
    return rc;
}
