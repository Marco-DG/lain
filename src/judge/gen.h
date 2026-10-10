#ifndef LAIN_JUDGE_GEN_H
#define LAIN_JUDGE_GEN_H
// gen.h — the judge's VC generator, phase 1a (local/internal/design/judge_translation_table.md §8).
// TRUSTED: every hypothesis it writes must hold whenever the program reaches the obligation's
// instruction, on the interpreter's semantics (src/ir/interp.h). A missing hypothesis costs only
// precision; a wrong one is a false proof. So it states few facts, each one exact:
//   - an SSA value's definition, for the exact rows: CONST (read by its type), ADD, SUB, NEG and
//     MUL by a constant under CHECK, an integer CAST under CHECK (never into bool), SLICE_LEN and
//     MAKE_SLICE's length; DIV and REM by a constant (x = c*q + m, truncating), LSHR and ASHR by a
//     constant (floored), SHL by a constant, AND with a constant, the bit counts' ranges; SIZEOF
//     and ALIGNOF of a type whose C spelling has an exact size (layout.h's fixed-size path only,
//     never an ABI layout);
//   - every integer value's TYPE range, refinement included, as the interpreter's landing and
//     settling guarantee it; never for a value whose own obligation is being stated, since that
//     range is the obligation's conclusion;
//   - the version of a local integer or slice CELL a load reads (P2.1): a store's version equals
//     the value stored when its landing is exact; a join's or a written loop header's version is
//     a fresh variable; a cell the loop never writes keeps its version through the header;
//   - the branch conditions and ASSUMEs that dominate the point, as facts about the SSA values
//     they tested (an edge counts when its target has that one predecessor);
//   - at a join that dominates the point, one disjunct per predecessor (R7): the edge conditions
//     on its path, and the version each cell ends that path with;
//   - a CLAIM at a forward join (P3.5's hull row: `lo <= x@join`) or a loop header (its `inv`:
//     `j - i <= 0` at the header), about the versions the block makes, wherever such a version is
//     named. Claims are UNTRUSTED input, so each is also OWED on every edge into its block: `hull`
//     into a join, `inv.entry` and `inv.keep` into a header, at the end of the predecessor, with the
//     edge's condition a fact, about the versions the predecessor ends with (the cells' contents as
//     the block is entered).
// Why these hold. An SSA value's definition holds wherever the definition dominates: if an
// operand's definition ran again after the value's and before the point, there would be a path
// from the entry to the point that avoids the value's definition. The same argument covers a
// dominating edge and its condition's operands. A value made on ONE path into a join does not
// dominate the point, and on another path the frame holds it stale, so its facts are stated only
// inside that path's disjunct (jb_global_at). Type ranges are assume-guarantee over time: each is
// the conclusion of an obligation at an EARLIER event (the settling of the value, or the landing
// of the stored value), so at the first event whose obligation fails, every hypothesis of that
// obligation was already established; hence a value's own range is never its own hypothesis.
// Claims are the same argument: a version names the cell's content at its block's LATEST entry,
// and the claim is the conclusion of the obligation on the edge taken then, an earlier event. A
// claim's obligation sits at its predecessor's end, before the block makes the versions it is
// about, so it never assumes itself; on a back edge it assumes the claim at the header's PREVIOUS
// entry, which is induction over the trips. A claim's other terms are a cell's version current at
// the block, or an SSA value defined in a block that strictly dominates it: neither changes after
// the block's entry anywhere its version is named. A claim is sound only with its obligations:
// whoever proposes claims must have every claim's VCs implied, or drop the claim and generate again.
//
// Not stated yet, each costing precision only: cells whose address escapes (their loads are fresh
// variables); struct field and element cells; a loop invariant no claim proposes, or of three
// terms or more; landings at stores, arguments and returns; callees' contracts; an ABI layout's
// size; a product of two variables (`outside`). The obligations mirror VRA's final pass for
// bounds, division, overflow, integer casts, shifts and bit counts, so the two can be compared
// obligation by obligation.
//
// Checked: scripts/gates/judge_gate.sh evaluates every hypothesis at run time over every accepted
// program; src/tools/test_judge_gen.c pins the rules no program exercises.
//
// Constants are computed in __int128 and stored as JInt: every one is at most 2^65 in magnitude.
#include <string.h>
#include <stdio.h>
#include "ir/ir.h"
#include "ir/layout.h"     // SIZEOF's fixed sizes: a TRUSTED input (P2.4), the emitter's own decision
#include "judge/format.h"
#include "judge/mem.h"

// A FRESH variable's value, for a harness that evaluates a VC at run time: (sum of coef * var) / den,
// an exact division. The generator states its facts; the recipe only says which value they are
// about (the remainder of x / c is x - c*q).
typedef struct { int32_t n; int32_t var[4]; __int128 coef[4]; __int128 den; } JRecipe;

// A claim at a cut point, `sum(coef * term) <= rhs`, over one or two terms by value id: a cell's
// ALLOCA id is the version the cell has as block `blk` is entered, any other id that integer value.
// One term at least is a version the block makes afresh, so it is a join or a loop header, and it
// is not the function's entry, the one block entered with no edge to owe the claim on
// (jg_claims_ok).
typedef struct { int32_t blk, nt; int32_t key[2]; __int128 coef[2]; __int128 rhs; } JClaim;

// An obligation: its instruction, its position, and its VC; or `outside` with the reason. A
// claim's obligation has no instruction: it is owed at the end of block `blk`, on its edge to `to`.
typedef struct {
    IrInstr *at; IrBlock *blk; int32_t idx;
    IrBlock *to; int32_t claim;               // a claim's obligation: the edge's target, the claim
    JVc vc;
    const char *outside;
    // per VC variable, what it is (for a harness that evaluates the VC at run time): kind 0 an SSA
    // value, 1 a cell's version, 2 a slice's length; key the value id, or for a version the cell's
    // ALLOCA value id; type the type to read it with (NULL for a length)
    const int32_t *vkind, *vkey; const IrType **vtype;
    // for a version: the SSA value its store wrote when the version equals it (-1 otherwise), and
    // whether it is the cell's version current at the point (a VC may name an older one)
    const int32_t *vaux; const uint8_t *vcur;
    const JRecipe **vrec;                    // kind 3 (a fresh variable): its recipe
} JObl;

// ── JInt from a machine integer (the generator's side; the kernel has its own arithmetic) ──
static void jg_int(JInt *x, __int128 v) {
    memset(x, 0, sizeof *x);
    unsigned __int128 m = v < 0 ? (unsigned __int128)0 - (unsigned __int128)v : (unsigned __int128)v;
    x->neg = v < 0;
    while (m) { x->limb[x->n++] = (uint32_t)m; m >>= 32; }
}

// A type's integer range as the interpreter reads it (iv_get): an unsigned 64-bit value is in
// [0, 2^64 - 1], not clamped as irtype_int_range clamps it for the octagon. Refinement included.
typedef struct { __int128 lo, hi; bool has_ne; int64_t ne; } JRange;
static bool jg_range(const IrType *t, JRange *r) {
    if (!t) return false;
    r->has_ne = false; r->ne = 0;
    if (t->kind == IRT_BOOL) { r->lo = 0; r->hi = 1; return true; }
    if (t->kind != IRT_INT || t->bits < 1 || t->bits > 64) return false;
    int b = t->bits;
    if (t->is_signed) { r->lo = -((__int128)1 << (b - 1)); r->hi = ((__int128)1 << (b - 1)) - 1; }
    else              { r->lo = 0; r->hi = ((__int128)1 << b) - 1; }
    if (t->has_refine) {
        if ((__int128)t->refine_lo > r->lo) r->lo = t->refine_lo;
        if ((__int128)t->refine_hi < r->hi) r->hi = t->refine_hi;
    }
    if (t->has_ne) { r->has_ne = true; r->ne = t->refine_ne; }
    return true;
}
static bool jg_is_int(const IrType *t) { return t && (t->kind == IRT_INT || t->kind == IRT_BOOL); }
static void jg_type_name(const IrType *t, char *buf, size_t cap) {
    if (!t) { snprintf(buf, cap, "?"); return; }
    if (t->kind == IRT_BOOL) { snprintf(buf, cap, "bool"); return; }
    int o = snprintf(buf, cap, "%c%d", t->is_signed ? 'i' : 'u', t->bits);
    if (t->has_refine && o > 0 && (size_t)o < cap)
        o += snprintf(buf + o, cap - (size_t)o, "[%lld..%lld]", (long long)t->refine_lo, (long long)t->refine_hi);
    if (t->has_ne && o > 0 && (size_t)o < cap) snprintf(buf + o, cap - (size_t)o, "!=%lld", (long long)t->refine_ne);
}
// A CONST's value read by its type, as iv_get reads the 64 stored bits.
static __int128 jg_const(const IrInstr *ins) {
    const IrType *t = ins->result ? ins->result->type : NULL;
    if (t && t->kind == IRT_INT && !t->is_signed && t->bits == 64) return (__int128)(uint64_t)ins->aux.imm;
    return (__int128)ins->aux.imm;
}

// ── the function's structure ────────────────────────────────────────────────────────────────
enum { JV_ENTRY, JV_STORE, JV_JOIN, JV_HEAD };
typedef struct { int kind; int32_t cell, blk, idx; IrValue *stored; IrType *slot; bool unchecked; } JVer;
typedef struct {
    JMem *m; IrFunc *f; char *err; size_t errcap;
    int32_t nb, nv;
    IrBlock **blk;                 // by block id
    int32_t *rpo, nrpo, *rpoix;    // reverse post-order; rpoix[id] = position, -1 unreached
    int32_t *idom;                 // by block id; the entry's is itself
    int32_t *npred;
    IrInstr **def; int32_t *defblk, *defidx;   // by value id
    int32_t *cell, ncells;         // by value id: the cell an ALLOCA is, or -1
    IrValue **cellv;               // by cell: the ALLOCA's value
    JVer *ver; int32_t nver, cver;
    int32_t *instate;              // by block id * ncells: the version at the block's entry
    int32_t *outstate;             // by block id * ncells: the version at the block's exit
    int32_t *loadver;              // by value id: the version a LOAD reads, or -1
    uint8_t *inloop;               // by header id * nb: block in the header's natural loop
    uint8_t *writes;               // by header id * ncells: the loop stores the cell
    uint32_t hash;
    const JClaim *cl; int32_t ncl; // claims (untrusted input: each is owed on every edge into its block)
} JG;

// The value with id `id`: an instruction's result or a parameter.
static IrValue *jg_value(JG *g, int32_t id) {
    if (id < 0 || id >= g->nv) return NULL;
    if (g->def[id]) return g->def[id]->result;
    for (IrParam *p = g->f->params; p; p = p->next) if (p->value && p->value->id == id) return p->value;
    return NULL;
}
static bool jg_fail(JG *g, const char *why) { if (!g->err[0]) snprintf(g->err, g->errcap, "%s", why); return false; }
#define JG_NEW(g, T, n) ((T *)jmem_alloc((g)->m, sizeof(T), (int64_t)(n)))

static int jg_succ(IrBlock *b, IrBlock **out) {
    int n = 0;
    switch (b->term.kind) {
        case IR_TERM_BR: if (b->term.a) out[n++] = b->term.a; break;
        case IR_TERM_BR_COND:
            if (b->term.a) out[n++] = b->term.a;
            if (b->term.b) out[n++] = b->term.b;
            break;
        case IR_TERM_SWITCH:
            if (b->term.a) out[n++] = b->term.a;
            for (IrSwitchCase *c = b->term.cases; c && n < 64; c = c->next) out[n++] = c->target;
            break;
        default: break;
    }
    return n;
}
static bool jg_dominates(JG *g, int32_t a, int32_t b) {   // block a dominates block b
    if (g->rpoix[a] < 0 || g->rpoix[b] < 0) return false;
    while (b != a) { int32_t p = g->idom[b]; if (p == b) return false; b = p; }
    return true;
}
static uint32_t jg_fnv(uint32_t h, int64_t v) {
    for (int i = 0; i < 8; i++) { h ^= (uint8_t)(v >> (8 * i)); h *= 16777619u; }
    return h;
}

