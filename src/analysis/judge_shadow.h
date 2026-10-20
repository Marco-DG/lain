#ifndef LAIN_ANALYSIS_JUDGE_SHADOW_H
#define LAIN_ANALYSIS_JUDGE_SHADOW_H
// judge_shadow.h — `--judge=shadow` (PE.6, phase 3 of the proof-engine plan). The judge
// (src/judge/judge.h: the generator, the kernel, and the module's records) runs beside VRA on the
// module the compiler lowered, with the untrusted solver (src/solver/lia.h) and finder
// (src/solver/finder.h) as its callbacks, and every place the two engines disagree is reported:
//
//   vra-only     VRA discharges a check the judge does not prove. The judge cannot take authority
//                while one is unexplained: the program would be refused where it compiles today.
//   judge-only   the judge proves what VRA refuses: what authority gains.
//   unowed       VRA checks something the judge states no obligation for. Under authority that
//                check would be made by nobody, so each one is a hole to close before the switch.
//   judge-extra  the judge owes an obligation where VRA checks nothing of that class.
//
// The two are compared check by check: a VRA check and the judge's records at the same place (the
// instruction, or for a return's landing the returned value's definition, where VRA files it) and
// of the same class: bounds, division by zero, overflow and narrowing, preconditions. A judge
// record of the class counts as proved only when every one at that place is (a bounds check is two
// obligations, lo and hi). Termination is compared per loop (rank.* or, for a written measure,
// measure.* on the back edges into its header) and per function for a recursion (every rec.* in
// it), and counted apart in the summary.
//
// Shadow changes nothing the compiler decides: no verdict, no exit status, no emitted C. Its lines
// go to stderr after the diagnostics, each starting `judge-shadow:`, and end with one summary.
#include "judge/judge.h"
#include "solver/lia.h"
#include "solver/finder.h"
#include "analysis/vra.h"

// ── the judge's records, kept past the sink ──────────────────────────────────────────────────
typedef struct {
    IrFunc *fn; IrInstr *at; IrBlock *blk; int32_t idx;
    JRecordKind k;
    char cls;                     // 'B' bounds, 'D' division by zero, 'O' overflow/narrowing, 'V' TYPE_MIN / -1,
                                  // 'P' precondition,
                                  // 'S' a slice's length bound (objsize), 'T' a loop's termination,
                                  // 'R' a recursion's, '?' a claim's VC or other
    bool used;                    // matched to a VRA check
    char id[200];                 // "fn irhash point kind ordinal", as §3 prints it
} JshRec;
typedef struct { JshRec *r; int64_t n, cap; bool oom; long long kinds[9]; } JshRecs;

static char jsh_class(const char *kind) {
    if (!kind) return '?';
    if (!strncmp(kind, "bounds", 6)) return 'B';
    if (!strcmp(kind, "divzero") || !strcmp(kind, "nonzero")) return 'D';
    if (!strcmp(kind, "divovf")) return 'V';
    if (!strncmp(kind, "narrow", 6) || !strcmp(kind, "shift")) return 'O';
    if (!strncmp(kind, "assert", 6)) return 'P';
    if (!strcmp(kind, "objsize")) return 'S';      // VRA has no such check: always judge-extra, shown when unproved
    if (!strncmp(kind, "rank.", 5)) return 'T';     // a loop's termination (PE.5): keyed to VRA's by its header
    if (!strncmp(kind, "measure.", 8)) return 'T';  // ...or its WRITTEN measure's (D-44), owed instead of a proposal's
    if (!strncmp(kind, "rec.", 4)) return 'R';      // a recursion's: keyed to VRA's one check per function
    return '?';
}
static void jsh_sink(void *ctx, const JRecord *r) {
    JshRecs *R = ctx;
    if ((unsigned)r->k < 9) R->kinds[r->k]++;
    if (r->k == JREC_NO_CLAIMS || r->k == JREC_REFUSED || !r->id) {
        // a module's or a function's record: no obligation to compare, only to report
        if (r->k == JREC_NO_CLAIMS) fprintf(stderr, "judge-shadow: no-claims: %s\n", r->reason ? r->reason : "");
        else if (r->fn) fprintf(stderr, "judge-shadow: refused %.*s: %s\n", (int)r->fn->name->length, r->fn->name->name,
                                r->reason ? r->reason : "");
        return;
    }
    if (R->n == R->cap) {
        int64_t nc = R->cap ? 2 * R->cap : 256;
        JshRec *x = realloc(R->r, sizeof *x * (size_t)nc);
        if (!x) { R->oom = true; return; }
        R->r = x; R->cap = nc;
    }
    JshRec *q = &R->r[R->n++];
    memset(q, 0, sizeof *q);
    q->fn = r->fn; q->at = r->at; q->blk = r->blk; q->idx = r->idx; q->k = r->k;
    q->cls = jsh_class(r->id->kind);
    snprintf(q->id, sizeof q->id, "%s %s %s %s %s", r->id->fn ? r->id->fn : "?", r->id->irhash ? r->id->irhash : "?",
             r->id->point ? r->id->point : "?", r->id->kind ? r->id->kind : "?", r->id->ordinal ? r->id->ordinal : "?");
}

