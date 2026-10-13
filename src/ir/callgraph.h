// callgraph.h — THE MODULE'S CALL GRAPH AND ITS RECURSION COMPONENTS, defined once.
//
// Two clients ask "which calls close a cycle?": VRA's termination rules (analysis/vra.h: the
// self-call rule, and the cycle rule of I.130) and the judge's recursion obligations (judge/gen.h).
// A function the two place on different cycles is one the shadow comparison and the authority
// switch would disagree about for no reason the proof can see, so the set is decided HERE and both
// read it. It is plain IR (no analysis), which is what lets the trusted judge include it.
//
// ★ AN INDIRECT CALL IS AN EDGE TOO (I.136). The graph was the DIRECT calls only, so a cycle that
// closes through a function pointer drew no obligation at all: `func spin(x u8) u8 { return
// apply(spin, x) }` with `apply` calling `f(x)` was accepted and ran out of stack. An indirect call
// can reach any function whose address the program takes (an IR_FUNC_REF: lowering makes a
// function value no other way) and whose parameter count matches the call's arguments, so it is an
// edge to each of them. That is the PARAMETER COUNT, not the type: a superset of the functions the
// pointer's type admits, which is the sound direction.
// An extern that is handed a function and calls it back (qsort's comparator) is outside this graph:
// an extern is a trusted boundary, and its row is believed, here as everywhere else.
//
// A direct call is an edge to the callee NAMED (a duplicate name resolves to the first function of
// the module with it). A call to an extern, or to a name the module does not define, is no edge.
//
// The components are Tarjan's algorithm, run iteratively: a deep call chain must not overflow the
// compiler's own stack. A client builds the graph ONCE per module and asks it per function: rebuilding
// it per function cost a 1000-function module ten times its compile time (0.95 s to 9.9 s, measured
// 2026-10-03). `ir_cg_build` keeps what it built when asked again for the same module with the same
// functions and a signature of their sizes, so a module lowered again, or changed, is rebuilt.
//
// The edges of function k are off[k] .. off[k+1], in instruction order; the edges of one call are
// CONTIGUOUS there, its targets in module order, so a client walks a function's calls in one pass.
// The queries are `static inline`: a translation unit that uses none of them builds without a warning.
#ifndef LAIN_IR_CALLGRAPH_H
#define LAIN_IR_CALLGRAPH_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ir.h"

typedef struct {
    IrFunc *mod; int n; unsigned long sig;
    IrFunc **idx;                  // the functions in module order
    int *hash, hcap;               // name -> index, open addressing
    int *off, *to; IrInstr **ecall;   // the calls of function k are edges off[k] .. off[k+1]
    int *comp;                     // the component of each function
    bool *at;                      // its address is taken (I.136)
} IrCallGraph;

static void ir_cg_free(IrCallGraph *G) {
    free(G->idx); free(G->hash); free(G->off); free(G->to); free(G->ecall);
    free(G->comp); free(G->at);
    memset(G, 0, sizeof *G);
}
static uint32_t ir_cg_name_hash(const IrName *nm) {
    uint32_t h = 2166136261u;
    for (isize i = 0; i < nm->length; i++) { h ^= (uint8_t)nm->name[i]; h *= 16777619u; }
    return h;
}
// The function of the module named `nm`, or -1.
static int ir_cg_lookup(const IrCallGraph *G, const IrName *nm) {
    if (!nm || !G->hash) return -1;
    for (uint32_t h = ir_cg_name_hash(nm) & (uint32_t)(G->hcap - 1);; h = (h + 1) & (uint32_t)(G->hcap - 1)) {
        int k = G->hash[h];
        if (k < 0) return -1;
        IrName *g = G->idx[k]->name;
        if (g && g->length == nm->length && !memcmp(g->name, nm->name, (size_t)nm->length)) return k;
    }
}
static int ir_cg_param_count(const IrFunc *g) { int n = 0; for (IrParam *p = g ? g->params : NULL; p; p = p->next) n++; return n; }

