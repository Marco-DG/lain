// src/tools/test_judge_gen.c — unit test of the judge's VC generator (src/judge/gen.h) over a
// HAND-BUILT IR. The generator is trusted, and its main test runs every accepted program on the
// interpreter and evaluates each hypothesis at the values the program has (judge_vc_driver.c
// --check-hyps). That test cannot see a rule no program exercises: since I.182 lowering emits no
// unsigned comparison with a signed operand, and an ASSUME stated before the point it follows
// holds at that point in every accepted program. Each rule here is pinned by the IR that makes
// it matter, and every VC is checked to be in the canonical text form (it parses and prints back).
// Claims at forward joins (the hull row) are pinned the same way: where the claim is stated, the
// obligation it owes on each edge, and the claims the generator refuses.
//
// Build & run (run_ir_tests.sh does this):
//   gcc -std=c99 -o /tmp/test_judge_gen src/tools/test_judge_gen.c -I src && /tmp/test_judge_gen
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include <string.h>
#include "judge/gen.h"
#include "judge/text.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Arena A;
static char region[1 << 24], parse_region[1 << 24];
static IrName *nm(const char *s) { return ir_intern(&A, s, (isize)strlen(s)); }
static IrType *u64t, *i64t, *i32t, *u8t, *u32t, *boolt;
static IrType *slice_of(IrType *e) { IrType *t = ir_type_new(&A, IRT_SLICE); t->elem = e; return t; }

// Every VC of `f` as text, each checked to round-trip; NULL when the generator refuses the
// function (its reason in `why` when given, else printed). `f`'s CFG must be finalized (once:
// ir_finalize_cfg adds the edges again on each call).
static IrFunc *gen_mod = NULL;   // the module the generator looks callees up in (none: every call unknown)
static const JRank *gen_rk = NULL; static int32_t gen_nrk = 0;   // the ranking claims gen_with owes
// Every obligation's ID of `f`, one line each, "point kind ordinal" and " outside" for one the
// generator cannot state (gen_with prints an outside by its reason alone); NULL when refused.
static char *gen_ids(IrFunc *f, char *why) {
    JMem m = jmem_over(region, sizeof region);
    JObl *o; int32_t n; char err[200];
    if (!jgen_func(&m, f, gen_mod, NULL, 0, gen_rk, gen_nrk, NULL, JGEN_ALL, &o, &n, err, sizeof err)) { strcpy(why, err); return NULL; }
    size_t cap = 64 * (size_t)(n + 1), len = 0; char *t = malloc(cap); t[0] = 0;
    for (int32_t i = 0; i < n; i++)
        len += (size_t)snprintf(t + len, cap - len, "%s %s %s%s\n", o[i].vc.id.point, o[i].vc.id.kind, o[i].vc.id.ordinal, o[i].outside ? " outside" : "");
    return t;
}
// Every MAX or MIN variable's recipe (c41), one line each: "point kind ordinal var mm M:" then each
// form as " {c0 coef*var ...}"; NULL when refused.
static char *gen_recipes(IrFunc *f, char *why) {
    JMem m = jmem_over(region, sizeof region);
    JObl *o; int32_t n; char err[200];
    if (!jgen_func(&m, f, gen_mod, NULL, 0, gen_rk, gen_nrk, NULL, JGEN_ALL, &o, &n, err, sizeof err)) { strcpy(why, err); return NULL; }
    size_t cap = 1 << 16, len = 0; char *t = malloc(cap); t[0] = 0;
    for (int32_t i = 0; i < n; i++)
        for (int32_t x = 0; !o[i].outside && x < o[i].vc.nvars; x++) {
            const JRecipe *r = o[i].vrec ? o[i].vrec[x] : NULL;
            if (!r || !r->mm || len + 512 > cap) continue;
            len += (size_t)snprintf(t + len, cap - len, "%s %s %s %s mm %d:", o[i].vc.id.point, o[i].vc.id.kind, o[i].vc.id.ordinal, o[i].vc.vars[x], r->mm);
            for (int32_t k = 0; r->mmf && k < r->mmf->nf; k++) {
                len += (size_t)snprintf(t + len, cap - len, " {%lld", (long long)r->mmf->fc0[k]);
                for (int q = 0; q < r->mmf->fn[k]; q++)
                    len += (size_t)snprintf(t + len, cap - len, " %lld*%s", (long long)r->mmf->fcoef[k][q], o[i].vc.vars[r->mmf->fvar[k][q]]);
                len += (size_t)snprintf(t + len, cap - len, "}");
            }
            len += (size_t)snprintf(t + len, cap - len, "\n");
        }
    return t;
}
static char *gen_with(IrFunc *f, const JClaim *cl, int32_t ncl, int32_t only, char *why) {
    JMem m = jmem_over(region, sizeof region);
    JObl *o; int32_t n; char err[200];
    if (!jgen_func(&m, f, gen_mod, cl, ncl, gen_rk, gen_nrk, NULL, only, &o, &n, err, sizeof err)) {
        if (why) strcpy(why, err); else printf("  generator refused: %s\n", err);
        return NULL;
    }
    FILE *t = tmpfile();
    for (int32_t i = 0; i < n; i++) {
        if (o[i].outside) { fprintf(t, "outside %s\n", o[i].outside); continue; }
        FILE *one = tmpfile(); jvc_print(&o[i].vc, one);
        long len = ftell(one); rewind(one);
        char *s = malloc((size_t)len + 1); size_t got = fread(s, 1, (size_t)len, one); s[got] = 0; fclose(one);
        JMem pm = jmem_over(parse_region, sizeof parse_region);
        JParse ps = jparse_over(&pm, s); JVc back;
        bool ok = jvc_parse(&ps, &back) && *ps.p == 0;
        CHECK(ok, "a VC the generator wrote is not canonical: %s\n%s", ps.err, s);
        if (ok) {
            FILE *two = tmpfile(); jvc_print(&back, two);
            long l2 = ftell(two); rewind(two);
            char *s2 = malloc((size_t)l2 + 1); size_t g2 = fread(s2, 1, (size_t)l2, two); s2[g2] = 0; fclose(two);
            CHECK(strcmp(s, s2) == 0, "a VC does not print back as it parsed:\n%s", s);
            free(s2);
        }
        fputs(s, t); free(s);
    }
    long len = ftell(t); rewind(t);
    char *s = malloc((size_t)len + 1); size_t got = fread(s, 1, (size_t)len, t); s[got] = 0; fclose(t);
    return s;
}
static char *gen_text(IrFunc *f) { ir_finalize_cfg(f); return gen_with(f, NULL, 0, JGEN_ALL, NULL); }
static IrInstr *g_last_instr(IrBlock *b) { IrInstr *l = b->instrs; while (l && l->next) l = l->next; return l; }
// Is a version named by `prefix` (".../bbB." and then its index) stated EQUAL to its stored value,
// " :" right after its name? A waived landing's wrap relation reads "... wrap :" instead.
static bool plain_store(const char *vc, const char *prefix) {
    for (const char *p = vc; (p = strstr(p, prefix)); p++) {
        const char *q = p + strlen(prefix);
        while (*q && *q != ' ') q++;
        if (!strncmp(q, " :", 2)) return true;
    }
    return false;
}
static int nins(IrBlock *b) { int n = 0; for (IrInstr *i = b->instrs; i; i = i->next) n++; return n; }
static int count(const char *all, const char *key) { int n = 0; for (const char *p = all; (p = strstr(p, key)); p++) n++; return n; }
// Some line of `t` is `def %vP mul.mono : or { L | R }` with L the terms -s*a, s*b and R s*p, c, -s*q
// (s = 1: a <= b - 1 -> p <= q - c; s = -1 mirrored), each a whole token
static bool mono_has(const char *t, int p, int a, int b, int c, int q, int sg) {
    char key[48]; snprintf(key, sizeof key, "def %%v%d mul.mono : or { ", p);
    for (const char *l = t ? strstr(t, key) : NULL; l; l = strstr(l + 1, key)) {
        const char *e = strchr(l, '\n'), *bar = strstr(l, " | ");
        if (!e || !bar || bar > e) continue;
        char L[200], R[200], w[5][24];
        snprintf(L, sizeof L, " %.*s ", (int)(bar - l - strlen(key)), l + strlen(key));
        snprintf(R, sizeof R, " %.*s ", (int)(e - bar - 3), bar + 3);
        snprintf(w[0], 24, " %d*%%v%d ", -sg, a); snprintf(w[1], 24, " %d*%%v%d ", sg, b);
        snprintf(w[2], 24, " %d*%%v%d ", sg, p); snprintf(w[3], 24, " 1*%%v%d ", c); snprintf(w[4], 24, " %d*%%v%d ", -sg, q);
        if (strstr(L, w[0]) && strstr(L, w[1]) && strstr(R, w[2]) && strstr(R, w[3]) && strstr(R, w[4]) && strstr(L, " <= 0 ") && strstr(R, " <= 0 }")) return true;
    }
    return false;
}
static IrValue *mk_call(IrFunc *f, IrBlock *b, const char *callee, IrValue *arg, IrType *rt) {
    IrInstr *c = ir_instr(f, IR_CALL, rt, 1);
    c->aux.callee = nm(callee); c->operands[0] = arg;
    ir_emit(b, c);
    return c->result;
}
static int idx_of(IrBlock *b, IrValue *v) { int n = 0; for (IrInstr *i = b->instrs; i; i = i->next, n++) if (i->result == v) return n; return -1; }
static JClaim claim1(IrBlock *b, IrValue *cell, int coef, long long rhs) {
    JClaim c; memset(&c, 0, sizeof c);
    c.blk = b->id; c.nt = 1; c.key[0] = cell->id; c.coef[0] = coef; c.rhs = rhs;
    return c;
}
// The text of the first VC whose header line contains `key` (e.g. "bounds.hi"), or "".
static __int128 jint128(const JInt *x) {                 // a VC's constants are below 2^66
    unsigned __int128 m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(__int128)m : (__int128)m;
}
static const char *vc_of(const char *all, const char *key) {
    static char buf[1 << 16];
    const char *p = all;
    while ((p = strstr(p, "vc ")) != NULL) {
        const char *eol = strchr(p, '\n'), *end = strstr(p, "\nend\n");
        if (!eol || !end) break;
        bool hit = false;
        for (const char *q = p; q + strlen(key) <= eol && !hit; q++) hit = !strncmp(q, key, strlen(key));
        if (hit) {
            size_t n = (size_t)(end - p) + 5; if (n >= sizeof buf) n = sizeof buf - 1;
            memcpy(buf, p, n); buf[n] = 0; return buf;
        }
        p = end + 5;
    }
    return "";
}

int main(void) {
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 512);
    u64t = ir_type_int(&A, 64, false); i64t = ir_type_int(&A, 64, true); i32t = ir_type_int(&A, 32, true);
    u8t = ir_type_int(&A, 8, false); u32t = ir_type_int(&A, 32, false); boolt = ir_type_bool(&A);

    // 1. a guard on the edge into a single-predecessor block, over a slice's length
    {   IrFunc *f = ir_func_new(&A, nm("g1"), u8t, IR_FUNC_PURE);
        IrValue *a = ir_add_param(f, slice_of(u8t), nm("a")), *i = ir_add_param(f, u64t, nm("i"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        IrValue *len = ir_slice_len(f, b0, a);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, i, len), b1, b2);
        IrValue *d = ir_slice_data(f, b1, a, u8t);
        ir_set_ret(b1, ir_load(f, b1, ir_elem_ptr(f, b1, d, i, u8t), u8t));
        ir_set_ret(b2, ir_const_int(f, b2, 0, u8t));
        char *t = gen_text(f);
        const char *hi = t ? vc_of(t, "bounds.hi") : "";
        CHECK(strstr(hi, "guard bb0->bb1 : 1*%v1 + -1*%v2 <= -1"), "the guard i < a.len is a hypothesis of a[i]'s upper bound:\n%s", hi);
        CHECK(strstr(hi, "def %v2 slice_len : -1*len(%v0) + 1*%v2 = 0"), "a.len is the slice's length:\n%s", hi);
        CHECK(strstr(hi, "goal 1*%v1 + -1*len(%v0) <= -1"), "the goal is i < len(a):\n%s", hi);
        free(t);
        printf("  a guard into a single-predecessor block, and a slice's length\n");
    }
    // 2 and 3. an unsigned comparison: stated when its signed operand is a non-negative constant,
    // and split on its sign when it is a variable that may be negative (it compares as uint64)
    for (int k = 0; k < 2; k++) {
        IrFunc *f = ir_func_new(&A, nm(k ? "g3" : "g2"), u64t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i64t, nm("x")), *y = ir_add_param(f, u64t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        IrValue *lhs = k ? ir_const_int(f, b0, 3, i32t) : x;
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, lhs, y), b1, b2);
        ir_set_ret(b1, ir_binop(f, b1, IR_UDIV, ir_const_int(f, b1, 100, u64t), y, u64t));
        ir_set_ret(b2, ir_const_int(f, b2, 0, u64t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        if (k == 0) CHECK(strstr(dz, "guard bb0->bb1 : or {") && strstr(dz, "<= -18446744073709551617 }"),
                          "x <u y with x an i64 that may be negative: split on its sign, x + 2^64 < y below 0 (test 41):\n%s", dz);
        else        CHECK(strstr(dz, "guard bb0->bb1"), "3 <u y with 3 a non-negative constant is stated:\n%s", dz);
        free(t);
    }
    printf("  an unsigned comparison: a signed variable split on its sign, a non-negative constant stated\n");
    // 4. an ASSUME holds at the points after it, never before
    for (int k = 0; k < 2; k++) {
        IrFunc *f = ir_func_new(&A, nm(k ? "g5" : "g4"), i32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i32t, nm("x"));
        IrBlock *b0 = f->entry;
        IrValue *c = ir_icmp(f, b0, IR_CMP_SGT, x, ir_const_int(f, b0, 0, i32t));
        if (k) ir_assume(f, b0, c);
        IrValue *q = ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 100, i32t), x, i32t);
        if (!k) ir_assume(f, b0, c);
        ir_set_ret(b0, q);
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        if (k) CHECK(strstr(dz, "assume bb0."), "an ASSUME before the division is its hypothesis:\n%s", dz);
        else   CHECK(!strstr(dz, "assume"), "an ASSUME after the division is not:\n%s", dz);
        free(t);
    }
    printf("  an ASSUME before the point is a hypothesis, one after it is not\n");
    // 5. a join of two different stores is a fresh version, not either one
    {   IrFunc *f = ir_func_new(&A, nm("g6"), u32t, IR_FUNC_PURE);
        IrValue *p = ir_add_param(f, boolt, nm("p"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *s = ir_alloca(f, b0, u32t);
        ir_set_br_cond(b0, p, b1, b2);
        ir_store(f, b1, s, ir_const_int(f, b1, 1, u32t)); ir_set_br(b1, b3);
        ir_store(f, b2, s, ir_const_int(f, b2, 2, u32t)); ir_set_br(b2, b3);
        ir_set_ret(b3, ir_binop(f, b3, IR_UDIV, ir_const_int(f, b3, 10, u32t), ir_load(f, b3, s, u32t), u32t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        CHECK(strstr(dz, "@join.bb3") && !strstr(dz, "store %s"), "the load after the join reads a join version:\n%s", dz);
        free(t);
        printf("  a join of two stores is a fresh version\n");
    }
    // 6. the loop cut: a cell the loop writes gets a header version; one it does not write keeps
    // the version it entered with (D1 A2)
    {   IrFunc *f = ir_func_new(&A, nm("g7"), u64t, IR_FUNC_PURE);
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *kc = ir_alloca(f, b0, u64t), *ic = ir_alloca(f, b0, u64t);
        ir_store(f, b0, kc, ir_const_int(f, b0, 5, u64t));
        ir_store(f, b0, ic, ir_const_int(f, b0, 0, u64t));
        ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, ic, u64t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, li, ir_const_int(f, b1, 10, u64t)), b2, b3);
        IrValue *q = ir_binop(f, b2, IR_UDIV, ir_const_int(f, b2, 100, u64t), ir_load(f, b2, kc, u64t), u64t);
        (void)q;
        IrValue *li2 = ir_load(f, b2, ic, u64t);
        ir_store(f, b2, ic, ir_binop(f, b2, IR_ADD, li2, ir_const_int(f, b2, 1, u64t), u64t));
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_load(f, b3, kc, u64t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        char want[64]; snprintf(want, sizeof want, "%%s%d@bb0.", kc->id);
        CHECK(strstr(dz, want) && !strstr(dz, "@head"), "k, which the loop never writes, keeps its store version:\n%s", dz);
        const char *ov = t ? vc_of(t, "narrow.hi") : "";
        snprintf(want, sizeof want, "%%s%d@head.bb1", ic->id);
        CHECK(strstr(ov, want), "i, which the loop writes, has a header version:\n%s", ov);
        free(t);
        printf("  the loop cut: a written cell is cut at the header, an unwritten one is not\n");
    }
    // 7. a cast to bool is 1 for any nonzero value, so it is not an equality
    {   IrFunc *f = ir_func_new(&A, nm("g8"), u32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i32t, nm("x"));
        IrBlock *b0 = f->entry;
        IrValue *b = ir_binop(f, b0, IR_ADD, x, x, i32t); (void)b;
        IrInstr *cb = NULL;
        IrValue *bv = ir_new_value(f, boolt);
        { IrInstr *ins = arena_push_aligned(&A, IrInstr); memset(ins, 0, sizeof *ins);
          ins->op = IR_CAST; ins->result = bv; ins->operands = arena_push_many(&A, IrValue *, 1); ins->operands[0] = x;
          ins->n_operands = 1; ins->wrap = IR_WRAP_CHECK; ir_emit(b0, ins); cb = ins; }
        IrValue *w = ir_new_value(f, u32t);
        { IrInstr *ins = arena_push_aligned(&A, IrInstr); memset(ins, 0, sizeof *ins);
          ins->op = IR_CAST; ins->result = w; ins->operands = arena_push_many(&A, IrValue *, 1); ins->operands[0] = bv;
          ins->n_operands = 1; ins->wrap = IR_WRAP_CHECK; ir_emit(b0, ins); }
        (void)cb;
        ir_set_ret(b0, ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 10, u32t), w, u32t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        char want[64]; snprintf(want, sizeof want, "def %%v%d cast", bv->id);
        CHECK(!strstr(dz, want), "the cast of x to bool states no equality:\n%s", dz);
        snprintf(want, sizeof want, "def %%v%d cast", w->id);
        CHECK(strstr(dz, want), "the cast of that bool to u32 does:\n%s", dz);
        free(t);
        printf("  a cast to bool is not an equality, a cast from bool is\n");
    }
    // 8. a store inside `unsafe` that narrows wraps, so its version is not the value stored: it is
    // that value wrapped into the slot (`... wrap`, x = v - k*2^8 within u8)
    for (int k = 0; k < 2; k++) {
        IrFunc *f = ir_func_new(&A, nm(k ? "g10" : "g9"), u8t, IR_FUNC_PURE);
        IrValue *v = ir_add_param(f, i32t, nm("v"));
        IrBlock *b0 = f->entry;
        IrValue *s = ir_alloca(f, b0, u8t);
        ir_store(f, b0, s, v);
        b0->instrs_tail->unchecked = k == 0;
        ir_set_ret(b0, ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 10, u8t), ir_load(f, b0, s, u8t), u8t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        if (k == 0) CHECK(!plain_store(dz, "store %s") && strstr(dz, " wrap : "),
                          "an unchecked narrowing store states no equality, but its wrap into u8:\n%s", dz);
        else        CHECK(strstr(dz, "store %s"), "a checked one does (its landing is an obligation):\n%s", dz);
        free(t);
    }
    printf("  a store that wraps in `unsafe` is not an equality, a checked one is\n");
    // 9. a u64 constant is read unsigned: -1's bits are 2^64 - 1
    {   IrFunc *f = ir_func_new(&A, nm("g11"), u64t, IR_FUNC_PURE);
        IrBlock *b0 = f->entry;
        IrValue *c = ir_const_int(f, b0, -1, u64t);
        ir_set_ret(b0, ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 10, u64t), c, u64t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        CHECK(strstr(dz, "= 18446744073709551615"), "a u64 constant of bits -1 is 2^64 - 1:\n%s", dz);
        free(t);
        printf("  a u64 constant is read unsigned\n");
    }
    // 10. a join (R7): a value made on one path is stated only inside that path's disjunct; as a
    // hypothesis of its own it would be read stale where the other path was taken
    {   IrFunc *f = ir_func_new(&A, nm("g12"), u64t, IR_FUNC_PURE);
        IrValue *i = ir_add_param(f, i32t, nm("i")), *n = ir_add_param(f, u64t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *bj = ir_new_block(f), *br = ir_new_block(f);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SLT, i, ir_const_int(f, b0, 0, i32t)), bj, b1);
        IrValue *x = ir_new_value(f, u64t);
        { IrInstr *ins = arena_push_aligned(&A, IrInstr); memset(ins, 0, sizeof *ins);
          ins->op = IR_CAST; ins->result = x; ins->operands = arena_push_many(&A, IrValue *, 1); ins->operands[0] = i;
          ins->n_operands = 1; ins->wrap = IR_WRAP_CHECK; ir_emit(b1, ins); }
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_UGE, x, n), br, bj);
        ir_set_ret(br, ir_const_int(f, br, 0, u64t));
        ir_set_ret(bj, ir_binop(f, bj, IR_UDIV, ir_const_int(f, bj, 100, u64t), n, u64t));
        char *t = gen_text(f);
        const char *dz = t ? vc_of(t, "divzero") : "";
        // x = i, in whichever order the two were declared
        char w1[64], w2[64];
        snprintf(w1, sizeof w1, "-1*%%v%d + 1*%%v%d = 0", i->id, x->id);
        snprintf(w2, sizeof w2, "1*%%v%d + -1*%%v%d = 0", x->id, i->id);
        const char *jl = strstr(dz, " join bb");
        CHECK(jl && (strstr(jl, w1) || strstr(jl, w2)), "the join states x = i inside the cast arm's disjunct:\n%s", dz);
        char want[64];
        snprintf(want, sizeof want, "def %%v%d cast", x->id);
        CHECK(!strstr(dz, want), "x = i is not a hypothesis of its own (on the other path x is stale):\n%s", dz);
        free(t);
        printf("  a join's path-local value is stated inside its disjunct only\n");
    }
    // 11. a claim at a forward join: a hypothesis wherever its version is named after the join, and
    // owed on each edge in, at the predecessor's end, the edge's condition a fact whichever arm of
    // the branch the edge is (y < 10 on the true arm of `y < 10`, and on the false arm of `y >= 10`)
    for (int k = 0; k < 2; k++) {
        IrFunc *f = ir_func_new(&A, nm(k ? "g14" : "g13"), u32t, IR_FUNC_PURE);
        IrValue *y = ir_add_param(f, u32t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        IrValue *s = ir_alloca(f, b0, u32t);
        ir_store(f, b0, s, y);
        IrValue *ten = ir_const_int(f, b0, 10, u32t);
        if (k == 0) ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, y, ten), b2, b1);
        else        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_UGE, y, ten), b1, b2);
        ir_store(f, b1, s, ir_const_int(f, b1, 5, u32t)); ir_set_br(b1, b2);
        ir_set_ret(b2, ir_binop(f, b2, IR_UDIV, ir_const_int(f, b2, 100, u32t), ir_load(f, b2, s, u32t), u32t));
        ir_finalize_cfg(f);
        JClaim c = claim1(b2, s, 1, 9);
        char *t = gen_with(f, &c, 1, JGEN_ALL, NULL);
        char want[128], key[32];
        const char *dz = t ? vc_of(t, "divzero") : "";
        snprintf(want, sizeof want, "claim bb%d #0 : 1*%%s%d@join.bb%d <= 9", b2->id, s->id, b2->id);
        CHECK(strstr(dz, want), "the claim is a hypothesis after its join:\n%s", dz);
        CHECK(t && count(t, " hull 0\n") == 2, "the claim is owed once on each of the 2 edges into its join:\n%s", t ? t : "");
        snprintf(key, sizeof key, "bb%d.%d hull", b0->id, nins(b0));
        const char *e0 = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "edge bb%d->bb%d : 1*%%v%d + -1*%%v%d <= -1", b0->id, b2->id, y->id, ten->id);
        CHECK(strstr(e0, want), "on the branch's edge, y < 10 is a fact:\n%s", e0);
        snprintf(want, sizeof want, "goal 1*%%s%d@bb%d.1 <= 9", s->id, b0->id);
        CHECK(strstr(e0, want), "the goal is the claim about the version the edge brings:\n%s", e0);
        snprintf(key, sizeof key, "bb%d.%d hull", b1->id, nins(b1));
        const char *e1 = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "goal 1*%%s%d@bb%d.1 <= 9", s->id, b1->id);
        CHECK(strstr(e1, want) && !strstr(e1, "edge "), "the other edge, an unconditional branch, owes it with no edge fact:\n%s", e1);
        free(t);
    }
    printf("  a claim at a join: stated after it, owed on each edge in with the edge's condition\n");
    // 12. a claim about a version made on ONE path into an outer join is stated inside that path's
    // disjunct only: where the other path was taken, the version is stale (as test 10's value)
    {   IrFunc *f = ir_func_new(&A, nm("g15"), u32t, IR_FUNC_PURE);
        IrValue *p = ir_add_param(f, boolt, nm("p")), *y = ir_add_param(f, u32t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrBlock *b4 = ir_new_block(f), *b5 = ir_new_block(f);
        IrValue *s = ir_alloca(f, b0, u32t);
        ir_store(f, b0, s, y);
        ir_set_br_cond(b0, p, b1, b4);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, y, ir_const_int(f, b1, 10, u32t)), b3, b2);
        ir_store(f, b2, s, ir_const_int(f, b2, 3, u32t)); ir_set_br(b2, b3);
        ir_set_br(b3, b5);
        ir_store(f, b4, s, ir_const_int(f, b4, 4, u32t)); ir_set_br(b4, b5);
        ir_set_ret(b5, ir_binop(f, b5, IR_UDIV, ir_const_int(f, b5, 100, u32t), ir_load(f, b5, s, u32t), u32t));
        ir_finalize_cfg(f);
        JClaim c = claim1(b3, s, 1, 9);
        char *t = gen_with(f, &c, 1, JGEN_ALL, NULL);
        const char *dz = t ? vc_of(t, "divzero") : "";
        char want[96], jl[32]; snprintf(jl, sizeof jl, " join bb%d : or {", b5->id);
        snprintf(want, sizeof want, "1*%%s%d@join.bb%d <= 9", s->id, b3->id);
        const char *j = strstr(dz, jl), *eol = j ? strchr(j, '\n') : NULL, *in = j ? strstr(j, want) : NULL;
        CHECK(in && eol && in < eol, "the inner join's claim is inside the outer join's disjunct:\n%s", dz);
        snprintf(want, sizeof want, "claim bb%d #0", b3->id);
        CHECK(!strstr(dz, want), "and is not a hypothesis of its own:\n%s", dz);
        free(t);
        // the slots a proposer may claim about: both joins' fresh versions, the inner one first
        JMem m = jmem_over(region, sizeof region);
        JSlot *sl; int32_t ns; char err[200];
        bool ok = jgen_slots(&m, f, gen_mod, &sl, &ns, err, sizeof err);
        CHECK(ok && ns == 2 && sl[0].blk == b3->id && sl[1].blk == b5->id && sl[0].key == s->id,
              "the slots are the two joins' versions of s, in reverse post-order (%d)", ok ? ns : -1);
        // what a caller asks for: the claims' obligations only, or one block's
        t = gen_with(f, &c, 1, JGEN_CLAIMS, NULL);
        CHECK(t && count(t, "vc ") == 2 && count(t, " hull ") == 2, "JGEN_CLAIMS gives the claims' VCs only:\n%s", t ? t : "");
        free(t);
        t = gen_with(f, &c, 1, b5->id, NULL);
        CHECK(t && t[0] == 0, "a block with no claim gives no VC:\n%s", t ? t : "");
        free(t);
        printf("  a path-local claim is stated in its disjunct; the slots; the claims-only modes\n");
    }
    // 13. claims are untrusted input: one the generator cannot state and owe is refused, never
    // dropped (a claim stated without its obligations would be a false proof)
    {   IrFunc *f = ir_func_new(&A, nm("g16"), u32t, IR_FUNC_PURE);
        IrValue *y = ir_add_param(f, u32t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f);
        IrValue *s = ir_alloca(f, b0, u32t), *u = ir_alloca(f, b0, u32t);
        ir_store(f, b0, s, y); ir_store(f, b0, u, y);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, y, ir_const_int(f, b0, 10, u32t)), b2, b1);
        ir_store(f, b1, s, ir_const_int(f, b1, 5, u32t)); ir_set_br(b1, b2);
        // b2 joins (s afresh; u the same on both paths), then a loop headed by b3
        ir_set_br(b2, b3);
        IrValue *ls = ir_load(f, b3, s, u32t);
        IrBlock *b5 = ir_new_block(f);
        ir_set_br_cond(b3, ir_icmp(f, b3, IR_CMP_ULT, ls, ir_const_int(f, b3, 100, u32t)), b4, b5);
        ir_store(f, b4, s, ir_binop(f, b4, IR_ADD, ls, ir_const_int(f, b4, 1, u32t), u32t));
        ir_set_br(b4, b3);
        ir_set_ret(b5, ls);
        ir_finalize_cfg(f);
        struct { JClaim c; const char *why; } bad[7];
        bad[0].c = claim1(b0, s, 1, 9); bad[0].why = "not at a block entered by edges only";
        bad[1].c = claim1(b2, u, 1, 9); bad[1].why = "about no version its block makes afresh";
        bad[2].c = claim1(b2, s, 1, 9); bad[2].c.nt = 2; bad[2].c.key[1] = ls->id; bad[2].c.coef[1] = -1;
        bad[2].why = "not a cell or a value fixed at its block";      // ls is made in the loop, after b2
        bad[3].c = claim1(b2, s, 1, 9); bad[3].c.nt = JCLAIM_MAXT + 1; bad[3].why = "of more than it may have";
        bad[4].c = claim1(b2, s, 1, 9); bad[4].c.nt = 2; bad[4].c.key[1] = s->id; bad[4].c.coef[1] = 1;
        bad[4].why = "naming one term twice";
        bad[5].c = claim1(b2, y, 1, 9); bad[5].why = "about no version its block makes afresh";
        bad[6].c = claim1(b3, s, 1, 9); bad[6].c.nt = 2; bad[6].c.key[1] = ls->id; bad[6].c.coef[1] = -1;
        bad[6].why = "not a cell or a value fixed at its block";      // ls is made IN the header: not fixed at its entry
        for (int k = 0; k < 7; k++) {
            char why[200] = "";
            char *t = gen_with(f, &bad[k].c, 1, JGEN_ALL, why);
            CHECK(!t && strstr(why, bad[k].why), "claim %d is refused (%s): got \"%s\"", k, bad[k].why, why);
            free(t);
        }
        JClaim good[5];
        good[0] = claim1(b2, s, 1, 9);                                // at the join
        good[1] = claim1(b3, s, 1, 100);                              // at the loop header
        good[2] = claim1(b2, s, 1, 0); good[2].nt = 2; good[2].key[1] = y->id; good[2].coef[1] = -1;   // s - y, y a parameter
        good[3] = claim1(b3, s, 1, 0); good[3].nt = 2; good[3].key[1] = u->id; good[3].coef[1] = -1;  // s - u, u not written
        good[4] = claim1(b3, s, 1, 0); good[4].nt = 3; good[4].key[1] = u->id; good[4].coef[1] = -1;  // s - u - y: three terms
        good[4].key[2] = y->id; good[4].coef[2] = -1;
        for (int k = 0; k < 5; k++) {
            char why[200] = "";
            char *t = gen_with(f, &good[k], 1, JGEN_ALL, why);
            CHECK(t != NULL, "claim %d is accepted: got \"%s\"", k, why);
            free(t);
        }
        // what a proposer may name at the header: s made there, u and y fixed there, ls not
        {   JMem m = jmem_over(region, sizeof region);
            int32_t keys[4] = { s->id, u->id, y->id, ls->id }; uint8_t kind[4]; char err[200];
            bool ok = jgen_term_kinds(&m, f, gen_mod, b3->id, keys, NULL, 4, kind, err, sizeof err);
            CHECK(ok && kind[0] == 2 && kind[1] == 1 && kind[2] == 1 && kind[3] == 0,
                  "term kinds at the header: s 2, u 1, y 1, ls 0 (got %d %d %d %d)", kind[0], kind[1], kind[2], kind[3]);
        }
        printf("  claims refused: at the entry, no fresh version, a term not fixed, five terms, one twice\n");
        // 15. a claim at a loop header is owed on the entry edge (`inv.entry`) and on the back edge
        // (`inv.keep`), where the header's own claim, about its previous entry, is a hypothesis
        {   char *t = gen_with(f, &good[1], 1, JGEN_CLAIMS, NULL);
            char key[32], want[96];
            snprintf(key, sizeof key, "bb%d.%d inv.entry 0", b2->id, nins(b2));
            const char *en = t ? vc_of(t, key) : "";
            snprintf(want, sizeof want, "goal 1*%%s%d@join.bb%d <= 100", s->id, b2->id);
            CHECK(strstr(en, want) && !strstr(en, "claim bb"), "the entry edge owes it about the join's version, assuming no claim:\n%s", en);
            snprintf(key, sizeof key, "bb%d.%d inv.keep 0", b4->id, nins(b4));
            const char *kp = t ? vc_of(t, key) : "";
            snprintf(want, sizeof want, "claim bb%d #0 : 1*%%s%d@head.bb%d <= 100", b3->id, s->id, b3->id);
            CHECK(strstr(kp, want), "the back edge assumes the claim at the header:\n%s", kp);
            snprintf(want, sizeof want, "goal 1*%%s%d@bb%d.", s->id, b4->id);
            CHECK(strstr(kp, want), "and owes it about the version the latch stores:\n%s", kp);
            CHECK(t && count(t, "vc ") == 2, "two edges, two VCs:\n%s", t ? t : "");
            free(t);
            printf("  a header's claim: owed on the entry edge, and on the back edge assuming itself\n");
        }
    }
    // 14. a predecessor that reaches the join by BOTH arms of its branch owes the claim once (two
    // VCs with one id otherwise), with no edge fact: the condition holds on one arm only
    {   IrFunc *f = ir_func_new(&A, nm("g17"), u32t, IR_FUNC_PURE);
        IrValue *y = ir_add_param(f, u32t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *s = ir_alloca(f, b0, u32t);
        ir_store(f, b0, s, y);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, y, ir_const_int(f, b0, 10, u32t)), b1, b2);
        ir_store(f, b1, s, ir_const_int(f, b1, 1, u32t));
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, y, ir_const_int(f, b1, 5, u32t)), b3, b3);
        ir_store(f, b2, s, ir_const_int(f, b2, 2, u32t)); ir_set_br(b2, b3);
        ir_set_ret(b3, ir_load(f, b3, s, u32t));
        ir_finalize_cfg(f);
        JClaim c = claim1(b3, s, 1, 2);
        char *t = gen_with(f, &c, 1, JGEN_CLAIMS, NULL);
        char key[32]; snprintf(key, sizeof key, "bb%d.%d hull", b1->id, nins(b1));
        CHECK(t && count(t, key) == 1 && count(t, " hull 0\n") == 2, "the branch owes the claim once, and b2 once:\n%s", t ? t : "");
        const char *e1 = t ? vc_of(t, key) : "";
        CHECK(e1[0] && !strstr(e1, "edge "), "with no edge fact:\n%s", e1);
        free(t);
        printf("  a branch whose both arms enter the join owes its claim once, with no edge fact\n");
    }
    // 16. a struct's FIELDS are cells (P2.1), and P2.2's frame rule: a constructor's store gives each
    // field its value; a call handed no reference to the struct, or a read-only one, keeps them; a
    // call handed a mutable one makes them fresh, and so does a call to a callee the generator
    // cannot see, once the struct has been handed to calls at all
    for (int known = 1; known >= 0; known--) {
        IrType *S = ir_type_new(&A, IRT_STRUCT);
        IrType *ps = ir_type_new(&A, IRT_PTR); ps->elem = S;
        IrType *pws = ir_type_new(&A, IRT_PTR); pws->elem = S; pws->ptr_mut = true;
        IrFunc *rd = ir_func_new(&A, nm("rd"), u32t, IR_FUNC_PURE), *wr = ir_func_new(&A, nm("wr"), u32t, IR_FUNC_PURE);
        IrFunc *nop = ir_func_new(&A, nm("nop"), u32t, IR_FUNC_PURE), *f = ir_func_new(&A, nm(known ? "g18" : "g19"), u32t, IR_FUNC_PURE);
        ir_add_param(rd, ps, nm("p")); ir_add_param(wr, pws, nm("p")); ir_add_param(nop, u32t, nm("x"));
        ir_set_ret(rd->entry, ir_const_int(rd, rd->entry, 0, u32t)); ir_set_ret(wr->entry, ir_const_int(wr, wr->entry, 0, u32t));
        ir_set_ret(nop->entry, ir_const_int(nop, nop->entry, 0, u32t));
        rd->next = wr; wr->next = nop; nop->next = f;
        IrBlock *b0 = f->entry;
        IrValue *s = ir_alloca(f, b0, S);
        IrValue *fv[2] = { ir_const_int(f, b0, 5, u32t), ir_const_int(f, b0, 6, u32t) };
        ir_store(f, b0, s, ir_struct_new(f, b0, S, fv, 2));
        mk_call(f, b0, "nop", ir_const_int(f, b0, 1, u32t), u32t);
        IrValue *q1 = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), ir_load(f, b0, ir_field_ptr(f, b0, s, 0, u32t), u32t), u32t);
        mk_call(f, b0, "rd", s, u32t);
        IrValue *q2 = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), ir_load(f, b0, ir_field_ptr(f, b0, s, 1, u32t), u32t), u32t);
        mk_call(f, b0, "wr", s, u32t);
        IrValue *q3 = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), ir_load(f, b0, ir_field_ptr(f, b0, s, 0, u32t), u32t), u32t);
        IrInstr *a1 = ir_instr(f, IR_ADD, u32t, 2); a1->operands[0] = q1; a1->operands[1] = q2; a1->wrap = IR_WRAP_MODULAR; ir_emit(b0, a1);
        IrInstr *a2 = ir_instr(f, IR_ADD, u32t, 2); a2->operands[0] = a1->result; a2->operands[1] = q3; a2->wrap = IR_WRAP_MODULAR; ir_emit(b0, a2);
        ir_set_ret(b0, a2->result);
        ir_finalize_cfg(f);
        gen_mod = known ? rd : NULL;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        gen_mod = NULL;
        // vc_of returns one buffer: each VC is copied out before the next is looked up
        char key[48], want[64], v1[1 << 12], v2[1 << 12], v3[1 << 12];
        IrValue *qs[3] = { q1, q2, q3 }; char *vs[3] = { v1, v2, v3 };
        for (int k = 0; k < 3; k++) {
            snprintf(key, sizeof key, "bb%d.%d divzero", b0->id, idx_of(b0, qs[k]));
            snprintf(vs[k], sizeof v1, "%s", t ? vc_of(t, key) : "");
        }
        snprintf(want, sizeof want, "%%s%d.0@havoc.bb%d.", s->id, b0->id);
        CHECK(strstr(v3, want), "after the call handed a mutable reference, the field is fresh:\n%s", v3);
        if (known) {
            snprintf(want, sizeof want, "store %%s%d.0@bb%d.", s->id, b0->id);
            CHECK(strstr(v1, want) && !strstr(v1, "@havoc"), "after a call handed no reference, the constructor's field 0 holds:\n%s", v1);
            snprintf(want, sizeof want, "store %%s%d.1@bb%d.", s->id, b0->id);
            CHECK(strstr(v2, want) && !strstr(v2, "@havoc"), "after a read-only reference, the constructor's field 1 holds:\n%s", v2);
        } else {
            snprintf(want, sizeof want, "%%s%d.0@havoc.bb%d.", s->id, b0->id);
            CHECK(strstr(v1, want), "with no module, an unknown callee may write a struct handed to calls:\n%s", v1);
        }
        free(t);
    }
    printf("  field cells: a constructor's values; kept by a call with no or a read-only reference, not by a mutable one\n");
    // 17. a RETURN claim (a callee contract): owed as `post` at every return, about the value
    // returned and the parameters' ENTRY values (a parameter's copy the body changes is not the
    // parameter); stated at a call in the same module, the call's result for the returned value
    // and each argument for its parameter, where the argument lands exactly; refused when it
    // cannot be owed
    for (int other = 0; other <= 1; other++) {
        // mn(a, b): var r = a; if b < a { r = b }; return r     (claimed: ret - a <= 0)
        IrFunc *mn = ir_func_new(&A, nm("mn"), u32t, IR_FUNC_PURE), *f = ir_func_new(&A, nm(other ? "g21" : "g20"), u32t, IR_FUNC_PURE);
        IrValue *pa = ir_add_param(mn, u32t, nm("a")), *pb = ir_add_param(mn, u32t, nm("b"));
        IrBlock *m0 = mn->entry, *m1 = ir_new_block(mn), *m2 = ir_new_block(mn), *m3 = ir_new_block(mn);
        IrValue *r = ir_alloca(mn, m0, u32t);
        ir_store(mn, m0, r, pa);
        ir_set_br_cond(m0, ir_icmp(mn, m0, IR_CMP_ULT, pb, pa), m1, m2);
        ir_store(mn, m1, r, pb); ir_set_ret(m1, ir_load(mn, m1, r, u32t));
        ir_set_ret(m2, ir_load(mn, m2, r, u32t));
        (void)m3;
        mn->src_file = other ? "other.ln" : NULL;
        mn->next = f;
        ir_finalize_cfg(mn);
        JClaim c; memset(&c, 0, sizeof c);
        c.blk = JCLAIM_RET; c.fn = mn; c.nt = 2; c.key[0] = JKEY_RET; c.coef[0] = 1; c.key[1] = pa->id; c.coef[1] = -1; c.rhs = 0;
        if (!other) {
            char *t = gen_with(mn, &c, 1, JGEN_POST, NULL);
            CHECK(t && count(t, " post 0\n") == 2, "one `post` VC at each of the two returns:\n%s", t ? t : "");
            char key[32], want[64];
            snprintf(key, sizeof key, "bb%d.%d post", m1->id, nins(m1));
            const char *p1 = t ? vc_of(t, key) : "";
            // the returned load is r's version there (b, stored); `a` is the parameter, not r
            snprintf(want, sizeof want, "goal 1*%%s%d@bb%d.0 + -1*%%v%d <= 0", r->id, m1->id, pa->id);
            CHECK(strstr(p1, want), "the goal is the value returned minus a's entry value:\n%s", p1);
            free(t);
            // JGEN_CLAIMS owes it too; a block id (one cut point's) does not
            t = gen_with(mn, &c, 1, JGEN_CLAIMS, NULL);
            CHECK(t && count(t, " post 0\n") == 2, "JGEN_CLAIMS owes the return claim:\n%s", t ? t : "");
            free(t);
            t = gen_with(mn, &c, 1, m1->id, NULL);
            CHECK(t && count(t, " post ") == 0, "a block's claims only: no return claim:\n%s", t ? t : "");
            free(t);
        }
        // the caller: q = 100 / mn(x, 7)
        IrValue *x = ir_add_param(f, u32t, nm("x"));
        IrBlock *b0 = f->entry;
        IrInstr *call = ir_instr(f, IR_CALL, u32t, 2);
        call->aux.callee = nm("mn"); call->operands[0] = x; call->operands[1] = ir_const_int(f, b0, 7, u32t);
        ir_emit(b0, call);
        IrValue *q = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), call->result, u32t);
        ir_set_ret(b0, q);
        ir_finalize_cfg(f);
        f->src_file = NULL;
        gen_mod = mn;
        char *t = gen_with(f, &c, 1, JGEN_ALL, NULL);
        char key[32], want[96];
        snprintf(key, sizeof key, "bb%d.%d divzero", b0->id, idx_of(b0, q));
        const char *dz = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "contract mn #0 : 1*%%v%d + -1*%%v%d <= 0", call->result->id, x->id);
        if (other) CHECK(dz[0] && !strstr(dz, "contract"), "a callee of another module states no contract (I.181):\n%s", dz);
        else       CHECK(strstr(dz, want), "the call states mn's claim, its result and the argument for a:\n%s", dz);
        CHECK(t && count(t, " post ") == 0, "the caller owes no callee's return claim:\n%s", t ? t : "");
        free(t);
        if (!other) {
            // an argument that does not land exactly (a narrowing the call waives) states nothing
            IrFunc *n8 = ir_func_new(&A, nm("n8"), u8t, IR_FUNC_PURE), *g = ir_func_new(&A, nm("g22"), u8t, IR_FUNC_PURE);
            IrValue *pn = ir_add_param(n8, u8t, nm("v"));
            ir_set_ret(n8->entry, pn);
            n8->next = g; ir_finalize_cfg(n8);
            IrValue *y = ir_add_param(g, u32t, nm("y"));
            IrInstr *c2 = ir_instr(g, IR_CALL, u8t, 1);
            c2->aux.callee = nm("n8"); c2->operands[0] = y; c2->unchecked = true;
            ir_emit(g->entry, c2);
            IrValue *q2 = ir_binop(g, g->entry, IR_UDIV, ir_const_int(g, g->entry, 100, u8t), c2->result, u8t);
            ir_set_ret(g->entry, q2);
            ir_finalize_cfg(g);
            JClaim c8; memset(&c8, 0, sizeof c8);
            c8.blk = JCLAIM_RET; c8.fn = n8; c8.nt = 2; c8.key[0] = JKEY_RET; c8.coef[0] = 1; c8.key[1] = pn->id; c8.coef[1] = -1;
            gen_mod = n8;
            t = gen_with(g, &c8, 1, JGEN_ALL, NULL);
            snprintf(key, sizeof key, "bb%d.%d divzero", g->entry->id, idx_of(g->entry, q2));
            dz = t ? vc_of(t, key) : "";
            CHECK(dz[0] && !strstr(dz, "contract"), "an argument narrowed unchecked does not land exactly: no contract:\n%s", dz);
            free(t);
            // c2 checked: one that does not fit traps before n8 runs, so the claim holds of it
            c2->unchecked = false;
            t = gen_with(g, &c8, 1, JGEN_ALL, NULL);
            dz = t ? vc_of(t, key) : "";
            CHECK(strstr(dz, "contract n8 #0"), "a checked landing states it:\n%s", dz);
            free(t);
            // refused: no function, not about the returned value, a term no parameter, a return
            // that wraps, a function returning no integer, one lowered with a construct dropped
            struct { JClaim c; IrFunc *at; const char *why; } bad[6];
            for (int k = 0; k < 6; k++) { bad[k].c = c; bad[k].at = mn; }
            bad[0].c.fn = NULL; bad[0].why = "with no function";
            bad[1].c.key[0] = pb->id; bad[1].why = "not about the returned value";
            bad[2].c.key[1] = r->id; bad[2].why = "neither the returned value nor an integer parameter";
            bad[3].c = c8; bad[3].at = n8; bad[3].why = "whose return wraps";
            bad[4].c = c8; bad[4].at = n8; bad[4].why = "returning no integer";
            bad[5].why = "lowered incompletely";
            n8->entry->term.unchecked = true; n8->ret_type = u8t;
            IrValue *wide = ir_add_param(n8, u32t, nm("w"));
            n8->entry->term.cond = wide;                     // return w as u8, waived: it wraps
            gen_mod = mn;
            for (int k = 0; k < 6; k++) {
                if (k == 4) { n8->entry->term.unchecked = false; n8->entry->term.cond = pn; n8->ret_type = slice_of(u8t); }
                mn->incomplete = k == 5;
                char why[200] = "";
                char *tb = gen_with(bad[k].at, &bad[k].c, 1, JGEN_ALL, why);
                CHECK(!tb && strstr(why, bad[k].why), "return claim %d is refused (%s): got \"%s\"", k, bad[k].why, why);
                free(tb);
            }
            mn->incomplete = false;
        }
        gen_mod = NULL;
    }
    // a RECURSIVE function's return claim is a fact at its own call: down(n) returns n below 8, else
    // down(n - 8); `ret <= 7` is owed at the second return assuming itself of the call's result
    {   IrFunc *dn = ir_func_new(&A, nm("down"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(dn, u32t, nm("n"));
        IrBlock *b0 = dn->entry, *b1 = ir_new_block(dn), *b2 = ir_new_block(dn);
        ir_set_br_cond(b0, ir_icmp(dn, b0, IR_CMP_ULT, n, ir_const_int(dn, b0, 8, u32t)), b1, b2);
        ir_set_ret(b1, n);
        IrInstr *sub = ir_instr(dn, IR_SUB, u32t, 2); sub->operands[0] = n; sub->operands[1] = ir_const_int(dn, b2, 8, u32t);
        sub->wrap = IR_WRAP_MODULAR; ir_emit(b2, sub);
        IrValue *r = mk_call(dn, b2, "down", sub->result, u32t);
        ir_set_ret(b2, r);
        ir_finalize_cfg(dn);
        JClaim c; memset(&c, 0, sizeof c);
        c.blk = JCLAIM_RET; c.fn = dn; c.nt = 1; c.key[0] = JKEY_RET; c.coef[0] = 1; c.rhs = 7;
        gen_mod = dn;
        char *t = gen_with(dn, &c, 1, JGEN_POST, NULL);
        gen_mod = NULL;
        char key[32], want[64];
        snprintf(key, sizeof key, "bb%d.%d post", b2->id, nins(b2));
        const char *p2 = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "contract down #0 : 1*%%v%d <= 7", r->id);
        CHECK(strstr(p2, want), "the self-call's result is assumed below 8 where it is returned:\n%s", p2);
        free(t);
    }
    printf("  a return claim: owed at every return about the entry values, a contract at a same-module call\n");
    // 18. LANDINGS (P2.3): a value meeting a slot of another integer type, or a refined one, owes
    // the slot's range where the interpreter lands it (ii_land): a store, a struct field, a sum
    // payload, a call argument (a direct callee's parameter, or the function pointer type's), the
    // result. A waived landing owes nothing unless the slot is refined, and the obligation is what
    // makes a store's version the value stored
    {   IrType *r03 = ir_type_int(&A, 32, true); r03->has_refine = true; r03->refine_lo = 0; r03->refine_hi = 3;
        IrType *nz = ir_type_int(&A, 32, true); nz->has_ne = true; nz->refine_ne = 0;
        IrType *i33 = ir_type_int(&A, 33, true);
        IrType *S = ir_type_new(&A, IRT_STRUCT);
        S->n_fields = 2; S->fields = arena_push_many_aligned(&A, IrType*, 2); S->fields[0] = u8t; S->fields[1] = i32t;
        IrType *Su = ir_type_new(&A, IRT_SUM);
        Su->n_fields = 1; Su->fields = arena_push_many_aligned(&A, IrType*, 1); Su->fields[0] = u8t;
        IrType *fnt = ir_type_new(&A, IRT_FUNC);
        fnt->n_fields = 1; fnt->fields = arena_push_many_aligned(&A, IrType*, 1); fnt->fields[0] = u8t; fnt->elem = u8t;
        IrFunc *take = ir_func_new(&A, nm("take8"), u8t, IR_FUNC_PURE), *f = ir_func_new(&A, nm("g23"), u8t, IR_FUNC_PURE);
        IrFunc *two = ir_func_new(&A, nm("take2"), u8t, IR_FUNC_PURE);
        IrValue *tp = ir_add_param(take, u8t, nm("x"));
        ir_set_ret(take->entry, tp);
        ir_add_param(two, i32t, nm("a")); IrValue *tb = ir_add_param(two, u8t, nm("b"));
        ir_set_ret(two->entry, tb);
        take->next = two; two->next = f; ir_finalize_cfg(take); ir_finalize_cfg(two);
        IrValue *w = ir_add_param(f, i33, nm("w")), *y = ir_add_param(f, i32t, nm("y")), *b8 = ir_add_param(f, u8t, nm("b8"));
        IrBlock *b0 = f->entry;
        IrValue *s[6];
        const IrType *sty[6] = { i32t, i32t, i32t, r03, nz, u32t };
        IrValue *sv[6] = { w, y, w, y, y, b8 };
        bool unc[6] = { false, false, true, true, false, true };
        int at[6];
        for (int q = 0; q < 6; q++) s[q] = ir_alloca(f, b0, (IrType *)sty[q]);
        for (int q = 0; q < 6; q++) {
            at[q] = nins(b0); ir_store(f, b0, s[q], sv[q]);
            IrInstr *st = b0->instrs; while (st->next) st = st->next;
            st->unchecked = unc[q];
        }
        IrValue *fv[2] = { y, w };
        IrValue *sn = ir_struct_new(f, b0, S, fv, 2);
        IrValue *pv[1] = { y };
        IrValue *un = ir_sum_new(f, b0, Su, 0, pv, 1);
        IrValue *c1 = mk_call(f, b0, "take8", y, u8t);
        IrValue *c2 = mk_call(f, b0, "builtin_nobody", y, u8t);       // a builtin: lands nowhere
        IrInstr *c3 = ir_instr(f, IR_CALL, u8t, 2);                    // take2(y, y): only b narrows
        c3->aux.callee = nm("take2"); c3->operands[0] = y; c3->operands[1] = y; ir_emit(b0, c3);
        IrValue *fp = ir_func_ref(f, b0, nm("take8"), fnt);
        IrInstr *ic = ir_instr(f, IR_CALL, u8t, 2); ic->operands[0] = fp; ic->operands[1] = y; ir_emit(b0, ic);
        // a constructor's field lands at the CONSTRUCTOR: waived there, the field's version is not
        // the value even when the struct's store is checked
        IrType *S1 = ir_type_new(&A, IRT_STRUCT);
        S1->n_fields = 1; S1->fields = arena_push_many_aligned(&A, IrType*, 1); S1->fields[0] = i32t;
        IrValue *sc[2], *fq[2];
        for (int k = 0; k < 2; k++) {
            sc[k] = ir_alloca(f, b0, S1);
            IrValue *one[1] = { w };
            IrValue *cv = ir_struct_new(f, b0, S1, one, 1);
            IrInstr *cn = b0->instrs; while (cn->next) cn = cn->next;
            cn->unchecked = k == 1;
            ir_store(f, b0, sc[k], cv);
        }
        IrValue *q[6];
        for (int k = 0; k < 6; k++) q[k] = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), ir_load(f, b0, s[k], (IrType *)sty[k]), u32t);
        for (int k = 0; k < 2; k++) fq[k] = ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 100, i32t), ir_load(f, b0, ir_field_ptr(f, b0, sc[k], 0, i32t), i32t), i32t);
        (void)sn; (void)un; (void)c2; (void)q;
        ir_set_ret(b0, y);                                           // an i32 returned as a u8
        ir_finalize_cfg(f);
        gen_mod = take;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        gen_mod = NULL;
        char key[48];
        #define HAS(pt, kind, ord) (snprintf(key, sizeof key, " bb%d.%d " kind " %d\n", b0->id, (pt), (ord)), t && strstr(t, key))
        CHECK(HAS(at[0], "narrow.lo", 0) && HAS(at[0], "narrow.hi", 0), "an i33 stored in an i32 owes both sides:\n%s", t ? t : "");
        CHECK(!HAS(at[1], "narrow.lo", 0) && !HAS(at[1], "narrow.hi", 0), "a store of its own type owes nothing");
        CHECK(!HAS(at[2], "narrow.lo", 0) && !HAS(at[2], "narrow.hi", 0), "a waived store into a plain slot owes nothing");
        CHECK(HAS(at[3], "narrow.lo", 0) && HAS(at[3], "narrow.hi", 0), "a waived store into a refined slot owes its range (I.60)");
        CHECK(!HAS(at[4], "narrow.lo", 0) && !HAS(at[4], "narrow.hi", 0) && HAS(at[4], "narrow.ne", 0), "a slot != 0 owes only the excluded value");
        CHECK(!HAS(at[5], "narrow.lo", 0) && !HAS(at[5], "narrow.hi", 0), "a u8 into a u32 cannot lose it");
        int isn = idx_of(b0, sn), iun = idx_of(b0, un), ic1 = idx_of(b0, c1), ic2 = idx_of(b0, c2), iic = idx_of(b0, ic->result);
        CHECK(HAS(isn, "narrow.lo", 0) && HAS(isn, "narrow.hi", 0) && HAS(isn, "narrow.lo", 1) && HAS(isn, "narrow.hi", 1),
              "a struct's fields each owe their type, by position:\n%s", t ? t : "");
        CHECK(HAS(iun, "narrow.lo", 0) && HAS(iun, "narrow.hi", 0), "a sum's payload owes its type");
        CHECK(HAS(ic1, "narrow.lo", 0) && HAS(ic1, "narrow.hi", 0), "a call's argument owes its parameter's type");
        CHECK(!HAS(ic2, "narrow.lo", 0) && !HAS(ic2, "narrow.hi", 0), "a builtin's argument lands nowhere");
        int ic3 = idx_of(b0, c3->result);
        CHECK(!HAS(ic3, "narrow.lo", 0) && HAS(ic3, "narrow.lo", 1) && HAS(ic3, "narrow.hi", 1),
              "each argument owes its own parameter, by position:\n%s", t ? t : "");
        CHECK(HAS(iic, "narrow.lo", 0) && HAS(iic, "narrow.hi", 0), "an indirect call's argument owes the pointer type's parameter");
        CHECK(HAS(nins(b0), "narrow.lo", 0) && HAS(nins(b0), "narrow.hi", 0), "the result owes the return type, at the block's end");
        const char *ne = t ? vc_of(t, (snprintf(key, sizeof key, "bb%d.%d narrow.ne", b0->id, at[4]), key)) : "";
        char want[96]; snprintf(want, sizeof want, "goal 1*%%v%d != 0", y->id);
        CHECK(strstr(ne, want), "the excluded value is a `!=` goal:\n%s", ne);
        // which versions are the value stored: the checked one, and the waived one into a refined
        // slot (its obligation is owed); not the waived one into a plain slot (it may wrap)
        bool exact[6] = { true, true, false, true, true, true };
        for (int k = 0; k < 6; k++) {
            snprintf(key, sizeof key, "bb%d.%d divzero", b0->id, idx_of(b0, q[k]));
            char vcbuf[1 << 12]; snprintf(vcbuf, sizeof vcbuf, "%s", t ? vc_of(t, key) : "");
            snprintf(want, sizeof want, "store %%s%d@bb%d.%d :", s[k]->id, b0->id, at[k]);
            CHECK(!!strstr(vcbuf, want) == exact[k], "store %d is %sthe value stored:\n%s", k, exact[k] ? "" : "not ", vcbuf);
        }
        for (int k = 0; k < 2; k++) {
            snprintf(key, sizeof key, "bb%d.%d divzero", b0->id, idx_of(b0, fq[k]));
            char vcbuf[1 << 12]; snprintf(vcbuf, sizeof vcbuf, "%s", t ? vc_of(t, key) : "");
            snprintf(want, sizeof want, "store %%s%d.0@bb%d.", sc[k]->id, b0->id);
            CHECK(plain_store(vcbuf, want) == (k == 0), "a field constructed %s is %sthe value:\n%s", k ? "waived" : "checked", k ? "not " : "", vcbuf);
        }
        #undef HAS
        free(t);
        // waived, the result owes nothing (it wraps); into a refined result type it is owed
        b0->term.unchecked = true;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        snprintf(key, sizeof key, " bb%d.%d narrow.hi 0\n", b0->id, nins(b0));
        CHECK(t && !strstr(t, key), "a waived return into a plain type owes nothing");
        free(t);
        IrType *r8 = ir_type_int(&A, 8, false); r8->has_refine = true; r8->refine_lo = 0; r8->refine_hi = 9;
        f->ret_type = r8;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && strstr(t, key), "a waived return into a refined type owes its range");
        free(t);
    }
    printf("  landings: a store, a field, a payload, an argument, the result; waived unless refined\n");
    // 19. an ASSERT (P2.6) owes its condition where it stands, `unsafe` or not, its kind naming its
    // code; a comparison is a goal as a fact is read (an equality two VCs; one value compared with
    // itself the empty sum), anything else `!= 0`, and a comparison that cannot be a goal is owed
    // through its own result, which nothing defines. After it, its condition is a fact; never
    // before it, nor in its own VC
    {   IrFunc *f = ir_func_new(&A, nm("g24"), u32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i32t, nm("x")), *y = ir_add_param(f, i32t, nm("y"));
        IrValue *u = ir_add_param(f, u32t, nm("u")), *p = ir_add_param(f, boolt, nm("p"));
        IrBlock *b0 = f->entry;
        IrValue *q0 = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), u, u32t);   // before the asserts
        struct { IrValue *c; int code; } as[7];
        as[0].c = ir_icmp(f, b0, IR_CMP_SLT, x, y);                                  as[0].code = 86;
        as[1].c = ir_icmp(f, b0, IR_CMP_EQ, x, y);                                   as[1].code = 0;
        IrValue *zero = ir_const_int(f, b0, 0, u32t);
        as[2].c = ir_icmp(f, b0, IR_CMP_NE, u, zero);                                as[2].code = 121;
        as[3].c = ir_icmp(f, b0, IR_CMP_ULT, x, u);                                  as[3].code = 87;   // x may be negative
        as[4].c = p;                                                                 as[4].code = 0;
        as[5].c = ir_icmp(f, b0, IR_CMP_NE, x, x);                                   as[5].code = 135;
        as[6].c = ir_icmp(f, b0, IR_CMP_ULE, u, u);                                  as[6].code = 86;
        int at[7];
        for (int k = 0; k < 7; k++) {
            at[k] = nins(b0); ir_assert_coded(f, b0, as[k].c, as[k].code);
            if (k == 2) { IrInstr *z = b0->instrs; while (z->next) z = z->next; z->unchecked = true; }
        }
        IrValue *q1 = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, u32t), u, u32t);   // after them
        IrInstr *a1 = ir_instr(f, IR_ADD, u32t, 2); a1->operands[0] = q0; a1->operands[1] = q1; a1->wrap = IR_WRAP_MODULAR; ir_emit(b0, a1);
        ir_set_ret(b0, a1->result);
        ir_finalize_cfg(f);
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        char key[48], want[96];
        #define VC(k, kind, ord) (snprintf(key, sizeof key, "bb%d.%d " kind " %d", b0->id, at[k], ord), t ? vc_of(t, key) : "")
        snprintf(want, sizeof want, "goal 1*%%v%d + -1*%%v%d <= -1", x->id, y->id);
        CHECK(strstr(VC(0, "assert.86", 0), want), "x < y is the goal, the kind naming code 86:\n%s", t ? t : "");
        snprintf(want, sizeof want, "goal 1*%%v%d + -1*%%v%d <= 0", x->id, y->id);
        CHECK(strstr(VC(1, "assert.0", 0), want), "x = y is two VCs, x - y <= 0...");
        snprintf(want, sizeof want, "goal -1*%%v%d + 1*%%v%d <= 0", x->id, y->id);
        CHECK(strstr(VC(1, "assert.0", 1), want), "...and y - x <= 0");
        snprintf(want, sizeof want, "goal 1*%%v%d + -1*%%v%d != 0", u->id, zero->id);
        CHECK(strstr(VC(2, "assert.121", 0), want), "u != 0 is owed inside `unsafe` too:\n%s", VC(2, "assert.121", 0));
        snprintf(want, sizeof want, "goal 1*%%v%d != 0", as[3].c->id);
        CHECK(strstr(VC(3, "assert.87", 0), want), "an unsigned comparison of a signed value is owed through its result:\n%s", VC(3, "assert.87", 0));
        snprintf(want, sizeof want, "goal 1*%%v%d != 0", p->id);
        CHECK(strstr(VC(4, "assert.0", 0), want), "a bool is owed != 0");
        CHECK(strstr(VC(5, "assert.135", 0), "goal 0 != 0"), "x != x is the empty sum, never implied:\n%s", VC(5, "assert.135", 0));
        CHECK(strstr(VC(6, "assert.86", 0), "goal 0 <= 0"), "u <= u is the empty sum, always implied:\n%s", VC(6, "assert.86", 0));
        const char *v0 = VC(0, "assert.86", 0);
        CHECK(!strstr(v0, "assert bb"), "an assert's own condition is not its hypothesis, nor a later one's:\n%s", v0);
        snprintf(key, sizeof key, "bb%d.%d divzero", b0->id, idx_of(b0, q1));
        char after[1 << 12]; snprintf(after, sizeof after, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "assert bb%d.%d : ", b0->id, at[2]);
        CHECK(strstr(after, want), "after the assert, u != 0 is a fact:\n%s", after);
        snprintf(key, sizeof key, "bb%d.%d divzero", b0->id, idx_of(b0, q0));
        CHECK(t && !strstr(vc_of(t, key), "assert bb"), "before it, it is none");
        #undef VC
        free(t);
    }
    printf("  asserts: owed where they stand, their condition a fact after them\n");
    // 20. a BOOL condition is a fact as a comparison is: 1 on its branch's first edge, 0 on the
    // other (a branch takes its first target on nonzero, and a bool is 0 or 1), along a guard,
    // inside a join's disjunct, and after an ASSERT of it
    {   IrFunc *f = ir_func_new(&A, nm("g25"), u32t, IR_FUNC_PURE);
        IrValue *p = ir_add_param(f, boolt, nm("p")), *q = ir_add_param(f, boolt, nm("q")), *u = ir_add_param(f, u32t, nm("u"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *s = ir_alloca(f, b0, u32t);
        ir_store(f, b0, s, u);
        ir_set_br_cond(b0, p, b1, b2);
        IrValue *d1 = ir_binop(f, b1, IR_UDIV, ir_const_int(f, b1, 100, u32t), ir_load(f, b1, s, u32t), u32t);   // p = 1 here
        ir_store(f, b1, s, ir_const_int(f, b1, 1, u32t));
        ir_set_br(b1, b3);
        ir_store(f, b2, s, ir_const_int(f, b2, 2, u32t));
        ir_set_br(b2, b3);
        IrValue *ls = ir_load(f, b3, s, u32t);
        int aq = nins(b3); ir_assert(f, b3, q);
        IrValue *d2 = ir_binop(f, b3, IR_UDIV, ir_const_int(f, b3, 100, u32t), ls, u32t);
        IrInstr *a1 = ir_instr(f, IR_ADD, u32t, 2); a1->operands[0] = d1; a1->operands[1] = d2; a1->wrap = IR_WRAP_MODULAR; ir_emit(b3, a1);
        IrValue *d3 = ir_binop(f, b3, IR_UDIV, ir_const_int(f, b3, 7, u32t), ir_load(f, b3, ir_alloca(f, b3, u32t), u32t), u32t);
        (void)d3;
        ir_set_ret(b3, a1->result);
        ir_finalize_cfg(f);
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        char key[48], want[96], vb[1 << 12];
        snprintf(key, sizeof key, "bb%d.%d divzero", b1->id, idx_of(b1, d1));
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "guard bb%d->bb%d : 1*%%v%d = 1", b0->id, b1->id, p->id);
        (void)want;
        // d1's VC names no p: a guard is stated once one of its operands is in the VC
        CHECK(!strstr(vb, "guard"), "a guard about nothing the VC names is not stated:\n%s", vb);
        snprintf(key, sizeof key, "bb%d.%d divzero", b3->id, idx_of(b3, d2));
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "1*%%v%d = 1", p->id);
        CHECK(strstr(vb, "join bb") && strstr(vb, want), "the join's disjunct through the first edge has p = 1:\n%s", vb);
        snprintf(want, sizeof want, "1*%%v%d = 0", p->id);
        CHECK(strstr(vb, want), "and the other p = 0:\n%s", vb);
        snprintf(key, sizeof key, "bb%d.%d assert.0", b3->id, aq);
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "goal 1*%%v%d != 0", q->id);
        CHECK(strstr(vb, want) && !strstr(vb, "assert bb"), "assert(q) owes q != 0, not assuming it:\n%s", vb);
        free(t);
        // after assert(q), a later VC that names q has q = 1
        IrFunc *h = ir_func_new(&A, nm("g26"), u32t, IR_FUNC_PURE);
        IrValue *r = ir_add_param(h, boolt, nm("r"));
        IrBlock *h0 = h->entry;
        int ar = nins(h0); ir_assert(h, h0, r);
        int ar2 = nins(h0); ir_assert(h, h0, r);
        ir_set_ret(h0, ir_const_int(h, h0, 0, u32t));
        ir_finalize_cfg(h);
        t = gen_with(h, NULL, 0, JGEN_ALL, NULL);
        snprintf(key, sizeof key, "bb%d.%d assert.0", h0->id, ar2);
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "assert bb%d.%d : 1*%%v%d = 1", h0->id, ar, r->id);
        CHECK(strstr(vb, want), "after assert(r), r = 1 is a fact:\n%s", vb);
        free(t);
    }
    printf("  a bool condition: 1 on its first edge, 0 on the other, and after an assert of it\n");
    // 27. a subslice a[lo..lo+n] is ELEM_PTR(data, lo) and MAKE_SLICE(that, n): its start and its
    // end are owed AT the MAKE_SLICE (where VRA checks the pair), never at the ELEM_PTR, which is
    // not an access; and objsize, n <= MAX_LEN(u8), which every reader of a length is given
    {   IrFunc *f = ir_func_new(&A, nm("g27"), u64t, IR_FUNC_PURE);
        IrValue *a = ir_add_param(f, slice_of(u8t), nm("a")), *lo = ir_add_param(f, u64t, nm("lo"));
        IrValue *n = ir_add_param(f, u64t, nm("n"));
        IrBlock *b0 = f->entry;
        IrValue *d = ir_slice_data(f, b0, a, u8t);
        IrValue *p = ir_elem_ptr(f, b0, d, lo, u8t);
        int ms = nins(b0); IrValue *s = ir_make_slice(f, b0, p, n, u8t);
        ir_set_ret(b0, ir_slice_len(f, b0, s));
        char *t = gen_text(f);
        char key[64], want[128], vb[1 << 14];
        CHECK(t && count(t, "bounds.") == 2, "one start and one end, both at the MAKE_SLICE:\n%s", t ? t : "");
        snprintf(key, sizeof key, "bb%d.%d bounds.lo 0", b0->id, ms);
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "goal -1*%%v%d <= 0", lo->id);
        CHECK(strstr(vb, want), "the start: 0 <= lo:\n%s", vb);
        snprintf(key, sizeof key, "bb%d.%d bounds.hi 0", b0->id, ms);
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "goal 1*%%v%d + 1*%%v%d + -1*len(%%v%d) <= 0", lo->id, n->id, a->id);
        CHECK(strstr(vb, want), "the end: lo + n <= len(a):\n%s", vb);
        snprintf(key, sizeof key, "bb%d.%d objsize 0", b0->id, ms);
        snprintf(vb, sizeof vb, "%s", t ? vc_of(t, key) : "");
        snprintf(want, sizeof want, "goal 1*%%v%d <= 9223372036854775807", n->id);
        CHECK(strstr(vb, want), "objsize: n <= MAX_LEN(u8):\n%s", vb);
        free(t);
        printf("  a subslice: its start and end at the MAKE_SLICE, and objsize\n");
    }
    // 28. any other data must be an object the lowering made with the slice's length: a string
    // literal (its NUL counted), a local array, a dynamic local of exactly n elements, each owing
    // objsize only; data the generator cannot bound is `outside`; in `unsafe` only objsize, never
    // waived. A dynamic local of integers has the probed-allocation premise (I.121); a struct's, not.
    for (int k = 0; k < 7; k++) {
        IrFunc *f = ir_func_new(&A, nm("g28"), u64t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u64t, nm("n")), *raw = ir_add_param(f, ir_type_new(&A, IRT_PTR), nm("p"));
        IrBlock *b0 = f->entry;
        IrType *arr = ir_type_new(&A, IRT_ARRAY); arr->elem = u8t; arr->array_len = 4;
        IrType *st = ir_type_new(&A, IRT_STRUCT);
        IrValue *data, *len;
        switch (k) {
            case 0: data = ir_str_const(f, b0, "abc", 3); len = ir_const_int(f, b0, 4, u64t); break;     // with the NUL
            case 1: data = ir_str_const(f, b0, "abc", 3); len = ir_const_int(f, b0, 5, u64t); break;     // past it
            case 2: data = ir_alloca_array(f, b0, arr); len = ir_const_int(f, b0, 4, u64t); break;
            case 3: data = ir_alloca_array(f, b0, arr); len = ir_const_int(f, b0, 5, u64t); break;
            case 4: data = ir_alloca_dyn(f, b0, u8t, n); len = n; break;
            case 5: data = ir_alloca_dyn(f, b0, st, n); len = n; break;
            default: data = raw; len = n; break;
        }
        IrValue *s = ir_make_slice(f, b0, data, len, k == 5 ? st : u8t);
        if (k == 6) b0->instrs_tail->unchecked = true;
        ir_set_ret(b0, ir_slice_len(f, b0, s));
        char *t = gen_text(f);
        bool out = t && strstr(t, "outside a slice over data whose length the generator does not know");
        CHECK(t && count(t, "objsize 0\n") == 1, "objsize at every MAKE_SLICE (shape %d):\n%s", k, t ? t : "");
        CHECK(t && !strstr(t, "bounds."), "no start or end to owe (shape %d):\n%s", k, t ? t : "");
        CHECK(out == (k == 1 || k == 3), "outside exactly where the data cannot hold n (shape %d):\n%s", k, t ? t : "");
        bool prem = t && strstr(vc_of(t, "objsize"), "premise object");
        CHECK(prem == (k == 4), "the probed-allocation premise only for a dynamic local of integers (shape %d):\n%s", k, t ? t : "");
        free(t);
        // the raw pointer outside `unsafe`: outside
        if (k == 6) {
            IrFunc *h = ir_func_new(&A, nm("g28r"), u64t, IR_FUNC_PURE);
            IrValue *hn = ir_add_param(h, u64t, nm("n")), *hp = ir_add_param(h, ir_type_new(&A, IRT_PTR), nm("p"));
            IrValue *hs = ir_make_slice(h, h->entry, hp, hn, u8t);
            ir_set_ret(h->entry, ir_slice_len(h, h->entry, hs));
            t = gen_text(h);
            CHECK(t && strstr(t, "outside a slice over data whose length the generator does not know"), "a raw pointer's slice outside `unsafe` is outside:\n%s", t ? t : "");
            free(t);
        }
    }
    printf("  a slice over a literal, an array, a dynamic local, a raw pointer, in unsafe\n");
    // 29. what a division's result owes, on the interpreter's semantics (ii_settle, I.185)
    {   IrType *i48t = ir_type_int(&A, 48, true), *i16t = ir_type_int(&A, 16, true);
        struct { IrOp op; IrType *xt, *yt, *rt; IrWrapMode w; const char *want; } dc[] = {
            { IR_SDIV, i32t, i32t, i32t, IR_WRAP_CHECK,   "0:-2147483648" },        // i32 / i32 at i32
            { IR_SDIV, i32t, i32t, i64t, IR_WRAP_CHECK,   "" },                     // widened: its narrowing owes
            { IR_SDIV, i64t, u64t, i64t, IR_WRAP_CHECK,   "0:-9223372036854775808" },// as VRA states it
            { IR_SDIV, i64t, i64t, i64t, IR_WRAP_MODULAR, "" },                     // `/%` defines it
            { IR_SREM, i32t, i32t, i32t, IR_WRAP_CHECK,   "0:-2147483648" },        // C traps
            { IR_SREM, i48t, i48t, i48t, IR_WRAP_CHECK,   "" },                     // computed in a wider C type
            { IR_SREM, i16t, i16t, i16t, IR_WRAP_CHECK,   "" },
            { IR_UDIV, i64t, u64t, u64t, IR_WRAP_CHECK,   "1" },                    // `100 /% x`: the literal an i64
            { IR_UDIV, u64t, i64t, u64t, IR_WRAP_CHECK,   "3" },                    // `(lo + hi) / 2`
            { IR_SDIV, i64t, i64t, i32t, IR_WRAP_CHECK,   "0:-2147483648 1 2" },    // a dividend wider than the result
        };
        for (int k = 0; k < (int)(sizeof dc / sizeof dc[0]); k++) {
            IrFunc *f = ir_func_new(&A, nm("g29"), dc[k].rt, IR_FUNC_PURE);
            IrValue *x = ir_add_param(f, dc[k].xt, nm("x")), *y = ir_add_param(f, dc[k].yt, nm("y"));
            IrBlock *b0 = f->entry;
            IrValue *q = ir_binop(f, b0, dc[k].op, x, y, dc[k].rt);
            b0->instrs_tail->wrap = dc[k].w;
            ir_set_ret(b0, q);
            char *t = gen_text(f);
            char got[96] = "", one[40];
            for (int o = 0; o < 4; o++) {
                char key[24]; snprintf(key, sizeof key, "divovf %d", o);
                const char *v = t ? vc_of(t, key) : "";
                if (!*v) continue;
                const char *pm = strstr(v, "premise divovf");
                const char *eq = pm ? strstr(pm, " = ") : NULL;
                if (o == 0) snprintf(one, sizeof one, "%s0:%lld", *got ? " " : "", eq ? atoll(eq + 3) : 0LL);
                else snprintf(one, sizeof one, "%s%d", *got ? " " : "", o);
                strcat(got, one);
            }
            CHECK(!strcmp(got, dc[k].want), "division %d owes [%s], not [%s]:\n%s", k, dc[k].want, got, t ? t : "");
            if (k == 8) {
                char want[64]; snprintf(want, sizeof want, "goal -1*%%v%d <= 0", y->id);
                CHECK(t && strstr(vc_of(t, "divovf 3"), want), "a signed divisor into an unsigned quotient: y >= 0:\n%s", t ? t : "");
            }
            free(t);
        }
    }
    printf("  a division's result: MIN / -1 at its own width, MIN %% -1 where C traps, the dividend's range, the divisor's sign\n");
    // 30. a VECTOR lane by lane (spec 07, I.202): a scalar shift amount is owed in [0, w-1] for
    // w-bit lanes and a scalar divisor non-zero, with no narrowing and no divovf (lanes wrap); an
    // amount or a divisor given per lane is `outside`
    {   IrType *v = ir_type_new(&A, IRT_VECTOR); v->elem = u32t; v->array_len = 4;
        IrType *vi = ir_type_new(&A, IRT_VECTOR); vi->elem = i32t; vi->array_len = 4;
        for (int k = 0; k < 4; k++) {
            IrFunc *f = ir_func_new(&A, nm("g30"), k < 2 ? v : vi, IR_FUNC_PURE);
            IrValue *a = ir_add_param(f, k < 2 ? v : vi, nm("a"));
            IrValue *y = ir_add_param(f, k == 1 ? v : k == 3 ? vi : k == 0 ? u32t : i32t, nm("y"));
            IrBlock *b0 = f->entry;
            IrValue *r = ir_binop(f, b0, k < 2 ? IR_SHL : IR_SDIV, a, y, k < 2 ? v : vi);
            ir_set_ret(b0, r);
            char *t = gen_text(f), want[96];
            if (k == 0) {
                snprintf(want, sizeof want, "goal 1*%%v%d <= 31", y->id);
                CHECK(t && strstr(vc_of(t, "shift 1"), want), "a lane's amount: y <= 31 for u32 lanes:\n%s", t ? t : "");
                snprintf(want, sizeof want, "goal -1*%%v%d <= 0", y->id);
                CHECK(t && strstr(vc_of(t, "shift 0"), want), "and 0 <= y:\n%s", t ? t : "");
                CHECK(t && !strstr(t, "narrow."), "lanes wrap: no narrowing:\n%s", t ? t : "");
            }
            if (k == 2) {
                snprintf(want, sizeof want, "goal 1*%%v%d != 0", y->id);
                CHECK(t && strstr(vc_of(t, "divzero"), want), "a scalar divisor of lanes: y != 0:\n%s", t ? t : "");
                CHECK(t && !strstr(t, "divovf"), "lanes wrap: no divovf:\n%s", t ? t : "");
            }
            if (k == 1) CHECK(t && strstr(t, "outside a shift amount given per lane") && !strstr(t, "vc "), "a per-lane amount is outside:\n%s", t ? t : "");
            if (k == 3) CHECK(t && strstr(t, "outside a divisor given per lane") && !strstr(t, "vc "), "a per-lane divisor is outside:\n%s", t ? t : "");
            free(t);
        }
    }
    printf("  a vector: a scalar amount and a scalar divisor owed, per-lane ones outside\n");
    // 31. a loop's termination: `i = 0; while i < n { i = i + 1 }`. With no ranking claim it is
    // `outside` (a function that may diverge owes nothing); with f = n - i, on the back edge
    // `rank.fall` (it falls by 1) and `rank.bound` (it is >= 0 where the trip is taken); a tuple
    // whose second component falls also owes the first `rank.keep`; a claim the generator cannot
    // state, or one that leaves the back edge out, refuses the function
    {   IrFunc *f = ir_func_new(&A, nm("g31"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u32t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u32t);
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u32t)); ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, i, u32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, li, n), b2, b3);
        IrValue *li2 = ir_load(f, b2, i, u32t);
        ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li2, ir_const_int(f, b2, 1, u32t), u32t));
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_const_int(f, b3, 0, u32t));
        ir_finalize_cfg(f);
        CHECK(b1->is_loop_header, "b1 heads the loop");
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && strstr(t, "outside no ranking function proposed for this loop") && !strstr(t, "rank."),
              "no ranking claim: the loop's termination is outside:\n%s", t ? t : "");
        free(t);
        f->may_diverge = true;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && !strstr(t, "no ranking function"), "a function that may diverge owes no termination:\n%s", t ? t : "");
        free(t);
        f->may_diverge = false;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.ncomp = 1; r.nt = 2; r.key[0] = i->id; r.key[1] = n->id;
        r.coef[0][0] = -1; r.coef[0][1] = 1; r.c0[0] = 0;                 // f = n - i
        r.nedge = 1; r.from[0] = b2->id; r.fall[0] = 0;
        gen_rk = &r; gen_nrk = 1;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        char key[48], hv[48], want[160];
        snprintf(hv, sizeof hv, "%%s%d@head.bb%d", i->id, b1->id);
        snprintf(key, sizeof key, "bb%d.%d rank.fall 0", b2->id, nins(b2));
        const char *vf = t ? vc_of(t, key) : "";
        const char *g = strstr(vf, "goal ");
        CHECK(g && strstr(g, hv) && strstr(g, "<= -1") && !strstr(g, "%v"), "rank.fall: n - i falls by 1, n cancelling:\n%s", vf);
        snprintf(key, sizeof key, "bb%d.%d rank.bound 0", b2->id, nins(b2));
        const char *vb = t ? vc_of(t, key) : "";
        g = strstr(vb, "goal ");
        snprintf(want, sizeof want, "-1*%%v%d", n->id);
        CHECK(g && strstr(g, hv) && strstr(g, want) && strstr(g, "<= 0"), "rank.bound: i - n <= 0 where the trip is taken:\n%s", vb);
        CHECK(strstr(vb, "guard bb"), "the bound has the edge's guard:\n%s", vb);
        CHECK(t && !strstr(t, "no ranking function") && !strstr(t, "rank.keep"), "one component: no keep, no outside:\n%s", t ? t : "");
        free(t);
        // a tuple (0, n - i): the first keeps, the second falls
        JRank r2 = r; r2.ncomp = 2;
        r2.coef[0][0] = 0; r2.coef[0][1] = 0; r2.c0[0] = 0;
        r2.coef[1][0] = -1; r2.coef[1][1] = 1; r2.c0[1] = 0; r2.fall[0] = 1;
        gen_rk = &r2;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && count(t, "rank.keep 0") == 1 && count(t, "rank.fall 1") == 1 && count(t, "rank.bound 1") == 1 && !strstr(t, "rank.fall 0"),
              "a tuple falling at 1: keep 0, fall 1, bound 1:\n%s", t ? t : "");
        free(t);
        // refused: the back edge left out, a component out of range, an edge that is no back
        // edge, a term made in the loop, two claims at one header, a claim naming no function
        struct { JRank r; int n; const char *why; } bad[7];
        for (int k = 0; k < 7; k++) { bad[k].r = r; bad[k].n = 1; }
        bad[0].r.nedge = 0; bad[0].why = "a back edge the ranking claim does not name";
        bad[1].r.fall[0] = 1; bad[1].why = "falling component out of range";
        bad[2].r.from[0] = b0->id; bad[2].why = "no back edge of its loop";
        bad[3].r.key[1] = li2->id; bad[3].why = "not a cell or a value fixed at its header";
        bad[4].n = 2; bad[4].why = "two ranking claims at one loop header";
        bad[5].r.fn = NULL; bad[5].why = "a ranking claim with no function";
        bad[6].r.nle = 1; bad[6].why = "a loop's ranking claim lists calls";
        for (int k = 0; k < 7; k++) {
            JRank two[2] = { bad[k].r, bad[k].r };
            gen_rk = two; gen_nrk = bad[k].n;
            char why[200] = "";
            t = gen_with(f, NULL, 0, JGEN_ALL, why);
            CHECK(!t && strstr(why, bad[k].why), "ranking claim %d is refused (%s): got \"%s\"", k, bad[k].why, why);
            free(t);
        }
        gen_rk = NULL; gen_nrk = 0;
        // 32. the paths a proposer is offered: the entry edge (no `pre`) and the back edge, each
        // with the header's version of i before and after, and i's ENTRY value (i is written in the
        // loop: a fixed pair, pre and post one variable on the back edge, on the entry edge i's
        // value there); a block that heads no loop has none
        {   memset(region, 0x5a, sizeof region);                                // the region is not zeroed
            JMem m = jmem_over(region, sizeof region);
            JPaths P; char err[200];
            bool ok = jgen_paths(&m, f, gen_mod, NULL, 0, b1->id, &P, err, sizeof err);
            CHECK(ok && !P.refused && P.n == 2, "two edges into the header (got %d, %s)", ok ? P.n : -1, ok ? (P.refused ? P.refused : "") : err);
            for (int32_t k = 0; ok && k < P.n; k++) {
                const JPath *p = &P.p[k];
                CHECK(p->at == -1 && !p->callee && !p->entry && p->nentry == 0, "a loop's path names no call (path %d)", k);
                CHECK(p->npairs == 2 && p->pairs[0].key == i->id && p->pairs[0].tk == JTERM_VALUE && p->pairs[1].key == i->id &&
                      p->pairs[1].tk == JTERM_ENTRY, "two pairs, i's and i's entry value (path %d)", k);
                if (p->npairs != 2) continue;
                const JPathPair *q = &p->pairs[0], *qe = &p->pairs[1];
                if (p->back) CHECK(qe->pre >= 0 && qe->pre == qe->post && qe->pre != q->pre && qe->pre != q->post, "the back edge: i's entry value fixed, neither i before nor after");
                else CHECK(qe->pre == -1 && qe->post == q->post, "the entry edge: i's entry value is i's value there");
                if (p->back) {
                    CHECK(p->from == b2->id && q->pre >= 0 && q->post >= 0 && q->pre != q->post, "the back edge: i before and after differ");
                    CHECK(q->pre < p->vc->nvars && strstr(p->vc->vars[q->pre], "@head"), "pre is the header's version");
                    bool guard = false;
                    for (int32_t h = 0; h < p->vc->nhyps; h++) if (strstr(p->vc->hyps[h].origin, "guard")) guard = true;
                    CHECK(guard, "the back edge's path has the header's guard");
                } else CHECK(p->from == b0->id && q->pre == -1 && q->post >= 0, "the entry edge: no pre, i's value at entry");
            }
            m = jmem_over(region, sizeof region);
            ok = jgen_paths(&m, f, gen_mod, NULL, 0, b2->id, &P, err, sizeof err);
            CHECK(ok && P.refused && strstr(P.refused, "not a loop header"), "b2 heads no loop");
        }
    }
    printf("  a loop's termination: outside without a ranking claim, keep, fall and bound with one, and the paths\n");
    // 33. a slice's LENGTH as a term (JTERM_LEN): `while i < xs.len { i = i + 1 }`. A ranking
    // claim len(xs) - i, len(xs) fixed at the header (pre == post, cancelling in the fall), its
    // bound read from the guard; a claim i - len(xs) <= 0 at the header, stated and owed; a slice
    // cell the loop rewrites is a pair of the paths, by its length; refused: a length of an
    // integer, an unknown kind, a return claim over a length
    {   IrFunc *f = ir_func_new(&A, nm("g33"), u32t, IR_FUNC_PURE);
        IrValue *xs = ir_add_param(f, slice_of(u8t), nm("xs")), *ys = ir_add_param(f, slice_of(u8t), nm("ys"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u64t), *sc = ir_alloca(f, b0, slice_of(u8t));
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u64t)); ir_store(f, b0, sc, xs); ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, i, u64t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, li, ir_slice_len(f, b1, xs)), b2, b3);
        IrValue *li2 = ir_load(f, b2, i, u64t);
        ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li2, ir_const_int(f, b2, 1, u64t), u64t));
        IrValue *ln = ir_slice_len(f, b2, ir_load(f, b2, sc, slice_of(u8t)));   // the header version's length
        IrValue *hundred = ir_const_int(f, b2, 100, u64t);
        int dz = nins(b2); ir_binop(f, b2, IR_UDIV, hundred, ln, u64t);
        ir_store(f, b2, sc, ys);                                             // the slice cell, rewritten
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_const_int(f, b3, 0, u32t));
        ir_finalize_cfg(f);
        char want[96], key[48];
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.ncomp = 1; r.nt = 2;
        r.key[0] = i->id; r.key[1] = xs->id; r.tk[1] = JTERM_LEN; r.coef[0][0] = -1; r.coef[0][1] = 1;   // len(xs) - i
        r.nedge = 1; r.from[0] = b2->id;
        gen_rk = &r; gen_nrk = 1;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        snprintf(key, sizeof key, "bb%d.%d rank.bound 0", b2->id, nins(b2));
        const char *vb = t ? vc_of(t, key) : "", *g = strstr(vb, "goal ");
        snprintf(want, sizeof want, "-1*len(%%v%d)", xs->id);
        CHECK(g && strstr(g, want) && strstr(g, "<= 0") && strstr(vb, "guard bb"), "the bound: i - len(xs) <= 0, under the guard:\n%s", vb);
        snprintf(key, sizeof key, "bb%d.%d rank.fall 0", b2->id, nins(b2));
        const char *vf = t ? vc_of(t, key) : "";
        g = strstr(vf, "goal ");
        CHECK(g && !strstr(g, "len(") && strstr(g, "<= -1"), "the fall: len(xs) cancels, i rises by 1:\n%s", vf);
        free(t);
        gen_rk = NULL; gen_nrk = 0;
        JClaim c; memset(&c, 0, sizeof c);
        c.blk = b1->id; c.fn = f; c.nt = 2; c.key[0] = i->id; c.coef[0] = 1; c.key[1] = xs->id; c.tk[1] = JTERM_LEN; c.coef[1] = -1;
        t = gen_with(f, &c, 1, JGEN_CLAIMS, NULL);
        snprintf(key, sizeof key, "bb%d.%d inv.keep 0", b2->id, nins(b2));
        const char *vk = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "-1*len(%%v%d)", xs->id);
        CHECK(strstr(vk, want) && strstr(vk, "claim bb"), "the claim i <= len(xs): owed on the back edge, a fact at the previous entry:\n%s", vk);
        free(t);
        {   JMem m = jmem_over(region, sizeof region);
            JPaths P; char err[200];
            bool ok = jgen_paths(&m, f, gen_mod, NULL, 0, b1->id, &P, err, sizeof err);
            int lens = 0;
            for (int32_t k = 0; ok && k < P.n; k++)
                for (int32_t q = 0; q < P.p[k].npairs; q++)
                    if (P.p[k].pairs[q].tk == JTERM_LEN && P.p[k].pairs[q].key == sc->id) {
                        lens++;
                        const JVc *v = P.p[k].vc; const JPathPair *pp = &P.p[k].pairs[q];
                        CHECK(!strncmp(v->vars[pp->post], "len(", 4) && (!P.p[k].back || !strncmp(v->vars[pp->pre], "len(", 4)),
                              "a slice cell's pair is its length (%s)", v->vars[pp->post]);
                    }
            CHECK(ok && lens == 2, "the rewritten slice cell is a pair on both edges, by its length (got %d)", lens);
        }
        // a claim about the slice cell's length alone, len(sc) >= 1: stated where only that length is
        // named (the division's divisor), so 100 / sc.len is owed with it
        JClaim c3; memset(&c3, 0, sizeof c3);
        c3.blk = b1->id; c3.fn = f; c3.nt = 1; c3.key[0] = sc->id; c3.tk[0] = JTERM_LEN; c3.coef[0] = -1; c3.rhs = -1;
        t = gen_with(f, &c3, 1, JGEN_ALL, NULL);
        snprintf(key, sizeof key, "bb%d.%d divzero 0", b2->id, dz);
        const char *vd = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "claim bb%d #0 : -1*len(%%s%d@head.bb%d) <= -1", b1->id, sc->id, b1->id);
        CHECK(strstr(vd, want), "the claim about len(sc) is a fact where its length alone is named:\n%s", vd);
        free(t);
        // what a proposer may name at the header, by kind: xs's length (fixed), sc's length (made
        // there), i (made there); xs as a value is no integer
        {   JMem m = jmem_over(region, sizeof region);
            int32_t keys[4] = { xs->id, sc->id, i->id, xs->id };
            uint8_t tks[4] = { JTERM_LEN, JTERM_LEN, JTERM_VALUE, JTERM_VALUE }, kind[4]; char err[200];
            bool ok = jgen_term_kinds(&m, f, gen_mod, b1->id, keys, tks, 4, kind, err, sizeof err);
            CHECK(ok && kind[0] == 1 && kind[1] == 2 && kind[2] == 2 && kind[3] == 0,
                  "term kinds at the header: len(xs) 1, len(sc) 2, i 2, xs 0 (got %d %d %d %d)", kind[0], kind[1], kind[2], kind[3]);
        }
        struct { JClaim c; const char *why; } bad[3];
        for (int k = 0; k < 3; k++) bad[k].c = c;
        bad[0].c.tk[0] = JTERM_LEN; bad[0].why = "not a cell or a value fixed at its block";       // i is no slice
        bad[1].c.tk[0] = 5; bad[1].why = "not a cell or a value fixed at its block";               // no such kind, on an integer
        memset(&bad[2].c, 0, sizeof bad[2].c);
        bad[2].c.blk = JCLAIM_RET; bad[2].c.fn = f; bad[2].c.nt = 2; bad[2].c.key[0] = JKEY_RET; bad[2].c.coef[0] = 1;
        bad[2].c.key[1] = xs->id; bad[2].c.tk[1] = JTERM_LEN; bad[2].c.coef[1] = -1; bad[2].why = "a return claim over a length";
        for (int k = 0; k < 3; k++) {
            char why[200] = "";
            t = gen_with(f, &bad[k].c, 1, JGEN_ALL, why);
            CHECK(!t && strstr(why, bad[k].why), "length claim %d is refused (%s): got \"%s\"", k, bad[k].why, why);
            free(t);
        }
    }
    printf("  a slice's length as a term: in a ranking claim, in a claim, as a pair, and refused where it is none\n");
    // 34. a WRAPPING add or sub is the exact result less k*2^w, k one of the few the operands'
    // type ranges allow (the interpreter wraps once): u8 `+%` two cases, i8 three, u8 `-%` two
    // (an offset of +256), u64 at 2^64; unsigned `+` inside `unsafe` wraps alike; signed `+` inside
    // `unsafe` is undefined past its range, and gets nothing. Read where the result is a divisor.
    {   IrType *i8t = ir_type_int(&A, 8, true);
        struct { IrOp op; IrType *t; IrWrapMode w; bool unsafe_; int ncase; const char *has; } wc[] = {
            { IR_ADD, u8t,  IR_WRAP_MODULAR, false, 2, "= -256" },
            { IR_ADD, i8t,  IR_WRAP_MODULAR, false, 3, "= 256" },
            { IR_SUB, u8t,  IR_WRAP_MODULAR, false, 2, "= 256" },
            { IR_ADD, u64t, IR_WRAP_MODULAR, false, 2, "= -18446744073709551616" },
            { IR_ADD, u8t,  IR_WRAP_CHECK,   true,  2, "= -256" },
            { IR_ADD, i8t,  IR_WRAP_CHECK,   true,  0, "" },
        };
        for (int k = 0; k < (int)(sizeof wc / sizeof wc[0]); k++) {
            IrFunc *f = ir_func_new(&A, nm("g34"), wc[k].t, IR_FUNC_PURE);
            IrValue *x = ir_add_param(f, wc[k].t, nm("x")), *y = ir_add_param(f, wc[k].t, nm("y"));
            IrBlock *b0 = f->entry;
            IrValue *r = ir_binop(f, b0, wc[k].op, x, y, wc[k].t);
            b0->instrs_tail->wrap = wc[k].w; b0->instrs_tail->unchecked = wc[k].unsafe_;
            IrValue *hundred = ir_const_int(f, b0, 100, wc[k].t);
            ir_set_ret(b0, ir_binop(f, b0, wc[k].t->is_signed ? IR_SDIV : IR_UDIV, hundred, r, wc[k].t));
            char *t = gen_text(f);
            const char *vd = t ? vc_of(t, "divzero") : "";
            const char *h = strstr(vd, ".wrap : or {");
            int bars = 0;
            for (const char *q = h; q && *q && *q != '\n'; q++) if (*q == '|') bars++;
            if (wc[k].ncase) CHECK(h && bars == wc[k].ncase - 1 && strstr(h, wc[k].has), "wrap %d: %d cases, among them `%s`:\n%s", k, wc[k].ncase, wc[k].has, vd);
            if (wc[k].ncase && h) {                                  // r - x - y (add) or r - x + y (sub), in every case
                char line[512]; size_t n = 0;
                while (h[n] && h[n] != '\n' && n + 1 < sizeof line) { line[n] = h[n]; n++; }
                line[n] = 0;
                char mx[32], my[32]; snprintf(mx, sizeof mx, "-1*%%v%d", x->id); snprintf(my, sizeof my, "-1*%%v%d", y->id);
                int cx = count(line, mx), cy = count(line, my);
                CHECK(cx == wc[k].ncase && cy == (wc[k].op == IR_ADD ? wc[k].ncase : 0), "wrap %d: x and y with their signs in each case: %s", k, line);
            }
            else CHECK(!strstr(vd, ".wrap") && !strstr(vd, " add :"), "wrap %d: signed `+` in `unsafe` states nothing:\n%s", k, vd);
            free(t);
        }
    }
    // ...and a constant operand's range is its value: `i +% 1` on a u8 lowers its 1 as an i32
    // literal, whose type range alone leaves thousands of k (Main Compiler Work)
    {   IrType *i32c = ir_type_int(&A, 32, true);
        IrFunc *f = ir_func_new(&A, nm("g34c"), u8t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, u8t, nm("x"));
        IrBlock *b0 = f->entry;
        IrValue *one = ir_const_int(f, b0, 1, i32c);
        IrValue *r = ir_binop(f, b0, IR_ADD, x, one, u8t);
        b0->instrs_tail->wrap = IR_WRAP_MODULAR;
        IrValue *hundred = ir_const_int(f, b0, 100, u8t);
        ir_set_ret(b0, ir_binop(f, b0, IR_UDIV, hundred, r, u8t));
        char *t = gen_text(f);
        const char *vd = t ? vc_of(t, "divzero") : "";
        const char *h = strstr(vd, ".wrap : or {");
        int bars = 0;
        for (const char *q = h; q && *q && *q != '\n'; q++) if (*q == '|') bars++;
        CHECK(h && bars == 1 && strstr(h, "= -256"), "u8 +%% an i32 literal 1: two cases, by the constant's value:\n%s", vd);
        free(t);
    }
    printf("  a wrapping add or sub: the exact result less k*2^w, in its few cases\n");
    // 35. a WRITTEN measure (`while i < n decreasing m`, D-44) is what a loop owes: `measure.fall` and
    // `measure.bound` on its back edge, m read as a sum over what the trip starts from; a ranking
    // claim proposed for it is not read; owed in a function that may diverge too; a measure that is
    // no sum is outside; a constant one cannot fall; a slice cell stored before the loop is one
    // length in the guard and in the measure
    {   IrFunc *f = ir_func_new(&A, nm("g35"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u64t, nm("n")), *xs = ir_add_param(f, slice_of(u8t), nm("xs"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u64t), *sc = ir_alloca(f, b0, slice_of(u8t));
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u64t)); ir_store(f, b0, sc, xs); ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, i, u64t);
        IrValue *sl = ir_slice_len(f, b1, ir_load(f, b1, sc, slice_of(u8t)));
        IrValue *g1 = ir_icmp(f, b1, IR_CMP_ULT, li, n), *g2 = ir_icmp(f, b1, IR_CMP_ULT, li, sl);
        IrValue *both = ir_binop(f, b1, IR_AND, g1, g2, boolt);
        ir_set_br_cond(b1, both, b2, b3);
        IrValue *li2 = ir_load(f, b2, i, u64t);
        IrValue *m_n = ir_binop(f, b2, IR_SUB, n, li2, u64t);                // n - i
        IrValue *m_s = ir_binop(f, b2, IR_SUB, ir_slice_len(f, b2, ir_load(f, b2, sc, slice_of(u8t))), li2, u64t);   // s.len - i
        IrValue *m_c = ir_const_int(f, b2, 5, u64t);                          // 5
        IrValue *m_x = ir_binop(f, b2, IR_MUL, n, li2, u64t);                // n * i: no sum
        IrValue *m_k = ir_binop(f, b2, IR_SUB, ir_const_int(f, b2, 100, u64t), li2, u64t);   // 100 - i
        // (i * (2^64 - 1)) * (2^64 - 1): the second product is 2^128 - 2^65 + 1, past __int128, and
        // wrapped it would be -(2^65 - 1), a coefficient within every bound: refused, not wrapped
        IrValue *umax = ir_const_int(f, b2, -1, u64t);
        IrValue *m_big = ir_binop(f, b2, IR_MUL, ir_binop(f, b2, IR_MUL, li2, umax, u64t), umax, u64t);
        ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li2, ir_const_int(f, b2, 1, u64t), u64t));
        IrValue *m_late = ir_load(f, b2, i, u64t);                            // i after the trip's store
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_const_int(f, b3, 0, u32t));
        ir_finalize_cfg(f);
        (void)both;
        char key[48], want[96];
        snprintf(key, sizeof key, "bb%d.%d measure.", b2->id, nins(b2));
        b1->has_measure = true;
        // n - i, with a ranking claim proposed that would rise: not read
        JRank bad; memset(&bad, 0, sizeof bad);
        bad.blk = b1->id; bad.fn = f; bad.ncomp = 1; bad.nt = 1; bad.key[0] = i->id; bad.coef[0][0] = 1; bad.nedge = 1; bad.from[0] = b2->id;
        gen_rk = &bad; gen_nrk = 1;
        b1->measure_val = m_n;
        char *t = gen_text(f);
        CHECK(t && count(t, key) == 2 && !strstr(t, "rank.") && !strstr(t, "outside a written measure") && !strstr(t, "no ranking function"),
              "n - i: measure.fall and .bound, the proposal not read:\n%s", t ? t : "");
        snprintf(want, sizeof want, "bb%d.%d measure.fall", b2->id, nins(b2));
        const char *vf = t ? vc_of(t, want) : "", *g = strstr(vf, "goal ");
        CHECK(g && strstr(g, "<= -1") && !strstr(g, "%v0"), "the fall: i rises by 1, n cancelling:\n%s", vf);
        free(t);
        gen_rk = NULL; gen_nrk = 0;
        // in a function that may diverge, a written measure is owed all the same (D-44)
        f->may_diverge = true;
        t = gen_text(f);
        CHECK(t && count(t, key) == 2, "may diverge, the written measure still owed:\n%s", t ? t : "");
        free(t);
        f->may_diverge = false;
        // s.len - i: the guard's length and the measure's are one variable
        b1->measure_val = m_s;
        t = gen_text(f);
        snprintf(want, sizeof want, "bb%d.%d measure.bound", b2->id, nins(b2));
        const char *vb = t ? vc_of(t, want) : "";
        g = strstr(vb, "goal ");
        snprintf(want, sizeof want, "-1*len(%%v%d)", xs->id);
        CHECK(g && strstr(g, want), "s.len - i: the stored slice's length, as the guard reads it:\n%s", vb);
        free(t);
        // 5: no terms, it cannot fall
        b1->measure_val = m_c;
        t = gen_text(f);
        snprintf(want, sizeof want, "bb%d.%d measure.fall", b2->id, nins(b2));
        vf = t ? vc_of(t, want) : "";
        CHECK(strstr(vf, "goal 0 <= -1") || strstr(vf, "goal  <= -1"), "a constant measure owes 0 <= -1:\n%s", vf);
        free(t);
        // 100 - i: the bound keeps the constant, i <= 100
        b1->measure_val = m_k;
        t = gen_text(f);
        snprintf(want, sizeof want, "bb%d.%d measure.bound", b2->id, nins(b2));
        vb = t ? vc_of(t, want) : "";
        g = strstr(vb, "goal ");
        CHECK(g && strstr(g, "<= 100") && !strstr(g, "%v"), "100 - i: the bound is i <= 100:\n%s", vb);
        free(t);
        // a load after the trip's store reads no version the trip starts from: outside
        b1->measure_val = m_late;
        t = gen_text(f);
        CHECK(t && strstr(t, "outside a written measure the generator cannot read as a sum"), "a measure read after a store is outside:\n%s", t ? t : "");
        free(t);
        // coefficients past 2^100 are refused before any product overflows: outside (Main Compiler Work)
        b1->measure_val = m_big;
        t = gen_text(f);
        CHECK(t && strstr(t, "outside a written measure the generator cannot read as a sum"), "a measure whose coefficients pass 2^100 is outside:\n%s", t ? t : "");
        free(t);
        // n * i: no sum, outside
        b1->measure_val = m_x;
        t = gen_text(f);
        CHECK(t && strstr(t, "outside a written measure the generator cannot read as a sum") && !strstr(t, "measure."),
              "n * i is outside:\n%s", t ? t : "");
        free(t);
        b1->has_measure = false; b1->measure_val = NULL;
    }
    printf("  a written measure: owed as written, the proposal not read, outside where it is no sum\n");
    // 36. a RECURSION's termination, at the calls that may close a cycle (callgraph.h): f(n) calls
    // f(n - 1) past `n == 0`. With no measure, outside at the call; with the measure n, `rec.fall`
    // ((n - 1) - n <= -1) and `rec.bound` (n >= 0); nothing where the function may diverge unless it
    // wrote `decreasing`; refused: a second measure, a term no parameter, a group of kind 3; p and q
    // calling each other owe at each call, outside where either has no measure; a call through a
    // pointer that may close the cycle is outside; the paths a proposer is offered
    {   IrFunc *f = ir_func_new(&A, nm("f36"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u32t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_EQ, n, ir_const_int(f, b0, 0, u32t)), b1, b2);
        ir_set_ret(b1, ir_const_int(f, b1, 0, u32t));
        IrValue *m = ir_binop(f, b2, IR_SUB, n, ir_const_int(f, b2, 1, u32t), u32t);
        (void)mk_call(f, b2, "h36", n, u32t);                                // a helper off the cycle: owes nothing
        int ci = nins(b2);
        IrValue *r = mk_call(f, b2, "f36", m, u32t);
        ir_set_ret(b2, r);
        ir_finalize_cfg(f);
        IrFunc *hh = ir_func_new(&A, nm("h36"), u32t, IR_FUNC_PURE);
        IrValue *hp = ir_add_param(hh, u32t, nm("x"));
        ir_set_ret(hh->entry, hp);
        ir_finalize_cfg(hh);
        f->next = hh;
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && count(t, "outside no measure proposed for this recursion") == 1 && !strstr(t, "rec."), "no measure: outside at the call:\n%s", t ? t : "");
        free(t);
        JRank rr; memset(&rr, 0, sizeof rr);
        rr.blk = JRANK_ENTRY; rr.fn = f; rr.ncomp = 1; rr.nt = 1; rr.key[0] = n->id; rr.coef[0][0] = 1;   // n
        gen_rk = &rr; gen_nrk = 1;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        char key[48], want[96];
        snprintf(key, sizeof key, "bb%d.%d rec.fall 0", b2->id, ci);
        const char *vf = t ? vc_of(t, key) : "", *g = strstr(vf, "goal ");
        snprintf(want, sizeof want, "-1*%%v%d", n->id);
        char want2[32]; snprintf(want2, sizeof want2, "1*%%v%d ", m->id);
        CHECK(g && strstr(g, want) && strstr(g, want2) && strstr(g, "<= -1"), "rec.fall: (n - 1) - n <= -1, the argument less the parameter:\n%s", vf);
        snprintf(key, sizeof key, "bb%d.%d rec.bound 0", b2->id, ci);
        const char *vb = t ? vc_of(t, key) : "";
        g = strstr(vb, "goal ");
        CHECK(g && strstr(g, want) && strstr(g, "<= 0") && strstr(vb, "def "), "rec.bound: n >= 0, under the call's facts:\n%s", vb);
        CHECK(t && !strstr(t, "outside no measure"), "with a measure, no outside:\n%s", t ? t : "");
        free(t);
        f->may_diverge = true;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && !strstr(t, "rec.") && !strstr(t, "recursion"), "a function that may diverge owes nothing:\n%s", t ? t : "");
        free(t);
        f->has_decreasing = true;
        t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && strstr(t, "outside a written measure the generator cannot read as a sum") && !strstr(t, "rec.fall 0"),
              "...unless it wrote `decreasing` (D-44), which is owed (no ghost here: outside, the proposal not read):\n%s", t ? t : "");
        free(t);
        f->may_diverge = false; f->has_decreasing = false;
        struct { JRank r; int n; const char *why; } bad[3];
        for (int k = 0; k < 3; k++) { bad[k].r = rr; bad[k].n = 1; }
        bad[0].n = 2; bad[0].why = "two measures for one function's recursion";
        bad[1].r.key[0] = m->id; bad[1].why = "not over its function's parameters";
        bad[2].r.ncomp = 2; bad[2].r.cont[1] = 3; bad[2].why = "max or min of rows out of shape";
        for (int k = 0; k < 3; k++) {
            JRank two[2] = { bad[k].r, bad[k].r };
            gen_rk = two; gen_nrk = bad[k].n;
            char why[200] = "";
            t = gen_with(f, NULL, 0, JGEN_ALL, why);
            CHECK(!t && strstr(why, bad[k].why), "recursion measure %d is refused (%s): got \"%s\"", k, bad[k].why, why);
            free(t);
        }
        // the paths: one call, n's pair, before at entry and after as the argument
        {   gen_rk = NULL; gen_nrk = 0;
            JMem jm = jmem_over(region, sizeof region);
            JPaths P; char err[200];
            bool ok = jgen_call_paths(&jm, f, gen_mod, NULL, 0, &P, err, sizeof err);
            CHECK(ok && !P.refused && P.n == 1 && P.p[0].npairs == 1 && P.p[0].pairs[0].key == n->id &&
                  P.p[0].pairs[0].pre >= 0 && P.p[0].pairs[0].post >= 0 && P.p[0].pairs[0].pre != P.p[0].pairs[0].post,
                  "one call path: n at entry, n - 1 at the call (%d paths)", ok ? P.n : -1);
            CHECK(ok && P.n == 1 && P.p[0].callee == f && P.p[0].from == b2->id && P.p[0].at == ci && P.p[0].nentry == 1 &&
                  P.p[0].entry[0].key == n->id && P.p[0].entry[0].pre == P.p[0].pairs[0].pre && P.p[0].entry[0].post == -1,
                  "the call named (callee f, at bb%d.%d) and the caller's n at entry", b2->id, ci);
        }
        // p and q call each other
        IrFunc *pf = ir_func_new(&A, nm("p36"), u32t, IR_FUNC_PURE), *qf = ir_func_new(&A, nm("q36"), u32t, IR_FUNC_PURE);
        IrFunc *fs[2] = { pf, qf }; IrValue *ps[2]; IrBlock *cb[2]; int cix[2];
        for (int w = 0; w < 2; w++) {
            IrFunc *h = fs[w];
            ps[w] = ir_add_param(h, u32t, nm("n"));
            IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h);
            ir_set_br_cond(h0, ir_icmp(h, h0, IR_CMP_EQ, ps[w], ir_const_int(h, h0, 0, u32t)), h1, h2);
            ir_set_ret(h1, ir_const_int(h, h1, 0, u32t));
            IrValue *hm = ir_binop(h, h2, IR_SUB, ps[w], ir_const_int(h, h2, 1, u32t), u32t);
            cix[w] = nins(h2); cb[w] = h2;
            ir_set_ret(h2, mk_call(h, h2, w ? "p36" : "q36", hm, u32t));
            ir_finalize_cfg(h);
        }
        pf->next = qf; gen_mod = pf;
        JRank pr[2]; memset(pr, 0, sizeof pr);
        for (int w = 0; w < 2; w++) { pr[w].blk = JRANK_ENTRY; pr[w].fn = fs[w]; pr[w].ncomp = 1; pr[w].nt = 1; pr[w].key[0] = ps[w]->id; pr[w].coef[0][0] = 1; }
        gen_rk = pr; gen_nrk = 1;                                // p's measure only
        for (int w = 0; w < 2; w++) {
            t = gen_with(fs[w], NULL, 0, JGEN_ALL, NULL);
            CHECK(t && count(t, "outside no measure proposed for this recursion") == 1, "%s: the other's measure missing, outside:\n%s", w ? "q" : "p", t ? t : "");
            free(t);
        }
        gen_nrk = 2;
        for (int w = 0; w < 2; w++) {
            t = gen_with(fs[w], NULL, 0, JGEN_ALL, NULL);
            snprintf(key, sizeof key, "bb%d.%d rec.fall 0", cb[w]->id, cix[w]);
            CHECK(t && strstr(t, key) && !strstr(t, "outside no measure"), "%s: both measures, rec.fall at the call:\n%s", w ? "q" : "p", t ? t : "");
            free(t);
        }
        // a call through a pointer that may close the cycle: z takes its own address
        IrType *fnt = ir_type_new(&A, IRT_FUNC); fnt->elem = u32t;
        IrFunc *z = ir_func_new(&A, nm("z36"), u32t, IR_FUNC_PURE);
        IrValue *zn = ir_add_param(z, u32t, nm("n"));
        IrBlock *z0 = z->entry;
        IrValue *fr = ir_func_ref(z, z0, nm("z36"), fnt);
        IrInstr *ic = ir_instr(z, IR_CALL, u32t, 2); ic->operands[0] = fr; ic->operands[1] = zn; ir_emit(z0, ic);
        ir_set_ret(z0, ic->result);
        ir_finalize_cfg(z);
        gen_mod = z; gen_rk = NULL; gen_nrk = 0;
        t = gen_with(z, NULL, 0, JGEN_ALL, NULL);
        CHECK(t && strstr(t, "outside no measure proposed for this recursion"), "an indirect call on the cycle, no measure: outside:\n%s", t ? t : "");
        free(t);
        // with a measure n, the call through the pointer owes it as a direct call would (c31):
        // its target z's measure at the argument, op[1] (op[0] is the pointer)
        JRank zr; memset(&zr, 0, sizeof zr);
        zr.blk = JRANK_ENTRY; zr.fn = z; zr.ncomp = 1; zr.nt = 1; zr.key[0] = zn->id; zr.coef[0][0] = 1;
        gen_rk = &zr; gen_nrk = 1;
        t = gen_with(z, NULL, 0, JGEN_ALL, NULL);
        snprintf(key, sizeof key, "bb%d.%d rec.fall 0", z0->id, nins(z0) - 1);
        const char *vzf = t ? vc_of(t, key) : "", *gz = strstr(vzf, "goal ");
        CHECK(gz && !strncmp(gz, "goal 0 <= -1\n", 13) && !strstr(t, "outside"), "an indirect self-call with z(n) owes n - n <= -1:\n%s", t ? t : "");
        free(t);
        gen_rk = NULL; gen_nrk = 0;
        gen_mod = NULL;
    }
    printf("  a recursion's termination: rec.fall and rec.bound at each call on the cycle, outside where unmeasured\n");
    // 37. spec 12's CUT: a(n) -> b(n - 1) past `n == 0`, b(n) -> h(n) (a helper) and c(n), c(n) ->
    // a(n - 1) past `n == 0`; s(n) -> s(n - 1) on its own; the measure n each. With no call listed,
    // `rec.fall` at every call (b -> c too); b -> c listed, `rec.le` there (n - n <= 0) and the
    // others fall; a -> b and b -> c listed, c -> a cuts the cycle. Refused: every call of the cycle
    // listed (each member), s's self-call listed (a cycle of one; the others unaffected), z's call
    // through its own address listed (no direct call), a listed helper off the cycle, an index no
    // instruction, a count of listed calls out of range: each refuses b and a (the cut is the
    // component's). A measure's edges are not read: the finder fills them as for a loop
    {   IrFunc *fa = ir_func_new(&A, nm("a37"), u32t, IR_FUNC_PURE), *fb = ir_func_new(&A, nm("b37"), u32t, IR_FUNC_PURE),
               *fc = ir_func_new(&A, nm("c37"), u32t, IR_FUNC_PURE), *fh = ir_func_new(&A, nm("h37"), u32t, IR_FUNC_PURE),
               *fs = ir_func_new(&A, nm("s37"), u32t, IR_FUNC_PURE);
        IrFunc *fall[3] = { fa, fc, fs }; const char *callee[3] = { "b37", "a37", "s37" };
        IrValue *pn[5]; IrBlock *cb[5]; int cix[5];                      // a, c, s; then b's to h, to c
        for (int w = 0; w < 3; w++) {
            IrFunc *h = fall[w];
            pn[w] = ir_add_param(h, u32t, nm("n"));
            IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h);
            ir_set_br_cond(h0, ir_icmp(h, h0, IR_CMP_EQ, pn[w], ir_const_int(h, h0, 0, u32t)), h1, h2);
            ir_set_ret(h1, ir_const_int(h, h1, 0, u32t));
            IrValue *hm = ir_binop(h, h2, IR_SUB, pn[w], ir_const_int(h, h2, 1, u32t), u32t);
            cix[w] = nins(h2); cb[w] = h2;
            ir_set_ret(h2, mk_call(h, h2, callee[w], hm, u32t));
            ir_finalize_cfg(h);
        }
        pn[3] = pn[4] = ir_add_param(fb, u32t, nm("n"));
        cb[3] = cb[4] = fb->entry;
        cix[3] = nins(fb->entry); (void)mk_call(fb, fb->entry, "h37", pn[3], u32t);
        cix[4] = nins(fb->entry); ir_set_ret(fb->entry, mk_call(fb, fb->entry, "c37", pn[4], u32t));
        ir_finalize_cfg(fb);
        ir_set_ret(fh->entry, ir_add_param(fh, u32t, nm("x")));
        ir_finalize_cfg(fh);
        IrType *fnt = ir_type_new(&A, IRT_FUNC); fnt->elem = u32t;           // z(n) calls itself through &z
        IrFunc *fz = ir_func_new(&A, nm("z37"), u32t, IR_FUNC_PURE);
        IrValue *zn = ir_add_param(fz, u32t, nm("n"));
        IrValue *zr = ir_func_ref(fz, fz->entry, nm("z37"), fnt);
        int zci = nins(fz->entry);
        IrInstr *zc = ir_instr(fz, IR_CALL, u32t, 2); zc->operands[0] = zr; zc->operands[1] = zn; ir_emit(fz->entry, zc);
        ir_set_ret(fz->entry, zc->result);
        ir_finalize_cfg(fz);
        fa->next = fb; fb->next = fc; fc->next = fh; fh->next = fs; fs->next = fz; gen_mod = fa;
        JRank r4[4]; memset(r4, 0, sizeof r4);
        IrFunc *of[4] = { fa, fb, fc, fs }; IrValue *ov[4] = { pn[0], pn[3], pn[1], pn[2] };
        for (int w = 0; w < 4; w++) { r4[w].blk = JRANK_ENTRY; r4[w].fn = of[w]; r4[w].ncomp = 1; r4[w].nt = 1; r4[w].key[0] = ov[w]->id; r4[w].coef[0][0] = 1;
                                      r4[w].nedge = 1; r4[w].from[0] = cb[w < 3 ? w : 2]->id; }   // as the finder fills them: not read
        gen_rk = r4; gen_nrk = 4;
        char key[48], why[200];
        #define AT(i) (snprintf(key, sizeof key, "bb%d.%d", cb[i]->id, cix[i]), key)
        char *t = gen_with(fb, NULL, 0, JGEN_ALL, NULL);
        snprintf(why, sizeof why, "%s rec.fall 0", AT(4));
        CHECK(t && strstr(t, why) && !strstr(t, "rec.le") && !strstr(t, "outside"), "no call listed: b -> c falls:\n%s", t ? t : "");
        free(t);
        r4[1].nle = 1; r4[1].le_blk[0] = cb[4]->id; r4[1].le_idx[0] = cix[4];        // b -> c listed
        t = gen_with(fb, NULL, 0, JGEN_ALL, NULL);
        snprintf(why, sizeof why, "%s rec.le 0", AT(4));
        const char *vl = t ? vc_of(t, why) : "", *g = strstr(vl, "goal ");
        CHECK(g && !strncmp(g, "goal 0 <= 0\n", 12) && !strstr(t, "rec.fall") && strstr(t, "rec.bound"),
              "b -> c listed: rec.le, n - n <= 0, and rec.bound:\n%s", t ? t : "");
        free(t);
        for (int w = 0; w < 2; w++) {
            t = gen_with(w ? fc : fa, NULL, 0, JGEN_ALL, NULL);
            snprintf(why, sizeof why, "%s rec.fall 0", AT(w));
            CHECK(t && strstr(t, why) && !strstr(t, "rec.le"), "%s still falls, the cut accepted:\n%s", w ? "c" : "a", t ? t : "");
            free(t);
        }
        r4[0].nle = 1; r4[0].le_blk[0] = cb[0]->id; r4[0].le_idx[0] = cix[0];        // and a -> b: c -> a cuts
        t = gen_with(fa, NULL, 0, JGEN_ALL, NULL);
        snprintf(why, sizeof why, "%s rec.le 0", AT(0));
        CHECK(t && strstr(t, why), "a -> b and b -> c listed, c -> a falls: accepted:\n%s", t ? t : "");
        free(t);
        {   JMem jm = jmem_over(region, sizeof region);                     // a's call path names b
            JPaths P; char err[200];
            bool ok = jgen_call_paths(&jm, fa, gen_mod, NULL, 0, &P, err, sizeof err);
            CHECK(ok && !P.refused && P.n == 1 && P.p[0].callee == fb && P.p[0].from == cb[0]->id && P.p[0].at == cix[0] &&
                  P.p[0].npairs == 1 && P.p[0].pairs[0].key == pn[3]->id && P.p[0].pairs[0].pre == -1 && P.p[0].pairs[0].post >= 0 &&
                  P.p[0].nentry == 1 && P.p[0].entry[0].key == pn[0]->id && P.p[0].entry[0].pre >= 0 &&
                  P.p[0].entry[0].pre != P.p[0].pairs[0].post && P.p[0].entry[0].post == -1,
                  "a -> b's path: callee b, at bb%d.%d, b's n as the argument, a's n at entry", cb[0]->id, cix[0]);
            ok = jgen_call_paths(&jm, fb, gen_mod, NULL, 0, &P, err, sizeof err);
            CHECK(ok && P.n == 1 && P.p[0].callee == fc && P.p[0].at == cix[4], "b's one path is its call to c, not h's");
        }
        {   // u(x, n) -> v(n), v(n) -> u(n, n): the caller's parameters are not the callee's (value
            // ids are per function: a's n and b's n above are both value 0)
            IrFunc *fu = ir_func_new(&A, nm("u37"), u32t, IR_FUNC_PURE), *fv = ir_func_new(&A, nm("v37"), u32t, IR_FUNC_PURE);
            IrValue *ux = ir_add_param(fu, u32t, nm("x")), *un = ir_add_param(fu, u32t, nm("n")), *vn = ir_add_param(fv, u32t, nm("n"));
            ir_set_ret(fu->entry, mk_call(fu, fu->entry, "v37", un, u32t));
            IrInstr *vc = ir_instr(fv, IR_CALL, u32t, 2); vc->aux.callee = nm("u37"); vc->operands[0] = vn; vc->operands[1] = vn;
            ir_emit(fv->entry, vc); ir_set_ret(fv->entry, vc->result);
            ir_finalize_cfg(fu); ir_finalize_cfg(fv);
            fu->next = fv;
            JMem jm = jmem_over(region, sizeof region); JPaths P; char err[200];
            bool ok = jgen_call_paths(&jm, fu, fu, NULL, 0, &P, err, sizeof err);
            CHECK(ok && P.n == 1 && P.p[0].callee == fv && P.p[0].npairs == 1 && P.p[0].nentry == 2 &&
                  P.p[0].entry[0].key == ux->id && P.p[0].entry[1].key == un->id && P.p[0].pairs[0].post == P.p[0].entry[1].pre,
                  "u -> v(n): v's one parameter, u's two at entry, the argument u's n");
        }
        r4[2].nle = 1; r4[2].le_blk[0] = cb[1]->id; r4[2].le_idx[0] = cix[1];        // and c -> a: no cut
        for (int w = 0; w < 3; w++) {
            why[0] = 0;
            t = gen_with(of[w], NULL, 0, JGEN_ALL, why);
            CHECK(!t && strstr(why, "close a cycle"), "every call listed: member %d refused: got \"%s\"", w, why);
            free(t);
        }
        r4[0].nle = r4[2].nle = 0;
        r4[3].nle = 1; r4[3].le_blk[0] = cb[2]->id; r4[3].le_idx[0] = cix[2];        // s's self-call listed
        why[0] = 0; t = gen_with(fs, NULL, 0, JGEN_ALL, why);
        CHECK(!t && strstr(why, "close a cycle"), "a self-call listed is a cycle of one: got \"%s\"", why);
        free(t);
        t = gen_with(fa, NULL, 0, JGEN_ALL, NULL);
        CHECK(t != NULL, "s's list does not touch a's component");
        free(t);
        r4[3].nle = 0;
        JRank rz; memset(&rz, 0, sizeof rz);                                 // z's call through &z listed
        rz.blk = JRANK_ENTRY; rz.fn = fz; rz.ncomp = 1; rz.nt = 1; rz.key[0] = zn->id; rz.coef[0][0] = 1;
        rz.nle = 1; rz.le_blk[0] = fz->entry->id; rz.le_idx[0] = zci;
        gen_rk = &rz; gen_nrk = 1;
        why[0] = 0; t = gen_with(fz, NULL, 0, JGEN_ALL, why);
        CHECK(!t && strstr(why, "close a cycle"), "a self-call through a pointer listed is a cycle of one: got \"%s\"", why);
        free(t);
        gen_rk = r4; gen_nrk = 4;
        struct { int blk, idx, nle; const char *why; } bad[4] = {
            { cb[3]->id, cix[3], 1, "not a call that may close its cycle" },                    // b -> h
            { cb[3]->id, 99, 1, "not a call that may close its cycle" },
            { cb[4]->id, cix[4], JRANK_MAXL + 1, "listed calls out of range" },
            { cb[4]->id, cix[4], -1, "listed calls out of range" },
        };
        for (int k = 0; k < 4; k++) {
            JRank keep = r4[1];
            r4[1].le_blk[0] = bad[k].blk; r4[1].le_idx[0] = bad[k].idx; r4[1].nle = bad[k].nle;
            for (int w = 0; w < 2; w++) {                                        // b's own, then a's
                why[0] = 0; t = gen_with(w ? fa : fb, NULL, 0, JGEN_ALL, why);
                CHECK(!t && strstr(why, bad[k].why), "bad list %d refuses %s (%s): got \"%s\"", k, w ? "a" : "b", bad[k].why, why);
                free(t);
            }
            r4[1] = keep;
        }
        #undef AT
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  spec 12's cut: rec.le at the calls a measure lists, which must close no cycle\n");
    // 38. a CONSTANT FACTOR behind a cast (`2 *? y` widens both operands to i64 first: cast(2) *
    // cast(y)). A product is stated, not `atom.mul` outside, when a factor is a constant, a checked
    // `neg` of one, or a cast of either into an integer type whose bits hold its value, at most four
    // casts deep: (a) cast(2 : i32) : i64; (b) cast(2^64 - 1 : u64) : i64, which i64 cannot hold,
    // outside; (c) a cast of (a)'s cast; (d) a cast that reads itself (an IR no lowering makes),
    // outside and no recursion for ever; (e) `neg 4`, whose product is -4 * y; (f) cast(neg 4) :
    // i64, the same; (g) cast(200 : i32) : i8 and (h) cast(-1 : i32) : u32, neither held, outside
    IrType *i8t = ir_type_int(&A, 8, true);
    for (int c = 0; c < 8; c++) {
        IrType *pt = c == 4 ? i32t : c == 6 ? i8t : c == 7 ? u32t : i64t;   // the product's type
        IrFunc *f = ir_func_new(&A, nm("m38"), i64t, IR_FUNC_PURE);
        IrValue *y = ir_add_param(f, i32t, nm("y"));
        IrBlock *b0 = f->entry;
        IrValue *k = NULL, *yw = NULL;
        IrInstr *ci;
        switch (c) {
            case 0: case 2:
                ci = ir_instr(f, IR_CAST, i64t, 1); ci->operands[0] = ir_const_int(f, b0, 2, i32t); ir_emit(b0, ci); k = ci->result;
                if (c == 2) { ci = ir_instr(f, IR_CAST, i64t, 1); ci->operands[0] = k; ir_emit(b0, ci); k = ci->result; }
                break;
            case 1:
                ci = ir_instr(f, IR_CAST, i64t, 1); ci->operands[0] = ir_const_int(f, b0, -1, u64t); ir_emit(b0, ci); k = ci->result;
                break;
            case 3:
                ci = ir_instr(f, IR_CAST, i64t, 1); ci->operands[0] = ci->result; ir_emit(b0, ci); k = ci->result;
                break;
            case 4: case 5:
                ci = ir_instr(f, IR_NEG, i32t, 1); ci->operands[0] = ir_const_int(f, b0, 4, i32t); ir_emit(b0, ci); k = ci->result;
                if (c == 5) { ci = ir_instr(f, IR_CAST, i64t, 1); ci->operands[0] = k; ir_emit(b0, ci); k = ci->result; }
                break;
            default:
                ci = ir_instr(f, IR_CAST, pt, 1); ci->operands[0] = ir_const_int(f, b0, c == 6 ? 200 : -1, i32t); ir_emit(b0, ci); k = ci->result;
                break;
        }
        if (c == 4) yw = y;
        else { ci = ir_instr(f, IR_CAST, pt, 1); ci->operands[0] = y; ir_emit(b0, ci); yw = ci->result; }
        IrValue *p = ir_binop(f, b0, IR_MUL, k, yw, pt);
        ir_set_ret(b0, p);
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), want[96];
        snprintf(want, sizeof want, "def %%v%d mul : 1*%%v%d + %s*%%v%d = 0", p->id, p->id, c == 4 || c == 5 ? "4" : "-2", yw->id);
        if (c == 1 || c == 3 || c == 6 || c == 7)
            CHECK(t && count(t, "outside atom.mul") == 1 && !strstr(t, " mul : "), "case %d: no constant factor, outside:\n%s", c, t ? t : "");
        else
            CHECK(t && !strstr(t, "atom.mul") && count(t, want) == 2, "case %d: the product stated (%s):\n%s", c, want, t ? t : "");
        free(t);
    }
    gen_mod = NULL;
    printf("  a constant factor behind a cast: the product stated, outside where the cast cannot hold it\n");
    // 39. a condition the IR FIXES: `x = n; while C { x = x + 1 }` with C (0) `0 != 0`, (1) `false`,
    // (2) `0 == 0`, (3) `5 <u -1` (true in C, -1 being 2^32 - 1 there; a signed constant below 0 in
    // an unsigned compare is not fixed, so it cannot be read as 5 < -1, false), (4)
    // cast(2) : i64 < 1. Where C is false the body is never entered: x + 1's VCs hold `0 <= -1`, from
    // the guard; where it holds, or is not fixed, nothing is stated. A constant ranking claim (no
    // terms) is accepted, and its fall on the dead back edge is `0 <= -1` under that guard. Then a
    // JOIN past `if 0 != 0 { x = 1 } else { x = 2 }`: the dead branch's disjunct holds `0 <= -1`
    for (int c = 0; c < 5; c++) {
        IrFunc *f = ir_func_new(&A, nm("k39"), i32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, i32t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *x = ir_alloca(f, b0, i32t);
        ir_store(f, b0, x, n); ir_set_br(b0, b1);
        IrValue *cond;
        switch (c) {
            case 0: cond = ir_icmp(f, b1, IR_CMP_NE, ir_const_int(f, b1, 0, i32t), ir_const_int(f, b1, 0, i32t)); break;
            case 1: cond = ir_const_int(f, b1, 0, boolt); break;
            case 2: cond = ir_icmp(f, b1, IR_CMP_EQ, ir_const_int(f, b1, 0, i32t), ir_const_int(f, b1, 0, i32t)); break;
            case 3: cond = ir_icmp(f, b1, IR_CMP_ULT, ir_const_int(f, b1, 5, i32t), ir_const_int(f, b1, -1, i32t)); break;
            default: {
                IrInstr *ci = ir_instr(f, IR_CAST, i64t, 1); ci->operands[0] = ir_const_int(f, b1, 2, i32t); ir_emit(b1, ci);
                cond = ir_icmp(f, b1, IR_CMP_SLT, ci->result, ir_const_int(f, b1, 1, i64t)); break;
            }
        }
        ir_set_br_cond(b1, cond, b2, b3);
        IrValue *lx = ir_load(f, b2, x, i32t);
        int ai = nins(b2) + 1;                                           // the add, after its constant
        ir_store(f, b2, x, ir_binop(f, b2, IR_ADD, lx, ir_const_int(f, b2, 1, i32t), i32t));
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_load(f, b3, x, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48];
        snprintf(key, sizeof key, "bb%d.%d narrow.hi 0", b2->id, ai);
        const char *v = t ? vc_of(t, key) : "";
        char want[64]; snprintf(want, sizeof want, "guard bb%d->bb%d : 0 <= -1", b1->id, b2->id);
        bool dead = c == 0 || c == 1 || c == 4;
        CHECK(*v && (dead ? strstr(v, want) != NULL : !strstr(t, "0 <= -1")), "case %d: %s:\n%s", c,
              dead ? "the guard is false, the body never entered" : "nothing stated", t ? t : "");
        free(t);
        if (c == 0) {                                                    // a constant ranking claim
            JRank r; memset(&r, 0, sizeof r);
            r.blk = b1->id; r.fn = f; r.ncomp = 1; r.nt = 0; r.nedge = 1; r.from[0] = b2->id; r.fall[0] = 0;
            gen_rk = &r; gen_nrk = 1;
            char why[200] = "";
            t = gen_with(f, NULL, 0, JGEN_ALL, why);
            snprintf(key, sizeof key, "bb%d.%d rank.fall 0", b2->id, nins(b2));
            v = t ? vc_of(t, key) : "";
            CHECK(t && strstr(v, want) && strstr(v, "goal 0 <= -1\n"), "a constant ranking claim: its fall is 0 <= -1 past the dead guard (%s):\n%s", why, t ? t : "");
            free(t);
            gen_rk = NULL; gen_nrk = 0;
        }
    }
    {   IrFunc *f = ir_func_new(&A, nm("j39"), u32t, IR_FUNC_PURE);
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *x = ir_alloca(f, b0, u32t);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_NE, ir_const_int(f, b0, 0, u32t), ir_const_int(f, b0, 0, u32t)), b1, b2);
        ir_store(f, b1, x, ir_const_int(f, b1, 1, u32t)); ir_set_br(b1, b3);
        ir_store(f, b2, x, ir_const_int(f, b2, 2, u32t)); ir_set_br(b2, b3);
        IrValue *lx = ir_load(f, b3, x, u32t);
        ir_set_ret(b3, ir_binop(f, b3, IR_UDIV, ir_const_int(f, b3, 100, u32t), ir_binop(f, b3, IR_SUB, lx, ir_const_int(f, b3, 1, u32t), u32t), u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        const char *v = t ? vc_of(t, " divzero 0") : "", *jh = strstr(v, "join bb");
        CHECK(jh && strstr(jh, ", 0 <= -1 }"), "the join's dead disjunct (x = 1) holds 0 <= -1:\n%s", v);
        free(t);
    }
    {   // every predicate on 1 ? 2, 2 ? 2, 3 ? 2, -1 ? 2 and 2 ? -1: the unsigned ones leave -1 unfixed
        IrFunc *f = ir_func_new(&A, nm("t39"), u32t, IR_FUNC_PURE);
        IrBlock *b0 = f->entry;
        static const struct { IrCmp c; int want[5]; } P[] = {
            { IR_CMP_EQ,  { 0, 1, 0, 0, 0 } }, { IR_CMP_NE,  { 1, 0, 1, 1, 1 } },
            { IR_CMP_SLT, { 1, 0, 0, 1, 0 } }, { IR_CMP_SLE, { 1, 1, 0, 1, 0 } },
            { IR_CMP_SGT, { 0, 0, 1, 0, 1 } }, { IR_CMP_SGE, { 0, 1, 1, 0, 1 } },
            { IR_CMP_ULT, { 1, 0, 0, -1, -1 } }, { IR_CMP_ULE, { 1, 1, 0, -1, -1 } },
            { IR_CMP_UGT, { 0, 0, 1, -1, -1 } }, { IR_CMP_UGE, { 0, 1, 1, -1, -1 } } };
        enum { NP = (int)(sizeof P / sizeof *P) };
        IrValue *cv[NP][5];
        static const int L[5] = { 1, 2, 3, -1, 2 }, R[5] = { 2, 2, 2, 2, -1 };
        for (int q = 0; q < NP; q++)
            for (int k = 0; k < 5; k++)
                cv[q][k] = ir_icmp(f, b0, P[q].c, ir_const_int(f, b0, L[k], i32t), ir_const_int(f, b0, R[k], i32t));
        ir_set_ret(b0, ir_const_int(f, b0, 0, u32t));
        ir_finalize_cfg(f);
        JG g; memset(&g, 0, sizeof g);
        g.nv = f->next_value_id;
        IrInstr **def = calloc((size_t)g.nv, sizeof *def);
        for (IrInstr *i = b0->instrs; i; i = i->next) if (i->result) def[i->result->id] = i;
        g.def = def;
        for (int q = 0; q < NP; q++)
            for (int k = 0; k < 5; k++) {
                int got = jg_cond_value(&g, cv[q][k]);
                CHECK(got == P[q].want[k], "predicate %d on %d ? %d: %d, not %d", (int)P[q].c, L[k], R[k], got, P[q].want[k]);
            }
        free(def);
    }
    gen_mod = NULL;
    printf("  a fixed condition: false on its edge, 0 <= -1 there; a constant ranking claim for a dead loop\n");
    // 40. a cell the loop only READS is a pair of its paths: `i = 0; c = n; z = 7; while i < c { i =
    // i + 1 }; return z`. The header's pairs are i (written) and c (read in the header): on the back
    // edge c's pre and post are one variable, on the entry edge its pre is -1. z, read only past the
    // loop, is no pair
    {   IrFunc *f = ir_func_new(&A, nm("r40"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u32t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u32t), *c = ir_alloca(f, b0, u32t), *z = ir_alloca(f, b0, u32t);
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u32t)); ir_store(f, b0, c, n); ir_store(f, b0, z, ir_const_int(f, b0, 7, u32t));
        ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, i, u32t), *lc = ir_load(f, b1, c, u32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, li, lc), b2, b3);
        IrValue *li2 = ir_load(f, b2, i, u32t);
        ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li2, ir_const_int(f, b2, 1, u32t), u32t));
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_load(f, b3, z, u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JMem m = jmem_over(region, sizeof region);
        JPaths P; char err[200];
        bool ok = jgen_paths(&m, f, gen_mod, NULL, 0, b1->id, &P, err, sizeof err);
        CHECK(ok && !P.refused && P.n == 2, "two edges into the header (%s)", ok ? "" : err);
        for (int32_t k = 0; ok && k < P.n; k++) {
            const JPath *p = &P.p[k]; const JPathPair *pi = NULL, *pc = NULL; int other = 0;
            int ient = 0;                                       // i's ENTRY pair (i is written; c is not)
            for (int32_t q = 0; q < p->npairs; q++) {
                if (p->pairs[q].key == i->id && p->pairs[q].tk == JTERM_ENTRY) ient++;
                else if (p->pairs[q].key == i->id) pi = &p->pairs[q];
                else if (p->pairs[q].key == c->id && p->pairs[q].tk == JTERM_VALUE) pc = &p->pairs[q];
                else other++;
            }
            CHECK(pi && pc && ient == 1 && other == 0, "path %d: the pairs are i, i's entry and c, not z (%d pairs)", k, p->npairs);
            if (!pi || !pc) continue;
            if (p->back) CHECK(pc->pre >= 0 && pc->pre == pc->post && pi->pre != pi->post, "the back edge: c's pre is its post, i's is not");
            else CHECK(pc->pre == -1 && pc->post >= 0, "the entry edge: c's pre is -1, its post its value");
        }
        gen_mod = NULL;
    }
    printf("  a cell the loop only reads: a pair of its paths, one variable before and after a trip\n");
    // 41. an UNSIGNED comparison with a signed operand compares both as uint64 (emit_c.h, interp.h):
    // the guard stated on each edge must be TRUE exactly where C's `(uint64_t)x op (uint64_t)y` is
    // (soundness one way, precision the other), for ult/ule/ugt/uge, x and y of i64/u64, u64/i64,
    // i64/i64 (four disjuncts) and i32/u32, on values at both ends of each type
    {   // cases 4 and 5: one operand a NEGATIVE constant, y = -1 : i64 against a u64 x, x = -3 : i32
        // against a u32 y (as a uint64 it is 2^64 - 3)
        IrType *T[6][2] = { { i64t, u64t }, { u64t, i64t }, { i64t, i64t }, { i32t, u32t }, { u64t, i64t }, { i32t, u32t } };
        IrCmp C[4] = { IR_CMP_ULT, IR_CMP_ULE, IR_CMP_UGT, IR_CMP_UGE };
        const __int128 P63 = (__int128)1 << 63, P64 = (__int128)1 << 64;
        int bad = 0, evals = 0;
        for (int ti = 0; ti < 6; ti++) for (int ci = 0; ci < 4; ci++) {
            IrFunc *f = ir_func_new(&A, nm("u41"), u64t, IR_FUNC_PURE);
            IrValue *x = ir_add_param(f, T[ti][0], nm("x")), *y = ir_add_param(f, T[ti][1], nm("y"));
            IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
            __int128 kv = ti == 4 ? -1 : -3;                               // the constant's value
            if (ti == 4) y = ir_const_int(f, b0, -1, i64t);
            if (ti == 5) x = ir_const_int(f, b0, -3, i32t);
            ir_set_br_cond(b0, ir_icmp(f, b0, C[ci], x, y), b1, b2);
            ir_set_ret(b1, ir_binop(f, b1, T[ti][1]->is_signed ? IR_SDIV : IR_UDIV, ir_const_int(f, b1, 100, T[ti][1]), y, T[ti][1]));
            ir_set_ret(b2, ir_binop(f, b2, T[ti][0]->is_signed ? IR_SDIV : IR_UDIV, ir_const_int(f, b2, 100, T[ti][0]), x, T[ti][0]));
            ir_finalize_cfg(f);
            JMem m = jmem_over(region, sizeof region); JObl *o; int32_t n; char err[200];
            if (!jgen_func(&m, f, f, NULL, 0, NULL, 0, NULL, JGEN_ALL, &o, &n, err, sizeof err)) { bad++; continue; }
            for (int e = 0; e < 2; e++) {                                  // the true edge, the false one
                char org[32], xn[16], yn[16]; snprintf(org, sizeof org, "guard bb0->bb%d", e ? b2->id : b1->id);
                snprintf(xn, sizeof xn, "%%v%d", x->id); snprintf(yn, sizeof yn, "%%v%d", y->id);
                const JVc *vc = NULL; const JHyp *h = NULL;
                for (int32_t i = 0; i < n && !h; i++) {
                    if (o[i].outside || strcmp(o[i].vc.id.kind, "divzero")) continue;
                    for (int32_t k = 0; k < o[i].vc.nhyps && !h; k++)
                        if (!strcmp(o[i].vc.hyps[k].origin, org)) { h = &o[i].vc.hyps[k]; vc = &o[i].vc; }
                }
                if (!h) { bad++; printf("  no guard %s (types %d, predicate %d)\n", org, ti, ci); continue; }
                __int128 xs[8], ys[8]; int nx = 0, ny = 0;
                for (int w = 0; w < 2; w++) {
                    IrType *t = T[ti][w]; __int128 *vs = w ? ys : xs; int *nv = w ? &ny : &nx;
                    __int128 lo = t->is_signed ? -((__int128)1 << (t->bits - 1)) : 0, hi = t->is_signed ? ((__int128)1 << (t->bits - 1)) - 1 : ((__int128)1 << t->bits) - 1;
                    __int128 cand[8] = { lo, lo + 1, -2, -1, 0, 1, hi - 1, hi };
                    if ((ti == 4 && w == 1) || (ti == 5 && w == 0)) { vs[(*nv)++] = kv; continue; }   // the constant
                    for (int q = 0; q < 8; q++) if (cand[q] >= lo && cand[q] <= hi) vs[(*nv)++] = cand[q];
                }
                for (int ix = 0; ix < nx; ix++) for (int iy = 0; iy < ny; iy++) {
                    __int128 ux = xs[ix] < 0 ? xs[ix] + P64 : xs[ix], uy = ys[iy] < 0 ? ys[iy] + P64 : ys[iy];
                    bool c = C[ci] == IR_CMP_ULT ? ux < uy : C[ci] == IR_CMP_ULE ? ux <= uy : C[ci] == IR_CMP_UGT ? ux > uy : ux >= uy;
                    if (e) c = !c;
                    // the hypothesis at (x, y): every conjunct of some disjunct
                    bool any = false;
                    for (int d = 0; d < (h->disj ? h->ndis : 1) && !any; d++) {
                        const JCons *cs = h->disj ? h->dis[d].c : &h->cons; int nc = h->disj ? h->dis[d].nc : 1; bool all = true;
                        for (int k = 0; k < nc && all; k++) {
                            __int128 sum = 0;
                            for (int32_t tt = 0; tt < cs[k].nt; tt++) {
                                const char *vn = vc->vars[cs[k].t[tt].var];
                                __int128 val = !strcmp(vn, xn) ? xs[ix] : !strcmp(vn, yn) ? ys[iy] : (__int128)1 << 100;
                                sum += jint128(&cs[k].t[tt].coef) * val;
                            }
                            __int128 r = jint128(&cs[k].rhs);
                            all = cs[k].op == J_LE ? sum <= r : cs[k].op == J_EQ ? sum == r : sum != r;
                        }
                        any = all;
                    }
                    evals++;
                    if (any != c) { bad++; if (bad < 6) printf("  types %d predicate %d edge %d at x=%lld y=%lld: stated %d, C %d\n", ti, ci, e, (long long)xs[ix], (long long)ys[iy], any, c); }
                }
            }
            (void)P63; (void)kv;
        }
        CHECK(bad == 0 && evals > 500, "every guard equals C's uint64 comparison (%d wrong of %d)", bad, evals);
    }
    {   // inside a JOIN's disjunct the split cannot be stated (a disjunct holds no disjunction): past
        // `if x <u y { r = 1 } else { r = 2 }`, the guard is in no hypothesis of its own (stated
        // globally it would hold on the path that did not test it)
        IrFunc *f = ir_func_new(&A, nm("w41"), u64t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i64t, nm("x")), *y = ir_add_param(f, u64t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *r = ir_alloca(f, b0, u64t);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, x, y), b1, b2);
        ir_store(f, b1, r, ir_const_int(f, b1, 1, u64t)); ir_set_br(b1, b3);
        ir_store(f, b2, r, ir_const_int(f, b2, 2, u64t)); ir_set_br(b2, b3);
        ir_set_ret(b3, ir_binop(f, b3, IR_UDIV, ir_const_int(f, b3, 100, u64t), ir_load(f, b3, r, u64t), u64t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        const char *v = t ? vc_of(t, " divzero 0") : "";
        CHECK(strstr(v, "join bb") && !strstr(v, "guard") && !strstr(v, "  : "), "no guard of a join's path stated alone:\n%s", v);
        free(t);
        gen_mod = NULL;
    }
    printf("  an unsigned comparison of a signed operand: split on its sign, exact against C\n");
    // 42. a WRITTEN function measure (I.206), the ghost over the parameters: w(n, k, s) recursing
    // on w(n - 1, k, s) past n <= 0, its measure (0) 2 * n: one node, so rec.def at the entry
    // (ordinals 0, 1) and at the arguments (1000, 1001); (1) n / 2 and (2) n * k: no sum, outside;
    // (3) s.len - n over a slice parameter's length; (4) 2 * n with a proposal whose terms are no
    // parameter: not read, so not refused; (5) the same proposal listing the self-call: its list
    // IS read, a cycle of one, refused; (6) two proposals: refused. Then a MIXED component: p
    // written (decreasing n), q proposed (n), each calling the other on n - 1: both owe rec.fall
    for (int c = 0; c < 7; c++) {
        IrType *slt = ir_type_new(&A, IRT_SLICE); slt->elem = u8t;
        IrFunc *f = ir_func_new(&A, nm("w42"), i32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, i32t, nm("n")), *k = ir_add_param(f, i32t, nm("k")), *sl = ir_add_param(f, slt, nm("s"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SLE, n, ir_const_int(f, b0, 0, i32t)), b1, b2);
        ir_set_ret(b1, ir_const_int(f, b1, 0, i32t));
        IrValue *m1 = ir_binop(f, b2, IR_SUB, n, ir_const_int(f, b2, 1, i32t), i32t);
        int ci = nins(b2);
        IrInstr *call = ir_instr(f, IR_CALL, i32t, 3); call->aux.callee = nm("w42");
        call->operands[0] = m1; call->operands[1] = k; call->operands[2] = sl; ir_emit(b2, call);
        ir_set_ret(b2, call->result);
        f->has_decreasing = true; f->measure_blk = ir_new_ghost_block(f);
        IrBlock *gb = f->measure_blk;
        switch (c) {
            case 1: f->measure_val = ir_binop(f, gb, IR_SDIV, n, ir_const_int(f, gb, 2, i32t), i32t); break;
            case 2: f->measure_val = ir_binop(f, gb, IR_MUL, n, k, i32t); break;
            case 3: { IrValue *len = ir_slice_len(f, gb, sl);
                      f->measure_val = ir_binop(f, gb, IR_SUB, len, ir_const_int(f, gb, 0, len->type), len->type); break; }
            default: f->measure_val = ir_binop(f, gb, IR_MUL, ir_const_int(f, gb, 2, i32t), n, i32t); break;
        }
        ir_finalize_cfg(f);
        JRank pr[2]; memset(pr, 0, sizeof pr);
        for (int w = 0; w < 2; w++) { pr[w].blk = JRANK_ENTRY; pr[w].fn = f; pr[w].ncomp = 1; pr[w].nt = 1; pr[w].key[0] = m1->id; pr[w].coef[0][0] = 7; }
        if (c == 5) { pr[0].nle = 1; pr[0].le_blk[0] = b2->id; pr[0].le_idx[0] = ci; }
        gen_rk = c >= 4 ? pr : NULL; gen_nrk = c == 6 ? 2 : c >= 4 ? 1 : 0;
        gen_mod = f;
        char why[200] = "", key[48];
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        snprintf(key, sizeof key, "bb%d.%d rec.", b2->id, ci);
        if (c == 1 || c == 2)
            CHECK(t && strstr(t, "outside a written measure the generator cannot read as a sum") && !strstr(t, key), "case %d: no sum, outside:\n%s", c, t ? t : why);
        else if (c == 5 || c == 6)
            CHECK(!t && strstr(why, c == 5 ? "close a cycle" : "two measures"), "case %d: refused (%s)", c, why);
        else {
            int nd = 0; char dk[64];
            for (int o = 0; o < 2; o++) for (int w = 0; w < 2; w++) {
                snprintf(dk, sizeof dk, "bb%d.%d rec.def %d", b2->id, ci, o * 1000 + w);
                if (strstr(t ? t : "", dk)) nd++;
            }
            snprintf(dk, sizeof dk, "bb%d.%d rec.fall 0", b2->id, ci);
            const char *v = t ? vc_of(t, dk) : "";
            char len[32]; snprintf(len, sizeof len, "len(%%v%d)", sl->id);
            snprintf(dk, sizeof dk, "bb%d.%d rec.def 0", b2->id, ci);
            const char *vd = t ? vc_of(t, dk) : "";
            snprintf(dk, sizeof dk, "bb%d.%d rec.fall 0", b2->id, ci);
            CHECK(t && strstr(t, dk) && nd == 4 && !strstr(t, "outside") && (c != 3 || (strstr(v, len) && strstr(vd, len))),
                  "case %d: rec.fall, and rec.def at both ends on each side (%d of 4):\n%s", c, nd, t ? t : why);
        }
        free(t);
    }
    {   IrFunc *pf = ir_func_new(&A, nm("p42"), i32t, IR_FUNC_PURE), *qf = ir_func_new(&A, nm("q42"), i32t, IR_FUNC_PURE);
        IrFunc *fs[2] = { pf, qf }; IrValue *ps[2]; IrBlock *cb[2]; int cix[2];
        for (int w = 0; w < 2; w++) {
            IrFunc *h = fs[w];
            ps[w] = ir_add_param(h, i32t, nm("n"));
            IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h);
            ir_set_br_cond(h0, ir_icmp(h, h0, IR_CMP_SLE, ps[w], ir_const_int(h, h0, 0, i32t)), h1, h2);
            ir_set_ret(h1, ir_const_int(h, h1, 0, i32t));
            IrValue *hm = ir_binop(h, h2, IR_SUB, ps[w], ir_const_int(h, h2, 1, i32t), i32t);
            cix[w] = nins(h2); cb[w] = h2;
            ir_set_ret(h2, mk_call(h, h2, w ? "p42" : "q42", hm, i32t));
            ir_finalize_cfg(h);
        }
        pf->has_decreasing = true; pf->measure_blk = ir_new_ghost_block(pf); pf->measure_val = ps[0];
        pf->next = qf; gen_mod = pf;
        JRank qr[2]; memset(qr, 0, sizeof qr);
        qr[0].blk = JRANK_ENTRY; qr[0].fn = qf; qr[0].ncomp = 1; qr[0].nt = 1; qr[0].key[0] = ps[1]->id; qr[0].coef[0][0] = 1;
        gen_rk = qr; gen_nrk = 1;
        for (int w = 0; w < 2; w++) {
            char key[48]; snprintf(key, sizeof key, "bb%d.%d rec.fall 0", cb[w]->id, cix[w]);
            char *t = gen_with(fs[w], NULL, 0, JGEN_ALL, NULL);
            CHECK(t && strstr(t, key) && !strstr(t, "outside"), "mixed: %s owes rec.fall:\n%s", w ? "q" : "p", t ? t : "");
            free(t);
        }
        // a proposal for p (written) whose terms are nothing of p's, listing p's call to q: its list
        // is read (p -> q owes rec.le; q -> p, strict, cuts the cycle), its measure not
        qr[1].blk = JRANK_ENTRY; qr[1].fn = pf; qr[1].ncomp = 1; qr[1].nt = 1; qr[1].key[0] = 999; qr[1].coef[0][0] = 5;
        qr[1].nle = 1; qr[1].le_blk[0] = cb[0]->id; qr[1].le_idx[0] = cix[0];
        gen_nrk = 2;
        {   char key[48]; snprintf(key, sizeof key, "bb%d.%d rec.le 0", cb[0]->id, cix[0]);
            char *t = gen_with(pf, NULL, 0, JGEN_ALL, NULL);
            CHECK(t && strstr(t, key) && !strstr(t, "rec.fall 0"), "p's proposal's list read, its measure not:\n%s", t ? t : "");
            free(t);
        }
    }
    gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    printf("  a written function measure: owed as written, defined at both ends, outside where no sum\n");
    // 43. ELEMENT cells (P2.1: a constant field AND element path): a local `a i32[4]` filled by
    // constant-index stores (u64 indices) and read at a constant index (an i32 one): the load is
    // the stored version, `%sR[0]`. Cases: (0) read a[0] back: its version is the store's, 10;
    // (1) a store through a[i] in between: every element of a is fresh (havoc), so a[0] is no
    // longer 10; (2) a[5], past the length: no cell, no version; (3) the store through a[i]
    // UNCHECKED: fresh as well; (4) a[i]
    // handed to a call as a mutable argument: a's elements fresh; then a STRUCT `s {n i32, arr
    // i32[3]}`: (5) `s.arr[i] = 4` leaves s.n's stored 1 standing and makes s.arr[0] fresh; (6) a
    // whole-struct store S(1, t) makes s.arr[0] fresh, never t's address (jg_part stops at `[k]`);
    // (7) an array of structs, a[1].x: the cell `[1].0`; (8) case 5 with `s.arr[i]` UNCHECKED: it
    // may write anywhere in s, so s.n is fresh too; (9) case 7 then `a[i].x = 5`: a field of an
    // element at a variable index may be a[1].x, which is fresh; (10) a[-1]: no cell either
    IrType *a4 = ir_type_new(&A, IRT_ARRAY); a4->elem = i32t; a4->array_len = 4;
    IrType *a3 = ir_type_new(&A, IRT_ARRAY); a3->elem = i32t; a3->array_len = 3;
    IrType *S43 = ir_type_new(&A, IRT_STRUCT);
    S43->n_fields = 2; S43->fields = arena_push_many_aligned(&A, IrType *, 2); S43->fields[0] = i32t; S43->fields[1] = a3;
    IrType *P43 = ir_type_new(&A, IRT_STRUCT);
    P43->n_fields = 2; P43->fields = arena_push_many_aligned(&A, IrType *, 2); P43->fields[0] = i32t; P43->fields[1] = i32t;
    IrType *ap = ir_type_new(&A, IRT_ARRAY); ap->elem = P43; ap->array_len = 2;
    for (int c = 0; c < 11; c++) {
        IrFunc *f = ir_func_new(&A, nm("e43"), i32t, IR_FUNC_PURE);
        IrValue *ix = ir_add_param(f, u64t, nm("i"));
        IrBlock *b0 = f->entry;
        IrValue *got = NULL, *root = NULL, *tmp = NULL; const char *path = "";
        if (c <= 4 || c == 10) {
            root = ir_alloca_array(f, b0, a4);
            for (int k = 0; k < 4; k++)
                ir_store(f, b0, ir_elem_ptr(f, b0, root, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 10 * (k + 1), i32t));
            if (c == 1 || c == 3) {
                IrValue *ep = ir_elem_ptr(f, b0, root, ix, i32t);
                if (c == 3) g_last_instr(b0)->unchecked = true;
                ir_store(f, b0, ep, ir_const_int(f, b0, 7, i32t));
            }
            if (c == 4) {
                IrType *vp = ir_type_new(&A, IRT_PTR); vp->elem = i32t; vp->ptr_mut = true;
                IrFunc *callee = ir_func_new(&A, nm("touch43"), u32t, IR_FUNC_PURE);
                ir_add_param(callee, vp, nm("p")); ir_set_ret(callee->entry, ir_const_int(callee, callee->entry, 0, u32t)); ir_finalize_cfg(callee);
                f->next = callee;
                (void)mk_call(f, b0, "touch43", ir_elem_ptr(f, b0, root, ix, i32t), u32t);
            }
            got = ir_load(f, b0, ir_elem_ptr(f, b0, root, ir_const_int(f, b0, c == 2 ? 5 : c == 10 ? -1 : 0, i32t), i32t), i32t);
            path = c == 2 ? "[5]" : c == 10 ? "[-1]" : "[0]";
        } else if (c <= 6 || c == 8) {
            root = ir_alloca(f, b0, S43);
            if (c == 6) {
                IrValue *t = tmp = ir_alloca_array(f, b0, a3);
                for (int k = 0; k < 3; k++) ir_store(f, b0, ir_elem_ptr(f, b0, t, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 5 + k, i32t));
                IrInstr *sn = ir_instr(f, IR_STRUCT_NEW, S43, 2); sn->operands[0] = ir_const_int(f, b0, 1, i32t); sn->operands[1] = t; ir_emit(b0, sn);
                ir_store(f, b0, root, sn->result);
            } else {
                ir_store(f, b0, ir_field_ptr(f, b0, root, 0, i32t), ir_const_int(f, b0, 1, i32t));
                ir_store(f, b0, ir_elem_ptr(f, b0, ir_field_ptr(f, b0, root, 1, a3), ir_const_int(f, b0, 0, u64t), i32t), ir_const_int(f, b0, 5, i32t));
                IrValue *ep = ir_elem_ptr(f, b0, ir_field_ptr(f, b0, root, 1, a3), ix, i32t);
                if (c == 8) g_last_instr(b0)->unchecked = true;
                ir_store(f, b0, ep, ir_const_int(f, b0, 4, i32t));
            }
            got = ir_load(f, b0, ir_elem_ptr(f, b0, ir_field_ptr(f, b0, root, 1, a3), ir_const_int(f, b0, 0, i32t), i32t), i32t);
            path = ".1[0]";
        } else {
            root = ir_alloca_array(f, b0, ap);
            IrValue *e1 = ir_elem_ptr(f, b0, root, ir_const_int(f, b0, 1, u64t), P43);
            ir_store(f, b0, ir_field_ptr(f, b0, e1, 0, i32t), ir_const_int(f, b0, 3, i32t));
            if (c == 9) ir_store(f, b0, ir_field_ptr(f, b0, ir_elem_ptr(f, b0, root, ix, P43), 0, i32t), ir_const_int(f, b0, 5, i32t));
            got = ir_load(f, b0, ir_field_ptr(f, b0, ir_elem_ptr(f, b0, root, ir_const_int(f, b0, 1, i32t), P43), 0, i32t), i32t);
            path = "[1].0";
        }
        IrValue *sn5 = c == 5 || c == 8 ? ir_load(f, b0, ir_field_ptr(f, b0, root, 0, i32t), i32t) : NULL;
        IrValue *r = ir_binop(f, b0, IR_SUB, got, ir_const_int(f, b0, c == 7 ? 3 : c >= 5 ? 5 : 10, i32t), i32t);
        if (sn5) r = ir_binop(f, b0, IR_ADD, r, sn5, i32t);
        ir_set_ret(b0, r);
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), cellnm[48], stored[96];
        snprintf(cellnm, sizeof cellnm, "%%s%d%s@", root->id, path);
        // the load's version is a store's (`@bb0.N`) defined equal to the constant stored
        const char *v = t ? strstr(t, " narrow.hi 0") : NULL;
        bool named = v && strstr(v, cellnm), fresh = v && strstr(v, "@havoc");
        snprintf(stored, sizeof stored, "hyp h4 store %%s%d%s@", root->id, path);
        char sn[48]; snprintf(sn, sizeof sn, "store %%s%d.0@bb", root->id);
        char tn[24]; snprintf(tn, sizeof tn, "%%v%d ", tmp ? tmp->id : -1);
        switch (c) {
            case 0: case 7: CHECK(named && !fresh && strstr(v, stored), "case %d: the load is the stored version of %s:\n%s", c, cellnm, t ? t : ""); break;
            case 1: case 3: case 4: CHECK(named && fresh, "case %d: a store or call through a[i] makes a[0] fresh:\n%s", c, t ? t : ""); break;
            case 2: case 10: CHECK(t && !strstr(t, cellnm), "case %d: %s outside the length is no cell:\n%s", c, path, t ? t : ""); break;
            case 5: CHECK(named && fresh && strstr(t, sn), "case 5: s.arr[i] makes s.arr[0] fresh, s.n keeps its store:\n%s", t ? t : ""); break;
            case 8: CHECK(named && fresh && !strstr(t, sn), "case 8: an unchecked s.arr[i] makes s.n fresh too:\n%s", t ? t : ""); break;
            case 9: CHECK(named && fresh, "case 9: a[i].x = 5 makes a[1].x fresh:\n%s", t ? t : ""); break;
            default: CHECK(named && fresh && !strstr(t, tn), "case 6: s.arr[0] after S(1, t) is fresh, not t's address:\n%s", t ? t : ""); break;
        }
        free(t);
    }
    gen_mod = NULL;
    printf("  element cells: a constant index is a cell, a store through any other index refreshes its array\n");
    // 44. a PRODUCT of two variables between its corners, from the operands' static ranges (a
    // constant's value; through a cast whose source's range fits its type, the source's; else
    // the type's): (0) cast(y i32):i64 * cast(x i32):i64, `*?`'s shape: hull [-2^62 + 2^31, 2^62],
    // owed and stated; (1) i32 * i32 into i32: the corners leave i32, outside; (2) cast(u8):u16 *
    // cast(u8):u16: [0, 65025], owed; (3) cast(u64):i64 * cast(i32):i64: the u64 range does not fit
    // i64, so i64's is used, and the corners leave it: outside; (4) the same as (0) WRAPPING:
    // nothing owed, no hull; (5) a constant factor: the linear row, no hull; (6) case 2 with one
    // u8 made by a WRAPPING cast from an i8: its range is u8's [0, 255], never the i8's
    IrType *i8x = ir_type_int(&A, 8, true), *u16t = ir_type_int(&A, 16, false);
    for (int c = 0; c < 7; c++) {
        IrType *X = c == 1 ? i32t : c == 2 || c == 6 ? u8t : c == 3 ? u64t : i32t;
        IrType *R = c == 1 ? i32t : c == 2 || c == 6 ? u16t : i64t;
        IrFunc *f = ir_func_new(&A, nm("m44"), R, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, c == 6 ? i8x : X, nm("x")), *y = ir_add_param(f, c == 3 ? i32t : X, nm("y"));
        IrBlock *b0 = f->entry;
        IrValue *xs = x;
        if (c == 6) { IrInstr *w = ir_instr(f, IR_CAST, u8t, 1); w->operands[0] = x; w->wrap = IR_WRAP_MODULAR; ir_emit(b0, w); xs = w->result; }
        IrValue *xw = xs, *yw = y;
        if (c != 1 && c != 5) {
            IrInstr *cx = ir_instr(f, IR_CAST, R, 1); cx->operands[0] = xs; ir_emit(b0, cx); xw = cx->result;
            IrInstr *cy = ir_instr(f, IR_CAST, R, 1); cy->operands[0] = y; ir_emit(b0, cy); yw = cy->result;
        }
        if (c == 5) { xw = ir_const_int(f, b0, 3, i32t); yw = y; }
        IrValue *p = ir_binop(f, b0, IR_MUL, xw, yw, R);
        if (c == 4) g_last_instr(b0)->wrap = IR_WRAP_MODULAR;
        // case 4 divides by its product, so the product's facts are stated in the divisor's VC
        ir_set_ret(b0, c == 4 ? ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 100, R), p, R) : p);
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), lo[96], hi[96], hn[48];
        snprintf(hn, sizeof hn, "def %%v%d mul.hull", p->id);
        const char *wantlo = c == 0 ? "4611686016279904256" : "0", *wanthi = c == 0 ? "4611686018427387904" : "65025";
        snprintf(lo, sizeof lo, "%s : -1*%%v%d <= %s\n", hn, p->id, wantlo);
        snprintf(hi, sizeof hi, "%s : 1*%%v%d <= %s\n", hn, p->id, wanthi);
        int n = idx_of(b0, p); char key[48]; snprintf(key, sizeof key, "bb%d.%d narrow.hi 0", b0->id, n);
        const char *v = t ? vc_of(t, key) : "";
        switch (c) {
            case 0: case 2: case 6:
                CHECK(strstr(v, lo) && strstr(v, hi) && !strstr(t, "atom.mul"), "case %d: owed, the hull stated:\n%s", c, t ? t : ""); break;
            case 1: case 3:
                CHECK(t && strstr(t, "outside atom.mul: a product of two variables whose corners leave its type") && !strstr(t, "mul.hull"),
                      "case %d: the corners leave the type, outside:\n%s", c, t ? t : ""); break;
            default: {
                char pv[24]; snprintf(pv, sizeof pv, "%%v%d", p->id);
                CHECK(t && !strstr(t, "mul.hull") && !strstr(t, "atom.mul") && (c != 4 || strstr(t, pv)),
                      "case %d: no hull (case 4's product in a VC):\n%s", c, t ? t : ""); break; }
        }
        free(t);
    }
    gen_mod = NULL;
    printf("  a product of two variables: between its corners, outside where they leave its type\n");
    // 45. a PARAMETER's refinement in its static range: `x * y` into i32 with x, y bounded by
    // ASSUMEs in the entry block's prologue: (0) 0 <= x <= 100 and 0 <= y <= 100: hull [0, 10000],
    // owed; (1) the same assumes after an add: not the prologue, not read, outside; (2) -1 < x and
    // 101 > x (y the same), the constant on the left: as case 0; (3) x <=u 100 on an UNSIGNED u32
    // x (y the same): [0, 10000] into u32; (4) x <=u 100 on a SIGNED x: not read (the orders differ
    // there), outside; (5) x > -1 and x < 101, strict: as case 0; (6) x == 5 and y == 7: [35, 35].
    // The product's OWN hull is at its point (c25), where every dominating assume holds and x >=u 0
    // with x <= 100 agrees with the signed order: in cases 1 and 4 it is [0, 10000] too. The STATIC
    // range is what a nested hull reads: q = p & m, m in [0, 65535] by assumes (at q's point), has an
    // and.hull up to p's static hi, 10000 (35) where the refinements are read, 65535 where they are not
    for (int c = 0; c < 7; c++) {
        IrType *T = c == 3 ? u32t : i32t;
        IrFunc *f = ir_func_new(&A, nm("r45"), T, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, T, nm("x")), *y = ir_add_param(f, T, nm("y")), *m = ir_add_param(f, T, nm("m"));
        IrBlock *b0 = f->entry;
        if (c == 1) (void)ir_binop(f, b0, IR_ADD, x, y, T);
        ir_assume(f, b0, ir_icmp(f, b0, c == 3 ? IR_CMP_UGE : IR_CMP_SGE, m, ir_const_int(f, b0, 0, T)));
        ir_assume(f, b0, ir_icmp(f, b0, c == 3 ? IR_CMP_ULE : IR_CMP_SLE, m, ir_const_int(f, b0, 65535, T)));
        IrValue *vs[2] = { x, y };
        for (int w = 0; w < 2; w++) {
            IrValue *v = vs[w];
            switch (c) {
                case 0: case 1:
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, v, ir_const_int(f, b0, 0, T)));
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, v, ir_const_int(f, b0, 100, T))); break;
                case 2:                                        // strict, so a mirror's error moves the corners
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLT, ir_const_int(f, b0, -1, T), v));
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGT, ir_const_int(f, b0, 101, T), v)); break;
                case 3: case 4:
                    if (c == 4) ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, v, ir_const_int(f, b0, 0, T)));
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_ULE, v, ir_const_int(f, b0, 100, T))); break;
                case 5:
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGT, v, ir_const_int(f, b0, -1, T)));
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLT, v, ir_const_int(f, b0, 101, T))); break;
                default:
                    ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_EQ, v, ir_const_int(f, b0, w ? 7 : 5, T))); break;
            }
        }
        IrValue *p = ir_binop(f, b0, IR_MUL, x, y, T);
        IrValue *q = ir_binop(f, b0, IR_AND, p, m, T);
        ir_set_ret(b0, ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 100, T), q, T));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), lo[96], hi[96], qh[96];
        snprintf(lo, sizeof lo, "def %%v%d mul.hull : -1*%%v%d <= %s\n", p->id, p->id, c == 6 ? "-35" : "0");
        snprintf(hi, sizeof hi, "def %%v%d mul.hull : 1*%%v%d <= %s\n", p->id, p->id, c == 6 ? "35" : "10000");
        snprintf(qh, sizeof qh, "def %%v%d and.hull : 1*%%v%d <= %s\n", q->id, q->id, c == 6 ? "35" : c == 4 || c == 1 ? "65535" : "10000");
        CHECK(t && strstr(t, lo) && strstr(t, hi) && !strstr(t, "atom.mul"), "case %d: the product's hull at its point:\n%s", c, t ? t : "");
        CHECK(t && strstr(t, qh), "case %d: the static range under a nested hull, %s:\n%s", c, qh, t ? t : "");
        free(t);
    }
    gen_mod = NULL;
    printf("  a parameter's refinement: the prologue's assumes narrow its static range\n");
    // 46. a LOAD a[i] by a variable index from an array whose every element is a cell: the value
    // is the element the index names, `or { i = 0 , v = a[0] | ... }` over its length: (0) a local
    // i32[4] filled by constant stores; (1) a[3] never stored at a constant index: a cell all the
    // same (c30), its disjunct its entry version; (2) the index unchecked: nothing; (3) an i32[17], past JELEM_MAXN: nothing; (4) the
    // load on one path into a join, read past it: local to that path, nothing stated alone; (5)
    // `a[j] = 9` before it: the disjunction names the elements' fresh versions
    {   IrType *a17 = ir_type_new(&A, IRT_ARRAY); a17->elem = i32t; a17->array_len = 17;
        for (int c = 0; c < 6; c++) {
            IrFunc *f = ir_func_new(&A, nm("v46"), i32t, IR_FUNC_PURE);
            IrValue *ix = ir_add_param(f, u64t, nm("i")), *jx = ir_add_param(f, u64t, nm("j"));
            IrBlock *b0 = f->entry, *b1 = NULL, *b2 = NULL, *b3 = NULL, *at = b0;
            IrType *AT = c == 3 ? a17 : a4;
            int len = c == 3 ? 17 : 4;
            IrValue *root = ir_alloca_array(f, b0, AT);
            for (int k = 0; k < len; k++)
                if (!(c == 1 && k == 3)) ir_store(f, b0, ir_elem_ptr(f, b0, root, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 30 + k, i32t));
            if (c == 5) ir_store(f, b0, ir_elem_ptr(f, b0, root, jx, i32t), ir_const_int(f, b0, 9, i32t));
            if (c == 4) {
                b1 = ir_new_block(f); b2 = ir_new_block(f); b3 = ir_new_block(f);
                ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, jx, ir_const_int(f, b0, 5, u64t)), b1, b2);
                at = b1;
            }
            IrValue *ep = ir_elem_ptr(f, at, root, ix, i32t);
            if (c == 2) g_last_instr(at)->unchecked = true;
            IrValue *v = ir_load(f, at, ep, i32t);
            IrValue *q;
            if (c == 4) {
                IrValue *cell = ir_alloca(f, b0, i32t);
                ir_store(f, b1, cell, v); ir_set_br(b1, b3);
                ir_store(f, b2, cell, ir_const_int(f, b2, 40, i32t)); ir_set_br(b2, b3);
                q = ir_binop(f, b3, IR_SDIV, ir_const_int(f, b3, 100, i32t), ir_load(f, b3, cell, i32t), i32t);
                ir_set_ret(b3, q);
            } else {
                q = ir_binop(f, at, IR_SDIV, ir_const_int(f, at, 100, i32t), v, i32t);
                ir_set_ret(at, q);
            }
            ir_finalize_cfg(f);
            gen_mod = f;
            char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), hn[48];
            snprintf(hn, sizeof hn, "def %%v%d element : or {", v->id);
            const char *vz = t ? vc_of(t, " divzero 0") : "", *h = strstr(vz, hn);
            if (c == 0 || c == 1 || c == 5) {
                bool ok = h != NULL;
                for (int k = 0; ok && k < 4; k++) {                  // disjunct k: i = k and v = a[k]'s version
                    char want[96];
                    snprintf(want, sizeof want, "1*%%v%d = %d , 1*%%v%d + -1*%%s%d[%d]@%s", ix->id, k, v->id, root->id, k,
                             c == 5 ? "havoc" : c == 1 && k == 3 ? "entry" : "bb");
                    ok = strstr(h, want) != NULL;
                    if (!ok) printf("  missing disjunct %s\n", want);
                }
                CHECK(ok, "case %d: the element the index names:\n%s", c, vz);
            } else CHECK(t && !strstr(t, " element : "), "case %d: no element fact:\n%s", c, t ? t : "");
            free(t);
        }
        gen_mod = NULL;
    }
    printf("  a variable-index load: the element the index names, as a disjunction over the cells\n");
    // 47. a REFERENCE BINDING (`var r = var n`): a slot whose every use is a load or its one store
    // of n's address; each load the store dominates is n's address, so `r = 2` writes n and `r`
    // reads it. Then `return 10 / n`: (0) n's version is the store through r (2); (1) the slot
    // stored twice: not resolved, n is fresh after the unattributed store; (2) the slot's address
    // also handed to a call: not resolved; (3) the store on one path, the load past the join: not
    // dominated, not resolved; (4) a binding to a field s.f (path .1): s.f's version is the store
    {   IrType *pi32 = ir_type_new(&A, IRT_PTR); pi32->elem = i32t; pi32->ptr_mut = true;
        for (int c = 0; c < 5; c++) {
            IrFunc *f = ir_func_new(&A, nm("b47"), i32t, IR_FUNC_PURE);
            IrValue *jx = ir_add_param(f, u64t, nm("j"));
            IrBlock *b0 = f->entry, *at = b0;
            IrValue *root, *target;
            if (c == 4) { root = ir_alloca(f, b0, P43); target = ir_field_ptr(f, b0, root, 1, i32t); }
            else { root = ir_alloca(f, b0, i32t); target = root; }
            ir_store(f, b0, target, ir_const_int(f, b0, 7, i32t));
            IrValue *slot = ir_alloca(f, b0, pi32);
            if (c == 3) {
                IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
                ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, jx, ir_const_int(f, b0, 5, u64t)), b1, b2);
                ir_store(f, b1, slot, target); ir_set_br(b1, b3);
                ir_set_br(b2, b3);
                at = b3;
            } else ir_store(f, b0, slot, target);
            if (c == 1) ir_store(f, b0, slot, target);
            if (c == 2) {
                IrType *pp = ir_type_new(&A, IRT_PTR); pp->elem = pi32; pp->ptr_mut = true;
                IrFunc *callee = ir_func_new(&A, nm("peek47"), u32t, IR_FUNC_PURE);
                ir_add_param(callee, pp, nm("q")); ir_set_ret(callee->entry, ir_const_int(callee, callee->entry, 0, u32t)); ir_finalize_cfg(callee);
                f->next = callee;
                (void)mk_call(f, b0, "peek47", slot, u32t);
            }
            ir_store(f, at, ir_load(f, at, slot, pi32), ir_const_int(f, at, 2, i32t));     // r = 2
            IrValue *n = ir_load(f, at, ir_load(f, at, slot, pi32), i32t);                 // read through r
            ir_set_ret(at, ir_binop(f, at, IR_SDIV, ir_const_int(f, at, 10, i32t), n, i32t));
            ir_finalize_cfg(f);
            gen_mod = f;
            char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), want[64];
            snprintf(want, sizeof want, "store %%s%d%s@", root->id, c == 4 ? ".1" : "");
            const char *vz = t ? vc_of(t, " divzero 0") : "";
            if (c == 0 || c == 4)
                CHECK(strstr(vz, want) && !strstr(vz, "havoc"), "case %d: the store through r is the target's version:\n%s", c, vz);
            else
                CHECK(*vz && !strstr(vz, want), "case %d: not resolved, no store version read through r:\n%s", c, vz);
            free(t);
        }
        gen_mod = NULL;
    }
    printf("  a reference binding: a slot stored once with an address is that address where it dominates\n");
    // 48. a WAIVED narrowing wraps, as it_wrap makes it: x = v - k*2^w within the type's width, k a
    // fresh integer. (0) an unchecked cast i16 -> i8 (k*256); (1) a `%` cast u64 -> u32 (k*2^32); (2)
    // a `|` cast clamps: nothing; (3) a checked cast: the plain `def ... cast`; (4) an unchecked
    // store of an i16 into an i8 slot: the version wraps; (5) an unchecked SIGNED i8 + i8 in an i9
    // cannot leave it: exact; (6) an unchecked i32 + i32 in an i32 may: nothing
    {   IrType *i16t = ir_type_int(&A, 16, true), *i9t = ir_type_int(&A, 9, true), *u32w = ir_type_int(&A, 32, false);
        for (int c = 0; c < 7; c++) {
            IrType *S = c == 1 ? u64t : c >= 5 ? (c == 5 ? i8x : i32t) : i16t;
            IrType *D = c == 1 ? u32w : c == 5 ? i9t : c == 6 ? i32t : i8x;
            IrFunc *f = ir_func_new(&A, nm("w48"), i32t, IR_FUNC_PURE);
            IrValue *a = ir_add_param(f, S, nm("a")), *b2 = ir_add_param(f, S, nm("b"));
            IrBlock *b0 = f->entry;
            IrValue *r; IrInstr *ri;
            if (c <= 3) {
                ri = ir_instr(f, IR_CAST, D, 1); ri->operands[0] = a; ir_emit(b0, ri); r = ri->result;
                if (c == 0) ri->unchecked = true;
                if (c == 1) ri->wrap = IR_WRAP_MODULAR;
                if (c == 2) ri->wrap = IR_WRAP_SATURATE;
            } else if (c == 4) {
                IrValue *slot = ir_alloca(f, b0, i8x);
                ir_store(f, b0, slot, a); b0->instrs_tail->unchecked = true;
                r = ir_load(f, b0, slot, i8x);
            } else {
                r = ir_binop(f, b0, IR_ADD, a, b2, D); g_last_instr(b0)->unchecked = true;
            }
            IrInstr *wd = ir_instr(f, IR_CAST, i32t, 1); wd->operands[0] = r; ir_emit(b0, wd);     // widen, then divide
            ir_set_ret(b0, ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 100, i32t), wd->result, i32t));
            ir_finalize_cfg(f);
            gen_mod = f;
            char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), want[128];
            const char *vz = t ? vc_of(t, " divzero 0") : "";
            switch (c) {
                case 0: case 1:
                    snprintf(want, sizeof want, "def %%v%d cast.wrap : 1*%%v%d + -1*%%v%d + %s*wrap.%%v%d = 0", r->id, r->id, a->id, c ? "4294967296" : "256", r->id);
                    CHECK(strstr(vz, want) && strstr(vz, c ? "cast.wrap : 1*" : "cast.wrap : -1*"), "case %d: the cast wraps:\n%s", c, vz); break;
                case 2: snprintf(want, sizeof want, "def %%v%d cast", r->id); CHECK(*vz && !strstr(vz, want), "case 2: a clamp states nothing:\n%s", vz); break;
                case 3: snprintf(want, sizeof want, "def %%v%d cast :", r->id); CHECK(strstr(vz, want) && !strstr(vz, "wrap"), "case 3: a checked cast is exact:\n%s", vz); break;
                case 4: CHECK(strstr(vz, " wrap : ") && strstr(vz, "256*wrap.ver"), "case 4: the waived store wraps into its slot:\n%s", vz); break;
                case 5: snprintf(want, sizeof want, "def %%v%d add :", r->id); CHECK(strstr(vz, want), "case 5: i8 + i8 in an i9 is exact:\n%s", vz); break;
                default: snprintf(want, sizeof want, "def %%v%d add", r->id); CHECK(*vz && !strstr(vz, want), "case 6: i32 + i32 in an i32 states nothing:\n%s", vz); break;
            }
            free(t);
        }
        gen_mod = NULL;
    }
    printf("  a waived narrowing wraps: x = v - k*2^w within the width, k fresh\n");
    // 49. a DIVISION or REMAINDER by a variable divisor, between bounds from the static ranges:
    // (0) 100 / b, b any i32 (an i33 quotient): |q| <= 100; (1) x in [0, 50] / y in [2, 5]: the
    // corners, [0, 25]; (2) x in [-10, 10] / y any i32 (y takes both signs): |q| <= 10 only; (3) x
    // in [0, 1000] % y in [1, 7]: [0, 6]; (4) x in [-10, 3] % y in [-100, 100]: [-10, 3]; (5) (a +
    // b) % (0 - 4) over u8 a, b in an i32: x folds to [0, 510], y to [-4, -4]: [0, 3]; (6) a signed
    // UNCHECKED division: nothing; (7) a constant divisor: the exact row, no hull; (8) (a + b) / (0 -
    // 4) as case 5: [-127, 0]; (9) an UNCHECKED i8 100 + 100 is not folded (it may wrap): x / y, y
    // any i8, |q| <= 128 from x's type; (10) I.198's mixed signs, an i32 x % a u32 y in i64 (each
    // extended by its own signedness): [-2^31, 2^31 - 1], x's own ends (|r| <= max|x| was 2^31);
    // (11) x in [5, 10] % y in [1, 3]: [0, 2], not x's lo; (12) x in [-10, -5] % y in [-3, 3]: [-2, 0];
    // (13) x in [-3, 10] % y in [-100, 100]: [-3, 10], case 4 mirrored
    {   IrType *i33t = ir_type_int(&A, 33, true);
        static const struct { int lo, hi; } X[14] = { { 100, 100 }, { 0, 50 }, { -10, 10 }, { 0, 1000 }, { -10, 3 }, { 0, 0 }, { 0, 50 }, { 0, 50 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 5, 10 }, { -10, -5 }, { -3, 10 } },
                                            Y[14] = { { 0, 0 }, { 2, 5 }, { 0, 0 }, { 1, 7 }, { -100, 100 }, { 0, 0 }, { 2, 5 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 1, 3 }, { -3, 3 }, { -100, 100 } };
        static const char *want[14] = { "-100 100", "0 25", "-10 10", "0 6", "-10 3", "0 3", "", "", "-127 0", "-128 128", "-2147483648 2147483647", "0 2", "-2 0",
                                        "-3 10" };
        for (int c = 0; c < 14; c++) {
            IrType *T = c == 0 ? i33t : c == 9 ? i8x : c == 10 ? i64t : i32t;
            IrFunc *f = ir_func_new(&A, nm("d49"), T, IR_FUNC_PURE);
            IrValue *x = ir_add_param(f, c == 5 || c == 8 ? u8t : c == 10 ? i32t : T, nm("x")), *y = ir_add_param(f, c == 5 || c == 8 ? u8t : c == 10 ? u32t : T, nm("y"));
            IrBlock *b0 = f->entry;
            if (c != 0 && c != 2 && c != 5 && c != 8 && c != 9 && c != 10) { // the refinements, in the prologue
                ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, x, ir_const_int(f, b0, X[c].lo, T)));
                ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, x, ir_const_int(f, b0, X[c].hi, T)));
                if (c != 7) { ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, y, ir_const_int(f, b0, Y[c].lo, T)));
                              ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, y, ir_const_int(f, b0, Y[c].hi, T))); }
            }
            if (c == 2) { ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, x, ir_const_int(f, b0, -10, T)));
                          ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, x, ir_const_int(f, b0, 10, T))); }
            IrValue *xs = c == 0 ? ir_const_int(f, b0, 100, T) : x, *ys = y;
            if (c == 9) { xs = ir_binop(f, b0, IR_ADD, ir_const_int(f, b0, 100, i8x), ir_const_int(f, b0, 100, i8x), i8x); g_last_instr(b0)->unchecked = true; }
            if (c == 5 || c == 8) {
                IrInstr *ca = ir_instr(f, IR_CAST, i32t, 1); ca->operands[0] = x; ir_emit(b0, ca);
                IrInstr *cb = ir_instr(f, IR_CAST, i32t, 1); cb->operands[0] = y; ir_emit(b0, cb);
                xs = ir_binop(f, b0, IR_ADD, ca->result, cb->result, i32t);
                ys = ir_binop(f, b0, IR_SUB, ir_const_int(f, b0, 0, i32t), ir_const_int(f, b0, 4, i32t), i32t);
            }
            if (c == 7) ys = ir_const_int(f, b0, 4, T);
            if (c == 10) {
                IrInstr *ca = ir_instr(f, IR_CAST, i64t, 1); ca->operands[0] = x; ir_emit(b0, ca);
                IrInstr *cb = ir_instr(f, IR_CAST, i64t, 1); cb->operands[0] = y; ir_emit(b0, cb);
                xs = ca->result; ys = cb->result;
            }
            IrValue *q = ir_binop(f, b0, c == 3 || c == 4 || c == 5 || c >= 10 ? IR_SREM : IR_SDIV, xs, ys, c == 5 || c == 8 ? i32t : T);
            if (c == 6) g_last_instr(b0)->unchecked = true;
            IrInstr *w = ir_instr(f, IR_CAST, i64t, 1); w->operands[0] = q; ir_emit(b0, w);
            IrValue *rv = ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 1000, i64t), w->result, i64t);   // q in a VC
            IrInstr *back = ir_instr(f, IR_CAST, T, 1); back->operands[0] = rv; back->unchecked = true; ir_emit(b0, back);
            ir_set_ret(b0, back->result);
            ir_finalize_cfg(f);
            gen_mod = f;
            char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), lo[96], hi[96];
            const char *hn = c == 3 || c == 4 || c == 5 || c >= 10 ? "rem.hull" : "div.hull";
            long long wl = 0, wh = 0; sscanf(want[c], "%lld %lld", &wl, &wh);
            snprintf(lo, sizeof lo, "def %%v%d %s : -1*%%v%d <= %lld\n", q->id, hn, q->id, -wl);
            snprintf(hi, sizeof hi, "def %%v%d %s : 1*%%v%d <= %lld\n", q->id, hn, q->id, wh);
            if (c == 6 || c == 7) CHECK(t && !strstr(t, ".hull"), "case %d: no hull:\n%s", c, t ? t : "");
            else CHECK(t && strstr(t, lo) && strstr(t, hi), "case %d: q in [%lld, %lld]:\n%s", c, wl, wh, t ? t : "");
            free(t);
        }
        gen_mod = NULL;
    }
    printf("  a division or remainder by a variable divisor: bounds from the static ranges\n");
    // 50. a JOIN's disjuncts are its PATHS from its dominator: one per path through an inner join,
    // one per case of a condition that is itself a disjunction, so no disjunct holds one. A cell r
    // set on each path, read past the join S by `100 / r`: (0) if x < 0 { r = 1 } else { if x < 5
    // { r = 2 } else { r = 3 } }: 3 disjuncts, the inner join's r in the two inner ones; (1) if x
    // != 0 { r = 1 } else { r = 2 }: the `!=` arm split, 3 disjuncts; (2) if y <u x { r = 1 } else
    // { r = 2 } with y and x i32: each arm split on both signs, 4 + 4, the shifted compare in them;
    // (3) the else arm six two-way ifs in a row (65 paths, past the cap): one disjunct per
    // predecessor, 2. In case 1 the `!=` arm's two cases are x <= -1 and x >= 1
    for (int c = 0; c < 4; c++) {
        IrFunc *f = ir_func_new(&A, nm("j50"), i32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i32t, nm("x")), *y = ir_add_param(f, i32t, nm("y"));
        IrBlock *b0 = f->entry;
        IrValue *r = ir_alloca(f, b0, i32t);
        IrBlock *at = b0;
        IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f), *S = ir_new_block(f);
        int inner = -1;                                        // the inner join's block (case 0)
        IrValue *cnd = c == 1 ? ir_icmp(f, at, IR_CMP_NE, x, ir_const_int(f, at, 0, i32t))
                     : c == 2 ? ir_icmp(f, at, IR_CMP_ULT, y, x) : ir_icmp(f, at, IR_CMP_SLT, x, ir_const_int(f, at, 0, i32t));
        ir_set_br_cond(at, cnd, b1, b2);
        ir_store(f, b1, r, ir_const_int(f, b1, 1, i32t)); ir_set_br(b1, S);
        if (c == 0) {
            IrBlock *b3 = ir_new_block(f), *b4 = ir_new_block(f), *b5 = ir_new_block(f);
            ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_SLT, x, ir_const_int(f, b2, 5, i32t)), b3, b4);
            ir_store(f, b3, r, ir_const_int(f, b3, 2, i32t)); ir_set_br(b3, b5);
            ir_store(f, b4, r, ir_const_int(f, b4, 3, i32t)); ir_set_br(b4, b5);
            ir_set_br(b5, S);
            inner = b5->id;
        } else if (c == 3) {                                   // 6 diamonds in a row in the else arm
            IrValue *tc = ir_alloca(f, b0, i32t);
            IrBlock *e = b2;
            for (int k = 0; k < 6; k++) {
                IrBlock *t1 = ir_new_block(f), *t2 = ir_new_block(f), *t3 = ir_new_block(f);
                ir_set_br_cond(e, ir_icmp(f, e, IR_CMP_SLT, y, ir_const_int(f, e, k, i32t)), t1, t2);
                ir_store(f, t1, tc, ir_const_int(f, t1, k, i32t)); ir_set_br(t1, t3);
                ir_store(f, t2, tc, ir_const_int(f, t2, k + 10, i32t)); ir_set_br(t2, t3);
                e = t3;
            }
            ir_store(f, e, r, ir_const_int(f, e, 2, i32t)); ir_set_br(e, S);
        } else { ir_store(f, b2, r, ir_const_int(f, b2, 2, i32t)); ir_set_br(b2, S); }
        ir_set_ret(S, ir_binop(f, S, IR_SDIV, ir_const_int(f, S, 100, i32t), ir_load(f, S, r, i32t), i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), jn[32];
        snprintf(jn, sizeof jn, "join bb%d : or {", S->id);
        const char *vz = t ? vc_of(t, " divzero 0") : "", *h = strstr(vz, jn);
        int bars = 0;
        for (const char *q = h; q && *q && *q != '\n'; q++) if (*q == '|') bars++;
        int want = c == 3 ? 1 : c == 2 ? 7 : 2;                // disjuncts - 1
        char ie[64]; snprintf(ie, sizeof ie, "1*%%s%d@join.bb%d + -1*%%s%d@bb", r->id, inner, r->id);   // the inner join's own
        bool ok = c != 0 || (h && strstr(h, ie));
        if (c == 2) ok = h && strstr(h, "18446744073709551");   // a case past 0: the 2^64-shifted compare
        if (c == 1) {                                          // both ways round 0
            char ne1[48], ne2[48]; snprintf(ne1, sizeof ne1, "1*%%v%d + -1*%%v", x->id); snprintf(ne2, sizeof ne2, "-1*%%v%d + 1*%%v", x->id);
            ok = h && strstr(h, ne1) && strstr(h, ne2);
        }
        CHECK(h && bars == want && ok, "case %d: %d disjuncts (got %d):\n%s", c, want + 1, bars + 1, vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  a join's disjuncts are its paths: inner joins and split conditions distributed\n");
    // 51. BITWISE and SHIFT hulls from the static ranges, which now evaluate through every hulled
    // operation: (0) x & y, x in [0, 10] and y any i32: [0, 10]; (1) x | y over u8: [0, 255]; (2)
    // x ^ y, x in [-3, 5] and y in [-1, 2]: [-8, 7]; (3) x << k, x in [-3, 5], k in [0, 3]: [-24,
    // 40]; (4) x << k unchecked: nothing; (5) ((x ^ 3) % 7) | 8 over u8, the or's operand a
    // remainder's range: [8, 15]; (6) x << k, x in [-3, 5], k any i32: k in [0, 31] (its shift
    // obligation), [-3 * 2^31, 5 * 2^31]; (7) x ^ y, x in [-3, 4] (4 needs the 4th bit) and y in
    // [-1, 2]: [-8, 7], never [-4, 3]
    for (int c = 0; c < 8; c++) {
        IrType *T = c == 1 || c == 5 ? u8t : c == 2 || c == 7 ? i8x : i32t;
        IrFunc *f = ir_func_new(&A, nm("b51"), T, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, T, nm("x")), *y = ir_add_param(f, T, nm("y"));
        IrBlock *b0 = f->entry;
        static const int lo[8] = { 0, 0, -3, -3, -3, 0, -3, -3 }, hi[8] = { 10, 0, 5, 5, 5, 0, 5, 4 }, ylo[8] = { 0, 0, -1, 0, 0, 0, 0, -1 }, yhi[8] = { 0, 0, 2, 3, 3, 0, 0, 2 };
        if (c != 1 && c != 5) {
            ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, x, ir_const_int(f, b0, lo[c], T)));
            ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, x, ir_const_int(f, b0, hi[c], T)));
            if (c >= 2 && c != 6) { ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, y, ir_const_int(f, b0, ylo[c], T)));
                          ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, y, ir_const_int(f, b0, yhi[c], T))); }
        }
        IrValue *p;
        if (c == 5) {
            IrValue *xr = ir_binop(f, b0, IR_XOR, x, ir_const_int(f, b0, 3, T), T);
            IrValue *rm = ir_binop(f, b0, IR_UREM, xr, ir_const_int(f, b0, 7, T), T);
            p = ir_binop(f, b0, IR_OR, rm, ir_const_int(f, b0, 8, T), T);
        } else {
            IrOp op = c == 0 ? IR_AND : c == 1 ? IR_OR : c == 2 || c == 7 ? IR_XOR : IR_SHL;
            p = ir_binop(f, b0, op, x, y, T);
            if (c == 4) g_last_instr(b0)->unchecked = true;
        }
        IrInstr *w = ir_instr(f, IR_CAST, i64t, 1); w->operands[0] = p; ir_emit(b0, w);
        IrValue *q = ir_binop(f, b0, IR_SDIV, ir_const_int(f, b0, 1000, i64t), w->result, i64t);
        IrInstr *bk = ir_instr(f, IR_CAST, T, 1); bk->operands[0] = q; bk->unchecked = true; ir_emit(b0, bk);
        ir_set_ret(b0, bk->result);
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), wl[96], wh[96];
        static const char *hn[8] = { "and.hull", "or.hull", "xor.hull", "shl.hull", "shl.hull", "or.hull", "shl.hull", "xor.hull" };
        static const long long want[8][2] = { { 0, 10 }, { 0, 255 }, { -8, 7 }, { -24, 40 }, { 0, 0 }, { 8, 15 }, { -6442450944LL, 10737418240LL }, { -8, 7 } };
        snprintf(wl, sizeof wl, "def %%v%d %s : -1*%%v%d <= %lld\n", p->id, hn[c], p->id, -want[c][0]);
        snprintf(wh, sizeof wh, "def %%v%d %s : 1*%%v%d <= %lld\n", p->id, hn[c], p->id, want[c][1]);
        if (c == 4) CHECK(t && !strstr(t, ".hull"), "case 4: an unchecked shift states nothing:\n%s", t ? t : "");
        else CHECK(t && strstr(t, wl) && strstr(t, wh), "case %d: [%lld, %lld]:\n%s", c, want[c][0], want[c][1], t ? t : "");
        free(t);
    }
    // a goal of constants: `if y > 3 { return x / D }`: D = 0 is false, so the guard (about y, not the
    // goal) is pulled in; D = 4 is true and pulls in nothing
    for (int c = 0; c < 2; c++) {
        IrFunc *f = ir_func_new(&A, nm("k51"), i32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i32t, nm("x")), *y = ir_add_param(f, i32t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SGT, y, ir_const_int(f, b0, 3, i32t)), b1, b2);
        ir_set_ret(b1, ir_binop(f, b1, IR_SDIV, x, ir_const_int(f, b1, c ? 4 : 0, i32t), i32t));
        ir_set_ret(b2, ir_const_int(f, b2, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        const char *vz = t ? vc_of(t, " divzero 0") : "";
        CHECK(*vz && !!strstr(vz, "guard") == (c == 0), "a %s constant goal %s the guard:\n%s", c ? "true" : "false", c ? "pulls in nothing, not" : "pulls in", vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  bitwise and shift hulls, and static ranges through every hulled operation\n");
    // 52. a module constant TABLE (an ALLOCA with data, read-only): (0) T[1] of [44, 4, 255] (u8)
    // is 4; (1) T[i] is one of the three, by i; (2) T[i] of 20 elements is between the least and the
    // greatest; (3) T[0] of a u64 table holding 2^64 - 1 (stored as -1) is 18446744073709551615;
    // (4) T[i] unchecked: nothing; (5) T[5], past its three: nothing; (6) T[i] on one path into a
    // join, read past it: local to that path, no disjunction stated alone
    for (int c = 0; c < 7; c++) {
        int n = c == 2 ? 20 : 3;
        IrType *E = c == 3 ? u64t : u8t, *AT = ir_type_new(&A, IRT_ARRAY); AT->elem = E; AT->array_len = n;
        IrData *td = arena_push_aligned(&A, IrData); memset(td, 0, sizeof *td);
        td->type = AT; td->n = n; td->vals = arena_push_many_aligned(&A, int64_t, n);
        for (int k = 0; k < n; k++) td->vals[k] = c == 3 ? -1 : c == 2 ? 10 + k : (k == 0 ? 44 : k == 1 ? 4 : 255);
        IrFunc *f = ir_func_new(&A, nm("t52"), E, IR_FUNC_PURE);
        IrValue *ix = ir_add_param(f, u64t, nm("i"));
        IrBlock *b0 = f->entry;
        IrValue *tb = ir_alloca_array(f, b0, AT); g_last_instr(b0)->data = td;
        IrBlock *at = b0, *b3 = NULL;
        IrValue *cell = NULL;
        if (c == 6) {
            IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f); b3 = ir_new_block(f);
            cell = ir_alloca(f, b0, E);
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, ix, ir_const_int(f, b0, 2, u64t)), b1, b2);
            ir_store(f, b2, cell, ir_const_int(f, b2, 7, E)); ir_set_br(b2, b3);
            at = b1;
        }
        IrValue *idx = c == 0 ? ir_const_int(f, at, 1, u64t) : c == 3 ? ir_const_int(f, at, 0, u64t) : c == 5 ? ir_const_int(f, at, 5, u64t) : ix;
        IrValue *ep = ir_elem_ptr(f, at, tb, idx, E);
        if (c == 4) g_last_instr(at)->unchecked = true;
        IrValue *v = ir_load(f, at, ep, E);
        if (c == 6) {
            ir_store(f, at, cell, v); ir_set_br(at, b3);
            ir_set_ret(b3, ir_binop(f, b3, IR_UDIV, ir_const_int(f, b3, 100, E), ir_load(f, b3, cell, E), E));
        } else ir_set_ret(b0, ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, E), v, E));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), want[96];
        const char *vz = t ? vc_of(t, " divzero 0") : "";
        switch (c) {
            case 0: case 3: snprintf(want, sizeof want, "def %%v%d table : 1*%%v%d = %s", v->id, v->id, c ? "18446744073709551615" : "4");
                    CHECK(strstr(vz, want), "case %d: the element's value:\n%s", c, vz); break;
            case 1: snprintf(want, sizeof want, "def %%v%d table : or { 1*%%v%d = 0 , 1*%%v%d = 44 | 1*%%v%d = 1 , 1*%%v%d = 4 | 1*%%v%d = 2 , 1*%%v%d = 255 }",
                             v->id, ix->id, v->id, ix->id, v->id, ix->id, v->id);
                    CHECK(strstr(vz, want), "case 1: the element the index names:\n%s", vz); break;
            case 2: snprintf(want, sizeof want, "def %%v%d table : 1*%%v%d <= 29", v->id, v->id);
                    CHECK(strstr(vz, want) && strstr(vz, "table : -1*") && !strstr(vz, "or {"), "case 2: between 10 and 29:\n%s", vz); break;
            case 6: snprintf(want, sizeof want, "def %%v%d table : or", v->id);
                    CHECK(*vz && strstr(vz, "join bb") && !strstr(vz, want), "case 6: no disjunction alone:\n%s", vz); break;
            default: CHECK(*vz && !strstr(vz, "table"), "case %d: nothing:\n%s", c, vz); break;
        }
        free(t);
    }
    gen_mod = NULL;
    printf("  a constant table: its element at a constant index, one of them at any other\n");
    // 53. (0) a slot holding a slice made over n: a guard `5 < a.len`, read through a load of the
    // slot, is about the length a bound names through ANOTHER load of it (both the stored slice's,
    // as jb_len names them): the guard joins the bound's VC; (1) x & (y - 1) over u64: at most y - 1;
    // (2) x % y over u64: below y; (3) x % y over i32, x >= 0 and y >= 1: below y; with no fact
    // where (4) neither, (5) only y >= 1, (6) only x >= 0 is known; (7) x & y over i32: only
    // where an operand is >= 0, a disjunction: y <= -1, or 0 <= x & y <= y (8)
    // x / y over u64 with y >= 1: none (100 / 2 is 50)
    {   IrType *slu8 = ir_type_new(&A, IRT_SLICE); slu8->elem = u8t;
        IrFunc *f = ir_func_new(&A, nm("v53"), u8t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u64t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f);
        IrValue *slot = ir_alloca(f, b0, slu8);
        IrValue *data = ir_alloca_dyn(f, b0, u8t, n);
        IrInstr *mk = ir_instr(f, IR_MAKE_SLICE, slu8, 2); mk->operands[0] = data; mk->operands[1] = n; ir_emit(b0, mk);
        ir_store(f, b0, slot, mk->result);
        IrValue *ln = ir_slice_len(f, b0, ir_load(f, b0, slot, slu8));
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, ir_const_int(f, b0, 5, u64t), ln), b1, b2);
        IrValue *sd = ir_slice_data(f, b1, ir_load(f, b1, slot, slu8), u8t);
        IrValue *ep = ir_elem_ptr(f, b1, sd, ir_const_int(f, b1, 5, u64t), u8t);
        ir_set_ret(b1, ir_load(f, b1, ep, u8t));
        ir_set_ret(b2, ir_const_int(f, b2, 0, u8t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        const char *vb = t ? vc_of(t, " bounds.hi 0") : "";
        CHECK(strstr(vb, "guard bb0->bb") != NULL, "case 0: the guard on a.len joins the bound's VC:\n%s", vb);
        free(t);
    }
    for (int c = 1; c < 9; c++) {
        IrType *T = c <= 2 || c == 8 ? u64t : i32t;
        IrFunc *f = ir_func_new(&A, nm("r53"), T, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, T, nm("x")), *y = ir_add_param(f, T, nm("y"));
        IrBlock *b0 = f->entry;
        if (c == 3 || c == 6) ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, x, ir_const_int(f, b0, 0, T)));
        if (c == 3 || c == 5 || c == 8) ir_assume(f, b0, ir_icmp(f, b0, c == 8 ? IR_CMP_UGE : IR_CMP_SGE, y, ir_const_int(f, b0, 1, T)));
        IrValue *m = c == 1 ? ir_binop(f, b0, IR_SUB, y, ir_const_int(f, b0, 1, T), T) : y;
        bool isand = c == 1 || c == 7;
        IrValue *r = ir_binop(f, b0, isand ? IR_AND : c == 2 ? IR_UREM : c == 8 ? IR_UDIV : IR_SREM, x, m, T);
        ir_set_ret(b0, ir_binop(f, b0, c <= 2 || c == 8 ? IR_UDIV : IR_SDIV, ir_const_int(f, b0, 100, T), r, T));
        int at = nins(b0) - 1;
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), want[96], key[48];
        snprintf(want, sizeof want, "%s : 1*%%v%d + -1*%%v%d <= %s", isand ? "and.hull" : c == 8 ? "div.hull" : "rem.hull", r->id, m->id, isand ? "0" : "-1");
        snprintf(key, sizeof key, " bb0.%d divzero 0", at);
        const char *vz = t ? vc_of(t, key) : "";
        bool has = c <= 3;
        CHECK(*vz && !!strstr(vz, want) == has, "case %d: %s%s:\n%s", c, has ? "" : "no ", want, vz);
        if (c == 7) {                                          // y <= -1 or x & y <= y, both ways
            char w2[96];
            snprintf(w2, sizeof w2, "and.hull : or { 1*%%v%d <= -1 | -1*%%v%d <= 0 , 1*%%v%d + -1*%%v%d <= 0 }", m->id, r->id, r->id, m->id);
            CHECK(strstr(vz, w2) != NULL, "case 7: %s:\n%s", w2, vz);
            snprintf(w2, sizeof w2, "and.hull : or { 1*%%v%d <= -1 | -1*%%v%d <= 0 , 1*%%v%d + -1*%%v%d <= 0 }", x->id, r->id, r->id, x->id);
            CHECK(strstr(vz, w2) != NULL, "case 7: %s:\n%s", w2, vz);
        }
        free(t);
    }
    gen_mod = NULL;
    printf("  a slice slot's length one variable for relevance; and and remainder below their operands\n");
    // 54. a quotient's and a remainder's relation to the dividend x >= 0: over y in [lo, hi] with
    // lo >= 1, lo*q <= x <= hi*q + hi - 1; a remainder r <= x. (0) x / y over u32, y in [2, 8];
    // (1) over u64, y any (an unsigned divisor is >= 1 where the division completes); (2) x / y
    // over i32 with x >= 0 and y >= 3; (3) the same with y any: none; (4) x % y over u64; (5) over
    // i32 with x >= 0, y any; (6) over i32, x any: none. (7) A guard `i < n / 2` joins the VC of a
    // goal on n - 1 (not naming i), its quotient's statement naming n; (8) and `i < n * 2` as
    // before.
    for (int c = 0; c < 9; c++) {
        IrType *T = c == 0 ? u32t : c == 1 || c == 4 || c >= 7 ? u64t : i32t;
        bool sg = T == i32t, dv = c <= 3, gd = c >= 7;
        IrFunc *f = ir_func_new(&A, nm("q54"), T, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, T, nm("x")), *y = ir_add_param(f, T, nm("y"));
        IrBlock *b0 = f->entry;
        if (c == 0) { ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_UGE, y, ir_const_int(f, b0, 2, T)));
                      ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_ULE, y, ir_const_int(f, b0, 8, T))); }
        if (c == 2 || c == 3 || c == 5) ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, x, ir_const_int(f, b0, 0, T)));
        if (c == 2) ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, y, ir_const_int(f, b0, 3, T)));
        if (gd) {                                              // x is n, y is i
            IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f);
            IrValue *q = ir_binop(f, b0, c == 7 ? IR_UDIV : IR_MUL, x, ir_const_int(f, b0, 2, T), T);
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, y, q), b1, b2);
            IrValue *df = ir_binop(f, b1, IR_SUB, x, ir_const_int(f, b1, 1, T), T);
            ir_set_ret(b1, ir_binop(f, b1, IR_UDIV, ir_const_int(f, b1, 100, T), df, T));
            ir_set_ret(b2, ir_const_int(f, b2, 0, T));
            int at = nins(b1) - 1;
            ir_finalize_cfg(f);
            gen_mod = f;
            char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48];
            snprintf(key, sizeof key, " bb1.%d divzero 0", at);
            const char *vz = t ? vc_of(t, key) : "";
            CHECK(strstr(vz, "guard bb0->bb1") != NULL, "case %d: the guard on i < n %s 2 joins:\n%s", c, c == 7 ? "/" : "*", vz);
            free(t);
            continue;
        }
        IrValue *r = ir_binop(f, b0, dv ? (sg ? IR_SDIV : IR_UDIV) : (sg ? IR_SREM : IR_UREM), x, y, T);
        ir_set_ret(b0, ir_binop(f, b0, sg ? IR_SDIV : IR_UDIV, ir_const_int(f, b0, 100, T), r, T));
        int at = nins(b0) - 1;
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), w1[128], w2[128], key[48];
        snprintf(key, sizeof key, " bb0.%d divzero 0", at);
        const char *vz = t ? vc_of(t, key) : "";
        const char *o = dv ? "div.hull" : "rem.hull";
        long long lo = c == 0 ? 2 : c == 2 ? 3 : 1, hi = c == 0 ? 8 : c == 1 ? -1 : 2147483647;
        if (dv) {
            snprintf(w1, sizeof w1, "%s : %lld*%%v%d + -1*%%v%d <= 0", o, lo, r->id, x->id);
            if (c == 1) snprintf(w2, sizeof w2, "%s : -18446744073709551615*%%v%d + 1*%%v%d <= 18446744073709551614", o, r->id, x->id);
            else snprintf(w2, sizeof w2, "%s : %lld*%%v%d + 1*%%v%d <= %lld", o, -hi, r->id, x->id, hi - 1);
        } else {
            snprintf(w1, sizeof w1, "%s : 1*%%v%d + -1*%%v%d <= 0", o, r->id, x->id);
            snprintf(w2, sizeof w2, "%s", w1);
        }
        bool has = c != 3 && c != 6;
        CHECK(*vz && !!strstr(vz, w1) == has && !!strstr(vz, w2) == has, "case %d: %s%s and %s:\n%s", c, has ? "" : "no ", w1, w2, vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  a quotient and a remainder against their dividend; a guard on a quotient joins\n");
    // 55. a bit count and its operand m: (0) popcount over u32, m <= (2^32 - 1)*p; (1) over i8 any,
    // m <= 127*p and -m <= 128*p; (2) ctz over u8, one disjunct per k with m >= 2^k; (3) clz over
    // u8, 2^(7-k) <= m <= 2^(8-k) - 1; (4) ctz over i8 any: no disjunction; (5) 100 / y, y in
    // [0, 8] over u32, is in [12, 100] (y is nonzero where the division completes); (6) x / y over
    // i32 with x >= 0 and y >= 0 (so >= 1): q <= x
    for (int c = 0; c < 7; c++) {
        IrType *T = c == 0 || c == 5 ? u32t : c == 2 || c == 3 ? u8t : c == 6 ? i32t : i8x, *R = c == 6 ? i32t : u32t;
        IrFunc *f = ir_func_new(&A, nm("b55"), R, IR_FUNC_PURE);
        IrValue *m = ir_add_param(f, T, nm("m"));
        IrBlock *b0 = f->entry;
        IrValue *p, *x6 = NULL;
        if (c == 6) {
            x6 = ir_add_param(f, T, nm("x"));
            ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, x6, ir_const_int(f, b0, 0, T)));
            ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, m, ir_const_int(f, b0, 0, T)));
            p = ir_binop(f, b0, IR_SDIV, x6, m, T);
        } else if (c == 5) {
            ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_ULE, m, ir_const_int(f, b0, 8, T)));
            p = ir_binop(f, b0, IR_UDIV, ir_const_int(f, b0, 100, T), m, T);
        } else p = ir_bitcount(f, b0, c <= 1 ? IR_POPCOUNT : c == 3 ? IR_CLZ : IR_CTZ, m, u32t);
        ir_set_ret(b0, ir_binop(f, b0, c == 6 ? IR_SDIV : IR_UDIV, ir_const_int(f, b0, 1000, R), p, R));
        int at = nins(b0) - 1;
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), w1[160], w2[160], key[48];
        snprintf(key, sizeof key, " bb0.%d divzero 0", at);
        const char *vz = t ? vc_of(t, key) : "";
        w2[0] = 0;
        if (c == 0) snprintf(w1, sizeof w1, "popcount : -4294967295*%%v%d + 1*%%v%d <= 0", p->id, m->id);
        if (c == 1) { snprintf(w1, sizeof w1, "popcount : -127*%%v%d + 1*%%v%d <= 0", p->id, m->id);
                      snprintf(w2, sizeof w2, "popcount : -128*%%v%d + -1*%%v%d <= 0", p->id, m->id); }
        if (c == 2) { snprintf(w1, sizeof w1, "| 1*%%v%d = 3 , -1*%%v%d <= -8 |", p->id, m->id);
                      snprintf(w2, sizeof w2, "| 1*%%v%d = 7 , -1*%%v%d <= -128 }", p->id, m->id); }
        if (c == 3) { snprintf(w1, sizeof w1, "or { 1*%%v%d = 0 , -1*%%v%d <= -128 , 1*%%v%d <= 255 |", p->id, m->id, m->id);
                      snprintf(w2, sizeof w2, "| 1*%%v%d = 2 , -1*%%v%d <= -32 , 1*%%v%d <= 63 |", p->id, m->id, m->id); }
        if (c == 4) snprintf(w1, sizeof w1, "bitcount : or {");
        if (c == 6) snprintf(w1, sizeof w1, "div.hull : 1*%%v%d + -1*%%v%d <= 0", p->id, x6->id);
        if (c == 5) { snprintf(w1, sizeof w1, "div.hull : -1*%%v%d <= -12", p->id);
                      snprintf(w2, sizeof w2, "div.hull : 1*%%v%d <= 100", p->id); }
        bool has = c != 4;
        CHECK(*vz && !!strstr(vz, w1) == has && (!w2[0] || strstr(vz, w2)), "case %d: %s%s %s:\n%s", c, has ? "" : "no ", w1, w2, vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  a bit count against its operand; a divisor's zero end lifted\n");
    // 56. a slice's data, and an element's neighbours: xs i32[5] stored 10..50, s = MAKE_SLICE(&xs[1],
    // 3) stored once in a slot. (0) s[2] through the slot's load is xs[3], its stored version; (1) the
    // slice used without a slot; (2) &xs[2] by -1 is xs[1]; (3) s[4] would be xs[5], past xs: no
    // cell; (4) a store s[i] = 7 makes xs[3] fresh (it indexes xs, not xs[1]); no cell where (5) the
    // slot is stored twice, (6) its one store is in one arm and the load after the join; (7) &xs[1]
    // by -2 would be xs[-1]: no cell
    for (int c = 0; c < 8; c++) {
        IrType *a5 = ir_type_new(&A, IRT_ARRAY); a5->elem = i32t; a5->array_len = 5;
        IrType *sl = slice_of(i32t);
        IrFunc *f = ir_func_new(&A, nm("s56"), i32t, IR_FUNC_PURE);
        IrValue *ix = ir_add_param(f, u64t, nm("i"));
        IrBlock *b0 = f->entry, *bl = b0;
        IrValue *xs = ir_alloca_array(f, b0, a5);
        for (int k = 0; k < 5; k++)
            ir_store(f, b0, ir_elem_ptr(f, b0, xs, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 10 * (k + 1), i32t));
        IrValue *e1 = ir_elem_ptr(f, b0, xs, ir_const_int(f, b0, 1, u64t), i32t);
        IrInstr *mk = ir_instr(f, IR_MAKE_SLICE, sl, 2); mk->operands[0] = e1; mk->operands[1] = ir_const_int(f, b0, 3, u64t); ir_emit(b0, mk);
        IrValue *sv = mk->result, *got;
        if (c != 1 && c != 2 && c != 7) {
            IrValue *slot = ir_alloca(f, b0, sl);
            if (c == 6) {
                IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
                ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, ix, ir_const_int(f, b0, 3, u64t)), b1, b2);
                ir_store(f, b1, slot, sv); ir_set_br(b1, b3); ir_set_br(b2, b3);
                bl = b3;
            } else {
                ir_store(f, b0, slot, sv);
                if (c == 5) ir_store(f, b0, slot, sv);
            }
            sv = ir_load(f, bl, slot, sl);
        }
        const char *path = c == 2 ? "[1]" : c == 3 ? "[5]" : c == 7 ? "[-1]" : "[3]";
        if (c == 2 || c == 7) got = ir_load(f, bl, ir_elem_ptr(f, bl, ir_elem_ptr(f, bl, xs, ir_const_int(f, bl, c == 2 ? 2 : 1, u64t), i32t),
                                                               ir_const_int(f, bl, c == 2 ? -1 : -2, i64t), i32t), i32t);
        else {
            IrValue *d = ir_slice_data(f, bl, sv, i32t);
            if (c == 4) ir_store(f, bl, ir_elem_ptr(f, bl, d, ix, i32t), ir_const_int(f, bl, 7, i32t));
            got = ir_load(f, bl, c == 4 ? ir_elem_ptr(f, bl, xs, ir_const_int(f, bl, 3, u64t), i32t)
                                        : ir_elem_ptr(f, bl, d, ir_const_int(f, bl, c == 3 ? 4 : 2, u64t), i32t), i32t);
        }
        ir_set_ret(bl, ir_binop(f, bl, IR_SUB, got, ir_const_int(f, bl, 10, i32t), i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), cellnm[48], stored[64];
        snprintf(cellnm, sizeof cellnm, "%%s%d%s@", xs->id, path);
        snprintf(stored, sizeof stored, "store %%s%d%s@", xs->id, path);
        const char *v = t ? strstr(t, " narrow.hi 0") : NULL;
        const char *e = v ? strstr(v, "\nend\n") : NULL;
        char vc[8192] = ""; if (v && e && e - v < (long)sizeof vc) { memcpy(vc, v, (size_t)(e - v)); vc[e - v] = 0; }
        bool named = strstr(vc, cellnm) != NULL, fresh = strstr(vc, "@havoc") != NULL;
        if (c <= 2) CHECK(named && !fresh && strstr(vc, stored), "case %d: the load is the stored version of %s:\n%s", c, cellnm, vc);
        else if (c == 4) CHECK(named && fresh, "case 4: a store through s[i] makes xs[3] fresh:\n%s", vc);
        else CHECK(*vc && !named, "case %d: no cell %s:\n%s", c, cellnm, vc);
        free(t);
    }
    gen_mod = NULL;
    printf("  a slice's data is the address it took; an element's neighbour by a constant is a cell\n");
    // 57. an ASSERT of an unsigned comparison with a SIGNED operand: (0) x = pos + 1 in i34 over a
    // u32 pos (static range [1, 2^32]) `x <=u len`: the comparison is the goal; (1) x an i32
    // parameter (it may be negative, compared as uint64): the bool `!= 0`; (2) x a constant -1
    // (i64): the bool
    for (int c = 0; c < 3; c++) {
        IrType *i34 = ir_type_int(&A, 34, true);
        IrFunc *f = ir_func_new(&A, nm("a57"), u32t, IR_FUNC_PURE);
        IrValue *pos = ir_add_param(f, c == 1 ? i32t : u32t, nm("pos")), *len = ir_add_param(f, u64t, nm("len"));
        IrBlock *b0 = f->entry;
        IrValue *x = c == 0 ? ir_binop(f, b0, IR_ADD, pos, ir_const_int(f, b0, 1, i32t), i34) : c == 1 ? pos : ir_const_int(f, b0, -1, i64t);
        IrValue *q = ir_icmp(f, b0, IR_CMP_ULE, x, len);
        int aq = nins(b0); ir_assert(f, b0, q);
        ir_set_ret(b0, ir_const_int(f, b0, 0, u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48], want[64];
        snprintf(key, sizeof key, " bb0.%d assert.0 0", aq);
        const char *vz = t ? vc_of(t, key) : "";
        if (c == 0) snprintf(want, sizeof want, "goal 1*%%v%d + -1*%%v%d <= 0", x->id, len->id);
        else snprintf(want, sizeof want, "goal 1*%%v%d != 0", q->id);
        CHECK(*vz && strstr(vz, want), "case %d: %s:\n%s", c, want, vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  an assert of an unsigned comparison: a signed operand >= 0 by its static range\n");
    // 58. a load's static range is its store version's stored value's: slots a and b stored 10 and 3,
    // p = load a * load b in i64. (0) p in [30, 30]; (1) a stored 10 or 20 in two arms (a join
    // version): its type's range; (2) a an i8 slot stored 300 by a waived store (it wraps): its
    // type's range; (3) a stored from another slot's load (c stored 10): [30, 30]
    for (int c = 0; c < 4; c++) {
        IrType *i8s = ir_type_int(&A, 8, true);
        IrFunc *f = ir_func_new(&A, nm("m58"), i64t, IR_FUNC_PURE);
        IrValue *ix = ir_add_param(f, u64t, nm("i"));
        IrBlock *b0 = f->entry, *bl = b0;
        IrType *at = c == 2 ? i8s : i32t;
        IrValue *sa = ir_alloca(f, b0, at), *sb = ir_alloca(f, b0, i32t);
        if (c == 1) {
            IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, ix, ir_const_int(f, b0, 3, u64t)), b1, b2);
            ir_store(f, b1, sa, ir_const_int(f, b1, 10, i32t)); ir_set_br(b1, b3);
            ir_store(f, b2, sa, ir_const_int(f, b2, 20, i32t)); ir_set_br(b2, b3);
            bl = b3;
        } else if (c == 2) {
            ir_store(f, b0, sa, ir_const_int(f, b0, 300, i32t)); g_last_instr(b0)->unchecked = true;
        } else if (c == 3) {
            IrValue *sc = ir_alloca(f, b0, i32t);
            ir_store(f, b0, sc, ir_const_int(f, b0, 10, i32t));
            ir_store(f, b0, sa, ir_load(f, b0, sc, i32t));
        } else ir_store(f, b0, sa, ir_const_int(f, b0, 10, i32t));
        ir_store(f, bl, sb, ir_const_int(f, bl, 3, i32t));
        IrValue *p = ir_binop(f, bl, IR_MUL, ir_load(f, bl, sa, at), ir_load(f, bl, sb, i32t), i64t);
        ir_set_ret(bl, ir_binop(f, bl, IR_SDIV, ir_const_int(f, bl, 100, i64t), ir_binop(f, bl, IR_SUB, p, ir_const_int(f, bl, 29, i64t), i64t), i64t));
        int at2 = nins(bl) - 1;
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48], w1[64], w2[64];
        snprintf(key, sizeof key, " bb%d.%d divzero 0", bl->id, at2);
        const char *vz = t ? vc_of(t, key) : "";
        snprintf(w1, sizeof w1, "mul.hull : -1*%%v%d <= -30", p->id);
        snprintf(w2, sizeof w2, "mul.hull : 1*%%v%d <= 30", p->id);
        bool has = c == 0 || c == 3;
        CHECK(*vz && !!strstr(vz, w1) == has && !!strstr(vz, w2) == has, "case %d: %s%s, %s:\n%s", c, has ? "" : "not ", w1, w2, vz);
        if (c == 2) {                                          // i8 times 3: [-384, 381], not 900
            snprintf(w1, sizeof w1, "mul.hull : 1*%%v%d <= 381", p->id);
            CHECK(strstr(vz, w1) != NULL, "case 2: %s:\n%s", w1, vz);
        }
        free(t);
    }
    gen_mod = NULL;
    printf("  a load's static range is its store version's stored value's\n");
    // 59. a written measure with a QUOTIENT fixed at the header: `while i < n / 2 decreasing n / 2 - i`.
    // (0) n / 2 made in the header, (1) made again in the body: one term, its division stated, the
    // fall with it cancelling; outside where the quotient is (2) of the cell i, (3) by a variable
    // divisor, (4) a signed one that wraps; (5) a proposed ranking claim naming it is refused
    {   IrFunc *f = ir_func_new(&A, nm("q59"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u64t, nm("n")), *kv = ir_add_param(f, u64t, nm("k")), *ns = ir_add_param(f, i64t, nm("ns"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u64t);
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u64t)); ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, i, u64t);
        IrValue *q = ir_binop(f, b1, IR_UDIV, n, ir_const_int(f, b1, 2, u64t), u64t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, li, q), b2, b3);
        IrValue *li2 = ir_load(f, b2, i, u64t);
        IrValue *m[5];
        m[0] = ir_binop(f, b2, IR_SUB, q, li2, u64t);
        IrValue *q1 = ir_binop(f, b2, IR_UDIV, n, ir_const_int(f, b2, 2, u64t), u64t);
        m[1] = ir_binop(f, b2, IR_SUB, q1, li2, u64t);
        m[2] = ir_binop(f, b2, IR_SUB, ir_binop(f, b2, IR_UDIV, li2, ir_const_int(f, b2, 2, u64t), u64t), li2, u64t);
        m[3] = ir_binop(f, b2, IR_SUB, ir_binop(f, b2, IR_UDIV, n, kv, u64t), li2, u64t);
        IrValue *sq = ir_binop(f, b2, IR_SDIV, ns, ir_const_int(f, b2, 2, i64t), i64t);
        g_last_instr(b2)->unchecked = true;
        m[4] = ir_binop(f, b2, IR_SUB, sq, li2, i64t);
        ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li2, ir_const_int(f, b2, 1, u64t), u64t));
        ir_set_br(b2, b1);
        ir_set_ret(b3, ir_const_int(f, b3, 0, u32t));
        ir_finalize_cfg(f);
        char key[48], want[64], qd[48];
        snprintf(key, sizeof key, "bb%d.%d measure.", b2->id, nins(b2));
        b1->has_measure = true;
        for (int c = 0; c < 5; c++) {
            b1->measure_val = m[c];
            char *t = gen_text(f);
            bool ok = c <= 1;
            if (ok) {
                IrValue *qq = c == 0 ? q : q1;
                snprintf(want, sizeof want, "bb%d.%d measure.bound", b2->id, nins(b2));
                snprintf(qd, sizeof qd, "def %%v%d div", qq->id);
                const char *vb = t ? vc_of(t, want) : "";
                snprintf(want, sizeof want, "bb%d.%d measure.fall", b2->id, nins(b2));
                const char *vf = t ? vc_of(t, want) : "", *gf = strstr(vf, "goal ");
                char qv[24]; snprintf(qv, sizeof qv, "%%v%d ", qq->id);
                CHECK(t && count(t, key) == 2 && !strstr(t, "a written measure the generator cannot read"), "case %d: owed:\n%s", c, t ? t : "");
                CHECK(strstr(vb, qd) != NULL, "case %d: the bound states the quotient's division:\n%s", c, vb);
                CHECK(gf && !strstr(gf, qv), "case %d: the fall, the quotient cancelling:\n%s", c, vf);
            } else CHECK(t && strstr(t, "a written measure the generator cannot read as a sum"), "case %d: outside:\n%s", c, t ? t : "");
            free(t);
        }
        b1->has_measure = false; b1->measure_val = NULL;
        // (5) a PROPOSED ranking claim (untrusted) naming that quotient: refused, as before
        JRank pr; memset(&pr, 0, sizeof pr);
        pr.blk = b1->id; pr.fn = f; pr.ncomp = 1; pr.nt = 2; pr.key[0] = q->id; pr.coef[0][0] = 1; pr.key[1] = i->id; pr.coef[0][1] = -1;
        pr.nedge = 1; pr.from[0] = b2->id;
        gen_rk = &pr; gen_nrk = 1;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        CHECK(!t && strstr(why, "is not a cell or a value fixed at its header"), "case 5: a proposed claim's quotient refused: %s", why);
        free(t);
        gen_rk = NULL; gen_nrk = 0;
    }
    printf("  a written measure's quotient fixed at its header is one term\n");
    // 60. a float converted to an integer (P3.3): the narrowing's goal is on trunc(x), a fresh
    // variable with only F1's or F2's bounds. F1: (0) -32768.5 as i16 is -32768; (1) 16777219.0 typed
    // f32 rounds to 16777220 (the constant's IR type, not its spelling); (2) a slot stored 3.75 once
    // is 3; (3) one stored twice: nothing. F2, TRUE edges about x itself: (4) x >= -32768.0 then
    // x < 32768.0: [-32768, 32767]; (5) x > -3.0 then x <= 2.5: [-2, 2]; (6) the false edge of x < 0.0
    // (a NaN passes): nothing; (7) 4.0 > x: at most 3; (8) x != 5.0: nothing; (9) a guard on y, x
    // converted: nothing; (10) x == 2.5 as an ASSUME: [2, 2]; (11) the conversion's result, named
    // by a later goal, has the same bounds; (12) x > 0.0 then x < 0.5: [0, 0] (an integral c >= 0
    // excluded is no tighter); (13) x > -1.0 then x < 0.0: [0, 0]; (14) a slot stored 3.75 in one arm
    // only, loaded after the join: nothing; (15) ASSUME (x >= 0.0 and x < 4.0): [0, 3]
    for (int c = 0; c < 16; c++) {
        IrType *f64 = ir_type_new(&A, IRT_FLOAT), *f32 = ir_type_new(&A, IRT_FLOAT);
        f64->float_bits = 64; f32->float_bits = 32;
        IrType *T = c == 0 ? ir_type_int(&A, 16, true) : c == 6 ? u8t : i32t;
        IrFunc *f = ir_func_new(&A, nm("f60"), i32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, f64, nm("x")), *y = ir_add_param(f, f64, nm("y"));
        IrBlock *b0 = f->entry, *bl = b0, *bz = NULL;
        IrValue *src = x;
        #define GUARD(cmp, a, bb) do { IrBlock *nb_ = ir_new_block(f); if (!bz) { bz = ir_new_block(f); ir_set_ret(bz, ir_const_int(f, bz, 0, i32t)); } \
            ir_set_br_cond(bl, ir_icmp(f, bl, cmp, a, bb), nb_, bz); bl = nb_; } while (0)
        if (c == 0) { IrValue *k = ir_const_float(f, b0, 32768.5, f64); IrInstr *ng = ir_instr(f, IR_NEG, f64, 1); ng->operands[0] = k; ir_emit(b0, ng); src = ng->result; }
        if (c == 1) src = ir_const_float(f, b0, 16777219.0, f32);
        if (c == 2 || c == 3) {
            IrValue *sl = ir_alloca(f, b0, f64);
            ir_store(f, b0, sl, ir_const_float(f, b0, 3.75, f64));
            if (c == 3) ir_store(f, b0, sl, ir_const_float(f, b0, 1.0e10, f64));
            src = ir_load(f, b0, sl, f64);
        }
        if (c == 4) { GUARD(IR_CMP_SGE, x, ir_const_float(f, bl, -32768.0, f64)); GUARD(IR_CMP_SLT, x, ir_const_float(f, bl, 32768.0, f64)); }
        if (c == 5) { GUARD(IR_CMP_SGT, x, ir_const_float(f, bl, -3.0, f64)); GUARD(IR_CMP_SLE, x, ir_const_float(f, bl, 2.5, f64)); }
        if (c == 6) {                                          // if x < 0.0 { return 0 }: the false edge
            IrBlock *nb = ir_new_block(f); bz = ir_new_block(f); ir_set_ret(bz, ir_const_int(f, bz, 0, i32t));
            ir_set_br_cond(bl, ir_icmp(f, bl, IR_CMP_SLT, x, ir_const_float(f, bl, 0.0, f64)), bz, nb); bl = nb;
        }
        if (c == 7) GUARD(IR_CMP_SGT, ir_const_float(f, bl, 4.0, f64), x);
        if (c == 8) GUARD(IR_CMP_NE, x, ir_const_float(f, bl, 5.0, f64));
        if (c == 9) { GUARD(IR_CMP_SGE, y, ir_const_float(f, bl, 0.0, f64)); GUARD(IR_CMP_SLT, y, ir_const_float(f, bl, 4.0, f64)); }
        if (c == 10 || c == 11) ir_assume(f, bl, ir_icmp(f, bl, IR_CMP_EQ, x, ir_const_float(f, bl, 2.5, f64)));
        if (c == 12) { GUARD(IR_CMP_SGT, x, ir_const_float(f, bl, 0.0, f64)); GUARD(IR_CMP_SLT, x, ir_const_float(f, bl, 0.5, f64)); }
        if (c == 13) { GUARD(IR_CMP_SGT, x, ir_const_float(f, bl, -1.0, f64)); GUARD(IR_CMP_SLT, x, ir_const_float(f, bl, 0.0, f64)); }
        if (c == 14) {
            IrValue *sl = ir_alloca(f, b0, f64);
            IrBlock *ba = ir_new_block(f), *bj = ir_new_block(f);
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SGT, y, ir_const_float(f, b0, 0.0, f64)), ba, bj);
            ir_store(f, ba, sl, ir_const_float(f, ba, 3.75, f64)); ir_set_br(ba, bj);
            bl = bj; src = ir_load(f, bl, sl, f64);
        }
        if (c == 15) ir_assume(f, bl, ir_binop(f, bl, IR_AND, ir_icmp(f, bl, IR_CMP_SGE, x, ir_const_float(f, bl, 0.0, f64)),
                                                ir_icmp(f, bl, IR_CMP_SLT, x, ir_const_float(f, bl, 4.0, f64)), boolt));
        #undef GUARD
        IrInstr *cv = ir_instr(f, IR_CAST, T, 1); cv->operands[0] = src; ir_emit(bl, cv);
        int at = nins(bl) - 1;
        IrValue *r = cv->result;
        if (c == 11) r = ir_binop(f, bl, IR_SDIV, ir_const_int(f, bl, 100, i32t), r, i32t);
        ir_set_ret(bl, T == i32t ? r : ir_const_int(f, bl, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48], lo[80], hi[80];
        if (c == 11) snprintf(key, sizeof key, " bb%d.%d divzero 0", bl->id, nins(bl) - 1);
        else snprintf(key, sizeof key, " bb%d.%d narrow.lo 0", bl->id, at);
        const char *vz = t ? vc_of(t, key) : "";
        long long L = c == 0 ? -32768 : c == 1 ? 16777220 : c == 2 ? 3 : c == 4 ? -32768 : c == 5 ? -2 : c >= 12 ? 0 : 2;
        long long H = c == 0 ? -32768 : c == 1 ? 16777220 : c == 2 ? 3 : c == 4 ? 32767 : c == 5 ? 2 : c == 7 ? 3 : c == 15 ? 3 : c >= 12 ? 0 : 2;
        if (c == 11) { snprintf(lo, sizeof lo, "def %%v%d f2i : -1*%%v%d <= %lld", cv->result->id, cv->result->id, -L);
                       snprintf(hi, sizeof hi, "def %%v%d f2i : 1*%%v%d <= %lld", cv->result->id, cv->result->id, H); }
        else { snprintf(lo, sizeof lo, ": -1*trunc.%%v%d <= %lld", cv->result->id, -L);
               snprintf(hi, sizeof hi, ": 1*trunc.%%v%d <= %lld", cv->result->id, H); }
        bool hasL = c <= 2 || c == 4 || c == 5 || (c >= 10 && c != 14), hasH = hasL || c == 7;
        CHECK(*vz && (c == 11 || strstr(vz, "goal -1*trunc.%v")) && !!strstr(vz, lo) == hasL && !!strstr(vz, hi) == hasH,
              "case %d: %s%s, %s%s:\n%s", c, hasL ? "" : "no ", lo, hasH ? "" : "no ", hi, vz);
        if (!hasL && !hasH) CHECK(!strstr(vz, "f2i"), "case %d: no bound at all:\n%s", c, vz);
        free(t);
    }
    gen_mod = NULL;
    // F2's bound rule against the interpreter's conversion (interp.h's ii_cast, ii_cmp_scalar), as
    // P3.3 asks: for each constant c of a set of boundaries and halves, read at f32 and f64, each
    // predicate, and every f32 x within 64 steps of c, where `x p c` holds, trunc(x) is within the
    // bounds; and jg_ftrunc's integral part and integrality are C's for each of those values
    {   static const double cs[] = { 0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 2.5, -2.5, 3.0, -3.0, 127.0, 127.5, -128.0, -128.5,
                                     255.0, 256.0, 32767.0, 32768.0, -32768.0, -32769.0, 2147483647.0, 2147483648.0,
                                     -2147483648.0, 4294967296.0, 16777216.0, 16777217.0, 9223372036854775808.0,
                                     -9223372036854775808.0, 1.0e-30, -1.0e-30 };
        static const IrCmp ps[] = { IR_CMP_EQ, IR_CMP_SGE, IR_CMP_SGT, IR_CMP_SLE, IR_CMP_SLT, IR_CMP_UGE, IR_CMP_ULT };
        long bad = 0, tried = 0;
        for (size_t ci = 0; ci < sizeof cs / sizeof cs[0]; ci++)
            for (int prec = 0; prec < 2; prec++) {
                double c = prec ? (double)(float)cs[ci] : cs[ci];
                float cf = (float)c; uint32_t cb; memcpy(&cb, &cf, sizeof cb);
                for (size_t pi = 0; pi < sizeof ps / sizeof ps[0]; pi++) {
                    JRange r; bool hl = false, hh = false; r.lo = r.hi = 0;
                    jg_f2_bound(ps[pi], c, &r, &hl, &hh);
                    for (int d = -64; d <= 64; d++) {
                        uint32_t xb = cb + (uint32_t)d; float xf; memcpy(&xf, &xb, sizeof xf);
                        double x = xf;
                        if (x != x || x > 1e30 || x < -1e30) continue;
                        bool holds = ps[pi] == IR_CMP_EQ ? x == c : ps[pi] == IR_CMP_SGE || ps[pi] == IR_CMP_UGE ? x >= c
                                   : ps[pi] == IR_CMP_SGT ? x > c : ps[pi] == IR_CMP_SLE ? x <= c : x < c;
                        __int128 t = (__int128)x, tt; bool in;
                        tried++;
                        if (!jg_ftrunc(x, &tt, &in) || tt != t || in != (x == (double)t)) bad++;
                        if (holds && ((hl && t < r.lo) || (hh && t > r.hi))) bad++;
                    }
                }
            }
        CHECK(bad == 0 && tried > 20000, "F2's bounds against the conversion: %ld wrong of %ld", bad, tried);
    }
    printf("  a float conversion: F1's constants and F2's true guards bound trunc(x)\n");
    // 61. a product's own hull at its point: p = i * k over u32 with k <= 100 (a prologue assume), i
    // unbounded statically. Narrowed where (0) a true edge of i < n, n <= 100; (1) the false edge of
    // i >= n; (3) the guard's load of a cell i and the product's are one version; (5) i an i32 (p in
    // i32) under i <u n (a negative i fails it: i in [0, 99]); (6) an ASSUME of (i < n and k > 0); (7)
    // n > i; (8) i == n: p in [0, 9900] (8: [0, 10000]), its narrowing owed. Not where (2) the guard
    // is on j; (4) i is stored between the guard's load and the product's; (9) i an i32 under i <= 100
    // and i >u n (a negative i passes it); (10) i an i32 under i <u n with n in [-5, 100] (a negative
    // n reads as 2^64 + n, so i is bounded by nothing): the narrowing stays outside
    for (int c = 0; c < 11; c++) {
        IrType *T = c == 5 || c >= 9 ? i32t : u32t;
        IrFunc *f = ir_func_new(&A, nm("p61"), T, IR_FUNC_PURE);
        IrValue *ip = ir_add_param(f, T, nm("i")), *jp = ir_add_param(f, T, nm("j"));
        IrValue *n = ir_add_param(f, T, nm("n")), *k = ir_add_param(f, T, nm("k"));
        IrBlock *b0 = f->entry, *bl = b0;
        bool sg = T == i32t;
        ir_assume(f, b0, ir_icmp(f, b0, sg ? IR_CMP_SLE : IR_CMP_ULE, n, ir_const_int(f, b0, 100, T)));
        ir_assume(f, b0, ir_icmp(f, b0, sg ? IR_CMP_SLE : IR_CMP_ULE, k, ir_const_int(f, b0, 100, T)));
        if (sg) { ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, k, ir_const_int(f, b0, 0, T)));
                  ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, n, ir_const_int(f, b0, c == 10 ? -5 : 0, T))); }
        if (c == 9) ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SLE, ip, ir_const_int(f, b0, 100, T)));
        IrValue *iv = ip, *slot = NULL;
        if (c == 3 || c == 4) { slot = ir_alloca(f, b0, T); ir_store(f, b0, slot, ip); iv = ir_load(f, b0, slot, T); }
        IrBlock *b1 = ir_new_block(f), *bz = ir_new_block(f);
        ir_set_ret(bz, ir_const_int(f, bz, 0, T));
        if (c == 6) { ir_assume(f, b0, ir_binop(f, b0, IR_AND, ir_icmp(f, b0, IR_CMP_ULT, iv, n), ir_icmp(f, b0, IR_CMP_UGT, k, ir_const_int(f, b0, 0, T)), boolt)); ir_set_br(b0, b1); }
        else {
            IrValue *g = c == 1 ? ir_icmp(f, b0, IR_CMP_UGE, iv, n) : c == 2 ? ir_icmp(f, b0, IR_CMP_ULT, jp, n)
                       : c == 5 || c == 10 ? ir_icmp(f, b0, IR_CMP_ULT, iv, n) : c == 9 ? ir_icmp(f, b0, IR_CMP_UGT, iv, n)
                       : c == 7 ? ir_icmp(f, b0, IR_CMP_UGT, n, iv)
                       : c == 8 ? ir_icmp(f, b0, IR_CMP_EQ, iv, n) : ir_icmp(f, b0, IR_CMP_ULT, iv, n);
            if (c == 1) ir_set_br_cond(b0, g, bz, b1); else ir_set_br_cond(b0, g, b1, bz);
        }
        bl = b1;
        if (c == 4) ir_store(f, bl, slot, jp);
        IrValue *i2 = slot ? ir_load(f, bl, slot, T) : iv;
        IrValue *p = ir_binop(f, bl, IR_MUL, i2, k, T);
        int at = nins(bl) - 1;
        ir_set_ret(bl, p);
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48], want[64];
        snprintf(key, sizeof key, " bb%d.%d narrow.hi 0", bl->id, at);
        const char *vz = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "mul.hull : 1*%%v%d <= %d", p->id, c == 8 ? 10000 : 9900);
        bool owed = c != 2 && c != 4 && c != 9 && c != 10;
        CHECK(t && !!*vz == owed && (!owed || strstr(vz, want)), "case %d: %s%s:\n%s", c, owed ? "owed, " : "outside", owed ? want : "", owed ? vz : (t ? t : ""));
        free(t);
    }
    gen_mod = NULL;
    printf("  an operation's own hull at its point: its operands narrowed by the guards there\n");
    // 62. a store a[i] = 7 through a checked variable index into a fixed array whose elements are
    // cells: element k's fresh version is `or { i = k , a[k]' = 7 | i <= k - 1 , a[k]' = a[k] |
    // i >= k + 1 , a[k]' = a[k] }`. (0) an i32[4] stored by constant indexes, a[2] read after; (3)
    // a[3] no cell: a[2]'s all the same. Nothing where (1) the index is unchecked; (2) the array has
    // 17 elements; (4) the value is an i64 landing waived in the i32 slots (it may wrap); (5) the
    // store is on one arm of a join and a[2] is read past it (a disjunct holds no disjunction)
    for (int c = 0; c < 6; c++) {
        IrType *an = ir_type_new(&A, IRT_ARRAY); an->elem = i32t; an->array_len = c == 2 ? 17 : 4;
        IrFunc *f = ir_func_new(&A, nm("s62"), i32t, IR_FUNC_PURE);
        IrValue *ix = ir_add_param(f, u64t, nm("i")), *w = ir_add_param(f, i64t, nm("w"));
        IrBlock *b0 = f->entry, *bl = b0;
        IrValue *a = ir_alloca_array(f, b0, an);
        for (int k = 0; k < (c == 3 ? 3 : 4); k++)
            ir_store(f, b0, ir_elem_ptr(f, b0, a, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 10 * (k + 1), i32t));
        IrBlock *bs = b0;
        if (c == 5) {
            IrBlock *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_ULT, ix, ir_const_int(f, b0, 2, u64t)), b1, b2);
            ir_set_br(b1, b3); ir_set_br(b2, b3);
            bs = b1; bl = b3;
        }
        IrValue *ep = ir_elem_ptr(f, bs, a, ix, i32t);
        if (c == 1) g_last_instr(bs)->unchecked = true;
        IrValue *v = ir_const_int(f, bs, 7, i32t);
        if (c == 4) v = w;
        ir_store(f, bs, ep, v);
        if (c == 4) g_last_instr(bs)->unchecked = true;
        IrValue *a2 = ir_load(f, bl, ir_elem_ptr(f, bl, a, ir_const_int(f, bl, 2, u64t), i32t), i32t);
        ir_set_ret(bl, ir_binop(f, bl, IR_SDIV, ir_const_int(f, bl, 100, i32t), a2, i32t));
        int at = nins(bl) - 1;
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48], d0[64], d1[64], d2[64];
        snprintf(key, sizeof key, " bb%d.%d divzero 0", bl->id, at);
        const char *vz = t ? vc_of(t, key) : "";
        snprintf(d0, sizeof d0, "element : or { 1*%%v%d = 2 , ", ix->id);
        snprintf(d1, sizeof d1, " | 1*%%v%d <= 1 , ", ix->id);
        snprintf(d2, sizeof d2, " | -1*%%v%d <= -3 , ", ix->id);
        bool has = c == 0 || c == 3;
        CHECK(*vz && !!strstr(vz, d0) == has && !!strstr(vz, d1) == has && !!strstr(vz, d2) == has && (has || !strstr(vz, "element :")),
              "case %d: %sthe element's disjunction:\n%s", c, has ? "" : "no ", vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  a store by a variable index: each element written or kept, by the index\n");
    // 63. an EACH claim (JTERM_EACH) over a fill loop: a i32[4] stored 0 by constant indexes, then
    // `while i < 4 { a[i] = 10; i = i + 1 }` whose header also stores a[0] = 5, and 100 / (11 - a[2])
    // after it (the instances name the versions the header STARTS with). Claims at the header:
    // #0 each(a) <= 10 and #1 -each(a) <= 0. (0) Owed per element: 8 inv.entry and 8 inv.keep VCs,
    // ordinals ci*1000 + k; stated as all four instances where a VC names an element's header
    // version. Refused: (1) EACH over the integer cell i; (2) over an array of structs, whose
    // elements are no integer cells; (3) two EACH terms; (4) at the exit block, where no element is made afresh; (6) over
    // an i32[17], every element a cell (past JELEM_MAXN). (5) term_kinds: each(a) 2 at the header, 1
    // at the exit; each(i) 0
    {   IrType *a4 = ir_type_new(&A, IRT_ARRAY); a4->elem = i32t; a4->array_len = 4;
        for (int c = 0; c < 7; c++) {
            if (c == 6) a4->array_len = 17;
            IrFunc *f = ir_func_new(&A, nm("e63"), i32t, IR_FUNC_PURE);
            IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
            IrValue *a = ir_alloca_array(f, b0, a4), *b = ir_alloca_array(f, b0, a4), *i = ir_alloca(f, b0, u64t);
            IrType *sp = ir_type_new(&A, IRT_STRUCT); sp->n_fields = 1; sp->fields = arena_push_many_aligned(&A, IrType *, 1); sp->fields[0] = i32t;
            IrType *as = ir_type_new(&A, IRT_ARRAY); as->elem = sp; as->array_len = 4;
            IrValue *nc = ir_alloca_array(f, b0, as);                      // its elements are structs
            for (int k = 0; k < (int)a4->array_len; k++) ir_store(f, b0, ir_elem_ptr(f, b0, a, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 0, i32t));
            for (int k = 0; k < 3; k++) ir_store(f, b0, ir_elem_ptr(f, b0, b, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 0, i32t));
            ir_store(f, b0, i, ir_const_int(f, b0, 0, u64t)); ir_set_br(b0, b1);
            ir_store(f, b1, ir_elem_ptr(f, b1, a, ir_const_int(f, b1, 0, u64t), i32t), ir_const_int(f, b1, 5, i32t));
            ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, ir_load(f, b1, i, u64t), ir_const_int(f, b1, 4, u64t)), b2, b3);
            IrValue *li = ir_load(f, b2, i, u64t);
            ir_store(f, b2, ir_elem_ptr(f, b2, a, li, i32t), ir_const_int(f, b2, 10, i32t));
            ir_store(f, b2, ir_elem_ptr(f, b2, b, li, i32t), ir_const_int(f, b2, 10, i32t));
            ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li, ir_const_int(f, b2, 1, u64t), u64t)); ir_set_br(b2, b1);
            IrValue *a2 = ir_load(f, b3, ir_elem_ptr(f, b3, a, ir_const_int(f, b3, 2, u64t), i32t), i32t);
            ir_set_ret(b3, ir_binop(f, b3, IR_SDIV, ir_const_int(f, b3, 100, i32t), ir_binop(f, b3, IR_SUB, ir_const_int(f, b3, 11, i32t), a2, i32t), i32t));
            int at = nins(b3) - 1;
            ir_finalize_cfg(f);
            gen_mod = f;
            JClaim cl[2]; memset(cl, 0, sizeof cl);
            for (int q = 0; q < 2; q++) {
                cl[q].blk = b1->id; cl[q].fn = f; cl[q].nt = 1; cl[q].key[0] = a->id; cl[q].tk[0] = JTERM_EACH;
                cl[q].coef[0] = q ? -1 : 1; cl[q].rhs = q ? 0 : 10;
            }
            if (c == 5) {
                JMem m = jmem_over(region, sizeof region);
                int32_t keys[3] = { a->id, a->id, i->id };
                uint8_t tks[3] = { JTERM_EACH, JTERM_EACH, JTERM_EACH }, kh[3], kx[3]; char err[200];
                bool ok = jgen_term_kinds(&m, f, gen_mod, b1->id, keys, tks, 3, kh, err, sizeof err) &&
                          jgen_term_kinds(&m, f, gen_mod, b3->id, keys, tks, 3, kx, err, sizeof err);
                CHECK(ok && kh[0] == 2 && kx[0] == 1 && kh[2] == 0, "case 5: each(a) 2 at the header, 1 at the exit, each(i) 0 (got %d %d %d)", kh[0], kx[0], kh[2]);
                continue;
            }
            const char *why_want = NULL;
            if (c == 1) { cl[0].key[0] = i->id; why_want = "is not a fixed array with an element cell"; }
            if (c == 2) { cl[0].key[0] = nc->id; why_want = "is not a fixed array with an element cell"; }
            if (c == 3) { cl[0].nt = 2; cl[0].key[1] = b->id; cl[0].tk[1] = JTERM_EACH; cl[0].coef[1] = 1; why_want = "two EACH terms"; }
            if (c == 4) { cl[0].blk = b3->id; why_want = "a claim about no version its block makes afresh"; }
            if (c == 6) why_want = "is not a fixed array with an element cell";
            char why[200] = "";
            char *t = gen_with(f, cl, c ? 1 : 2, JGEN_ALL, why);
            if (c) { CHECK(!t && strstr(why, why_want), "case %d: refused (%s): got \"%s\"", c, why_want, why); free(t); continue; }
            char key[48], want[96];
            snprintf(key, sizeof key, " bb%d.%d divzero 0", b3->id, at);
            const char *vz = t ? vc_of(t, key) : "";
            int nent = t ? count(t, " inv.entry ") : -1, nkeep = t ? count(t, " inv.keep ") : -1;
            snprintf(want, sizeof want, " inv.keep 1002\n");
            bool ord = t && strstr(t, want) && strstr(t, " inv.entry 3\n");
            int inst = 0;
            for (int k = 0; k < 4; k++) {
                snprintf(want, sizeof want, "claim bb%d #1 : -1*%%s%d[%d]@head.bb%d <= 0", b1->id, a->id, k, b1->id);
                if (strstr(vz, want)) inst++;
            }
            CHECK(nent == 8 && nkeep == 8 && ord, "case 0: 8 inv.entry and 8 inv.keep, ordinals ci*1000 + k (got %d, %d):\n%s", nent, nkeep, t ? t : "");
            CHECK(inst == 4, "case 0: all four instances of claim #1 stated where a[2]'s header version is named (%d):\n%s", inst, vz);
            free(t);
        }
    }
    gen_mod = NULL;
    printf("  an EACH claim: one instance per element, stated and owed\n");
    // 64. a GUARDED EACH claim (JTERM_UPTO): a u8[8] never stored before `while i < 8 { a[i] = 7;
    // i = i + 1 }`, then a[7] - 7; every element is a cell (a[7] by its constant index, the others
    // c30's elements no address names). Claims at the header:
    // #0 each(a) <= 7 and #1 -each(a) <= -7, both upto(i): `for each k < i, a[k] = 7`. (0) eight
    // instances; k = 7's stated as `or { i <= 7 | -a[7] <= -7 }` (origin `claim bbH #ci upto`, which the
    // finder reads); owed on the entry edge and the back edge with the hypothesis `i >= k + 1` (a
    // `guard`), each obligation guarded (o->guarded, gmin k + 1 from its ordinal, gvar i's version). Refused: (1) UPTO without EACH; (2) two UPTO terms (one key twice); (3) UPTO of
    // coefficient 2; (4) UPTO over the array's address (one key twice)
    {   IrType *a8 = ir_type_new(&A, IRT_ARRAY); a8->elem = u8t; a8->array_len = 8;
        for (int c = 0; c < 5; c++) {
            IrFunc *f = ir_func_new(&A, nm("u64g"), i32t, IR_FUNC_PURE);
            IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
            IrValue *a = ir_alloca_array(f, b0, a8), *i = ir_alloca(f, b0, u64t);
            ir_store(f, b0, i, ir_const_int(f, b0, 0, u64t)); ir_set_br(b0, b1);
            ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, ir_load(f, b1, i, u64t), ir_const_int(f, b1, 8, u64t)), b2, b3);
            IrValue *li = ir_load(f, b2, i, u64t);
            ir_store(f, b2, ir_elem_ptr(f, b2, a, li, u8t), ir_const_int(f, b2, 7, u8t));
            ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li, ir_const_int(f, b2, 1, u64t), u64t)); ir_set_br(b2, b1);
            IrValue *a7 = ir_load(f, b3, ir_elem_ptr(f, b3, a, ir_const_int(f, b3, 7, u64t), u8t), u8t);
            ir_set_ret(b3, ir_binop(f, b3, IR_SUB, a7, ir_const_int(f, b3, 7, i32t), i32t));
            int at = nins(b3) - 1;
            ir_finalize_cfg(f);
            gen_mod = f;
            JClaim cl[2]; memset(cl, 0, sizeof cl);
            for (int q = 0; q < 2; q++) {
                cl[q].blk = b1->id; cl[q].fn = f; cl[q].nt = 2; cl[q].key[0] = a->id; cl[q].tk[0] = JTERM_EACH; cl[q].coef[0] = q ? -1 : 1;
                cl[q].key[1] = i->id; cl[q].tk[1] = JTERM_UPTO; cl[q].coef[1] = 1; cl[q].rhs = q ? -7 : 7;
            }
            const char *why_want = NULL;
            if (c == 1) { cl[0].nt = 1; cl[0].key[0] = i->id; cl[0].tk[0] = JTERM_UPTO; why_want = "UPTO term without an EACH term"; }
            if (c == 2) { cl[0].nt = 3; cl[0].key[2] = i->id; cl[0].tk[2] = JTERM_UPTO; cl[0].coef[2] = 1; why_want = "naming one term twice"; }
            if (c == 3) { cl[0].coef[1] = 2; why_want = "UPTO term of a coefficient other than 1"; }
            if (c == 4) { cl[0].key[1] = a->id; why_want = "naming one term twice"; }
            char why[200] = "";
            char *t = gen_with(f, cl, c ? 1 : 2, JGEN_ALL, why);
            if (c) { CHECK(!t && strstr(why, why_want), "case %d: refused (%s): got \"%s\"", c, why_want, why); free(t); continue; }
            char key[48], st[160], gd[96];
            snprintf(key, sizeof key, " bb%d.%d narrow.lo 0", b3->id, at);
            const char *vz = t ? vc_of(t, key) : "";
            snprintf(st, sizeof st, "claim bb%d #1 upto : or { 1*%%s%d@head.bb%d <= 7 | -1*%%s%d[7]@head.bb%d <= -7 }", b1->id, i->id, b1->id, a->id, b1->id);
            snprintf(gd, sizeof gd, "claim bb%d #1 guard : -1*%%s%d@", b1->id, i->id);
            CHECK(strstr(vz, st) != NULL, "case 0: the instance for a[7], guarded, stated after the loop: %s\n%s", st, vz);
            CHECK(t && count(t, " inv.entry ") == 16 && count(t, " inv.keep ") == 16 && count(t, gd) == 16 && strstr(t, " inv.keep 1007\n"),
                  "case 0: eight entry and eight keep VCs per claim, each with its guard:\n%s", t ? t : "");
            free(t);
            // the obligations carry the guard for a run-time checker
            JMem m = jmem_over(region, sizeof region);
            JObl *o; int32_t n; char err[200];
            int guarded = 0, right = 0;
            if (jgen_func(&m, f, gen_mod, cl, 2, NULL, 0, NULL, JGEN_ALL, &o, &n, err, sizeof err))
                for (int32_t q = 0; q < n; q++) {
                    if (o[q].claim < 0) { CHECK(!o[q].guarded, "an obligation that is no claim's is not guarded"); continue; }
                    guarded += o[q].guarded;
                    char vn[48]; snprintf(vn, sizeof vn, "%%s%d@", i->id);
                    long long kk = atoll(o[q].vc.id.ordinal) % 1000;
                    if (o[q].guarded && o[q].gmin == kk + 1 && o[q].gvar >= 0 && o[q].gvar < o[q].vc.nvars && !strncmp(o[q].vc.vars[o[q].gvar], vn, strlen(vn))) right++;
                }
            CHECK(guarded == 32 && right == 32, "case 0: all 32 claim VCs guarded by i >= k + 1 (%d, %d)", guarded, right);
        }
    }
    gen_mod = NULL;
    printf("  a guarded EACH claim: each element below the guard\n");
    // 65. a FACT about a place hands its address to no one: a local i32[4] stored 10..40, then a
    // marker on its address, then a call to an unknown function, then a[2]. After (0) init, (1)
    // shape, (2) borrow, (3) borrow_end, (4) consume, a[2] is still its store's version (the call
    // writes only what escaped); after (5) a store of the address into a slot, or (6) the address
    // passed to the call, it is fresh
    for (int c = 0; c < 7; c++) {
        IrType *a4 = ir_type_new(&A, IRT_ARRAY); a4->elem = i32t; a4->array_len = 4;
        IrType *pi = ir_type_new(&A, IRT_PTR); pi->elem = i32t;
        IrFunc *f = ir_func_new(&A, nm("m65"), i32t, IR_FUNC_PURE);
        IrBlock *b0 = f->entry;
        IrValue *a = ir_alloca_array(f, b0, a4);
        for (int k = 0; k < 4; k++) ir_store(f, b0, ir_elem_ptr(f, b0, a, ir_const_int(f, b0, k, u64t), i32t), ir_const_int(f, b0, 10 * (k + 1), i32t));
        IrValue *ext = ir_const_int(f, b0, 0, u32t);
        if (c == 0) ir_init_fact(f, b0, a);
        if (c == 1) { IrValue *e[1] = { ir_const_int(f, b0, 4, u64t) }; ir_shape(f, b0, a, e, 1); }
        if (c == 2) ir_borrow_begin(f, b0, a);
        if (c == 3) ir_borrow_end(f, b0, a);
        if (c == 4) ir_consume(f, b0, a);
        if (c == 5) ir_store(f, b0, ir_alloca(f, b0, pi), a);
        (void)mk_call(f, b0, "unknown65", c == 6 ? a : ext, u32t);
        IrValue *a2 = ir_load(f, b0, ir_elem_ptr(f, b0, a, ir_const_int(f, b0, 2, u64t), i32t), i32t);
        ir_set_ret(b0, ir_binop(f, b0, IR_SUB, a2, ir_const_int(f, b0, 10, i32t), i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), cn[48];
        snprintf(cn, sizeof cn, "%%s%d[2]@", a->id);
        const char *v = t ? strstr(t, " narrow.hi 0") : NULL;
        const char *e = v ? strstr(v, "\nend\n") : NULL;
        char vc[8192] = ""; if (v && e && e - v < (long)sizeof vc) { memcpy(vc, v, (size_t)(e - v)); vc[e - v] = 0; }
        bool kept = strstr(vc, cn) && !strstr(vc, "@havoc");
        CHECK(*vc && kept == (c <= 4), "case %d: a[2] %s after the call:\n%s", c, c <= 4 ? "keeps its store" : "is fresh", vc);
        free(t);
    }
    gen_mod = NULL;
    printf("  a fact about a place is no escape: a call keeps the cells of an array only marked\n");
    // 66. the elements of a local fixed array of at most JELEM_MAXN integers are cells, named by an
    // address or not (c30): `while i < 4 { a[i] = 5; i = i + 1 }` then 100 / a[j], no constant index
    // anywhere. (0) the load is the element j names over the four cells' header versions, and an
    // obligation's variable of a[2]'s version reads at its array's root + 2 (vkey the root, voff 2);
    // (1) no slot, term or path pair is offered for those cells (no address to key them by, an EACH
    // term names them); none for (2) an i32[17]
    // or (3) a dynamic array
    for (int c = 0; c < 4; c++) {
        if (c == 1) continue;                                  // case 1 is case 0's function's
        IrType *an = ir_type_new(&A, IRT_ARRAY); an->elem = i32t; an->array_len = c == 2 ? 17 : 4;
        IrFunc *f = ir_func_new(&A, nm("s66"), i32t, IR_FUNC_PURE);
        IrValue *jx = ir_add_param(f, u64t, nm("j"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *a = c == 3 ? ir_alloca_dyn(f, b0, i32t, ir_const_int(f, b0, 4, u64t)) : ir_alloca_array(f, b0, an);
        IrValue *i = ir_alloca(f, b0, u64t);
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u64t)); ir_set_br(b0, b1);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, ir_load(f, b1, i, u64t), ir_const_int(f, b1, 4, u64t)), b2, b3);
        IrValue *li = ir_load(f, b2, i, u64t);
        ir_store(f, b2, ir_elem_ptr(f, b2, a, li, i32t), ir_const_int(f, b2, 5, i32t));
        ir_store(f, b2, i, ir_binop(f, b2, IR_ADD, li, ir_const_int(f, b2, 1, u64t), u64t)); ir_set_br(b2, b1);
        IrValue *v = ir_load(f, b3, ir_elem_ptr(f, b3, a, jx, i32t), i32t);
        ir_set_ret(b3, ir_binop(f, b3, IR_SDIV, ir_const_int(f, b3, 100, i32t), v, i32t));
        int at = nins(b3) - 1;
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48], hn[48];
        snprintf(key, sizeof key, " bb%d.%d divzero 0", b3->id, at);
        snprintf(hn, sizeof hn, "def %%v%d element : or {", v->id);
        const char *vz = t ? vc_of(t, key) : "";
        if (c == 0) {
            bool ok = strstr(vz, hn) != NULL;
            for (int k = 0; ok && k < 4; k++) {
                char want[64]; snprintf(want, sizeof want, "%%s%d[%d]@head.bb%d", a->id, k, b1->id);
                ok = strstr(vz, want) != NULL;
            }
            CHECK(ok, "case 0: a[j] is one of the four cells' header versions:\n%s", vz);
            JMem m = jmem_over(region, sizeof region);
            JObl *o; int32_t n; char err[200]; int seen = 0, right = 0;
            if (jgen_func(&m, f, gen_mod, NULL, 0, NULL, 0, NULL, JGEN_ALL, &o, &n, err, sizeof err))
                for (int32_t q = 0; q < n; q++)
                    for (int32_t x = 0; x < o[q].vc.nvars; x++) {
                        char nm2[32]; snprintf(nm2, sizeof nm2, "%%s%d[2]@", a->id);
                        if (strncmp(o[q].vc.vars[x], nm2, strlen(nm2))) continue;
                        seen++;
                        if (o[q].voff && o[q].voff[x] == 2 && o[q].vkey[x] == a->id) right++;
                    }
            CHECK(seen > 0 && right == seen, "case 0: a[2]'s versions read at the root + 2 (%d of %d)", right, seen);
            JMem m2 = jmem_over(region, sizeof region);
            JSlot *sl; int32_t ns; bool none = true;
            if (jgen_slots(&m2, f, gen_mod, &sl, &ns, err, sizeof err))
                for (int32_t q = 0; q < ns; q++) if (sl[q].key == a->id) none = false;
            JMem m3 = jmem_over(region, sizeof region);
            if (jgen_terms(&m3, f, gen_mod, b1->id, &sl, &ns, err, sizeof err))
                for (int32_t q = 0; q < ns; q++) if (sl[q].key == a->id) none = false;
            JMem m4 = jmem_over(region, sizeof region);
            JPaths P; int pairs = 0;
            if (jgen_paths(&m4, f, gen_mod, NULL, 0, b1->id, &P, err, sizeof err))
                for (int32_t q = 0; q < P.n; q++)
                    for (int32_t r = 0; r < P.p[q].npairs; r++) { if (P.p[q].pairs[r].key == a->id) none = false; pairs++; }
            CHECK(none && pairs > 0, "case 1: no slot, term or path pair keyed by the array's root (%d pairs)", pairs);
        } else CHECK(*vz && !strstr(vz, " element : "), "case %d: no element cells:\n%s", c, vz);
        free(t);
    }
    gen_mod = NULL;
    printf("  a small local array's elements are cells, named by an address or not\n");
    // 67. a call through a pointer on a recursion with TWO targets: h67(fp, x) calls fp(x); p67(x) and
    // q67(x) each call h67(&self, x - 1), so the call graph gives h's call both p and q. Measures p
    // 2x, q 2x, h 2x + 1. (0) h's call owes rec.fall per target (ordinals 0 and 1), the argument its
    // op[1], and rec.bound once; (1) call_paths gives h two paths, callee p and q, the argument h's x;
    // (2) h's call listed `rec.le`: rec.le per target, the cut accepted (p's and q's calls fall);
    // (3) p's call listed too: h -> p -> h closes a cycle, refused; (4) q's instead: refused as well
    {   IrType *fnt = ir_type_new(&A, IRT_FUNC); fnt->elem = u32t;
        IrType *fpt = ir_type_new(&A, IRT_PTR); fpt->elem = fnt;
        IrFunc *h = ir_func_new(&A, nm("h67"), u32t, IR_FUNC_PURE), *fp = ir_func_new(&A, nm("p67"), u32t, IR_FUNC_PURE),
               *fq = ir_func_new(&A, nm("q67"), u32t, IR_FUNC_PURE);
        IrValue *hf = ir_add_param(h, fpt, nm("fp")), *hx = ir_add_param(h, u32t, nm("x"));
        IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h);
        ir_set_br_cond(h0, ir_icmp(h, h0, IR_CMP_EQ, hx, ir_const_int(h, h0, 0, u32t)), h1, h2);
        ir_set_ret(h1, ir_const_int(h, h1, 0, u32t));
        IrInstr *ic = ir_instr(h, IR_CALL, u32t, 2); ic->operands[0] = hf; ic->operands[1] = hx; ir_emit(h2, ic);
        int hci = nins(h2) - 1;
        ir_set_ret(h2, ic->result);
        ir_finalize_cfg(h);
        IrFunc *pq[2] = { fp, fq }; IrValue *px[2]; IrBlock *pb[2]; int pci[2];
        for (int w = 0; w < 2; w++) {
            IrFunc *u = pq[w];
            px[w] = ir_add_param(u, u32t, nm("x"));
            IrBlock *u0 = u->entry;
            IrValue *self = ir_func_ref(u, u0, nm(w ? "q67" : "p67"), fpt);
            IrValue *xm = ir_binop(u, u0, IR_SUB, px[w], ir_const_int(u, u0, 1, u32t), u32t);
            IrInstr *dc = ir_instr(u, IR_CALL, u32t, 2); dc->aux.callee = nm("h67"); dc->operands[0] = self; dc->operands[1] = xm; ir_emit(u0, dc);
            pci[w] = nins(u0) - 1; pb[w] = u0;
            ir_set_ret(u0, dc->result);
            ir_finalize_cfg(u);
        }
        h->next = fp; fp->next = fq; gen_mod = h;
        JRank r3[3]; memset(r3, 0, sizeof r3);
        IrFunc *fs[3] = { h, fp, fq }; IrValue *ks[3] = { hx, px[0], px[1] };
        for (int w = 0; w < 3; w++) { r3[w].blk = JRANK_ENTRY; r3[w].fn = fs[w]; r3[w].ncomp = 1; r3[w].nt = 1; r3[w].key[0] = ks[w]->id;
                                      r3[w].coef[0][0] = 2; r3[w].c0[0] = w == 0 ? 1 : 0; }
        gen_rk = r3; gen_nrk = 3;
        char *t = gen_with(h, NULL, 0, JGEN_ALL, NULL), key[48];
        int nf = 0;
        for (int q = 0; q < 2; q++) { snprintf(key, sizeof key, "bb%d.%d rec.fall %d\n", h2->id, hci, q); if (t && strstr(t, key)) nf++; }
        snprintf(key, sizeof key, "bb%d.%d rec.bound", h2->id, hci);
        CHECK(t && nf == 2 && count(t, key) == 1 && !strstr(t, "outside"), "case 0: h's call: rec.fall per target, rec.bound once:\n%s", t ? t : "");
        snprintf(key, sizeof key, "bb%d.%d rec.fall 0", h2->id, hci);
        const char *vf = t ? vc_of(t, key) : "", *g = strstr(vf, "goal ");
        // 2x at h's x, op[1], less h's 2x + 1, falls by 1: x cancels (op[0], the pointer, would not)
        CHECK(g && !strncmp(g, "goal 0 <= 0\n", 12), "case 0: the target's measure at op[1] (h's x):\n%s", vf);
        free(t);
        {   JMem jm = jmem_over(region, sizeof region); JPaths P; char err[200];
            bool ok = jgen_call_paths(&jm, h, gen_mod, NULL, 0, &P, err, sizeof err);
            bool cp = ok && P.n == 2 && P.p[0].callee != P.p[1].callee && (P.p[0].callee == fp || P.p[0].callee == fq) &&
                      (P.p[1].callee == fp || P.p[1].callee == fq);
            for (int q = 0; cp && q < 2; q++) cp = P.p[q].at == hci && P.p[q].npairs == 1 && P.p[q].nentry == 1 &&
                                                   P.p[q].pairs[0].post == P.p[q].entry[0].pre;   // the argument is h's x
            CHECK(cp, "case 1: h's two call paths, callee p and q, the argument h's x at entry");
        }
        r3[0].nle = 1; r3[0].le_blk[0] = h2->id; r3[0].le_idx[0] = hci;
        t = gen_with(h, NULL, 0, JGEN_ALL, NULL);
        nf = 0;
        for (int q = 0; q < 2; q++) { snprintf(key, sizeof key, "bb%d.%d rec.le %d\n", h2->id, hci, q); if (t && strstr(t, key)) nf++; }
        CHECK(t && nf == 2 && !strstr(t, "rec.fall"), "case 2: h's call listed: rec.le per target, the cut accepted:\n%s", t ? t : "");
        free(t);
        r3[1].nle = 1; r3[1].le_blk[0] = pb[0]->id; r3[1].le_idx[0] = pci[0];
        char why[200] = ""; t = gen_with(h, NULL, 0, JGEN_ALL, why);
        CHECK(!t && strstr(why, "close a cycle"), "case 3: h -> p -> h listed, refused: got \"%s\"", why);
        free(t);
        r3[1].nle = 0; r3[2].nle = 1; r3[2].le_blk[0] = pb[1]->id; r3[2].le_idx[0] = pci[1];
        why[0] = 0; t = gen_with(h, NULL, 0, JGEN_ALL, why);
        CHECK(!t && strstr(why, "close a cycle"), "case 4: h -> q -> h listed, refused: got \"%s\"", why);
        free(t);
        // (5) p and q's measures written, 2x: at h's call rec.def per target, side 1 at the
        // arguments, its term h's x (op[1]), not the pointer
        r3[0].nle = 0; r3[2].nle = 0; gen_nrk = 1;
        for (int w = 0; w < 2; w++) {
            IrFunc *u = pq[w];
            u->has_decreasing = true; u->measure_blk = ir_new_ghost_block(u);
            u->measure_val = ir_binop(u, u->measure_blk, IR_MUL, ir_const_int(u, u->measure_blk, 2, u32t), px[w], u32t);
        }
        t = gen_with(h, NULL, 0, JGEN_ALL, why);
        char xk[24], fk[24]; snprintf(xk, sizeof xk, "%%v%d", hx->id); snprintf(fk, sizeof fk, "%%v%d", hf->id);
        int nd = 0;
        for (int q = 0; q < 2; q++) {
            snprintf(key, sizeof key, "bb%d.%d rec.def %d", h2->id, hci, q * 100000 + 1001);
            const char *vd = t ? vc_of(t, key) : "", *gd = strstr(vd, "goal ");
            if (gd && strstr(gd, xk) && !strstr(gd, fk)) nd++;
        }
        CHECK(t && nd == 2, "case 5: rec.def per target at h's x:\n%s", t ? t : why);
        free(t);
        // (6) h's measure written too, 2x + 1: its rec.def at h's entry (side 0) owed once, not per target
        h->has_decreasing = true; h->measure_blk = ir_new_ghost_block(h);
        h->measure_val = ir_binop(h, h->measure_blk, IR_ADD, ir_binop(h, h->measure_blk, IR_MUL, ir_const_int(h, h->measure_blk, 2, u32t), hx, u32t),
                                  ir_const_int(h, h->measure_blk, 1, u32t), u32t);
        gen_nrk = 0;
        t = gen_with(h, NULL, 0, JGEN_ALL, why);
        snprintf(key, sizeof key, "bb%d.%d rec.def 0\n", h2->id, hci);
        char k1[48]; snprintf(k1, sizeof k1, "bb%d.%d rec.def 10000", h2->id, hci);
        CHECK(t && strstr(t, key) && !strstr(t, k1) && !strstr(t, "outside"), "case 6: h's rec.def at its entry once:\n%s", t ? t : why);
        free(t);
        h->has_decreasing = false; h->measure_blk = NULL; h->measure_val = NULL;
        for (int w = 0; w < 2; w++) { pq[w]->has_decreasing = false; pq[w]->measure_blk = NULL; pq[w]->measure_val = NULL; }
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a call through a pointer on a recursion: owed per target, a path per target, cut per target\n");
    // 68. MONOTONE products: q = b*c in bb0, then p = a*c in bb1 (bb0 -> bb1 | bb2, bb1, bb2 -> bb3),
    // and 100 / p after p, all u32 unless said. (0) p's def: a <= b - 1 -> p <= q - c and mirrored;
    // (1) q = c*b, p = c*a: the same; (2) i32, c not >= 0: nothing; (3) i32 with `assume c >= 0`:
    // both; (4) q in bb2, p in bb3 (q does not dominate p): nothing; (5) q unchecked: nothing; (6)
    // q = a*c (a == b): nothing; (7) q = b*c, d*c, e*c (three, all in bb0): two of them, four facts;
    // (8) q and p both in bb1, q first: both; (9) p first, q after it in bb1: nothing about p (q is
    // not a term of `100 / p`); (10) q = b*c, p = c*a (c at either position): both
    for (int c = 0; c < 11; c++) {
        IrType *T = c == 2 || c == 3 ? i32t : u32t;
        IrFunc *f = ir_func_new(&A, nm("m68"), T, IR_FUNC_PURE);
        IrValue *pa = ir_add_param(f, T, nm("a")), *pb = ir_add_param(f, T, nm("b")), *pc = ir_add_param(f, T, nm("c")),
                *pd = ir_add_param(f, T, nm("d")), *pe = ir_add_param(f, T, nm("e"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        if (c == 3) ir_assume(f, b0, ir_icmp(f, b0, IR_CMP_SGE, pc, ir_const_int(f, b0, 0, T)));
        IrValue *q = NULL, *p = NULL;
        IrBlock *qb = c == 4 ? b2 : c >= 8 ? b1 : b0, *pblk = c == 4 ? b3 : b1;
        if (c != 9) {
            q = ir_binop(f, qb, IR_MUL, c == 1 ? pc : c == 6 ? pa : pb, c == 1 ? pb : pc, T);
            if (c == 5) g_last_instr(qb)->unchecked = true;
            if (c == 7) { ir_binop(f, qb, IR_MUL, pd, pc, T); ir_binop(f, qb, IR_MUL, pe, pc, T); }
        }
        p = ir_binop(f, pblk, IR_MUL, c == 1 || c == 10 ? pc : pa, c == 1 || c == 10 ? pa : pc, T);
        if (c == 9) q = ir_binop(f, b1, IR_MUL, pb, pc, T);
        IrValue *r = ir_binop(f, pblk, IR_UDIV, ir_const_int(f, pblk, 100, T), p, T);
        if (T == i32t) g_last_instr(pblk)->op = IR_SDIV;
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_NE, pd, ir_const_int(f, b0, 0, T)), b1, b2);
        if (pblk == b1) { ir_set_ret(b1, r); ir_set_br(b2, b3); }
        else { ir_set_br(b1, b3); ir_set_br(b2, b3); }
        ir_set_ret(b3, pblk == b3 ? r : ir_const_int(f, b3, 0, T));
        ir_finalize_cfg(f);
        gen_mod = f;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL), key[48];
        snprintf(key, sizeof key, "def %%v%d mul.mono", p->id);
        int bv = c == 6 ? pa->id : pb->id;
        bool both = mono_has(t, p->id, pa->id, bv, pc->id, q->id, 1) && mono_has(t, p->id, pa->id, bv, pc->id, q->id, -1);
        if (c == 0 || c == 1 || c == 3 || c == 8 || c == 10) CHECK(both, "case %d: p's two monotone facts:\n%s", c, t ? t : "");
        else if (c == 7) CHECK(t && count(t, key) == 4, "case %d: two of three products, four facts:\n%s", c, t ? t : "");
        else CHECK(t && !strstr(t, "mul.mono"), "case %d: nothing:\n%s", c, t ? t : "");
        free(t);
    }
    gen_mod = NULL;
    printf("  a product beside an earlier one of the same c >= 0: a <= b - 1 gives p <= q - c\n");
    // 69. a FREE back edge (fall -1): Cook, See and Zuleger's Fig. 7b, `while x != 0 { if p { x-- }
    // else if q { y--; z = n1 } else { z--; x = n2 } }` over u32 cells, its three paths joined before
    // one back edge, ranked <y, z, x>: each path lowers another component. (0) four rank.lex VCs on
    // the back edge, keep 0, keep 1, fall 2 and bound 2, each under `premise rank.lex 0` (two cases:
    // y did not fall, or was negative) and `premise rank.lex 1` (three: y rose, z did not fall, z
    // negative, its constant moved right: <y + 5, z + 7, x>); no rank.keep, rank.fall or rank.bound;
    // (1) <y> alone, free: fall 0 and bound 0 as rank.lex, no premise; (2) fall -2: refused
    for (int c = 0; c < 3; c++) {
        IrFunc *f = ir_func_new(&A, nm("cs7b"), u32t, IR_FUNC_PURE);
        IrValue *xp = ir_add_param(f, u32t, nm("xp")), *pp = ir_add_param(f, u32t, nm("p")), *qp = ir_add_param(f, u32t, nm("q")),
                *n1 = ir_add_param(f, u32t, nm("n1")), *n2 = ir_add_param(f, u32t, nm("n2"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f),
                *b5 = ir_new_block(f), *b6 = ir_new_block(f), *b7 = ir_new_block(f), *b8 = ir_new_block(f);
        IrValue *x = ir_alloca(f, b0, u32t), *y = ir_alloca(f, b0, u32t), *z = ir_alloca(f, b0, u32t);
        ir_store(f, b0, x, xp); ir_store(f, b0, y, xp); ir_store(f, b0, z, xp); ir_set_br(b0, b1);
        IrValue *lx = ir_load(f, b1, x, u32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_NE, lx, ir_const_int(f, b1, 0, u32t)), b2, b7);
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_NE, pp, ir_const_int(f, b2, 0, u32t)), b3, b4);
        ir_store(f, b3, x, ir_binop(f, b3, IR_SUB, lx, ir_const_int(f, b3, 1, u32t), u32t)); ir_set_br(b3, b6);
        ir_set_br_cond(b4, ir_icmp(f, b4, IR_CMP_NE, qp, ir_const_int(f, b4, 0, u32t)), b5, b8);
        ir_store(f, b5, y, ir_binop(f, b5, IR_SUB, ir_load(f, b5, y, u32t), ir_const_int(f, b5, 1, u32t), u32t)); ir_store(f, b5, z, n1); ir_set_br(b5, b6);
        ir_store(f, b8, z, ir_binop(f, b8, IR_SUB, ir_load(f, b8, z, u32t), ir_const_int(f, b8, 1, u32t), u32t)); ir_store(f, b8, x, n2); ir_set_br(b8, b6);
        ir_set_br(b6, b1);
        ir_set_ret(b7, ir_const_int(f, b7, 0, u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.ncomp = c == 1 ? 1 : 3; r.nt = c == 1 ? 1 : 3; r.key[0] = y->id; r.key[1] = z->id; r.key[2] = x->id;
        for (int j = 0; j < 3; j++) r.coef[j][j] = 1;
        if (c == 0) { r.c0[0] = 5; r.c0[1] = 7; }                       // y + 5 < 0 is y <= -6
        r.nedge = 1; r.from[0] = b6->id; r.fall[0] = c == 2 ? -2 : -1;
        gen_rk = &r; gen_nrk = 1;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (c == 2) { CHECK(!t && strstr(why, "falling component out of range"), "case 2: refused: got \"%s\"", why); free(t); continue; }
        bool ok = t && !strstr(t, "rank.keep") && !strstr(t, "rank.fall") && !strstr(t, "rank.bound");
        int nq = c == 1 ? 2 : 4;
        for (int q = 0; q < nq && ok; q++) {
            char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.lex %d", b6->id, q);
            const char *v = vc_of(t, key), *g = strstr(v, "goal ");
            int cv = c == 1 ? y->id : q == 0 ? y->id : q == 1 ? z->id : x->id;
            char cvs[24]; snprintf(cvs, sizeof cvs, "%%s%d@", cv);
            const char *rhs = q == nq - 2 ? "<= -1\n" : "<= 0\n";
            ok = g && strstr(g, cvs) && strstr(g, rhs);
            for (int j = 0; ok && j < 2; j++) {
                char pk[48]; snprintf(pk, sizeof pk, "premise rank.lex %d : or { ", j);
                const char *l = strstr(v, pk), *e = l ? strchr(l, '\n') : NULL;
                int bars = 0;
                for (const char *u = l; u && e && (u = strstr(u, " | ")) && u < e; u++) bars++;
                ok = c == 1 ? !l : l && bars == j + 1;
                char neg[48]; snprintf(neg, sizeof neg, "| 1*%%s%d@head.bb%d <= %d }", j ? z->id : y->id, b1->id, j ? -8 : -6);
                if (ok && c == 0) ok = strstr(l, neg) && strstr(l, neg) < e;   // the component's constant moved right
            }
            if (!ok) CHECK(false, "case %d: rank.lex %d, its goal on component %s and its premises:\n%s", c, q, cvs, v);
        }
        CHECK(ok && count(t, " rank.lex ") >= nq, "case %d: %d rank.lex VCs, nothing else on the edge:\n%s", c, nq, t ? t : why);
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a free back edge owes the lexicographic decrease itself, under premises\n");
    // 70. a component that is the MAX of rows: `while x != 0 { if x > 0 { x-- } else { x++ } }` over
    // an i32 cell, ranked max(x, -x) (rows x and -x, cont[1] = 1). (0) rank.fall 1000 and 2000, one
    // per row, each under one `premise rank.max 0`, and rank.bound 0 under one; no rank.keep; (1)
    // <0, max(x, -x)>, falling at 1: rank.keep 0 (a row of its own), rank.fall 1001 and 2001,
    // rank.bound 1; refused: (2) cont[0] = 1; (3) cont[1] = 3; (4) fall 1 (one component, two
    // rows); (6) max(x + 5, -x + 2), the constants moved right: the row x's goal x' + x <= -4, the
    // row -x's premise x + x' <= -3; a FREE edge (c41): (5) over the max, rank.lex 0 (m' <= m - 1)
    // and 1 (m >= 0), each under m and m' defined exactly (`def rank.max 0 pre` and `post`: m >= x,
    // m >= -x, m <= x or m <= -x; the bound names m alone) and nothing per row; (7) over min(x + 5,
    // -x + 2), its definitions' signs and constants (m <= x + 5, m <= -x + 2, m >= x + 5 or m >= -x + 2)
    for (int c = 0; c < 8; c++) {
        IrFunc *f = ir_func_new(&A, nm("abs71"), i32t, IR_FUNC_PURE);
        IrValue *xp = ir_add_param(f, i32t, nm("xp"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f),
                *b5 = ir_new_block(f), *b6 = ir_new_block(f);
        IrValue *x = ir_alloca(f, b0, i32t);
        ir_store(f, b0, x, xp); ir_set_br(b0, b1);
        IrValue *lx = ir_load(f, b1, x, i32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_NE, lx, ir_const_int(f, b1, 0, i32t)), b2, b5);
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_SGT, lx, ir_const_int(f, b2, 0, i32t)), b3, b4);
        ir_store(f, b3, x, ir_binop(f, b3, IR_SUB, lx, ir_const_int(f, b3, 1, i32t), i32t)); ir_set_br(b3, b6);
        ir_store(f, b4, x, ir_binop(f, b4, IR_ADD, lx, ir_const_int(f, b4, 1, i32t), i32t)); ir_set_br(b4, b6);
        ir_set_br(b6, b1);
        ir_set_ret(b5, ir_const_int(f, b5, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.nt = 1; r.key[0] = x->id; r.nedge = 1; r.from[0] = b6->id;
        int o1 = c == 1 ? 1 : 0;                                // <0, ...>: row 0 all zero
        r.ncomp = 2 + o1; r.coef[o1][0] = 1; r.coef[o1 + 1][0] = -1; r.cont[o1 + 1] = 1; r.fall[0] = (int8_t)o1;
        if (c == 6 || c == 7) { r.c0[0] = 5; r.c0[1] = 2; }
        const char *bad = NULL;
        if (c == 2) { r.cont[0] = 1; bad = "max or min of rows out of shape"; }
        if (c == 3) { r.cont[1] = 3; bad = "max or min of rows out of shape"; }
        if (c == 4) { r.fall[0] = 1; bad = "falling component out of range"; }
        if (c == 5 || c == 7) r.fall[0] = -1;
        if (c == 7) r.cont[1] = 2;
        gen_rk = &r; gen_nrk = 1;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (bad) { CHECK(!t && strstr(why, bad), "case %d: refused (%s): got \"%s\"", c, bad, why); free(t); gen_rk = NULL; gen_nrk = 0; continue; }
        if (c == 5 || c == 7) {                                // a free edge: rank.lex 0 and 1, over m
            const char *k = c == 5 ? "max" : "min";
            bool ok = t && !strstr(t, "rank.fall") && !strstr(t, "rank.keep") && !strstr(t, "rank.bound") && !strstr(t, "premise ");
            for (int q = 0; ok && q < 2; q++) {
                char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.lex %d", b6->id, q);
                const char *v = vc_of(t, key);
                char d[8][200]; int nd = 0;
                for (int side = 0; side < (q == 0 ? 2 : 1); side++) {
                    char xv[40], m[24];
                    snprintf(xv, sizeof xv, side ? "%%s%d@join.bb%d" : "%%s%d@head.bb%d", x->id, side ? b6->id : b1->id);
                    snprintf(m, sizeof m, "rank.%s.0.%s", k, side ? "post" : "pre");
                    const char *o = side ? "post" : "pre";
                    if (c == 5) {
                        snprintf(d[nd++], 200, "def rank.max 0 %s : 1*%s + -1*%s <= 0\n", o, xv, m);
                        snprintf(d[nd++], 200, "def rank.max 0 %s : -1*%s + -1*%s <= 0\n", o, xv, m);
                        snprintf(d[nd++], 200, "def rank.max 0 %s : or { -1*%s + 1*%s <= 0 | 1*%s + 1*%s <= 0 }\n", o, xv, m, xv, m);
                    } else {
                        snprintf(d[nd++], 200, "def rank.min 0 %s : -1*%s + 1*%s <= 5\n", o, xv, m);
                        snprintf(d[nd++], 200, "def rank.min 0 %s : 1*%s + 1*%s <= 2\n", o, xv, m);
                        snprintf(d[nd++], 200, "def rank.min 0 %s : or { 1*%s + -1*%s <= -5 | -1*%s + -1*%s <= -2 }\n", o, xv, m, xv, m);
                    }
                }
                char dk[24]; snprintf(dk, sizeof dk, "def rank.%s 0 ", k);
                ok = count(v, dk) == (q == 0 ? 6 : 3) && (q == 0 || !strstr(v, ".0.post"));
                for (int i = 0; ok && i < nd; i++) ok = strstr(v, d[i]) != NULL;
                char gl[80];
                if (q == 0) snprintf(gl, sizeof gl, "goal -1*rank.%s.0.pre + 1*rank.%s.0.post <= -1\n", k, k);
                else snprintf(gl, sizeof gl, "goal -1*rank.%s.0.pre <= 0\n", k);
                ok = ok && strstr(v, gl);
                if (!ok) CHECK(false, "case %d: rank.lex %d over m, defined exactly on each side:\n%s", c, q, v);
            }
            char k2[48]; snprintf(k2, sizeof k2, "bb%d.0 rank.lex 2", b6->id);
            CHECK(ok && !strstr(t, k2), "case %d: rank.lex 0 and 1, nothing per row:\n%s", c, t ? t : why);
            free(t);
            // each m's recipe, for a run-time checker: its kind, and the rows x + c0[0], -x + c0[1]
            // on its own side (rank.lex 0: pre and post; 1: pre)
            char *rc = gen_recipes(f, why), want[3][200];
            const char *xs[3] = { "head", "join", "head" };
            int bs[3] = { b1->id, b6->id, b1->id };
            int c0a = c == 7 ? 5 : 0, c0b = c == 7 ? 2 : 0;
            for (int q = 0; q < 3; q++)
                snprintf(want[q], 200, "bb%d.0 rank.lex %d rank.%s.0.%s mm %d: {%d 1*%%s%d@%s.bb%d} {%d -1*%%s%d@%s.bb%d}\n", b6->id, q < 2 ? 0 : 1, k,
                         q == 1 ? "post" : "pre", c == 7 ? -1 : 1, c0a, x->id, xs[q], bs[q], c0b, x->id, xs[q], bs[q]);
            CHECK(rc && count(rc, " mm ") == 3 && strstr(rc, want[0]) && strstr(rc, want[1]) && strstr(rc, want[2]),
                  "case %d: the recipes, three, each the rows on its side:\n%s\nwanted e.g.\n%s", c, rc ? rc : why, want[0]);
            free(rc); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
            continue;
        }
        const char *want[4] = { "rank.fall 1000", "rank.fall 2000", "rank.bound 0", NULL };
        const char *want1[5] = { "rank.keep 0", "rank.fall 1001", "rank.fall 2001", "rank.bound 1", NULL };
        const char **w = c == 1 ? want1 : want;
        bool ok = t != NULL;
        for (int k = 0; ok && w[k]; k++) {
            char key[48]; snprintf(key, sizeof key, "bb%d.0 %s\n", b6->id, w[k]);
            ok = strstr(t, key) != NULL;
            char k2[48]; snprintf(k2, sizeof k2, "bb%d.0 %s", b6->id, w[k]);
            const char *v = ok ? vc_of(t, k2) : "";
            int np = count(v, "premise rank.max");
            ok = ok && np == (strstr(w[k], "keep") ? 0 : 1);
            if (!ok) CHECK(false, "case %d: %s with its premise:\n%s", c, w[k], v);
        }
        CHECK(ok && count(t, " rank.") == (c == 1 ? 4 : 3) + count(t, "premise rank."), "case %d: those VCs only:\n%s", c, t ? t : why);
        if (c == 0 || c == 6) {                                // the bound's premise: x (+ 5) < 0 before the trip
            char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.bound 0", b6->id);
            const char *pr = strstr(vc_of(t, key), "premise rank.max 0 : "), *nl = pr ? strchr(pr, '\n') : NULL;
            char hx[40]; snprintf(hx, sizeof hx, ": 1*%%s%d@head.bb%d <= %s\n", x->id, b1->id, c ? "-6" : "-1");
            CHECK(pr && nl && !strncmp(pr + strlen("premise rank.max 0 "), hx, strlen(hx)), "case %d: the bound's premise, x%s <= -1:\n%s", c, c ? " + 5" : "", vc_of(t, key));
        }
        if (c == 6) {
            char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.fall 1000", b6->id);
            const char *g = strstr(vc_of(t, key), "goal ");
            CHECK(g && strstr(g, "<= -4\n"), "case 6: the row x's goal, x' + 5 - (-x + 2) <= -1:\n%s", vc_of(t, key));
            snprintf(key, sizeof key, "bb%d.0 rank.fall 2000", b6->id);
            const char *pr = strstr(vc_of(t, key), "premise rank.max 0 : ");
            CHECK(pr && !strncmp(strchr(pr, '\n') - 5, "<= -3\n", 6), "case 6: the row -x's premise, x + 5 - (-x' + 2) <= 0:\n%s", vc_of(t, key));
        }
        if (c == 0) {                                           // a = x: premise x - x' <= 0, goal x' + x <= -1
            char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.fall 1000", b6->id);
            const char *v = vc_of(t, key), *g = strstr(v, "goal ");
            char hx[32], jx[32]; snprintf(hx, sizeof hx, "1*%%s%d@head.bb%d", x->id, b1->id); snprintf(jx, sizeof jx, "1*%%s%d@join.bb%d", x->id, b6->id);
            CHECK(g && strstr(g, hx) && strstr(g, jx) && !strstr(g, "-1*%s") && strstr(g, "<= -1\n"), "case 0: the row x's goal against -x, x' + x <= -1:\n%s", v);
        }
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a ranking component that is the max of rows: one VC per row, under premises\n");
    // 71. a component that is the MIN of rows (Piecewise): `while p > 0 and q > 0 { if q < p { q--;
    // p = n1 } else { p--; q = n2 } }` over i32 cells, ranked min(p, q) (cont[1] = 2). (0) rank.fall
    // 1000 and 2000, one per row before the trip, the goal on the last row q after it, each under
    // one `premise rank.min 0` of two cases (p after the trip not below p or not below q, both
    // before it); rank.bound 1000 and 2000, one per row, no premise; (1) refused: rows p, q, p's
    // negation with cont 1, 2 (a max and a min in one component); (2) min(p, q, p + q): three falls,
    // each under two premises (rows p and q) of three cases
    for (int c = 0; c < 3; c++) {
        IrFunc *f = ir_func_new(&A, nm("pw72"), i32t, IR_FUNC_PURE);
        IrValue *pp = ir_add_param(f, i32t, nm("p0")), *qp = ir_add_param(f, i32t, nm("q0")),
                *n1 = ir_add_param(f, i32t, nm("n1")), *n2 = ir_add_param(f, i32t, nm("n2"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f),
                *b5 = ir_new_block(f), *b6 = ir_new_block(f), *b7 = ir_new_block(f);
        IrValue *p = ir_alloca(f, b0, i32t), *q = ir_alloca(f, b0, i32t);
        ir_store(f, b0, p, pp); ir_store(f, b0, q, qp); ir_set_br(b0, b1);
        IrValue *lp = ir_load(f, b1, p, i32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_SGT, lp, ir_const_int(f, b1, 0, i32t)), b2, b7);
        IrValue *lq = ir_load(f, b2, q, i32t);
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_SGT, lq, ir_const_int(f, b2, 0, i32t)), b3, b7);
        ir_set_br_cond(b3, ir_icmp(f, b3, IR_CMP_SLT, lq, lp), b4, b5);
        ir_store(f, b4, q, ir_binop(f, b4, IR_SUB, lq, ir_const_int(f, b4, 1, i32t), i32t)); ir_store(f, b4, p, n1); ir_set_br(b4, b6);
        ir_store(f, b5, p, ir_binop(f, b5, IR_SUB, lp, ir_const_int(f, b5, 1, i32t), i32t)); ir_store(f, b5, q, n2); ir_set_br(b5, b6);
        ir_set_br(b6, b1);
        ir_set_ret(b7, ir_const_int(f, b7, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.nt = 2; r.key[0] = p->id; r.key[1] = q->id; r.nedge = 1; r.from[0] = b6->id;
        r.ncomp = 2; r.coef[0][0] = 1; r.coef[1][1] = 1; r.cont[1] = 2; r.fall[0] = 0;
        if (c == 1) { r.ncomp = 3; r.coef[2][0] = -1; r.cont[1] = 1; r.cont[2] = 2; }
        if (c == 2) { r.ncomp = 3; r.coef[2][0] = 1; r.coef[2][1] = 1; r.cont[2] = 2; }
        gen_rk = &r; gen_nrk = 1;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (c == 1) { CHECK(!t && strstr(why, "max or min of rows out of shape"), "case 1: refused: got \"%s\"", why); free(t); gen_rk = NULL; gen_nrk = 0; continue; }
        if (c == 2) {
            bool ok3 = t != NULL;
            for (int k = 1; ok3 && k <= 3; k++) {
                char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.fall %d", b6->id, 1000 * k);
                const char *v = vc_of(t, key);
                int np = 0;
                for (const char *pr = strstr(v, "premise rank.min 0 : or { "); pr; pr = strstr(pr + 1, "premise rank.min 0 : or { ")) {
                    const char *nl = strchr(pr, '\n'); int bars = 0;
                    for (const char *u = pr; nl && (u = strstr(u, " | ")) && u < nl; u++) bars++;
                    if (bars == 2) np++;
                }
                ok3 = np == 2;
                if (!ok3) CHECK(false, "case 2: %s under two premises of three cases:\n%s", key, v);
            }
            CHECK(ok3, "case 2: min(p, q, p + q)'s three falls:\n%s", t ? t : why);
            free(t); gen_rk = NULL; gen_nrk = 0; continue;
        }
        bool ok = t && count(t, " rank.") == 4 + count(t, "premise rank.");
        char hp[32], hq[32], jq[32];
        snprintf(hp, sizeof hp, "%%s%d@head.bb%d", p->id, b1->id); snprintf(hq, sizeof hq, "%%s%d@head.bb%d", q->id, b1->id);
        snprintf(jq, sizeof jq, "%%s%d@join.bb%d", q->id, b6->id);
        for (int k = 0; ok && k < 4; k++) {
            char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.%s %d", b6->id, k < 2 ? "fall" : "bound", 1000 * (k % 2 + 1));
            const char *v = vc_of(t, key), *g = strstr(v, "goal ");
            const char *pr = strstr(v, "premise rank.min 0 : or { "), *nl = pr ? strchr(pr, '\n') : NULL;
            int bars = 0;
            for (const char *u = pr; u && nl && (u = strstr(u, " | ")) && u < nl; u++) bars++;
            if (k < 2) ok = g && strstr(g, jq) && strstr(g, k ? hq : hp) && strstr(g, "<= -1\n") && count(v, "premise rank.") == 1 && bars == 1;
            else ok = g && strstr(g, k == 2 ? hp : hq) && !strstr(g, "@join") && !strstr(v, "premise rank.");
            if (!ok) CHECK(false, "case 0: %s, its goal and premise:\n%s", key, v);
        }
        CHECK(ok, "case 0: two falls and two bounds on the min:\n%s", t ? t : why);
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a ranking component that is the min of rows: under premises, one VC per row before the trip\n");
    // 72. a PIECEWISE ranking (regions): UrbanMine's Fig. 3, `while x != 0 and y > 0 { if x > 0 { if
    // p { x--; y = v1 } else { y-- } } else { if q { x++ } else { y--; x = v2 } } }` over i32 cells,
    // region 0 P (x - 1 >= 0, <x, y>), region 1 N (-x - 1 >= 0, <y, -x>), the edge free. (0) on the
    // back edge: rank.cover 0 under `premise rank.cover 0` (x - 1 <= -1), rank.region 1 under
    // `premise rank.region 0 pre` with the goal x' >= 0 (N's atom failing after the trip),
    // rank.lex 0..2 and 1000..1002 each under its region's pre and post premise and one
    // `premise rank.lex 0`; nothing else, each (kind, ordinal) once. Refused, each region field
    // probed in memory: (1) nreg 5; (2) a region past nreg not 0; (3) g0 + ng past the rows; (4)
    // t0 + nt past them; (5) ng 0; (6) nt 0; (7) a row in two regions; (8) a row in none; (9) a
    // fixed fall; (10) a guard row in a max (P's last tuple row, then N's guard); (15) a max across
    // a region's start (N's guard, then its tuple's first row); (16) rows laid out P's guard, N's,
    // P's tuple, N's, a max over P's last tuple row and N's first; and a recursion's measure with
    // (11) a region whose guard and tuple are its one row (c43: as a loop's), (12) a cont on its
    // first row. (13) two atoms in each guard (P: x - 1 >= 0, y - 1 >= 0; N: -x - 1
    // >= 0, y - 1 >= 0): rank.cover 0 and 1, each under a two-case `premise rank.cover 0`;
    // rank.region 1 under two pre premises and one post (all of N's but the last); each rank.lex
    // under two pre and two post. (14) three regions (P, N, then y - 1 >= 0 with <y>): rank.region
    // 1, 2 and 102, one per pair k < l. (17) c41: P's tuple <max(x, y)>, one component: rank.lex 0
    // and 1 under P's premises, over m (`def rank.max 1`: component 1, the guard row is 0), its
    // after defined only where it falls; N's rank.lex 1000 .. 1002 as in (0), no definition
    for (int c = 0; c < 18; c++) {
        IrFunc *f = ir_func_new(&A, nm("um72"), i32t, IR_FUNC_PURE);
        IrValue *x0 = ir_add_param(f, i32t, nm("x0")), *y0 = ir_add_param(f, i32t, nm("y0")), *pp = ir_add_param(f, i32t, nm("p")),
                *qp = ir_add_param(f, i32t, nm("q")), *v1 = ir_add_param(f, i32t, nm("v1")), *v2 = ir_add_param(f, i32t, nm("v2"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f),
                *b5 = ir_new_block(f), *b6 = ir_new_block(f), *b7 = ir_new_block(f), *b8 = ir_new_block(f), *b9 = ir_new_block(f),
                *b10 = ir_new_block(f), *b11 = ir_new_block(f);
        IrValue *x = ir_alloca(f, b0, i32t), *y = ir_alloca(f, b0, i32t);
        ir_store(f, b0, x, x0); ir_store(f, b0, y, y0); ir_set_br(b0, b1);
        IrValue *lx = ir_load(f, b1, x, i32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_NE, lx, ir_const_int(f, b1, 0, i32t)), b2, b9);
        IrValue *ly = ir_load(f, b2, y, i32t);
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_SGT, ly, ir_const_int(f, b2, 0, i32t)), b3, b9);
        ir_set_br_cond(b3, ir_icmp(f, b3, IR_CMP_SGT, lx, ir_const_int(f, b3, 0, i32t)), b4, b5);
        ir_set_br_cond(b4, ir_icmp(f, b4, IR_CMP_NE, pp, ir_const_int(f, b4, 0, i32t)), b6, b7);
        ir_store(f, b6, x, ir_binop(f, b6, IR_SUB, lx, ir_const_int(f, b6, 1, i32t), i32t)); ir_store(f, b6, y, v1); ir_set_br(b6, b8);
        ir_store(f, b7, y, ir_binop(f, b7, IR_SUB, ly, ir_const_int(f, b7, 1, i32t), i32t)); ir_set_br(b7, b8);
        ir_set_br_cond(b5, ir_icmp(f, b5, IR_CMP_NE, qp, ir_const_int(f, b5, 0, i32t)), b10, b11);
        ir_store(f, b10, x, ir_binop(f, b10, IR_ADD, lx, ir_const_int(f, b10, 1, i32t), i32t)); ir_set_br(b10, b8);
        ir_store(f, b11, y, ir_binop(f, b11, IR_SUB, ly, ir_const_int(f, b11, 1, i32t), i32t)); ir_store(f, b11, x, v2); ir_set_br(b11, b8);
        ir_set_br(b8, b1);
        ir_set_ret(b9, ir_const_int(f, b9, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.nt = 2; r.key[0] = x->id; r.key[1] = y->id; r.nedge = 1; r.from[0] = b8->id; r.fall[0] = -1;
        r.ncomp = 6;
        r.coef[0][0] = 1; r.c0[0] = -1; r.coef[1][0] = 1; r.coef[2][1] = 1;          // P: x - 1 >= 0; <x, y>
        r.coef[3][0] = -1; r.c0[3] = -1; r.coef[4][1] = 1; r.coef[5][0] = -1;        // N: -x - 1 >= 0; <y, -x>
        r.nreg = 2; r.reg[0].g0 = 0; r.reg[0].ng = 1; r.reg[0].t0 = 1; r.reg[0].nt = 2;
        r.reg[1].g0 = 3; r.reg[1].ng = 1; r.reg[1].t0 = 4; r.reg[1].nt = 2;
        const char *bad = NULL;
        switch (c) {
            case 1: r.nreg = 5; bad = "more regions than it may have"; break;
            case 2: r.reg[2].nt = 1; bad = "region past its count"; break;
            case 3: r.reg[1].ng = 4; bad = "region out of shape"; break;
            case 4: r.reg[1].nt = 3; bad = "region out of shape"; break;
            case 5: r.reg[0].ng = 0; bad = "region out of shape"; break;
            case 6: r.reg[0].nt = 0; bad = "region out of shape"; break;
            case 7: r.reg[1].t0 = 3; bad = "row in no region or in two"; break;
            case 8: r.ncomp = 7; bad = "row in no region or in two"; break;
            case 9: r.fall[0] = 0; bad = "a piecewise ranking claim with a fixed falling component"; break;
            case 10: r.cont[3] = 1; bad = "region guard in a max or min of rows"; break;
            case 15: r.cont[4] = 1; bad = "max or min of rows across a region's start"; break;
            case 16:                                            // P's guard 0, N's 1; P's tuple 2, 3; N's 4, 5
                memset(r.coef, 0, sizeof r.coef); memset(r.c0, 0, sizeof r.c0);
                r.coef[0][0] = 1; r.c0[0] = -1; r.coef[1][0] = -1; r.c0[1] = -1; r.coef[2][0] = 1; r.coef[3][1] = 1;
                r.coef[4][1] = 1; r.coef[5][0] = -1;
                r.reg[0].g0 = 0; r.reg[0].t0 = 2; r.reg[1].g0 = 1; r.reg[1].t0 = 4;
                r.cont[4] = 1; bad = "max or min of rows across a region's start"; break;
            case 17: r.cont[2] = 1; break;
            case 13:                                            // P rows 0, 1 | 2, 3; N rows 4, 5 | 6, 7
                memset(r.coef, 0, sizeof r.coef); memset(r.c0, 0, sizeof r.c0); r.ncomp = 8;
                r.coef[0][0] = 1; r.c0[0] = -1; r.coef[1][1] = 1; r.c0[1] = -1; r.coef[2][0] = 1; r.coef[3][1] = 1;
                r.coef[4][0] = -1; r.c0[4] = -1; r.coef[5][1] = 1; r.c0[5] = -1; r.coef[6][1] = 1; r.coef[7][0] = -1;
                r.reg[0].g0 = 0; r.reg[0].ng = 2; r.reg[0].t0 = 2; r.reg[0].nt = 2; r.reg[1].g0 = 4; r.reg[1].ng = 2; r.reg[1].t0 = 6; r.reg[1].nt = 2;
                break;
            case 14:                                            // P 0 | 1; N 2 | 3; Y 4 | 5, one-row tuples
                memset(r.coef, 0, sizeof r.coef); memset(r.c0, 0, sizeof r.c0); r.ncomp = 6;
                r.coef[0][0] = 1; r.c0[0] = -1; r.coef[1][0] = 1; r.coef[2][0] = -1; r.c0[2] = -1; r.coef[3][1] = 1;
                r.coef[4][1] = 1; r.c0[4] = -1; r.coef[5][1] = 1;
                r.nreg = 3; for (int k = 0; k < 3; k++) { r.reg[k].g0 = (uint8_t)(2 * k); r.reg[k].ng = 1; r.reg[k].t0 = (uint8_t)(2 * k + 1); r.reg[k].nt = 1; }
                break;
        }
        gen_rk = &r; gen_nrk = 1;
        if (c == 11 || c == 12) {                              // a recursion: rec(n) calls rec(n - 1)
            IrFunc *h = ir_func_new(&A, nm("rec72"), u32t, IR_FUNC_PURE);
            IrValue *hn = ir_add_param(h, u32t, nm("n"));
            IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h);
            ir_set_br_cond(h0, ir_icmp(h, h0, IR_CMP_EQ, hn, ir_const_int(h, h0, 0, u32t)), h1, h2);
            ir_set_ret(h1, ir_const_int(h, h1, 0, u32t));
            IrInstr *call = ir_instr(h, IR_CALL, u32t, 1); call->aux.callee = nm("rec72");
            call->operands[0] = ir_binop(h, h2, IR_SUB, hn, ir_const_int(h, h2, 1, u32t), u32t); ir_emit(h2, call);
            ir_set_ret(h2, call->result);
            ir_finalize_cfg(h);
            JRank e; memset(&e, 0, sizeof e);
            e.blk = JRANK_ENTRY; e.fn = h; e.ncomp = 1; e.nt = 1; e.key[0] = hn->id; e.coef[0][0] = 1;
            if (c == 11) { e.nreg = 1; e.reg[0].ng = 1; e.reg[0].nt = 1; } else e.cont[0] = 1;
            gen_mod = h; gen_rk = &e; gen_nrk = 1;
            char why[200] = "";
            char *t = gen_with(h, NULL, 0, JGEN_ALL, why);
            CHECK(!t && strstr(why, c == 11 ? "a ranking claim's row in no region or in two" : "a recursion's measure's max or min of rows out of shape"),
                  "case %d: a recursion's measure with a %s refused: got \"%s\"", c, c == 11 ? "region" : "cont", why);
            free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
            continue;
        }
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (bad) { CHECK(!t && strstr(why, bad), "case %d: refused (%s): got \"%s\"", c, bad, why); free(t); gen_rk = NULL; gen_nrk = 0; continue; }
        if (c == 13) {
            bool ok2 = t != NULL;
            for (int k = 0; ok2 && k < 2; k++) {
                char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.cover %d", b8->id, k);
                const char *v = vc_of(t, key), *pr = strstr(v, "premise rank.cover 0 : or { "), *nl = pr ? strchr(pr, '\n') : NULL;
                int bars = 0;
                for (const char *u = pr; u && nl && (u = strstr(u, " | ")) && u < nl; u++) bars++;
                ok2 = *v && bars == 1 && count(v, "premise rank.") == 1;
            }
            char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.region 1", b8->id);
            const char *v = ok2 ? vc_of(t, key) : "";
            ok2 = ok2 && count(v, "premise rank.region 0 pre : ") == 2 && count(v, "premise rank.region 1 post : ") == 1 && count(v, "premise rank.") == 3;
            for (int k = 0; ok2 && k < 6; k++) {
                snprintf(key, sizeof key, "bb%d.0 rank.lex %d", b8->id, k < 3 ? k : 1000 + k - 3);
                const char *w = vc_of(t, key); int rk = k < 3 ? 0 : 1;
                char rp[48], rq[48]; snprintf(rp, sizeof rp, "premise rank.region %d pre : ", rk); snprintf(rq, sizeof rq, "premise rank.region %d post : ", rk);
                ok2 = count(w, rp) == 2 && count(w, rq) == 2;
            }
            CHECK(ok2 && count(t, " rank.") == 9 + count(t, "premise rank."), "case 13: two-atom guards:\n%s", t ? t : why);
            free(t); gen_rk = NULL; gen_nrk = 0; continue;
        }
        if (c == 14) {
            bool ok3 = t != NULL;
            const int pairs[3] = { 1, 2, 102 };
            for (int k = 0; ok3 && k < 3; k++) { char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.region %d\n", b8->id, pairs[k]); ok3 = count(t, key) == 1; }
            char hk[48]; snprintf(hk, sizeof hk, "bb%d.0 rank.region ", b8->id);   // a VC's header line
            CHECK(ok3 && count(t, hk) == 3, "case 14: one rank.region per pair k < l:\n%s", t ? t : why);
            free(t); gen_rk = NULL; gen_nrk = 0; continue;
        }
        if (c == 17) {
            bool ok4 = t != NULL;
            for (int k = 0; ok4 && k < 5; k++) {
                char key[48]; snprintf(key, sizeof key, "bb%d.0 rank.lex %d\n", b8->id, k < 2 ? k : 1000 + k - 2);
                ok4 = count(t, key) == 1;
                snprintf(key, sizeof key, "bb%d.0 rank.lex %d", b8->id, k < 2 ? k : 1000 + k - 2);
                const char *v = ok4 ? vc_of(t, key) : "";
                if (k < 2)
                    ok4 = ok4 && count(v, "def rank.max 1 pre : ") == 3 && count(v, "def rank.max 1 post : ") == (k ? 0 : 3) &&
                          count(v, "premise rank.region 0 pre : ") == 1 && count(v, "premise rank.region 0 post : ") == 1 &&
                          count(v, "premise rank.") == 2 && strstr(v, k ? "goal -1*rank.max.1.pre <= 0\n" : "goal -1*rank.max.1.pre + 1*rank.max.1.post <= -1\n");
                else ok4 = ok4 && !strstr(v, "def rank.") && count(v, "premise rank.region 1 pre : ") == 1 && count(v, "premise rank.lex 0 : or { ") == 1;
                if (!ok4) CHECK(false, "case 17: rank.lex %d:\n%s", k < 2 ? k : 1000 + k - 2, v);
            }
            char k2[48]; snprintf(k2, sizeof k2, "bb%d.0 rank.lex 2\n", b8->id);
            CHECK(ok4 && !strstr(t, k2), "case 17: P's tuple one component:\n%s", t ? t : why);
            free(t); gen_rk = NULL; gen_nrk = 0; continue;
        }
        const char *want[8] = { "rank.cover 0", "rank.region 1", "rank.lex 0", "rank.lex 1", "rank.lex 2", "rank.lex 1000", "rank.lex 1001", "rank.lex 1002" };
        bool ok = t && count(t, " rank.") == 8 + count(t, "premise rank.");
        for (int k = 0; ok && k < 8; k++) {
            char key[48]; snprintf(key, sizeof key, "bb%d.0 %s\n", b8->id, want[k]);
            ok = count(t, key) == 1;                            // each (kind, ordinal) once
            snprintf(key, sizeof key, "bb%d.0 %s", b8->id, want[k]);
            const char *v = ok ? vc_of(t, key) : "";
            if (k == 0) ok = ok && count(v, "premise rank.cover 0 : ") == 1 && count(v, "premise rank.") == 1;
            else if (k == 1) {
                const char *g = strstr(v, "goal ");
                char jx[32]; snprintf(jx, sizeof jx, "-1*%%s%d@join.bb%d <= 0\n", x->id, b8->id);
                ok = ok && count(v, "premise rank.region 0 pre : ") == 1 && count(v, "premise rank.") == 1 && g && strstr(g, jx);
            } else {
                char rp[48], rq[48]; int rk = k < 5 ? 0 : 1;
                snprintf(rp, sizeof rp, "premise rank.region %d pre : ", rk); snprintf(rq, sizeof rq, "premise rank.region %d post : ", rk);
                ok = ok && count(v, rp) == 1 && count(v, rq) == 1 && count(v, "premise rank.lex 0 : or { ") == 1 && count(v, "premise rank.") == 3;
            }
            if (!ok) CHECK(false, "case 0: %s, its premises:\n%s", want[k], v);
        }
        CHECK(ok && !strstr(t, "rank.keep") && !strstr(t, "rank.fall") && !strstr(t, "rank.bound"), "case 0: the eight region VCs only:\n%s", t ? t : why);
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a piecewise ranking: covered, never up a region, each region's tuple falling\n");
    // 73. every obligation of a function has its own ID. Loops headed by b1 (outer) and b2 (inner),
    // b3 their body: (0) b3 closes BOTH, `br c ? b2 : b1`, so both loops' ranking VCs are at b3's end
    // with one kind and ordinal: refused, two obligations of one ID; (1) b3 closes the inner alone
    // (`br b2`, the outer closed by b2's exit): generated, every (point, kind, ordinal) once
    for (int c = 0; c < 2; c++) {
        IrFunc *f = ir_func_new(&A, nm("dup73"), u32t, IR_FUNC_PURE);
        IrValue *cp = ir_add_param(f, u32t, nm("c"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u32t), *j = ir_alloca(f, b0, u32t);
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u32t)); ir_store(f, b0, j, ir_const_int(f, b0, 0, u32t)); ir_set_br(b0, b1);
        IrValue *li = ir_load(f, b1, i, u32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, li, ir_const_int(f, b1, 10, u32t)), b2, b4);
        IrValue *lj = ir_load(f, b2, j, u32t);
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_ULT, lj, ir_const_int(f, b2, 5, u32t)), b3, b1);
        ir_store(f, b3, j, ir_binop(f, b3, IR_ADD, lj, ir_const_int(f, b3, 1, u32t), u32t));
        ir_store(f, b3, i, ir_binop(f, b3, IR_ADD, ir_load(f, b3, i, u32t), ir_const_int(f, b3, 1, u32t), u32t));
        if (c == 0) ir_set_br_cond(b3, ir_icmp(f, b3, IR_CMP_NE, cp, ir_const_int(f, b3, 0, u32t)), b2, b1);
        else ir_set_br(b3, b2);
        ir_set_ret(b4, ir_const_int(f, b4, 0, u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JRank r2[2]; memset(r2, 0, sizeof r2);
        r2[0].blk = b1->id; r2[0].fn = f; r2[0].ncomp = 1; r2[0].nt = 1; r2[0].key[0] = i->id; r2[0].coef[0][0] = -1; r2[0].c0[0] = 10;
        r2[0].nedge = c == 0 ? 2 : 1; r2[0].from[0] = b2->id; r2[0].from[1] = b3->id;
        r2[1].blk = b2->id; r2[1].fn = f; r2[1].ncomp = 1; r2[1].nt = 1; r2[1].key[0] = j->id; r2[1].coef[0][0] = -1; r2[1].c0[0] = 5;
        r2[1].nedge = 1; r2[1].from[0] = b3->id;
        gen_rk = r2; gen_nrk = 2;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (c == 0) CHECK(!t && strstr(why, "two obligations of one ID"), "case 0: a block closing two loops, refused: got \"%s\"", why);
        else {
            bool ok = t != NULL;
            for (const char *h = t; ok && (h = strstr(h, "\nvc ")); h++) {   // each VC header once
                const char *e = strchr(h + 1, '\n'); char line[160];
                snprintf(line, sizeof line, "%.*s\n", (int)(e - h - 1), h + 1);
                ok = count(t, line) == 1;
            }
            CHECK(ok && strstr(t, "rank.fall"), "case 1: generated, every ID once:\n%s", t ? t : why);
        }
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    // (2) the duplicate pair sorted first: the closing block made first (bb1, nothing before it
    // owes), the inner ranking a max (its falls ordinals 1000 and 2000), so the two loops share only
    // `rank.bound 0`, which sorts before every other ID: refused all the same
    {   IrFunc *f = ir_func_new(&A, nm("dup73b"), u32t, IR_FUNC_PURE);
        IrValue *cp = ir_add_param(f, u32t, nm("c"));
        IrBlock *b0 = f->entry, *bl = ir_new_block(f), *h1 = ir_new_block(f), *h2 = ir_new_block(f), *bx = ir_new_block(f);
        IrValue *i = ir_alloca(f, b0, u32t), *j = ir_alloca(f, b0, u32t);
        ir_store(f, b0, i, ir_const_int(f, b0, 0, u32t)); ir_store(f, b0, j, ir_const_int(f, b0, 0, u32t)); ir_set_br(b0, h1);
        IrValue *li = ir_load(f, h1, i, u32t);
        ir_set_br_cond(h1, ir_icmp(f, h1, IR_CMP_ULT, li, ir_const_int(f, h1, 10, u32t)), h2, bx);
        IrValue *lj = ir_load(f, h2, j, u32t);
        ir_store(f, h2, j, ir_binop(f, h2, IR_ADD, lj, ir_const_int(f, h2, 1, u32t), u32t));
        ir_set_br_cond(h2, ir_icmp(f, h2, IR_CMP_ULT, lj, ir_const_int(f, h2, 5, u32t)), bl, h1);
        ir_set_br_cond(bl, ir_icmp(f, bl, IR_CMP_NE, cp, ir_const_int(f, bl, 0, u32t)), h2, h1);
        ir_set_ret(bx, ir_const_int(f, bx, 0, u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JRank r2[2]; memset(r2, 0, sizeof r2);
        r2[0].blk = h1->id; r2[0].fn = f; r2[0].ncomp = 1; r2[0].nt = 1; r2[0].key[0] = i->id; r2[0].coef[0][0] = -1; r2[0].c0[0] = 10;
        r2[0].nedge = 2; r2[0].from[0] = h2->id; r2[0].from[1] = bl->id;
        r2[1].blk = h2->id; r2[1].fn = f; r2[1].ncomp = 2; r2[1].nt = 1; r2[1].key[0] = j->id; r2[1].coef[0][0] = -1; r2[1].c0[0] = 5;
        r2[1].coef[1][0] = -1; r2[1].c0[1] = 4; r2[1].cont[1] = 1; r2[1].nedge = 1; r2[1].from[0] = bl->id;
        gen_rk = r2; gen_nrk = 2;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        CHECK(!t && strstr(why, "two obligations of one ID"), "case 2: the duplicate sorted first, refused: got \"%s\"", why);
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    // (3), (4): a call through a pointer that may close a recursion (test 67's shape: h(fp, x) calls
    // fp(x); p and q each call h(&self, x - 1), so h's call has the targets p and q). (3) no measure
    // at all: h's call is `outside` ONCE, not once per target; (4) measures for h and p, none for q:
    // at h's call p's rec.fall (a goal) and q's (outside) under their own ordinals, generated
    for (int c = 3; c < 5; c++) {
        IrType *fnt = ir_type_new(&A, IRT_FUNC); fnt->elem = u32t;
        IrType *fpt = ir_type_new(&A, IRT_PTR); fpt->elem = fnt;
        IrFunc *h = ir_func_new(&A, nm("h73"), u32t, IR_FUNC_PURE), *fp = ir_func_new(&A, nm("p73"), u32t, IR_FUNC_PURE),
               *fq = ir_func_new(&A, nm("q73"), u32t, IR_FUNC_PURE);
        IrValue *hf = ir_add_param(h, fpt, nm("fp")), *hx = ir_add_param(h, u32t, nm("x"));
        IrBlock *h0 = h->entry, *h1 = ir_new_block(h), *h2 = ir_new_block(h);
        ir_set_br_cond(h0, ir_icmp(h, h0, IR_CMP_EQ, hx, ir_const_int(h, h0, 0, u32t)), h1, h2);
        ir_set_ret(h1, ir_const_int(h, h1, 0, u32t));
        IrInstr *ic = ir_instr(h, IR_CALL, u32t, 2); ic->operands[0] = hf; ic->operands[1] = hx; ir_emit(h2, ic);
        int hci = nins(h2) - 1;
        ir_set_ret(h2, ic->result);
        ir_finalize_cfg(h);
        IrFunc *pq[2] = { fp, fq }; IrValue *px[2];
        for (int w = 0; w < 2; w++) {
            IrFunc *u = pq[w];
            px[w] = ir_add_param(u, u32t, nm("x"));
            IrBlock *u0 = u->entry;
            IrValue *self = ir_func_ref(u, u0, nm(w ? "q73" : "p73"), fpt);
            IrValue *xm = ir_binop(u, u0, IR_SUB, px[w], ir_const_int(u, u0, 1, u32t), u32t);
            IrInstr *dc = ir_instr(u, IR_CALL, u32t, 2); dc->aux.callee = nm("h73"); dc->operands[0] = self; dc->operands[1] = xm; ir_emit(u0, dc);
            ir_set_ret(u0, dc->result);
            ir_finalize_cfg(u);
        }
        h->next = fp; fp->next = fq; gen_mod = h;
        JRank r3[2]; memset(r3, 0, sizeof r3);
        IrFunc *fs[2] = { h, fp }; IrValue *ks[2] = { hx, px[0] };
        for (int w = 0; w < 2; w++) { r3[w].blk = JRANK_ENTRY; r3[w].fn = fs[w]; r3[w].ncomp = 1; r3[w].nt = 1; r3[w].key[0] = ks[w]->id;
                                      r3[w].coef[0][0] = 2; r3[w].c0[0] = w == 0 ? 1 : 0; }
        gen_rk = c == 4 ? r3 : NULL; gen_nrk = c == 4 ? 2 : 0;
        char why[200] = "";
        char *t = gen_ids(h, why);
        char g0[48], g1[48], o0[48], o1[48];                 // a goal's ID, an outside's
        snprintf(g0, sizeof g0, "bb%d.%d rec.fall 0\n", h2->id, hci); snprintf(g1, sizeof g1, "bb%d.%d rec.fall 1\n", h2->id, hci);
        snprintf(o0, sizeof o0, "bb%d.%d rec.fall 0 outside\n", h2->id, hci); snprintf(o1, sizeof o1, "bb%d.%d rec.fall 1 outside\n", h2->id, hci);
        if (c == 3) CHECK(t && count(t, o0) == 1 && count(t, " outside") == 1 && !strstr(t, "rec.fall 1"),
                          "case 3: one outside for h's call, not one per target:\n%s", t ? t : why);
        else CHECK(t && count(t, g0) == 1 && count(t, o1) == 1 && !strstr(t, o0) && !strstr(t, g1) && count(t, " outside") == 1,
                   "case 4: p's rec.fall (a goal, ordinal 0) and q's (outside, ordinal 1):\n%s", t ? t : why);
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  every obligation of a function has its own ID\n");
    // 74. an ENTRY term: LarrazOliveras' Fig. 1, `while y >= 1 { x = x - 1; while y < z { x++; z-- };
    // y = x + y }` over i32 cells, its inner header b3 entered from b2 alone. (0) x + z - entry(x) -
    // entry(z) <= 0 at b3: entry(x) the store x - 1 (b2's version), entry(z) z's outer-header version,
    // the same variables in the statement, the keep and the entry, whose goal is 0 <= 0; (1) a ranking
    // at b3 <z - y + entry(z)>: accepted, entry(z) gone from its fall's goal (fixed, it cancels).
    // Refused: (2) ENTRY at b5 (no loop header); (3) a header entered from two blocks; (4) ENTRY over
    // a parameter (no cell); (5) a claim of entry(x) alone; (6) entry(x) and a parameter (no version
    // b3 makes); (7) x twice, both VALUE (one term twice); (8) entry(x) twice
    for (int c = 0; c < 9; c++) {
        IrFunc *f = ir_func_new(&A, nm("lz74"), i32t, IR_FUNC_PURE);
        IrValue *x0 = ir_add_param(f, i32t, nm("x0")), *y0 = ir_add_param(f, i32t, nm("y0")), *z0 = ir_add_param(f, i32t, nm("z0"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f),
                *b5 = ir_new_block(f), *b6 = ir_new_block(f), *b7 = ir_new_block(f);
        IrValue *x = ir_alloca(f, b0, i32t), *y = ir_alloca(f, b0, i32t), *z = ir_alloca(f, b0, i32t);
        ir_store(f, b0, x, x0); ir_store(f, b0, y, y0); ir_store(f, b0, z, z0); ir_set_br(b0, b1);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_SGE, ir_load(f, b1, y, i32t), ir_const_int(f, b1, 1, i32t)), b2, b6);
        ir_store(f, b2, x, ir_binop(f, b2, IR_SUB, ir_load(f, b2, x, i32t), ir_const_int(f, b2, 1, i32t), i32t));
        if (c == 3) ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_EQ, x0, ir_const_int(f, b2, 0, i32t)), b3, b7);   // two ways into b3
        else ir_set_br(b2, b3);
        ir_set_br(b7, b3);
        ir_set_br_cond(b3, ir_icmp(f, b3, IR_CMP_SLT, ir_load(f, b3, y, i32t), ir_load(f, b3, z, i32t)), b4, b5);
        ir_store(f, b4, x, ir_binop(f, b4, IR_ADD, ir_load(f, b4, x, i32t), ir_const_int(f, b4, 1, i32t), i32t));
        ir_store(f, b4, z, ir_binop(f, b4, IR_SUB, ir_load(f, b4, z, i32t), ir_const_int(f, b4, 1, i32t), i32t)); ir_set_br(b4, b3);
        ir_store(f, b5, y, ir_binop(f, b5, IR_ADD, ir_load(f, b5, x, i32t), ir_load(f, b5, y, i32t), i32t)); ir_set_br(b5, b1);
        ir_set_ret(b6, ir_const_int(f, b6, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        JClaim cl; memset(&cl, 0, sizeof cl);
        cl.blk = b3->id; cl.fn = f; cl.nt = 4; cl.key[0] = x->id; cl.key[1] = z->id; cl.key[2] = x->id; cl.key[3] = z->id;
        cl.tk[2] = JTERM_ENTRY; cl.tk[3] = JTERM_ENTRY; cl.coef[0] = 1; cl.coef[1] = 1; cl.coef[2] = -1; cl.coef[3] = -1;
        const char *bad = NULL;
        JRank r; memset(&r, 0, sizeof r);
        switch (c) {
            case 1: r.blk = b3->id; r.fn = f; r.ncomp = 1; r.nt = 3; r.key[0] = z->id; r.key[1] = y->id; r.key[2] = z->id; r.tk[2] = JTERM_ENTRY;
                    r.coef[0][0] = 1; r.coef[0][1] = -1; r.coef[0][2] = 1; r.nedge = 1; r.from[0] = b4->id; break;
            case 2: cl.blk = b5->id; bad = "ENTRY term at a block that is no loop header"; break;
            case 3: bad = "ENTRY term at a loop entered by more than one edge"; break;
            case 4: cl.key[2] = x0->id; bad = "ENTRY term is no integer cell"; break;
            case 5: cl.nt = 1; cl.key[0] = x->id; cl.tk[0] = JTERM_ENTRY; cl.coef[0] = 1; cl.rhs = 100; bad = "no version its block makes afresh"; break;
            case 6: cl.nt = 2; cl.key[0] = x->id; cl.tk[0] = JTERM_ENTRY; cl.key[1] = x0->id; cl.tk[1] = JTERM_VALUE; cl.coef[1] = -1; bad = "no version its block makes afresh"; break;
            case 7: cl.tk[2] = JTERM_VALUE; bad = "naming one term twice"; break;
            case 8: cl.tk[0] = JTERM_ENTRY; bad = "naming one term twice"; break;
        }
        if (c == 1) { gen_rk = &r; gen_nrk = 1; }
        char why[200] = "";
        char *t = gen_with(f, &cl, 1, JGEN_ALL, why);
        if (bad) { CHECK(!t && strstr(why, bad), "case %d: refused (%s): got \"%s\"", c, bad, why); free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL; continue; }
        char ex[40], ez[40]; snprintf(ex, sizeof ex, "%%s%d@bb%d.", x->id, b2->id); snprintf(ez, sizeof ez, "%%s%d@head.bb%d", z->id, b1->id);
        if (c == 0) {
            char k1[48], k2[48]; snprintf(k1, sizeof k1, "bb%d.%d inv.keep", b4->id, nins(b4)); snprintf(k2, sizeof k2, "bb%d.%d inv.entry", b2->id, nins(b2));
            const char *vk = t ? vc_of(t, k1) : "", *gk = strstr(vk, "goal ");   // vc_of's buffer is one: check before the next
            CHECK(gk && strstr(gk, ex) && strstr(gk, ez) && strstr(vk, "claim bb"), "case 0: the keep's goal names entry(x) and entry(z):\n%s", vk);
            const char *ve = t ? vc_of(t, k2) : "", *ge = strstr(ve, "goal ");
            CHECK(ge && !strncmp(ge, "goal 0 <= 0\n", 12), "case 0: the entry's goal is 0 <= 0:\n%s", ve);
        } else {
            char kf[48]; snprintf(kf, sizeof kf, "bb%d.%d rank.fall", b4->id, nins(b4));
            const char *vf = t ? vc_of(t, kf) : "", *gf = strstr(vf, "goal ");
            char zv[40]; snprintf(zv, sizeof zv, "%%s%d@head.bb%d", z->id, b1->id);
            CHECK(gf && !strstr(gf, zv), "case 1: a ranking's ENTRY term cancels from its fall:\n%s", vf);
        }
        free(t);
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  an ENTRY term: a cell's version on its loop's entry edge, fixed in the loop\n");
    // 75. a FREE edge over a MAX or MIN component in a tuple of two (c41): 4NestedWith3Variables, all
    // i32, `while q > 0 { q = q + a - 1; (a, b) = (3a - 4b, 4a + 3b) }`. (0) <min(2^32 -+ a -+ b), q>:
    // rank.lex 0 .. 2, each under m and m' defined (five hypotheses a side: four atoms, a four-way
    // disjunction; m <= 2^32 - a - b the first atom) and `premise rank.lex 0` over m alone (m did
    // not fall, or m < 0); goals m' <= m, then q's fall and bound. (1) <q, max(a, -a)>: the max is
    // named only where its VC uses it: not in rank.lex 0 (q kept), m and m' in rank.lex 1 (it
    // falls), m alone in rank.lex 2 (bounded). (2) TWO groups, <min(2^32 -+ a -+ b), max(q, -q)>
    // (Main Compiler Work's f11 proposes it): each its own fresh pair, rank.min.0 and rank.max.1;
    // the min (not last) defined both sides in every VC, the max (last) as in (1)
    for (int c = 0; c < 3; c++) {
        IrFunc *f = ir_func_new(&A, nm("nest75"), i32t, IR_FUNC_PURE);
        IrValue *a0p = ir_add_param(f, i32t, nm("a0")), *b0p = ir_add_param(f, i32t, nm("b0")), *q0p = ir_add_param(f, i32t, nm("q0"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f);
        IrValue *a = ir_alloca(f, b0, i32t), *b = ir_alloca(f, b0, i32t), *q = ir_alloca(f, b0, i32t);
        ir_store(f, b0, a, a0p); ir_store(f, b0, b, b0p); ir_store(f, b0, q, q0p); ir_set_br(b0, b1);
        IrValue *lq = ir_load(f, b1, q, i32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_SGT, lq, ir_const_int(f, b1, 0, i32t)), b2, b3);
        IrValue *la = ir_load(f, b2, a, i32t), *lb = ir_load(f, b2, b, i32t);
        IrValue *k3 = ir_const_int(f, b2, 3, i32t), *k4 = ir_const_int(f, b2, 4, i32t);
        IrValue *q1 = ir_binop(f, b2, IR_SUB, ir_binop(f, b2, IR_ADD, lq, la, i32t), ir_const_int(f, b2, 1, i32t), i32t);
        IrValue *a1 = ir_binop(f, b2, IR_SUB, ir_binop(f, b2, IR_MUL, k3, la, i32t), ir_binop(f, b2, IR_MUL, k4, lb, i32t), i32t);
        IrValue *bb1 = ir_binop(f, b2, IR_ADD, ir_binop(f, b2, IR_MUL, k4, la, i32t), ir_binop(f, b2, IR_MUL, k3, lb, i32t), i32t);
        ir_store(f, b2, q, q1); ir_store(f, b2, a, a1); ir_store(f, b2, b, bb1); ir_set_br(b2, b1);
        ir_set_ret(b3, ir_const_int(f, b3, 0, i32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        int ni = 0; for (IrInstr *i = b2->instrs; i; i = i->next) ni++;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = b1->id; r.fn = f; r.nt = 3; r.key[0] = a->id; r.key[1] = b->id; r.key[2] = q->id;
        r.nedge = 1; r.from[0] = b2->id; r.fall[0] = -1;
        if (c != 1) {
            r.ncomp = c == 2 ? 6 : 5;
            for (int k = 0; k < 4; k++) { r.coef[k][0] = (k & 2) ? 1 : -1; r.coef[k][1] = (k & 1) ? 1 : -1; r.c0[k] = (__int128)1 << 32; if (k) r.cont[k] = 2; }
            r.coef[4][2] = 1;
            if (c == 2) { r.coef[5][2] = -1; r.cont[5] = 1; }   // max(q, -q)
        } else {
            r.ncomp = 3; r.coef[0][2] = 1; r.coef[1][0] = 1; r.coef[2][0] = -1; r.cont[2] = 1;
        }
        gen_rk = &r; gen_nrk = 1;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        bool ok = t != NULL;
        if (c == 2) {                                          // per VC: min defs pre/post, max defs pre/post
            const int want[3][4] = { { 5, 5, 0, 0 }, { 5, 5, 3, 3 }, { 5, 5, 3, 0 } };
            const char *goals[3] = { "goal -1*rank.min.0.pre + 1*rank.min.0.post <= 0\n", "goal -1*rank.max.1.pre + 1*rank.max.1.post <= -1\n",
                                     "goal -1*rank.max.1.pre <= 0\n" };
            for (int qq = 0; ok && qq < 3; qq++) {
                char key[48]; snprintf(key, sizeof key, "bb%d.%d rank.lex %d", b2->id, ni, qq);
                const char *v = vc_of(t, key);
                ok = *v && count(v, "def rank.min 0 pre : ") == want[qq][0] && count(v, "def rank.min 0 post : ") == want[qq][1] &&
                     count(v, "def rank.max 1 pre : ") == want[qq][2] && count(v, "def rank.max 1 post : ") == want[qq][3] &&
                     count(v, "premise rank.lex 0 : or { 1*rank.min.0.pre + -1*rank.min.0.post <= 0 | 1*rank.min.0.pre <= -1 }\n") == 1 &&
                     count(v, "premise rank.") == 1 && strstr(v, goals[qq]) && !strstr(v, "rank.max.0") && !strstr(v, "rank.min.1");
                if (!ok) CHECK(false, "case 2: rank.lex %d, two groups, each named once where used:\n%s", qq, v);
            }
            char k3s[48]; snprintf(k3s, sizeof k3s, "bb%d.%d rank.lex 3", b2->id, ni);
            CHECK(ok && !strstr(t, k3s), "case 2: rank.lex 0 .. 2 only:\n%s", t ? t : why);
            free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
            continue;
        }
        const char *k = c == 0 ? "min" : "max";
        int dc = c == 0 ? 0 : 1, rows = c == 0 ? 4 : 2;      // the group's component, its rows
        for (int qq = 0; ok && qq < 3; qq++) {
            char key[48]; snprintf(key, sizeof key, "bb%d.%d rank.lex %d", b2->id, ni, qq);
            const char *v = vc_of(t, key);
            char dp[40], dq[40]; snprintf(dp, sizeof dp, "def rank.%s %d pre : ", k, dc); snprintf(dq, sizeof dq, "def rank.%s %d post : ", k, dc);
            int wp = c == 0 || qq > 0 ? rows + 1 : 0, wq = c == 0 || qq == 1 ? rows + 1 : 0;
            ok = *v && count(v, dp) == wp && count(v, dq) == wq && count(v, "premise rank.lex 0 : or { ") == 1 && count(v, "premise rank.") == 1;
            if (ok && c == 0) {                                // over m alone; the first atom; four disjuncts
                char pr[120], at[160];
                snprintf(pr, sizeof pr, "premise rank.lex 0 : or { 1*rank.min.0.pre + -1*rank.min.0.post <= 0 | 1*rank.min.0.pre <= -1 }\n");
                snprintf(at, sizeof at, "def rank.min 0 pre : 1*%%s%d@head.bb%d + 1*%%s%d@head.bb%d + 1*rank.min.0.pre <= 4294967296\n", a->id, b1->id, b->id, b1->id);
                const char *d = strstr(v, "def rank.min 0 pre : or { "), *e = d ? strchr(d, '\n') : NULL;
                int bars = 0;
                for (const char *u = d; u && e && (u = strstr(u, " | ")) && u < e; u++) bars++;
                ok = strstr(v, pr) && strstr(v, at) && bars == 3 &&
                     (qq > 0 || strstr(v, "goal -1*rank.min.0.pre + 1*rank.min.0.post <= 0\n"));
            }
            if (!ok) CHECK(false, "case %d: rank.lex %d, its definitions and premise:\n%s", c, qq, v);
        }
        char k3s[48]; snprintf(k3s, sizeof k3s, "bb%d.%d rank.lex 3", b2->id, ni);
        CHECK(ok && !strstr(t, k3s), "case %d: rank.lex 0 .. 2 only:\n%s", c, t ? t : why);
        free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a free edge over a max or min: a fresh value a side, defined exactly, where its VC uses it\n");
    // 76. a RECURSION's measure of several rows (c42): every call free, owing c35's lexicographic
    // decrease (`rec.lex` q) from the caller's parameters at entry to the arguments, a MAX or MIN
    // c41's exact m. (0) LeeJonesBen-Amram Ex5, all i32: `f(x, y)`: y == 0 returns; x == 0 calls
    // f(y, y - 1), else f(y, x - 1); ranked MAX(2x + 2^33, 2y + 1 + 2^33): at each call rec.lex 0
    // (m' <= m - 1, m and m' defined, m' over the arguments: x's row at y) and 1 (m >= 0, m alone), no
    // rec.fall, rec.bound or premise; m's recipe the two rows. (1) Parts, `parts(p, q)`: p <= 0 or q
    // <= 0 return; q > p calls parts(p, p), else parts(p - q, q) and parts(p, q - 1); ranked <p, q>:
    // at each call rec.lex 0 .. 2 under `premise rank.lex 0`, no definition. Refused: (2) Ex5's
    // max in p and q calling each other, from either side (v1); (3) a row's coefficient past 2^66;
    // (4) a row's constant past it; (5) no rows; (6) nine; (7) a region field past nreg 0
    for (int c = 0; c < 8; c++) {
        IrFunc *f = ir_func_new(&A, nm(c == 1 ? "parts76" : "f76"), i32t, IR_FUNC_PURE);
        IrValue *x = ir_add_param(f, i32t, nm("x")), *y = ir_add_param(f, i32t, nm("y"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f), *b5 = ir_new_block(f);
        IrBlock *cb[3] = { NULL, NULL, NULL }; int ci[3] = { 0, 0, 0 }, ncall = 0;
        IrFunc *other = NULL;
        const char *self = c == 1 ? "parts76" : "f76", *callee = c == 2 ? "g76" : self;
        #define CALL2(b, a0, a1) do { IrValue *a0_ = (a0), *a1_ = (a1); cb[ncall] = (b); ci[ncall] = nins(b);   /* after its arguments */ \
                                     IrInstr *k_ = ir_instr(f, IR_CALL, i32t, 2); k_->aux.callee = nm(callee); \
                                     k_->operands[0] = a0_; k_->operands[1] = a1_; ir_emit((b), k_); ncall++; } while (0)
        if (c != 1) {                                           // Ex5
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_EQ, y, ir_const_int(f, b0, 0, i32t)), b1, b2);
            ir_set_ret(b1, x);
            ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_EQ, x, ir_const_int(f, b2, 0, i32t)), b3, b4);
            CALL2(b3, y, ir_binop(f, b3, IR_SUB, y, ir_const_int(f, b3, 1, i32t), i32t));
            ir_set_ret(b3, ir_const_int(f, b3, 0, i32t));
            CALL2(b4, y, ir_binop(f, b4, IR_SUB, x, ir_const_int(f, b4, 1, i32t), i32t));
            ir_set_ret(b4, ir_const_int(f, b4, 0, i32t));
            ir_set_ret(b5, ir_const_int(f, b5, 0, i32t));
        } else {                                                // Parts
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SLE, x, ir_const_int(f, b0, 0, i32t)), b1, b2);
            ir_set_ret(b1, ir_const_int(f, b1, 1, i32t));
            ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_SLE, y, ir_const_int(f, b2, 0, i32t)), b5, b3);
            ir_set_ret(b5, ir_const_int(f, b5, 0, i32t));
            ir_set_br_cond(b3, ir_icmp(f, b3, IR_CMP_SGT, y, x), b4, ir_new_block(f));
            CALL2(b4, x, x);
            ir_set_ret(b4, ir_const_int(f, b4, 0, i32t));
            IrBlock *b6 = b3->term.b;
            CALL2(b6, ir_binop(f, b6, IR_SUB, x, y, i32t), y);
            CALL2(b6, x, ir_binop(f, b6, IR_SUB, y, ir_const_int(f, b6, 1, i32t), i32t));
            ir_set_ret(b6, ir_const_int(f, b6, 0, i32t));
        }
        #undef CALL2
        ir_finalize_cfg(f);
        if (c == 2) {                                           // g76(x, y) calls f76(y, x - 1)
            other = ir_func_new(&A, nm("g76"), i32t, IR_FUNC_PURE);
            IrValue *gx = ir_add_param(other, i32t, nm("x")), *gy = ir_add_param(other, i32t, nm("y"));
            IrBlock *g0 = other->entry, *g1 = ir_new_block(other), *g2 = ir_new_block(other);
            ir_set_br_cond(g0, ir_icmp(other, g0, IR_CMP_EQ, gy, ir_const_int(other, g0, 0, i32t)), g1, g2);
            ir_set_ret(g1, gx);
            IrInstr *k_ = ir_instr(other, IR_CALL, i32t, 2); k_->aux.callee = nm("f76");
            k_->operands[0] = gy; k_->operands[1] = ir_binop(other, g2, IR_SUB, gx, ir_const_int(other, g2, 1, i32t), i32t); ir_emit(g2, k_);
            ir_set_ret(g2, k_->result);
            ir_finalize_cfg(other);
            f->next = other;
        }
        gen_mod = f;
        JRank r[2]; memset(r, 0, sizeof r);
        r[0].blk = JRANK_ENTRY; r[0].fn = f; r[0].nt = 2; r[0].key[0] = x->id; r[0].key[1] = y->id; r[0].ncomp = 2;
        if (c != 1) { r[0].coef[0][0] = 2; r[0].c0[0] = (__int128)1 << 33; r[0].coef[1][1] = 2; r[0].c0[1] = ((__int128)1 << 33) + 1; r[0].cont[1] = 1; }
        else { r[0].coef[0][0] = 1; r[0].coef[1][1] = 1; }
        int nr = 1;
        const char *bad = NULL;
        if (c == 2) {                                           // g76's measure plain: x + y
            r[1].blk = JRANK_ENTRY; r[1].fn = other; r[1].nt = 2; r[1].key[0] = other->params->value->id; r[1].key[1] = other->params->next->value->id;
            r[1].ncomp = 1; r[1].coef[0][0] = 1; r[1].coef[0][1] = 1; nr = 2; bad = "a recursion's measure of several rows in a mutual recursion (v1)";
        }
        if (c == 3) { r[0].coef[1][1] = (__int128)1 << 67; bad = "a recursion's measure's coefficient out of range"; }
        if (c == 4) { r[0].c0[1] = (__int128)1 << 67; bad = "a recursion's measure's constant out of range"; }
        if (c == 5) { r[0].ncomp = 0; bad = "a recursion's measure of no rows, or of more than it may have"; }
        if (c == 6) { r[0].ncomp = 9; bad = "a recursion's measure of no rows, or of more than it may have"; }
        if (c == 7) { r[0].reg[2].nt = 1; bad = "a ranking claim's region past its count"; }
        gen_rk = r; gen_nrk = nr;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (bad) {
            CHECK(!t && strstr(why, bad), "case %d: refused (%s): got \"%s\"", c, bad, why);
            free(t);
            if (c == 2) {                                       // and from g76's side, f76 its callee
                why[0] = 0;
                t = gen_with(other, NULL, 0, JGEN_ALL, why);
                CHECK(!t && strstr(why, bad), "case 2: refused from the plain side too: got \"%s\"", why);
                free(t);
            }
            gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
            continue;
        }
        bool ok = t && !strstr(t, " rec.fall ") && !strstr(t, " rec.bound ") && !strstr(t, " rec.le ");   // not rec.lex
        int nq = c == 0 ? 2 : 3;
        for (int w = 0; ok && w < ncall; w++)
            for (int q = 0; ok && q < nq; q++) {
                char key[48]; snprintf(key, sizeof key, "bb%d.%d rec.lex %d", cb[w]->id, ci[w], q);
                const char *v = vc_of(t, key);
                char hx[48]; snprintf(hx, sizeof hx, "2*%%v%d + -1*rank.max.0.pre <= -8589934592\n", x->id);
                char hy[64]; snprintf(hy, sizeof hy, "post : 2*%%v%d + -1*rank.max.0.post <= -8589934592\n", y->id);   // x's row at the arguments: y
                if (c == 0)
                    ok = *v && count(v, "def rank.max 0 pre : ") == 3 && count(v, "def rank.max 0 post : ") == (q ? 0 : 3) && !strstr(v, "premise ") &&
                         strstr(v, hx) && (q || strstr(v, hy)) && strstr(v, q ? "goal -1*rank.max.0.pre <= 0\n" : "goal -1*rank.max.0.pre + 1*rank.max.0.post <= -1\n");
                else ok = *v && count(v, "premise rank.lex 0 : or { ") == 1 && count(v, "premise rank.") == 1 && !strstr(v, "def rank.");
                if (!ok) CHECK(false, "case %d: call %d's rec.lex %d:\n%s", c, w, q, v);
            }
        char k3[48]; snprintf(k3, sizeof k3, "rec.lex %d\n", nq);
        CHECK(ok && !strstr(t, k3) && count(t, " rec.lex ") == ncall * nq, "case %d: rec.lex 0 .. %d at each of %d calls, nothing else of a recursion:\n%s", c, nq - 1, ncall, t ? t : why);
        free(t);
        if (c == 0) {                                           // m's recipe: the two rows at the caller's entry
            char *rc = gen_recipes(f, why), want[160];
            snprintf(want, sizeof want, "rank.max.0.pre mm 1: {8589934592 2*%%v%d} {8589934593 2*%%v%d}\n", x->id, y->id);
            CHECK(rc && count(rc, want) == 4, "case 0: the recipe of m before, at each call's two VCs:\n%s", rc ? rc : why);
            free(rc);
        }
        gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a recursion's measure of several rows: every call free, its lexicographic decrease owed\n");
    // 77. a RECURSION's measure in REGIONS (c43, c38's at a call): TwoWay, all i32, `tw(t, n)`: n < 0
    // returns; m = n - 1 where t != 0, else n + 1; calls tw(t, m). Regions over the fixed t: t - 1
    // >= 0 with <n>, -t - 1 >= 0 with <n>, then t >= 0 and -t >= 0 with <2^31 - 1 - n>. (0) at the
    // call: rec.cover 0 and 1 (the last region's atoms, under `premise rank.cover 0` and 1),
    // rec.region 1, 2 and 102, rec.lex 0, 1, 1000, 1001, 2000, 2001, nothing else of a recursion;
    // rec.cover 0's goal t >= 0 at entry, rec.region 2's goal -t' >= 0 failing at the argument t
    // (the parameter itself), rec.lex 2000 under G_2 before and after (two atoms each). (1) refused:
    // the regions in tw and a g calling each other (v1). (2) RecursiveMultiplication, `mult(n, m)`:
    // m < 0 calls mult(n, -m), m == 0 returns, else mult(n, m - 1); regions m >= 0 with <m>, -m - 1
    // >= 0 with <-m>: at the second call rec.cover 0's goal m <= -1 over the PARAMETER m, and
    // rec.region 1's goal m' >= 0 over the ARGUMENT m - 1
    for (int c = 0; c < 3; c++) {
        if (c == 2) {
            IrFunc *f = ir_func_new(&A, nm("mult77"), i32t, IR_FUNC_PURE);
            IrValue *np = ir_add_param(f, i32t, nm("n")), *mp = ir_add_param(f, i32t, nm("m"));
            IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f);
            ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SLT, mp, ir_const_int(f, b0, 0, i32t)), b1, b2);
            IrInstr *k1 = ir_instr(f, IR_CALL, i32t, 2); k1->aux.callee = nm("mult77");
            k1->operands[0] = np; k1->operands[1] = ir_binop(f, b1, IR_SUB, ir_const_int(f, b1, 0, i32t), mp, i32t); ir_emit(b1, k1);
            ir_set_ret(b1, k1->result);
            ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_EQ, mp, ir_const_int(f, b2, 0, i32t)), b3, b4);
            ir_set_ret(b3, ir_const_int(f, b3, 0, i32t));
            IrValue *am = ir_binop(f, b4, IR_SUB, mp, ir_const_int(f, b4, 1, i32t), i32t);
            int ci = nins(b4);
            IrInstr *k2 = ir_instr(f, IR_CALL, i32t, 2); k2->aux.callee = nm("mult77");
            k2->operands[0] = np; k2->operands[1] = am; ir_emit(b4, k2);
            ir_set_ret(b4, k2->result);
            ir_finalize_cfg(f);
            gen_mod = f;
            JRank r; memset(&r, 0, sizeof r);
            r.blk = JRANK_ENTRY; r.fn = f; r.nt = 1; r.key[0] = mp->id; r.ncomp = 4; r.nreg = 2;
            r.coef[0][0] = 1; r.coef[1][0] = 1; r.coef[2][0] = -1; r.c0[2] = -1; r.coef[3][0] = -1;
            r.reg[0].g0 = 0; r.reg[0].ng = 1; r.reg[0].t0 = 1; r.reg[0].nt = 1;
            r.reg[1].g0 = 2; r.reg[1].ng = 1; r.reg[1].t0 = 3; r.reg[1].nt = 1;
            gen_rk = &r; gen_nrk = 1;
            char why[200] = "", key[48], g1[48], g2[48];
            char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
            snprintf(key, sizeof key, "bb%d.%d rec.cover 0", b4->id, ci);
            const char *v = vc_of(t ? t : "", key);
            snprintf(g1, sizeof g1, "goal 1*%%v%d <= -1\n", mp->id);
            CHECK(t && strstr(v, g1) && count(v, "premise rank.cover 0 : ") == 1, "case 2: rec.cover 0, m <= -1 over the parameter m:\n%s", *v ? v : (t ? t : why));
            snprintf(key, sizeof key, "bb%d.%d rec.region 1", b4->id, ci);
            v = vc_of(t ? t : "", key);
            snprintf(g2, sizeof g2, "goal -1*%%v%d <= 0\n", am->id);
            CHECK(t && strstr(v, g2) && count(v, "premise rank.region 0 pre : ") == 1, "case 2: rec.region 1, m - 1 >= 0 over the argument:\n%s", *v ? v : (t ? t : why));
            free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
            continue;
        }
        IrFunc *f = ir_func_new(&A, nm("tw77"), i32t, IR_FUNC_PURE);
        IrValue *tp = ir_add_param(f, i32t, nm("t")), *np = ir_add_param(f, i32t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f), *b5 = ir_new_block(f);
        IrValue *mc = ir_alloca(f, b0, i32t);
        ir_set_br_cond(b0, ir_icmp(f, b0, IR_CMP_SLT, np, ir_const_int(f, b0, 0, i32t)), b1, b2);
        ir_set_ret(b1, ir_const_int(f, b1, 1, i32t));
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_NE, tp, ir_const_int(f, b2, 0, i32t)), b3, b4);
        ir_store(f, b3, mc, ir_binop(f, b3, IR_SUB, np, ir_const_int(f, b3, 1, i32t), i32t)); ir_set_br(b3, b5);
        ir_store(f, b4, mc, ir_binop(f, b4, IR_ADD, np, ir_const_int(f, b4, 1, i32t), i32t)); ir_set_br(b4, b5);
        IrValue *lm = ir_load(f, b5, mc, i32t);
        int ci = nins(b5);
        IrInstr *k_ = ir_instr(f, IR_CALL, i32t, 2); k_->aux.callee = nm(c == 1 ? "g77" : "tw77");
        k_->operands[0] = tp; k_->operands[1] = lm; ir_emit(b5, k_);
        ir_set_ret(b5, k_->result);
        ir_finalize_cfg(f);
        IrFunc *other = NULL;
        if (c == 1) {                                           // g77(t, n) calls tw77(t, n - 1)
            other = ir_func_new(&A, nm("g77"), i32t, IR_FUNC_PURE);
            IrValue *gt = ir_add_param(other, i32t, nm("t")), *gn = ir_add_param(other, i32t, nm("n"));
            IrBlock *g0 = other->entry, *g1 = ir_new_block(other), *g2 = ir_new_block(other);
            ir_set_br_cond(g0, ir_icmp(other, g0, IR_CMP_SLT, gn, ir_const_int(other, g0, 0, i32t)), g1, g2);
            ir_set_ret(g1, ir_const_int(other, g1, 1, i32t));
            IrInstr *gk = ir_instr(other, IR_CALL, i32t, 2); gk->aux.callee = nm("tw77");
            gk->operands[0] = gt; gk->operands[1] = ir_binop(other, g2, IR_SUB, gn, ir_const_int(other, g2, 1, i32t), i32t); ir_emit(g2, gk);
            ir_set_ret(g2, gk->result);
            ir_finalize_cfg(other);
            f->next = other;
        }
        gen_mod = f;
        JRank r; memset(&r, 0, sizeof r);
        r.blk = JRANK_ENTRY; r.fn = f; r.nt = 2; r.key[0] = tp->id; r.key[1] = np->id; r.ncomp = 7; r.nreg = 3;
        r.coef[0][0] = 1; r.c0[0] = -1; r.coef[1][1] = 1;                        // t - 1 >= 0: <n>
        r.coef[2][0] = -1; r.c0[2] = -1; r.coef[3][1] = 1;                       // -t - 1 >= 0: <n>
        r.coef[4][0] = 1; r.coef[5][0] = -1; r.coef[6][1] = -1; r.c0[6] = 2147483647;   // t = 0: <2^31 - 1 - n>
        r.reg[0].g0 = 0; r.reg[0].ng = 1; r.reg[0].t0 = 1; r.reg[0].nt = 1;
        r.reg[1].g0 = 2; r.reg[1].ng = 1; r.reg[1].t0 = 3; r.reg[1].nt = 1;
        r.reg[2].g0 = 4; r.reg[2].ng = 2; r.reg[2].t0 = 6; r.reg[2].nt = 1;
        gen_rk = &r; gen_nrk = 1;
        char why[200] = "";
        char *t = gen_with(f, NULL, 0, JGEN_ALL, why);
        if (c == 1) {
            CHECK(!t && strstr(why, "a recursion's measure of several rows in a mutual recursion (v1)"), "case 1: regions in a mutual recursion refused: got \"%s\"", why);
            free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
            continue;
        }
        const char *ids[11] = { "rec.cover 0", "rec.cover 1", "rec.region 1", "rec.region 2", "rec.region 102", "rec.lex 0", "rec.lex 1",
                                "rec.lex 1000", "rec.lex 1001", "rec.lex 2000", "rec.lex 2001" };
        bool ok = t && !strstr(t, " rec.fall ") && !strstr(t, " rec.bound ") && !strstr(t, " rec.le ") && count(t, " rec.") == 11;
        for (int q = 0; ok && q < 11; q++) { char key[48]; snprintf(key, sizeof key, "bb%d.%d %s\n", b5->id, ci, ids[q]); ok = count(t, key) == 1; }
        CHECK(ok, "case 0: the eleven VCs at the call, each once, nothing else of a recursion:\n%s", t ? t : why);
        char key[48], g1[48], g2[48];
        snprintf(key, sizeof key, "bb%d.%d rec.cover 0", b5->id, ci);
        const char *v = vc_of(t ? t : "", key);
        snprintf(g1, sizeof g1, "goal -1*%%v%d <= 0\n", tp->id);
        CHECK(count(v, "premise rank.cover 0 : ") == 1 && count(v, "premise rank.cover 1 : ") == 1 && strstr(v, g1), "case 0: rec.cover 0, t >= 0 where no region before holds:\n%s", v);
        snprintf(key, sizeof key, "bb%d.%d rec.region 2", b5->id, ci);
        v = vc_of(t ? t : "", key);
        snprintf(g2, sizeof g2, "goal -1*%%v%d <= -1\n", tp->id);
        CHECK(count(v, "premise rank.region 0 pre : ") == 1 && count(v, "premise rank.region 2 post : ") == 1 && strstr(v, g2),
              "case 0: rec.region 2, from t >= 1 the argument t not in t = 0 (its last atom fails):\n%s", v);
        snprintf(key, sizeof key, "bb%d.%d rec.lex 2000", b5->id, ci);
        v = vc_of(t ? t : "", key);
        CHECK(count(v, "premise rank.region 2 pre : ") == 2 && count(v, "premise rank.region 2 post : ") == 2 && !strstr(v, "premise rank.lex"),
              "case 0: rec.lex 2000 under t = 0 before and after:\n%s", v);
        // region 0's tuple is <n> alone: rec.lex 0 its fall (n before, less 1), rec.lex 1 its bound
        snprintf(key, sizeof key, "bb%d.%d rec.lex 0", b5->id, ci);
        v = vc_of(t ? t : "", key);
        const char *gl = strstr(v, "goal "), *ge = gl ? strchr(gl, '\n') : NULL;
        char nn[24]; snprintf(nn, sizeof nn, "-1*%%v%d ", np->id);
        CHECK(gl && ge && strstr(gl, nn) && strstr(gl, nn) < ge && !strncmp(ge - 5, "<= -1", 5) && count(v, "premise rank.region 0 pre : ") == 1,
              "case 0: rec.lex 0, n falls in t >= 1:\n%s", v);
        snprintf(key, sizeof key, "bb%d.%d rec.lex 1", b5->id, ci);
        v = vc_of(t ? t : "", key);
        snprintf(g2, sizeof g2, "goal -1*%%v%d <= 0\n", np->id);
        CHECK(strstr(v, g2) != NULL, "case 0: rec.lex 1, n >= 0 in t >= 1:\n%s", v);
        free(t); gen_rk = NULL; gen_nrk = 0; gen_mod = NULL;
    }
    printf("  a recursion's measure in regions: c38's VCs at each call, from the parameters to the arguments\n");
    // 78. jgen_paths offers no pair for a DEAD header version (c45). One loop, all u32, `while r < n`:
    // r read at the header (live); t stored then read each trip (dead: no pair, no ENTRY pair); j
    // stored on one arm and read after the join (its header version reaches the join from the other
    // arm: live); k stored then read each trip, dead unless (1) a header claim names it beside r
    // (k - r <= 0): then kept. (0) no claims: r and j with their ENTRY pairs, nothing of t or k. (2)
    // a fifth cell h, stored then read each trip, but handed by a mutable pointer to a call in the
    // loop (its HAVOC version: the callee may read it): kept
    for (int c = 0; c < 3; c++) {
        IrFunc *f = ir_func_new(&A, nm("dead78"), u32t, IR_FUNC_PURE);
        IrValue *n = ir_add_param(f, u32t, nm("n"));
        IrBlock *b0 = f->entry, *b1 = ir_new_block(f), *b2 = ir_new_block(f), *b3 = ir_new_block(f), *b4 = ir_new_block(f),
                *b5 = ir_new_block(f), *b6 = ir_new_block(f);
        IrValue *r = ir_alloca(f, b0, u32t), *t = ir_alloca(f, b0, u32t), *j = ir_alloca(f, b0, u32t), *k = ir_alloca(f, b0, u32t);
        IrValue *z0 = ir_const_int(f, b0, 0, u32t);
        ir_store(f, b0, r, z0); ir_store(f, b0, t, z0); ir_store(f, b0, j, z0); ir_store(f, b0, k, z0); ir_set_br(b0, b1);
        IrValue *lr = ir_load(f, b1, r, u32t);
        ir_set_br_cond(b1, ir_icmp(f, b1, IR_CMP_ULT, lr, n), b2, b6);
        ir_store(f, b2, t, ir_binop(f, b2, IR_ADD, lr, ir_const_int(f, b2, 1, u32t), u32t));
        IrValue *lt = ir_load(f, b2, t, u32t);
        ir_store(f, b2, k, lt);
        IrValue *lk = ir_load(f, b2, k, u32t);
        IrValue *h = NULL; IrFunc *wrh = NULL;
        if (c == 2) {                                          // h: stored, read, then handed to wrh
            IrType *pmu = ir_type_new(&A, IRT_PTR); pmu->elem = u32t; pmu->ptr_mut = true;
            wrh = ir_func_new(&A, nm("wrh78"), u32t, IR_FUNC_PURE);
            ir_add_param(wrh, pmu, nm("p")); ir_set_ret(wrh->entry, ir_const_int(wrh, wrh->entry, 0, u32t));
            ir_finalize_cfg(wrh);
            h = ir_alloca(f, b0, u32t);
            ir_store(f, b2, h, lt);
            (void)ir_load(f, b2, h, u32t);
            mk_call(f, b2, "wrh78", h, u32t);
        }
        ir_set_br_cond(b2, ir_icmp(f, b2, IR_CMP_UGT, lk, ir_const_int(f, b2, 5, u32t)), b3, b4);
        ir_store(f, b3, j, lt); ir_set_br(b3, b5);
        ir_set_br(b4, b5);
        IrValue *lj = ir_load(f, b5, j, u32t);
        ir_store(f, b5, r, ir_binop(f, b5, IR_ADD, lr, ir_binop(f, b5, IR_UDIV, lj, ir_const_int(f, b5, 1000, u32t), u32t), u32t));
        ir_store(f, b5, r, ir_binop(f, b5, IR_ADD, ir_load(f, b5, r, u32t), ir_const_int(f, b5, 1, u32t), u32t));
        ir_set_br(b5, b1);
        ir_set_ret(b6, ir_const_int(f, b6, 0, u32t));
        ir_finalize_cfg(f);
        gen_mod = f;
        if (wrh) { wrh->next = f; gen_mod = wrh; }
        JClaim cl[1]; memset(cl, 0, sizeof cl);
        cl[0].blk = b1->id; cl[0].fn = f; cl[0].nt = 2; cl[0].key[0] = k->id; cl[0].coef[0] = 1; cl[0].key[1] = r->id; cl[0].coef[1] = -1;
        JMem m = jmem_over(region, sizeof region);
        JPaths P; char err[200];
        bool ok = jgen_paths(&m, f, gen_mod, cl, c == 1, b1->id, &P, err, sizeof err);
        CHECK(ok && !P.refused && P.n == 2, "case %d: two edges into the header (got %d, %s)", c, ok ? P.n : -1, ok ? (P.refused ? P.refused : "") : err);
        for (int32_t q = 0; ok && q < P.n; q++) {
            const JPath *p = &P.p[q];
            int has[5][2] = { { 0 } };                         // r, t, j, k, h: VALUE, ENTRY
            IrValue *cells[5] = { r, t, j, k, h };
            for (int32_t z = 0; z < p->npairs; z++)
                for (int w = 0; w < 5; w++)
                    if (cells[w] && p->pairs[z].key == cells[w]->id) has[w][p->pairs[z].tk == JTERM_ENTRY]++;
            bool want_k = c == 1, want_h = c == 2;
            CHECK(has[0][0] == 1 && has[0][1] == 1 && has[2][0] == 1 && has[2][1] == 1 && has[1][0] == 0 && has[1][1] == 0 &&
                  has[3][0] == want_k && has[3][1] == want_k && has[4][0] == want_h && p->npairs == 4 + 2 * want_k + (has[4][0] + has[4][1]),
                  "case %d, path %d: r and j live, t dead, k %s, h %s (r %d/%d, t %d/%d, j %d/%d, k %d/%d, h %d/%d of %d pairs)", c, q,
                  want_k ? "kept by the claim" : "dead", want_h ? "kept by the call" : "absent", has[0][0], has[0][1], has[1][0], has[1][1],
                  has[2][0], has[2][1], has[3][0], has[3][1], has[4][0], has[4][1], p->npairs);
        }
        gen_mod = NULL;
    }
    printf("  the paths offer no pair for a header version no trip uses\n");
    printf("%s\n", failures ? "test_judge_gen: FAILED" : "test_judge_gen: all passed");
    return failures ? 1 : 0;
}
