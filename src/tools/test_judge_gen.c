// src/tools/test_judge_gen.c — unit test of the judge's VC generator (src/judge/gen.h) over a
// HAND-BUILT IR. The generator is trusted, and its main test runs every accepted program on the
// interpreter and evaluates each hypothesis at the values the program has (judge_vc_driver.c
// --check-hyps). That test cannot see a rule no program exercises: since I.182 lowering emits no
// unsigned comparison with a signed operand, and an ASSUME stated before the point it follows
// holds at that point in every accepted program. Each rule here is pinned by the IR that makes
// it matter, and every VC is checked to be in the canonical text form (it parses and prints back).
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

// Every VC of `f` as text, each followed by its own round trip's verdict; NULL when the
// generator refuses the function.
static char *gen_text(IrFunc *f) {
    ir_finalize_cfg(f);
    JMem m = jmem_over(region, sizeof region);
    JObl *o; int32_t n; char err[200];
    if (!jgen_func(&m, f, &o, &n, err, sizeof err)) { printf("  generator refused: %s\n", err); return NULL; }
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
    printf("%s\n", failures ? "test_judge_gen: FAILED" : "test_judge_gen: all passed");
    return failures ? 1 : 0;
}