// Build G for `mod` (or keep it, when it is already that module's graph). False when out of memory,
// with G empty: a client fails closed.
static bool ir_cg_build(IrCallGraph *G, IrFunc *mod) {
    // The signature costs one step per FUNCTION, not per instruction: the analysis runs many times
    // per function (effects, nested call-site analyses), and a walk of every instruction at each
    // run cost a 1000-function module twice its compile time. A function's value and block counters
    // grow with every instruction and block added to it, and the functions are compared by identity.
    int n = 0; unsigned long sig = 0; bool same = G->mod == mod && G->comp;   // a hash: unsigned, it wraps by definition (I.170)
    for (IrFunc *g = mod; g; g = g->next) {
        if (same && (n >= G->n || G->idx[n] != g)) same = false;
        n++; sig = sig * 31u + (unsigned long)g->next_value_id * 7u + (unsigned long)g->next_block_id;
    }
    if (same && G->n == n && G->sig == sig) return true;
    ir_cg_free(G);
    if (!mod || n == 0) return false;
    G->mod = mod; G->n = n; G->sig = sig;
    G->hcap = 1; while (G->hcap < 2 * n) G->hcap <<= 1;
    G->idx = calloc((size_t)n, sizeof *G->idx); G->hash = malloc((size_t)G->hcap * sizeof *G->hash);
    G->off = calloc((size_t)n + 1, sizeof *G->off); G->comp = calloc((size_t)n, sizeof *G->comp);
    G->at = calloc((size_t)n, sizeof *G->at);
    int *np = calloc((size_t)n, sizeof *np);
    if (!G->idx || !G->hash || !G->off || !G->comp || !G->at || !np) { free(np); ir_cg_free(G); return false; }
    for (int h = 0; h < G->hcap; h++) G->hash[h] = -1;
    { int k = 0;
      for (IrFunc *g = mod; g; g = g->next, k++) {
          G->idx[k] = g; np[k] = ir_cg_param_count(g);
          if (!g->name || ir_cg_lookup(G, g->name) >= 0) continue;   // a duplicate name resolves to the first
          uint32_t h = ir_cg_name_hash(g->name) & (uint32_t)(G->hcap - 1);
          while (G->hash[h] >= 0) h = (h + 1) & (uint32_t)(G->hcap - 1);
          G->hash[h] = k;
      } }
    // the functions whose address is taken: the targets an indirect call may reach (I.136)
    int nat = 0;
    for (int k = 0; k < n; k++)
        for (IrBlock *b = G->idx[k]->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next)
                if (i->op == IR_FUNC_REF && i->aux.callee) { int c = ir_cg_lookup(G, i->aux.callee); if (c >= 0 && !G->at[c]) { G->at[c] = true; nat++; } }
    long E = 0;
    for (int k = 0; k < n; k++)
        for (IrBlock *b = G->idx[k]->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next)
                if (i->op == IR_CALL) E += i->aux.callee ? 1 : nat;
    G->to = calloc((size_t)E + 1, sizeof *G->to); G->ecall = calloc((size_t)E + 1, sizeof *G->ecall);
    if (!G->to || !G->ecall) { free(np); ir_cg_free(G); return false; }
    { int e = 0;
      for (int k = 0; k < n; k++) {
          G->off[k] = e;
          for (IrBlock *b = G->idx[k]->blocks; b; b = b->next)
              for (IrInstr *i = b->instrs; i; i = i->next) {
                  if (i->op != IR_CALL) continue;
                  if (i->aux.callee) {
                      int c = ir_cg_lookup(G, i->aux.callee);
                      if (c >= 0) { G->to[e] = c; G->ecall[e] = i; e++; }
                  } else
                      for (int c = 0; c < n; c++)
                          if (G->at[c] && np[c] == i->n_operands - 1) { G->to[e] = c; G->ecall[e] = i; e++; }
              }
      }
      G->off[n] = e; }
    free(np);
    // Tarjan, iteratively: index/low per function, an explicit DFS stack of (function, next edge)
    int *ix = malloc((size_t)n * sizeof *ix), *low = malloc((size_t)n * sizeof *low);
    int *st = malloc((size_t)n * sizeof *st), *cs = malloc((size_t)n * sizeof *cs), *ce = malloc((size_t)n * sizeof *ce);
    bool *on = calloc((size_t)n, sizeof *on);
    if (!ix || !low || !st || !cs || !ce || !on) { free(ix); free(low); free(st); free(cs); free(ce); free(on); ir_cg_free(G); return false; }
    for (int k = 0; k < n; k++) ix[k] = -1;
    int counter = 0, sp = 0, ncomp = 0;
    for (int r = 0; r < n; r++) {
        if (ix[r] >= 0) continue;
        int top = 0; cs[top] = r; ce[top] = G->off[r]; top++;
        ix[r] = low[r] = counter++; st[sp++] = r; on[r] = true;
        while (top > 0) {
            int v = cs[top-1];
            if (ce[top-1] < G->off[v+1]) {
                int w = G->to[ce[top-1]++];
                if (ix[w] < 0) {
                    ix[w] = low[w] = counter++; st[sp++] = w; on[w] = true;
                    cs[top] = w; ce[top] = G->off[w]; top++;
                } else if (on[w] && ix[w] < low[v]) low[v] = ix[w];
                continue;
            }
            if (low[v] == ix[v]) {                     // v roots a component
                int w;
                do { w = st[--sp]; on[w] = false; G->comp[w] = ncomp; } while (w != v);
                ncomp++;
            }
            top--;
            if (top > 0) { int u = cs[top-1]; if (low[v] < low[u]) low[u] = low[v]; }
        }
    }
    free(ix); free(low); free(st); free(cs); free(ce); free(on);
    return true;
}