static bool jg_structure(JG *g) {
    IrFunc *f = g->f;
    g->nb = f->next_block_id; g->nv = f->next_value_id;
    if (!(g->blk = JG_NEW(g, IrBlock *, g->nb)) || !(g->rpoix = JG_NEW(g, int32_t, g->nb)) ||
        !(g->idom = JG_NEW(g, int32_t, g->nb)) || !(g->npred = JG_NEW(g, int32_t, g->nb)) ||
        !(g->rpo = JG_NEW(g, int32_t, g->nb)) || !(g->def = JG_NEW(g, IrInstr *, g->nv)) ||
        !(g->defblk = JG_NEW(g, int32_t, g->nv)) || !(g->defidx = JG_NEW(g, int32_t, g->nv)) ||
        !(g->cell = JG_NEW(g, int32_t, g->nv)) || !(g->loadver = JG_NEW(g, int32_t, g->nv)))
        return jg_fail(g, "out of memory");
    for (int32_t i = 0; i < g->nb; i++) { g->blk[i] = NULL; g->rpoix[i] = -1; g->npred[i] = 0; }
    for (int32_t i = 0; i < g->nv; i++) { g->def[i] = NULL; g->defblk[i] = -1; g->defidx[i] = -1; g->cell[i] = -1; g->loadver[i] = -1; }
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (b->id < 0 || b->id >= g->nb) return jg_fail(g, "a block id out of range");
        g->blk[b->id] = b;
        if (b->phis) return jg_fail(g, "a phi (the IR keeps locals in allocas, so the generator reads none)");
        int32_t k = 0;
        for (IrInstr *i = b->instrs; i; i = i->next, k++)
            if (i->result) {
                if (i->result->id < 0 || i->result->id >= g->nv) return jg_fail(g, "a value id out of range");
                g->def[i->result->id] = i; g->defblk[i->result->id] = b->id; g->defidx[i->result->id] = k;
            }
    }
    // reverse post-order by an explicit stack (a deep CFG must not overflow C's)
    {
        int32_t *st = JG_NEW(g, int32_t, g->nb), *nx = JG_NEW(g, int32_t, g->nb), *post = JG_NEW(g, int32_t, g->nb);
        uint8_t *seen = JG_NEW(g, uint8_t, g->nb);
        if (!st || !nx || !post || !seen) return jg_fail(g, "out of memory");
        memset(seen, 0, (size_t)g->nb);
        int32_t sp = 0, np = 0;
        st[sp] = f->entry->id; nx[sp++] = 0; seen[f->entry->id] = 1;
        while (sp) {
            IrBlock *b = g->blk[st[sp - 1]], *s[66]; int ns = jg_succ(b, s);
            if (nx[sp - 1] < ns) {
                IrBlock *c = s[nx[sp - 1]++];
                if (c && !seen[c->id]) { seen[c->id] = 1; st[sp] = c->id; nx[sp++] = 0; }
            } else post[np++] = st[--sp];
        }
        g->nrpo = np;
        for (int32_t i = 0; i < np; i++) { g->rpo[i] = post[np - 1 - i]; g->rpoix[g->rpo[i]] = i; }
    }
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (g->rpoix[b->id] < 0) continue;
        IrBlock *s[66]; int ns = jg_succ(b, s);
        for (int k = 0; k < ns; k++) if (s[k]) g->npred[s[k]->id]++;
    }
    // dominators (Cooper, Harvey and Kennedy), over the RPO
    for (int32_t i = 0; i < g->nb; i++) g->idom[i] = -1;
    g->idom[f->entry->id] = f->entry->id;
    for (bool changed = true; changed; ) {
        changed = false;
        for (int32_t r = 1; r < g->nrpo; r++) {
            IrBlock *b = g->blk[g->rpo[r]]; int32_t nd = -1;
            for (IrEdge *e = b->preds; e; e = e->next) {
                int32_t p = e->block->id;
                if (g->rpoix[p] < 0 || g->idom[p] < 0) continue;
                if (nd < 0) { nd = p; continue; }
                int32_t x = p, y = nd;
                while (x != y) {
                    while (g->rpoix[x] > g->rpoix[y]) x = g->idom[x];
                    while (g->rpoix[y] > g->rpoix[x]) y = g->idom[y];
                }
                nd = x;
            }
            if (nd >= 0 && g->idom[b->id] != nd) { g->idom[b->id] = nd; changed = true; }
        }
    }
    // the hash: the function's IR without positions (judge_interfaces §1)
    uint32_t h = 2166136261u;
    for (int32_t r = 0; r < g->nrpo; r++) {
        IrBlock *b = g->blk[g->rpo[r]];
        h = jg_fnv(h, b->id);
        for (IrInstr *i = b->instrs; i; i = i->next) {
            h = jg_fnv(h, i->op); h = jg_fnv(h, i->result ? i->result->id : -1); h = jg_fnv(h, i->wrap);
            h = jg_fnv(h, i->unchecked);
            for (int k = 0; k < i->n_operands; k++) h = jg_fnv(h, i->operands[k] ? i->operands[k]->id : -1);
            if (i->op == IR_CONST || i->op == IR_ICMP || i->op == IR_CAST || i->op == IR_ELEM_PTR || i->op == IR_FIELD_PTR)
                h = jg_fnv(h, i->aux.imm);
        }
        h = jg_fnv(h, b->term.kind); h = jg_fnv(h, b->term.cond ? b->term.cond->id : -1);
        h = jg_fnv(h, b->term.a ? b->term.a->id : -1); h = jg_fnv(h, b->term.b ? b->term.b->id : -1);
    }
    g->hash = h;
    return true;
}

static bool jg_pushver(JG *g, int kind, int32_t cell, int32_t blk, int32_t idx, IrValue *stored, bool unchecked) {
    if (g->nver == g->cver) {
        JVer *nv = JG_NEW(g, JVer, g->cver * 2);
        if (!nv) return jg_fail(g, "out of memory");
        memcpy(nv, g->ver, sizeof *nv * (size_t)g->nver); g->ver = nv; g->cver *= 2;
    }
    JVer *v = &g->ver[g->nver++];
    v->kind = kind; v->cell = cell; v->blk = blk; v->idx = idx; v->stored = stored; v->unchecked = unchecked;
    v->slot = g->def[g->cellv[cell]->id]->aux.alloca_ty;
    return true;
}

// ── cells and their versions (P2.1, step A: a local whose every use is a LOAD's or STORE's address) ──
static bool jg_cells(JG *g) {
    IrFunc *f = g->f;
    uint8_t *ok = JG_NEW(g, uint8_t, g->nv);
    if (!ok) return jg_fail(g, "out of memory");
    for (int32_t i = 0; i < g->nv; i++) {
        IrInstr *d = g->def[i];
        // an integer cell, or a slice cell (whose versions carry a length: a slice is copied whole)
        ok[i] = d && d->op == IR_ALLOCA && d->aux.alloca_ty && !d->data &&
                (jg_is_int(d->aux.alloca_ty) || d->aux.alloca_ty->kind == IRT_SLICE);
    }
    for (IrBlock *b = f->blocks; b; b = b->next) {
        for (IrInstr *i = b->instrs; i; i = i->next)
            for (int k = 0; k < i->n_operands; k++) {
                IrValue *o = i->operands[k];
                if (!o || o->id < 0 || o->id >= g->nv || !ok[o->id]) continue;
                bool addr = (i->op == IR_LOAD && k == 0) || (i->op == IR_STORE && k == 0);
                if (!addr) ok[o->id] = 0;                    // escapes: P2.1's later classes
            }
        if (b->term.cond && b->term.cond->id >= 0 && b->term.cond->id < g->nv) ok[b->term.cond->id] = 0;
    }
    g->ncells = 0;
    for (int32_t i = 0; i < g->nv; i++) if (ok[i]) g->cell[i] = g->ncells++;
    if (!(g->cellv = JG_NEW(g, IrValue *, g->ncells ? g->ncells : 1))) return jg_fail(g, "out of memory");
    for (int32_t i = 0; i < g->nv; i++) if (g->cell[i] >= 0) g->cellv[g->cell[i]] = g->def[i]->result;
    // natural loops: a back edge t -> h, h dominating t; the loop is h and every block reaching t
    // without passing h
    size_t lsz = (size_t)g->nb * (size_t)g->nb, wsz = (size_t)g->nb * (size_t)(g->ncells ? g->ncells : 1);
    if (!(g->inloop = JG_NEW(g, uint8_t, lsz)) || !(g->writes = JG_NEW(g, uint8_t, wsz))) return jg_fail(g, "out of memory");
    memset(g->inloop, 0, lsz); memset(g->writes, 0, wsz);
    int32_t *wl = JG_NEW(g, int32_t, g->nb);
    if (!wl) return jg_fail(g, "out of memory");
    for (int32_t r = 0; r < g->nrpo; r++) {
        IrBlock *t = g->blk[g->rpo[r]], *s[66]; int ns = jg_succ(t, s);
        for (int k = 0; k < ns; k++) {
            if (!s[k] || !jg_dominates(g, s[k]->id, t->id)) continue;
            int32_t h = s[k]->id; uint8_t *L = g->inloop + (size_t)h * (size_t)g->nb;
            L[h] = 1;
            int32_t n = 0;
            if (!L[t->id]) { L[t->id] = 1; wl[n++] = t->id; }
            while (n) {
                IrBlock *x = g->blk[wl[--n]];
                for (IrEdge *e = x->preds; e; e = e->next)
                    if (g->rpoix[e->block->id] >= 0 && !L[e->block->id]) { L[e->block->id] = 1; wl[n++] = e->block->id; }
            }
        }
    }
    for (int32_t h = 0; h < g->nb; h++) {
        uint8_t *L = g->inloop + (size_t)h * (size_t)g->nb;
        if (!L[h]) continue;
        for (int32_t x = 0; x < g->nb; x++) {
            if (!L[x] || !g->blk[x]) continue;
            for (IrInstr *i = g->blk[x]->instrs; i; i = i->next)
                if (i->op == IR_STORE && i->n_operands >= 2 && i->operands[0] && i->operands[0]->id >= 0 &&
                    i->operands[0]->id < g->nv && g->cell[i->operands[0]->id] >= 0)
                    g->writes[(size_t)h * (size_t)g->ncells + (size_t)g->cell[i->operands[0]->id]] = 1;
        }
    }
    // versions, forward over the RPO: a join's predecessors that are not back edges come first
    g->cver = 16; g->nver = 0;
    if (!(g->ver = JG_NEW(g, JVer, g->cver))) return jg_fail(g, "out of memory");
    size_t ssz = (size_t)g->nb * (size_t)(g->ncells ? g->ncells : 1);
    int32_t *out = JG_NEW(g, int32_t, ssz);
    if (!(g->instate = JG_NEW(g, int32_t, ssz)) || !out) return jg_fail(g, "out of memory");
    g->outstate = out;
    for (size_t i = 0; i < ssz; i++) { g->instate[i] = -1; out[i] = -1; }
    for (int32_t r = 0; r < g->nrpo; r++) {
        IrBlock *b = g->blk[g->rpo[r]];
        int32_t *in = g->instate + (size_t)b->id * (size_t)g->ncells;
        uint8_t *L = g->inloop + (size_t)b->id * (size_t)g->nb;
        bool header = L[b->id];
        for (int32_t c = 0; c < g->ncells; c++) {
            if (b == g->f->entry) {
                if (!jg_pushver(g, JV_ENTRY, c, b->id, -1, NULL, false)) return false;
                in[c] = g->nver - 1; continue;
            }
            if (header && g->writes[(size_t)b->id * (size_t)g->ncells + (size_t)c]) {
                if (!jg_pushver(g, JV_HEAD, c, b->id, -1, NULL, false)) return false;
                in[c] = g->nver - 1; continue;
            }
            int32_t v = -2;                                  // -2: no predecessor seen yet
            for (IrEdge *e = b->preds; e; e = e->next) {
                int32_t p = e->block->id;
                if (g->rpoix[p] < 0) continue;
                if (g->rpoix[p] >= r) continue;              // a back edge: the cut decided above
                int32_t pv = out[(size_t)p * (size_t)g->ncells + (size_t)c];
                v = v == -2 ? pv : (v == pv ? v : -1);
            }
            if (v < 0) {
                if (!jg_pushver(g, JV_JOIN, c, b->id, -1, NULL, false)) return false;
                v = g->nver - 1;
            }
            in[c] = v;
        }
        int32_t *o = out + (size_t)b->id * (size_t)g->ncells;
        for (int32_t c = 0; c < g->ncells; c++) o[c] = in[c];
        int32_t k = 0;
        for (IrInstr *i = b->instrs; i; i = i->next, k++) {
            IrValue *a = i->n_operands >= 1 ? i->operands[0] : NULL;
            int32_t c = a && a->id >= 0 && a->id < g->nv ? g->cell[a->id] : -1;
            if (c < 0) continue;
            if (i->op == IR_LOAD && i->result) g->loadver[i->result->id] = o[c];
            else if (i->op == IR_STORE && i->n_operands >= 2) {
                if (!jg_pushver(g, JV_STORE, c, b->id, k, i->operands[1], i->unchecked)) return false;
                o[c] = g->nver - 1;
            }
        }
    }
    return true;
}

