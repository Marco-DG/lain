// test_callgraph.c — the module call graph that VRA's termination rules and the judge's recursion
// obligations share (src/ir/callgraph.h), on a module built by hand.
//
//   gcc -std=c99 -o /tmp/test_callgraph src/tools/test_callgraph.c -I src && /tmp/test_callgraph
//
// The module, one parameter each unless stated:
//   a -> b, b -> a              a component of two
//   c -> c                      a self-call
//   d -> a                      reaches a cycle, is on none
//   e: takes &g and &h, calls a 1-argument pointer       e -> g (h takes two arguments)
//   g -> e                      so {e, g} is a cycle that closes through the pointer (I.136)
//   h(x, y)                     address taken, no calls
//   s(x, y): takes &s, calls a 2-argument pointer        s -> h, s -> s (a self-cycle via pointer)
//   k: calls a 1-argument pointer, takes no address      k -> g, on no cycle
//   dup, dup: two functions with one name; t -> dup resolves to the first
// Exit status 0 when every check holds.
#include "utils/common/def.h"
#include "utils/arena.h"
#include "utils/common/system/memory.h"
#include "ir/ir.h"
#include "ir/build.h"
#include "ir/callgraph.h"
#include <stdio.h>
#include <string.h>

static Arena A;
static IrType *I32, *FN;
static int fails = 0, checks = 0;
static void check(bool ok, const char *what) { checks++; if (!ok) { fails++; printf("FAIL: %s\n", what); } }
static IrName *nm(const char *s) { return ir_intern(&A, s, (isize)strlen(s)); }
static IrFunc *fn(const char *name, int np) {
    IrFunc *f = ir_func_new(&A, nm(name), I32, IR_FUNC_PURE);
    for (int i = 0; i < np; i++) ir_add_param(f, I32, nm(i ? "y" : "x"));
    return f;
}
static IrValue *param(IrFunc *f, int i) { IrParam *p = f->params; while (i-- > 0) p = p->next; return p->value; }
// a direct call of `callee` with f's first `n` parameters as its arguments
static IrInstr *call(IrFunc *f, const char *callee, int n) {
    IrInstr *ins = ir_instr(f, IR_CALL, I32, n);
    for (int i = 0; i < n; i++) ins->operands[i] = param(f, i);
    ins->aux.callee = nm(callee);
    ir_emit(f->entry, ins);
    return ins;
}
// an indirect call through `ptr` with `n` arguments: operand 0 is the pointer
static IrInstr *icall(IrFunc *f, IrValue *ptr, int n) {
    IrInstr *ins = ir_instr(f, IR_CALL, I32, n + 1);
    ins->operands[0] = ptr;
    for (int i = 0; i < n; i++) ins->operands[i + 1] = param(f, i);
    ir_emit(f->entry, ins);
    return ins;
}

