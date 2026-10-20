// judge_authority.h — `--judge=authority` (PE.10, phase 4 of the proof-engine plan). The judge's
// records DECIDE every obligation its generator owes: bounds, overflow and narrowing, division,
// asserts, object sizes, and the termination of every loop and recursion. VRA's checks are read for
// no verdict. Design: local/internal/design/judge_authority.md.
//
// NO FALLBACK. A record that is not `proved` refuses the program: `not-implied`, `outside`, `budget`
// and `capacity` with the obligation's own code (outside and budget say why on a second line);
// `solver-bug`, `uncertified` and `refused` as an internal error (exit 70), since each is a compiler
// bug, in the solver, the proposer or the generator. A fallback to VRA would keep VRA's soundness
// bugs authoritative, which is what the switch exists to end.
//
// AN ADAPTER, NOT A SECOND REPORTER. Each unproved obligation becomes the check record report.h
// already prints (ir_report_check), placed where VRA files it, so a code and its text are one thing
// in both modes. A record has no words of its own for E011's path excerpt (I.200): VRA's witness
// gives them (jau_vra_twin, for the words only); and an accumulator's E086 in a loop the judge did
// not show to end is dropped as that E011's consequence (I.159, judge_authority_report).
//
// Opt-in. VRA still runs for the finder's candidates (untrusted) and, in report.h, for the borrow
// pass's disjointness queries (phase D) until those are owed as asserts. The effect row's DIVERGE is
// read from the row alone (effects.h, ir_effects_row_only): an accepted program has every loop and
// recursion obligation of a function without `diverge` proved.
#ifndef LAIN_ANALYSIS_JUDGE_AUTHORITY_H
#define LAIN_ANALYSIS_JUDGE_AUTHORITY_H
#include "analysis/judge_shadow.h"   // the untrusted callbacks (solver, finder) and VRA's header states

// The records kept: every one but `proved`, and every recursion's (its site is its first call).
typedef struct {
    IrFunc *fn; IrInstr *at; IrBlock *blk; int32_t idx;
    JRecordKind k;
    char kind[32];                   // the obligation's kind: bounds.hi, rank.fall, assert.121, ...
    char reason[200];                // an outside's, a budget's
} JauRec;
static JauRec *jau_r; static int64_t jau_n, jau_cap;
static bool jau_oom;
static char jau_internal[400];       // the first compiler bug the judge reported, if any

static void jau_sink(void *ctx, const JRecord *r) {
    (void)ctx;
    if (r->k == JREC_NO_CLAIMS) return;              // judged without claims: each record says the rest
    if (r->k == JREC_SOLVER_BUG || r->k == JREC_UNCERTIFIED || r->k == JREC_REFUSED) {
        if (!jau_internal[0])
            snprintf(jau_internal, sizeof jau_internal, "%s%s%.*s%s%s",
                     jsh_kind_name(r->k), r->fn ? " in " : "", r->fn ? (int)r->fn->name->length : 0,
                     r->fn ? r->fn->name->name : "", r->reason ? ": " : "", r->reason ? r->reason : "");
        return;
    }
    if (!r->id || !r->id->kind || !r->fn) return;
    bool rec = !strncmp(r->id->kind, "rec.", 4);
    if (r->k == JREC_PROVED && !rec) return;
    if (jau_n == jau_cap) {
        int64_t nc = jau_cap ? 2 * jau_cap : 256;
        JauRec *x = realloc(jau_r, sizeof *x * (size_t)nc);
        if (!x) { jau_oom = true; return; }
        jau_r = x; jau_cap = nc;
    }
    JauRec *q = &jau_r[jau_n++];
    memset(q, 0, sizeof *q);
    q->fn = r->fn; q->at = r->at; q->blk = r->blk; q->idx = r->idx; q->k = r->k;
    snprintf(q->kind, sizeof q->kind, "%s", r->id->kind);
    if (r->reason) snprintf(q->reason, sizeof q->reason, "%s", r->reason);
}