// ── one VC ─────────────────────────────────────────────────────────────────────────────────
typedef struct { int32_t var; __int128 c; } JLT;
typedef struct {
    JG *g; int32_t blk, idx;                 // the obligation's point
    int32_t *vval, *vver, *vlen;             // var of a value, a version, a slice's length; -1 none
    const char **names; int32_t nvars, cvars;
    int32_t *kind, *key;                     // per var: 0 value, 1 version, 2 length, 3 fresh; its key
    const IrType **vtype;                    // per var: the type it is read with
    const JRecipe **vrec;                    // per var: a fresh variable's recipe, else NULL
    int32_t *queue, qh, qt;                  // vars whose facts are still to be stated
    JHyp *hyps; int32_t nhyps, chyps;
    int32_t self;                            // the value whose own range is the goal's, or -1
    // R7, a join's disjunct being built: its join block (-1: facts are VC-wide), its conjuncts, and
    // per var the disjunct it was last described in. A value or version whose definition does not
    // dominate the join is LOCAL to a path: on another path it is stale, so its facts may only be
    // stated inside this disjunct, never as hypotheses of their own.
    int32_t jS; JCons *sink; int32_t nsink, csink; int32_t *dsc; int32_t dgen;
    int32_t to;                              // a claim's obligation: the edge's target block; else -1
    int32_t *cst;                            // per claim: -2 unstated, -1 VC-wide, else its disjunct
    bool fail;
} JB;

static char *jb_str(JB *B, const char *fmt, long long a, long long b2, long long c3) {
    char tmp[96]; int n = snprintf(tmp, sizeof tmp, fmt, a, b2, c3);
    char *s = JG_NEW(B->g, char, (n > 0 ? n : 0) + 1);
    if (!s) { B->fail = true; return NULL; }
    memcpy(s, tmp, (size_t)(n > 0 ? n : 0) + 1); return s;
}
static int32_t jb_newvar(JB *B, const char *name, int32_t kind, int32_t key, const IrType *ty) {
    if (!name) { B->fail = true; return 0; }
    if (B->nvars == B->cvars) {
        int32_t nc = B->cvars ? B->cvars * 2 : 16;
        const char **nn = JG_NEW(B->g, const char *, nc); int32_t *nk = JG_NEW(B->g, int32_t, nc), *ny = JG_NEW(B->g, int32_t, nc);
        int32_t *nq = JG_NEW(B->g, int32_t, nc); const IrType **nt = JG_NEW(B->g, const IrType *, nc);
        const JRecipe **nr = JG_NEW(B->g, const JRecipe *, nc);
        int32_t *nd = JG_NEW(B->g, int32_t, nc);
        if (!nn || !nk || !ny || !nq || !nt || !nr || !nd) { B->fail = true; return 0; }
        if (B->nvars) memcpy(nd, B->dsc, sizeof *nd * (size_t)B->nvars);
        B->dsc = nd;
        if (B->nvars) {
            memcpy(nn, B->names, sizeof *nn * (size_t)B->nvars); memcpy(nk, B->kind, sizeof *nk * (size_t)B->nvars);
            memcpy(ny, B->key, sizeof *ny * (size_t)B->nvars); memcpy(nq, B->queue, sizeof *nq * (size_t)B->qt);
            memcpy(nt, B->vtype, sizeof *nt * (size_t)B->nvars); memcpy(nr, B->vrec, sizeof *nr * (size_t)B->nvars);
        }
        B->names = nn; B->kind = nk; B->key = ny; B->queue = nq; B->vtype = nt; B->vrec = nr; B->cvars = nc;
    }
    B->names[B->nvars] = name; B->kind[B->nvars] = kind; B->key[B->nvars] = key; B->vtype[B->nvars] = ty;
    B->vrec[B->nvars] = NULL; B->dsc[B->nvars] = 0;
    B->queue[B->qt++] = B->nvars;
    return B->nvars++;
}
static void jb_state(JB *B, int32_t x);
// Does block `blk` (where a value or version is made) dominate the join being described?
static bool jb_global_at(JB *B, int32_t blk) { return B->jS < 0 || jg_dominates(B->g, blk < 0 ? B->g->f->entry->id : blk, B->jS); }
// A local variable: made without queueing, and described into the current disjunct once.
static int32_t jb_local(JB *B, int32_t x, bool fresh) {
    if (fresh) B->qt--;                                        // jb_newvar queued it: locals are not VC-wide
    if (!B->dsc || B->dsc[x] == B->dgen) return x;
    B->dsc[x] = B->dgen;
    jb_state(B, x);
    return x;
}
static int32_t jb_ver(JB *B, int32_t ver) {
    JVer *v = &B->g->ver[ver]; int32_t cid = B->g->cellv[v->cell]->id;
    bool local = !jb_global_at(B, v->kind == JV_ENTRY ? -1 : v->blk);
    if (B->vver[ver] >= 0) return local ? jb_local(B, B->vver[ver], false) : B->vver[ver];
    const char *n = v->kind == JV_ENTRY ? jb_str(B, "%%s%lld@entry", cid, 0, 0)
                  : v->kind == JV_STORE ? jb_str(B, "%%s%lld@bb%lld.%lld", cid, v->blk, v->idx)
                  : v->kind == JV_JOIN  ? jb_str(B, "%%s%lld@join.bb%lld", cid, v->blk, 0)
                  :                       jb_str(B, "%%s%lld@head.bb%lld", cid, v->blk, 0);
    B->vver[ver] = jb_newvar(B, n, 1, ver, v->slot);
    return local ? jb_local(B, B->vver[ver], true) : B->vver[ver];
}
// The variable a value is, at the point: a load of a cell is the version it read.
static int32_t jb_val(JB *B, IrValue *v) {
    if (!v || v->id < 0 || v->id >= B->g->nv) { B->fail = true; return 0; }
    if (B->g->loadver[v->id] >= 0) return jb_ver(B, B->g->loadver[v->id]);
    bool local = !jb_global_at(B, B->g->defblk[v->id]);
    if (B->vval[v->id] >= 0) return local ? jb_local(B, B->vval[v->id], false) : B->vval[v->id];
    B->vval[v->id] = jb_newvar(B, jb_str(B, "%%v%lld", v->id, 0, 0), 0, v->id, v->type);
    return local ? jb_local(B, B->vval[v->id], true) : B->vval[v->id];
}
static int32_t jb_len(JB *B, IrValue *s) {
    if (!s || s->id < 0 || s->id >= B->g->nv) { B->fail = true; return 0; }
    // a load of a slice cell's STORE version is the slice stored, length and all; the store and
    // that slice's definition dominate the load, hence the point
    for (int hop = 0; hop < 64 && B->g->loadver[s->id] >= 0; hop++) {
        JVer *v = &B->g->ver[B->g->loadver[s->id]];
        if (v->kind != JV_STORE || !v->stored || !v->stored->type || v->stored->type->kind != IRT_SLICE) break;
        s = v->stored;
        if (s->id < 0 || s->id >= B->g->nv) { B->fail = true; return 0; }
    }
    bool local = !jb_global_at(B, B->g->defblk[s->id]);
    if (B->vlen[s->id] >= 0) return local ? jb_local(B, B->vlen[s->id], false) : B->vlen[s->id];
    B->vlen[s->id] = jb_newvar(B, jb_str(B, "len(%%v%lld)", s->id, 0, 0), 2, s->id, NULL);
    return local ? jb_local(B, B->vlen[s->id], true) : B->vlen[s->id];
}
// A constraint from terms: sorted by var (declaration order), merged, zero terms dropped.
static bool jb_cons(JB *B, JLT *t, int n, JOp op, __int128 rhs, JCons *c) {
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && t[j - 1].var > t[j].var; j--) { JLT x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && t[m - 1].var == t[i].var) t[m - 1].c += t[i].c; else t[m++] = t[i];
        if (m && t[m - 1].c == 0) m--;
    }
    memset(c, 0, sizeof *c);
    c->op = op; jg_int(&c->rhs, rhs); c->nt = m;
    if (m && !(c->t = JG_NEW(B->g, JTerm, m))) { B->fail = true; return false; }
    for (int i = 0; i < m; i++) { c->t[i].var = t[i].var; jg_int(&c->t[i].coef, t[i].c); }
    return true;
}
static JHyp *jb_hyp(JB *B, const char *origin) {
    if (!origin) { B->fail = true; return NULL; }
    if (B->nhyps == B->chyps) {
        int32_t nc = B->chyps ? B->chyps * 2 : 16;
        JHyp *nh = JG_NEW(B->g, JHyp, nc);
        if (!nh) { B->fail = true; return NULL; }
        if (B->nhyps) memcpy(nh, B->hyps, sizeof *nh * (size_t)B->nhyps);
        B->hyps = nh; B->chyps = nc;
    }
    JHyp *h = &B->hyps[B->nhyps++]; memset(h, 0, sizeof *h); h->origin = origin;
    return h;
}
static void jb_atom(JB *B, const char *origin, JLT *t, int n, JOp op, __int128 rhs) {
    if (B->jS >= 0) {                                          // a conjunct of the current disjunct
        if (B->nsink == B->csink) {
            int32_t nc = B->csink ? B->csink * 2 : 8;
            JCons *ns = JG_NEW(B->g, JCons, nc);
            if (!ns) { B->fail = true; return; }
            if (B->nsink) memcpy(ns, B->sink, sizeof *ns * (size_t)B->nsink);
            B->sink = ns; B->csink = nc;
        }
        jb_cons(B, t, n, op, rhs, &B->sink[B->nsink++]);
        return;
    }
    JHyp *h = jb_hyp(B, origin);
    if (h) jb_cons(B, t, n, op, rhs, &h->cons);
}
// lo <= x <= hi (two hypotheses), and x != ne as a disjunction
static void jb_range(JB *B, int32_t x, const JRange *r, const char *origin) {
    JLT t[1];
    t[0].var = x; t[0].c = -1; jb_atom(B, origin, t, 1, J_LE, -r->lo);
    t[0].var = x; t[0].c = 1;  jb_atom(B, origin, t, 1, J_LE, r->hi);
    if (r->has_ne && B->jS < 0) {                              // a disjunct cannot hold a disjunction
        JHyp *h = jb_hyp(B, origin);
        if (!h) return;
        h->disj = true; h->ndis = 2;
        if (!(h->dis = JG_NEW(B->g, JConj, 2))) { B->fail = true; return; }
        for (int d = 0; d < 2; d++) {
            if (!(h->dis[d].c = JG_NEW(B->g, JCons, 1))) { B->fail = true; return; }
            h->dis[d].nc = 1;
            t[0].var = x; t[0].c = d == 0 ? 1 : -1;
            jb_cons(B, t, 1, J_LE, d == 0 ? (__int128)r->ne - 1 : -(__int128)r->ne - 1, &h->dis[d].c[0]);
        }
    }
}
static void jb_type(JB *B, int32_t x, const IrType *ty, const char *what, long long id) {
    JRange r; char tn[64];
    if (!jg_range(ty, &r)) return;
    jg_type_name(ty, tn, sizeof tn);
    char o[128]; snprintf(o, sizeof o, "type %s%lld %s", what, id, tn);
    char *os = JG_NEW(B->g, char, strlen(o) + 1);
    if (!os) { B->fail = true; return; }
    strcpy(os, o);
    jb_range(B, x, &r, os);
}
// x = sum of terms + k (an exact definition), as one equality: x - terms = k
static void jb_defk(JB *B, int32_t x, JLT *t, int n, __int128 k, const char *origin) {
    JLT u[4]; int m = 0;
    u[m].var = x; u[m].c = 1; m++;
    for (int i = 0; i < n && m < 4; i++) { u[m] = t[i]; u[m].c = -t[i].c; m++; }
    jb_atom(B, origin, u, m, J_EQ, k);
}
static void jb_def(JB *B, int32_t x, JLT *t, int n, const char *origin) { jb_defk(B, x, t, n, 0, origin); }

