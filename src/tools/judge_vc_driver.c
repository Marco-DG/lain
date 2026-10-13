// src/tools/judge_vc_driver.c — phase 1a of the proof engine's judge: the VCs the generator
// (src/judge/gen.h) states for every obligation of a Lain file, beside VRA's verdict on the same
// instruction. Nothing here decides anything: the judge has no kernel yet. The untrusted solver
// runs only to PROPOSE claims at forward joins, loop headers and returns (src/solver/finder.h),
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
//   --judge                     the entry point the compiler calls (src/judge/judge.h): one §3
//                               record a line (`v1 <id> proved cert=<n>`, `not-implied model {..}`,
//                               `outside`, `budget`, `solver-bug`, `uncertified`; `refused <fn>`,
//                               `no-claims`), the claims' VCs first, then a `# judge` count line;
//                               exit 1 on a solver bug, unstable claims or a refusal
//   --judge-corrupt             --judge with every certificate damaged (a sign byte of 2) before
//                               the judge reads it: nothing may be proved (judge_gate's canary)
//   --cover                     each of VRA's checks (termination aside) beside an obligation of
//                               its class at its instruction (Main Compiler Work's census): one
//                               `UNCOVERED` line for a check the judge owes nothing for, then a
//                               `COVER` count line (judge_gate's coverage leg)
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
#include "analysis/linearity.h"            // what static evaluation proves before it runs:
#include "analysis/borrow.h"               // the analyses main.c runs, in its order
#include "analysis/definite_init.h"
#include "analysis/report.h"
#include "frontends/lain/static_eval.h"   // DECIDE-W step 2, as main.c runs it
#include "judge/gen.h"
#include "judge/judge.h"     // --judge: the entry point the compiler calls
#include "judge/text.h"
#include "solver/finder.h" // the claim proposer (untrusted): src/solver/finder.h

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
        if (!strcmp(k, "divzero") && c->kind == VRA_DIVZERO) return c->ok ? "proved" : "refused";
        // VRA's TYPE_MIN / -1 is an overflow check at the division itself (vra_check_div, shift 3)
        if (!strcmp(k, "divovf") && c->kind == VRA_OVERFLOW && c->shift == 3) return c->ok ? "proved" : "refused";
        if ((!strncmp(k, "narrow", 6) || !strcmp(k, "shift")) && c->kind == VRA_OVERFLOW && c->shift != 3) return c->ok ? "proved" : "refused";
        if (!strncmp(k, "assert.", 7) && c->kind == VRA_PRECOND) return c->ok ? "proved" : "refused";
    }
    return "none";
}