// --dump-measures under --judge=authority: each ranking the judge CERTIFIED (the finder proposed it,
// and no rank.*, measure.* or rec.* record of its loop or function is unproved), in the source's
// names. VRA's own measure is not printed in this mode: it decides nothing here (Documentation found
// "no measure found" printed for loops the judge had proved, 2026-10-20). A ranking is the finder's,
// untrusted and not unique: what is printed is the one this build proved, not THE measure.
static bool jau_dump_measures;
static IrInstr *jau_def(IrFunc *f, IrValue *v);
static IrBlock *jau_header(const JauRec *q);
static void jau_name(IrFunc *f, int32_t key, char *b, size_t n) {
    const IrName *nm = NULL;
    for (IrParam *p = f->params; p && !nm; p = p->next) if (p->value && p->value->id == key) nm = p->value->src_name;
    for (IrBlock *bb = f->blocks; bb && !nm; bb = bb->next)
        for (IrInstr *i = bb->instrs; i && !nm; i = i->next) if (i->result && i->result->id == key) nm = i->result->src_name;
    if (nm && nm->length > 0) snprintf(b, n, "%.*s", (int)nm->length, nm->name);
    else snprintf(b, n, "%%%d", (int)key);
}
static void jau_row(IrFunc *f, const JRank *r, int32_t j, char *b, size_t n) {
    size_t at = 0; b[0] = 0;
    long long k = (long long)r->c0[j];
    bool pos = false;
    for (int32_t t = 0; t < r->nt; t++) pos |= r->coef[j][t] > 0;
    if (!pos && k > 0) { at += (size_t)snprintf(b, n, "%lld", k); k = 0; }   // `2147483647 - q`
    for (int32_t pass = 0; pass < 2; pass++)                 // the positive terms first: `n - i`
    for (int32_t t = 0; t < r->nt; t++) {
        __int128 c = r->coef[j][t];
        if (c == 0 || (pass == 0) != (c > 0)) continue;
        char nm[80], term[120];
        jau_name(f, r->key[t], nm, sizeof nm);
        const char *fmt = r->tk[t] == JTERM_LEN ? "len(%s)" : r->tk[t] == JTERM_ENTRY ? "entry(%s)" : "%s";
        char tn[100]; snprintf(tn, sizeof tn, fmt, nm);
        long long a = (long long)(c < 0 ? -c : c);
        if (a == 1) snprintf(term, sizeof term, "%s%s", at ? (c < 0 ? " - " : " + ") : (c < 0 ? "-" : ""), tn);
        else snprintf(term, sizeof term, "%s%lld*%s", at ? (c < 0 ? " - " : " + ") : (c < 0 ? "-" : ""), a, tn);
        at += (size_t)snprintf(b + at, at < n ? n - at : 0, "%s", term);
    }
    if (k || !at) at += (size_t)snprintf(b + at, at < n ? n - at : 0, at ? (k < 0 ? " - %lld" : " + %lld") : "%lld", at ? (k < 0 ? -k : k) : k);
}
static void jau_print_measures(const JRank *rk, int32_t nrk) {
    for (int32_t i = 0; i < nrk; i++) {
        const JRank *r = &rk[i];
        IrFunc *f = r->fn;
        if (!f) continue;
        bool rec = r->blk == JRANK_ENTRY, ok = true;
        for (int64_t q = 0; q < jau_n && ok; q++) {
            const JauRec *x = &jau_r[q];
            if (x->fn != f || x->k == JREC_PROVED) continue;
            if (rec ? !strncmp(x->kind, "rec.", 4)
                    : (!strncmp(x->kind, "rank.", 5) || !strncmp(x->kind, "measure.", 8)) && jau_header(x) && jau_header(x)->id == r->blk) ok = false;
        }
        if (!ok) continue;
        isize line = 0;
        if (!rec) {
            IrBlock *H = NULL;
            for (IrBlock *b = f->blocks; b && !H; b = b->next) if (b->id == r->blk) H = b;
            IrInstr *hc = H && H->term.cond ? jau_def(f, H->term.cond) : NULL;
            line = hc && hc->line ? hc->line : H ? H->term.line : 0;
        }
        char row[600], all[2000]; size_t at = 0; all[0] = 0;
        const IrName *fn = f->src_name ? f->src_name : f->name;
        if (r->nreg == 0) {
            for (int32_t j = 0; j < r->ncomp; j++) {
                jau_row(f, r, j, row, sizeof row);
                at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, "%s%s", j ? ", " : "", row);
            }
            if (rec) fprintf(stderr, "[measure] %.*s: recursion: `%s%s%s` decreases at every call that may close the cycle (the judge's)\n",
                             fn ? (int)fn->length : 1, fn ? fn->name : "?", r->ncomp > 1 ? "<" : "", all, r->ncomp > 1 ? ">" : "");
            else fprintf(stderr, "[measure] %.*s: loop at line %lld: `%s%s%s` decreases%s (the judge's)\n",
                         fn ? (int)fn->length : 1, fn ? fn->name : "?", (long long)line, r->ncomp > 1 ? "<" : "", all,
                         r->ncomp > 1 ? ">" : "", r->ncomp > 1 ? " lexicographically" : "");
            continue;
        }
        for (int32_t k = 0; k < r->nreg; k++) {                // piecewise: each region's guard and tuple
            at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, "%swhere ", k ? "; " : "");
            for (int32_t g = r->reg[k].g0; g < r->reg[k].g0 + r->reg[k].ng; g++) {
                jau_row(f, r, g, row, sizeof row);
                at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, "%s%s >= 0", g > r->reg[k].g0 ? " and " : "", row);
            }
            at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, ": <");
            for (int32_t t = r->reg[k].t0; t < r->reg[k].t0 + r->reg[k].nt; t++) {
                jau_row(f, r, t, row, sizeof row);
                at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, "%s%s", t > r->reg[k].t0 ? ", " : "", row);
            }
            at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, ">");
        }
        fprintf(stderr, "[measure] %.*s: loop at line %lld: piecewise, %s (the judge's)\n",
                fn ? (int)fn->length : 1, fn ? fn->name : "?", (long long)line, all);
    }
}
// Run the judge on `mod`. False with jau_internal set when it could not run, or reported a compiler
// bug: the caller refuses the program with exit 70, never emits it.
static bool judge_authority_run(IrFunc *mod) {
    free(jau_r); jau_r = NULL; jau_n = jau_cap = 0; jau_oom = false; jau_internal[0] = 0;
    jsh_nhdr = 0;                                     // VRA's header states: the finder's candidates
    for (IrFunc *f = mod; f; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        vra_header_sink = jsh_hdr_sink; vra_header_ctx = NULL;
        Vra *VH = vra_analyze(f);
        vra_header_sink = NULL;
        if (VH) vra_free(VH);
    }
    size_t cap = (size_t)1 << 28, wcap = (size_t)1 << 26;
    char *buf = malloc(cap), *work = malloc(wcap);
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
        ran = judge_module(mod, &lim, &calls, jau_sink, NULL, &m, err, sizeof err);
    } else snprintf(err, sizeof err, "no memory for the judge's region");
    if (!ran && !jau_internal[0]) snprintf(jau_internal, sizeof jau_internal, "the judge could not run: %s", err);
    if (jau_oom && !jau_internal[0]) snprintf(jau_internal, sizeof jau_internal, "out of memory keeping the judge's records");
    if (ran && jau_dump_measures && jc.last_rk) jau_print_measures(jc.last_rk, F.nrk);
    free(buf); free(work); free(jc.last); free(jc.last_rk); free(jc.last_gc);
    for (int i = 0; i < jsh_nhdr; i++) free((void *)jsh_hdr[i].r);
    free(jsh_hdr); jsh_hdr = NULL; jsh_nhdr = jsh_chdr = 0;
    return !jau_internal[0];
}