// Does a STORE keep the value it stores? Yes when the slot's type is the value's, or when the
// landing is checked (outside `unsafe` a value that does not fit is a failed proof); inside
// `unsafe` it wraps into the slot (ii_land), so the version is not the value.
static bool jg_store_exact(const JVer *v) {
    if (!v->stored || !jg_is_int(v->stored->type) || !v->slot) return false;
    const IrType *a = v->stored->type, *b = v->slot;
    if (a->kind == b->kind && (a->kind == IRT_BOOL || (a->bits == b->bits && a->is_signed == b->is_signed))) return true;
    return !v->unchecked;
}

// A fresh variable `<what>.%v<id>` whose value is (t[0..n) + 0) / den. It is stated nothing about
// by itself; the row that makes it states its facts. Not queued: it has no definition to state.
static int32_t jb_fresh(JB *B, const char *what, int32_t id, const JLT *t, int n, __int128 den) {
    char nm[48]; snprintf(nm, sizeof nm, "%s.%%v%d", what, id);
    char *s = JG_NEW(B->g, char, strlen(nm) + 1);
    JRecipe *r = JG_NEW(B->g, JRecipe, 1);
    if (!s || !r) { B->fail = true; return 0; }
    strcpy(s, nm);
    r->n = n; r->den = den;
    for (int i = 0; i < n; i++) { r->var[i] = t[i].var; r->coef[i] = t[i].c; }
    int32_t x = jb_newvar(B, s, 3, id, NULL);
    B->qt--;                                                   // nothing to state for it
    B->vrec[x] = r;
    return x;
}
// x = c*q + m, with m's range following x's sign (C's division truncates toward zero) or, when
// `floor`, always in [0, |c| - 1]: the one shape behind DIV, REM and the shifts by a constant.
// `xv` and `qv` are variables; m is `mv`. `xuns`: x is unsigned, so its sign is known.
static void jb_divmod(JB *B, int32_t xv, int32_t qv, int32_t mv, __int128 c, bool xuns, bool floor_, const char *origin) {
    JLT t[3];
    t[0].var = xv; t[0].c = 1; t[1].var = qv; t[1].c = -c; t[2].var = mv; t[2].c = -1;
    jb_atom(B, origin, t, 3, J_EQ, 0);                         // x - c*q - m = 0
    __int128 ac = c < 0 ? -c : c;
    if (xuns || floor_) {
        t[0].var = mv; t[0].c = -1; jb_atom(B, origin, t, 1, J_LE, 0);
        t[0].var = mv; t[0].c = 1;  jb_atom(B, origin, t, 1, J_LE, ac - 1);
        return;
    }
    if (B->jS >= 0) return;                                    // the sign cases are a disjunction
    JHyp *h = jb_hyp(B, origin);                               // x >= 0, 0 <= m <= |c|-1  |  x <= -1, -(|c|-1) <= m <= 0
    if (!h) return;
    h->disj = true; h->ndis = 2;
    if (!(h->dis = JG_NEW(B->g, JConj, 2))) { B->fail = true; return; }
    for (int d = 0; d < 2; d++) {
        if (!(h->dis[d].c = JG_NEW(B->g, JCons, 3))) { B->fail = true; return; }
        h->dis[d].nc = 3;
        JLT u[1];
        u[0].var = xv; u[0].c = d == 0 ? -1 : 1; jb_cons(B, u, 1, J_LE, d == 0 ? 0 : -1, &h->dis[d].c[0]);
        u[0].var = mv; u[0].c = -1; jb_cons(B, u, 1, J_LE, d == 0 ? 0 : ac - 1, &h->dis[d].c[1]);
        u[0].var = mv; u[0].c = 1;  jb_cons(B, u, 1, J_LE, d == 0 ? ac - 1 : 0, &h->dis[d].c[2]);
    }
}
// A constant operand: a CONST read by its type, or a signed NEG of one under CHECK (`x / -4`
// lowers its divisor as `neg 4`), which is -c wherever its own obligation held, as NEG's row says.
static bool jg_const_of(JG *g, IrValue *v, __int128 *c) {
    IrInstr *d = v && v->id >= 0 && v->id < g->nv ? g->def[v->id] : NULL;
    if (d && d->op == IR_NEG && d->wrap == IR_WRAP_CHECK && !d->unchecked && d->n_operands >= 1 && d->result &&
        d->result->type && d->result->type->kind == IRT_INT && d->result->type->is_signed) {
        IrValue *o = d->operands[0];
        IrInstr *e = o && o->id >= 0 && o->id < g->nv ? g->def[o->id] : NULL;
        if (!e || e->op != IR_CONST || !jg_is_int(e->result->type)) return false;
        *c = -jg_const(e); return true;
    }
    if (!d || d->op != IR_CONST || !jg_is_int(d->result->type)) return false;
    *c = jg_const(d); return true;
}
static bool jg_uns(const IrType *t) { return t && (t->kind == IRT_BOOL || (t->kind == IRT_INT && !t->is_signed)); }

