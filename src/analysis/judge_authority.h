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
#include <stdarg.h>                  // jau_put
#include "analysis/judge_shadow.h"   // the untrusted callbacks (solver, finder) and VRA's header states

// The records kept: every one but `proved`, and every recursion's (its site is its first call).
typedef struct {
    IrFunc *fn; IrInstr *at; IrBlock *blk; int32_t idx;
    JRecordKind k;
    char kind[32];                   // the obligation's kind: bounds.hi, rank.fall, assert.121, ...
    char reason[200];                // an outside's, a budget's
} JauRec;
static JauRec *jau_r; static int64_t jau_n, jau_cap;
// PE.12: a loop the judge refused, and the input it never ends for (the note's line), run first
typedef struct { IrFunc *fn; int32_t header; char note[200]; } JauWit;
static JauWit *jau_w; static int32_t jau_nw;
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
static void jau_site(IrFunc *f, const JauRec *q, bool loop, isize *line, isize *col);
// A term in the program's names, as VRA's --dump-measures spells them (vra_src_expr): a cell by its
// variable, a field by its path (`l.pos`), a slice's length as `s.len`, through loads and casts;
// an IR number (%N) only where no name exists.
// jau_src's writes into its caller's buffer: a name longer than the buffer is CUT, as it always was.
// Through vsnprintf, so gcc's truncation analysis, which sees the recursion's 200-byte buffers and
// warned on four of the literal formats below, is not in play; the formats and the text are the same.
static void jau_put(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void jau_put(char *b, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, n, fmt, ap);
    va_end(ap);
}
// ...and appended to what the buffer holds (PE.12's note is built from parts)
static void jau_cat(char *b, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void jau_cat(char *b, size_t n, const char *fmt, ...) {
    size_t at = strlen(b);
    if (at + 1 >= n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + at, n - at, fmt, ap);
    va_end(ap);
}
static void jau_src(IrFunc *f, IrValue *v, char *b, size_t n, int depth) {
    if (!v || n < 2) { jau_put(b, n, "?"); return; }
    if (depth > 8) { jau_put(b, n, "%%%d", v->id); return; }
    IrInstr *d = jau_def(f, v);
    if (!d || d->op == IR_ALLOCA) {                          // a parameter or a local
        if (v->src_name && v->src_name->length > 0) jau_put(b, n, "%.*s", (int)v->src_name->length, v->src_name->name);
        else jau_put(b, n, "%%%d", v->id);
        return;
    }
    char x[200], y[200];
    switch (d->op) {
        case IR_CONST: jau_put(b, n, "%lld", (long long)d->aux.imm); return;
        case IR_LOAD: case IR_CAST: jau_src(f, d->n_operands >= 1 ? d->operands[0] : NULL, b, n, depth + 1); return;
        case IR_SLICE_LEN: jau_src(f, d->operands[0], x, sizeof x, depth + 1); jau_put(b, n, "%s.len", x); return;
        case IR_FIELD_PTR: {
            jau_src(f, d->n_operands >= 1 ? d->operands[0] : NULL, x, sizeof x, depth + 1);
            IrType *bt = d->n_operands >= 1 ? d->operands[0]->type : NULL;
            if (bt && bt->kind == IRT_PTR && bt->elem) bt = bt->elem;
            int k = (int)d->aux.field_idx;
            IrName *fn = (bt && bt->kind == IRT_STRUCT && k >= 0 && k < bt->n_fields && bt->field_names) ? bt->field_names[k] : NULL;
            if (fn) jau_put(b, n, "%s.%.*s", x, (int)fn->length, fn->name); else jau_put(b, n, "%s.%d", x, k);
            return;
        }
        case IR_ADD: case IR_SUB:
            jau_src(f, d->operands[0], x, sizeof x, depth + 1); jau_src(f, d->operands[1], y, sizeof y, depth + 1);
            jau_put(b, n, "(%s %c %s)", x, d->op == IR_ADD ? '+' : '-', y);
            return;
        default:
            if (v->src_name && v->src_name->length > 0) jau_put(b, n, "%.*s", (int)v->src_name->length, v->src_name->name);
            else jau_put(b, n, "%%%d", v->id);
            return;
    }
}
static IrValue *jau_value_of(IrFunc *f, int32_t key) {
    IrValue *v = NULL;
    for (IrParam *p = f->params; p && !v; p = p->next) if (p->value && p->value->id == key) v = p->value;
    for (IrBlock *bb = f->blocks; bb && !v; bb = bb->next)
        for (IrInstr *i = bb->instrs; i && !v; i = i->next) if (i->result && i->result->id == key) v = i->result;
    return v;
}
static void jau_name(IrFunc *f, int32_t key, char *b, size_t n) {
    IrValue *v = NULL;
    for (IrParam *p = f->params; p && !v; p = p->next) if (p->value && p->value->id == key) v = p->value;
    for (IrBlock *bb = f->blocks; bb && !v; bb = bb->next)
        for (IrInstr *i = bb->instrs; i && !v; i = i->next) if (i->result && i->result->id == key) v = i->result;
    if (v) jau_src(f, v, b, n, 0); else snprintf(b, n, "%%%d", (int)key);
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
        const char *fmt = r->tk[t] == JTERM_LEN ? "%s.len" : r->tk[t] == JTERM_ENTRY ? "entry(%s)" : "%s";
        char tn[100]; snprintf(tn, sizeof tn, fmt, nm);
        long long a = (long long)(c < 0 ? -c : c);
        if (a == 1) snprintf(term, sizeof term, "%s%s", at ? (c < 0 ? " - " : " + ") : (c < 0 ? "-" : ""), tn);
        else snprintf(term, sizeof term, "%s%lld*%s", at ? (c < 0 ? " - " : " + ") : (c < 0 ? "-" : ""), a, tn);
        at += (size_t)snprintf(b + at, at < n ? n - at : 0, "%s", term);
    }
    if (k || !at) at += (size_t)snprintf(b + at, at < n ? n - at : 0, at ? (k < 0 ? " - %lld" : " + %lld") : "%lld", at ? (k < 0 ? -k : k) : k);
}
static void jau_print_measures(const JRank *rk, int32_t nrk) {
    // a loop or recursion the judge did not show to end: "no measure found", as the default mode
    // says it, once per header and per function's recursion
    for (int64_t q = 0; q < jau_n; q++) {
        const JauRec *x = &jau_r[q];
        if (x->k == JREC_PROVED || !x->fn) continue;
        bool rec = !strcmp(x->kind, "rec.fall"), loop = !strcmp(x->kind, "rank.fall") || !strcmp(x->kind, "measure.fall");
        if (!rec && !loop) continue;
        IrBlock *H = loop ? jau_header(x) : NULL;
        if (loop && !H) continue;
        bool seen = false;
        for (int64_t p2 = 0; p2 < q && !seen; p2++) {
            const JauRec *y = &jau_r[p2];
            if (y->fn != x->fn || y->k == JREC_PROVED || strcmp(y->kind, x->kind)) continue;
            seen = rec || (jau_header(y) == H);
        }
        if (seen) continue;
        isize line = 0, col = 0;
        jau_site(x->fn, x, loop, &line, &col);
        fprintf(stderr, "[measure] %.*s: %s at line %lld: no measure found\n", (int)x->fn->name->length, x->fn->name->name,
                rec ? "recursion" : "loop", (long long)line);
    }
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
        if (rec) {                                           // the first call that may close the cycle
            for (int64_t q = 0; q < jau_n && !line; q++)
                if (jau_r[q].fn == f && !strncmp(jau_r[q].kind, "rec.", 4)) { isize col = 0; jau_site(f, &jau_r[q], false, &line, &col); }
        } else {
            IrBlock *H = NULL;
            for (IrBlock *b = f->blocks; b && !H; b = b->next) if (b->id == r->blk) H = b;
            IrInstr *hc = H && H->term.cond ? jau_def(f, H->term.cond) : NULL;
            line = hc && hc->line ? hc->line : H ? H->term.line : 0;
        }
        char row[600], all[2000]; size_t at = 0; all[0] = 0;
        const IrName *fn = f->name;                          // as VRA's line names it, in both modes
        if (r->nreg == 0) {
            for (int32_t j = 0; j < r->ncomp; j++) {
                jau_row(f, r, j, row, sizeof row);
                at += (size_t)snprintf(all + at, at < sizeof all ? sizeof all - at : 0, "%s%s", j ? ", " : "", row);
            }
            // a function whose calls are listed (rec.le, spec 12's cut) only keeps its measure from
            // rising at them: what falls is every cycle, not every call
            if (rec) fprintf(stderr, "[measure] %.*s: recursion at line %lld: `%s%s%s` %s (the judge's)\n",
                             fn ? (int)fn->length : 1, fn ? fn->name : "?", (long long)line, r->ncomp > 1 ? "<" : "", all, r->ncomp > 1 ? ">" : "",
                             r->nle ? "does not rise at any call that may close the cycle, and every cycle of calls lowers it"
                                    : "decreases at every call that may close the cycle");
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
// The finder's accounts that ran out in the last run (finder.h's FD_WORK, one per function and one
// for the module): a refusal there may be the search's limit, and says so (spec 13: a refusal a
// budget caused names the budget, Documentation 2026-10-20)
static IrFunc *jau_out_fn[16]; static int32_t jau_nout_fn; static bool jau_out_more, jau_out_module;
static const char *jau_out_where(const IrFunc *f) {
    if (jau_out_module || jau_out_more) return "in this module";
    for (int32_t i = 0; i < jau_nout_fn; i++) if (jau_out_fn[i] == f) return "in this function";
    return NULL;
}
// Run the judge on `mod`. False with jau_internal set when it could not run, or reported a compiler
// bug: the caller refuses the program with exit 70, never emits it.
// PE.12: for each loop the judge refused, inputs the finder proposes (fd_witness, untrusted), each
// RUN (ir_interpret_witness) until one brings the header back to a state it was in; that one is
// E011's last note. The generator is offered again over the judge's region, which the judge is
// done with, with the claims the finder proposed. Only refused loops pay for it.
static void jau_witnesses(IrFunc *mod, JFinder *F, char *region, size_t cap, const JClaim *cl, int32_t ncl) {
    JGenOffer offer; memset(&offer, 0, sizeof offer);
    offer.buf = region; offer.cap = cap; offer.mod = mod;
    JudgeGen gen = jgen_offer(&offer);
    const JudgeGen *saved = F->gen; F->gen = &gen;
    for (int64_t j = 0; j < jau_n; j++) {
        const JauRec *q = &jau_r[j];
        if (q->k == JREC_PROVED || strncmp(q->kind, "rank.", 5)) continue;
        IrBlock *H = jau_header(q);
        if (!H || !q->fn || q->fn->is_extern) continue;
        bool done = false;
        for (int32_t w = 0; w < jau_nw && !done; w++) done = jau_w[w].fn == q->fn && jau_w[w].header == H->id;
        if (done) continue;
        IrFunc *f = q->fn;
        FdWitness cand[4];
        int32_t nc = fd_witness(F, f, H->id, cl, ncl, cand, 4);
        int np = 0;
        for (IrParam *p = f->params; p; p = p->next) np++;
        bool found = false;
        for (int32_t c0 = 0; c0 < nc && !found; c0++)
          for (int vk = 0; vk < 3 && !found; vk++) {
            FdWitness one;
            if (!fd_witness_variant(&cand[c0], f, vk, &one)) continue;
            FdWitness *cw = &one;
            uint64_t kv[FD_WMAXK]; uint8_t kg[FD_WMAXK];
            int r = ir_interpret_witness(f, mod, H->id, cw->val, cw->given, np < FD_WMAXP ? np : FD_WMAXP,
                                         2000000LL, 4096, cw->key, cw->nkey, kv, kg);
            if (r != IW_HANG) continue;
            found = true;
            JauWit *x = realloc(jau_w, sizeof *x * (size_t)(jau_nw + 1));
            if (!x) break;
            jau_w = x;
            JauWit *w = &jau_w[jau_nw++]; memset(w, 0, sizeof *w);
            w->fn = f; w->header = H->id;
            // The note, in Documentation's words (2026-10-25): the call as a user would type it, an
            // integer or bool argument by value, any other by its parameter's NAME and said zeroed;
            // no call for a function without parameters; the variables of the repeated state by their
            // source names; the period and the stem. No "checked by running it": it is printed only
            // on a repeated state, so it is a fact; the documentation says how it is known.
            char call[112] = "", zero[64] = "", st[112] = "";
            const IrName *fnm = f->src_name ? f->src_name : f->name;
            int k = 0, nz = 0, nzall = 0;
            for (IrParam *p = f->params; p; p = p->next) {
                const IrType *ty = p->value ? p->value->type : NULL;
                if (!(ty && (ty->kind == IRT_INT || ty->kind == IRT_BOOL))) nzall++;
            }
            if (f->params) jau_cat(call, sizeof call, " for %.*s(", (int)fnm->length, fnm->name);
            for (IrParam *p = f->params; p; p = p->next, k++) {
                const IrType *ty = p->value ? p->value->type : NULL;
                const IrName *pn = p->value ? p->value->src_name : NULL;
                __int128 x = k < FD_WMAXP ? cw->val[k] : 0;
                if (ty && ty->kind == IRT_BOOL) jau_cat(call, sizeof call, "%s%s", k ? ", " : "", x ? "true" : "false");
                else if (ty && ty->kind == IRT_INT && ty->is_signed) jau_cat(call, sizeof call, "%s%lld", k ? ", " : "", (long long)x);
                else if (ty && ty->kind == IRT_INT) jau_cat(call, sizeof call, "%s%llu", k ? ", " : "", (unsigned long long)x);
                else {
                    jau_cat(call, sizeof call, "%s%.*s", k ? ", " : "", pn ? (int)pn->length : 1, pn ? pn->name : "_");
                    jau_cat(zero, sizeof zero, "%s%.*s", nz == 0 ? ", with " : nz == nzall - 1 ? " and " : ", ",
                            pn ? (int)pn->length : 1, pn ? pn->name : "_");
                    nz++;
                }
            }
            if (f->params) jau_cat(call, sizeof call, ")");
            if (nz) jau_cat(zero, sizeof zero, " zeroed");
            int shown = 0, nshow = 0;
            for (int32_t q2 = 0; q2 < cw->nkey; q2++) {
                char nm[48]; jau_name(f, cw->key[q2], nm, sizeof nm);
                if (kg[q2] && nm[0] != '%' && nm[0] != '?') nshow++;
            }
            for (int32_t q2 = 0; q2 < cw->nkey; q2++) {
                if (!kg[q2]) continue;
                char nm[48]; jau_name(f, cw->key[q2], nm, sizeof nm);
                if (nm[0] == '%' || nm[0] == '?') continue;
                IrValue *kvv = jau_value_of(f, cw->key[q2]);
                const IrType *ct = kvv ? kvv->type : NULL;
                if (ct && ct->kind == IRT_PTR) ct = ct->elem;
                char val[32];
                if (ct && ct->kind == IRT_BOOL) snprintf(val, sizeof val, "%s", kv[q2] ? "true" : "false");
                else if (ct && ct->kind == IRT_INT && !ct->is_signed) snprintf(val, sizeof val, "%llu", (unsigned long long)kv[q2]);
                else snprintf(val, sizeof val, "%lld", (long long)(int64_t)kv[q2]);
                jau_cat(st, sizeof st, "%s%s = %s", shown == 0 ? "" : shown == nshow - 1 ? " and " : ", ", nm, val);
                shown++;
            }
            const char *period = cw->period == 1 ? "after every lap" : "every two laps";
            const char *stem = cw->stem == 1 ? ", from the second lap on" : "";
            isize ln = 0, cl0 = 0; jau_site(f, q, true, &ln, &cl0);
            char head[200];
            jau_put(head, sizeof head, "this loop never ends%s%s", call, zero);
            if (shown) jau_put(w->note, sizeof w->note, "%s: at line %lld, %s %s %s%s", head, (long long)ln, st,
                               shown == 1 ? "comes back" : "come back", period, stem);
            else jau_put(w->note, sizeof w->note, "%s: at line %lld, no lap changes anything%s", head, (long long)ln, stem);   // no variable to name (Documentation)
          }
    }
    F->gen = saved;
}
static bool judge_authority_run(IrFunc *mod) {
    free(jau_r); jau_r = NULL; jau_n = jau_cap = 0; jau_oom = false; jau_internal[0] = 0;
    free(jau_w); jau_w = NULL; jau_nw = 0;
    jau_nout_fn = 0; jau_out_more = jau_out_module = false;
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
    JshCtx jc = { work, wcap, &F, NULL, NULL, NULL, 0 };
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
    if (ran && jau_dump_measures) jau_print_measures(jc.last_rk, jc.last_rk ? F.nrk : 0);
    if (ran && !jau_internal[0]) jau_witnesses(mod, &F, buf, cap, jc.last, jc.last ? jc.nlast : 0);
    jau_nout_fn = F.nout_fn; jau_out_more = F.out_more; jau_out_module = F.out_module;
    for (int32_t i = 0; i < F.nout_fn; i++) jau_out_fn[i] = F.out_fn[i];
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
        if (loop && c.kind == VRA_TERMINATION && !c.recursion) {   // PE.12: its input that never ends
            IrBlock *H = jau_header(q);
            for (int32_t w = 0; H && w < jau_nw && !c.wit_input; w++)
                if (jau_w[w].fn == f && jau_w[w].header == H->id) c.wit_input = jau_w[w].note;
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
        else if (jau_out_where(f))
            fprintf(stderr, "       the prover's search for facts %s ran out of its budget, so this may be a limit "
                            "of the search, not of the program\n", jau_out_where(f));
    }
    free(it);
    return printed;
}

#endif // LAIN_ANALYSIS_JUDGE_AUTHORITY_H