// ── --cover: every VRA check (termination aside), and an obligation of its class at its place ──
// A class pairs what VRA and the judge each call the same rule. VRA files TYPE_MIN / -1 as an
// overflow at the division (shift 3, vra_check_div), and a quotient's NARROWING (shift 0 or 5) at
// the division too when it is returned: the judge owes that as the return's `narrow`, so only
// shift 3 is the division's own class.
static const char *cls_of_kind(const char *k) {
    if (!strncmp(k, "bounds", 6)) return "B";
    if (!strcmp(k, "divovf")) return "V";
    if (!strcmp(k, "divzero") || !strcmp(k, "nonzero")) return "D";
    if (!strncmp(k, "narrow", 6) || !strcmp(k, "shift")) return "O";
    if (!strncmp(k, "assert", 6)) return "P";
    return "?";
}
static const char *cls_of_check(const VraCheck *c) {
    bool dv = c->at && (c->at->op == IR_SDIV || c->at->op == IR_SREM || c->at->op == IR_UDIV || c->at->op == IR_UREM);
    switch (c->kind) {
        case VRA_BOUNDS: return "B";
        case VRA_DIVZERO: return "D";
        case VRA_OVERFLOW: return dv && c->shift == 3 ? "V" : "O";
        case VRA_PRECOND: return "P";
        default: return NULL;
    }
}
static long long cv_checks, cv_cov, cv_cov_out, cv_unc;
static void cover_func(IrFunc *f, Vra *V, JObl *o, int32_t n) {
    for (int i = 0; i < V->nchecks; i++) {
        VraCheck *c = &V->checks[i];
        const char *cc = cls_of_check(c);
        if (!cc) continue;
        cv_checks++;
        int found = 0;                                      // 1 stated, 2 outside
        for (int32_t j = 0; j < n && found != 1; j++) {
            IrInstr *at = o[j].at;
            if (!at && o[j].blk && o[j].blk->term.kind == IR_TERM_RET && o[j].blk->term.cond && !o[j].to) {
                IrValue *rv = o[j].blk->term.cond;          // a return's landing: VRA files it at the value's definition
                at = rv->id >= 0 && rv->id < V->nvar ? V->def[rv->id] : NULL;
            }
            if (at != c->at) continue;
            if (o[j].outside) { if (!found) found = 2; continue; }   // an outside record has no kind of its own
            if (!strcmp(cls_of_kind(o[j].vc.id.kind ? o[j].vc.id.kind : ""), cc)) found = 1;
        }
        if (found == 1) cv_cov++;
        else if (found == 2) cv_cov_out++;
        else {
            cv_unc++;
            printf("UNCOVERED\t%.*s\t%s\t%s\t%lld:%lld\top=%d\tshift=%d\n", (int)f->name->length, f->name->name, cc,
                   c->ok ? "vra-ok" : "vra-refused", (long long)c->line, (long long)c->col, c->at ? (int)c->at->op : -1, c->shift);
        }
    }
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
    // A CLAIM's goal is a fact other VCs assume, so it must hold where its edge is taken. An
    // obligation owed on an edge (a loop's `rank.*`, a written measure's `measure.*`; its `claim`
    // is -1) is owed, not assumed, and fails in a program the prover refuses (`decreasing 7`): its
    // hypotheses only, as at an instruction and at a return's landing.
    for (HE *h = hk_from(b); h && h < he + nhe && h->from == b; h++)
        if (h->o->to == to) {
            edges++;
            if (h->o->claim >= 0) hk_claim(h->o, v, nv, where, &claimgoals); else hk_check(h->o, v, nv);
        }
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
static FdHdr *hdr; static int nhdr, chdr;              // the finder's input (finder.h)
static void hdr_sink(void *ctx, IrFunc *f, int bid, const VraRel *r, int n) {
    (void)ctx;
    for (int i = 0; i < nhdr; i++) if (hdr[i].f == f && hdr[i].bid == bid) return;
    if (nhdr == chdr) {
        int nc = chdr ? 2 * chdr : 16;
        FdHdr *x = realloc(hdr, sizeof *x * (size_t)nc);
        if (!x) return;                                      // a candidate lost: precision only
        hdr = x; chdr = nc;
    }
    FdRel *c = malloc(sizeof *c * (size_t)(n ? n : 1));
    if (!c) return;
    for (int k = 0; k < n; k++) { c[k].a = r[k].a; c[k].sa = r[k].sa; c[k].b = r[k].b; c[k].sb = r[k].sb; c[k].c = r[k].c; }
    hdr[nhdr].f = f; hdr[nhdr].bid = bid; hdr[nhdr].r = c; hdr[nhdr].n = n; nhdr++;
}

// ── --judge: judge_module with the untrusted solver and finder as its callbacks, one §3 line a record
typedef struct { char *work; size_t wcap; bool corrupt; JFinder *F; } JjCtx;
static void jj_corrupt(JSteps *b, bool *done) {       // a multiplier's sign byte 2 (I.197): the kernel must refuse it
    for (int32_t i = 0; b && i < b->n && !*done; i++) {
        if (b->s[i].k == J_LIN && b->s[i].nmul > 0) { b->s[i].mul[0].m.neg = 2; *done = true; }
        for (int32_t k = 0; k < b->s[i].nbr && !*done; k++) jj_corrupt(&b->s[i].br[k], done);
    }
}
static bool jj_solve(void *ctx, const JVc *vc, JWork lim, JAnswer *out) {
    JjCtx *c = ctx;
    *out = lia_solve(vc, lim, c->work, c->wcap);
    if (c->corrupt && out->k == J_ANS_CERT) { bool done = false; jj_corrupt(&out->cert.body, &done); }
    return true;
}
static bool jj_propose(void *ctx, IrFunc *mod, const JudgeGen *gen, JClaim **out, int32_t *nout, JRank **rk, int32_t *nrk) {
    JjCtx *c = ctx;
    c->F->gen = gen; c->F->mod = mod;
    *out = finder_propose(c->F, nout);
    *rk = c->F->rk; *nrk = c->F->nrk;    // ranking claims (PE.5): owed, never facts
    return !c->F->oom;                   // a NULL for want of memory is not "no claims"
}
static long long jrec_n[9];
static void jrec_print(void *ctx, const JRecord *r) {
    (void)ctx;
    static const char *kn[] = { "proved", "not-implied", "outside", "budget", "capacity", "solver-bug", "uncertified", "refused", "no-claims" };
    jrec_n[r->k]++;
    if (r->k == JREC_NO_CLAIMS) { printf("no-claims: %s\n", r->reason); return; }
    if (r->k == JREC_REFUSED) { printf("refused %.*s: %s\n", (int)r->fn->name->length, r->fn->name->name, r->reason); return; }
    if (r->id && r->id->fn) printf("v1 %s %s %s %s %s %s", r->id->fn, r->id->irhash, r->id->point, r->id->kind, r->id->ordinal, kn[r->k]);
    else printf("v1 %.*s - bb%d.%d - - %s", (int)r->fn->name->length, r->fn->name->name, r->blk ? r->blk->id : -1, r->idx, kn[r->k]);
    switch (r->k) {
        case JREC_PROVED: printf(" cert=%d", r->steps); break;
        case JREC_NOT_IMPLIED:
            printf(" model {");
            for (int32_t i = 0; r->model && i < r->model->n; i++) {
                printf("%s %s=", i ? " ;" : "", r->vc->vars[i]); jint_print(&r->model->val[i], stdout);
            }
            printf(" }");
            break;
        case JREC_BUDGET: case JREC_CAPACITY:
            printf(" used=pivot:%llu,node:%llu,leaf:%llu", (unsigned long long)r->used.pivot, (unsigned long long)r->used.node, (unsigned long long)r->used.leaf);
            break;
        default: if (r->reason) printf(" reason=%s", r->reason); break;
    }
    printf("\n");
}

// --why: what a goal's value is made of, through the arithmetic the generator states, down to what
// nothing states there (a load, a call, a parameter): `sub(load.element.local,const)`. A load is
// named by its address: a local's slot, a field, an element of a local array or of another, a
// pointer parameter's target.
static IrInstr *why_def(IrFunc *f, int32_t id) {
    for (IrBlock *bb = f->blocks; bb; bb = bb->next)
        for (IrInstr *ii = bb->instrs; ii; ii = ii->next) if (ii->result && ii->result->id == id) return ii;
    return NULL;
}
static void why_val(IrFunc *f, IrValue *v, int depth, char *b, size_t cap) {
    if (strlen(b) + 64 >= cap) return;
    IrInstr *d = v ? why_def(f, v->id) : NULL;
    if (!d) { strcat(b, "param"); return; }
    if (d->op == IR_LOAD) {
        IrInstr *ad = d->n_operands ? why_def(f, d->operands[0]->id) : NULL;
        const char *w = !ad ? "load.param-ptr" : ad->op == IR_ALLOCA ? "load.local" : ad->op == IR_FIELD_PTR ? "load.field" : "load.other";
        if (ad && ad->op == IR_ELEM_PTR) {
            IrInstr *bd = ad->n_operands ? why_def(f, ad->operands[0]->id) : NULL;
            IrInstr *xd = ad->n_operands > 1 ? why_def(f, ad->operands[1]->id) : NULL;
            bool k = xd && xd->op == IR_CONST;                 // a constant index, or a variable one
            w = !bd ? (k ? "load.element.param.k" : "load.element.param.i") : bd->op == IR_ALLOCA ? (k ? "load.element.local.k" : "load.element.local.i")
              : (k ? "load.element.k" : "load.element.i");
            // a variable index into an array past the judge's cap (JELEM_MAXN) is stated nothing
            const IrType *at = bd && bd->op == IR_ALLOCA ? bd->aux.alloca_ty : NULL;
            if (!k && at && at->kind == IRT_ARRAY && at->array_len > JELEM_MAXN) w = "load.element.local.i.past-cap";
        }
        strcat(b, w);
        return;
    }
    static const char *opn[] = { [IR_CONST]="const", [IR_CAST]="cast", [IR_ADD]="add", [IR_SUB]="sub", [IR_MUL]="mul",
        [IR_SDIV]="div", [IR_UDIV]="div", [IR_SREM]="rem", [IR_UREM]="rem", [IR_NEG]="neg", [IR_AND]="and", [IR_OR]="or",
        [IR_XOR]="xor", [IR_SHL]="shl", [IR_LSHR]="lshr", [IR_ASHR]="ashr", [IR_BNOT]="bnot", [IR_CTZ]="ctz", [IR_CLZ]="clz",
        [IR_POPCOUNT]="popcount", [IR_SIZEOF]="sizeof", [IR_ALIGNOF]="alignof", [IR_ICMP]="icmp",
        [IR_SLICE_LEN]="slice_len", [IR_CALL]="call", [IR_SUM_TAG]="sum_tag", [IR_SUM_PAYLOAD]="sum_payload",
        [IR_VEC_MOVEMASK]="movemask", [IR_SEQ_EQ]="seq_eq" };
    strcat(b, (size_t)d->op < sizeof opn / sizeof *opn && opn[d->op] ? opn[d->op] : "other-op");
    bool arith = d->op == IR_ADD || d->op == IR_SUB || d->op == IR_MUL || d->op == IR_NEG || d->op == IR_CAST ||
                 d->op == IR_SDIV || d->op == IR_UDIV || d->op == IR_SREM || d->op == IR_UREM || d->op == IR_AND ||
                 d->op == IR_OR || d->op == IR_XOR || d->op == IR_SHL || d->op == IR_LSHR || d->op == IR_ASHR || d->op == IR_BNOT;
    if (!arith || depth >= 4 || d->n_operands < 1) return;
    strcat(b, "(");
    for (int k = 0; k < d->n_operands; k++) { if (k) strcat(b, ","); why_val(f, d->operands[k], depth + 1, b, cap); }
    strcat(b, ")");
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.ln> [--smt | --stats | --why | --check-hyps] [--no-claims]\n", argv[0]); return 2; }
    bool smt = false, stats = false, check = false, why = false, claims = true, show = false, finder = true, judge = false, corrupt = false, cover = false;
    for (int k = 2; k < argc; k++) {
        if (!strcmp(argv[k], "--no-claims")) claims = false;
        if (!strcmp(argv[k], "--claims")) show = true;
        if (!strcmp(argv[k], "--no-finder")) finder = false;
        if (!strcmp(argv[k], "--smt")) smt = true;
        if (!strcmp(argv[k], "--stats")) stats = true;
        if (!strcmp(argv[k], "--check-hyps")) check = true;
        if (!strcmp(argv[k], "--why")) why = true;
        if (!strcmp(argv[k], "--judge")) judge = true;
        if (!strcmp(argv[k], "--judge-corrupt")) judge = corrupt = true;
        if (!strcmp(argv[k], "--cover")) cover = true;
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
    // ...and stop where the compiler stops (main.c): lowering a program the front end refused is
    // not defined, and a `break` out of a deferred statement (E138) recursed until the stack ran out
    if (sema_check_undeclared(prog, path)) { fprintf(stderr, "refused by the front end\n"); return 1; }
    // A module constant whose initialiser calls a function is computed now and lowered as the
    // literal it is, as the compiler does it (main.c, DECIDE-W step 2): the IR judged is the IR
    // compiled. Without it the initialiser ran in `main`, with obligations the program never has.
    Arena ea = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE*4096);
    if (ir_static_eval_module(prog, path, &ea)) { fprintf(stderr, "static evaluation failed\n"); return 1; }
    IrFunc *irmod = ir_lower_module(prog, &ia);
    static IrCallGraph drv_cg;                                 // the module's call graph, built once (recursion)
    if (!ir_cg_build(&drv_cg, irmod)) { fprintf(stderr, "out of memory building the call graph\n"); return 1; }
    vra_mod = irmod;
    size_t cap = (size_t)1 << 28, hcap = (size_t)1 << 26;
    char *buf = malloc(cap), *hbuf = malloc(hcap);
    size_t wcap = (size_t)1 << 26;
    char *work = malloc(wcap);
    if (!buf || !hbuf || !work) { fprintf(stderr, "out of memory\n"); return 1; }
    // the finder (untrusted, src/solver/finder.h) proposes; the generator is offered to it as calls
    JGenOffer offer = { hbuf, hcap, irmod };
    JudgeGen gen = jgen_offer(&offer);
    JFinder F; memset(&F, 0, sizeof F);
    F.gen = &gen; F.mod = irmod; F.work = work; F.worksz = wcap;
    F.lim.pivot = 100000; F.lim.node = 4096; F.lim.leaf = 4096;   // solve_vc's
    int rc = 0;
    JMem m = jmem_over(buf, cap);                      // one region: --check-hyps keeps every VC
    // the module's claims: VRA's header states and return intervals first, as candidates
    int32_t ncl = 0;
    JClaim *cl = NULL;
    F.use_vra = finder;
    if (claims) {
        for (IrFunc *f = irmod; finder && f; f = f->next) {
            if (f->is_extern) continue;
            vra_header_sink = hdr_sink; vra_header_ctx = NULL;
            Vra *VH = vra_analyze(f);
            vra_header_sink = NULL;
            if (VH) vra_free(VH);
        }
        F.hdr = hdr; F.nhdr = nhdr;
        if (!judge) {
            cl = finder_propose(&F, &ncl);
            if (F.oom) fprintf(stderr, "finder_propose: out of memory\n");
        }
    }
    if (judge) {
        // the entry point the compiler calls, its callbacks the untrusted solver and finder
        JjCtx jc = { work, wcap, corrupt, &F };
        JudgeCalls calls = { &jc, jj_solve, claims ? jj_propose : NULL };
        JudgeLimits lim; lim.lim = F.lim;
        JMem jm = jmem_over(buf, cap);
        char err[200];
        if (!judge_module(irmod, &lim, &calls, jrec_print, NULL, &jm, err, sizeof err)) { fprintf(stderr, "judge_module: %s\n", err); return 1; }
        printf("# judge proved=%lld not-implied=%lld outside=%lld budget=%lld capacity=%lld solver-bug=%lld uncertified=%lld refused=%lld no-claims=%lld\n",
               jrec_n[0], jrec_n[1], jrec_n[2], jrec_n[3], jrec_n[4], jrec_n[5], jrec_n[6], jrec_n[7], jrec_n[8]);
        free(buf); free(hbuf); free(work);
        return jrec_n[JREC_SOLVER_BUG] || jrec_n[JREC_UNCERTIFIED] || jrec_n[JREC_REFUSED] || jrec_n[JREC_NO_CLAIMS] ? 1 : 0;
    }
    for (int32_t c = 0; show && c < ncl; c++) {
        static const char *kn[] = { "%ret" };
        printf("# claim %.*s ", (int)cl[c].fn->name->length, cl[c].fn->name->name);
        if (cl[c].blk == JCLAIM_RET) printf("ret: "); else printf("bb%d: ", cl[c].blk);
        for (int k = 0; k < cl[c].nt; k++) {
            if (k) printf(" + ");
            if (cl[c].key[k] == JKEY_RET) printf("%lld*%s", (long long)cl[c].coef[k], kn[0]);
            else if (cl[c].tk[k] == JTERM_LEN) printf("%lld*len(%%%d)", (long long)cl[c].coef[k], cl[c].key[k]);
            else printf("%lld*%%%d", (long long)cl[c].coef[k], cl[c].key[k]);
        }
        JInt r; jg_int(&r, cl[c].rhs); printf(" <= "); jint_print(&r, stdout); printf("\n");
    }
    for (IrFunc *f = irmod; f; f = f->next) {
        if (f->is_extern) continue;
        char err[200]; err[0] = 0;
        if (check) {
            JObl *o; int32_t n;
            if (!jgen_func(&m, f, irmod, cl, ncl, NULL, 0, &drv_cg, JGEN_ALL, &o, &n, err, sizeof err)) {
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
        if (!jgen_func(&m, f, irmod, cl, ncl, NULL, 0, &drv_cg, JGEN_ALL, &o, &n, err, sizeof err)) {
            printf("%s %.*s: %s\n", stats ? "ERROR\t" : "# generator refused", (int)f->name->length, f->name->name, err);
            rc = 1; vra_free(V); continue;
        }
        if (cover) { cover_func(f, V, o, n); vra_free(V); continue; }
        for (int32_t i = 0; i < n; i++) {
            const char *vv = o[i].outside ? "-" : vra_verdict(V, &o[i]);
            if (why) {
                // what the goal's variables are: the step that would have to state more about them
                if (o[i].outside) continue;
                printf("%s %s %s %s %s\t", o[i].vc.id.fn, o[i].vc.id.irhash, o[i].vc.id.point, o[i].vc.id.kind, o[i].vc.id.ordinal);
                for (int t = 0; t < o[i].vc.goal.nt; t++) {
                    int32_t x = o[i].vc.goal.t[t].var;
                    char what[512] = "";
                    if (o[i].vkind[x] == 2 || o[i].vkind[x] == 4) strcpy(what, "len");
                    else if (o[i].vkind[x] == 1) {
                        const char *nmx = o[i].vc.vars[x];
                        strcpy(what, strstr(nmx, "@head") ? "version.head" : strstr(nmx, "@join") ? "version.join"
                                   : strstr(nmx, "@entry") ? "version.entry" : "version.store");
                        IrInstr *sd = o[i].vaux && o[i].vaux[x] >= 0 ? why_def(f, o[i].vaux[x]) : NULL;
                        if (sd && sd->result) { strcat(what, ":"); why_val(f, sd->result, 1, what, sizeof what); }
                    } else {
                        IrInstr *d = why_def(f, o[i].vkey[x]);
                        if (d && d->result) why_val(f, d->result, 0, what, sizeof what); else strcpy(what, "param");
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
               (long long)nhk, reached, checked, falsec, unknown, premises, st, F.kept, F.dropped, (long long)nhe, edges,
               claimchk, claimgoals, posts, postgoals, contractchk);
        if (falsec) rc = 3;
    } else if (claims) printf("# claims proposed %lld, kept %lld, dropped %lld\n", F.proposed, F.kept, F.dropped);
    if (cover) printf("COVER\tchecks %lld\tstated %lld\toutside %lld\tuncovered %lld\n", cv_checks, cv_cov, cv_cov_out, cv_unc);
    free(cl); free(buf); free(hbuf); free(work);
    return rc;
}