// A claim's term as a VC variable: a cell is its version in `row` (a block's entry state, or the
// exit state of the predecessor an obligation is owed at), any other id that SSA value.
static int32_t jb_term(JB *B, int32_t key, const int32_t *row) {
    JG *g = B->g;
    if (g->cell[key] >= 0) return jb_ver(B, row[g->cell[key]]);
    return jb_val(B, jg_value(g, key));
}
// The claims about a version a join or a header makes, stated where the version is: VC-wide, or
// inside the current disjunct when the version is local to its path. Each once per place (a claim
// of two terms is reached from either).
static void jb_claims(JB *B, const JVer *v) {
    JG *g = B->g;
    int32_t at = B->jS >= 0 ? B->dgen : -1;
    for (int32_t ci = 0; ci < g->ncl && !B->fail; ci++) {
        const JClaim *c = &g->cl[ci];
        if (c->blk != v->blk || B->cst[ci] == at) continue;
        bool about = false;
        for (int k = 0; k < c->nt; k++) if (g->cell[c->key[k]] == v->cell) about = true;
        if (!about) continue;
        B->cst[ci] = at;
        JLT t[2];
        for (int k = 0; k < c->nt; k++) {
            t[k].var = jb_term(B, c->key[k], g->instate + (size_t)c->blk * (size_t)g->ncells);
            t[k].c = c->coef[k];
        }
        jb_atom(B, jb_str(B, "claim bb%lld #%lld", c->blk, ci, 0), t, c->nt, J_LE, c->rhs);
    }
}
// The facts of one variable: its definition and its range.
static void jb_state(JB *B, int32_t x) {
    JG *g = B->g;
    if (B->kind[x] == 1) {                                     // a cell's version
        JVer *v = &g->ver[B->key[x]];
        jb_type(B, x, v->slot, "%s", g->cellv[v->cell]->id);
        if (v->kind == JV_STORE && jg_store_exact(v)) {
            // the version is the value stored; a value of another type lands in the slot, and that
            // landing's obligation is the guarantee the slot's range above rests on
            JLT t[1]; t[0].var = jb_val(B, v->stored); t[0].c = 1;
            jb_def(B, x, t, 1, jb_str(B, "store %%s%lld@bb%lld.%lld", g->cellv[v->cell]->id, v->blk, v->idx));
        }
        if (v->kind == JV_JOIN || v->kind == JV_HEAD) jb_claims(B, v);
        return;
    }
    if (B->kind[x] == 2) {                                     // a slice's length
        IrValue *s = jg_value(g, B->key[x]);
        IrType *st = s ? s->type : NULL;
        int64_t es = 1;
        if (st && st->elem && st->elem->kind == IRT_INT) es = st->elem->bits >= 8 ? st->elem->bits / 8 : 1;
        JLT t[1]; t[0].var = x;
        t[0].c = -1; jb_atom(B, jb_str(B, "objsize %%v%lld", B->key[x], 0, 0), t, 1, J_LE, 0);
        t[0].c = 1;  jb_atom(B, jb_str(B, "objsize %%v%lld", B->key[x], 0, 0), t, 1, J_LE, (__int128)INT64_MAX / es);
        IrInstr *d = g->def[B->key[x]];
        if (d && d->op == IR_MAKE_SLICE && d->n_operands >= 2 && jg_is_int(d->operands[1]->type)) {
            JLT u[1]; u[0].var = jb_val(B, d->operands[1]); u[0].c = 1;
            jb_def(B, x, u, 1, jb_str(B, "def %%v%lld make_slice", B->key[x], 0, 0));
        }
        return;
    }
    if (B->kind[x] != 0) return;
    int32_t id = B->key[x];
    IrInstr *d = g->def[id];
    IrValue *v = d ? d->result : NULL;
    if (!v) {                                                  // a parameter
        for (IrParam *p = g->f->params; p; p = p->next)
            if (p->value && p->value->id == id) { jb_type(B, x, p->value->type, "%v", id); break; }
        return;
    }
    if (!jg_is_int(v->type)) return;
    // a constant's definition pins it; its range would say nothing more
    if (id != B->self && d->op != IR_CONST) jb_type(B, x, v->type, "%v", id);
    JLT t[2];
    switch (d->op) {
        case IR_CONST:
            jb_defk(B, x, NULL, 0, jg_const(d), jb_str(B, "def %%v%lld const", id, 0, 0));
            break;
        case IR_ADD: case IR_SUB:
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 2) break;
            if (!jg_is_int(d->operands[0]->type) || !jg_is_int(d->operands[1]->type)) break;
            t[0].var = jb_val(B, d->operands[0]); t[0].c = 1;
            t[1].var = jb_val(B, d->operands[1]); t[1].c = d->op == IR_ADD ? 1 : -1;
            jb_def(B, x, t, 2, jb_str(B, d->op == IR_ADD ? "def %%v%lld add" : "def %%v%lld sub", id, 0, 0));
            break;
        case IR_NEG:
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 1 || !jg_is_int(d->operands[0]->type)) break;
            t[0].var = jb_val(B, d->operands[0]); t[0].c = -1;
            jb_def(B, x, t, 1, jb_str(B, "def %%v%lld neg", id, 0, 0));
            break;
        case IR_MUL: {
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 2) break;
            for (int k = 0; k < 2; k++) {
                IrInstr *cd = d->operands[k] && d->operands[k]->id >= 0 && d->operands[k]->id < g->nv ? g->def[d->operands[k]->id] : NULL;
                IrValue *o = d->operands[1 - k];
                if (!cd || cd->op != IR_CONST || !jg_is_int(o->type)) continue;
                t[0].var = jb_val(B, o); t[0].c = jg_const(cd);
                jb_def(B, x, t, 1, jb_str(B, "def %%v%lld mul", id, 0, 0));
                break;
            }
            break;
        }
        case IR_CAST:
            // into an integer only: a cast to bool is 1 for any nonzero value (§3's Q row)
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 1 || !jg_is_int(d->operands[0]->type) ||
                v->type->kind != IRT_INT) break;
            t[0].var = jb_val(B, d->operands[0]); t[0].c = 1;
            jb_def(B, x, t, 1, jb_str(B, "def %%v%lld cast", id, 0, 0));
            break;
        case IR_SLICE_LEN:
            if (d->n_operands < 1) break;
            t[0].var = jb_len(B, d->operands[0]); t[0].c = 1;
            jb_def(B, x, t, 1, jb_str(B, "def %%v%lld slice_len", id, 0, 0));
            break;
        // DIV and REM by a constant: x = c*q + m, truncating (interp.h:463-475). A signed quotient
        // exists only where its CHECK held (divovf is that obligation); a MODULAR or waived one may
        // have wrapped, so it is stated nothing about.
        case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: {
            __int128 c;
            if (d->n_operands < 2 || !jg_const_of(g, d->operands[1], &c) || c == 0 || !jg_is_int(d->operands[0]->type)) break;
            bool uns = jg_uns(d->operands[0]->type);
            if (!uns && (d->wrap != IR_WRAP_CHECK || d->unchecked)) break;
            int32_t xv = jb_val(B, d->operands[0]);
            JLT r[2];
            if (d->op == IR_SDIV || d->op == IR_UDIV) {          // x is q = x, remainder fresh
                r[0].var = xv; r[0].c = 1; r[1].var = x; r[1].c = -c;
                int32_t m = jb_fresh(B, "rem", id, r, 2, 1);
                jb_divmod(B, xv, x, m, c, uns, false, jb_str(B, "def %%v%lld div", id, 0, 0));
            } else {                                             // x is the remainder, quotient fresh
                r[0].var = xv; r[0].c = 1; r[1].var = x; r[1].c = -1;
                int32_t q = jb_fresh(B, "quot", id, r, 2, c);
                jb_divmod(B, xv, q, x, c, uns, false, jb_str(B, "def %%v%lld rem", id, 0, 0));
            }
            break;
        }
        // shifts by a constant k: LSHR of an unsigned value and ASHR are a FLOORED division by 2^k
        // (I.179); SHL under CHECK is 2^k * x, its overflow its own obligation
        case IR_LSHR: case IR_ASHR: case IR_SHL: {
            __int128 k;
            if (d->n_operands < 2 || !jg_const_of(g, d->operands[1], &k) || !jg_is_int(d->operands[0]->type)) break;
            const IrType *xt = d->operands[0]->type;
            int w = xt->kind == IRT_BOOL ? 1 : xt->bits;
            if (k < 0 || k >= w || k > 62) break;
            int32_t xv = jb_val(B, d->operands[0]);
            __int128 p = (__int128)1 << k;
            if (d->op == IR_SHL) {
                if (d->wrap != IR_WRAP_CHECK || d->unchecked) break;
                t[0].var = xv; t[0].c = p;
                jb_def(B, x, t, 1, jb_str(B, "def %%v%lld shl", id, 0, 0));
                break;
            }
            if (d->op == IR_LSHR && !jg_uns(xt)) break;          // lowering picks LSHR for unsigned only
            JLT r[2]; r[0].var = xv; r[0].c = 1; r[1].var = x; r[1].c = -p;
            int32_t m = jb_fresh(B, "rem", id, r, 2, 1);
            jb_divmod(B, xv, x, m, p, false, true, jb_str(B, d->op == IR_LSHR ? "def %%v%lld lshr" : "def %%v%lld ashr", id, 0, 0));
            break;
        }
        // AND with a non-negative constant c: in [0, c] whatever x is (two's complement), and at
        // most x when x is unsigned (L-bit's exact half)
        case IR_AND: {
            __int128 c; int ci = -1;
            for (int k = 0; k < 2 && ci < 0; k++) if (d->n_operands >= 2 && jg_const_of(g, d->operands[k], &c) && c >= 0) ci = k;
            if (ci < 0) break;
            const char *o = jb_str(B, "def %%v%lld and", id, 0, 0);
            t[0].var = x; t[0].c = -1; jb_atom(B, o, t, 1, J_LE, 0);
            t[0].var = x; t[0].c = 1;  jb_atom(B, o, t, 1, J_LE, c);
            IrValue *xo = d->operands[1 - ci];
            if (jg_uns(xo->type)) { t[0].var = x; t[0].c = 1; t[1].var = jb_val(B, xo); t[1].c = -1; jb_atom(B, o, t, 2, J_LE, 0); }
            break;
        }
        // SIZEOF and ALIGNOF (P2.4), as VRA states them (vra.h, IR_SIZEOF): a type whose C spelling
        // has an exact size (an integer at its storage width, an enumeration, a [packed] struct or
        // sum, a vector, an array of those: ir_fixed_size) has that size, and its alignment is at
        // most it; any other type is the C ABI's, and only `>= 1` is claimed. layout.h is trusted
        // input; I.192 has the emitted C assert the fixed size against the C compiler.
        case IR_SIZEOF: case IR_ALIGNOF: {
            int64_t sz = d->aux.alloca_ty ? ir_fixed_size(d->aux.alloca_ty) : 0;
            const char *o = jb_str(B, d->op == IR_SIZEOF ? "def %%v%lld sizeof" : "def %%v%lld alignof", id, 0, 0);
            if (sz > 0 && d->op == IR_SIZEOF) { jb_defk(B, x, NULL, 0, sz, o); break; }
            t[0].var = x; t[0].c = -1; jb_atom(B, o, t, 1, J_LE, -1);
            if (sz > 0) { t[0].var = x; t[0].c = 1; jb_atom(B, o, t, 1, J_LE, sz); }
            break;
        }
        // bit counts: POPCOUNT in [0, w]; CTZ and CLZ in [0, w - 1], their argument owed nonzero
        case IR_POPCOUNT: case IR_CTZ: case IR_CLZ: {
            if (d->n_operands < 1 || !d->operands[0]->type || d->operands[0]->type->kind != IRT_INT) break;
            int w = d->operands[0]->type->bits;
            const char *o = jb_str(B, d->op == IR_POPCOUNT ? "def %%v%lld popcount" : "def %%v%lld bitcount", id, 0, 0);
            t[0].var = x; t[0].c = -1; jb_atom(B, o, t, 1, J_LE, 0);
            t[0].var = x; t[0].c = 1;  jb_atom(B, o, t, 1, J_LE, d->op == IR_POPCOUNT ? w : w - 1);
            break;
        }
        default: break;                                        // its range only
    }
}

// The comparison an ICMP states, true or false, over two variables; false if it is not stated
// exactly (an unsigned predicate with a signed operand that may be negative: §2's Q row).
static bool jb_cmp(JB *B, IrInstr *ic, bool truth, const char *origin) {
    if (!ic || ic->op != IR_ICMP || ic->n_operands < 2) return false;
    IrValue *a = ic->operands[0], *b = ic->operands[1];
    if (!jg_is_int(a->type) || !jg_is_int(b->type)) return false;
    IrCmp c = ic->aux.cmp;
    bool uns = c == IR_CMP_ULT || c == IR_CMP_ULE || c == IR_CMP_UGT || c == IR_CMP_UGE;
    for (int k = 0; k < 2 && uns; k++) {
        IrValue *o = ic->operands[k];
        if (o->type->kind == IRT_BOOL || !o->type->is_signed) continue;
        IrInstr *od = o->id >= 0 && o->id < B->g->nv ? B->g->def[o->id] : NULL;
        if (!(od && od->op == IR_CONST && jg_const(od) >= 0)) return false;
    }
    if (!truth) switch (c) {
        case IR_CMP_EQ: c = IR_CMP_NE; break;  case IR_CMP_NE: c = IR_CMP_EQ; break;
        case IR_CMP_SLT: case IR_CMP_ULT: c = IR_CMP_SGE; break;
        case IR_CMP_SLE: case IR_CMP_ULE: c = IR_CMP_SGT; break;
        case IR_CMP_SGT: case IR_CMP_UGT: c = IR_CMP_SLE; break;
        default: c = IR_CMP_SLT; break;
    }
    JLT t[2];
    t[0].var = jb_val(B, a); t[1].var = jb_val(B, b);
    switch (c) {
        case IR_CMP_EQ: t[0].c = 1; t[1].c = -1; jb_atom(B, origin, t, 2, J_EQ, 0); break;
        case IR_CMP_NE: {
            if (B->jS >= 0) return false;                      // a disjunction: not inside a disjunct
            JHyp *h = jb_hyp(B, origin);
            if (!h) return false;
            h->disj = true; h->ndis = 2;
            if (!(h->dis = JG_NEW(B->g, JConj, 2))) { B->fail = true; return false; }
            for (int d = 0; d < 2; d++) {
                if (!(h->dis[d].c = JG_NEW(B->g, JCons, 1))) { B->fail = true; return false; }
                h->dis[d].nc = 1;
                JLT u[2]; u[0] = t[0]; u[1] = t[1];
                u[0].c = d == 0 ? 1 : -1; u[1].c = d == 0 ? -1 : 1;
                jb_cons(B, u, 2, J_LE, -1, &h->dis[d].c[0]);
            }
            break;
        }
        case IR_CMP_SLT: case IR_CMP_ULT: t[0].c = 1;  t[1].c = -1; jb_atom(B, origin, t, 2, J_LE, -1); break;
        case IR_CMP_SLE: case IR_CMP_ULE: t[0].c = 1;  t[1].c = -1; jb_atom(B, origin, t, 2, J_LE, 0); break;
        case IR_CMP_SGT: case IR_CMP_UGT: t[0].c = -1; t[1].c = 1;  jb_atom(B, origin, t, 2, J_LE, -1); break;
        default:                          t[0].c = -1; t[1].c = 1;  jb_atom(B, origin, t, 2, J_LE, 0); break;
    }
    return true;
}
// Does a value's variable already exist in this VC (without creating one)?
static bool jb_has(JB *B, IrValue *v) {
    if (!v || v->id < 0 || v->id >= B->g->nv) return false;
    if (B->g->loadver[v->id] >= 0) return B->vver[B->g->loadver[v->id]] >= 0;
    if (B->vval[v->id] >= 0) return true;
    // a slice's `.len` read is its length: a test on `a.len` is about the variable len(a) a goal
    // names directly (`a[0]` under `if a.len >= 1`)
    IrInstr *d = B->g->def[v->id];
    if (d && d->op == IR_SLICE_LEN && d->n_operands >= 1 && d->operands[0] &&
        d->operands[0]->id >= 0 && d->operands[0]->id < B->g->nv) return B->vlen[d->operands[0]->id] >= 0;
    return false;
}
static void jb_drain(JB *B) { while (B->qh < B->qt && !B->fail) jb_state(B, B->queue[B->qh++]); }
// Is a condition's operand about the VC: in it, or computed exactly from what is (a guard on one
// `i + j` and an index computing `i + j` again are two SSA values with the same operands)? Depth 3.
static bool jb_rel(JB *B, IrValue *v, int depth) {
    if (jb_has(B, v)) return true;
    if (depth <= 0 || !v || v->id < 0 || v->id >= B->g->nv) return false;
    IrInstr *d = B->g->def[v->id];
    if (!d || !(d->op == IR_ADD || d->op == IR_SUB || d->op == IR_NEG || d->op == IR_MUL || d->op == IR_CAST)) return false;
    for (int k = 0; k < d->n_operands; k++) if (jb_rel(B, d->operands[k], depth - 1)) return true;
    return false;
}