// Where VRA would put a record's diagnostic. An instruction's position; a return's landing at the
// returned value's definition (VRA files a returned quotient's narrowing at the division), else at
// the return; a loop's termination at its header's test (the predecessor of a back edge branches to
// the header; an `outside` sits at the header itself).
static IrInstr *jau_def(IrFunc *f, IrValue *v) {
    for (IrBlock *b = f->blocks; v && b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) if (i->result == v) return i;
    return NULL;
}
static IrBlock *jau_header(const JauRec *q) {
    IrBlock *b = q->blk;
    if (!b) return NULL;
    if (!q->at && b->is_loop_header && strstr(q->kind, ".fall") && q->k == JREC_OUTSIDE) return b;
    if (b->term.a && b->term.a->is_loop_header) return b->term.a;
    if (b->term.b && b->term.b->is_loop_header) return b->term.b;
    return b->is_loop_header ? b : NULL;
}
static void jau_site(IrFunc *f, const JauRec *q, bool loop, isize *line, isize *col) {
    *line = 0; *col = 0;
    if (loop) {
        IrBlock *H = jau_header(q);
        if (!H) return;
        IrInstr *hc = H->term.cond ? jau_def(f, H->term.cond) : NULL;
        if (hc && hc->line) { *line = hc->line; *col = hc->col; }
        else { *line = H->term.line; *col = H->term.col; }
        return;
    }
    if (q->at) { *line = q->at->line; *col = q->at->col; return; }
    if (q->blk && q->blk->term.kind == IR_TERM_RET && q->blk->term.cond) {
        IrInstr *d = jau_def(f, q->blk->term.cond);
        if (d && d->line) { *line = d->line; *col = d->col; return; }
    }
    if (q->blk) { *line = q->blk->term.line; *col = q->blk->term.col; }
}