// ── the untrusted callbacks ──────────────────────────────────────────────────────────────────
typedef struct { char *work; size_t wcap; JFinder *F; JClaim *last; JRank *last_rk; JGuardClaim *last_gc; } JshCtx;
static bool jsh_solve(void *ctx, const JVc *vc, JWork lim, JAnswer *out) {
    JshCtx *c = ctx;
    *out = lia_solve(vc, lim, c->work, c->wcap);
    return true;
}
static bool jsh_propose(void *ctx, IrFunc *mod, const JudgeGen *gen, JClaim **out, int32_t *nout, JGuardClaim **gc, int32_t *ngc,
                        JRank **rk, int32_t *nrk) {
    JshCtx *c = ctx;
    c->F->gen = gen; c->F->mod = mod;
    free(c->last); free(c->last_rk);
    c->last = *out = finder_propose(c->F, nout);
    free(c->last_gc); c->last_gc = *gc = c->F->gc; *ngc = c->F->ngc;   // guarded return claims (s7)
    c->last_rk = *rk = c->F->rk; *nrk = c->F->nrk;      // ranking claims (PE.5): owed, never facts
    return !c->F->oom;                              // the judge reports a failed proposer
}
// VRA's converged loop-header states, the finder's candidates (PE.4's sink), first report only
static FdHdr *jsh_hdr; static int jsh_nhdr, jsh_chdr;
static void jsh_hdr_sink(void *ctx, IrFunc *f, int bid, const VraRel *r, int n) {
    (void)ctx;
    for (int i = 0; i < jsh_nhdr; i++) if (jsh_hdr[i].f == f && jsh_hdr[i].bid == bid) return;
    if (jsh_nhdr == jsh_chdr) {
        int nc = jsh_chdr ? 2 * jsh_chdr : 16;
        FdHdr *x = realloc(jsh_hdr, sizeof *x * (size_t)nc);
        if (!x) return;                             // a candidate lost: precision only
        jsh_hdr = x; jsh_chdr = nc;
    }
    FdRel *c = malloc(sizeof *c * (size_t)(n ? n : 1));
    if (!c) return;
    for (int k = 0; k < n; k++) { c[k].a = r[k].a; c[k].sa = r[k].sa; c[k].b = r[k].b; c[k].sb = r[k].sb; c[k].c = r[k].c; }
    jsh_hdr[jsh_nhdr].f = f; jsh_hdr[jsh_nhdr].bid = bid; jsh_hdr[jsh_nhdr].r = c; jsh_hdr[jsh_nhdr].n = n; jsh_nhdr++;
}

// ── the comparison ───────────────────────────────────────────────────────────────────────────
// The class of a VRA check. An overflow check at a division is one of two: TYPE_MIN / -1 itself
// (shift 3, the judge's divovf), or the landing of the quotient (VRA files a RETURNED quotient's
// narrowing at the division, the judge at the return: jsh_vra_at maps it back). Pairing both with
// both made one unproved record count against the other check.
static char jsh_vra_class(const VraCheck *c) {
    switch (c->kind) {
        case VRA_BOUNDS: return 'B';
        case VRA_DIVZERO: return 'D';
        case VRA_OVERFLOW: return c->shift == 3 ? 'V' : 'O';
        case VRA_PRECOND: return 'P';
        default: return 0;
    }
}
// Where VRA files the obligation a record is about: its instruction, or for a return's landing
// (no instruction) the returned value's definition.
static IrInstr *jsh_vra_at(Vra *V, const JshRec *q) {
    if (q->at) return q->at;
    if (q->blk && q->blk->term.kind == IR_TERM_RET && q->blk->term.cond) {
        IrValue *rv = q->blk->term.cond;
        return rv->id >= 0 && rv->id < V->nvar ? V->def[rv->id] : NULL;
    }
    return NULL;
}
static const char *jsh_kind_name(JRecordKind k) {
    static const char *kn[] = { "proved", "not-implied", "outside", "budget", "capacity", "solver-bug",
                                "uncertified", "refused", "no-claims" };
    return (unsigned)k < 9 ? kn[k] : "?";
}