// The facts that dominate the point: branch edges into single-predecessor blocks on the dominator
// chain, and ASSUMEs before the point. Each is added once one of its operands is in the VC, until
// nothing more joins: a condition about variables the goal never reaches costs only size.
// ★ A JOIN (R7). Control reached the join S through one of its predecessors, so ONE of these holds:
// for predecessor P, the conditions of the edges on its path from S's dominator, and, for each
// cell S versions afresh, `join version = the version P ends with`. Each is a disjunct, and a
// value or version made on that path is described inside it (jb_global_at). A loop header is not
// a join here (its back edge's values are a later iteration's: the loop cut decides it), and a
// path with no fact at all makes the disjunction say nothing, so none is stated.
static bool jb_join(JB *B, int32_t s) {
    JG *g = B->g; IrBlock *S = g->blk[s];
    IrBlock *pr[8]; int np = 0;
    for (IrEdge *e = S->preds; e; e = e->next) {
        if (g->rpoix[e->block->id] < 0) continue;
        if (g->rpoix[e->block->id] >= g->rpoix[s] || np == 8) return false;
        pr[np++] = e->block;
    }
    if (np < 2) return false;
    int32_t d0 = g->idom[s];
    JConj *dis = JG_NEW(g, JConj, np);
    if (!dis) { B->fail = true; return false; }
    for (int k = 0; k < np && !B->fail; k++) {
        B->jS = s; B->sink = NULL; B->nsink = B->csink = 0; B->dgen++;
        for (int32_t c = 0; c < g->ncells; c++) {
            int32_t jv = g->instate[(size_t)s * (size_t)g->ncells + (size_t)c];
            if (jv < 0 || g->ver[jv].kind != JV_JOIN || g->ver[jv].blk != s || B->vver[jv] < 0) continue;
            int32_t pv = g->outstate[(size_t)pr[k]->id * (size_t)g->ncells + (size_t)c];
            if (pv < 0) continue;
            JLT t[2]; t[0].var = B->vver[jv]; t[0].c = 1; t[1].var = jb_ver(B, pv); t[1].c = -1;
            jb_atom(B, NULL, t, 2, J_EQ, 0);
        }
        IrBlock *Y = S, *X = pr[k];
        for (int hop = 0; hop < 64; hop++) {
            if (X->term.kind == IR_TERM_BR_COND && X->term.a != X->term.b && X->term.cond &&
                X->term.cond->id >= 0 && X->term.cond->id < g->nv) {
                IrInstr *ic = g->def[X->term.cond->id];
                if (ic && ic->op == IR_ICMP) jb_cmp(B, ic, X->term.a == Y, "");
            }
            if (X->id == d0 || g->npred[X->id] != 1 || !X->preds) break;
            Y = X; X = X->preds->block;
        }
        dis[k].c = B->sink; dis[k].nc = B->nsink;
        B->jS = -1;
        if (dis[k].nc == 0) return false;
    }
    B->jS = -1;
    if (B->fail) return false;
    JHyp *h = jb_hyp(B, jb_str(B, "join bb%lld", s, 0, 0));
    if (!h) return false;
    h->disj = true; h->ndis = np; h->dis = dis;
    return true;
}
// Is the join S about the VC: a version it makes is a variable, or a condition into it tests one?
static bool jb_join_relevant(JB *B, int32_t s) {
    JG *g = B->g;
    for (int32_t c = 0; c < g->ncells; c++) {
        int32_t jv = g->instate[(size_t)s * (size_t)g->ncells + (size_t)c];
        if (jv >= 0 && g->ver[jv].kind == JV_JOIN && g->ver[jv].blk == s && B->vver[jv] >= 0) return true;
    }
    for (IrEdge *e = g->blk[s]->preds; e; e = e->next) {
        IrBlock *X = e->block;
        if (X->term.kind != IR_TERM_BR_COND || !X->term.cond || X->term.cond->id < 0 || X->term.cond->id >= g->nv) continue;
        IrInstr *ic = g->def[X->term.cond->id];
        if (ic && ic->op == IR_ICMP && (jb_rel(B, ic->operands[0], 3) || jb_rel(B, ic->operands[1], 3))) return true;
    }
    return false;
}

typedef struct { IrInstr *ic; bool truth; const char *origin; bool used; } JFact;
static void jb_facts(JB *B) {
    JG *g = B->g;
    int32_t nf = 0, cf = 16;
    JFact *F = JG_NEW(g, JFact, cf);
    int32_t J[64], nj = 0; bool jused[64];
    if (!F) { B->fail = true; return; }
    for (int32_t s = B->blk; ; s = g->idom[s]) {
        IrBlock *S = g->blk[s];
        if (g->npred[s] >= 2 && !g->inloop[(size_t)s * (size_t)g->nb + (size_t)s] && nj < 64) { jused[nj] = false; J[nj++] = s; }
        // ASSUMEs in S, before the point when S is the point's block
        int32_t k = 0;
        for (IrInstr *i = S->instrs; i; i = i->next, k++) {
            if (s == B->blk && k >= B->idx) break;
            if (i->op != IR_ASSUME || i->n_operands < 1 || !i->operands[0]) continue;
            IrInstr *ic = i->operands[0]->id >= 0 && i->operands[0]->id < g->nv ? g->def[i->operands[0]->id] : NULL;
            if (!ic || ic->op != IR_ICMP) continue;
            if (nf == cf) { JFact *n2 = JG_NEW(g, JFact, cf * 2); if (!n2) { B->fail = true; return; } memcpy(n2, F, sizeof *F * (size_t)nf); F = n2; cf *= 2; }
            F[nf].ic = ic; F[nf].truth = true; F[nf].used = false;
            F[nf].origin = jb_str(B, i->aux.imm == 1 ? "assume.trusted bb%lld.%lld" : "assume bb%lld.%lld", s, k, 0);
            nf++;
        }
        if (s == g->f->entry->id) break;
        if (g->npred[s] == 1 && S->preds) {
            IrBlock *P = S->preds->block;
            if (P->term.kind == IR_TERM_BR_COND && P->term.a != P->term.b && P->term.cond &&
                P->term.cond->id >= 0 && P->term.cond->id < g->nv) {
                IrInstr *ic = g->def[P->term.cond->id];
                if (ic && ic->op == IR_ICMP) {
                    if (nf == cf) { JFact *n2 = JG_NEW(g, JFact, cf * 2); if (!n2) { B->fail = true; return; } memcpy(n2, F, sizeof *F * (size_t)nf); F = n2; cf *= 2; }
                    F[nf].ic = ic; F[nf].truth = P->term.a == S; F[nf].used = false;
                    F[nf].origin = jb_str(B, "guard bb%lld->bb%lld", P->id, s, 0);
                    nf++;
                }
            }
        }
        if (g->idom[s] == s) break;
    }
    // a claim's obligation is owed on ONE edge out of its block: that edge's condition holds
    if (B->to >= 0) {
        IrBlock *P = g->blk[B->blk];
        if (P->term.kind == IR_TERM_BR_COND && P->term.a != P->term.b && P->term.cond &&
            P->term.cond->id >= 0 && P->term.cond->id < g->nv) {
            IrInstr *ic = g->def[P->term.cond->id];
            if (ic && ic->op == IR_ICMP) {
                if (nf == cf) { JFact *n2 = JG_NEW(g, JFact, cf * 2); if (!n2) { B->fail = true; return; } memcpy(n2, F, sizeof *F * (size_t)nf); F = n2; cf *= 2; }
                F[nf].ic = ic; F[nf].truth = P->term.a->id == B->to; F[nf].used = false;
                F[nf].origin = jb_str(B, "edge bb%lld->bb%lld", P->id, B->to, 0);
                nf++;
            }
        }
    }
    for (bool more = true; more && !B->fail; ) {
        more = false;
        jb_drain(B);
        for (int32_t i = 0; i < nf; i++) {
            if (F[i].used) continue;
            if (!jb_rel(B, F[i].ic->operands[0], 3) && !jb_rel(B, F[i].ic->operands[1], 3)) continue;
            F[i].used = true;
            if (jb_cmp(B, F[i].ic, F[i].truth, F[i].origin)) more = true;
        }
        for (int32_t i = 0; i < nj && !B->fail; i++) {
            if (jused[i]) continue;
            jb_drain(B);
            if (!jb_join_relevant(B, J[i])) continue;
            jused[i] = true;
            if (jb_join(B, J[i])) more = true;
        }
    }
    jb_drain(B);
}

static JB jb_begin(JG *g, int32_t blk, int32_t idx, int32_t self) {
    JB B; memset(&B, 0, sizeof B);
    B.g = g; B.blk = blk; B.idx = idx; B.self = self; B.jS = -1; B.to = -1;
    B.vval = JG_NEW(g, int32_t, g->nv); B.vlen = JG_NEW(g, int32_t, g->nv); B.vver = JG_NEW(g, int32_t, g->nver ? g->nver : 1);
    B.cst = JG_NEW(g, int32_t, g->ncl ? g->ncl : 1);
    if (!B.vval || !B.vlen || !B.vver || !B.cst) { B.fail = true; return B; }
    for (int32_t i = 0; i < g->nv; i++) B.vval[i] = B.vlen[i] = -1;
    for (int32_t i = 0; i < g->nver; i++) B.vver[i] = -1;
    for (int32_t i = 0; i < g->ncl; i++) B.cst[i] = -2;
    return B;
}
static bool jb_finish(JB *B, IrInstr *at, const char *kind, int ordinal, JCons *goal, JObl *o) {
    jb_facts(B);
    if (B->fail) return false;
    JG *g = B->g;
    memset(o, 0, sizeof *o);
    o->at = at; o->blk = g->blk[B->blk]; o->idx = B->idx;
    o->vc.id.fn = jb_str(B, "%s", 0, 0, 0);
    if (!o->vc.id.fn) return false;
    {   char *fn = JG_NEW(g, char, (size_t)g->f->name->length + 1);
        if (!fn) return false;
        memcpy(fn, g->f->name->name, (size_t)g->f->name->length); fn[g->f->name->length] = 0; o->vc.id.fn = fn; }
    o->vc.id.irhash = jb_str(B, "%08llx", (long long)g->hash, 0, 0);
    o->vc.id.point = jb_str(B, "bb%lld.%lld", B->blk, B->idx, 0);
    o->vc.id.kind = kind;
    o->vc.id.ordinal = jb_str(B, "%lld", ordinal, 0, 0);
    o->vc.vars = B->names; o->vc.nvars = B->nvars;
    o->vc.hyps = B->hyps; o->vc.nhyps = B->nhyps;
    o->vc.goal = *goal;
    // what each variable is: a version's key becomes its cell's ALLOCA value id
    {   int32_t *k2 = JG_NEW(g, int32_t, B->nvars ? B->nvars : 1);
        if (!k2) return false;
        for (int32_t i = 0; i < B->nvars; i++)
            k2[i] = B->kind[i] == 1 ? g->cellv[g->ver[B->key[i]].cell]->id : B->key[i];
        o->vkind = B->kind; o->vkey = k2; o->vtype = B->vtype;
        int32_t *aux = JG_NEW(g, int32_t, B->nvars ? B->nvars : 1);
        uint8_t *cur = JG_NEW(g, uint8_t, B->nvars ? B->nvars : 1);
        if (!aux || !cur) return false;
        for (int32_t i = 0; i < B->nvars; i++) {
            aux[i] = -1; cur[i] = 0;
            if (B->kind[i] != 1) continue;
            JVer *v = &g->ver[B->key[i]];
            if (v->kind == JV_STORE && jg_store_exact(v)) aux[i] = v->stored->id;
            // the cell's version at the point: the block's entry version, then its stores before it
            int32_t cv = g->instate[(size_t)B->blk * (size_t)g->ncells + (size_t)v->cell];
            for (int32_t j = 0; j < g->nver; j++)
                if (g->ver[j].kind == JV_STORE && g->ver[j].cell == v->cell && g->ver[j].blk == B->blk && g->ver[j].idx < B->idx) cv = j;
            cur[i] = cv == B->key[i];
        }
        o->vaux = aux; o->vcur = cur; o->vrec = B->vrec; }
    return !B->fail && o->vc.id.irhash && o->vc.id.point && o->vc.id.ordinal;
}