// An `outside` reason as the user reads it. The generator's reasons name its own steps ("atom.mul
// ... step A", "F1 and F2"), which mean nothing outside it. NULL: no second line at all, because the
// first already says it (E011 is "no measure could be inferred").
static const char *jau_reason_text(const char *r) {
    if (!r || !r[0]) return "this is beyond what the prover can state yet";
    if (strstr(r, "no ranking function proposed") || strstr(r, "no measure proposed")) return NULL;
    if (strstr(r, "atom.mul")) return "the prover cannot yet bound a product of two variables";
    if (strstr(r, "float converted")) return "the prover cannot yet bound a float converted to an integer";
    if (strstr(r, "pointer that may close a recursion")) return "a call through a function pointer may close this recursion, and the prover cannot rank such a call yet";
    if (strstr(r, "written measure")) return "the prover cannot read the `decreasing` measure written as a sum of terms it tracks";
    if (strstr(r, "no length is known")) return "the prover knows no length for what is indexed here";
    if (strstr(r, "per lane")) return "the prover cannot yet check a value given per vector lane here";
    return "this is beyond what the prover can state yet";
}
// VRA's check at the place and of the class a record is about, for its WORDS only: the witness of a
// loop that does not end, a running total's numbers, the range a value lands in, which division case.
// The judge has decided; this says it as the default mode does. NULL when VRA checks nothing there.
static VraCheck *jau_vra_twin(Vra *V, IrFunc *f, const JauRec *q, const VraCheck *c) {
    if (!V) return NULL;
    IrInstr *at = q->at;
    if (!at && q->blk && q->blk->term.kind == IR_TERM_RET && q->blk->term.cond) at = jau_def(f, q->blk->term.cond);
    for (int i = 0; i < V->nchecks; i++) {
        VraCheck *v = &V->checks[i];
        if (v->kind != c->kind) continue;
        if (c->kind == VRA_TERMINATION) {
            if (c->recursion ? v->recursion : (!v->recursion && v->loop1 == c->loop1)) return v;
            continue;
        }
        if (at && v->at == at) return v;
    }
    return NULL;
}
// f's unproved obligations as diagnostics, in source order; the count printed. Installed as
// report.h's ir_report_numeric_hook; `V` is VRA's analysis of f, read for words only.
static int judge_authority_report(IrFunc *f, const char *file, Vra *V) {
    typedef struct { VraCheck c; const JauRec *q; } Item;
    int64_t cap = 16, n = 0;
    Item *it = malloc(sizeof *it * (size_t)cap);
    int nb = f->next_block_id > 0 ? f->next_block_id : 1;
    char *noterm = calloc((size_t)nb + 1, 1);           // loops (header id + 1) the judge did not show to end
    if (!it || !noterm) { free(it); free(noterm); fprintf(stderr, "internal error: out of memory reporting the judge's records; refusing to emit\n"); return 1; }
    for (int64_t j = 0; j < jau_n; j++) {
        const JauRec *q = &jau_r[j];
        if (q->fn != f || q->k == JREC_PROVED || (strncmp(q->kind, "rank.", 5) && strncmp(q->kind, "measure.", 8))) continue;
        IrBlock *H = jau_header(q);
        if (H && H->id + 1 <= nb) noterm[H->id + 1] = 1;
    }
    // a recursion: ONE diagnostic for the function, at the first call whose obligation FAILS (the
    // call that may close the cycle, not a self-call that falls)
    const JauRec *rsite = NULL; bool rptr = false;
    for (int64_t j = 0; j < jau_n; j++) {
        const JauRec *q = &jau_r[j];
        if (q->fn != f || strncmp(q->kind, "rec.", 4) || q->k == JREC_PROVED) continue;
        if (q->k == JREC_OUTSIDE && strstr(q->reason, "pointer")) rptr = true;
        isize l, c, rl, rc;
        jau_site(f, q, false, &l, &c);
        if (rsite) jau_site(f, rsite, false, &rl, &rc);
        if (!rsite || l < rl || (l == rl && c < rc)) rsite = q;
    }
    for (int64_t j = 0; j < jau_n; j++) {
        const JauRec *q = &jau_r[j];
        if (q->fn != f || q->k == JREC_PROVED) continue;
        bool rec = !strncmp(q->kind, "rec.", 4);
        if (rec && q != rsite) continue;
        VraCheck c; memset(&c, 0, sizeof c);
        bool loop = !strncmp(q->kind, "rank.", 5) || !strncmp(q->kind, "measure.", 8);
        if (rec) {
            c.kind = VRA_TERMINATION; c.recursion = true; c.had_measure = f->has_decreasing;
            // a written measure's failing part (I.206): rec.fall its descent, rec.le that it does
            // not grow (a call off the cut), rec.bound its bound, rec.def its definedness. By exact
            // name (`.le` was a substring of rec.lex, c42's), and FAIL CLOSED on any other: a written
            // measure is one row, so it owes none (rec.lex is a measure of several rows')
            if (f->has_decreasing) {
                const char *k = q->kind;
                c.judge_measure = !strcmp(k, "rec.fall") ? 1 : !strcmp(k, "rec.bound") ? 2 : !strcmp(k, "rec.def") ? 3 : !strcmp(k, "rec.le") ? 4 : 0;
                if (!c.judge_measure) {
                    snprintf(jau_internal, sizeof jau_internal, "a written measure's obligation of a kind --judge=authority does not map (%s, in %.*s)",
                             k, (int)f->name->length, f->name->name);
                    fprintf(stderr, "internal error: the judge: %s; refusing to emit\n", jau_internal);
                    free(it); free(noterm); return 1;     // main.c exits 70 on jau_internal
                }
            }
            c.via_pointer = rptr;
            IrCallGraph G; memset(&G, 0, sizeof G);
            int kf = ir_cg_build(&G, vra_mod) ? ir_cg_index(&G, f) : -1;
            c.mutual = rptr || (kf >= 0 && ir_cg_members(&G, kf, NULL, 0) > 1);
            ir_cg_free(&G);
        } else if (loop) {
            IrBlock *H = jau_header(q);
            c.kind = VRA_TERMINATION; c.loop1 = H ? H->id + 1 : 0; c.had_measure = H && H->has_measure;
            if (!strncmp(q->kind, "measure.", 8)) c.judge_measure = strstr(q->kind, "bound") ? 2 : 1;
        } else if (!strncmp(q->kind, "bounds", 6)) {
            c.kind = VRA_BOUNDS; c.has_len = !(q->k == JREC_OUTSIDE && strstr(q->reason, "no length"));
        } else if (!strncmp(q->kind, "narrow", 6) || !strcmp(q->kind, "shift")) {
            c.kind = VRA_OVERFLOW; if (!strcmp(q->kind, "shift")) c.shift = 1;
        } else if (!strcmp(q->kind, "divovf")) {
            c.kind = VRA_OVERFLOW; c.shift = 3;
        } else if (!strcmp(q->kind, "divzero")) {
            c.kind = VRA_DIVZERO; c.bitcount = q->at && (q->at->op == IR_CTZ || q->at->op == IR_CLZ);
        } else if (!strcmp(q->kind, "nonzero")) {     // @ctz/@clz's argument
            c.kind = VRA_DIVZERO; c.bitcount = true;
        } else if (!strncmp(q->kind, "assert.", 7)) {
            c.kind = VRA_PRECOND; c.diag = atoi(q->kind + 7);
        } else if (!strcmp(q->kind, "objsize")) {
            c.kind = VRA_PRECOND; c.diag = 2085;
        } else if (!strcmp(q->kind, "post") || !strcmp(q->kind, "hull") || !strncmp(q->kind, "inv.", 4)) {
            continue;                                 // a claim's own VC: if it fails, the judge says
                                                      // `uncertified` for every obligation (an internal error)
        } else {
            // FAIL CLOSED: a kind this adapter does not map is refused, never skipped. `nonzero` was
            // skipped in the first draft, and `@ctz(0)` compiled.
            snprintf(jau_internal, sizeof jau_internal, "an obligation of a kind --judge=authority does not map (%s, in %.*s)",
                     q->kind, (int)f->name->length, f->name->name);
            fprintf(stderr, "internal error: the judge: %s; refusing to emit\n", jau_internal);
            free(it); free(noterm); return 1;         // main.c exits 70 on jau_internal
        }
        jau_site(f, q, loop, &c.line, &c.col);
        // VRA's words for the same check, where it has one; the decision stays the judge's
        VraCheck *tw = jau_vra_twin(V, f, q, &c);
        if (tw) {
            VraCheck d = *tw;
            d.ok = false; d.judge_measure = c.judge_measure;
            // VRA's PROGRAM facts only (a witness path, a range, a running total's numbers, a site);
            // what VRA says of its own search (its cycle cap, "the loop ends by another measure") is
            // not the reason the judge refused, and is dropped (Documentation, 2026-10-13)
            d.cycle_cap = 0; d.cycle_n = 0; d.measure_mismatch = false;
            if (c.kind == VRA_BOUNDS) d.has_len = c.has_len && tw->has_len;
            if (c.kind == VRA_PRECOND) d.diag = c.diag;
            if (c.kind == VRA_TERMINATION && c.recursion) { d.via_pointer = c.via_pointer || tw->via_pointer; d.mutual = c.mutual || tw->mutual; }
            if (c.kind == VRA_TERMINATION && !c.recursion && !tw->ok) { /* VRA's witness and its site */ }
            else if (c.kind == VRA_TERMINATION && !c.recursion) { d.wit_counter = NULL; d.wit_n = 0; }
            c = d;
        }
        c.by_judge = true;
        // I.159: a running total in a loop the judge did not show to end is a consequence of that
        // E011, not a second error (Zune's `year = year + 1`); the loop's E011 is reported
        if (c.kind == VRA_OVERFLOW && c.accum && c.accum_T < 0 && c.loop1 > 0 && c.loop1 <= nb && noterm[c.loop1]) continue;
        // ...and in a loop the judge shows to end, VRA's "no bound on the trip count was found"
        // is its own search's result again (field_bound_accumulator_grows: 1000 trips, a u8 total)
        if (c.kind == VRA_OVERFLOW && c.accum && c.accum_T < 0) c.accum_T = -2;
        // one diagnostic per place and rule: bounds.lo and bounds.hi at one access are one E085
        bool dup = false;
        for (int64_t k = 0; k < n && !dup; k++)
            dup = it[k].c.line == c.line && it[k].c.col == c.col && it[k].c.kind == c.kind && it[k].c.diag == c.diag;
        if (dup) continue;
        if (n == cap) { Item *x = realloc(it, sizeof *x * (size_t)(cap *= 2)); if (!x) break; it = x; }
        it[n].c = c; it[n].q = q; n++;
    }
    free(noterm);
    for (int64_t a = 1; a < n; a++)                   // source order (insertion: the lists are short)
        for (int64_t b = a; b > 0 && (it[b].c.line < it[b-1].c.line ||
                                      (it[b].c.line == it[b-1].c.line && it[b].c.col < it[b-1].c.col)); b--) {
            Item t = it[b]; it[b] = it[b-1]; it[b-1] = t;
        }
    int printed = 0;
    for (int64_t k = 0; k < n; k++) {
        if (!ir_report_check(file, V, &it[k].c)) continue;
        printed++;
        const JauRec *q = it[k].q;
        const char *why = q->k == JREC_OUTSIDE ? jau_reason_text(q->reason) : NULL;
        if (why) fprintf(stderr, "       %s\n", why);
        else if (q->k == JREC_BUDGET || q->k == JREC_CAPACITY)
            fprintf(stderr, "       the proof ran out of its %s here\n", q->k == JREC_BUDGET ? "budget" : "room");
    }
    free(it);
    return printed;
}

#endif // LAIN_ANALYSIS_JUDGE_AUTHORITY_H
