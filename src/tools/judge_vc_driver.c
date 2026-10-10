// src/tools/judge_vc_driver.c — phase 1a of the proof engine's judge: the VCs the generator
// (src/judge/gen.h) states for every obligation of a Lain file, beside VRA's verdict on the same
// instruction. Nothing here decides anything: the judge has no kernel yet. The untrusted solver
// runs only to PROPOSE claims at forward joins, loop headers and returns (propose_module below),
// which the generator then owes as obligations of their own (`hull`, `inv.entry`, `inv.keep`,
// `post`); `--no-claims` proposes none.
//
//   gcc -std=c99 -O2 -o /tmp/jvc src/tools/judge_vc_driver.c -I src
//   /tmp/jvc FILE.ln            each obligation: a header line, then its VC in the text form; a
//                               claim's (`# claim on the edge bbP->bbH`, `# claim at the return
//                               of bbR`) after the function's own, and a last line counting the
//                               claims proposed and kept
//   --claims                    also each claim kept, as a comment line
//   --no-claims, --no-finder    propose no claim; or none from VRA's header states (PE.4) and
//                               return intervals
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
//                               join's version is named (its VCs implied, judge_gate.sh). A
//                               return claim's, and the claim, as the program returns.
//
// VRA's verdict is read from its checks on the same instruction: `proved` when VRA discharged the
// matching half, `refused` when it did not, `none` when VRA states no obligation there. A return's
// landing has no instruction: VRA files it at the returned value's definition.
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
    IrInstr *at = o->at;
    if (!at && o->blk && o->blk->term.kind == IR_TERM_RET && o->blk->term.cond) {
        IrValue *rv = o->blk->term.cond;                     // a return's landing: VRA's check sits at
        at = rv->id >= 0 && rv->id < V->nvar ? V->def[rv->id] : NULL;   // the value's definition
    }
    for (int i = 0; i < V->nchecks; i++) {
        VraCheck *c = &V->checks[i];
        if (c->at != at) continue;
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
static long long reached, checked, falsec, unknown, premises, claimchk, claimgoals, contractchk, postgoals;
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
        if (!strncmp(y->origin, "contract ", 9)) contractchk++;   // a callee's return claim
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
// A claim's obligations, by the block whose END they are owed at: each on one edge out of it, or a
// return claim's (`post`, no edge) as the block returns. A return's landing (P2.3) is owed there
// too, an obligation like any other (its `claim` is -1).
typedef struct { IrBlock *from; JObl *o; } HE;
static HE *he; static int64_t nhe; static long long edges, posts;
static int he_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const HE *)a)->from, y = (uintptr_t)((const HE *)b)->from;
    return x < y ? -1 : x > y;
}
// The edge taken is the interpreter's own: the block whose end was last reached, then the block
// entered next in the same frame (ii_on_block), with nothing run between. A record left by a
// block that returned matches no claim (such a block has no edge to owe one on).
static IrBlock *hk_last; static IVal *hk_lastv;
// A claim obligation reached where it is owed: its hypotheses, and its goal (the claim), counted
// in `goals` when it could be evaluated.
static void hk_claim(JObl *o, IVal *v, int nv, const char *where, long long *goals) {
    hk_check(o, v, nv);
    int r = hk_cons(o, &o->vc.goal, v, nv);
    if (r < 0) { unknown++; return; }
    (*goals)++;
    if (r == 1) return;
    falsec++;
    fprintf(stderr, "FALSE %s %s %s %s goal (the claim, %s):", o->vc.id.fn, o->vc.id.point, o->vc.id.kind, o->vc.id.ordinal, where);
    for (int32_t x = 0; x < o->vc.nvars; x++) {
        __int128 val;
        if (hk_var(o, x, v, nv, &val)) fprintf(stderr, " %s=%lld", o->vc.vars[x], (long long)val);
    }
    fprintf(stderr, "\n");
}
static HE *hk_from(const IrBlock *b) {
    HE key = { (IrBlock *)b, NULL };
    HE *h = nhe ? bsearch(&key, he, (size_t)nhe, sizeof *he, he_cmp) : NULL;
    while (h && h > he && h[-1].from == b) h--;
    return h;
}
static void hk_on_block(IrFunc *f, IrBlock *to, IVal *v, int nv) {
    (void)f;
    IrBlock *b = hk_last; IVal *bv = hk_lastv;
    hk_last = NULL;
    if (b && bv == v) { HkLast *e = &hk_lastpred[hk_lp_slot(v, to->id)]; e->v = v; e->blk = to->id; e->pred = b->id; }
    if (!b || bv != v) return;
    char where[64]; snprintf(where, sizeof where, "on the edge bb%d->bb%d", b->id, to->id);
    for (HE *h = hk_from(b); h && h < he + nhe && h->from == b; h++)
        if (h->o->to == to) { edges++; hk_claim(h->o, v, nv, where, &claimgoals); }
}
static void hk_on_instr(IrFunc *f, IrBlock *b, IrInstr *i, IVal *v, int nv) {
    (void)f;
    if (!i) {                                        // the block's end: its edge is known next
        hk_last = b; hk_lastv = v;
        if (b->term.kind == IR_TERM_RET)                 // owed as b returns
            for (HE *h = hk_from(b); h && h < he + nhe && h->from == b; h++) {
                if (h->o->to) continue;
                if (h->o->claim >= 0) { posts++; hk_claim(h->o, v, nv, "at the return", &postgoals); }
                else hk_check(h->o, v, nv);                  // the return's landing
            }
        return;
    }
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
// name, one of them a version the header makes. At a function's RETURNS (ret_candidates), for a
// function some function of its module calls: each side of the value returned and of its
// difference and sum with each integer parameter, bounded over the returns' paths, and VRA's
// return interval as a fixed candidate.
// Nothing here is believed: the generator owes every claim on every edge into its block, back
// edges included, and a return claim at every return, and HOUDINI drops a claim one of those VCs
// is not implied by, and solves the rest again, until none is: what survives is inductive
// together. Claims are proposed for the whole MODULE at once and Houdini runs over all of them,
// since a return claim may rest on its function's loop invariants, a loop on a callee's return
// claim, and a recursive function's return claim on itself at its own calls (induction over the
// calls). Bounds are computed in ROUNDS, each
// under the claims the previous round verified only (a candidate not yet verified may contradict
// another, and a bound computed under it would be vacuous): round 1 under none, then until a
// round verifies nothing new, three at most. A round's verified set contains the last one's.
#define CL_REGION ((size_t)1 << 26)
static char *cl_work;
static const JWork cl_lim = { 100000, 4096, 4096 };    // solve_vc's
static long long cl_proposed, cl_kept, cl_dropped;
static bool cl_finder = true;                          // VRA's facts as candidates (--no-finder)
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
// Houdini over the module's claims cl[0..nc): each function's claim VCs (its cut points' and its
// returns'), solved; a claim one of them is not implied by is dropped, and the rest solved again,
// until none is. The claims left, in order. A function the generator refuses has none of its own,
// and its final generation reports the refusal.
static int32_t houdini_module(IrFunc *mod, char *buf, size_t cap, JClaim *cl, int32_t nc) {
    char err[200];
    for (bool again = nc > 0; again; ) {
        again = false;
        uint8_t *bad = calloc((size_t)nc, 1);
        if (!bad) return -1;
        for (IrFunc *f = mod; f; f = f->next) {
            if (f->is_extern || !f->entry) continue;
            JMem m = jmem_over(buf, cap);
            JObl *o; int32_t n;
            if (!jgen_func(&m, f, cl, nc, JGEN_CLAIMS, &o, &n, err, sizeof err)) {
                for (int32_t ci = 0; ci < nc; ci++) if (cl[ci].fn == f) bad[ci] = 1;
                continue;
            }
            for (int32_t q = 0; q < n; q++)
                if (!bad[o[q].claim] && lia_solve(&o[q].vc, cl_lim, cl_work, CL_REGION).k != J_ANS_CERT) bad[o[q].claim] = 1;
        }
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
// Each candidate's bound, from the VCs o[0..n) of a claim set whose candidates start at `base`:
// the largest value its objective takes over its VCs (a fixed one keeps its own). What survives,
// unless a verified claim is as tight or this very claim was proposed already, goes into cl.
static bool bound_candidates(const JCand *cd, int32_t ncd, const JObl *o, int32_t n, int32_t base,
                             const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    for (int32_t c = 0; c < ncd; c++) {
        JClaim got = cd[c].c;
        if (!cd[c].fixed) {
            __int128 best = 0;
            bool ok = true, any = false;
            for (int32_t q = 0; q < n && ok; q++) {
                if (o[q].claim != base + c) continue;
                JVc v = o[q].vc;
                jg_int(&v.goal.rhs, cd[c].tb - 1);
                if (lia_solve(&v, cl_lim, cl_work, CL_REGION).k != J_ANS_CERT) { ok = false; break; }
                LMax x = lia_maximize(&o[q].vc, o[q].vc.goal.t, o[q].vc.goal.nt, cl_lim, cl_work, CL_REGION);
                if (x.k == LIA_MAX_EMPTY) continue;
                if (x.k != LIA_MAX_BOUND) { ok = false; break; }
                __int128 bb = hk_jint(&x.b);
                if (!any || bb > best) best = bb;
                any = true;
            }
            if (!ok || !any) continue;
            got.rhs = best;
        }
        // already verified as tight, or this very candidate already proposed (a tighter one not
        // yet verified does not make it redundant: it may fail)
        bool dup = false;
        for (int32_t v2 = 0; v2 < nv && !dup; v2++) dup = same_form(&as[v2], &got) && as[v2].fn == got.fn && as[v2].rhs <= got.rhs;
        for (int32_t v2 = nv; v2 < *nc && !dup; v2++) dup = same_form(&(*cl)[v2], &got) && (*cl)[v2].fn == got.fn && (*cl)[v2].rhs == got.rhs;
        if (dup) continue;
        if (*nc == *ccl) {
            int32_t nn = *ccl ? *ccl * 2 : 64;
            JClaim *x = realloc(*cl, sizeof *x * (size_t)nn);
            if (!x) return false;
            *cl = x; *ccl = nn;
        }
        (*cl)[(*nc)++] = got;
        cl_proposed++;
    }
    return true;
}
// Whether a function of the module that `f`'s claims reach (its own, I.181) calls f.
static bool called_here(IrFunc *mod, IrFunc *f) {
    for (IrFunc *g = mod; g; g = g->next) {
        if (g->is_extern || !jg_same_module(g, f)) continue;
        for (IrBlock *b = g->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next)
                if (i->op == IR_CALL && jg_callee(i) == f) return true;
    }
    return false;
}
// A function's return candidates: VRA's interval (its summary, after the analysis: ir.h's
// ret_range_state), each side of the returned value, and of its difference and sum with each
// integer parameter (`min(a, b) <= a`; `clamp(x, lo, hi) >= lo`). None for a function no caller
// here can use them in, or whose return may wrap, whose claims the generator would refuse.
static bool ret_candidates(IrFunc *mod, IrFunc *f, JCand **cd, int32_t *ncd, int32_t *ccd) {
    *ncd = 0;
    if (!jg_is_int(f->ret_type) || f->incomplete || !called_here(mod, f)) return true;
    for (IrBlock *b = f->blocks; b; b = b->next)
        if (b->term.kind == IR_TERM_RET && b->term.cond && !jg_lands_exact(b->term.cond->type, f->ret_type, b->term.unchecked)) return true;
    JRange tr;
    // a side tighter than the type's
    if (cl_finder && f->ret_range_state == 2 && jg_range(f->ret_type, &tr))
        for (int sd = 1; sd >= -1; sd -= 2) {
            __int128 rhs = sd > 0 ? (__int128)f->ret_range_hi : -(__int128)f->ret_range_lo;
            if (rhs >= (sd > 0 ? tr.hi : -tr.lo)) continue;
            int32_t at = *ncd;
            if (!cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, sd, -1, 0, f->ret_type, NULL)) return false;
            if (*ncd == at + 1) { (*cd)[at].fixed = true; (*cd)[at].c.rhs = rhs; }
        }
    if (!cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, 1, -1, 0, f->ret_type, NULL) ||
        !cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, -1, -1, 0, f->ret_type, NULL)) return false;
    for (IrParam *p = f->params; p; p = p->next) {
        if (!p->value || !jg_is_int(p->value->type)) continue;
        for (int sr = -1; sr <= 1; sr += 2)
            for (int sp = -1; sp <= 1; sp += 2)
                if (!cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, sr, p->value->id, sp, f->ret_type, p->value->type)) return false;
    }
    for (int32_t c = 0; c < *ncd; c++) (*cd)[c].c.fn = f;
    return true;
}
// The module's claims, in ROUNDS (each under the claims the previous one verified only): every
// function's cut-point candidates and return candidates, bounded, then Houdini over them all, since
// a return claim may need its function's loop invariants and a loop a callee's return claim.
static JClaim *propose_module(IrFunc *mod, char *buf, size_t cap, int32_t *ncl) {
    char err[200];
    JClaim *cl = NULL, *as = NULL;
    JCand *cd = NULL;
    int32_t nc = 0, ccl = 0, nv = 0, ncd = 0, ccd = 0;
    *ncl = 0;
    for (int round = 0; round < 3; round++) {
        if (nv > ccl) { JClaim *x = realloc(cl, sizeof *x * (size_t)nv); if (!x) goto oom; cl = x; ccl = nv; }
        if (nv) memcpy(cl, as, sizeof *cl * (size_t)nv);
        nc = nv;
        for (IrFunc *f = mod; f; f = f->next) {
            if (f->is_extern || !f->entry) continue;
            JMem m = jmem_over(buf, cap);
            JSlot *s0; int32_t ns = 0;
            JSlot *sl = NULL;
            if (jgen_slots(&m, f, &s0, &ns, err, sizeof err) && ns > 0) {
                if (!(sl = malloc(sizeof *sl * (size_t)ns))) goto oom;
                memcpy(sl, s0, sizeof *sl * (size_t)ns);
            }
            // the cut points, block by block, then the returns; each under the verified claims
            for (int32_t i = 0, j; i <= ns; i = j) {
                int32_t only;
                if (i < ns) {
                    for (j = i; j < ns && sl[j].blk == sl[i].blk; j++) ;
                    if (!block_candidates(f, buf, cap, sl, i, j, &cd, &ncd, &ccd, err, sizeof err)) continue;
                    for (int32_t c = 0; c < ncd; c++) cd[c].c.fn = f;
                    only = sl[i].blk;
                } else {
                    j = ns + 1;
                    if (!ret_candidates(mod, f, &cd, &ncd, &ccd)) { free(sl); goto oom; }
                    only = JGEN_POST;
                }
                if (ncd == 0) continue;
                JClaim *q2 = malloc(sizeof *q2 * (size_t)(nv + ncd + 1));
                if (!q2) { free(sl); goto oom; }
                if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
                for (int32_t c = 0; c < ncd; c++) q2[nv + c] = cd[c].c;
                m = jmem_over(buf, cap);
                JObl *o; int32_t n;
                bool gok = jgen_func(&m, f, q2, nv + ncd, only, &o, &n, err, sizeof err);
                free(q2);
                if (gok && !bound_candidates(cd, ncd, o, n, nv, as, nv, &cl, &nc, &ccl)) { free(sl); goto oom; }
                // a header's back edge from ANY previous trip: each side of each header version
                // alone, under the verified claims only (at their placeholders the header's
                // comparisons, give or take one, contradict each other, and the back edge bounds
                // nothing): `k = next(k)` with `next(k) <= 7` keeps `k <= 7` whatever k was
                if (gok && i < ns && sl[i].head) {
                    int32_t n1 = 0;
                    for (int32_t c = 0; c < ncd; c++) if (!cd[c].fixed && cd[c].c.nt == 1) cd[n1++] = cd[c];
                    if (n1 > 0) {
                        if (!(q2 = malloc(sizeof *q2 * (size_t)(nv + n1)))) { free(sl); goto oom; }
                        if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
                        for (int32_t c = 0; c < n1; c++) { q2[nv + c] = cd[c].c; q2[nv + c].rhs = cd[c].tb; }
                        m = jmem_over(buf, cap);
                        gok = jgen_func(&m, f, q2, nv + n1, only, &o, &n, err, sizeof err);
                        free(q2);
                        if (gok && !bound_candidates(cd, n1, o, n, nv, as, nv, &cl, &nc, &ccl)) { free(sl); goto oom; }
                    }
                }
            }
            free(sl);
        }
        if (nc == nv) break;                                 // nothing new to verify
        int32_t kept = houdini_module(mod, buf, cap, cl, nc);
        if (kept < 0) goto oom;
        bool grew = kept > nv;
        free(as); as = malloc(sizeof *as * (size_t)(kept ? kept : 1));
        if (!as) goto oom;
        memcpy(as, cl, sizeof *as * (size_t)kept);
        nv = kept;
        if (!grew) break;
    }
    free(cd); free(cl);
    cl_kept += nv;
    *ncl = nv;
    if (nv == 0) { free(as); return NULL; }
    return as;
oom:
    free(cd); free(cl); free(as);
    fprintf(stderr, "propose_module: out of memory\n");
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
    // the module's claims: VRA's header states and return intervals first, as candidates
    int32_t ncl = 0;
    JClaim *cl = NULL;
    cl_finder = finder;
    if (claims) {
        for (IrFunc *f = irmod; finder && f; f = f->next) {
            if (f->is_extern) continue;
            vra_header_sink = hdr_sink; vra_header_ctx = NULL;
            Vra *VH = vra_analyze(f);
            vra_header_sink = NULL;
            if (VH) vra_free(VH);
        }
        cl = propose_module(irmod, hbuf, hcap, &ncl);
    }
    for (int32_t c = 0; show && c < ncl; c++) {
        static const char *kn[] = { "%ret" };
        printf("# claim %.*s ", (int)cl[c].fn->name->length, cl[c].fn->name->name);
        if (cl[c].blk == JCLAIM_RET) printf("ret: "); else printf("bb%d: ", cl[c].blk);
        for (int k = 0; k < cl[c].nt; k++) {
            if (k) printf(" + ");
            if (cl[c].key[k] == JKEY_RET) printf("%lld*%s", (long long)cl[c].coef[k], kn[0]);
            else printf("%lld*%%%d", (long long)cl[c].coef[k], cl[c].key[k]);
        }
        JInt r; jg_int(&r, cl[c].rhs); printf(" <= "); jint_print(&r, stdout); printf("\n");
    }
    for (IrFunc *f = irmod; f; f = f->next) {
        if (f->is_extern) continue;
        char err[200]; err[0] = 0;
        if (check) {
            JObl *o; int32_t n;
            if (!jgen_func(&m, f, cl, ncl, JGEN_ALL, &o, &n, err, sizeof err)) {
                fprintf(stderr, "ERROR %.*s: %s\n", (int)f->name->length, f->name->name, err); rc = 1; continue;
            }
            HK *nh = realloc(hk, sizeof *hk * (size_t)(nhk + n + 1));
            HE *ne = realloc(he, sizeof *he * (size_t)(nhe + n + 1));
            if (!nh || !ne) { fprintf(stderr, "out of memory\n"); return 1; }
            hk = nh; he = ne;
            for (int32_t i = 0; i < n; i++) {
                if (o[i].outside) continue;
                if (o[i].to || !o[i].at) { he[nhe].from = o[i].blk; he[nhe].o = &o[i]; nhe++; }
                else { hk[nhk].at = o[i].at; hk[nhk].o = &o[i]; nhk++; }
            }
            continue;
        }
        Vra *V = vra_analyze(f);
        m = jmem_over(buf, cap);
        JObl *o; int32_t n;
        if (!jgen_func(&m, f, cl, ncl, JGEN_ALL, &o, &n, err, sizeof err)) {
            printf("%s %.*s: %s\n", stats ? "ERROR\t" : "# generator refused", (int)f->name->length, f->name->name, err);
            rc = 1; vra_free(V); continue;
        }
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
            else if (!o[i].at && o[i].claim >= 0) printf("# claim at the return of bb%d\n", o[i].blk->id);
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
                " claims=%lld claims-dropped=%lld claim-obligations=%lld edges=%lld claim-hyps-checked=%lld claim-goals-checked=%lld"
               " returns=%lld return-goals-checked=%lld contract-hyps-checked=%lld\n",
               (long long)nhk, reached, checked, falsec, unknown, premises, st, cl_kept, cl_dropped, (long long)nhe, edges,
               claimchk, claimgoals, posts, postgoals, contractchk);
        if (falsec) rc = 3;
    } else if (claims) printf("# claims proposed %lld, kept %lld, dropped %lld\n", cl_proposed, cl_kept, cl_dropped);
    free(cl); free(buf); free(hbuf); free(cl_work);
    return rc;
}
