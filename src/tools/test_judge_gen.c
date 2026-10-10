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
static char *gen_with(IrFunc *f, const JClaim *cl, int32_t ncl, int32_t only, char *why) {
    JMem m = jmem_over(region, sizeof region);
    JObl *o; int32_t n; char err[200];
    if (!jgen_func(&m, f, cl, ncl, only, &o, &n, err, sizeof err)) {
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
static int nins(IrBlock *b) { int n = 0; for (IrInstr *i = b->instrs; i; i = i->next) n++; return n; }
static int count(const char *all, const char *key) { int n = 0; for (const char *p = all; (p = strstr(p, key)); p++) n++; return n; }
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
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 256);
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
    // not when it is a variable that may be negative (the interpreter compares as uint64)
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
        if (k == 0) CHECK(!strstr(dz, "guard"), "x <u y with x an i64 that may be negative is not stated (it compares as uint64):\n%s", dz);
        else        CHECK(strstr(dz, "guard bb0->bb1"), "3 <u y with 3 a non-negative constant is stated:\n%s", dz);
        free(t);
    }
    printf("  an unsigned comparison: a signed variable is not stated, a non-negative constant is\n");
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
    // 8. a store inside `unsafe` that narrows wraps, so its version is not the value stored
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
        if (k == 0) CHECK(!strstr(dz, "store %s"), "an unchecked narrowing store states no equality:\n%s", dz);
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
        bool ok = jgen_slots(&m, f, &sl, &ns, err, sizeof err);
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
        bad[3].c = claim1(b2, s, 1, 9); bad[3].c.nt = 3; bad[3].why = "other than one or two terms";
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
        JClaim good[4];
        good[0] = claim1(b2, s, 1, 9);                                // at the join
        good[1] = claim1(b3, s, 1, 100);                              // at the loop header
        good[2] = claim1(b2, s, 1, 0); good[2].nt = 2; good[2].key[1] = y->id; good[2].coef[1] = -1;   // s - y, y a parameter
        good[3] = claim1(b3, s, 1, 0); good[3].nt = 2; good[3].key[1] = u->id; good[3].coef[1] = -1;  // s - u, u not written
        for (int k = 0; k < 4; k++) {
            char why[200] = "";
            char *t = gen_with(f, &good[k], 1, JGEN_ALL, why);
            CHECK(t != NULL, "claim %d is accepted: got \"%s\"", k, why);
            free(t);
        }
        // what a proposer may name at the header: s made there, u and y fixed there, ls not
        {   JMem m = jmem_over(region, sizeof region);
            int32_t keys[4] = { s->id, u->id, y->id, ls->id }; uint8_t kind[4]; char err[200];
            bool ok = jgen_term_kinds(&m, f, b3->id, keys, 4, kind, err, sizeof err);
            CHECK(ok && kind[0] == 2 && kind[1] == 1 && kind[2] == 1 && kind[3] == 0,
                  "term kinds at the header: s 2, u 1, y 1, ls 0 (got %d %d %d %d)", kind[0], kind[1], kind[2], kind[3]);
        }
        printf("  claims refused: at the entry, no fresh version, a term not fixed, three terms, one twice\n");
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
        jgen_module = known ? rd : NULL;
        char *t = gen_with(f, NULL, 0, JGEN_ALL, NULL);
        jgen_module = NULL;
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
        jgen_module = mn;
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
            jgen_module = n8;
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
            jgen_module = mn;
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
        jgen_module = NULL;
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
        jgen_module = dn;
        char *t = gen_with(dn, &c, 1, JGEN_POST, NULL);
        jgen_module = NULL;
        char key[32], want[64];
        snprintf(key, sizeof key, "bb%d.%d post", b2->id, nins(b2));
        const char *p2 = t ? vc_of(t, key) : "";
        snprintf(want, sizeof want, "contract down #0 : 1*%%v%d <= 7", r->id);
        CHECK(strstr(p2, want), "the self-call's result is assumed below 8 where it is returned:\n%s", p2);
        free(t);
    }
    printf("  a return claim: owed at every return about the entry values, a contract at a same-module call\n");
    printf("%s\n", failures ? "test_judge_gen: FAILED" : "test_judge_gen: all passed");
    return failures ? 1 : 0;
}