// f's index in G, by identity (not by name), or -1.
static inline int ir_cg_index(const IrCallGraph *G, const IrFunc *f) {
    for (int k = 0; f && k < G->n; k++) if (G->idx[k] == f) return k;
    return -1;
}
// The component of function k: its size (k included), written in module order to `mem` (at most
// `cap`; the size is returned even when larger, and the caller fails closed).
static inline int ir_cg_members(const IrCallGraph *G, int k, IrFunc **mem, int cap) {
    if (k < 0 || k >= G->n) return 0;
    int count = 0;
    for (int j = 0; j < G->n; j++)
        if (G->comp[j] == G->comp[k]) { if (count < cap && mem) mem[count] = G->idx[j]; count++; }
    return count;
}
// Is function k on a recursion cycle: a component of more than one function, or an edge to itself
// (a direct self-call, or an indirect call that may reach k)?
static inline bool ir_cg_on_cycle(const IrCallGraph *G, int k) {
    if (k < 0 || k >= G->n) return false;
    for (int j = 0; j < G->n; j++) if (j != k && G->comp[j] == G->comp[k]) return true;
    for (int e = G->off[k]; e < G->off[k+1]; e++) if (G->to[e] == k) return true;
    return false;
}
// The functions `call` (an instruction of function k) may reach, as indices in module order, at
// most `cap` written to `out`; the count is returned even when larger. With `in_comp`, only those
// in k's component: the calls that can close a cycle through k.
static inline int ir_cg_call_targets(const IrCallGraph *G, int k, const IrInstr *call, bool in_comp, int *out, int cap) {
    if (k < 0 || k >= G->n || !call) return 0;
    int count = 0;
    for (int e = G->off[k]; e < G->off[k+1]; e++) {
        if (G->ecall[e] != call) continue;
        if (in_comp && G->comp[G->to[e]] != G->comp[k]) continue;
        if (count < cap && out) out[count] = G->to[e];
        count++;
    }
    return count;
}

#endif
