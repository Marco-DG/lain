// soundness_driver.c — the PER-OPERATION SOUNDNESS HARNESS (DECIDE-W v2, section 5.3).
//
// The range analysis (VRA) is an abstraction of one semantics, the IR's, and the interpreter
// (ir/interp.h) is that semantics, executable. So the analysis's soundness is a RELATION between
// two implementations, and it can be tested one operation at a time. Each trial builds a function
// around ONE IR operation, states random abstract inputs as assumptions (an interval per operand,
// sometimes a relation between the two), runs VRA, and then runs the interpreter on concrete
// inputs inside those assumptions (the corners of each interval and random points):
//
//   RANGE      the value the interpreter computes lies in the interval VRA gives the result;
//   RELATION   every difference bound VRA holds between operands and result (r - x <= c) holds;
//   UNREACHED  VRA did not call the result unreachable while the interpreter produced it;
//   OBLIGATION an obligation VRA DISCHARGED at the operation (no overflow, no division by zero, a
//              shift amount in range, a conversion that fits) holds: the interpreter, which checks
//              each one, does not stop there. A trap with no obligation at all is MISSING.
//
// One counterexample is an unsound transfer function. fuzz_vra.c set this yardstick for loop
// indices against a 17-case model of the IR; this one uses the real semantics, for every
// arithmetic, bitwise, shift, count, cast and comparison operation, each wrap mode, and every
// integer width. It also reports PRECISION COVERAGE (how many results got a range narrower than
// their type), because a harness over an analysis that says nothing finds nothing.
//
//   gcc -std=c99 -O2 -o soundness src/tools/soundness_driver.c -I src
//   ./soundness [seed] [trials] [--verbose]          exit 1 on any finding
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "analysis/vra.h"
#include "ir/emit_c.h"     // interp.h's C layout model reads the backend's storage order
#include "ir/interp.h"
#include "ir/dump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Arena A;
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static __int128 rnd_in(__int128 lo, __int128 hi) {                 // uniform in [lo, hi]
    unsigned __int128 span = (unsigned __int128)(hi - lo) + 1;
    unsigned __int128 r = ((unsigned __int128)rnd() << 64) | rnd();
    return lo + (__int128)(r % span);
}
static IrName *nm(const char *s) { return ir_intern(&A, s, (isize)strlen(s)); }
static const char *i128s(__int128 x) {
    static char buf[8][48]; static int k; char *p = buf[k++ & 7], t[48]; int n = 0, j = 0;
    unsigned __int128 u = x < 0 ? -(unsigned __int128)x : (unsigned __int128)x;
    do { t[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    if (x < 0) p[j++] = '-';
    while (n) p[j++] = t[--n];
    p[j] = 0; return p;
}

// Integer constants in the IR are int64, so a u64 interval stays below 2^63 (a u64 literal at or
// above 2^63 cannot be written in Lain at all yet: DECIDE-T). Results may still go above it.
static __int128 dom_max(IrType *t) { __int128 m = it_max(t); return m > INT64_MAX ? INT64_MAX : m; }

// An interval inside the type, in one of several shapes that reach different transfer cases.
static void pick_box(IrType *t, __int128 *lo, __int128 *hi) {
    __int128 mn = it_min(t), mx = dom_max(t), k = (__int128)(1 + rnd() % 40), a, b;
    switch (rnd() % 8) {
        case 0: *lo = *hi = rnd_in(mn, mx); return;                                 // a point
        case 1: *lo = mn > -k ? mn : -k; *hi = mx < k ? mx : k; return;            // around zero
        case 2: *hi = mx; *lo = mx - k > mn ? mx - k : mn; return;                 // at the top
        case 3: *lo = mn; *hi = mn + k < mx ? mn + k : mx; return;                 // at the bottom
        case 4: *lo = mn; *hi = mx; return;                                        // everything
        case 5: *lo = 0 > mn ? 0 : mn; *hi = k < mx ? k : mx; return;              // small, >= 0
        case 6: a = rnd_in(mn, mx); b = rnd_in(mn, mx);                            // anywhere
                *lo = a < b ? a : b; *hi = a < b ? b : a; return;
        default: {                                                                 // around 2^s
            int s = (int)(rnd() % (unsigned)(it_bits(t) + 1));
            __int128 p = (__int128)1 << s;
            a = p - k; b = p + k;
            *lo = a < mn ? mn : a > mx ? mx : a; *hi = b > mx ? mx : b < mn ? mn : b;
            if (*lo > *hi) { *lo = mn; *hi = mx; }
            return;
        }
    }
}

typedef enum { K_BIN, K_UN, K_CAST, K_CMP, K_GUARD } Kind;
// `modes`: how many of check, modular, saturate lowering can put on the operation. The harness
// builds only IR that lowering emits (lower.h's operator table and cast tiers): a combination it
// never produces, such as a saturating IR_ADD (`+|` is expanded into a widened add and a clamp),
// would test a transfer for a program that cannot exist.
typedef struct { const char *name; Kind k; IrOp op; int modes; } OpSpec;
static const OpSpec OPS[] = {
    {"add", K_BIN, IR_ADD, 2}, {"sub", K_BIN, IR_SUB, 2}, {"mul", K_BIN, IR_MUL, 2},  // +% -% *%
    {"div", K_BIN, IR_SDIV, 3}, {"rem", K_BIN, IR_SREM, 1},   // signed /% and /|; unsigned: check
    {"and", K_BIN, IR_AND, 1}, {"or", K_BIN, IR_OR, 1}, {"xor", K_BIN, IR_XOR, 1},
    {"shl", K_BIN, IR_SHL, 1}, {"shr", K_BIN, IR_ASHR, 1},
    {"neg", K_UN, IR_NEG, 1}, {"bnot", K_UN, IR_BNOT, 1},
    {"ctz", K_UN, IR_CTZ, 1}, {"clz", K_UN, IR_CLZ, 1}, {"popcount", K_UN, IR_POPCOUNT, 1},
    {"cast", K_CAST, IR_CAST, 3}, {"cmp", K_CMP, IR_ICMP, 1}, {"guard", K_GUARD, IR_ICMP, 1},  // as as% as|
};
#define NOPS ((int)(sizeof OPS / sizeof OPS[0]))
static const char *WRAPS[] = {"check", "modular", "saturate"};
static const char *CMPS[] = {"==", "!=", "<s", "<=s", ">s", ">=s", "<u", "<=u", ">u", ">=u"};

static bool cmp_holds(IrCmp c, __int128 x, __int128 y) {   // signed predicates on signed types,
    switch (c) {                                              // unsigned on unsigned: both exact
        case IR_CMP_EQ: return x == y; case IR_CMP_NE: return x != y;
        case IR_CMP_SLT: case IR_CMP_ULT: return x < y; case IR_CMP_SLE: case IR_CMP_ULE: return x <= y;
        case IR_CMP_SGT: case IR_CMP_UGT: return x > y; default: return x >= y;
    }
}

typedef struct {
    long trials, samples, traps, infeasible, nontrivial, results;
    long f_range, f_rel, f_unreached, f_oblig, f_missing, f_harness;
    long per_op_findings[32];
} Stats;
static Stats st;
static bool verbose = false, dump_next = false;

typedef struct {
    const OpSpec *o; IrType *tx, *ty, *rt, *land; IrWrapMode wrap; IrCmp rel; bool has_rel;
    __int128 xlo, xhi, ylo, yhi; IrCmp pred;
} Case;

static void describe(const Case *c, FILE *o) {
    fprintf(o, "%s/%s %s%d", c->o->name, WRAPS[c->wrap], c->tx->is_signed ? "i" : "u", c->tx->bits);
    if (c->o->k == K_CAST) fprintf(o, "->%s%d", c->rt->is_signed ? "i" : "u", c->rt->bits);
    if (c->o->k == K_CMP || c->o->k == K_GUARD) fprintf(o, " pred %s", CMPS[c->pred]);
    fprintf(o, "  x in [%s, %s]", i128s(c->xlo), i128s(c->xhi));
    if (c->ty) fprintf(o, "  y in [%s, %s]", i128s(c->ylo), i128s(c->yhi));
    if (c->has_rel) fprintf(o, "  assume x %s y", CMPS[c->rel]);
    if (c->land) fprintf(o, "  returned as %s%d", c->land->is_signed ? "i" : "u", c->land->bits);
}

static void finding(const Case *c, const char *what, long *counter, const char *fmt, __int128 a, __int128 b,
                    __int128 lo, __int128 hi) {
    (*counter)++;
    for (int k = 0; k < NOPS; k++) if (&OPS[k] == c->o) st.per_op_findings[k]++;
    printf("FINDING %-11s ", what); describe(c, stdout);
    printf("\n        x=%s", i128s(a)); if (c->ty) printf(" y=%s", i128s(b));
    printf("  %s [%s, %s]\n", fmt, i128s(lo), i128s(hi));
}

static IrCmp pick_pred(IrType *t) {
    int k = (int)(rnd() % 6);                                 // ==, !=, <, <=, >, >=
    if (k < 2) return (IrCmp)k;
    return (IrCmp)((t->is_signed ? IR_CMP_SLT : IR_CMP_ULT) + (k - 2));
}

static IrType *pick_int(void) {
    static const int W[] = {8, 16, 32, 64};
    return ir_type_int(&A, W[rnd() % 4], (rnd() & 1) != 0);
}

// The interval VRA gives value v in state W, as exact integers; an end at int64's limit is open
// (the domain stores int64, so it cannot say more for a u64 above 2^63).
static void range_of(Vra *V, Octagon *W, IrValue *v, __int128 *lo, __int128 *hi) {
    int64_t l = INT64_MIN, h = INT64_MAX;
    vra_range(V, W, v, &l, &h);
    *lo = l == INT64_MIN ? it_min(v->type) : l;
    *hi = h == INT64_MAX ? it_max(v->type) : h;
    if (*lo < it_min(v->type)) *lo = it_min(v->type);
    if (*hi > it_max(v->type)) *hi = it_max(v->type);
}

static void trial(void) {
    Case c; memset(&c, 0, sizeof c);
    c.o = &OPS[rnd() % NOPS];
    c.tx = pick_int();
    c.wrap = (IrWrapMode)(rnd() % (uint64_t)c.o->modes);
    IrOp op = c.o->op;
    if (op == IR_SDIV && !c.tx->is_signed) c.wrap = IR_WRAP_CHECK;   // unsigned `/%` is the plain division
    if (op == IR_SDIV && !c.tx->is_signed) op = IR_UDIV;
    if (op == IR_SREM && !c.tx->is_signed) op = IR_UREM;
    if (op == IR_ASHR && !c.tx->is_signed) op = IR_LSHR;
    bool two = c.o->k == K_BIN || c.o->k == K_CMP || c.o->k == K_GUARD;
    if (two) c.ty = c.tx;
    c.rt = c.o->k == K_CAST ? pick_int() : (c.o->k == K_CMP ? ir_type_bool(&A) : c.tx);
    if (c.o->k == K_CAST && rnd() % 3 == 0)                    // same width, other signedness: the
        c.rt = ir_type_int(&A, c.tx->bits, !c.tx->is_signed);  // cast that preserves no value above 2^(w-1)
    pick_box(c.tx, &c.xlo, &c.xhi);
    if (two) pick_box(c.ty, &c.ylo, &c.yhi);
    if (c.o->op == IR_SHL || c.o->op == IR_ASHR) {           // amounts: mostly in range, sometimes not
        if (rnd() % 3) { c.ylo = rnd_in(0, it_bits(c.tx) - 1); c.yhi = rnd_in(c.ylo, it_bits(c.tx) - 1); }
    }
    if (c.o->k == K_CMP || c.o->k == K_GUARD) c.pred = pick_pred(c.tx);
    if (c.o->k == K_BIN && rnd() % 3 == 0) { c.has_rel = true; c.rel = pick_pred(c.tx); }
    // LANDING: a quarter of the results are returned through another integer type, a narrowing
    // the analysis owes at the return (vra_check_narrow) and the interpreter checks there.
    if ((c.o->k == K_BIN || c.o->k == K_UN || c.o->k == K_CAST) && rnd() % 4 == 0) c.land = pick_int();
    IrType *bitcount_t = ir_type_int(&A, 32, false);
    IrType *fret = c.land ? c.land : c.o->k == K_GUARD ? c.tx
                 : (c.o->k == K_UN && op != IR_NEG && op != IR_BNOT) ? bitcount_t : c.rt;

    // the function: assumptions, the operation, a block that observes everything
    IrFunc *f = ir_func_new(&A, nm("t"), fret, IR_FUNC_PURE);
    IrValue *x = ir_add_param(f, c.tx, nm("x")), *y = two ? ir_add_param(f, c.ty, nm("y")) : NULL;
    IrBlock *e = f->entry, *obs = ir_new_block(f);
    IrCmp ge = c.tx->is_signed ? IR_CMP_SGE : IR_CMP_UGE, le = c.tx->is_signed ? IR_CMP_SLE : IR_CMP_ULE;
    ir_assume(f, e, ir_icmp(f, e, ge, x, ir_const_int(f, e, (int64_t)c.xlo, c.tx)));
    ir_assume(f, e, ir_icmp(f, e, le, x, ir_const_int(f, e, (int64_t)c.xhi, c.tx)));
    if (y) {
        ir_assume(f, e, ir_icmp(f, e, ge, y, ir_const_int(f, e, (int64_t)c.ylo, c.ty)));
        ir_assume(f, e, ir_icmp(f, e, le, y, ir_const_int(f, e, (int64_t)c.yhi, c.ty)));
    }
    if (c.has_rel) ir_assume(f, e, ir_icmp(f, e, c.rel, x, y));
    IrValue *r = NULL; IrInstr *opins = NULL;
    switch (c.o->k) {
        case K_BIN: r = ir_binop(f, e, op, x, y, c.rt); break;
        case K_UN:
            if (op == IR_NEG || op == IR_BNOT) { IrInstr *u = ir_instr(f, op, c.rt, 1); u->operands[0] = x; ir_emit(e, u); r = u->result; }
            else r = ir_bitcount(f, e, op, x, bitcount_t);   // a bit count is a u32
            break;
        case K_CAST: {
            IrInstr *cv = ir_instr(f, IR_CAST, c.rt, 1); cv->operands[0] = x;
            cv->aux.cast_kind = c.tx->bits > c.rt->bits ? IR_CAST_TRUNC : c.tx->bits < c.rt->bits
                              ? (c.tx->is_signed ? IR_CAST_SEXT : IR_CAST_ZEXT) : IR_CAST_BITCAST;   // as lowering
            ir_emit(e, cv); r = cv->result; break;
        }
        case K_CMP: r = ir_icmp(f, e, c.pred, x, y); break;
        case K_GUARD: ir_assume(f, e, ir_icmp(f, e, c.pred, x, y)); break;
    }
    opins = e->instrs_tail;
    if (c.o->k == K_UN && op != IR_NEG && op != IR_BNOT) c.rt = r->type;
    opins->wrap = c.wrap; opins->line = 100; opins->col = 1;
    ir_set_br(e, obs);
    ir_set_ret(obs, c.o->k == K_GUARD ? x : r);
    ir_finalize_cfg(f);

    // the abstract side
    vra_mod = f;
    Vra *V = vra_analyze(f);
    long found_before = st.f_range + st.f_rel + st.f_unreached + st.f_oblig + st.f_missing + st.f_harness;
    bool any_ovf = false, ovf_open = false, any_div = false, div_open = false;
    for (int i = 0; i < V->nchecks; i++) {
        if (V->checks[i].at != opins) continue;
        if (V->checks[i].kind == VRA_OVERFLOW) { any_ovf = true; if (!V->checks[i].ok) ovf_open = true; }
        else { any_div = true; if (!V->checks[i].ok) div_open = true; }
    }
    bool reached = V->reached[obs->id];
    int dim = 2 * V->noct;
    int64_t *m = malloc((size_t)V->dsz * 8 + 8);
    Octagon W = { V->noct, dim, m };
    __int128 rlo = 0, rhi = 0, xlo = 0, xhi = 0, ylo = 0, yhi = 0;
    IrValue *vals[3] = { x, y, r }; int64_t dub[3][3];
    const int *map_saved = oct_map; oct_map = V->odim;          // queries outside vra_analyze map
    if (reached) {                                               // value ids to packed slots
        memcpy(m, V->in[obs->id], (size_t)V->dsz * 8);
        oct_close(&W);
        if (r) range_of(V, &W, r, &rlo, &rhi);
        range_of(V, &W, x, &xlo, &xhi);
        if (y) range_of(V, &W, y, &ylo, &yhi);
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++)
            dub[a][b] = (a != b && vals[a] && vals[b] && vals[a]->type && vals[b]->type
                         && vals[a]->type->kind == IRT_INT && vals[b]->type->kind == IRT_INT)
                        ? vra_diff_ub(V, &W, vals[a]->id, vals[b]->id) : OCT_INF;
        if (r) { st.results++; if (rlo > it_min(r->type) || rhi < it_max(r->type)) st.nontrivial++; }
    }
    oct_map = map_saved;

    // the concrete side: corners and random points inside the assumptions
    __int128 cx[12], cy[12]; int ncx = 0, ncy = 0;
    __int128 xs[] = { c.xlo, c.xhi, c.xlo + 1, c.xhi - 1, 0, -1, 1, c.xlo + (c.xhi - c.xlo) / 2 };
    for (int k = 0; k < 8; k++) if (xs[k] >= c.xlo && xs[k] <= c.xhi) cx[ncx++] = xs[k];
    if (y) {
        __int128 ys[] = { c.ylo, c.yhi, c.ylo + 1, c.yhi - 1, 0, -1, 1, c.ylo + (c.yhi - c.ylo) / 2 };
        for (int k = 0; k < 8; k++) if (ys[k] >= c.ylo && ys[k] <= c.yhi) cy[ncy++] = ys[k];
    } else cy[ncy++] = 0;
    int ran = 0;
    for (int s = 0; s < ncx * ncy + 48; s++) {
        __int128 vx = s < ncx * ncy ? cx[s / ncy] : rnd_in(c.xlo, c.xhi);
        __int128 vy = !y ? 0 : s < ncx * ncy ? cy[s % ncy] : rnd_in(c.ylo, c.yhi);
        if (c.has_rel && !cmp_holds(c.rel, vx, vy)) continue;
        if (c.o->k == K_GUARD && !cmp_holds(c.pred, vx, vy)) continue;
        IVal args[2]; iv_int(&args[0], vx); iv_int(&args[1], vy);
        IVal out; memset(&out, 0, sizeof out); long long used = 0;
        ii_quiet = true;
        int status = ir_interpret_call(f, f, "soundness", args, y ? 2 : 1, &out, 100000, &used);
        ii_quiet = false;
        st.samples++; ran++;
        if (status == 99) {
            st.traps++;
            bool div = strstr(ii_why, "by zero") != NULL;
            bool ovf = strstr(ii_why, "overflows") || strstr(ii_why, "shift by") || strstr(ii_why, "conversion of")
                    || strstr(ii_why, "lands in");
            if (!div && !ovf) {
                if (strstr(ii_why, "of zero (undefined")) div = true;      // @ctz/@clz of zero
                else { finding(&c, "HARNESS", &st.f_harness, ii_why, vx, vy, 0, 0); continue; }
            }
            bool any = div ? any_div : any_ovf, open = div ? div_open : ovf_open;
            if (!any) finding(&c, "MISSING", &st.f_missing, ii_why, vx, vy, 0, 0);
            else if (!open) finding(&c, "OBLIGATION", &st.f_oblig, ii_why, vx, vy, 0, 0);
            continue;
        }
        if (status != 0) { finding(&c, "HARNESS", &st.f_harness, ii_why, vx, vy, status, status); continue; }
        if (!reached) { finding(&c, "UNREACHED", &st.f_unreached, "VRA: the result is never produced", vx, vy, 0, 0); continue; }
        __int128 vr = 0;
        if (r) {
            vr = it_wrap_bits((__int128)out.i, it_bits(r->type), it_signed(r->type));
            if (vr < rlo || vr > rhi) {
                finding(&c, "RANGE", &st.f_range, "result outside VRA's", vx, vy, rlo, rhi);
                printf("        result=%s\n", i128s(vr));
                continue;
            }
        }
        if (c.o->k == K_GUARD) {
            if (vx < xlo || vx > xhi) { finding(&c, "RANGE", &st.f_range, "x after the guard outside VRA's", vx, vy, xlo, xhi); continue; }
            if (vy < ylo || vy > yhi) { finding(&c, "RANGE", &st.f_range, "y after the guard outside VRA's", vx, vy, ylo, yhi); continue; }
        }
        __int128 cv[3] = { vx, vy, vr };
        for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
            if (dub[a][b] >= OCT_INF / 2 || !vals[a] || !vals[b]) continue;
            if (cv[a] - cv[b] > (__int128)dub[a][b]) {
                static const char *N[3] = {"x", "y", "r"};
                char what[64]; snprintf(what, sizeof what, "%s - %s = %s exceeds VRA's bound", N[a], N[b], i128s(cv[a] - cv[b]));
                finding(&c, "RELATION", &st.f_rel, what, vx, vy, dub[a][b], dub[a][b]);
            }
        }
    }
    if (!ran) st.infeasible++;
    if (dump_next && st.f_range + st.f_rel + st.f_unreached + st.f_oblig + st.f_missing + st.f_harness > found_before) {
        ir_dump_func(f, stdout); fflush(stdout);                  // the first trial that found something
        oct_map = V->odim; vra_dump_state(V, stdout); oct_map = map_saved; dump_next = false;
    }
    free(m);
    vra_free(V);
    st.trials++;
}