// ── the obligations of one function ─────────────────────────────────────────────────────────
typedef struct { JObl *o; int32_t n, c; } JObls;
static JObl *jg_push(JG *g, JObls *L) {
    if (L->n == L->c) {
        int32_t nc = L->c ? L->c * 2 : 16;
        JObl *no = JG_NEW(g, JObl, nc);
        if (!no) return NULL;
        if (L->n) memcpy(no, L->o, sizeof *no * (size_t)L->n);
        L->o = no; L->c = nc;
    }
    JObl *o = &L->o[L->n++]; memset(o, 0, sizeof *o); return o;
}
static bool jg_outside(JG *g, JObls *L, IrInstr *at, int32_t blk, int32_t idx, const char *why) {
    JObl *o = jg_push(g, L);
    if (!o) return jg_fail(g, "out of memory");
    o->at = at; o->blk = g->blk[blk]; o->idx = idx; o->outside = why;
    return true;
}
// One goal: a VC whose goal is `sum(t) op rhs`, built by `mk` over a fresh builder.
typedef bool (*JGoalFn)(JB *B, IrInstr *at, void *arg, JCons *goal);
static bool jg_goal(JG *g, JObls *L, IrInstr *at, int32_t blk, int32_t idx, int32_t self,
                    const char *kind, int ordinal, JGoalFn mk, void *arg) {
    JB B = jb_begin(g, blk, idx, self);
    JCons goal;
    if (B.fail || !mk(&B, at, arg, &goal)) return jg_fail(g, "out of memory building a goal");
    JObl *o = jg_push(g, L);
    if (!o || !jb_finish(&B, at, kind, ordinal, &goal, o)) return jg_fail(g, "out of memory building a VC");
    return true;
}

// The goals, one function each. `arg` carries the bound or the width.
typedef struct { int64_t w; int64_t clen; IrValue *slice; } JBoundArg;
static bool jgk_bounds_lo(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)arg; JLT t[1]; t[0].var = jb_val(B, at->operands[1]); t[0].c = -1;
    return jb_cons(B, t, 1, J_LE, 0, goal);
}
static bool jgk_bounds_hi(JB *B, IrInstr *at, void *arg, JCons *goal) {
    JBoundArg *a = arg; JLT t[2];
    t[0].var = jb_val(B, at->operands[1]); t[0].c = 1;
    if (a->slice) { t[1].var = jb_len(B, a->slice); t[1].c = -1; return jb_cons(B, t, 2, J_LE, -(__int128)a->w, goal); }
    return jb_cons(B, t, 1, J_LE, (__int128)a->clen - a->w, goal);
}
static bool jgk_divzero(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)arg; JLT t[1]; t[0].var = jb_val(B, at->operands[1]); t[0].c = 1;
    return jb_cons(B, t, 1, J_NE, 0, goal);
}
static bool jgk_nonzero(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)arg; JLT t[1]; t[0].var = jb_val(B, at->operands[0]); t[0].c = 1;
    return jb_cons(B, t, 1, J_NE, 0, goal);
}
static bool jgk_divovf(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)arg; JLT t[1];
    t[0].var = jb_val(B, at->operands[0]); t[0].c = 1;
    jb_atom(B, jb_str(B, "premise divovf %%v%lld", at->result ? at->result->id : -1, 0, 0), t, 1, J_EQ, -((__int128)1 << 63));
    t[0].var = jb_val(B, at->operands[1]); t[0].c = 1;
    return jb_cons(B, t, 1, J_NE, -1, goal);
}
typedef struct { __int128 bound; bool upper; IrValue *v; } JNarrowArg;
static bool jgk_narrow(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)at; JNarrowArg *a = arg; JLT t[1];
    t[0].var = jb_val(B, a->v);
    if (a->upper) { t[0].c = 1; return jb_cons(B, t, 1, J_LE, a->bound, goal); }
    t[0].c = -1; return jb_cons(B, t, 1, J_LE, -a->bound, goal);
}

static bool jg_obligations(JG *g, JObls *L) {
    for (int32_t r = 0; r < g->nrpo; r++) {
        IrBlock *b = g->blk[g->rpo[r]];
        // a subslice's start pointer is checked by its MAKE_SLICE, as VRA does (subslice_gep)
        int32_t k = 0;
        for (IrInstr *i = b->instrs; i; i = i->next, k++) {
            switch (i->op) {
                case IR_ELEM_PTR: {
                    if (i->unchecked || i->n_operands < 2 || !i->result) break;
                    bool sub = false;
                    for (IrInstr *j = i->next; j && !sub; j = j->next)
                        if (j->op == IR_MAKE_SLICE && j->n_operands >= 1 && j->operands[0] == i->result) sub = true;
                    if (sub) break;
                    IrValue *base = i->operands[0];
                    IrInstr *bd = base && base->id >= 0 && base->id < g->nv ? g->def[base->id] : NULL;
                    JBoundArg a; a.w = i->aux.elem_width > 0 ? i->aux.elem_width : 1; a.clen = -1; a.slice = NULL;
                    if (bd && bd->op == IR_ALLOCA && bd->aux.alloca_ty &&
                        (bd->aux.alloca_ty->kind == IRT_ARRAY || bd->aux.alloca_ty->kind == IRT_VECTOR))
                        a.clen = bd->aux.alloca_ty->array_len;
                    else if (base->type && (base->type->kind == IRT_ARRAY || base->type->kind == IRT_VECTOR))
                        a.clen = base->type->array_len;
                    else if (base->type && base->type->kind == IRT_PTR && base->type->elem && base->type->elem->kind == IRT_VECTOR)
                        a.clen = base->type->elem->array_len;
                    else if (bd && bd->op == IR_SLICE_DATA && bd->n_operands >= 1) a.slice = bd->operands[0];
                    if (!jg_is_int(i->operands[1]->type)) { if (!jg_outside(g, L, i, b->id, k, "an index that is not an integer")) return false; break; }
                    if (!jg_goal(g, L, i, b->id, k, -1, "bounds.lo", 0, jgk_bounds_lo, NULL)) return false;
                    if (a.clen < 0 && !a.slice) { if (!jg_outside(g, L, i, b->id, k, "no length is known for the base (a shape or a shared-term cancellation)")) return false; break; }
                    if (!jg_goal(g, L, i, b->id, k, -1, "bounds.hi", 0, jgk_bounds_hi, &a)) return false;
                    break;
                }
                case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: {
                    if (i->unchecked || i->n_operands < 2) break;
                    if (!jg_is_int(i->operands[0]->type) || !jg_is_int(i->operands[1]->type)) break;
                    if (!jg_goal(g, L, i, b->id, k, -1, "divzero", 0, jgk_divzero, NULL)) return false;
                    IrType *xt = i->operands[0]->type, *yt = i->operands[1]->type;
                    if (xt->kind == IRT_INT && xt->is_signed && xt->bits == 64 && yt->kind == IRT_INT && yt->is_signed)
                        if (!jg_goal(g, L, i, b->id, k, -1, "divovf", 0, jgk_divovf, NULL)) return false;
                    break;
                }
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_NEG: {
                    if (i->wrap != IR_WRAP_CHECK || i->unchecked || !i->result || !jg_is_int(i->result->type)) break;
                    JRange rr;
                    if (!jg_range(i->result->type, &rr)) break;
                    if (i->op == IR_MUL) {
                        bool c0 = i->operands[0] && g->def[i->operands[0]->id] && g->def[i->operands[0]->id]->op == IR_CONST;
                        bool c1 = i->operands[1] && g->def[i->operands[1]->id] && g->def[i->operands[1]->id]->op == IR_CONST;
                        if (!c0 && !c1) { if (!jg_outside(g, L, i, b->id, k, "atom.mul: a product of two variables has no lemma in step A")) return false; break; }
                    }
                    JNarrowArg lo = { rr.lo, false, i->result }, hi = { rr.hi, true, i->result };
                    if (!jg_goal(g, L, i, b->id, k, i->result->id, "narrow.lo", 0, jgk_narrow, &lo)) return false;
                    if (!jg_goal(g, L, i, b->id, k, i->result->id, "narrow.hi", 0, jgk_narrow, &hi)) return false;
                    break;
                }
                case IR_CAST: {
                    if (i->wrap != IR_WRAP_CHECK || i->unchecked || i->n_operands < 1 || !i->result) break;
                    // into an integer only: a cast to bool is `x != 0` and owes nothing (§3)
                    if (!i->result->type || i->result->type->kind != IRT_INT) break;
                    if (!jg_is_int(i->operands[0]->type)) {
                        if (i->operands[0]->type && i->operands[0]->type->kind == IRT_FLOAT)
                            if (!jg_outside(g, L, i, b->id, k, "a float converted to an integer (F1 and F2 are a later step)")) return false;
                        break;
                    }
                    JRange rr;
                    if (!jg_range(i->result->type, &rr)) break;
                    JNarrowArg lo = { rr.lo, false, i->operands[0] }, hi = { rr.hi, true, i->operands[0] };
                    if (!jg_goal(g, L, i, b->id, k, -1, "narrow.lo", 0, jgk_narrow, &lo)) return false;
                    if (!jg_goal(g, L, i, b->id, k, -1, "narrow.hi", 0, jgk_narrow, &hi)) return false;
                    break;
                }
                case IR_SHL: case IR_LSHR: case IR_ASHR: {
                    if (i->unchecked || i->n_operands < 2 || !i->result || !i->result->type ||
                        i->result->type->kind != IRT_INT || !jg_is_int(i->operands[1]->type)) break;
                    int w = i->operands[0]->type && i->operands[0]->type->kind == IRT_INT ? i->operands[0]->type->bits : 64;
                    JNarrowArg lo = { 0, false, i->operands[1] }, hi = { w - 1, true, i->operands[1] };
                    if (!jg_goal(g, L, i, b->id, k, -1, "shift", 0, jgk_narrow, &lo)) return false;
                    if (!jg_goal(g, L, i, b->id, k, -1, "shift", 1, jgk_narrow, &hi)) return false;
                    // SHL under CHECK may not lose a bit: its result settles into its type (ii_settle)
                    if (i->op == IR_SHL && i->wrap == IR_WRAP_CHECK) {
                        JRange rr;
                        if (!jg_range(i->result->type, &rr)) break;
                        JNarrowArg nlo = { rr.lo, false, i->result }, nhi = { rr.hi, true, i->result };
                        if (!jg_goal(g, L, i, b->id, k, i->result->id, "narrow.lo", 0, jgk_narrow, &nlo)) return false;
                        if (!jg_goal(g, L, i, b->id, k, i->result->id, "narrow.hi", 0, jgk_narrow, &nhi)) return false;
                    }
                    break;
                }
                case IR_CTZ: case IR_CLZ: {
                    // of 0 they fail (interp.h:710, "proven nonzero"): an obligation, not a range
                    if (i->unchecked || i->n_operands < 1 || !i->operands[0]->type || i->operands[0]->type->kind != IRT_INT) break;
                    if (!jg_goal(g, L, i, b->id, k, -1, "nonzero", 0, jgk_nonzero, NULL)) return false;
                    break;
                }
                default: break;
            }
        }
    }
    return true;
}