int main(void) {
    A = arena_new(memory_alloc, MEMORY_PAGE_MINIMUM_SIZE * 64);
    I32 = ir_type_int(&A, 32, true);
    FN = ir_type_new(&A, IRT_FUNC); FN->elem = I32;

    IrFunc *a = fn("a", 1), *b = fn("b", 1), *c = fn("c", 1), *d = fn("d", 1), *e = fn("e", 1),
           *g = fn("g", 1), *h = fn("h", 2), *s = fn("s", 2), *k = fn("k", 1),
           *dup1 = fn("dup", 1), *dup2 = fn("dup", 1), *t = fn("t", 1);
    IrFunc *order[] = { a, b, c, d, e, g, h, s, k, dup1, dup2, t };
    int n = (int)(sizeof order / sizeof *order);
    for (int i = 0; i + 1 < n; i++) order[i]->next = order[i + 1];

    call(a, "b", 1); call(b, "a", 1); call(c, "c", 1);
    IrInstr *d_a = call(d, "a", 1);
    ir_func_ref(e, e->entry, nm("g"), FN);
    IrValue *hp = ir_func_ref(e, e->entry, nm("h"), FN);
    IrInstr *e_ptr = icall(e, hp, 1);              // the pointer is h's, but only arity decides
    call(g, "e", 1);
    IrValue *sp = ir_func_ref(s, s->entry, nm("s"), FN);
    IrInstr *s_ptr = icall(s, sp, 2);
    IrInstr *k_ptr = icall(k, hp, 1);
    call(dup2, "dup", 1);                          // dup2 -> dup resolves to dup1, so dup2 is on no cycle
    IrInstr *t_dup = call(t, "dup", 1);
    call(t, "extern_or_missing", 1);               // a name the module does not define: no edge

    IrCallGraph G; memset(&G, 0, sizeof G);
    check(ir_cg_build(&G, a), "build");
    #define IX(f) ir_cg_index(&G, f)
    check(G.n == n, "every function is a node");
    for (int i = 0; i < n; i++) check(IX(order[i]) == i, "indices in module order, by identity");

    IrFunc *mem[8]; int out[8];
    check(ir_cg_members(&G, IX(a), mem, 8) == 2 && mem[0] == a && mem[1] == b, "{a, b} in module order");
    check(ir_cg_on_cycle(&G, IX(a)) && ir_cg_on_cycle(&G, IX(b)), "a and b on a cycle");
    check(ir_cg_members(&G, IX(c), mem, 8) == 1 && ir_cg_on_cycle(&G, IX(c)), "c alone, on a cycle by its self-call");
    check(ir_cg_members(&G, IX(d), mem, 8) == 1 && !ir_cg_on_cycle(&G, IX(d)), "d on no cycle");
    check(ir_cg_call_targets(&G, IX(d), d_a, false, out, 8) == 1 && out[0] == IX(a), "d's call reaches a");
    check(ir_cg_call_targets(&G, IX(d), d_a, true, out, 8) == 0, "d's call closes no cycle");

    check(ir_cg_members(&G, IX(e), mem, 8) == 2 && mem[0] == e && mem[1] == g, "{e, g} closes through the pointer");
    check(ir_cg_call_targets(&G, IX(e), e_ptr, false, out, 8) == 1 && out[0] == IX(g),
          "e's 1-argument pointer reaches g, not h (two parameters) nor s");
    check(ir_cg_call_targets(&G, IX(e), e_ptr, true, out, 8) == 1, "and that call is in e's component");

    check(ir_cg_members(&G, IX(s), mem, 8) == 1 && ir_cg_on_cycle(&G, IX(s)), "s alone, on a cycle through its own pointer");
    check(ir_cg_call_targets(&G, IX(s), s_ptr, false, out, 8) == 2 && out[0] == IX(h) && out[1] == IX(s),
          "s's 2-argument pointer reaches h and s, in module order");
    check(ir_cg_call_targets(&G, IX(s), s_ptr, true, out, 8) == 1 && out[0] == IX(s), "only s is in s's component");
    check(!ir_cg_on_cycle(&G, IX(h)), "h on no cycle");
    check(ir_cg_call_targets(&G, IX(k), k_ptr, false, out, 8) == 1 && out[0] == IX(g) && !ir_cg_on_cycle(&G, IX(k)),
          "k's pointer reaches g; k on no cycle (no one reaches k)");

    check(ir_cg_call_targets(&G, IX(t), t_dup, false, out, 8) == 1 && out[0] == IX(dup1), "a duplicate name resolves to the first");
    check(ir_cg_on_cycle(&G, IX(dup1)) == false && ir_cg_on_cycle(&G, IX(dup2)) == false, "dup2 -> dup is dup2 -> dup1");
    check(G.off[IX(t) + 1] - G.off[IX(t)] == 1, "a call to an undefined name is no edge");
    check(ir_cg_members(&G, IX(a), NULL, 0) == 2 && ir_cg_members(&G, IX(a), mem, 1) == 2, "the size is returned past cap");
    check(ir_cg_index(&G, NULL) == -1 && ir_cg_members(&G, -1, mem, 8) == 0, "absent functions");

    // the cache: the same module unchanged is kept; a module that grew is rebuilt
    int *off_before = G.off;
    check(ir_cg_build(&G, a) && G.off == off_before, "an unchanged module keeps its graph");
    call(d, "d", 1);                               // d now calls itself
    check(ir_cg_build(&G, a) && ir_cg_on_cycle(&G, IX(d)), "a changed module is rebuilt (d's new self-call)");

    ir_cg_free(&G);
    printf("test_callgraph: %d of %d checks hold\n", checks - fails, checks);
    return fails ? 1 : 0;
}