int main(int argc, char **argv) {
    long seed = argc > 1 ? atol(argv[1]) : 1, trials = argc > 2 ? atol(argv[2]) : 2000;
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--verbose")) verbose = true;
    for (int k = 1; k < argc; k++) if (!strcmp(argv[k], "--dump")) dump_next = true;   // the first trial with a finding
    rs ^= (uint64_t)seed * 0x9E3779B97F4A7C15ULL; if (!rs) rs = 1;
    for (int k = 0; k < 8; k++) rnd();
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 4096);
    for (long t = 0; t < trials; t++) { trial(); if (t % 64 == 63) arena_clear(&A); }
    long bugs = st.f_range + st.f_rel + st.f_unreached + st.f_oblig + st.f_missing + st.f_harness;
    printf("soundness: seed %ld, %ld trials, %ld samples (%ld trapped), %ld trials with no feasible sample\n",
           seed, st.trials, st.samples, st.traps, st.infeasible);
    printf("precision coverage: %ld of %ld results got a range narrower than their type\n", st.nontrivial, st.results);
    printf("findings by operation:");
    for (int k = 0; k < NOPS; k++) if (st.per_op_findings[k]) printf(" %s=%ld", OPS[k].name, st.per_op_findings[k]);
    printf("\nbugs: %ld (RANGE=%ld RELATION=%ld UNREACHED=%ld OBLIGATION=%ld MISSING=%ld HARNESS=%ld)\n",
           bugs, st.f_range, st.f_rel, st.f_unreached, st.f_oblig, st.f_missing, st.f_harness);
    return bugs ? 1 : 0;
}