// ── claims (P3.5: the hull row, and `inv` at a header) ─────────────────────────────────────────
// A FORWARD join: two or more predecessors, every reachable one earlier in the RPO. No retreating
// edge enters it, so it heads no loop (a header's back edge comes from later in the RPO), and it
// is not the entry, which is entered with no edge. Its every entry is an edge a claim is owed on.
static bool jg_forward_join(JG *g, int32_t b) {
    if (b < 0 || b >= g->nb || !g->blk[b] || g->rpoix[b] < 0 || g->npred[b] < 2) return false;
    for (IrEdge *e = g->blk[b]->preds; e; e = e->next)
        if (g->rpoix[e->block->id] >= g->rpoix[b]) return false;
    return true;
}
// A loop header, for the proposer's listing: a natural loop's head (a slot there is `head`).
static bool jg_loop_header(JG *g, int32_t b) {
    return b >= 0 && b < g->nb && g->blk[b] && g->rpoix[b] >= 0 && g->inloop[(size_t)b * (size_t)g->nb + (size_t)b];
}
// A claim's term at block b: an integer cell (its version at b), an integer parameter, or an
// integer value defined in a block that strictly dominates b.
static bool jg_term_ok(JG *g, int32_t b, int32_t key) {
    if (key < 0 || key >= g->nv) return false;
    if (g->cell[key] >= 0) return jg_is_int(g->def[key]->aux.alloca_ty);
    IrValue *v = jg_value(g, key);
    if (!v || !jg_is_int(v->type)) return false;
    if (!g->def[key]) return true;                                // a parameter
    int32_t d = g->defblk[key];
    return d >= 0 && d != b && jg_dominates(g, d, b);
}
// Claims are untrusted input: one the generator cannot state AND owe is refused, never skipped.
static bool jg_claims_ok(JG *g) {
    const __int128 big = (__int128)1 << 66;
    for (int32_t ci = 0; ci < g->ncl; ci++) {
        const JClaim *c = &g->cl[ci];
        // a claim is owed on every edge into its block, so any reached block will do but the
        // function's entry, which is entered with no edge; one with a fresh version is a join or
        // a header (the `made` rule below)
        if (c->blk < 0 || c->blk >= g->nb || !g->blk[c->blk] || g->rpoix[c->blk] < 0 || c->blk == g->f->entry->id)
            return jg_fail(g, "a claim not at a block entered by edges only");
        if (c->nt < 1 || c->nt > 2) return jg_fail(g, "a claim of other than one or two terms");
        if (c->nt == 2 && c->key[0] == c->key[1]) return jg_fail(g, "a claim naming one term twice");
        if (c->rhs > big || c->rhs < -big) return jg_fail(g, "a claim's bound out of range");
        bool made = false;
        for (int k = 0; k < c->nt; k++) {
            int32_t key = c->key[k];
            if (!jg_term_ok(g, c->blk, key)) return jg_fail(g, "a claim's term is not a cell or a value fixed at its block");
            if (c->coef[k] == 0 || c->coef[k] > big || c->coef[k] < -big) return jg_fail(g, "a claim's coefficient out of range");
            if (g->cell[key] < 0) continue;
            int32_t jv = g->instate[(size_t)c->blk * (size_t)g->ncells + (size_t)g->cell[key]];
            if (jv >= 0 && (g->ver[jv].kind == JV_JOIN || g->ver[jv].kind == JV_HEAD) && g->ver[jv].blk == c->blk) made = true;
        }
        if (!made) return jg_fail(g, "a claim about no version its block makes afresh");
    }
    return true;
}
// Each claim's obligations (P3.5): on every edge P -> H into its block, at the end of P, the edge's
// condition a fact, the claim about the versions P ends with: `hull` into a join, `inv.entry` into
// a header from before its loop, `inv.keep` along a back edge (where H's own claims about its
// previous entry are hypotheses: the induction step). A predecessor that reaches H by both arms
// of its branch owes it once, with no edge condition.
static bool jg_claim_obligations(JG *g, JObls *L, int32_t only_blk) {
    for (int32_t ci = 0; ci < g->ncl; ci++) {
        const JClaim *c = &g->cl[ci];
        if (only_blk >= 0 && c->blk != only_blk) continue;
        IrBlock *H = g->blk[c->blk];
        for (IrEdge *e = H->preds; e; e = e->next) {
            IrBlock *P = e->block;
            if (g->rpoix[P->id] < 0) continue;
            bool again = false;
            for (IrEdge *e2 = H->preds; e2 != e; e2 = e2->next) if (e2->block == P) again = true;
            if (again) continue;
            int32_t n = 0;
            for (IrInstr *i = P->instrs; i; i = i->next) n++;
            JB B = jb_begin(g, P->id, n, -1);
            B.to = H->id;
            JLT t[2]; JCons goal;
            for (int k = 0; k < c->nt; k++) {
                t[k].var = jb_term(&B, c->key[k], g->outstate + (size_t)P->id * (size_t)g->ncells);
                t[k].c = c->coef[k];
            }
            if (B.fail || !jb_cons(&B, t, c->nt, J_LE, c->rhs, &goal)) return jg_fail(g, "out of memory building a claim");
            const char *kind = jg_forward_join(g, H->id) ? "hull" : g->rpoix[P->id] < g->rpoix[H->id] ? "inv.entry" : "inv.keep";
            JObl *o = jg_push(g, L);
            if (!o || !jb_finish(&B, NULL, kind, ci, &goal, o)) return jg_fail(g, "out of memory building a VC");
            o->to = H; o->claim = ci;
        }
    }
    return true;
}

// The obligations of `f`, their VCs in `m`: with `only` JGEN_ALL, every obligation and every
// claim's; JGEN_CLAIMS, the claims' only; a block id, the claims' at that block only. `cl` are
// claims at cut points (untrusted: see the header). False with `err` on an IR the generator does
// not read (a phi), a claim it cannot state and owe, or a region that is full.
enum { JGEN_ALL = -1, JGEN_CLAIMS = -2 };
static bool jgen_func(JMem *m, IrFunc *f, const JClaim *cl, int32_t ncl, int32_t only,
                      JObl **out, int32_t *nout, char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.err = err; g.errcap = errcap; err[0] = 0;
    g.cl = cl; g.ncl = cl ? ncl : 0;
    *out = NULL; *nout = 0;
    if (!f || !f->entry || f->is_extern) return g.ncl == 0 || jg_fail(&g, "a claim on a function without a body");
    if (!jg_structure(&g) || !jg_cells(&g) || !jg_claims_ok(&g)) return false;
    JObls L; memset(&L, 0, sizeof L);
    if (only == JGEN_ALL && !jg_obligations(&g, &L)) return false;
    if (!jg_claim_obligations(&g, &L, only >= 0 ? only : -1)) return false;
    *out = L.o; *nout = L.n;
    return true;
}

// For whoever proposes claims (nothing here is trusted: every claim is checked and owed). The
// versions a claim can be about: every forward join's fresh version of an integer cell, and every
// loop header's (`head`), in reverse post-order, so a block's forward predecessors come first.
typedef struct { int32_t blk, key; const IrType *type; bool head; } JSlot;
static bool jgen_slots(JMem *m, IrFunc *f, JSlot **out, int32_t *nout, char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.err = err; g.errcap = errcap; err[0] = 0;
    *out = NULL; *nout = 0;
    if (!f || !f->entry || f->is_extern) return true;
    if (!jg_structure(&g) || !jg_cells(&g)) return false;
    JSlot *S = NULL;
    for (int pass = 0; pass < 2; pass++) {                   // count, then fill
        int32_t n = 0;
        for (int32_t r = 0; r < g.nrpo; r++) {
            int32_t b = g.rpo[r];
            bool head = jg_loop_header(&g, b);
            if (!head && !jg_forward_join(&g, b)) continue;
            for (int32_t c = 0; c < g.ncells; c++) {
                int32_t jv = g.instate[(size_t)b * (size_t)g.ncells + (size_t)c];
                if (jv < 0 || g.ver[jv].blk != b || !jg_is_int(g.ver[jv].slot)) continue;
                if (g.ver[jv].kind != (head ? JV_HEAD : JV_JOIN)) continue;
                if (S) { S[n].blk = b; S[n].key = g.cellv[c]->id; S[n].type = g.ver[jv].slot; S[n].head = head; }
                n++;
            }
        }
        if (S) { *out = S; *nout = n; break; }
        if (!(S = JG_NEW(&g, JSlot, n ? n : 1))) return jg_fail(&g, "out of memory");
    }
    return true;
}
// The other terms a claim at block `blk` may relate a version to (as slots: key and type): every
// integer cell, and every integer value a comparison inside blk's loop (or in blk, at a join) tests
// that is fixed at blk. A constant is a bound's business, not a term's.
static bool jgen_terms(JMem *m, IrFunc *f, int32_t blk, JSlot **out, int32_t *nout, char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.err = err; g.errcap = errcap; err[0] = 0;
    *out = NULL; *nout = 0;
    if (!f || !f->entry || f->is_extern) return true;
    if (!jg_structure(&g) || !jg_cells(&g)) return false;
    if (blk < 0 || blk >= g.nb || !g.blk[blk]) return jg_fail(&g, "no such block");
    JSlot *K = JG_NEW(&g, JSlot, g.nv ? g.nv : 1); int32_t n = 0;
    uint8_t *seen = JG_NEW(&g, uint8_t, g.nv ? g.nv : 1);
    if (!K || !seen) return jg_fail(&g, "out of memory");
    memset(seen, 0, (size_t)g.nv);
    for (int32_t c = 0; c < g.ncells; c++) {
        int32_t id = g.cellv[c]->id;
        if (!jg_term_ok(&g, blk, id)) continue;
        K[n].blk = blk; K[n].key = id; K[n].type = g.def[id]->aux.alloca_ty; K[n].head = false; n++; seen[id] = 1;
    }
    const uint8_t *L = g.inloop + (size_t)blk * (size_t)g.nb;
    for (int32_t x = 0; x < g.nb; x++) {
        if (!g.blk[x] || !(L[blk] ? L[x] : x == blk)) continue;
        for (IrInstr *i = g.blk[x]->instrs; i; i = i->next) {
            if (i->op != IR_ICMP || i->n_operands < 2) continue;
            for (int k = 0; k < 2; k++) {
                IrValue *o = i->operands[k];
                if (!o || o->id < 0 || o->id >= g.nv || seen[o->id]) continue;
                if (g.def[o->id] && g.def[o->id]->op == IR_CONST) continue;
                if (!jg_term_ok(&g, blk, o->id)) continue;
                K[n].blk = blk; K[n].key = o->id; K[n].type = o->type; K[n].head = false; n++; seen[o->id] = 1;
            }
        }
    }
    *out = K; *nout = n;
    return true;
}

// For whoever proposes claims: per key, 0 when a claim at `blk` may not name it, 1 when it may (a
// cell's version, or a value fixed at blk), 2 when it is moreover a version blk makes afresh (a
// claim must name one). A candidate from elsewhere (VRA's header states) is filtered by it, since
// a claim the generator cannot state is refused with the whole function.
static bool jgen_term_kinds(JMem *m, IrFunc *f, int32_t blk, const int32_t *keys, int32_t n, uint8_t *kind,
                            char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.err = err; g.errcap = errcap; err[0] = 0;
    for (int32_t i = 0; i < n; i++) kind[i] = 0;
    if (!f || !f->entry || f->is_extern) return true;
    if (!jg_structure(&g) || !jg_cells(&g)) return false;
    if (blk < 0 || blk >= g.nb || !g.blk[blk]) return true;
    for (int32_t i = 0; i < n; i++) {
        if (!jg_term_ok(&g, blk, keys[i])) continue;
        kind[i] = 1;
        if (g.cell[keys[i]] < 0) continue;
        int32_t jv = g.instate[(size_t)blk * (size_t)g.ncells + (size_t)g.cell[keys[i]]];
        if (jv >= 0 && (g.ver[jv].kind == JV_JOIN || g.ver[jv].kind == JV_HEAD) && g.ver[jv].blk == blk) kind[i] = 2;
    }
    return true;
}

#endif // LAIN_JUDGE_GEN_H