// Run the judge on `mod` and report against VRA, on stderr.
static void judge_shadow_module(IrFunc *mod) {
    long long n_chk = 0, agree_p = 0, agree_r = 0, vra_only = 0, judge_only = 0, judge_only_c = 0, unowed = 0, outside = 0, term = 0;
    long long nterm = 0, term_p = 0, term_j = 0;      // loops' termination: compared, agreed proved, judge only
    long long nrec = 0, rec_p = 0, rec_j = 0;         // recursions' termination, the same three
    long long extra = 0, extra_p = 0;
    // 1. VRA's header states for the finder, from a VRA pass of each function
    jsh_nhdr = 0;
    for (IrFunc *f = mod; f; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        vra_header_sink = jsh_hdr_sink; vra_header_ctx = NULL;
        Vra *VH = vra_analyze(f);
        vra_header_sink = NULL;
        if (VH) vra_free(VH);
    }
    // 2. the judge
    size_t cap = (size_t)1 << 28, wcap = (size_t)1 << 26;
    char *buf = malloc(cap), *work = malloc(wcap);
    JshRecs R; memset(&R, 0, sizeof R);
    JFinder F; memset(&F, 0, sizeof F);
    F.work = work; F.worksz = wcap; F.use_vra = true; F.hdr = jsh_hdr; F.nhdr = jsh_nhdr;
    F.lim.pivot = 100000; F.lim.node = 4096; F.lim.leaf = 4096;
    JshCtx jc = { work, wcap, &F, NULL, NULL, NULL };
    JudgeCalls calls = { &jc, jsh_solve, jsh_propose };
    JudgeLimits lim; lim.lim = F.lim;
    char err[200] = "";
    bool ran = buf && work;
    if (ran) {
        JMem m = jmem_over(buf, cap);
        ran = judge_module(mod, &lim, &calls, jsh_sink, &R, &m, err, sizeof err);
    } else snprintf(err, sizeof err, "no memory for the judge's region");
    if (!ran) fprintf(stderr, "judge-shadow: the judge could not run: %s\n", err);
    if (R.oom) fprintf(stderr, "judge-shadow: out of memory keeping the records: the comparison is incomplete\n");
    // 3. VRA's checks against the records, function by function
    for (IrFunc *f = mod; ran && f; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        Vra *V = vra_analyze(f);
        if (!V) continue;
        // a judge proof in a function with an unproved obligation may rest on it: the generator
        // states each obligation as a fact past its point, so a later proof is conditional on it
        // (sound for the program, which is refused, but no evidence that VRA is imprecise)
        long long unproved = 0;
        for (int64_t j = 0; j < R.n; j++) if (R.r[j].fn == f && R.r[j].k != JREC_PROVED && R.r[j].cls != '?') unproved++;
        for (int i = 0; i < V->nchecks; i++) {
            VraCheck *c = &V->checks[i];
            // a LOOP's termination (PE.5): VRA's check names its header (loop1 - 1); the judge's
            // rank.* records, or measure.* for a WRITTEN measure, sit on the back edges into it (the
            // predecessor branches to it), and with nothing proposed or a measure it cannot read there
            // is one `outside` record at the header, which is the judge proving nothing.
            // A RECURSION's: VRA files ONE check per function (at a self-call, or at the call that
            // enters its cycle); the judge owes rec.* at every call that may close a cycle through it,
            // so the function's check is proved by the judge when every rec.* record in it is.
            if (c->kind == VRA_TERMINATION && c->recursion) {
                int seen = 0, proved = 0; JRecordKind worst = JREC_PROVED;
                for (int64_t j = 0; j < R.n; j++) {
                    JshRec *q = &R.r[j];
                    if (q->fn != f || q->cls != 'R') continue;
                    q->used = true; seen++;
                    if (q->k == JREC_PROVED) proved++; else worst = q->k;
                }
                n_chk++; nrec++;
                bool jok = seen && proved == seen;
                if (!seen) { unowed++; fprintf(stderr, "judge-shadow: unowed %.*s Ln %lld, Col %lld: a recursion's termination check (VRA %s) has no obligation\n",
                                                (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, c->ok ? "proves it" : "refuses it"); }
                else if (c->ok && jok) { agree_p++; rec_p++; }
                else if (!c->ok && !jok) agree_r++;
                else if (c->ok) { vra_only++; fprintf(stderr, "judge-shadow: vra-only %.*s Ln %lld, Col %lld: VRA proves this recursion terminates, the judge: %s\n",
                                                      (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, jsh_kind_name(worst)); }
                else { judge_only++; rec_j++; fprintf(stderr, "judge-shadow: judge-only %.*s Ln %lld, Col %lld: the judge proves this recursion terminates, VRA refuses it\n",
                                                       (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col); }
                continue;
            }
            if (c->kind == VRA_TERMINATION) {
                if (c->loop1 <= 0) { term++; continue; }
                int32_t hid = c->loop1 - 1;
                int seen = 0, proved = 0; JRecordKind worst = JREC_PROVED;
                for (int64_t j = 0; j < R.n; j++) {
                    JshRec *q = &R.r[j];
                    if (q->fn != f || !q->blk) continue;
                    bool rank = q->cls == 'T' && ((q->blk->term.a && q->blk->term.a->id == hid) ||
                                                  (q->blk->term.b && q->blk->term.b->id == hid));
                    bool none = q->k == JREC_OUTSIDE && !q->at && q->blk->id == hid;
                    if (!rank && !none) continue;
                    q->used = true; seen++;
                    if (q->k == JREC_PROVED) proved++; else worst = q->k;
                }
                n_chk++; nterm++;
                bool jok = seen && proved == seen;
                if (!seen) { unowed++; fprintf(stderr, "judge-shadow: unowed %.*s Ln %lld, Col %lld: a termination check (VRA %s) has no obligation\n",
                                                (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, c->ok ? "proves it" : "refuses it"); }
                else if (c->ok && jok) { agree_p++; term_p++; }
                else if (!c->ok && !jok) agree_r++;
                else if (c->ok) { vra_only++; fprintf(stderr, "judge-shadow: vra-only %.*s Ln %lld, Col %lld: VRA proves this loop terminates, the judge: %s\n",
                                                      (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, jsh_kind_name(worst)); }
                else { judge_only++; term_j++; fprintf(stderr, "judge-shadow: judge-only %.*s Ln %lld, Col %lld: the judge proves this loop terminates, VRA refuses it\n",
                                                       (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col); }
                continue;
            }
            char cc = jsh_vra_class(c);
            if (!cc) continue;
            // VRA may file two checks of one class at one instruction, the judge one record there
            // under another rule (`return n + 1`: VRA's add proved and the return's landing refused,
            // the judge's narrowing at the add not implied): one comparison per instruction and
            // class, VRA proving it when it proves every check there (ackermann_checked's "vra-only"
            // where the default mode itself reports E086, Documentation 2026-10-20)
            bool vok = c->ok, dup = false;
            for (int i2 = 0; c->at && i2 < V->nchecks && !dup; i2++) {
                VraCheck *c2 = &V->checks[i2];
                if (c2 == c || c2->at != c->at || jsh_vra_class(c2) != cc) continue;
                if (i2 < i) dup = true; else vok = vok && c2->ok;
            }
            if (dup) continue;
            n_chk++;
            int seen = 0, proved = 0, out = 0; JRecordKind worst = JREC_PROVED;
            for (int64_t j = 0; j < R.n; j++) {
                JshRec *q = &R.r[j];
                // an `outside` record carries no kind of its own: the judge says the place is beyond
                // what it can state, which is not the same as owing nothing there
                if (q->fn == f && q->k == JREC_OUTSIDE && c->at && jsh_vra_at(V, q) == c->at) { q->used = true; out++; continue; }
                if (q->fn != f || q->cls != cc) continue;
                // a return of a value with no defining instruction (a parameter, `return a`): VRA
                // files its landing with no instruction, at the return's position, so it is keyed
                // by the return's terminator there
                if (!c->at ? !(!q->at && q->blk && q->blk->term.kind == IR_TERM_RET &&
                               q->blk->term.line == c->line && q->blk->term.col == c->col)
                           : jsh_vra_at(V, q) != c->at) continue;
                q->used = true; seen++;
                if (q->k == JREC_PROVED) proved++; else worst = q->k;
            }
            const char *cls = cc == 'B' ? "bounds" : cc == 'D' ? "division-by-zero" : cc == 'O' ? "overflow"
                            : cc == 'V' ? "TYPE_MIN / -1" : "precondition";
            const char *art = cc == 'O' ? "an" : "a";
            if (!seen && out) {
                outside++;
                fprintf(stderr, "judge-shadow: outside %.*s Ln %lld, Col %lld: %s %s check (VRA %s) is beyond what the judge states\n",
                        (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, art, cls,
                        vok ? "proves it" : "refuses it");
                continue;
            }
            if (!seen) {
                unowed++;
                fprintf(stderr, "judge-shadow: unowed %.*s Ln %lld, Col %lld: %s %s check (VRA %s) has no obligation\n",
                        (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, art, cls,
                        vok ? "proves it" : "refuses it");
                continue;
            }
            bool jok = proved == seen;
            if (vok && jok) agree_p++;
            else if (!vok && !jok) agree_r++;
            else if (vok) {
                vra_only++;
                fprintf(stderr, "judge-shadow: vra-only %.*s Ln %lld, Col %lld: VRA proves this %s check, the judge: %s\n",
                        (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, cls, jsh_kind_name(worst));
            } else {
                judge_only++; if (unproved) judge_only_c++;
                fprintf(stderr, "judge-shadow: judge-only %.*s Ln %lld, Col %lld: the judge proves this %s check, VRA refuses it%s\n",
                        (int)f->name->length, f->name->name, (long long)c->line, (long long)c->col, cls,
                        unproved ? " (the function has an unproved obligation this proof may rest on)" : "");
            }
        }
        vra_free(V);
    }
    for (int64_t j = 0; ran && j < R.n; j++) {
        if (R.r[j].used || R.r[j].cls == '?') continue;
        extra++; if (R.r[j].k == JREC_PROVED) extra_p++;
        if (R.r[j].k != JREC_PROVED)
            fprintf(stderr, "judge-shadow: judge-extra %s: %s, where VRA checks nothing of its class\n", R.r[j].id, jsh_kind_name(R.r[j].k));
    }
    if (ran)
        fprintf(stderr, "judge-shadow: %lld VRA checks: %lld agree (%lld proved, %lld refused), %lld vra-only, %lld judge-only (%lld conditional), "
                        "%lld unowed, %lld outside; %lld judge-extra (%lld proved); %lld loops' termination among them (%lld proved by both, %lld by the judge only), %lld recursions' termination among them (%lld proved by both, %lld by the judge only), %lld termination checks not judged; records: "
                        "%lld proved, %lld not-implied, %lld outside, %lld budget, %lld capacity, %lld solver-bug, %lld uncertified\n",
                n_chk, agree_p + agree_r, agree_p, agree_r, vra_only, judge_only, judge_only_c, unowed, outside, extra, extra_p, nterm, term_p, term_j, nrec, rec_p, rec_j, term,
                R.kinds[JREC_PROVED], R.kinds[JREC_NOT_IMPLIED], R.kinds[JREC_OUTSIDE], R.kinds[JREC_BUDGET],
                R.kinds[JREC_CAPACITY], R.kinds[JREC_SOLVER_BUG], R.kinds[JREC_UNCERTIFIED]);
    free(R.r); free(jc.last); free(jc.last_rk); free(jc.last_gc); free(buf); free(work);
    for (int i = 0; i < jsh_nhdr; i++) free((void *)jsh_hdr[i].r);
    free(jsh_hdr); jsh_hdr = NULL; jsh_nhdr = jsh_chdr = 0;
}

#endif // LAIN_ANALYSIS_JUDGE_SHADOW_H
