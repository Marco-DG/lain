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
//   - the version of an integer or slice CELL a load reads (P2.1: a local slot, or the pointee of
//     a checked reference parameter, and a constant field path inside it): a store's version
//     equals the value stored when its landing is exact (a constructor's field, for a whole
//     struct); a join's, a changed loop header's, and a havoc point's version (P2.2's frame rule,
//     at jg_cells) is a fresh variable; a cell the loop never changes keeps its version;
//   - the branch conditions, ASSUMEs and ASSERTs that dominate the point, as facts about the SSA
//     values they tested (an edge counts when its target has that one predecessor; an ASSERT's
//     condition is owed where it stands, so a program past it has it true); a condition that is
//     a bool value rather than a comparison is 1 where it held and 0 where it did not;
//   - at a join that dominates the point, one disjunct per predecessor (R7): the edge conditions
//     on its path, and the version each cell ends that path with;
//   - a CLAIM at a forward join (P3.5's hull row: `lo <= x@join`) or a loop header (its `inv`:
//     `j - i <= 0` at the header), about the versions the block makes, wherever such a version is
//     named. Claims are UNTRUSTED input, so each is also OWED on every edge into its block: `hull`
//     into a join, `inv.entry` and `inv.keep` into a header, at the end of the predecessor, with the
//     edge's condition a fact, about the versions the predecessor ends with (the cells' contents as
//     the block is entered).
//   - a callee's RETURN claim (its contract: `min(a, b) - b <= 0`), at a call to a function of the
//     same module (I.181), about the call's result and the arguments; owed as `post` at every
//     return of the callee, about the value returned and the parameters' entry values.
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
// Not stated yet, each costing precision only: element cells; a raw pointer's pointee (its loads
// are fresh variables); a loop invariant no claim proposes, or of five terms or more; another
// module's callee's contract; an ABI layout's size; a product of two variables (`outside`). The
// obligations mirror VRA's final pass for bounds, division, overflow, integer casts, shifts, bit
// counts, landings (P2.3) and ASSERTs (P2.6), so the two can be compared obligation by obligation.
//
// Checked: scripts/gates/judge_gate.sh evaluates every hypothesis at run time over every accepted
// program; src/tools/test_judge_gen.c pins the rules no program exercises.
//
// Constants are computed in __int128 and stored as JInt: every one is at most 2^65 in magnitude.
#include <string.h>
#include <stdio.h>
#include "ir/ir.h"
#include "ir/layout.h"     // SIZEOF's fixed sizes: a TRUSTED input (P2.4), the emitter's own decision
#include "ir/callgraph.h"  // the module's call graph and its recursion components: the set VRA reads
#include "judge/format.h"
#include "judge/mem.h"

// A FRESH variable's value, for a harness that evaluates a VC at run time: (sum of coef * var) / den,
// an exact division; or, with fval >= 0, the integral part of that FLOAT value (a conversion's,
// P3.3); or, with mm +1 (-1), the MAX (MIN) of the forms mmf holds (a ranking component's rows on
// one side of a trip, c41), mmf NULL where mm is 0; or, with wbits 1..64, the linear value WRAPPED
// into wbits bits, signed where wsgn, as it_wrap narrows it (a return's landed value, s8). The
// generator states its facts; the recipe only says which value they are about (the remainder of
// x / c is x - c*q).
// A form k: sum over i < fn[k] of fcoef[k][i] * fvar[k][i], plus fc0[k]; nf forms, 1 .. JRANK_MAXC.
typedef struct {
    int32_t nf; uint8_t fn[JRANK_MAXC]; int32_t fvar[JRANK_MAXC][JRANK_MAXT];
    __int128 fcoef[JRANK_MAXC][JRANK_MAXT], fc0[JRANK_MAXC];
} JMinMax;
typedef struct { int32_t n; int32_t var[4]; __int128 coef[4]; __int128 den; int32_t fval; int8_t mm; const JMinMax *mmf;
                 int8_t wbits; bool wsgn; } JRecipe;

// A claim at a cut point, `sum(coef * term) <= rhs`, over one to four terms by value id: a cell's
// ALLOCA id is the version the cell has as block `blk` is entered, any other id that integer value.
// One term at least is a version the block makes afresh, so it is a join or a loop header, and it
// is not the function's entry, the one block entered with no edge to owe the claim on
// (jg_claims_ok).
// A RETURN claim (blk JCLAIM_RET) is about what a function returns: its terms are the returned
// value (JKEY_RET) and the function's integer parameters (their entry values, which no
// reassignment changes: a parameter is an SSA value). It is owed at every return of its function
// (`post`), and is a fact about a call's result at a call in the SAME module (I.181's boundary:
// across modules only the declared contract, which reaches the caller as its ASSUMEs).
// `fn` is the claim's function; NULL means the one being generated. JClaim and its constants are
// format.h's, since an untrusted proposer writes claims.

// An obligation: its instruction, its position, and its VC; or `outside` with the reason. A
// claim's obligation has no instruction: it is owed at the end of block `blk`, on its edge to `to`,
// or, a return claim's (`to` NULL), as `blk` returns.
typedef struct {
    IrInstr *at; IrBlock *blk; int32_t idx;
    IrBlock *to; int32_t claim;               // a claim's obligation: the edge's target, the claim
    const int32_t *voff;                     // per var: a version of an element no address names, that
                                             // element's index past its array's root (vkey); else -1
    bool guarded; int32_t gvar; __int128 gmin; // a guarded EACH instance: owed only where var gvar >= gmin
    JVc vc;
    const char *outside;
    // per VC variable, what it is (for a harness that evaluates the VC at run time): kind 0 an SSA
    // value, 1 a cell's version, 2 a slice's length; key the value id, or for a version the cell's
    // ALLOCA value id; type the type to read it with (NULL for a length)
    const int32_t *vkind, *vkey; const IrType **vtype;
    // for a version: the SSA value its store wrote when the version equals it (-1 otherwise), and
    // whether it is the cell's version current at the point (a VC may name an older one)
    const int32_t *vaux; const uint8_t *vcur;
    // for an older version that is a loop's ENTRY version (its value on the edge E -> H into a loop
    // the point is in), H and E: a harness reads it from the cell as that edge was taken (-1 else);
    // for a header H's own version (JV_HEAD), H and E -1: the cell as H was last entered, by any edge
    const int32_t *vent, *vente;
    const JRecipe **vrec;                    // kind 3 (a fresh variable): its recipe
    // per hypothesis, for a harness that knows the path taken: an R7 join's block (-1 for any other
    // hypothesis), and the predecessor each of its disjuncts is the path through
    const int32_t *hjoin; const int32_t *const *hpred;
} JObl;

// ── JInt from a machine integer (the generator's side; the kernel has its own arithmetic) ──
// A JInt the generator made (below 2^100) back as an integer.
static __int128 jg_int_get(const JInt *x) {
    unsigned __int128 m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(__int128)m : (__int128)m;
}
static void jg_int(JInt *x, __int128 v) {
    memset(x, 0, sizeof *x);
    unsigned __int128 m = v < 0 ? (unsigned __int128)0 - (unsigned __int128)v : (unsigned __int128)v;
    x->neg = v < 0;
    while (m) { x->limb[x->n++] = (uint32_t)m; m >>= 32; }
}

// A type's integer range as the interpreter reads it (iv_get): an unsigned 64-bit value is in
// [0, 2^64 - 1], not clamped as irtype_int_range clamps it for the octagon. Refinement included.
typedef struct { __int128 lo, hi; bool has_ne; int64_t ne; } JRange;
// An integer type's range by its width alone (its refinement aside).
static __int128 jg_width_lo(const IrType *t) { return t->is_signed ? -((__int128)1 << (t->bits - 1)) : 0; }
static __int128 jg_width_hi(const IrType *t) {
    return t->is_signed ? ((__int128)1 << (t->bits - 1)) - 1 : ((__int128)1 << t->bits) - 1;
}
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
enum { JV_ENTRY, JV_STORE, JV_JOIN, JV_HEAD, JV_HAVOC };
// A HAVOC version made by a store through a variable index into an array of element cells (es: that
// store, ek: this element's index, eprev: its version before) is that element with or without the
// value stored (jg_elem_store); any other havoc version is es NULL.
typedef struct { int kind; int32_t cell, blk, idx; IrValue *stored; IrType *slot; bool unchecked;
                 IrInstr *es; int32_t ek, eprev; } JVer;
typedef struct {
    JMem *m; IrFunc *f; char *err; size_t errcap;
    IrFunc *mod;                   // the module: where callees are looked up (NULL: every call is unknown)
    int32_t nb, nv;
    IrBlock **blk;                 // by block id
    int32_t *rpo, nrpo, *rpoix;    // reverse post-order; rpoix[id] = position, -1 unreached
    int32_t *idom;                 // by block id; the entry's is itself
    int32_t *npred;
    IrInstr **def; int32_t *defblk, *defidx;   // by value id
    // P2.1's cells: a ROOT (a local slot, or the pointee of a checked reference parameter) and a
    // constant field path inside it, whose leaf is an integer or a slice
    int32_t *cell, ncells;         // by value id: the leaf cell an address is, or -1
    IrValue **cellv;               // by cell: its first address in the RPO (the root itself for a root leaf)
    uint8_t *csyn; int32_t *coff;  // by cell: an element no address names (cellv its array's root), its index
    int32_t *croot; const char **cpath; const IrType **cty; const char **cname;   // by cell
    int32_t *aroot;                // by value id: the root an attributable address is inside, or -1
    const char **apath;            // by value id: that address's field path (".1.0"; "" for the root)
    int32_t *iroot;                // by value id: the root an ELEMENT address is inside, or -1
    const char **ipath;            // by value id: for an element address, the path of the array it indexes
    int32_t *alen;                 // by value id: for an attributable address ending in `[k]`, that array's length
    int32_t *asame;                // by value id: the address a SLICE_DATA is (its slice's data), or -1
    uint8_t *iunsafe;              // by value id: that element address came through an unchecked index
    uint8_t *rclass;               // by value id, for a root: JR_LOCAL, JR_BORROWED or JR_LEAKED
    IrInstr **one; int32_t *oneblk, *oneidx;   // by ALLOCA value id: its one store (every other use a load)
    int32_t *eff_cell; IrValue **eff_val; uint8_t *eff_unc; int32_t neff, eff_gen; int32_t *eff_stamp;  // jg_changes
    JVer *ver; int32_t nver, cver;
    int32_t *instate;              // by block id * ncells: the version at the block's entry
    int32_t *outstate;             // by block id * ncells: the version at the block's exit
    int32_t *loadver;              // by value id: the version a LOAD reads, or -1
    struct JELoad **eload;         // by value id: a LOAD through a variable index into cells, or NULL
    uint8_t *inloop;               // by header id * nb: block in the header's natural loop
    uint8_t *writes;               // by header id * ncells: the loop stores the cell
    uint32_t hash;
    const JClaim *cl; int32_t ncl; // claims (untrusted input: each is owed on every edge into its block)
    int32_t *ltwin;                // by LOAD value id: its twin load's id (jg_load_twin), -1 none, -2 not asked
    uint8_t *wneed;                // by value id: a path's stored values and what they are made of (jb_join_wraps)
    const JGuardClaim *gc; int32_t ngc;   // guarded return claims (untrusted input, owed as return claims are)
    int8_t wraps;                  // s5: f has a wrapping ADD or SUB of two or three cases (2), none (1), not asked yet (0)
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

// A LOAD through a CHECKED variable index into a fixed array of at most JELEM_MAXN elements, each
// a cell: the version each element has at the load, and the index. The value read is one of them,
// the one the index names (jb_state's IR_LOAD row): the load runs only where its bounds hold.
enum { JELEM_MAXN = 16 };
typedef struct JELoad { int32_t n; int32_t ver[JELEM_MAXN]; IrValue *ix; } JELoad;
static const IrType *jg_pointee(JG *g, int32_t id);
static JELoad *jg_elem_load(JG *g, IrValue *a, const int32_t *o) {
    IrInstr *d = g->def[a->id];
    if (!d || d->op != IR_ELEM_PTR || d->n_operands < 2 || !d->operands[0] || d->unchecked || g->iunsafe[a->id]) return NULL;
    IrValue *base = d->operands[0];
    if (base->id < 0 || base->id >= g->nv || g->aroot[base->id] < 0) return NULL;
    const IrType *t = jg_pointee(g, base->id);
    if (!t || t->kind != IRT_ARRAY || t->array_len < 1 || t->array_len > JELEM_MAXN) return NULL;
    JELoad *el = JG_NEW(g, JELoad, 1);
    if (!el) return NULL;
    el->n = (int32_t)t->array_len; el->ix = d->operands[1];
    for (int32_t k = 0; k < el->n; k++) {
        char pth[256]; snprintf(pth, sizeof pth, "%s[%lld]", g->apath[base->id], (long long)k);
        int32_t c = -1;
        for (int32_t c2 = 0; c2 < g->ncells && c < 0; c2++)
            if (g->croot[c2] == g->aroot[base->id] && !strcmp(g->cpath[c2], pth)) c = c2;
        if (c < 0 || !jg_is_int(g->cty[c])) return NULL;      // an element that is no cell: nothing stated
        el->ver[k] = o[c];
    }
    return el;
}
// A STORE through a CHECKED variable index into a fixed array of at most JELEM_MAXN elements, the
// value landing exactly in each integer cell among them (c10's rule for a load, mirrored): cells[k]
// is element k's cell, or -1. Its havoc version of each is that element, or the value stored where
// the index names it (jb_state); each on its own, so an element that is no cell takes nothing from
// the others. 0 where any of that fails: the store's versions are plain havoc.
static bool jg_lands_exact(const IrType *from, const IrType *to, bool waived);   // below
static int32_t jg_elem_store(JG *g, IrInstr *st, int32_t *cells) {
    IrValue *a = st->n_operands >= 2 ? st->operands[0] : NULL;
    IrInstr *d = a && a->id >= 0 && a->id < g->nv ? g->def[a->id] : NULL;
    if (!d || d->op != IR_ELEM_PTR || d->n_operands < 2 || !d->operands[0] || d->unchecked || g->iunsafe[a->id]) return 0;
    IrValue *base = d->operands[0];
    if (base->id < 0 || base->id >= g->nv || g->aroot[base->id] < 0) return 0;
    const IrType *t = jg_pointee(g, base->id);
    if (!t || t->kind != IRT_ARRAY || t->array_len < 1 || t->array_len > JELEM_MAXN) return 0;
    for (int32_t k = 0; k < (int32_t)t->array_len; k++) {
        char pth[256]; snprintf(pth, sizeof pth, "%s[%lld]", g->apath[base->id], (long long)k);
        int32_t c = -1;
        for (int32_t c2 = 0; c2 < g->ncells && c < 0; c2++)
            if (g->croot[c2] == g->aroot[base->id] && !strcmp(g->cpath[c2], pth)) c = c2;
        if (c >= 0 && (!jg_is_int(g->cty[c]) || !jg_lands_exact(st->operands[1]->type, g->cty[c], st->unchecked))) return 0;
        cells[k] = c;
    }
    return (int32_t)t->array_len;
}
static bool jg_pushver(JG *g, int kind, int32_t cell, int32_t blk, int32_t idx, IrValue *stored, bool unchecked) {
    if (g->nver == g->cver) {
        JVer *nv = JG_NEW(g, JVer, g->cver * 2);
        if (!nv) return jg_fail(g, "out of memory");
        memcpy(nv, g->ver, sizeof *nv * (size_t)g->nver); g->ver = nv; g->cver *= 2;
    }
    JVer *v = &g->ver[g->nver++];
    v->kind = kind; v->cell = cell; v->blk = blk; v->idx = idx; v->stored = stored; v->unchecked = unchecked;
    v->slot = (IrType *)g->cty[cell];
    v->es = NULL; v->ek = v->eprev = -1;
    return true;
}

// ── cells and their versions (P2.1) ─────────────────────────────────────────────────────────
// A ROOT is a local slot (an ALLOCA of a fixed type, not a module table) or the pointee of a
// parameter that is a CHECKED reference (a pointer type that is not raw). A CELL is a root and a
// constant field and element path inside it whose leaf is an integer or a slice: `l.pos` through
// `l var Lexer` is the parameter's root and path ".1", `a[2]` of a local `i32[4]` path "[2]". Every
// address an access names is ATTRIBUTABLE (the root, or FIELD_PTRs and ELEM_PTRs by a constant
// index within a fixed array's length from it), an ELEMENT address inside one (an ELEM_PTR by any
// other index, or a FIELD_PTR from one: it may name any element of the array it indexes), or
// neither.
//
// A root's ESCAPE CLASS (P2.1) is read from every use of an address inside it: LOCAL when each is
// an access (a LOAD's or a STORE's address) or forms another address inside it; BORROWED when some
// are also call arguments; LEAKED otherwise (a store of the address as a value, a reference
// binding's included, a return, a cast, a constructor), or a call whose RESULT may carry a raw
// pointer: `func get(var c C) *var u32 { return &c.val }` hands out an address that outlives the
// borrow, and a store through it changes c.val with no reference in sight. A call that returns a
// REFERENCE into its argument needs no rule of its own: it was handed a mutable reference, so the
// fields are fresh after it, and while that reference lives the struct is neither read nor
// written by name (E004), so no fact about it can outlive a write through it. What changes a cell
// (P2.2's frame rule), each a fresh version unless a store's value is known:
//   - a store to the cell, or to a whole struct around it (a constructor's field is its value);
//   - a call handed a MUTABLE reference to it or to a struct around it (a read-only reference
//     cannot write it, E004);
//   - for a BORROWED or LEAKED root, a call whose callee may run unmodelled writes (its effect
//     row's write bit), is not known, or is indirect, and an opaque that writes;
//   - for a LEAKED root, every call;
//   - for a LEAKED root and a PARAMETER's pointee, a store through an address that is not
//     attributable, and a call handed a value that may carry a RAW pointer (a raw pointer, or a
//     struct or reference holding one). A raw pointer is checked by nothing: the caller may have
//     taken one to the very storage a `var` parameter borrows (`var py = &y; kept(var y, py)`).
//     A local root's address, never taken as a value, no raw pointer can name.
// A call handed no reference to a root cannot write it: Lain has no stored references (E126) and
// no mutable globals, and a `var` parameter's pointee is reachable by references only through its
// exclusive borrow (E004, which binds references, not raw addresses). Those are TRUSTED premises,
// as the translation table lists them; an extern's signature and effect row are believed. A store
// through an element address, checked, may change any cell of the array it indexes (its `ipath`);
// one through an unchecked index may write anywhere in its root.
enum { JR_NONE, JR_LOCAL, JR_BORROWED, JR_LEAKED };

static IrFunc *jg_callee(const JG *g, IrInstr *i) {
    if (i->op != IR_CALL || !i->aux.callee) return NULL;
    for (IrFunc *c = g->mod; c; c = c->next)
        if (c->name && c->name->length == i->aux.callee->length &&
            !memcmp(c->name->name, i->aux.callee->name, (size_t)c->name->length)) return c;
    return NULL;
}
// The type an attributable address points at, or NULL.
static const IrType *jg_pointee(JG *g, int32_t id) {
    IrInstr *d = g->def[id];
    if (!d) { IrValue *v = jg_value(g, id); return v && v->type && v->type->kind == IRT_PTR ? v->type->elem : NULL; }
    if (d->op == IR_ALLOCA) return d->aux.alloca_ty;
    if (d->op == IR_FIELD_PTR && d->result && d->result->type)
        return d->result->type->kind == IRT_PTR ? d->result->type->elem : d->result->type;   // an array field decays
    if (d->op == IR_ELEM_PTR && d->result && d->result->type && d->result->type->kind == IRT_PTR)
        return d->result->type->elem;
    if (d->op == IR_LOAD && g->aroot[id] >= 0 && d->result && d->result->type && d->result->type->kind == IRT_PTR)
        return d->result->type->elem;                          // a reference binding's address (jg_cells)
    if (d->op == IR_SLICE_DATA && g->asame[id] >= 0) return jg_pointee(g, g->asame[id]);   // the slice's data
    return NULL;
}
static bool jg_const_of(JG *g, IrValue *v, __int128 *c);
// An ELEM_PTR's index as a constant within the fixed array its base points at: its element is a
// cell of that path (P2.1). Any other index names an element the judge cannot attribute.
static bool jg_elem_const(JG *g, int32_t base, IrInstr *i, __int128 *k) {
    const IrType *t = jg_pointee(g, base);
    return i->n_operands >= 2 && t && t->kind == IRT_ARRAY && t->array_len > 0 && jg_const_of(g, i->operands[1], k) &&
           *k >= 0 && *k < (__int128)t->array_len;
}
static bool jg_leaf(const IrType *t) { return t && (jg_is_int(t) || t->kind == IRT_SLICE); }
// Does path `p` lie inside path `q` (q a prefix at a field boundary)?
static bool jg_inside(const char *p, const char *q) {
    size_t n = strlen(q);
    return !strncmp(p, q, n) && (p[n] == 0 || p[n] == '.' || p[n] == '[');
}
static void jg_eff(JG *g, int32_t c, IrValue *val, bool unc) {
    if (g->eff_stamp[c] == g->eff_gen) return;                // one change per cell per instruction
    g->eff_stamp[c] = g->eff_gen;
    g->eff_cell[g->neff] = c; g->eff_val[g->neff] = val; g->eff_unc[g->neff] = unc; g->neff++;
}
static void jg_havoc_root(JG *g, int32_t r, const char *under) {
    for (int32_t c = 0; c < g->ncells; c++) if (g->croot[c] == r && jg_inside(g->cpath[c], under)) jg_eff(g, c, NULL, false);
}
static void jg_havoc_class(JG *g, int cls) {
    for (int32_t c = 0; c < g->ncells; c++) if (g->rclass[g->croot[c]] >= cls) jg_eff(g, c, NULL, false);
}
// What a raw pointer may name: a LEAKED root, and a parameter's pointee.
static void jg_havoc_rawable(JG *g) {
    for (int32_t c = 0; c < g->ncells; c++) {
        int32_t r = g->croot[c];
        if (g->rclass[r] >= JR_LEAKED || !g->def[r]) jg_eff(g, c, NULL, false);
    }
}
// May a value of this type carry a RAW pointer, itself or in what it holds or refers to? Too
// deep a type is assumed to.
static bool jg_carries_raw(const IrType *t, int depth) {
    if (!t) return false;
    if (depth > 8) return true;
    switch (t->kind) {
        case IRT_PTR: return t->is_raw || jg_carries_raw(t->elem, depth + 1);
        case IRT_SLICE: case IRT_ARRAY: case IRT_VECTOR: return jg_carries_raw(t->elem, depth + 1);
        case IRT_STRUCT: case IRT_SUM:
            for (int k = 0; k < t->n_fields; k++) if (jg_carries_raw(t->fields[k], depth + 1)) return true;
            return false;
        default: return false;
    }
}
// The part of a stored value at a path below the store's own: a constructor's field, recursively.
// A part lands in its field at its constructor, so `waived` becomes that constructor's (P2.3).
// An ELEMENT of a stored value (a `[k]` step) is not stated: the part is unknown, a fresh version.
static IrValue *jg_part(JG *g, IrValue *v, const char *rest, bool *waived) {
    while (*rest == '.') {
        IrInstr *d = v && v->id >= 0 && v->id < g->nv ? g->def[v->id] : NULL;
        if (!d || d->op != IR_STRUCT_NEW) return NULL;
        int k = atoi(rest + 1);
        if (k < 0 || k >= d->n_operands) return NULL;
        v = d->operands[k]; *waived = d->unchecked;
        rest++;
        while (*rest >= '0' && *rest <= '9') rest++;           // past this step's field index
    }
    return *rest ? NULL : v;
}
// The cells instruction `i` may change, into g->eff_*: each with the value a store makes it, or
// NULL for a fresh version (P2.2's frame rule, as above).
static void jg_changes(JG *g, IrInstr *i) {
    g->neff = 0; g->eff_gen++;
    if (i->op == IR_STORE && i->n_operands >= 2 && i->operands[0] && i->operands[0]->id >= 0 && i->operands[0]->id < g->nv) {
        int32_t a = i->operands[0]->id;
        if (g->aroot[a] >= 0) {
            for (int32_t c = 0; c < g->ncells; c++) {
                if (g->croot[c] != g->aroot[a] || !jg_inside(g->cpath[c], g->apath[a])) continue;
                bool waived = i->unchecked;
                IrValue *part = jg_part(g, i->operands[1], g->cpath[c] + strlen(g->apath[a]), &waived);
                jg_eff(g, c, part, waived);
            }
        } else if (g->iroot[a] >= 0) {                        // any element of the array it indexes
            jg_havoc_root(g, g->iroot[a], g->iunsafe[a] ? "" : g->ipath[a]);
        } else jg_havoc_rawable(g);
        return;
    }
    if (i->op == IR_OPAQUE) { if (i->aux.opaque.writes) jg_havoc_class(g, JR_BORROWED); return; }
    if (i->op != IR_CALL) return;
    IrFunc *cf = jg_callee(g, i);
    int first = i->aux.callee ? 0 : 1;                         // an indirect call's op[0] is its callee
    IrParam *pp = cf ? cf->params : NULL;
    bool raw = false;
    for (int k = first; k < i->n_operands; k++, pp = pp ? pp->next : NULL) {
        IrValue *o = i->operands[k];
        if (!o || o->id < 0 || o->id >= g->nv) continue;
        if (jg_carries_raw(o->type, 0)) raw = true;
        const IrType *pt = pp && pp->value ? pp->value->type : NULL;
        bool readonly = pt && pt->kind == IRT_PTR && !pt->is_raw && !pt->ptr_mut;
        if (readonly) continue;
        if (g->aroot[o->id] >= 0) jg_havoc_root(g, g->aroot[o->id], g->apath[o->id]);
        else if (g->iroot[o->id] >= 0) jg_havoc_root(g, g->iroot[o->id], g->iunsafe[o->id] ? "" : g->ipath[o->id]);
    }
    if (!cf || (cf->effects & IR_EFFECT_UNMODELLED_WRITE)) jg_havoc_class(g, JR_BORROWED);
    jg_havoc_class(g, JR_LEAKED);
    if (raw) jg_havoc_rawable(g);
}
// A local FIXED array of 1 to JELEM_MAXN integers, a root (a module table is none, nor a dynamic
// array): each of its elements is a cell, named by an address or not (jg_cells). The cap bounds the
// cells a large array would cost; every reader of element cells caps at JELEM_MAXN itself.
static bool jg_syn_array(JG *g, IrInstr *i) {
    const IrType *t = i->op == IR_ALLOCA ? i->aux.alloca_ty : NULL;
    return t && i->n_operands == 0 && i->result && i->result->id >= 0 && i->result->id < g->nv &&
           g->aroot[i->result->id] == i->result->id && t->kind == IRT_ARRAY && t->array_len >= 1 &&
           t->array_len <= JELEM_MAXN && jg_is_int(t->elem);
}
enum { JELEM_BIG = 1 << 30 };
// For a run-time reading only (vkey, voff): an element cell named by an address, `a[k]` of a fixed
// integer array that is an ALLOCA, k constant. The harness reads it from the array's own address and
// k, as an element no address names (csyn): the address value itself may be made only later in the
// function (`buf[10] = 97` after the loop that fills buf), where a loop's VCs cannot read it.
static bool jg_elem_at(JG *g, int32_t c, int32_t *root, int32_t *k) {
    int32_t r = g->croot[c];
    IrValue *rv = r >= 0 && r < g->nv ? jg_value(g, r) : NULL;
    IrInstr *d = rv && rv->id >= 0 && rv->id < g->nv ? g->def[rv->id] : NULL;
    const IrType *t = d && d->op == IR_ALLOCA && d->n_operands == 0 ? d->aux.alloca_ty : NULL;
    if (!t || t->kind != IRT_ARRAY || !jg_is_int(t->elem) || g->aroot[r] != r || !g->cpath[c]) return false;
    const char *p = g->cpath[c];                               // exactly `[k]`, as jg_cells spells it
    int64_t kk = 0;
    if (*p++ != '[' || *p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9' && kk < JELEM_BIG) kk = kk * 10 + (*p++ - '0');
    if (*p++ != ']' || *p != 0 || kk >= (int64_t)t->array_len) return false;
    *root = r; *k = (int32_t)kk;
    return true;
}
static bool jg_cells(JG *g) {
    IrFunc *f = g->f;
    int32_t nv = g->nv;
    if (!(g->aroot = JG_NEW(g, int32_t, nv)) || !(g->apath = JG_NEW(g, const char *, nv)) ||
        !(g->iroot = JG_NEW(g, int32_t, nv)) || !(g->iunsafe = JG_NEW(g, uint8_t, nv)) || !(g->rclass = JG_NEW(g, uint8_t, nv)) ||
        !(g->ipath = JG_NEW(g, const char *, nv)) || !(g->alen = JG_NEW(g, int32_t, nv)) || !(g->asame = JG_NEW(g, int32_t, nv)))
        return jg_fail(g, "out of memory");
    for (int32_t i = 0; i < nv; i++) {
        g->aroot[i] = g->iroot[i] = g->asame[i] = -1; g->apath[i] = g->ipath[i] = NULL; g->iunsafe[i] = 0; g->rclass[i] = JR_NONE;
        g->alen[i] = 0;
    }
    // roots: checked reference parameters, then fixed local slots
    for (IrParam *p = f->params; p; p = p->next) {
        IrValue *v = p->value;
        if (!v || v->id < 0 || v->id >= nv || !v->type || v->type->kind != IRT_PTR || v->type->is_raw || !v->type->elem) continue;
        g->aroot[v->id] = v->id; g->apath[v->id] = ""; g->rclass[v->id] = JR_LOCAL;
    }
    // a REFERENCE BINDING (`var r = var n`): a local slot whose every use is a load or its one
    // store, of an address. Every load the store dominates is that address: `r = 2` writes n
    IrInstr **one = g->one = JG_NEW(g, IrInstr *, nv);
    int32_t *oneblk = g->oneblk = JG_NEW(g, int32_t, nv), *oneidx = g->oneidx = JG_NEW(g, int32_t, nv);
    if (!one || !oneblk || !oneidx) return jg_fail(g, "out of memory");
    for (int32_t i = 0; i < nv; i++) { one[i] = NULL; oneblk[i] = -1; oneidx[i] = 0; }
    for (int32_t r = 0; r < g->nrpo; r++) {
        int32_t k = 0;
        for (IrInstr *i = g->blk[g->rpo[r]]->instrs; i; i = i->next, k++)
            for (int q = 0; q < i->n_operands; q++) {
                IrValue *o = i->operands[q];
                if (!o || o->id < 0 || o->id >= nv || !g->def[o->id] || g->def[o->id]->op != IR_ALLOCA) continue;
                if (i->op == IR_LOAD && q == 0) continue;
                if (i->op == IR_STORE && q == 0 && oneblk[o->id] == -1) { one[o->id] = i; oneblk[o->id] = g->rpo[r]; oneidx[o->id] = k; continue; }
                oneblk[o->id] = -2; one[o->id] = NULL;         // another use, or a second store
            }
    }
    // addresses, over the RPO (an operand's definition comes first)
    for (int32_t r = 0; r < g->nrpo; r++)
        for (IrInstr *i = g->blk[g->rpo[r]]->instrs; i; i = i->next) {
            if (!i->result || i->result->id < 0 || i->result->id >= nv) continue;
            int32_t id = i->result->id;
            if (i->op == IR_ALLOCA && i->aux.alloca_ty && !i->data && i->n_operands == 0) {
                g->aroot[id] = id; g->apath[id] = ""; g->rclass[id] = JR_LOCAL; continue;
            }
            IrValue *b0 = i->n_operands >= 1 ? i->operands[0] : NULL;
            if (!b0 || b0->id < 0 || b0->id >= nv) continue;
            if (i->op == IR_LOAD && one[b0->id] && one[b0->id]->n_operands >= 2) {
                IrValue *av = one[b0->id]->operands[1];
                int32_t sb = oneblk[b0->id], lb = g->rpo[r], li = 0;
                for (IrInstr *j = g->blk[lb]->instrs; j && j != i; j = j->next) li++;
                bool dom = sb == lb ? oneidx[b0->id] < li : jg_dominates(g, sb, lb);
                if (dom && av && av->id >= 0 && av->id < nv && g->aroot[av->id] >= 0) {
                    g->aroot[id] = g->aroot[av->id]; g->apath[id] = g->apath[av->id]; g->alen[id] = g->alen[av->id];
                    continue;
                }
            }
            // a slice's DATA (interp.h: SLICE_DATA is the pointer its MAKE_SLICE took), the slice made
            // here or loaded from a slot stored once, where that store dominates: the address it took
            if (i->op == IR_SLICE_DATA) {
                IrInstr *ms = g->def[b0->id];
                if (ms && ms->op == IR_LOAD && ms->n_operands >= 1 && ms->operands[0] && ms->operands[0]->id >= 0 &&
                    ms->operands[0]->id < nv && one[ms->operands[0]->id] && one[ms->operands[0]->id]->n_operands >= 2) {
                    int32_t sl = ms->operands[0]->id, sb = oneblk[sl], lb = -1, li = 0;
                    for (int32_t q = 0; q < g->nrpo && lb < 0; q++) {
                        li = 0;
                        for (IrInstr *j = g->blk[g->rpo[q]]->instrs; j; j = j->next, li++) if (j == ms) { lb = g->rpo[q]; break; }
                    }
                    IrValue *sv = one[sl]->operands[1];
                    bool dom = lb >= 0 && (sb == lb ? oneidx[sl] < li : jg_dominates(g, sb, lb));
                    ms = dom && sv && sv->id >= 0 && sv->id < nv ? g->def[sv->id] : NULL;
                }
                IrValue *dp = ms && ms->op == IR_MAKE_SLICE && ms->n_operands >= 1 ? ms->operands[0] : NULL;
                if (dp && dp->id >= 0 && dp->id < nv && g->aroot[dp->id] >= 0) {
                    g->aroot[id] = g->aroot[dp->id]; g->apath[id] = g->apath[dp->id]; g->alen[id] = g->alen[dp->id];
                    g->asame[id] = dp->id;
                }
                continue;
            }
            // an ELEM_PTR from the address of an ELEMENT `[k]` moves along that array (interp.h adds the
            // index to the pointer's last step): by a constant j to `[k + j]` while inside its length;
            // by any other index it is an element address of that array (the path before `[k]`)
            const char *ap = g->aroot[b0->id] >= 0 ? g->apath[b0->id] : NULL, *lk = ap ? strrchr(ap, '[') : NULL;
            char *ke = NULL;
            long long k0 = lk ? strtoll(lk + 1, &ke, 10) : 0;
            if (i->op == IR_ELEM_PTR && lk && ke && ke[0] == ']' && ke[1] == 0 && g->alen[b0->id] > 0) {
                __int128 kj;
                size_t m = (size_t)(lk - ap);
                if (i->n_operands >= 2 && jg_const_of(g, i->operands[1], &kj) && k0 + kj >= 0 && k0 + kj < g->alen[b0->id]) {
                    char buf[48]; int n = snprintf(buf, sizeof buf, "[%lld]", (long long)(k0 + kj));
                    char *pth = JG_NEW(g, char, m + (size_t)n + 1);
                    if (!pth) return jg_fail(g, "out of memory");
                    memcpy(pth, ap, m); memcpy(pth + m, buf, (size_t)n + 1);
                    g->aroot[id] = g->aroot[b0->id]; g->apath[id] = pth; g->alen[id] = g->alen[b0->id];
                } else {
                    char *pth = JG_NEW(g, char, m + 1);
                    if (!pth) return jg_fail(g, "out of memory");
                    memcpy(pth, ap, m); pth[m] = 0;
                    g->iroot[id] = g->aroot[b0->id]; g->ipath[id] = pth; g->iunsafe[id] = i->unchecked;
                }
                continue;
            }
            __int128 kx;
            if ((i->op == IR_FIELD_PTR && g->aroot[b0->id] >= 0) ||
                (i->op == IR_ELEM_PTR && g->aroot[b0->id] >= 0 && jg_elem_const(g, b0->id, i, &kx))) {
                char buf[48]; int n = i->op == IR_FIELD_PTR ? snprintf(buf, sizeof buf, ".%lld", (long long)i->aux.field_idx)
                                                           : snprintf(buf, sizeof buf, "[%lld]", (long long)kx);
                size_t m = strlen(g->apath[b0->id]);
                char *pth = JG_NEW(g, char, m + (size_t)n + 1);
                if (!pth) return jg_fail(g, "out of memory");
                memcpy(pth, g->apath[b0->id], m); memcpy(pth + m, buf, (size_t)n + 1);
                g->aroot[id] = g->aroot[b0->id]; g->apath[id] = pth;
                if (i->op == IR_ELEM_PTR) g->alen[id] = (int32_t)jg_pointee(g, b0->id)->array_len;
            } else if ((i->op == IR_FIELD_PTR || i->op == IR_ELEM_PTR) && (g->aroot[b0->id] >= 0 || g->iroot[b0->id] >= 0)) {
                g->iroot[id] = g->aroot[b0->id] >= 0 ? g->aroot[b0->id] : g->iroot[b0->id];
                g->ipath[id] = g->aroot[b0->id] >= 0 ? g->apath[b0->id] : g->ipath[b0->id];
                g->iunsafe[id] = g->iunsafe[b0->id] || (i->op == IR_ELEM_PTR && i->unchecked);
            }
        }
    // escape classes, from every use of an address inside a root
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (g->rpoix[b->id] < 0) continue;
        for (IrInstr *i = b->instrs; i; i = i->next)
            for (int k = 0; k < i->n_operands; k++) {
                IrValue *o = i->operands[k];
                if (!o || o->id < 0 || o->id >= nv) continue;
                int32_t r = g->aroot[o->id] >= 0 ? g->aroot[o->id] : g->iroot[o->id];
                if (r < 0) continue;
                if ((i->op == IR_LOAD || i->op == IR_STORE || i->op == IR_FIELD_PTR || i->op == IR_ELEM_PTR) && k == 0) continue;
                // a FACT about a place (definite init, a shape, a borrow's scope, a move): no result,
                // no run-time effect (interp.h, emit_c.h), so its operand hands the address to no one
                if (i->op == IR_INIT || i->op == IR_SHAPE || i->op == IR_BORROW || i->op == IR_BORROW_END || i->op == IR_CONSUME) continue;
                if (i->op == IR_CALL && !(i->aux.callee == NULL && k == 0) &&
                    !(i->result && jg_carries_raw(i->result->type, 0))) {
                    if (g->rclass[r] < JR_BORROWED) g->rclass[r] = JR_BORROWED;
                    continue;
                }
                g->rclass[r] = JR_LEAKED;
            }
        IrValue *tc = b->term.cond;
        if (tc && tc->id >= 0 && tc->id < nv) {
            int32_t r = g->aroot[tc->id] >= 0 ? g->aroot[tc->id] : g->iroot[tc->id];
            if (r >= 0) g->rclass[r] = JR_LEAKED;
        }
    }
    // leaf cells: one per (root, path) an attributable address names; and one per element of a
    // local fixed array of at most JELEM_MAXN integers that no address names (read and written only
    // through a variable index): a cell is a location the versions follow, an address or not
    g->ncells = 0;
    int32_t nsyn = 0;
    for (int32_t r = 0; r < g->nrpo; r++)
        for (IrInstr *i = g->blk[g->rpo[r]]->instrs; i; i = i->next)
            if (jg_syn_array(g, i)) nsyn += (int32_t)i->aux.alloca_ty->array_len;
    int32_t ncap = nv + nsyn ? nv + nsyn : 1;
    if (!(g->cellv = JG_NEW(g, IrValue *, ncap)) || !(g->croot = JG_NEW(g, int32_t, ncap)) ||
        !(g->cpath = JG_NEW(g, const char *, ncap)) || !(g->cty = JG_NEW(g, const IrType *, ncap)) ||
        !(g->cname = JG_NEW(g, const char *, ncap)) || !(g->csyn = JG_NEW(g, uint8_t, ncap)) || !(g->coff = JG_NEW(g, int32_t, ncap)))
        return jg_fail(g, "out of memory");
    memset(g->csyn, 0, (size_t)ncap);
    for (int32_t pass = 0; pass < 2; pass++)                    // the parameters, then the RPO
        for (int32_t q = 0; q < (pass ? g->nrpo : 1); q++) {
            int32_t ids[64]; int32_t n = 0;
            if (!pass) { for (IrParam *p = f->params; p && n < 64; p = p->next) if (p->value) ids[n++] = p->value->id; }
            for (IrInstr *i = pass ? g->blk[g->rpo[q]]->instrs : NULL; i; i = i->next) {
                if (!i->result || i->result->id < 0 || i->result->id >= nv || g->aroot[i->result->id] < 0) continue;
                int32_t id = i->result->id;
                const IrType *t = jg_pointee(g, id);
                if (!jg_leaf(t)) continue;
                int32_t c = -1;
                for (int32_t c2 = 0; c2 < g->ncells && c < 0; c2++)
                    if (g->croot[c2] == g->aroot[id] && !strcmp(g->cpath[c2], g->apath[id])) c = c2;
                if (c < 0) {
                    c = g->ncells++;
                    g->cellv[c] = i->result; g->croot[c] = g->aroot[id]; g->cpath[c] = g->apath[id]; g->cty[c] = t;
                    char nm[64]; snprintf(nm, sizeof nm, "%%s%lld%s", (long long)g->aroot[id], g->apath[id]);
                    char *s2 = JG_NEW(g, char, strlen(nm) + 1);
                    if (!s2) return jg_fail(g, "out of memory");
                    strcpy(s2, nm); g->cname[c] = s2;
                }
                g->cell[id] = c;
            }
            for (int32_t k = 0; k < n; k++) {
                int32_t id = ids[k];
                if (id < 0 || id >= nv || g->aroot[id] < 0) continue;
                const IrType *t = jg_pointee(g, id);
                if (!jg_leaf(t)) continue;
                int32_t c = g->ncells++;
                g->cellv[c] = jg_value(g, id); g->croot[c] = id; g->cpath[c] = ""; g->cty[c] = t;
                char nm[32]; snprintf(nm, sizeof nm, "%%s%lld", (long long)id);
                char *s2 = JG_NEW(g, char, strlen(nm) + 1);
                if (!s2) return jg_fail(g, "out of memory");
                strcpy(s2, nm); g->cname[c] = s2;
                g->cell[id] = c;
            }
        }
    for (int32_t r = 0; r < g->nrpo; r++)                       // the elements no address names
        for (IrInstr *i = g->blk[g->rpo[r]]->instrs; i; i = i->next) {
            if (!jg_syn_array(g, i)) continue;
            int32_t id = i->result->id;
            for (int32_t k = 0; k < (int32_t)i->aux.alloca_ty->array_len; k++) {
                char pth[32]; snprintf(pth, sizeof pth, "[%lld]", (long long)k);
                bool have = false;                     // one cell per location: lookups take the first
                for (int32_t c2 = 0; c2 < g->ncells && !have; c2++) have = g->croot[c2] == id && !strcmp(g->cpath[c2], pth);
                if (have) continue;
                int32_t c = g->ncells++;
                char *ps = JG_NEW(g, char, strlen(pth) + 1), nm[64];
                snprintf(nm, sizeof nm, "%%s%lld%s", (long long)id, pth);
                char *s2 = JG_NEW(g, char, strlen(nm) + 1);
                if (!ps || !s2) return jg_fail(g, "out of memory");
                strcpy(ps, pth); strcpy(s2, nm);
                g->cellv[c] = i->result; g->croot[c] = id; g->cpath[c] = ps; g->cty[c] = i->aux.alloca_ty->elem; g->cname[c] = s2;
                g->csyn[c] = 1; g->coff[c] = k;
            }
        }
    if (!(g->eff_cell = JG_NEW(g, int32_t, g->ncells ? g->ncells : 1)) || !(g->eff_val = JG_NEW(g, IrValue *, g->ncells ? g->ncells : 1)) ||
        !(g->eff_unc = JG_NEW(g, uint8_t, g->ncells ? g->ncells : 1)) || !(g->eff_stamp = JG_NEW(g, int32_t, g->ncells ? g->ncells : 1)))
        return jg_fail(g, "out of memory");
    for (int32_t c = 0; c < g->ncells; c++) g->eff_stamp[c] = 0;
    g->eff_gen = 0;
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
    // the loop cut (D1 A2): a header versions afresh a cell its loop may change, by a store or a
    // havoc point: the same jg_changes the versions below are made from
    for (int32_t h = 0; h < g->nb; h++) {
        uint8_t *L = g->inloop + (size_t)h * (size_t)g->nb;
        if (!L[h]) continue;
        for (int32_t x = 0; x < g->nb; x++) {
            if (!L[x] || !g->blk[x]) continue;
            for (IrInstr *i = g->blk[x]->instrs; i; i = i->next) {
                jg_changes(g, i);
                for (int32_t e = 0; e < g->neff; e++) g->writes[(size_t)h * (size_t)g->ncells + (size_t)g->eff_cell[e]] = 1;
            }
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
            if (i->op == IR_LOAD && i->result && a && a->id >= 0 && a->id < g->nv && g->cell[a->id] >= 0) {
                g->loadver[i->result->id] = o[g->cell[a->id]];
                continue;
            }
            if (i->op == IR_LOAD && i->result && a && a->id >= 0 && a->id < g->nv && g->iroot[a->id] >= 0 && jg_is_int(i->result->type)) {
                JELoad *el = jg_elem_load(g, a, o);
                if (el && !g->eload) {
                    if (!(g->eload = JG_NEW(g, JELoad *, g->nv))) return jg_fail(g, "out of memory");
                    for (int32_t v = 0; v < g->nv; v++) g->eload[v] = NULL;
                }
                if (el) g->eload[i->result->id] = el;
                continue;                                      // a load changes nothing
            }
            jg_changes(g, i);
            int32_t ecell[JELEM_MAXN], en = i->op == IR_STORE ? jg_elem_store(g, i, ecell) : 0;
            for (int32_t e = 0; e < g->neff; e++) {
                int32_t c = g->eff_cell[e], prev = o[c];
                if (!jg_pushver(g, g->eff_val[e] ? JV_STORE : JV_HAVOC, c, b->id, k, g->eff_val[e], g->eff_unc[e])) return false;
                o[c] = g->nver - 1;
                for (int32_t q = 0; q < en && !g->eff_val[e]; q++)
                    if (ecell[q] == c) { g->ver[o[c]].es = i; g->ver[o[c]].ek = q; g->ver[o[c]].eprev = prev; }
            }
        }
    }
    return true;
}

// ── one VC ─────────────────────────────────────────────────────────────────────────────────
typedef struct { int32_t var; __int128 c; } JLT;
typedef struct {
    JG *g; int32_t blk, idx;                 // the obligation's point
    int32_t *vval, *vver, *vlen, *vlenv;     // var of a value, a version, a slice's length, a version's; -1 none
    const char **names; int32_t nvars, cvars;
    int32_t *kind, *key;                     // per var: 0 value, 1 version, 2 length, 3 fresh, 4 a slice
                                             // cell version's length; its key
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
    int32_t *jhyp, *jblk, **jpred; int32_t njr, cjr;
    bool allrel;                   // a goal of no variable that is false: every dominating fact is about it   // R7 hypotheses: index, join, disjuncts' predecessors
    int32_t *cst;                            // per claim: -2 unstated, -1 VC-wide, else its disjunct
    const int32_t *wid, *wk; int32_t nw;     // the current disjunct's path: each wrapping op's case k (s5)
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
    JVer *v = &B->g->ver[ver]; const char *cn = B->g->cname[v->cell];
    bool local = !jb_global_at(B, v->kind == JV_ENTRY ? -1 : v->blk);
    if (B->vver[ver] >= 0) return local ? jb_local(B, B->vver[ver], false) : B->vver[ver];
    char nm[96];
    if (v->kind == JV_ENTRY)      snprintf(nm, sizeof nm, "%s@entry", cn);
    else if (v->kind == JV_STORE) snprintf(nm, sizeof nm, "%s@bb%lld.%lld", cn, (long long)v->blk, (long long)v->idx);
    else if (v->kind == JV_JOIN)  snprintf(nm, sizeof nm, "%s@join.bb%lld", cn, (long long)v->blk);
    else if (v->kind == JV_HEAD)  snprintf(nm, sizeof nm, "%s@head.bb%lld", cn, (long long)v->blk);
    else                          snprintf(nm, sizeof nm, "%s@havoc.bb%lld.%lld", cn, (long long)v->blk, (long long)v->idx);
    char *n = JG_NEW(B->g, char, strlen(nm) + 1);
    if (n) strcpy(n, nm); else B->fail = true;
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
// The length of a slice cell's version `ver`. It names the version too, so a join's or a header's
// claims, those about its length among them, are stated wherever the length is (jb_state).
static int32_t jb_verlen(JB *B, int32_t ver) {
    JVer *v = &B->g->ver[ver];
    bool vl = !jb_global_at(B, v->kind == JV_ENTRY ? -1 : v->blk);
    if (B->vlenv[ver] >= 0) return vl ? jb_local(B, B->vlenv[ver], false) : B->vlenv[ver];
    int32_t vv = jb_ver(B, ver);                             // its name, and its own facts
    if (B->fail) return 0;
    char nm[128]; snprintf(nm, sizeof nm, "len(%s)", B->names[vv]);
    char *n = JG_NEW(B->g, char, strlen(nm) + 1);
    if (n) strcpy(n, nm); else { B->fail = true; return 0; }
    B->vlenv[ver] = jb_newvar(B, n, 4, ver, NULL);
    return vl ? jb_local(B, B->vlenv[ver], true) : B->vlenv[ver];
}
static int32_t jb_len(JB *B, IrValue *s);
// The length of a slice cell at version `ver`, named as a load of it names it (jb_len): a STORE
// version is the slice stored, length and all, so the guard's `s.len` and a claim's term about the
// same cell are one variable.
static int32_t jb_cell_len(JB *B, int32_t ver) {
    JVer *v = &B->g->ver[ver];
    if (v->kind == JV_STORE && v->stored && v->stored->type && v->stored->type->kind == IRT_SLICE) return jb_len(B, v->stored);
    return jb_verlen(B, ver);
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
    // a load of a slice cell's other version: every load of that version has its length
    if (B->g->loadver[s->id] >= 0) return jb_verlen(B, B->g->loadver[s->id]);
    bool local = !jb_global_at(B, B->g->defblk[s->id]);
    if (B->vlen[s->id] >= 0) return local ? jb_local(B, B->vlen[s->id], false) : B->vlen[s->id];
    B->vlen[s->id] = jb_newvar(B, jb_str(B, "len(%%v%lld)", s->id, 0, 0), 2, s->id, NULL);
    return local ? jb_local(B, B->vlen[s->id], true) : B->vlen[s->id];
}
// A constraint from terms: sorted by var (declaration order), merged, zero terms dropped. The
// sort is IN PLACE: the caller's t is reordered, so a reused t needs each .var set with its .c.
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
// or { c[0, n0) | c[n0, n0 + n1) }, each a conjunction; nothing inside a disjunct (it cannot
// hold a disjunction)
static void jb_or2(JB *B, const char *origin, const JCons *c, int n0, int n1) {
    if (B->jS >= 0) return;
    JHyp *h = jb_hyp(B, origin);
    if (!h) return;
    h->disj = true; h->ndis = 2;
    if (!(h->dis = JG_NEW(B->g, JConj, 2))) { B->fail = true; return; }
    for (int d = 0; d < 2; d++) {
        int n = d == 0 ? n0 : n1;
        if (!(h->dis[d].c = JG_NEW(B->g, JCons, n))) { B->fail = true; return; }
        h->dis[d].nc = n;
        memcpy(h->dis[d].c, c + (d == 0 ? 0 : n0), sizeof *c * (size_t)n);
    }
}
// or { c[0] | c[1] | … | c[n - 1] }, n >= 2, each disjunct one constraint; nothing inside a disjunct
static void jb_orn(JB *B, const char *origin, const JCons *c, int n) {
    if (B->jS >= 0) return;
    JHyp *h = jb_hyp(B, origin);
    if (!h) return;
    h->disj = true; h->ndis = n;
    if (!(h->dis = JG_NEW(B->g, JConj, n))) { B->fail = true; return; }
    for (int d = 0; d < n; d++) {
        if (!(h->dis[d].c = JG_NEW(B->g, JCons, 1))) { B->fail = true; return; }
        h->dis[d].nc = 1; h->dis[d].c[0] = c[d];
    }
}
static void jb_type(JB *B, int32_t x, const IrType *ty, const char *what, long long id) {
    JRange r; char tn[64];
    if (!jg_range(ty, &r)) return;
    jg_type_name(ty, tn, sizeof tn);
    char o[160];
    if (id < 0) snprintf(o, sizeof o, "type %s %s", what, tn);            // a cell, by its name
    else snprintf(o, sizeof o, "type %s%lld %s", what, id, tn);
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

// Does a STORE keep the value it stores? When its landing is exact (jg_lands_exact): the landing
// obligation at the store (jg_land) licenses it; a waived one into a plain slot wraps (ii_land).
static bool jg_store_exact(const JVer *v) {
    return v->stored && v->slot && jg_lands_exact(v->stored->type, v->slot, v->unchecked);
}

// A fresh variable `<what>.%v<id>` whose value is (t[0..n) + 0) / den. It is stated nothing about
// by itself; the row that makes it states its facts. Not queued: it has no definition to state.
static int32_t jb_fresh(JB *B, const char *what, int32_t id, const JLT *t, int n, __int128 den) {
    char nm[48]; snprintf(nm, sizeof nm, "%s.%%v%d", what, id);
    char *s = JG_NEW(B->g, char, strlen(nm) + 1);
    JRecipe *r = JG_NEW(B->g, JRecipe, 1);
    if (!s || !r) { B->fail = true; return 0; }
    strcpy(s, nm);
    r->n = n; r->den = den; r->fval = -1; r->mm = 0; r->mmf = NULL; r->wbits = 0; r->wsgn = false;
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
// lowers its divisor as `neg 4`), which is -c wherever its own obligation held, as NEG's row says;
// or a CAST of a constant operand into an integer type whose bits hold its value (`2 *? y` widens
// both operands to i64 and multiplies there: cast(2) * cast(y)), which is that value under every
// mode a cast has. The type's refinement is not read: it is not what a cast computes. Four casts
// deep at most (an IR whose cast reads itself must not recurse for ever).
static bool jg_const_at(JG *g, IrValue *v, __int128 *c, int depth) {
    IrInstr *d = v && v->id >= 0 && v->id < g->nv ? g->def[v->id] : NULL;
    if (d && d->op == IR_CAST && d->n_operands >= 1 && d->result && d->result->type &&
        d->result->type->kind == IRT_INT && d->result->type->bits >= 1 && d->result->type->bits <= 64) {
        const IrType *t = d->result->type; __int128 x;           // the source's kind: its own case decides
        if (depth >= 4 || !jg_const_at(g, d->operands[0], &x, depth + 1)) return false;
        __int128 lo = t->is_signed ? -((__int128)1 << (t->bits - 1)) : 0;
        __int128 hi = t->is_signed ? ((__int128)1 << (t->bits - 1)) - 1 : ((__int128)1 << t->bits) - 1;
        if (x < lo || x > hi) return false;
        *c = x; return true;
    }
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
static bool jg_const_of(JG *g, IrValue *v, __int128 *c) { return jg_const_at(g, v, c, 0); }
static bool jg_mul_ok(__int128 a, __int128 b, __int128 *out);
static IrInstr *jg_cond_cmp(JG *g, IrValue *cv);
static IrValue *jg_param_value(const IrFunc *h, int32_t key);
// A parameter's REFINEMENT as its lowering states it: an ASSUME comparing it with a constant in
// the entry block's PROLOGUE (nothing but constants, comparisons and assumes before it), which
// holds at every point of the function (a parameter is one value, and the prologue runs first).
// Narrows r; a predicate it cannot read narrows nothing.
static void jg_param_refine(JG *g, IrValue *v, JRange *r) {
    for (IrInstr *i = g->f->entry ? g->f->entry->instrs : NULL; i; i = i->next) {
        if (i->op == IR_CONST || i->op == IR_ICMP) continue;
        if (i->op != IR_ASSUME) return;                        // the prologue ends here
        IrInstr *ic = i->n_operands ? jg_cond_cmp(g, i->operands[0]) : NULL;
        __int128 c;
        if (!ic || ic->n_operands < 2) continue;
        int side = ic->operands[0] == v ? 0 : ic->operands[1] == v ? 1 : -1;
        if (side < 0 || !jg_const_of(g, ic->operands[1 - side], &c)) continue;
        IrCmp p = ic->aux.cmp;
        bool uns = p == IR_CMP_ULT || p == IR_CMP_ULE || p == IR_CMP_UGT || p == IR_CMP_UGE;
        if (uns && (c < 0 || !v->type || v->type->kind != IRT_INT || v->type->is_signed)) continue;   // as values: both >= 0
        if (side == 1)                                         // c op v is v op' c
            p = p == IR_CMP_SLT ? IR_CMP_SGT : p == IR_CMP_SLE ? IR_CMP_SGE : p == IR_CMP_SGT ? IR_CMP_SLT : p == IR_CMP_SGE ? IR_CMP_SLE
              : p == IR_CMP_ULT ? IR_CMP_UGT : p == IR_CMP_ULE ? IR_CMP_UGE : p == IR_CMP_UGT ? IR_CMP_ULT : p == IR_CMP_UGE ? IR_CMP_ULE : p;
        switch (p) {
            case IR_CMP_EQ: if (c > r->lo) r->lo = c; if (c < r->hi) r->hi = c; break;
            case IR_CMP_SGE: case IR_CMP_UGE: if (c > r->lo) r->lo = c; break;
            case IR_CMP_SGT: case IR_CMP_UGT: if (c + 1 > r->lo) r->lo = c + 1; break;
            case IR_CMP_SLE: case IR_CMP_ULE: if (c < r->hi) r->hi = c; break;
            case IR_CMP_SLT: case IR_CMP_ULT: if (c - 1 < r->hi) r->hi = c - 1; break;
            default: break;
        }
    }
}
// ── a float converted to an integer (P3.3): F1 and F2 ──────────────────────────────────────────
// A double's integral part, exactly: c = +-M * 2^e from its bits, never through a C conversion.
// False for a NaN, an infinity, or |c| >= 2^99; `integral` whether c is that integer.
static bool jg_ftrunc(double c, __int128 *t, bool *integral) {
    uint64_t u; memcpy(&u, &c, sizeof u);
    int ex = (int)((u >> 52) & 0x7ff);
    uint64_t m = u & ((1ULL << 52) - 1);
    __int128 v; bool in;
    if (ex == 0x7ff) return false;
    if (ex == 0) { v = 0; in = m == 0; }                       // zero or subnormal: |c| < 1
    else {
        int e2 = ex - 1075;
        m |= 1ULL << 52;
        if (e2 >= 0) { if (e2 > 46) return false; v = (__int128)m << e2; in = true; }
        else if (e2 <= -64) { v = 0; in = false; }
        else { v = (__int128)(m >> -e2); in = (m & ((1ULL << -e2) - 1)) == 0; }
    }
    *t = (u >> 63) ? -v : v; *integral = in;
    return true;
}
// F1: a float CONSTANT's value, at its IR type (interp.h's ii_fround: an f32 one rounds through
// float): a literal, a negated one, or a float slot's one store of one where that store dominates
// the load (c11's rule; the constant is one value whichever execution stored it).
static bool jg_fconst(JG *g, IrValue *v, double *c, int depth) {
    IrInstr *d = v && v->id >= 0 && v->id < g->nv ? g->def[v->id] : NULL;
    if (!d || depth > 4 || !v->type || v->type->kind != IRT_FLOAT) return false;
    if (d->op == IR_CONST) { *c = v->type->float_bits == 32 ? (double)(float)d->aux.fimm : d->aux.fimm; return true; }
    if (d->op == IR_NEG && d->n_operands >= 1 && jg_fconst(g, d->operands[0], c, depth + 1)) { *c = -*c; return true; }
    if (d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0] && g->one) {
        int32_t sl = d->operands[0]->id;
        if (sl < 0 || sl >= g->nv || !g->one[sl] || g->one[sl]->n_operands < 2) return false;
        int32_t sb = g->oneblk[sl], lb = g->defblk[v->id];
        bool dom = lb >= 0 && (sb == lb ? g->oneidx[sl] < g->defidx[v->id] : jg_dominates(g, sb, lb));
        return dom && jg_fconst(g, g->one[sl]->operands[1], c, depth + 1);
    }
    return false;
}
// F2: a condition TRUE at the point about float value x: a comparison of x itself with a float
// constant (either way round), ordered, so its truth excludes NaN (interp.h's ii_cmp_scalar: C's
// <, <=, >, >=, ==; a `!=` holds for a NaN and bounds nothing), or an `and` of such. trunc is
// monotone: x >= c gives trunc(x) >= trunc(c), and x > c the same but trunc(c) + 1 for an integral
// c < 0; x <= c gives trunc(x) <= trunc(c), and x < c the same but c - 1 for an integral c > 0.
static void jg_f2_bound(IrCmp p, double c, JRange *r, bool *hl, bool *hh) {
    __int128 t; bool in;
    if (!jg_ftrunc(c, &t, &in)) return;
    __int128 lo = t, hi = t;
    bool L = false, H = false;
    switch (p) {
        case IR_CMP_EQ: L = H = true; break;
        case IR_CMP_SGE: case IR_CMP_UGE: L = true; break;
        case IR_CMP_SGT: case IR_CMP_UGT: L = true; if (in && t < 0) lo = t + 1; break;
        case IR_CMP_SLE: case IR_CMP_ULE: H = true; break;
        case IR_CMP_SLT: case IR_CMP_ULT: H = true; if (in && t > 0) hi = t - 1; break;
        default: return;                                       // `!=`
    }
    if (L && (!*hl || lo > r->lo)) { r->lo = lo; *hl = true; }
    if (H && (!*hh || hi < r->hi)) { r->hi = hi; *hh = true; }
}
static void jg_f2_cond(JG *g, IrValue *cv, IrValue *x, JRange *r, bool *hl, bool *hh, int depth) {
    IrInstr *d = cv && cv->id >= 0 && cv->id < g->nv ? g->def[cv->id] : NULL;
    if (!d || depth > 4) return;
    if (d->op == IR_AND && d->n_operands >= 2 && cv->type && cv->type->kind == IRT_BOOL) {
        jg_f2_cond(g, d->operands[0], x, r, hl, hh, depth + 1);
        jg_f2_cond(g, d->operands[1], x, r, hl, hh, depth + 1);
        return;
    }
    if (d->op != IR_ICMP || d->n_operands < 2) return;
    IrCmp p = d->aux.cmp;
    double c;
    if (d->operands[0] == x && jg_fconst(g, d->operands[1], &c, 0)) { }
    else if (d->operands[1] == x && jg_fconst(g, d->operands[0], &c, 0)) {
        switch (p) {                                           // c op x is x op' c
            case IR_CMP_SLT: case IR_CMP_ULT: p = IR_CMP_SGT; break;   case IR_CMP_SLE: case IR_CMP_ULE: p = IR_CMP_SGE; break;
            case IR_CMP_SGT: case IR_CMP_UGT: p = IR_CMP_SLT; break;   case IR_CMP_SGE: case IR_CMP_UGE: p = IR_CMP_SLE; break;
            default: break;
        }
    } else return;
    jg_f2_bound(p, c, r, hl, hh);
}
// trunc(x) at the point (block, index) of a conversion of float x: F1 where x is a constant, else F2
// from the guards on the dominator chain into the point (the edges TAKEN when a condition held,
// into single-predecessor blocks) and the ASSUMEs and ASSERTs before it. A negated or `else` edge
// gives nothing: it holds for a NaN. False where neither bound is known.
static bool jg_f2i(JG *g, int32_t blk, int32_t idx, IrValue *x, JRange *r, bool *hl, bool *hh) {
    double c; __int128 t; bool in;
    *hl = *hh = false; r->has_ne = false;
    if (jg_fconst(g, x, &c, 0)) {
        if (!jg_ftrunc(c, &t, &in)) return false;
        r->lo = r->hi = t; *hl = *hh = true;
        return true;
    }
    for (int32_t s = blk; ; s = g->idom[s]) {
        IrBlock *S = g->blk[s];
        int32_t k = 0;
        for (IrInstr *i = S->instrs; i; i = i->next, k++) {
            if (s == blk && k >= idx) break;
            if ((i->op == IR_ASSUME || i->op == IR_ASSERT) && i->n_operands >= 1) jg_f2_cond(g, i->operands[0], x, r, hl, hh, 0);
        }
        if (s == g->f->entry->id) break;
        if (g->npred[s] == 1 && S->preds) {
            IrBlock *P = S->preds->block;
            if (P->term.kind == IR_TERM_BR_COND && P->term.a != P->term.b && P->term.a == S) jg_f2_cond(g, P->term.cond, x, r, hl, hh, 0);
        }
    }
    return *hl || *hh;
}

// A value's STATIC range: a constant's value; through a cast whose source's static range lies in
// its result type (the value kept under any mode a cast has), the source's; a parameter's type
// narrowed by its refinement (jg_param_refine); a load of a store version, the stored value's; else
// its type's (a refinement included, as every type fact). False where none is known.
static bool jg_op_range(JG *g, const IrInstr *d, JRange *h, int depth);
static bool jg_static_range(JG *g, IrValue *v, JRange *r, int depth) {
    __int128 c;
    if (jg_const_of(g, v, &c)) { r->lo = r->hi = c; r->has_ne = false; return true; }
    if (!v || !v->type || !jg_range(v->type, r)) return false;
    IrInstr *d = v->id >= 0 && v->id < g->nv ? g->def[v->id] : NULL;
    if (!d && jg_param_value(g->f, v->id) == v) jg_param_refine(g, v, r);
    // a CHECKED +, - or unary - of static ranges: the exact interval, within its type (it traps outside)
    if (depth < 4 && d && (d->op == IR_ADD || d->op == IR_SUB || d->op == IR_NEG) && d->wrap == IR_WRAP_CHECK && !d->unchecked) {
        JRange a, b;
        if (d->op == IR_NEG ? d->n_operands >= 1 && jg_static_range(g, d->operands[0], &a, depth + 1)
                            : d->n_operands >= 2 && jg_static_range(g, d->operands[0], &a, depth + 1) && jg_static_range(g, d->operands[1], &b, depth + 1)) {
            __int128 lo = d->op == IR_ADD ? a.lo + b.lo : d->op == IR_SUB ? a.lo - b.hi : -a.hi;
            __int128 hi = d->op == IR_ADD ? a.hi + b.hi : d->op == IR_SUB ? a.hi - b.lo : -a.lo;
            if (lo > r->lo) r->lo = lo;
            if (hi < r->hi) r->hi = hi;
        }
    }
    JRange h;                                                  // a product, quotient, shift, bit op: its hull
    if (depth < 4 && d && jg_op_range(g, d, &h, depth)) {
        if (h.lo > r->lo) r->lo = h.lo;
        if (h.hi < r->hi) r->hi = h.hi;
    }
    JRange s;
    if (depth < 4 && d && d->op == IR_CAST && d->n_operands >= 1 && jg_is_int(d->operands[0]->type) &&
        jg_static_range(g, d->operands[0], &s, depth + 1) && s.lo >= r->lo && s.hi <= r->hi) *r = s;
    // a LOAD of a cell's STORE version (the one kind with a stored value, jg_part's piece of it): the
    // stored value's, where it lies in the loaded type (any landing keeps it), whichever execution
    // of that store the load reads (P2.1)
    if (depth < 4 && d && d->op == IR_LOAD && g->loadver && g->loadver[v->id] >= 0 && g->ver[g->loadver[v->id]].stored &&
        jg_static_range(g, g->ver[g->loadver[v->id]].stored, &s, depth + 1) && s.lo >= r->lo && s.hi <= r->hi) *r = s;
    r->has_ne = false;
    return true;
}
// HULLS of one operation from its operands' ranges, each where the operation completes (a checked
// one traps outside its type; a division's divisor is nonzero and its quotient in range, owed):
// a PRODUCT between the corners a*c, a*d, b*c, b*d (false past jg_mul_ok);
static bool jr_mul(const JRange *a, const JRange *b, JRange *h) {
    __int128 p[4];
    if (!jg_mul_ok(a->lo, b->lo, &p[0]) || !jg_mul_ok(a->lo, b->hi, &p[1]) || !jg_mul_ok(a->hi, b->lo, &p[2]) ||
        !jg_mul_ok(a->hi, b->hi, &p[3])) return false;
    h->lo = h->hi = p[0]; h->has_ne = false;
    for (int k = 1; k < 4; k++) { if (p[k] < h->lo) h->lo = p[k]; if (p[k] > h->hi) h->hi = p[k]; }
    return true;
}
// a QUOTIENT with |q| <= max|x| (|y| >= 1), and where y keeps one sign between the corners a/c,
// a/d, b/c, b/d (truncating division is monotone in each operand there; y in [0, d] is in [1, d]
// where it is nonzero, and [c, 0] in [c, -1]); a REMAINDER with |r| < max|y| and between x and 0
// (C's remainder truncates toward zero: r > 0 only where x > 0, then r <= x <= x.hi; r < 0 only
// where x < 0, then r >= x >= x.lo);
static bool jr_divrem(bool div, const JRange *a, const JRange *b0, JRange *h) {
    __int128 alo = a->lo < 0 ? -a->lo : a->lo, ahi = a->hi < 0 ? -a->hi : a->hi, mx = alo > ahi ? alo : ahi;
    JRange bn = *b0, *b = &bn;
    if (bn.lo == 0 && bn.hi >= 1) bn.lo = 1; else if (bn.hi == 0 && bn.lo <= -1) bn.hi = -1;
    h->has_ne = false;
    if (div) {
        h->lo = -mx; h->hi = mx;
        if (b->lo > 0 || b->hi < 0) {
            __int128 q[4] = { a->lo / b->lo, a->lo / b->hi, a->hi / b->lo, a->hi / b->hi }, lo = q[0], hi = q[0];
            for (int k = 1; k < 4; k++) { if (q[k] < lo) lo = q[k]; if (q[k] > hi) hi = q[k]; }
            if (lo > h->lo) h->lo = lo;
            if (hi < h->hi) h->hi = hi;
        }
        return true;
    }
    __int128 my = b->hi > -b->lo ? b->hi : -b->lo;
    if (my < 1) return false;
    h->lo = a->lo >= 0 ? 0 : a->lo > -(my - 1) ? a->lo : -(my - 1);
    h->hi = a->hi <= 0 ? 0 : a->hi < my - 1 ? a->hi : my - 1;
    return true;
}
// a BITWISE and, or, xor, at the operands' two's complement width: where both are >= 0, an and in
// [0, min(a.hi, b.hi)], an or in [max(a.lo, b.lo), 2^k - 1] and an xor in [0, 2^k - 1], 2^k the
// least power above both; else each in the least [-2^k, 2^k - 1] holding both (the result is as
// wide as its operands), an and with an operand >= 0 in [0, that operand's hi];
static bool jr_bit(IrOp op, const JRange *a, const JRange *b, JRange *h) {
    const __int128 top = (__int128)1 << 64;
    h->has_ne = false;
    if (a->lo >= 0 && b->lo >= 0) {
        __int128 m = a->hi > b->hi ? a->hi : b->hi, p = 1;
        while (p <= m && p < top) p <<= 1;
        if (p <= m) return false;
        if (op == IR_AND) { h->lo = 0; h->hi = a->hi < b->hi ? a->hi : b->hi; }
        else if (op == IR_OR) { h->lo = a->lo > b->lo ? a->lo : b->lo; h->hi = p - 1; }
        else { h->lo = 0; h->hi = p - 1; }
        return true;
    }
    __int128 p = 1;
    while ((a->lo < -p || a->hi > p - 1 || b->lo < -p || b->hi > p - 1) && p < top) p <<= 1;
    if (a->lo < -p || a->hi > p - 1 || b->lo < -p || b->hi > p - 1) return false;
    h->lo = -p; h->hi = p - 1;
    if (op == IR_AND && a->lo >= 0) { h->lo = 0; h->hi = a->hi; }
    else if (op == IR_AND && b->lo >= 0) { h->lo = 0; h->hi = b->hi; }
    return true;
}
// a SHIFT LEFT under CHECK, n in [0, w) (its shift obligation): x * 2^n, within the product's
// corners over x and [2^n.lo, 2^n.hi].
static bool jr_shl(const JRange *a, const JRange *n, int w, JRange *h) {
    __int128 nlo = n->lo < 0 ? 0 : n->lo, nhi = n->hi > w - 1 ? w - 1 : n->hi;
    if (nlo > nhi || nhi > 62) return false;
    JRange p2; p2.lo = (__int128)1 << nlo; p2.hi = (__int128)1 << nhi; p2.has_ne = false; p2.ne = 0;
    return jr_mul(a, &p2, h);
}
// The hull of instruction d from its operands' static ranges (at `depth`): a checked product or
// shift left, a division or remainder (checked, or unsigned), a bitwise and, or, xor. False where
// none is stated.
// Are two values one: the same SSA value, or two loads of one cell version (no write between)?
static bool jg_same_value(JG *g, IrValue *a, IrValue *b) {
    if (a == b) return true;
    if (!a || !b || a->id < 0 || a->id >= g->nv || b->id < 0 || b->id >= g->nv || !g->loadver) return false;
    return g->loadver[a->id] >= 0 && g->loadver[a->id] == g->loadver[b->id];
}
// May instruction i write memory? Every op NOT known to leave memory as it was: a STORE, a CALL,
// an INIT, an OPAQUE, a CONSUME, and any op added later (the safe side). An ALLOCA makes fresh
// storage and changes no existing cell.
static bool jg_may_write(const IrInstr *i) {
    switch (i->op) {
        case IR_CONST: case IR_CAST: case IR_ADD: case IR_SUB: case IR_MUL: case IR_SDIV: case IR_UDIV:
        case IR_SREM: case IR_UREM: case IR_NEG: case IR_AND: case IR_OR: case IR_XOR: case IR_SHL:
        case IR_LSHR: case IR_ASHR: case IR_BNOT: case IR_CTZ: case IR_CLZ: case IR_POPCOUNT:
        case IR_SIZEOF: case IR_ALIGNOF: case IR_ICMP: case IR_ALLOCA: case IR_SHAPE: case IR_LOAD:
        case IR_VEC_MOVEMASK: case IR_VEC_SHUFFLE: case IR_FIELD_PTR: case IR_ELEM_PTR:
        case IR_SLICE_LEN: case IR_SLICE_DATA: case IR_MAKE_SLICE: case IR_SEQ_EQ: case IR_SUBSLICE:
        case IR_ARRAY_NEW: case IR_STR_CONST: case IR_STRUCT_NEW: case IR_SUM_NEW: case IR_SUM_TAG:
        case IR_SUM_PAYLOAD: case IR_FUNC_REF: case IR_ASSUME: case IR_ASSERT: case IR_BORROW:
        case IR_BORROW_END:
            return false;
        default: return true;
    }
}
// Are two addresses one: jg_same_value, or the same ELEM_PTR or FIELD_PTR (one aux), or SLICE_DATA
// (a slice is a value: its data pointer is one), of operands that are pairwise one, to a small depth?
static bool jg_same_addr(JG *g, IrValue *a, IrValue *b, int depth) {
    if (jg_same_value(g, a, b)) return true;
    if (depth > 4 || !a || !b || a->id < 0 || a->id >= g->nv || b->id < 0 || b->id >= g->nv) return false;
    IrInstr *x = g->def[a->id], *y = g->def[b->id];
    if (!x || !y || x->op != y->op || (x->op != IR_ELEM_PTR && x->op != IR_FIELD_PTR && x->op != IR_SLICE_DATA) || x->n_operands != y->n_operands) return false;
    if (x->op == IR_FIELD_PTR && x->aux.field_idx != y->aux.field_idx) return false;
    for (int k = 0; k < x->n_operands; k++) {
        __int128 cx, cy;
        if (jg_const_of(g, x->operands[k], &cx) && jg_const_of(g, y->operands[k], &cy)) { if (cx != cy) return false; continue; }
        if (!jg_same_addr(g, x->operands[k], y->operands[k], depth + 1)) return false;
    }
    return true;
}
// A LOAD's TWIN (s9): an earlier load of the same address that dominates it, with no instruction
// that may write memory (jg_may_write) on any path from the twin to it, so the two read one value.
// L' in block D, L in block E: in one block, nothing between them writes; else no write in D after
// L', in E before L (or in all of E, where E reaches itself without passing D), nor in any block
// reachable from D without re-entering it that reaches E without passing D (a path back through D
// runs L' again, and the latest L' is the one compared). Only the nearest same-address load is
// tried: a farther one's paths pass the nearer one's block. The value id of the twin, or -1.
static bool jg_same_int_type(const IrType *a, const IrType *b) {   // one integer type, by value
    return a && b && a->kind == IRT_INT && b->kind == IRT_INT && a->bits == b->bits && a->is_signed == b->is_signed;
}
static bool jg_block_writes(JG *g, IrBlock *b, int32_t from, int32_t to) {   // instructions [from, to)
    int32_t k = 0;
    for (IrInstr *i = b->instrs; i; i = i->next, k++) if (k >= from && (to < 0 || k < to) && jg_may_write(i)) return true;
    (void)g; return false;
}
static int32_t jg_load_twin(JG *g, IrInstr *ld) {
    int32_t id = ld->result ? ld->result->id : -1;
    if (id < 0 || id >= g->nv || ld->n_operands < 1) return -1;
    if (!g->ltwin) {
        if (!(g->ltwin = JG_NEW(g, int32_t, g->nv))) return -1;
        for (int32_t i = 0; i < g->nv; i++) g->ltwin[i] = -2;
    }
    if (g->ltwin[id] != -2) return g->ltwin[id];
    g->ltwin[id] = -1;
    int32_t E = g->defblk[id], at = g->defidx[id];
    IrInstr *tw = NULL; int32_t D = -1, tat = -1;
    for (int32_t b = E; b >= 0 && !tw; ) {                   // up the dominator chain, nearest first
        int32_t k = 0, lim = b == E ? at : -1;
        for (IrInstr *i = g->blk[b]->instrs; i && (lim < 0 || k < lim); i = i->next, k++)
            if (i->op == IR_LOAD && i != ld && i->result && jg_same_int_type(i->result->type, ld->result->type) && i->n_operands >= 1 &&
                jg_same_addr(g, i->operands[0], ld->operands[0], 0)) { tw = i; D = b; tat = k; }
        if (tw) break;
        int32_t p = g->idom[b];
        if (p == b) break;
        b = p;
    }
    if (!tw) return -1;
    if (D == E) { if (jg_block_writes(g, g->blk[E], tat + 1, at)) return -1; }
    else {
        if (jg_block_writes(g, g->blk[D], tat + 1, -1) || jg_block_writes(g, g->blk[E], 0, at)) return -1;
        // fwd: reachable from D's successors without entering D; bwd: reach E without passing D
        uint8_t *fwd = JG_NEW(g, uint8_t, g->nb), *bwd = JG_NEW(g, uint8_t, g->nb), *cyc = JG_NEW(g, uint8_t, g->nb);
        int32_t *st = JG_NEW(g, int32_t, g->nb + 1);
        if (!fwd || !bwd || !cyc || !st) return -1;
        memset(fwd, 0, (size_t)g->nb); memset(bwd, 0, (size_t)g->nb); memset(cyc, 0, (size_t)g->nb);
        int32_t sp = 0; IrBlock *s[66];
        int ns = jg_succ(g->blk[D], s);
        for (int q = 0; q < ns; q++) if (s[q]->id != D && !fwd[s[q]->id]) { fwd[s[q]->id] = 1; st[sp++] = s[q]->id; }
        while (sp) { int32_t b = st[--sp]; ns = jg_succ(g->blk[b], s);
            for (int q = 0; q < ns; q++) if (s[q]->id != D && !fwd[s[q]->id]) { fwd[s[q]->id] = 1; st[sp++] = s[q]->id; } }
        bwd[E] = 1; st[sp++] = E;
        while (sp) { int32_t b = st[--sp];
            for (IrEdge *e = g->blk[b]->preds; e; e = e->next) { int32_t p = e->block->id;
                if (p != D && !bwd[p]) { bwd[p] = 1; st[sp++] = p; } } }
        ns = jg_succ(g->blk[E], s);                           // E reaches itself without D?
        for (int q = 0; q < ns; q++) if (s[q]->id != D && !cyc[s[q]->id]) { cyc[s[q]->id] = 1; st[sp++] = s[q]->id; }
        while (sp) { int32_t b = st[--sp]; ns = jg_succ(g->blk[b], s);
            for (int q = 0; q < ns; q++) if (s[q]->id != D && !cyc[s[q]->id]) { cyc[s[q]->id] = 1; st[sp++] = s[q]->id; } }
        if (cyc[E] && jg_block_writes(g, g->blk[E], 0, -1)) return -1;
        for (int32_t b = 0; b < g->nb; b++)
            if (b != E && b != D && fwd[b] && bwd[b] && g->blk[b] && jg_block_writes(g, g->blk[b], 0, -1)) return -1;
    }
    return g->ltwin[id] = tw->result->id;
}
// An integer condition known at a point (`truth` its value there) narrowing v: a comparison of v
// (jg_same_value) with a value of known static range, either way round, or an `and` that holds.
// An unsigned predicate (the interpreter compares as uint64) only against a y >= 0: v <u y and
// v <=u y put v in [0, y] whatever v's sign (a negative v reads as 2^64 + v, above any such y);
// v >u y and v >=u y bound v below only where v >= 0 by its static range.
static void jg_int_cond(JG *g, IrValue *cv, bool truth, IrValue *v, JRange *r, int depth) {
    IrInstr *d = cv && cv->id >= 0 && cv->id < g->nv ? g->def[cv->id] : NULL;
    if (!d || depth > 4) return;
    if (d->op == IR_AND && truth && d->n_operands >= 2 && cv->type && cv->type->kind == IRT_BOOL) {
        jg_int_cond(g, d->operands[0], true, v, r, depth + 1);
        jg_int_cond(g, d->operands[1], true, v, r, depth + 1);
        return;
    }
    if (d->op != IR_ICMP || d->n_operands < 2 || !jg_is_int(d->operands[0]->type) || !jg_is_int(d->operands[1]->type)) return;
    IrCmp p = d->aux.cmp;
    IrValue *y;
    if (jg_same_value(g, d->operands[0], v)) y = d->operands[1];
    else if (jg_same_value(g, d->operands[1], v)) {
        y = d->operands[0];
        switch (p) {                                           // y op v is v op' y
            case IR_CMP_SLT: p = IR_CMP_SGT; break; case IR_CMP_SLE: p = IR_CMP_SGE; break;
            case IR_CMP_SGT: p = IR_CMP_SLT; break; case IR_CMP_SGE: p = IR_CMP_SLE; break;
            case IR_CMP_ULT: p = IR_CMP_UGT; break; case IR_CMP_ULE: p = IR_CMP_UGE; break;
            case IR_CMP_UGT: p = IR_CMP_ULT; break; case IR_CMP_UGE: p = IR_CMP_ULE; break;
            default: break;
        }
    } else return;
    if (!truth)
        switch (p) {                                           // the false edge: the negation
            case IR_CMP_EQ: p = IR_CMP_NE; break;  case IR_CMP_NE: p = IR_CMP_EQ; break;
            case IR_CMP_SLT: p = IR_CMP_SGE; break; case IR_CMP_SGE: p = IR_CMP_SLT; break;
            case IR_CMP_SLE: p = IR_CMP_SGT; break; case IR_CMP_SGT: p = IR_CMP_SLE; break;
            case IR_CMP_ULT: p = IR_CMP_UGE; break; case IR_CMP_UGE: p = IR_CMP_ULT; break;
            case IR_CMP_ULE: p = IR_CMP_UGT; break; case IR_CMP_UGT: p = IR_CMP_ULE; break;
            default: return;
        }
    JRange yr, vr;
    if (!jg_static_range(g, y, &yr, 0)) return;
    if ((p == IR_CMP_ULT || p == IR_CMP_ULE || p == IR_CMP_UGT || p == IR_CMP_UGE) && yr.lo < 0) return;
    if ((p == IR_CMP_UGT || p == IR_CMP_UGE) && (!jg_static_range(g, v, &vr, 0) || vr.lo < 0)) return;
    if ((p == IR_CMP_ULT || p == IR_CMP_ULE) && r->lo < 0) r->lo = 0;
    switch (p) {
        case IR_CMP_SLT: case IR_CMP_ULT: if (yr.hi - 1 < r->hi) r->hi = yr.hi - 1; break;
        case IR_CMP_SLE: case IR_CMP_ULE: if (yr.hi < r->hi) r->hi = yr.hi; break;
        case IR_CMP_SGT: case IR_CMP_UGT: if (yr.lo + 1 > r->lo) r->lo = yr.lo + 1; break;
        case IR_CMP_SGE: case IR_CMP_UGE: if (yr.lo > r->lo) r->lo = yr.lo; break;
        case IR_CMP_EQ: if (yr.lo > r->lo) r->lo = yr.lo; if (yr.hi < r->hi) r->hi = yr.hi; break;
        default: break;
    }
}
// v's range at a point (block, index): its static range, narrowed by the integer conditions that
// hold there, the edges on the dominator chain into single-predecessor blocks (taken either way:
// an integer comparison is false exactly where its negation holds) and the ASSUMEs and ASSERTs
// before it. False where v has no static range, or the conditions leave nothing (a dead point).
static bool jg_point_range(JG *g, IrValue *v, int32_t blk, int32_t idx, JRange *r) {
    if (!jg_static_range(g, v, r, 0)) return false;
    if (blk < 0 || blk >= g->nb || !g->idom) return true;
    for (int32_t s = blk; ; s = g->idom[s]) {
        IrBlock *S = g->blk[s];
        int32_t k = 0;
        for (IrInstr *i = S->instrs; i; i = i->next, k++) {
            if (s == blk && k >= idx) break;
            if ((i->op == IR_ASSUME || i->op == IR_ASSERT) && i->n_operands >= 1) jg_int_cond(g, i->operands[0], true, v, r, 0);
        }
        if (s == g->f->entry->id) break;
        if (g->npred[s] == 1 && S->preds) {
            IrBlock *P = S->preds->block;
            if (P->term.kind == IR_TERM_BR_COND && P->term.a != P->term.b) jg_int_cond(g, P->term.cond, P->term.a == S, v, r, 0);
        }
    }
    return r->lo <= r->hi;
}
static bool jg_op_range(JG *g, const IrInstr *d, JRange *h, int depth) {
    JRange a, b;
    if (d->n_operands < 2 || !d->result || !d->result->type || d->result->type->kind != IRT_INT) return false;
    bool chk = d->wrap == IR_WRAP_CHECK && !d->unchecked;
    switch (d->op) {
        case IR_MUL: case IR_SHL: if (!chk) return false; break;
        case IR_SDIV: case IR_SREM: if (!chk) return false; break;
        case IR_UDIV: case IR_UREM: case IR_AND: case IR_OR: case IR_XOR: break;
        default: return false;
    }
    // an operation's OWN hull (depth -1) is at its point: its operands narrowed by what holds there,
    // which dominates the operation and so every use of its result; a nested one keeps static ranges
    int32_t rid = d->result->id, pb = depth < 0 && rid >= 0 && rid < g->nv ? g->defblk[rid] : -1, pi = pb >= 0 ? g->defidx[rid] : 0;
    if (pb >= 0 ? !jg_point_range(g, d->operands[0], pb, pi, &a) || !jg_point_range(g, d->operands[1], pb, pi, &b)
                : !jg_static_range(g, d->operands[0], &a, depth + 1) || !jg_static_range(g, d->operands[1], &b, depth + 1)) return false;
    switch (d->op) {
        case IR_MUL: return jr_mul(&a, &b, h);
        case IR_SHL: return jr_shl(&a, &b, d->operands[0]->type && d->operands[0]->type->kind == IRT_INT ? d->operands[0]->type->bits : 0, h);
        case IR_SDIV: case IR_UDIV: return jr_divrem(true, &a, &b, h);
        case IR_SREM: case IR_UREM: return jr_divrem(false, &a, &b, h);
        default: return jr_bit(d->op, &a, &b, h);
    }
}
static bool jg_divrem_hull(JG *g, const IrInstr *d, JRange *h) { return jg_op_range(g, d, h, -1); }
// A PRODUCT of two variables lies between its corners: x in [a, b] and y in [c, d] put x * y in
// [min, max] of a*c, a*d, b*c, b*d (`2 *? y` widens to i64, where two i32 ranges' corners fit).
// From the operands' static ranges; false where a corner is past what jg_mul_ok computes.
static bool jg_mul_hull(JG *g, const IrInstr *d, JRange *h) { return jg_op_range(g, d, h, -1); }
static bool jg_uns(const IrType *t) { return t && (t->kind == IRT_BOOL || (t->kind == IRT_INT && !t->is_signed)); }

// A claim's term as a VC variable: a cell is its version in `row` (a block's entry state, or the
// exit state of the predecessor an obligation is owed at), any other id that SSA value; of kind
// JTERM_LEN, the length of that slice version or value.
static int32_t jb_term(JB *B, int32_t key, uint8_t tk, const int32_t *row) {
    JG *g = B->g;
    if (g->cell[key] >= 0) return tk == JTERM_LEN ? jb_cell_len(B, row[g->cell[key]]) : jb_ver(B, row[g->cell[key]]);
    return tk == JTERM_LEN ? jb_len(B, jg_value(g, key)) : jb_val(B, jg_value(g, key));
}
// Loop header H's ENTRY predecessor: its one predecessor that is no back edge (both arms of one
// branch into H count once), or -1 where it has none or several. It dominates H (every first
// arrival at H comes through it), so a version it ends with is one value through a run of H's loop.
static bool jg_back_edge(const JG *g, int32_t P, int32_t H);
static int32_t jg_entry_pred(JG *g, int32_t H) {
    int32_t e = -1;
    for (IrEdge *pe = g->blk[H]->preds; pe; pe = pe->next) {
        int32_t P = pe->block->id;
        if (g->rpoix[P] < 0 || jg_back_edge(g, P, H)) continue;
        if (e >= 0 && e != P) return -1;
        e = P;
    }
    return e;
}
// A claim's term at its block H: as jb_term, but an ENTRY term is the cell's version on H's entry
// edge (jg_claims_ok has checked H has one), the same variable at every row.
static bool jg_loop_header(JG *g, int32_t b);
// Can an ENTRY term over `key` be stated at H: an integer cell, H a loop header with one entry
// predecessor E, the cell with a version where E ends.
static bool jg_entry_ok(JG *g, int32_t H, int32_t key) {
    if (key < 0 || key >= g->nv || g->cell[key] < 0 || !jg_is_int(g->cty[g->cell[key]]) || !jg_loop_header(g, H)) return false;
    int32_t E = jg_entry_pred(g, H);
    return E >= 0 && g->outstate[(size_t)E * (size_t)g->ncells + (size_t)g->cell[key]] >= 0;
}
// The header a HEAD term at block b names: the innermost natural loop's around b, b's own loop
// excluded; it dominates b. -1 where b is in no other loop, or where the loops around it do not
// nest. Its version of a cell is fixed from the trip's start to b: b's every edge but a back edge
// of its own loop comes from inside the trip, and a claim at b is stated only where b's latest
// version is (b dominating, or on a forward join's path), never past the header's next arrival.
static int32_t jg_head_of(JG *g, int32_t b) {
    if (b < 0 || b >= g->nb) return -1;
    int32_t H = -1;
    for (int32_t h = 0; h < g->nb; h++) {
        if (h == b || !jg_loop_header(g, h) || !g->inloop[(size_t)h * (size_t)g->nb + (size_t)b]) continue;
        if (H < 0 || g->rpoix[h] > g->rpoix[H]) H = h;
    }
    if (H < 0 || !jg_dominates(g, H, b)) return -1;
    for (int32_t h = 0; h < g->nb; h++)                        // every other loop around b holds H's
        if (h != b && h != H && jg_loop_header(g, h) && g->inloop[(size_t)h * (size_t)g->nb + (size_t)b] &&
            !g->inloop[(size_t)h * (size_t)g->nb + (size_t)H]) return -1;
    return H;
}
// Can a HEAD term over `key` be stated at b: an integer cell with a version at b's header.
static bool jg_head_ok(JG *g, int32_t b, int32_t key) {
    if (key < 0 || key >= g->nv || g->cell[key] < 0 || !jg_is_int(g->cty[g->cell[key]])) return false;
    int32_t H = jg_head_of(g, b);
    return H >= 0 && g->instate[(size_t)H * (size_t)g->ncells + (size_t)g->cell[key]] >= 0;
}
static int32_t jb_cterm(JB *B, int32_t key, uint8_t tk, const int32_t *row, int32_t H) {
    JG *g = B->g;
    if (tk == JTERM_HEAD)                                      // jg_claims_ok has checked b has one
        return jb_ver(B, g->instate[(size_t)jg_head_of(g, H) * (size_t)g->ncells + (size_t)g->cell[key]]);
    if (tk != JTERM_ENTRY) return jb_term(B, key, tk, row);
    int32_t E = jg_entry_pred(g, H);
    return jb_ver(B, g->outstate[(size_t)E * (size_t)g->ncells + (size_t)g->cell[key]]);
}
// A cut-point claim of the function being generated (block ids are per function: another
// function's claim at the same id is none of this one's).
static bool jg_mine(const JG *g, const JClaim *c) { return c->blk >= 0 && (!c->fn || c->fn == g->f); }
static bool jg_same_module(const IrFunc *a, const IrFunc *b) {
    return (!a->src_file && !b->src_file) || (a->src_file && b->src_file && !strcmp(a->src_file, b->src_file));
}
// Parameter `key` of h: its position, or -1.
static int jg_param_at(const IrFunc *h, int32_t key) {
    int k = 0;
    for (const IrParam *p = h->params; p; p = p->next, k++) if (p->value && p->value->id == key) return k;
    return -1;
}
// A value landing in a slot of type `to` keeps its value: the same integer type (no conversion);
// a landing whose obligation the generator owes there (jg_land: a checked one, or one into a
// refined slot, `unsafe` or not), which a program reaching past it has kept, the value in range;
// or a slot that holds every value of `from`. A waived landing into a plain slot may wrap.
static bool jg_lands_exact(const IrType *from, const IrType *to, bool waived) {
    if (!jg_is_int(from) || !jg_is_int(to)) return false;
    if (from->kind == to->kind && (from->kind == IRT_BOOL || (from->bits == to->bits && from->is_signed == to->is_signed))) return true;
    if (!waived || (to->kind == IRT_INT && (to->has_refine || to->has_ne))) return true;
    JRange a, b;
    return jg_range(from, &a) && jg_range(to, &b) && b.lo <= a.lo && a.hi <= b.hi;
}
// The claims about a version a join or a header makes, stated where the version is: VC-wide, or
// inside the current disjunct when the version is local to its path. Each once per place (a claim
// of several terms is reached from each).
static int32_t jg_each_cells(JG *g, int32_t key, int32_t *cells);   // below
static void jb_claims(JB *B, const JVer *v) {
    JG *g = B->g;
    int32_t at = B->jS >= 0 ? B->dgen : -1;
    for (int32_t ci = 0; ci < g->ncl && !B->fail; ci++) {
        const JClaim *c = &g->cl[ci];
        if (!jg_mine(g, c) || c->blk != v->blk || B->cst[ci] == at) continue;
        bool about = false;
        int32_t ec[JELEM_MAXN], en = 0, ek = -1, gk = -1;         // an EACH term: its elements' cells;
        for (int k = 0; k < c->nt; k++) {                         // an UPTO term: the guard
            if (c->tk[k] == JTERM_EACH) {
                ek = k; en = jg_each_cells(g, c->key[k], ec);
                for (int32_t q = 0; q < en; q++) if (ec[q] >= 0 && ec[q] == v->cell) about = true;
            } else {
                if (c->tk[k] == JTERM_UPTO) gk = k;
                if (g->cell[c->key[k]] == v->cell) about = true;
            }
        }
        if (!about) continue;
        if (gk >= 0 && B->jS >= 0) continue;                      // a disjunct holds no disjunction
        B->cst[ci] = at;
        const int32_t *row = g->instate + (size_t)c->blk * (size_t)g->ncells;
        const char *o = jb_str(B, "claim bb%lld #%lld", c->blk, ci, 0);
        for (int32_t q = 0; q < (ek >= 0 ? en : 1) && !B->fail; q++) {   // an EACH claim: every element cell
            if (ek >= 0 && ec[q] < 0) continue;
            JLT t[JCLAIM_MAXT]; int n = 0;
            for (int k = 0; k < c->nt; k++) {
                if (k == gk) continue;
                t[n].var = k == ek ? jb_ver(B, row[ec[q]]) : jb_cterm(B, c->key[k], c->tk[k], row, c->blk);
                t[n].c = c->coef[k]; n++;
            }
            if (gk < 0) { jb_atom(B, o, t, n, J_LE, c->rhs); continue; }
            // guarded: g <= k, or the sum (element q's instance of `for each k < g`)
            JCons cs[2]; JLT u[1];
            u[0].var = jb_term(B, c->key[gk], JTERM_VALUE, row); u[0].c = 1;
            if (!jb_cons(B, u, 1, J_LE, q, &cs[0]) || !jb_cons(B, t, n, J_LE, c->rhs, &cs[1])) { B->fail = true; break; }
            jb_or2(B, jb_str(B, "claim bb%lld #%lld upto", c->blk, ci, 0), cs, 1, 1);   // the finder keys on "upto"
        }
    }
}
// MAX_LEN(T) for a slice of type `st` (P2.4, D1 A3): ISIZE_MAX / sizeof(T), with sizeof(T) its
// byte width for an integer element and 1 otherwise, which only weakens the bound. ONE function
// for the fact every reader of a length is given (jb_state) and for the `objsize` obligation every
// MAKE_SLICE owes: the fact is sound only while the two agree.
static int64_t jg_len_max(const IrType *st) {
    int64_t es = 1;
    if (st && st->elem && st->elem->kind == IRT_INT) es = st->elem->bits >= 8 ? st->elem->bits / 8 : 1;
    return INT64_MAX / es;
}
// A WRAPPING x + y or x - y (`+%`, `-%`, or unsigned arithmetic inside `unsafe`): the interpreter
// computes s = x +- y exactly and lands it once in the result's w bits (ii_settle, it_wrap), so the
// result is s - k*2^w for the one k that puts it in the result's range. Over the operands' type
// ranges (facts, licensed by their own obligations; a constant's range is its value) s lies in
// [slo, shi], which leaves at most three k; with the result's own range that disjunction pins it.
// Inside a join's disjunct, which holds no disjunction, the one case its path took (R7 enumerates
// a path per case, s5) or, with none, nothing; nothing when the result is not an integer of at most
// 64 bits.
// Is d such a wrapping ADD or SUB?
static bool jg_wrap_op(const IrInstr *d) {
    if ((d->op != IR_ADD && d->op != IR_SUB && d->op != IR_MUL) || d->n_operands < 2 || !d->result || !d->result->type) return false;
    if (!jg_is_int(d->operands[0]->type) || !jg_is_int(d->operands[1]->type)) return false;
    if (d->op == IR_MUL) return d->wrap == IR_WRAP_MODULAR;  // `*%` (jg_wrap_ks: by one constant, |c| <= 3)
    return d->wrap == IR_WRAP_MODULAR || (d->unchecked && !d->result->type->is_signed && d->wrap == IR_WRAP_CHECK);
}
// Its cases: k from klo to khi (one to three), M = 2^w, the result's range rr. False where none is
// stated.
static bool jg_wrap_ks(JG *g, const IrInstr *d, __int128 *klo, __int128 *khi, __int128 *M, JRange *rr) {
    JRange ra, rb;
    const IrType *rt = d->result ? d->result->type : NULL;
    if (!rt || rt->kind != IRT_INT || rt->bits < 1 || rt->bits > 64) return false;
    if (!jg_range(d->operands[0]->type, &ra) || !jg_range(d->operands[1]->type, &rb)) return false;
    // a constant operand's range is its value: `i +% 1` on a u8 has an i32 literal, whose type
    // range alone would leave thousands of k (Main Compiler Work)
    __int128 cv;
    if (jg_const_of(g, d->operands[0], &cv)) ra.lo = ra.hi = cv;
    if (jg_const_of(g, d->operands[1], &cv)) rb.lo = rb.hi = cv;
    rr->lo = jg_width_lo(rt); rr->hi = jg_width_hi(rt); rr->has_ne = false;
    *M = (__int128)1 << rt->bits;
    __int128 slo = d->op == IR_ADD ? ra.lo + rb.lo : ra.lo - rb.hi, shi = d->op == IR_ADD ? ra.hi + rb.hi : ra.hi - rb.lo;
    if (d->op == IR_MUL) {                                     // `c *% y`: s = c*y over y's range, one constant
        __int128 c; JRange o;                                  // factor c, |c| <= 3 (three cases at most), as
        bool k0 = jg_const_of(g, d->operands[0], &c);          // interp.h wraps the exact product into the type
        if (k0) o = rb; else if (jg_const_of(g, d->operands[1], &c)) o = ra; else return false;
        if (k0 && jg_const_of(g, d->operands[1], &cv)) return false;   // both constant: folded, not stated here
        if (c > 3 || c < -3) return false;
        slo = c >= 0 ? c * o.lo : c * o.hi; shi = c >= 0 ? c * o.hi : c * o.lo;
    }
    // r = s - k*M in [rr.lo, rr.hi]: k from ceil((slo - rr.hi) / M) to floor((shi - rr.lo) / M)
    __int128 lo = slo - rr->hi, hi = shi - rr->lo;
    lo = lo >= 0 ? (lo + *M - 1) / *M : -((-lo) / *M);
    hi = hi >= 0 ? hi / *M : -((-hi + *M - 1) / *M);
    *klo = lo; *khi = hi;
    return hi >= lo && hi - lo <= 2;                           // three cases at most (a w-bit sum)
}
// jb_wrap's terms, r - x -+ y or r - c*y: built afresh for every jb_cons, which sorts and merges
// the array in place
static void jb_wrap_terms(JLT *t, int32_t x, int32_t xa, int32_t xb, IrOp op, int32_t my, __int128 mc) {
    t[0].var = x; t[0].c = 1;
    if (op == IR_MUL) { t[1].var = my; t[1].c = -mc; return; }
    t[1].var = xa; t[1].c = -1; t[2].var = xb; t[2].c = op == IR_ADD ? -1 : 1;
}
static void jb_wrap(JB *B, int32_t x, const IrInstr *d, int32_t id) {
    JRange rr; __int128 klo, khi, M;
    if (!jg_wrap_ks(B->g, d, &klo, &khi, &M, &rr)) return;
    bool taken = false;                                        // a case the disjunct's path took
    if (B->jS >= 0 && klo != khi) {
        int32_t q = -1;
        for (int32_t i = 0; i < B->nw; i++) if (B->wid[i] == id) q = i;
        if (q < 0) return;
        klo = khi = B->wk[q]; taken = true;
    }
    int32_t xa = jb_val(B, d->operands[0]), xb = jb_val(B, d->operands[1]);
    const char *origin = jb_str(B, d->op == IR_ADD ? "def %%v%lld add.wrap" : d->op == IR_SUB ? "def %%v%lld sub.wrap" : "def %%v%lld mul.wrap", id, 0, 0);
    // r - x -+ y, or r - c*y for `c *% y` (c the constant operand); REBUILT before each jb_cons, which
    // sorts and merges the array in place (`y +% y` leaves one term: a reused array is stale)
    JLT t[3]; int nt = 3; __int128 mc = 0; int32_t my = -1;
    if (d->op == IR_MUL) {
        bool k0 = jg_const_of(B->g, d->operands[0], &mc);
        if (!k0 && !jg_const_of(B->g, d->operands[1], &mc)) return;
        my = k0 ? xb : xa; nt = 2;
    }
    jb_wrap_terms(t, x, xa, xb, d->op, my, mc);
    if (klo == khi) {                                          // one case: exact
        jb_atom(B, origin, t, nt, J_EQ, -klo * M);
        if (taken) {                                           // with its guard: r within its range
            JLT u[1]; u[0].var = x; u[0].c = 1; jb_atom(B, origin, u, 1, J_LE, rr.hi);
            u[0].var = x; u[0].c = -1; jb_atom(B, origin, u, 1, J_LE, -rr.lo);
        }
        return;
    }
    JHyp *h = jb_hyp(B, origin);
    if (!h) return;
    int32_t nd = (int32_t)(khi - klo + 1);
    h->disj = true; h->ndis = nd;
    if (!(h->dis = JG_NEW(B->g, JConj, nd))) { B->fail = true; return; }
    for (int32_t k = 0; k < nd; k++) {                         // r - x -+ y = -k*M
        if (!(h->dis[k].c = JG_NEW(B->g, JCons, 1))) { B->fail = true; return; }
        h->dis[k].nc = 1;
        jb_wrap_terms(t, x, xa, xb, d->op, my, mc);
        jb_cons(B, t, nt, J_EQ, -(klo + k) * M, &h->dis[k].c[0]);
    }
}
// A load from a module constant TABLE (an ALLOCA with data: the static read-only object whose
// only writer is its initialiser, ir.h's seal) by a checked index: at a constant index k, the
// element's value; at any other, where the table has at most JELEM_MAXN elements, the one the
// index names (`or { i = k , v = vals[k] | ... }`, R7), else between its least and greatest.
// An element is read by its type (a u64 above 2^63 is stored as a negative int64).
static __int128 jg_table_val(const IrData *t, int32_t k) {
    const IrType *e = t->type ? t->type->elem : NULL;
    return e && e->kind == IRT_INT && !e->is_signed && e->bits == 64 ? (__int128)(uint64_t)t->vals[k] : (__int128)t->vals[k];
}
static void jb_table_load(JB *B, int32_t x, const IrInstr *ld, int32_t id) {
    JG *g = B->g;
    IrValue *a = ld->n_operands ? ld->operands[0] : NULL;
    IrInstr *ep = a && a->id >= 0 && a->id < g->nv ? g->def[a->id] : NULL;
    if (!ep || ep->op != IR_ELEM_PTR || ep->n_operands < 2 || ep->unchecked || !ep->operands[0]) return;
    IrValue *bv = ep->operands[0];
    IrInstr *ad = bv->id >= 0 && bv->id < g->nv ? g->def[bv->id] : NULL;
    const IrData *t = ad && ad->op == IR_ALLOCA ? ad->data : NULL;
    if (!t || t->n < 1 || !t->vals || !t->type || !t->type->elem || t->type->elem->kind != IRT_INT ||
        !ld->result || !ld->result->type || ld->result->type->kind != IRT_INT) return;
    __int128 k;
    if (jg_const_of(g, ep->operands[1], &k)) {
        if (k >= 0 && k < t->n) jb_defk(B, x, NULL, 0, jg_table_val(t, (int32_t)k), jb_str(B, "def %%v%lld table", id, 0, 0));
        return;
    }
    if (t->n <= JELEM_MAXN && B->jS < 0) {
        int32_t xi = jb_val(B, ep->operands[1]);
        JHyp *h = jb_hyp(B, jb_str(B, "def %%v%lld table", id, 0, 0));
        if (!h) return;
        h->disj = true; h->ndis = t->n;
        if (!(h->dis = JG_NEW(g, JConj, t->n))) { B->fail = true; return; }
        for (int32_t q = 0; q < t->n && !B->fail; q++) {
            JConj *cj = &h->dis[q];
            cj->nc = 2;
            if (!(cj->c = JG_NEW(g, JCons, 2))) { B->fail = true; return; }
            JLT u[1];
            u[0].var = xi; u[0].c = 1; jb_cons(B, u, 1, J_EQ, q, &cj->c[0]);
            u[0].var = x; u[0].c = 1; jb_cons(B, u, 1, J_EQ, jg_table_val(t, q), &cj->c[1]);
        }
        return;
    }
    JRange r; r.lo = r.hi = jg_table_val(t, 0); r.has_ne = false; r.ne = 0;
    for (int32_t q = 1; q < t->n; q++) { __int128 v = jg_table_val(t, q); if (v < r.lo) r.lo = v; if (v > r.hi) r.hi = v; }
    jb_range(B, x, &r, jb_str(B, "def %%v%lld table", id, 0, 0));
}
// A value WRAPPED into an integer type of w <= 64 bits, as interp.h's it_wrap makes it (a `%` cast,
// and an unchecked cast or landing that does not fit, I.60, I.185): x = v - k*2^w with x within
// the type's width, k a fresh integer (its recipe (v - x) / 2^w for a run-time check). One case,
// so it may stand inside a join's disjunct too; where v fits, k is 0.
static void jb_wrapto(JB *B, int32_t x, int32_t xv, const IrType *t, const char *what, int32_t id, const char *origin) {
    if (!t || t->kind != IRT_INT || t->bits < 1 || t->bits > 64) return;
    __int128 M = (__int128)1 << t->bits;
    JLT r[2]; r[0].var = xv; r[0].c = 1; r[1].var = x; r[1].c = -1;
    int32_t k = jb_fresh(B, what, id, r, 2, M);                // `what` keeps a version's k apart from a value's
    if (B->fail) return;
    JLT u[3];
    u[0].var = x; u[0].c = 1; u[1].var = xv; u[1].c = -1; u[2].var = k; u[2].c = M;
    jb_atom(B, origin, u, 3, J_EQ, 0);                         // x - v + k*M = 0
    JRange w; w.lo = jg_width_lo(t); w.hi = jg_width_hi(t); w.has_ne = false;
    jb_range(B, x, &w, origin);
}
// MONOTONE products (`i*w + j < h*w`): beside p = a*c, an earlier checked product q = b*c of the
// same variable c >= 0, its definition dominating p's (so at p, q is the product of the b and c
// current there), gives a <= b - 1 -> p <= q - c and a >= b + 1 -> p >= q + c, since (b - a)*c >= c
// there; both products exact, a checked one completing only within its type. At most JMONO_MAX
// such q per p, the first in block order: each is two disjunctions, which the finder pays for
// (it flattens them by "mul.mono", an interface).
enum { JMONO_MAX = 2 };
static void jb_mul_mono(JB *B, int32_t x, const IrInstr *d, int32_t id) {
    JG *g = B->g;
    if (B->jS >= 0 || id < 0 || id >= g->nv || g->defblk[id] < 0) return;   // a disjunct holds no disjunction
    int32_t pb = g->defblk[id], pi = g->defidx[id], n = 0;
    for (IrBlock *qb = g->f->blocks; qb && n < JMONO_MAX; qb = qb->next) {
        if (!jg_dominates(g, qb->id, pb)) continue;
        int32_t k = 0;
        for (IrInstr *e = qb->instrs; e && n < JMONO_MAX && !(qb->id == pb && k >= pi); e = e->next, k++) {
            if (e->op != IR_MUL || e->wrap != IR_WRAP_CHECK || e->unchecked || e->n_operands < 2 || !e->result) continue;
            int s = -1, u = -1;
            for (int s0 = 0; s0 < 2 && s < 0; s0++)
                for (int u0 = 0; u0 < 2 && s < 0; u0++) if (d->operands[s0] == e->operands[u0]) { s = s0; u = u0; }
            if (s < 0) continue;
            IrValue *c = d->operands[s], *a = d->operands[1 - s], *b = e->operands[1 - u];
            JRange rc;
            if (a == b || !jg_is_int(b->type) || !jg_static_range(g, c, &rc, 0) || rc.lo < 0) continue;
            int32_t av = jb_val(B, a), bv = jb_val(B, b), cv = jb_val(B, c), qv = jb_val(B, e->result);
            const char *o = jb_str(B, "def %%v%lld mul.mono", id, 0, 0);
            for (int w = 0; w < 2; w++) {                      // or { b - a <= 0 | p - q + c <= 0 }, and mirrored
                int sg = w ? -1 : 1;
                JCons cs[2]; JLT t[3];
                t[0].var = bv; t[0].c = sg; t[1].var = av; t[1].c = -sg;
                if (!jb_cons(B, t, 2, J_LE, 0, &cs[0])) return;
                t[0].var = x; t[0].c = sg; t[1].var = qv; t[1].c = -sg; t[2].var = cv; t[2].c = 1;
                if (!jb_cons(B, t, 3, J_LE, 0, &cs[1])) return;
                jb_or2(B, o, cs, 1, 1);
            }
            n++;
        }
    }
}
// The facts of one variable: its definition and its range.
// A call d to h of this module: h's RETURN claims (I.181's boundary), `rv` for the returned term (the
// call's result, or its length where h returns a slice: jg_ret_claim_ok ties the term's kind to h's
// return type) and each argument for its parameter, where the argument lands exactly (its landing
// obligation, owed at this call, licenses it).
static void jb_contracts(JB *B, IrInstr *d, IrFunc *h, int32_t rv) {
    JG *g = B->g;
    for (int32_t ci = 0; ci < g->ncl && !B->fail; ci++) {
        const JClaim *c = &g->cl[ci];
        if (c->blk != JCLAIM_RET || c->fn != h) continue;
        JLT u[JCLAIM_MAXT]; bool ok = true;                    // a return claim has up to JCLAIM_MAXT terms
        for (int k = 0; k < c->nt && ok; k++) {
            u[k].c = c->coef[k];
            if (c->key[k] == JKEY_RET) { u[k].var = rv; continue; }
            int pk = jg_param_at(h, c->key[k]);
            IrParam *pp = h->params;
            for (int q = 0; q < pk && pp; q++) pp = pp->next;
            if (pk < 0 || pk >= d->n_operands || !pp || !pp->value ||
                !jg_lands_exact(d->operands[pk]->type, pp->value->type, d->unchecked)) { ok = false; break; }
            u[k].var = jb_val(B, d->operands[pk]);
        }
        if (!ok) continue;
        char o[96]; snprintf(o, sizeof o, "contract %.*s #%lld", (int)h->name->length, h->name->name, (long long)ci);
        char *os = JG_NEW(g, char, strlen(o) + 1);
        if (!os) { B->fail = true; return; }
        strcpy(os, o);
        jb_atom(B, os, u, c->nt, J_LE, c->rhs);
    }
}
static void jb_state(JB *B, int32_t x) {
    JG *g = B->g;
    if (B->kind[x] == 1) {                                     // a cell's version
        JVer *v = &g->ver[B->key[x]];
        jb_type(B, x, v->slot, g->cname[v->cell], -1);
        if (v->kind == JV_STORE && !jg_store_exact(v) && v->stored && v->stored->type &&   // an int one: waived
            v->stored->type->kind == IRT_INT && v->slot && v->slot->kind == IRT_INT) {
            // a waived landing that does not keep the value wraps it into the slot (ii_land)
            char o[96]; snprintf(o, sizeof o, "store %s@bb%lld.%lld wrap", g->cname[v->cell], (long long)v->blk, (long long)v->idx);
            char *os = JG_NEW(g, char, strlen(o) + 1);
            if (os) strcpy(os, o); else B->fail = true;
            if (os) jb_wrapto(B, x, jb_val(B, v->stored), v->slot, "wrap.ver", B->key[x], os);
        }
        if (v->kind == JV_STORE && jg_store_exact(v)) {
            // the version is the value stored; a value of another type lands in the slot, and that
            // landing's obligation is the guarantee the slot's range above rests on
            JLT t[1]; t[0].var = jb_val(B, v->stored); t[0].c = 1;
            char o[96]; snprintf(o, sizeof o, "store %s@bb%lld.%lld", g->cname[v->cell], (long long)v->blk, (long long)v->idx);
            char *os = JG_NEW(g, char, strlen(o) + 1);
            if (os) strcpy(os, o); else B->fail = true;
            jb_def(B, x, t, 1, os);
        }
        if (v->kind == JV_HAVOC && v->es && v->eprev >= 0 && B->jS < 0) {
            // an element a store `a[i] = s` may have written (jg_elem_store): i = k and it is s, or
            // i is another index and it is what it was (one case per side; not inside a disjunct)
            IrInstr *ep = g->def[v->es->operands[0]->id];
            int32_t iv = jb_val(B, ep->operands[1]), sv = jb_val(B, v->es->operands[1]), pv = jb_ver(B, v->eprev);
            char o[96]; snprintf(o, sizeof o, "store %s@havoc.bb%lld.%lld element", g->cname[v->cell], (long long)v->blk, (long long)v->idx);
            char *os = JG_NEW(g, char, strlen(o) + 1);
            JHyp *h = os ? jb_hyp(B, strcpy(os, o)) : NULL;
            if (!h || !(h->dis = JG_NEW(g, JConj, 3))) { B->fail = true; return; }
            h->disj = true; h->ndis = 3;
            for (int dd = 0; dd < 3 && !B->fail; dd++) {
                if (!(h->dis[dd].c = JG_NEW(g, JCons, 2))) { B->fail = true; return; }
                h->dis[dd].nc = 2;
                JLT t[2];
                t[0].var = iv; t[0].c = dd == 2 ? -1 : 1;              // i = k, i <= k - 1, -i <= -(k + 1)
                jb_cons(B, t, 1, dd == 0 ? J_EQ : J_LE, dd == 0 ? v->ek : dd == 1 ? v->ek - 1 : -(v->ek + 1), &h->dis[dd].c[0]);
                t[0].var = x; t[0].c = 1; t[1].var = dd == 0 ? sv : pv; t[1].c = -1;
                jb_cons(B, t, 2, J_EQ, 0, &h->dis[dd].c[1]);
            }
        }
        if (v->kind == JV_JOIN || v->kind == JV_HEAD) jb_claims(B, v);
        return;
    }
    if (B->kind[x] == 4) {                                     // a slice cell version's length
        const IrType *st = g->cty[g->ver[B->key[x]].cell];
        char ob[160]; snprintf(ob, sizeof ob, "objsize %s", g->cname[g->ver[B->key[x]].cell]);
        char *o = JG_NEW(g, char, strlen(ob) + 1);
        if (!o) { B->fail = true; return; }
        strcpy(o, ob);
        JLT t[1]; t[0].var = x;
        t[0].c = -1; jb_atom(B, o, t, 1, J_LE, 0);
        t[0].c = 1;  jb_atom(B, o, t, 1, J_LE, jg_len_max(st));
        return;
    }
    if (B->kind[x] == 2) {                                     // a slice's length
        IrValue *s = jg_value(g, B->key[x]);
        IrType *st = s ? s->type : NULL;
        JLT t[1]; t[0].var = x;
        t[0].c = -1; jb_atom(B, jb_str(B, "objsize %%v%lld", B->key[x], 0, 0), t, 1, J_LE, 0);
        t[0].c = 1;  jb_atom(B, jb_str(B, "objsize %%v%lld", B->key[x], 0, 0), t, 1, J_LE, jg_len_max(st));
        IrInstr *d = g->def[B->key[x]];
        if (d && d->op == IR_CALL) {                           // a callee's return claims on len(ret)
            IrFunc *h = jg_callee(g, d);
            if (h && jg_same_module(h, g->f) && h->ret_type && h->ret_type->kind == IRT_SLICE) jb_contracts(B, d, h, x);
        }
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
        case IR_LOAD: {                                        // a[i]: the element the index names (R7)
            JELoad *el = g->eload ? g->eload[id] : NULL;
            if (!el) {
                jb_table_load(B, x, d, id);
                // the value an earlier load of the same address read, no write between (s9)
                int32_t tw = g->loadver && g->loadver[id] < 0 ? jg_load_twin(g, d) : -1;
                if (tw >= 0 && jg_is_int(v->type)) {
                    t[0].var = jb_val(B, jg_value(g, tw)); t[0].c = 1;
                    jb_def(B, x, t, 1, jb_str(B, "def %%v%lld load.same", id, 0, 0));
                }
                break;
            }
            if (B->jS >= 0) break;                             // a disjunct holds no disjunction
            int32_t xi = jb_val(B, el->ix);
            JHyp *h = jb_hyp(B, jb_str(B, "def %%v%lld element", id, 0, 0));
            if (!h) break;
            h->disj = true; h->ndis = el->n;
            if (!(h->dis = JG_NEW(g, JConj, el->n))) { B->fail = true; break; }
            for (int32_t k = 0; k < el->n && !B->fail; k++) {
                JConj *cj = &h->dis[k];
                cj->nc = 2;
                if (!(cj->c = JG_NEW(g, JCons, 2))) { B->fail = true; break; }
                JLT u[2];
                u[0].var = xi; u[0].c = 1; jb_cons(B, u, 1, J_EQ, k, &cj->c[0]);
                u[0].var = x; u[0].c = 1; u[1].var = jb_ver(B, el->ver[k]); u[1].c = -1; jb_cons(B, u, 2, J_EQ, 0, &cj->c[1]);
            }
            break;
        }
        case IR_ADD: case IR_SUB:
            if (d->n_operands < 2 || !jg_is_int(d->operands[0]->type) || !jg_is_int(d->operands[1]->type)) break;
            if (d->wrap == IR_WRAP_MODULAR || (d->unchecked && !v->type->is_signed && d->wrap == IR_WRAP_CHECK)) {
                jb_wrap(B, x, d, id);                          // `+%`, `-%`, and unsigned arithmetic in `unsafe`
                break;
            }
            if (d->wrap != IR_WRAP_CHECK) break;
            if (d->unchecked) {                                // a SIGNED one in `unsafe`: exact only where
                JRange ra, rb;                                 // its operands' static ranges keep it in its type
                if (!jg_static_range(g, d->operands[0], &ra, 0) || !jg_static_range(g, d->operands[1], &rb, 0)) break;
                __int128 slo = d->op == IR_ADD ? ra.lo + rb.lo : ra.lo - rb.hi, shi = d->op == IR_ADD ? ra.hi + rb.hi : ra.hi - rb.lo;
                if (v->type->bits < 1 || v->type->bits > 64 || slo < jg_width_lo(v->type) || shi > jg_width_hi(v->type)) break;
            }
            t[0].var = jb_val(B, d->operands[0]); t[0].c = 1;
            t[1].var = jb_val(B, d->operands[1]); t[1].c = d->op == IR_ADD ? 1 : -1;
            jb_def(B, x, t, 2, jb_str(B, d->op == IR_ADD ? "def %%v%lld add" : "def %%v%lld sub", id, 0, 0));
            break;
        case IR_NEG:
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 1 || !jg_is_int(d->operands[0]->type)) break;
            t[0].var = jb_val(B, d->operands[0]); t[0].c = -1;
            jb_def(B, x, t, 1, jb_str(B, "def %%v%lld neg", id, 0, 0));
            break;
        case IR_BNOT:                                          // exact: interp.h wraps ~x into the type,
            // which is -x - 1 for a signed w-bit x (never out of range) and (2^w - 1) - x for an
            // unsigned one; a result of another type, a bool or a vector states nothing
            if (d->n_operands < 1 || !d->operands[0]->type || d->operands[0]->type->kind != IRT_INT || !v->type || v->type->kind != IRT_INT ||
                !jg_same_int_type(d->operands[0]->type, v->type) || v->type->bits < 1 || v->type->bits > 64) break;
            t[0].var = jb_val(B, d->operands[0]); t[0].c = -1;
            jb_defk(B, x, t, 1, v->type->is_signed ? -1 : (((__int128)1 << v->type->bits) - 1), jb_str(B, "def %%v%lld bnot", id, 0, 0));
            break;
        case IR_MUL: {
            if (d->wrap == IR_WRAP_MODULAR && jg_wrap_op(d)) { jb_wrap(B, x, d, id); break; }   // `c *% y`, its cases
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 2) break;
            bool lin = false;
            for (int k = 0; k < 2 && !lin; k++) {
                IrValue *o = d->operands[1 - k]; __int128 kc;
                if (!jg_const_of(g, d->operands[k], &kc) || !jg_is_int(o->type)) continue;
                t[0].var = jb_val(B, o); t[0].c = kc;
                jb_def(B, x, t, 1, jb_str(B, "def %%v%lld mul", id, 0, 0));
                lin = true;
            }
            JRange h;                                          // two variables: between the corners
            if (!lin && jg_mul_hull(g, d, &h)) jb_range(B, x, &h, jb_str(B, "def %%v%lld mul.hull", id, 0, 0));
            if (!lin) jb_mul_mono(B, x, d, id);
            // two variables of an exact product: where both are >= 1, it is at least each (p - b =
            // b(a - 1) >= 0), so `fact(n) = n *? fact(n - 1)` keeps fact >= 1 (s6). Two disjunctions of
            // three, VC-wide only: a disjunct holds no disjunction
            if (!lin && B->jS < 0 && jg_is_int(d->operands[0]->type) && jg_is_int(d->operands[1]->type)) {
                int32_t av = jb_val(B, d->operands[0]), bv = jb_val(B, d->operands[1]);
                const char *o = jb_str(B, "def %%v%lld mul.sign", id, 0, 0);
                for (int w = 0; w < 2; w++) {
                    JCons cs[3]; JLT tt[2];
                    tt[0].var = av; tt[0].c = 1; if (!jb_cons(B, tt, 1, J_LE, 0, &cs[0])) break;
                    tt[0].var = bv; tt[0].c = 1; if (!jb_cons(B, tt, 1, J_LE, 0, &cs[1])) break;
                    tt[0].var = w ? av : bv; tt[0].c = 1; tt[1].var = x; tt[1].c = -1;
                    if (!jb_cons(B, tt, 2, J_LE, 0, &cs[2])) break;
                    jb_orn(B, o, cs, 3);
                }
            }
            break;
        }
        case IR_CAST:
            // from a FLOAT, checked: trunc(x), which F1 or F2 bounds at the conversion (P3.3)
            if (d->n_operands >= 1 && d->operands[0]->type && d->operands[0]->type->kind == IRT_FLOAT && v->type->kind == IRT_INT &&
                d->wrap == IR_WRAP_CHECK && !d->unchecked) {
                JRange fr; bool hl, hh;
                if (!jg_f2i(g, g->defblk[id], g->defidx[id], d->operands[0], &fr, &hl, &hh)) break;
                const char *o = jb_str(B, "def %%v%lld f2i", id, 0, 0);
                if (hl) { t[0].var = x; t[0].c = -1; jb_atom(B, o, t, 1, J_LE, -fr.lo); }
                if (hh) { t[0].var = x; t[0].c = 1;  jb_atom(B, o, t, 1, J_LE, fr.hi); }
                break;
            }
            // into an integer only: a cast to bool is 1 for any nonzero value (§3's Q row)
            if (d->n_operands < 1 || !jg_is_int(d->operands[0]->type) || v->type->kind != IRT_INT) break;
            if (d->wrap == IR_WRAP_MODULAR || (d->wrap == IR_WRAP_CHECK && d->unchecked)) {   // it wraps (ii_cast)
                jb_wrapto(B, x, jb_val(B, d->operands[0]), v->type, "wrap", id, jb_str(B, "def %%v%lld cast.wrap", id, 0, 0));
                break;
            }
            if (d->wrap != IR_WRAP_CHECK) break;                // `as|` clamps: nothing stated
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
            if (d->n_operands < 2 || !jg_is_int(d->operands[0]->type)) break;
            bool uns = jg_uns(d->operands[0]->type);
            if (!uns && d->unchecked) break;
            // a MODULAR signed quotient or remainder by a constant other than 0 and -1: only MIN / -1
            // wraps (interp.h), so it is the checked one's value (LeikeHeizmann-WST2014-Ex9's wrap port,
            // `x = x /% 2`, had nothing stated for x and no ranking); `/|` and a variable divisor as before
            if (!uns && d->wrap != IR_WRAP_CHECK &&
                (d->wrap != IR_WRAP_MODULAR || !jg_const_of(g, d->operands[1], &c) || c == 0 || c == -1)) break;
            if (!jg_const_of(g, d->operands[1], &c)) {           // a variable divisor: between bounds
                JRange h;
                bool dv = d->op == IR_SDIV || d->op == IR_UDIV;
                const char *o = jb_str(B, dv ? "def %%v%lld div.hull" : "def %%v%lld rem.hull", id, 0, 0);
                if (jg_divrem_hull(g, d, &h)) jb_range(B, x, &h, o);
                // a signed quotient by a variable y: where y >= 1 it is between 0 and x, on x's side (`/`
                // truncates toward zero, interp.h, and y >= 1 leaves no /0 and no MIN / -1), and where
                // y >= 2 and x >= 0, at most half of x (div.half). Disjunctions of three, VC-wide only (s6)
                if (dv && !uns && B->jS < 0 && jg_is_int(d->operands[1]->type)) {
                    int32_t xv0 = jb_val(B, d->operands[0]), yv0 = jb_val(B, d->operands[1]);
                    const char *os = jb_str(B, "def %%v%lld div.sign", id, 0, 0);
                    for (int w = 0; w < 4; w++) {
                        JCons cs[3]; JLT tt[2];
                        tt[0].var = yv0; tt[0].c = 1; if (!jb_cons(B, tt, 1, J_LE, 0, &cs[0])) break;            // y <= 0
                        int pos = w < 2;                                                                       // the x >= 0 side
                        tt[0].var = xv0; tt[0].c = pos ? 1 : -1; if (!jb_cons(B, tt, 1, J_LE, -1, &cs[1])) break;  // x <= -1 | -x <= -1
                        if (w % 2 == 0) { tt[0].var = x; tt[0].c = pos ? -1 : 1; if (!jb_cons(B, tt, 1, J_LE, 0, &cs[2])) break; }   // q >= 0 | q <= 0
                        else { tt[0].var = x; tt[0].c = pos ? 1 : -1; tt[1].var = xv0; tt[1].c = pos ? -1 : 1; if (!jb_cons(B, tt, 2, J_LE, 0, &cs[2])) break; }   // q <= x | x <= q
                        jb_orn(B, os, cs, 3);
                    }
                    {   // y >= 2 and x >= 0: 2q <= x
                        JCons cs[3]; JLT tt[2];
                        tt[0].var = yv0; tt[0].c = 1;
                        if (jb_cons(B, tt, 1, J_LE, 1, &cs[0])) {                                          // y <= 1
                            tt[0].var = xv0; tt[0].c = 1;
                            if (jb_cons(B, tt, 1, J_LE, -1, &cs[1])) {                                     // x <= -1
                                tt[0].var = x; tt[0].c = 2; tt[1].var = xv0; tt[1].c = -1;
                                if (jb_cons(B, tt, 2, J_LE, 0, &cs[2])) jb_orn(B, jb_str(B, "def %%v%lld div.half", id, 0, 0), cs, 3);   // 2q - x <= 0
                            }
                        }
                    }
                }
                // a remainder below its divisor itself (`i % s.len < s.len`): an unsigned one always
                // (its divisor is nonzero where it completes), a signed one where x >= 0 and y >= 1
                JRange ra, rb;
                if (!jg_static_range(g, d->operands[0], &ra, 0) || !jg_static_range(g, d->operands[1], &rb, 0)) break;
                int32_t xv = jb_val(B, d->operands[0]), yv = jb_val(B, d->operands[1]);
                if (!dv && (d->op == IR_UREM || (ra.lo >= 0 && rb.lo >= 1))) {
                    t[0].var = x; t[0].c = 1; t[1].var = yv; t[1].c = -1;
                    jb_atom(B, o, t, 2, J_LE, -1);
                }
                // and the dividend: x >= 0 keeps a remainder within [0, x] (of x's sign, |r| <= |x|);
                // over y in [lo, hi], lo >= 1 (a divisor >= 0 is nonzero where it completes), x = q*y + r
                // with 0 <= r <= y - 1 gives lo*q <= x <= hi*q + hi - 1
                if (ra.lo < 0) break;
                if (!dv) { t[0].var = x; t[0].c = 1; t[1].var = xv; t[1].c = -1; jb_atom(B, o, t, 2, J_LE, 0); break; }
                __int128 lo = rb.lo == 0 ? 1 : rb.lo;
                if (lo < 1) break;
                t[0].var = x; t[0].c = lo; t[1].var = xv; t[1].c = -1; jb_atom(B, o, t, 2, J_LE, 0);
                t[0].var = x; t[0].c = -rb.hi; t[1].var = xv; t[1].c = 1; jb_atom(B, o, t, 2, J_LE, rb.hi - 1);
                break;
            }
            if (c == 0) break;
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
            if (d->op == IR_SHL && d->n_operands >= 2 && !jg_const_of(g, d->operands[1], &k)) {   // by a variable
                JRange h;
                if (jg_op_range(g, d, &h, -1)) jb_range(B, x, &h, jb_str(B, "def %%v%lld shl.hull", id, 0, 0));
                break;
            }
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
        case IR_OR: case IR_XOR: {                             // between bounds from the static ranges
            JRange h;
            if (jg_op_range(g, d, &h, -1)) jb_range(B, x, &h, jb_str(B, d->op == IR_OR ? "def %%v%lld or.hull" : "def %%v%lld xor.hull", id, 0, 0));
            break;
        }
        case IR_AND: {
            __int128 c; int ci = -1;
            for (int k = 0; k < 2 && ci < 0; k++) if (d->n_operands >= 2 && jg_const_of(g, d->operands[k], &c) && c >= 0) ci = k;
            if (ci < 0) {                                      // two variables: between bounds, and at most
                JRange h, ra;                                  // each operand that is >= 0 (`x & (len - 1)`);
                const char *o = jb_str(B, "def %%v%lld and.hull", id, 0, 0);   // one not >= 0 by its
                if (jg_op_range(g, d, &h, -1)) jb_range(B, x, &h, o);    // static range: y <= -1, or
                for (int k = 0; k < 2 && d->n_operands >= 2; k++) {      // 0 <= x & y <= y
                    int32_t y = jb_val(B, d->operands[k]);
                    t[0].var = x; t[0].c = 1; t[1].var = y; t[1].c = -1;
                    if (jg_static_range(g, d->operands[k], &ra, 0) && ra.lo >= 0) { jb_atom(B, o, t, 2, J_LE, 0); continue; }
                    JCons c3[3]; JLT u[1];
                    u[0].var = y; u[0].c = 1;  if (!jb_cons(B, u, 1, J_LE, -1, &c3[0])) break;
                    u[0].var = x; u[0].c = -1; if (!jb_cons(B, u, 1, J_LE, 0, &c3[1])) break;
                    if (!jb_cons(B, t, 2, J_LE, 0, &c3[2])) break;
                    jb_or2(B, o, c3, 1, 2);
                }
                break;
            }
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
        // a call to a function of this module: its RETURN claims (I.181's boundary), the call's
        // result for the returned value and each argument for its parameter, where the argument
        // lands exactly (its landing obligation, owed at this call, licenses it)
        case IR_CALL: {
            IrFunc *h = jg_callee(g, d);
            if (!h || !jg_same_module(h, g->f)) break;
            jb_contracts(B, d, h, x);
            if (B->fail) return;
            // its GUARDED return claims (s7): where each guard argument is a literal equal to its
            // constant, the bound; a literal of another value, nothing; otherwise, VC-wide only,
            // `a1 <= c1 - 1 | a1 >= c1 + 1 | ... | sum <= rhs`, which the solver uses where it
            // shows the arguments are the constants
            for (int32_t gi = 0; gi < g->ngc && !B->fail; gi++) {
                const JGuardClaim *c = &g->gc[gi];
                if (c->fn != h) continue;
                JLT u[JCLAIM_MAXT]; int32_t ga[JGUARD_MAXP]; bool ok = true, lit = true, eq = true;
                for (int k = 0; k < c->nt + c->ng && ok; k++) {
                    int32_t key = k < c->nt ? c->key[k] : c->gkey[k - c->nt];
                    if (k < c->nt) u[k].c = c->coef[k];
                    if (k < c->nt && key == JKEY_RET) { u[k].var = x; continue; }
                    int pk = jg_param_at(h, key);
                    IrParam *pp = h->params;
                    for (int q = 0; q < pk && pp; q++) pp = pp->next;
                    if (pk < 0 || pk >= d->n_operands || !pp || !pp->value ||
                        !jg_lands_exact(d->operands[pk]->type, pp->value->type, d->unchecked)) { ok = false; break; }
                    if (k < c->nt) { u[k].var = jb_val(B, d->operands[pk]); continue; }
                    __int128 cv;
                    if (jg_const_of(g, d->operands[pk], &cv)) { if (cv != c->gval[k - c->nt]) eq = false; }
                    else lit = false;
                    ga[k - c->nt] = jb_val(B, d->operands[pk]);
                }
                if (!ok || (lit && !eq)) continue;
                const char *os = jb_str(B, "contract %%v%lld guarded #%lld", x, gi, 0);
                if (lit) { jb_atom(B, os, u, c->nt, J_LE, c->rhs); continue; }
                if (B->jS >= 0) continue;                          // a disjunct holds no disjunction
                JCons cs[2 * JGUARD_MAXP + 1]; int n = 0;
                for (int k = 0; k < c->ng && !B->fail; k++) {
                    JLT w[1];
                    w[0].var = ga[k]; w[0].c = 1;  if (!jb_cons(B, w, 1, J_LE, c->gval[k] - 1, &cs[n++])) B->fail = true;
                    w[0].var = ga[k]; w[0].c = -1; if (!jb_cons(B, w, 1, J_LE, -c->gval[k] - 1, &cs[n++])) B->fail = true;
                }
                if (B->fail || !jb_cons(B, u, c->nt, J_LE, c->rhs, &cs[n++])) { B->fail = true; break; }
                jb_orn(B, os, cs, n);
            }
            break;
        }
        // bit counts: POPCOUNT in [0, w]; CTZ and CLZ in [0, w - 1], their argument owed nonzero
        case IR_POPCOUNT: case IR_CTZ: case IR_CLZ: {
            if (d->n_operands < 1 || !d->operands[0]->type || d->operands[0]->type->kind != IRT_INT) break;
            int w = d->operands[0]->type->bits;
            const char *o = jb_str(B, d->op == IR_POPCOUNT ? "def %%v%lld popcount" : "def %%v%lld bitcount", id, 0, 0);
            t[0].var = x; t[0].c = -1; jb_atom(B, o, t, 1, J_LE, 0);
            t[0].var = x; t[0].c = 1;  jb_atom(B, o, t, 1, J_LE, d->op == IR_POPCOUNT ? w : w - 1);
            if (w < 1 || w > 64) break;
            // and the operand m, its bits at width w: m >= 0 (unsigned, or by its static range), or not
            JRange rm;
            int32_t mv = jb_val(B, d->operands[0]);
            bool nn = jg_uns(d->operands[0]->type) || (jg_static_range(g, d->operands[0], &rm, 0) && rm.lo >= 0);
            __int128 W = (__int128)1 << w, H = (__int128)1 << (w - 1);
            if (d->op == IR_POPCOUNT) {                          // no bit set only in 0: m <= (2^w - 1)*p
                t[0].var = mv; t[0].c = 1; t[1].var = x; t[1].c = nn ? -(W - 1) : -(H - 1);
                jb_atom(B, o, t, 2, J_LE, 0);                    // a negative m has its sign bit:
                if (!nn) {                                       // -m <= 2^(w-1)*p (jb_cons sorted t)
                    t[0].var = mv; t[0].c = -1; t[1].var = x; t[1].c = -H;
                    jb_atom(B, o, t, 2, J_LE, 0);
                }
                break;
            }
            // m != 0 where it completes: ctz = k when 2^k divides m, so m >= 2^k; clz = k when m's top
            // bit is w-1-k, 2^(w-1-k) <= m <= 2^(w-k) - 1. One disjunct per k (not inside a join's
            // disjunct, which cannot hold a disjunction)
            if (!nn || B->jS >= 0) break;
            JHyp *h = jb_hyp(B, o);
            if (!h) break;
            h->disj = true; h->ndis = w;
            if (!(h->dis = JG_NEW(g, JConj, w))) { B->fail = true; break; }
            for (int k = 0; k < w && !B->fail; k++) {
                bool cz = d->op == IR_CLZ;
                if (!(h->dis[k].c = JG_NEW(g, JCons, cz ? 3 : 2))) { B->fail = true; break; }
                h->dis[k].nc = cz ? 3 : 2;
                t[0].var = x; t[0].c = 1; jb_cons(B, t, 1, J_EQ, k, &h->dis[k].c[0]);
                t[0].var = mv; t[0].c = -1; jb_cons(B, t, 1, J_LE, -((__int128)1 << (cz ? w - 1 - k : k)), &h->dis[k].c[1]);
                if (cz) { t[0].var = mv; t[0].c = 1; jb_cons(B, t, 1, J_LE, ((__int128)1 << (w - k)) - 1, &h->dis[k].c[2]); }
            }
            break;
        }
        default: break;                                        // its range only
    }
}

// The comparison an ICMP states, true or false, over two variables; false if it is not stated
// exactly (an unsigned predicate with a signed operand that may be negative: §2's Q row).
// The operands of an unsigned comparison that are split on their sign (jb_cmpk): the signed ones
// that are not a constant >= 0. Their count, or -1 where one is wider than 64 bits (refused).
static int jg_cmp_split(JG *g, const IrInstr *ic, int sk[2]) {
    IrCmp c = ic->aux.cmp;
    int ns = 0;
    if (!(c == IR_CMP_ULT || c == IR_CMP_ULE || c == IR_CMP_UGT || c == IR_CMP_UGE)) return 0;
    for (int k = 0; k < 2; k++) {
        IrValue *o = ic->operands[k];
        if (o->type->kind == IRT_BOOL || !o->type->is_signed) continue;
        IrInstr *od = o->id >= 0 && o->id < g->nv ? g->def[o->id] : NULL;
        if (od && od->op == IR_CONST && jg_const(od) >= 0) continue;
        if (o->type->bits < 1 || o->type->bits > 64) return -1;
        sk[ns++] = k;
    }
    return ns;
}
// A comparison as a fact; `cas` >= 0 picks ONE case of a fact that is a disjunction (`!=`, an
// unsigned split), stated as conjuncts: how a join's path states it inside its disjunct.
static bool jb_cmpk(JB *B, IrInstr *ic, bool truth, const char *origin, int cas) {
    if (!ic || ic->op != IR_ICMP || ic->n_operands < 2) return false;
    IrValue *a = ic->operands[0], *b = ic->operands[1];
    if (!jg_is_int(a->type) || !jg_is_int(b->type)) return false;
    IrCmp c = ic->aux.cmp;
    // An UNSIGNED predicate compares its operands as uint64 (emit_c.h, interp.h's ii_cmp_scalar):
    // a signed operand below 0 is itself + 2^64 there. A signed one that is a constant >= 0 is its
    // value; any other is split on its sign, one disjunct per case (`(i + 15) <u a.len` with i + 15
    // an i34): (x >= 0 and x op y) or (x <= -1 and x + 2^64 op y).
    int sk[2], ns = jg_cmp_split(B->g, ic, sk);
    if (ns < 0) return false;
    if (ns) {
        if (B->jS >= 0 && (cas < 0 || cas >= (1 << ns))) return false;   // a disjunction: one case only there
        if (!truth) c = c == IR_CMP_ULT ? IR_CMP_UGE : c == IR_CMP_ULE ? IR_CMP_UGT : c == IR_CMP_UGT ? IR_CMP_ULE : IR_CMP_ULT;
        int32_t va = jb_val(B, a), vb = jb_val(B, b);
        if (B->jS >= 0) {                                      // case `cas`, as conjuncts of the disjunct
            __int128 sh[2] = { 0, 0 };
            for (int q = 0; q < ns; q++) {
                bool neg = (cas >> q) & 1;
                JLT u[1]; u[0].var = sk[q] ? vb : va; u[0].c = neg ? 1 : -1;
                jb_atom(B, origin, u, 1, J_LE, neg ? -1 : 0);
                if (neg) sh[sk[q]] = (__int128)1 << 64;
            }
            JLT u[2]; __int128 k0 = sh[1] - sh[0];
            bool less = c == IR_CMP_ULT || c == IR_CMP_ULE, strict = c == IR_CMP_ULT || c == IR_CMP_UGT;
            u[0].var = va; u[0].c = less ? 1 : -1; u[1].var = vb; u[1].c = less ? -1 : 1;
            jb_atom(B, origin, u, 2, J_LE, (less ? k0 : -k0) - (strict ? 1 : 0));
            return true;
        }
        JHyp *h = jb_hyp(B, origin);
        if (!h) return false;
        h->disj = true; h->ndis = 1 << ns;
        if (!(h->dis = JG_NEW(B->g, JConj, h->ndis))) { B->fail = true; return false; }
        for (int d = 0; d < h->ndis; d++) {
            JConj *cj = &h->dis[d];
            cj->nc = ns + 1;
            if (!(cj->c = JG_NEW(B->g, JCons, cj->nc))) { B->fail = true; return false; }
            __int128 sh[2] = { 0, 0 };                         // what each operand gains as a uint64
            for (int q = 0; q < ns; q++) {
                bool neg = (d >> q) & 1;
                JLT u[1]; u[0].var = sk[q] ? vb : va; u[0].c = neg ? 1 : -1;
                jb_cons(B, u, 1, J_LE, neg ? -1 : 0, &cj->c[q]);
                if (neg) sh[sk[q]] = (__int128)1 << 64;
            }
            // (a + sh0) op (b + sh1): a - b op sh1 - sh0
            JLT u[2]; __int128 k0 = sh[1] - sh[0];
            bool less = c == IR_CMP_ULT || c == IR_CMP_ULE, strict = c == IR_CMP_ULT || c == IR_CMP_UGT;
            u[0].var = va; u[0].c = less ? 1 : -1; u[1].var = vb; u[1].c = less ? -1 : 1;
            jb_cons(B, u, 2, J_LE, (less ? k0 : -k0) - (strict ? 1 : 0), &cj->c[ns]);
        }
        return true;
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
            if (B->jS >= 0) {                                  // inside a disjunct: case `cas` only
                if (cas < 0 || cas > 1) return false;
                JLT u[2]; u[0] = t[0]; u[1] = t[1]; u[0].c = cas == 0 ? 1 : -1; u[1].c = cas == 0 ? -1 : 1;
                jb_atom(B, origin, u, 2, J_LE, -1);
                break;
            }
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
// A CONDITION as a fact: a comparison (jb_cmp), or a bool value itself, which is 1 where it held
// and 0 where it did not (a bool is 0 or 1, and a branch takes its first target on nonzero).
// `ic` is the comparison that defines it, or NULL. False when nothing is stated.
static IrInstr *jg_cond_cmp(JG *g, IrValue *cv) {
    IrInstr *ic = cv && cv->id >= 0 && cv->id < g->nv ? g->def[cv->id] : NULL;
    return ic && ic->op == IR_ICMP ? ic : NULL;
}
static bool jg_cond_ok(JG *g, IrValue *cv) { return cv && cv->id >= 0 && cv->id < g->nv; }   // jb_cond decides
// A condition the IR FIXES: a bool or integer CONST (nonzero is true, as a branch reads it), or a
// comparison of two constant operands (jg_const_of), read as jb_cmp states one: a signed or an
// equality predicate on the values, an unsigned one only where both are non-negative (there the
// orders agree). 1 or 0; -1 when it is not fixed.
static int jg_cond_value(JG *g, IrValue *cv) {
    IrInstr *d = cv && cv->id >= 0 && cv->id < g->nv ? g->def[cv->id] : NULL;
    if (!d) return -1;
    if (d->op == IR_CONST) return d->result && jg_is_int(d->result->type) ? jg_const(d) != 0 : -1;
    __int128 a, b;
    if (d->op != IR_ICMP || d->n_operands < 2 || !jg_const_of(g, d->operands[0], &a) || !jg_const_of(g, d->operands[1], &b))
        return -1;
    switch (d->aux.cmp) {
        case IR_CMP_EQ: return a == b;   case IR_CMP_NE: return a != b;
        case IR_CMP_SLT: return a < b;   case IR_CMP_SLE: return a <= b;
        case IR_CMP_SGT: return a > b;   case IR_CMP_SGE: return a >= b;
        default: break;
    }
    if (a < 0 || b < 0) return -1;
    switch (d->aux.cmp) {
        case IR_CMP_ULT: return a < b;   case IR_CMP_ULE: return a <= b;
        case IR_CMP_UGT: return a > b;   case IR_CMP_UGE: return a >= b;
        default: return -1;
    }
}
// How many cases a condition is as a fact on an edge: 2 for a `!=` (and a false `==`), 2^n for an
// unsigned comparison split on n signs, else 1 (jb_cmpk's rules, read without stating anything).
static int jg_cond_ncases(JG *g, IrValue *cv, bool truth) {
    if (jg_cond_value(g, cv) >= 0) return 1;
    IrInstr *ic = jg_cond_cmp(g, cv);
    if (!ic || ic->n_operands < 2 || !jg_is_int(ic->operands[0]->type) || !jg_is_int(ic->operands[1]->type)) return 1;
    int sk[2], ns = jg_cmp_split(g, ic, sk);
    if (ns < 0) return 1;
    if (ns > 0) return 1 << ns;
    IrCmp c = ic->aux.cmp;
    return (truth && c == IR_CMP_NE) || (!truth && c == IR_CMP_EQ) ? 2 : 1;
}
static bool jb_condk(JB *B, IrValue *cv, bool truth, const char *origin, int cas);
static bool jb_cond(JB *B, IrValue *cv, bool truth, const char *origin) { return jb_condk(B, cv, truth, origin, -1); }
static bool jb_condk(JB *B, IrValue *cv, bool truth, const char *origin, int cas) {
    int k = jg_cond_value(B->g, cv);
    if (k >= 0) {                                              // fixed: no variable is needed
        if ((k == 1) == truth) return false;                   // it holds on this edge: nothing new
        JLT none[1];                                           // it fails: the edge is never taken
        jb_atom(B, origin, none, 0, J_LE, -1);
        return true;
    }
    IrInstr *ic = jg_cond_cmp(B->g, cv);
    if (ic) return jb_cmpk(B, ic, truth, origin, cas);
    if (!cv || !cv->type || cv->type->kind != IRT_BOOL) return false;
    JLT t[1]; t[0].var = jb_val(B, cv); t[0].c = 1;
    jb_atom(B, origin, t, 1, J_EQ, truth ? 1 : 0);
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
        d->operands[0]->id >= 0 && d->operands[0]->id < B->g->nv) {
        int32_t sv = d->operands[0]->id;
        // named as jb_len names it: a load of a slice cell's STORE version is the slice stored
        for (int hop = 0; hop < 64 && B->g->loadver[sv] >= 0; hop++) {
            JVer *ver = &B->g->ver[B->g->loadver[sv]];
            if (ver->kind != JV_STORE || !ver->stored || !ver->stored->type || ver->stored->type->kind != IRT_SLICE ||
                ver->stored->id < 0 || ver->stored->id >= B->g->nv) break;
            sv = ver->stored->id;
        }
        return B->vlen[sv] >= 0 || (B->g->loadver[sv] >= 0 && B->vlenv[B->g->loadver[sv]] >= 0);
    }
    return false;
}
static void jb_drain(JB *B) { while (B->qh < B->qt && !B->fail) jb_state(B, B->queue[B->qh++]); }
// Is a condition's operand about the VC: in it, or computed exactly from what is (a guard on one
// `i + j` and an index computing `i + j` again are two SSA values with the same operands), or a
// quotient or remainder, whose statement names its dividend (`i < n / 2` and a goal on n)? Depth 3.
static bool jb_rel(JB *B, IrValue *v, int depth) {
    if (jb_has(B, v)) return true;
    if (depth <= 0 || !v || v->id < 0 || v->id >= B->g->nv) return false;
    IrInstr *d = B->g->def[v->id];
    if (!d || !(d->op == IR_ADD || d->op == IR_SUB || d->op == IR_NEG || d->op == IR_MUL || d->op == IR_CAST ||
                d->op == IR_SDIV || d->op == IR_UDIV || d->op == IR_SREM || d->op == IR_UREM)) return false;
    for (int k = 0; k < d->n_operands; k++) if (jb_rel(B, d->operands[k], depth - 1)) return true;
    return false;
}

// The facts that dominate the point: branch edges into single-predecessor blocks on the dominator
// chain, and ASSUMEs before the point. Each is added once one of its operands is in the VC, until
// nothing more joins: a condition about variables the goal never reaches costs only size. A
// condition the IR fixes (jg_cond_value) is added at once: false on its edge, the point is never
// reached (`while 0 != 0 { x = x + 1 }`), and everything there is implied.
// ★ A JOIN (R7). Control reached the join S through one of its predecessors, so ONE of these holds:
// for predecessor P, the conditions of the edges on its path from S's dominator, and, for each
// cell S versions afresh, `join version = the version P ends with`. Each is a disjunct, and a
// value or version made on that path is described inside it (jb_global_at). A loop header is not
// a join here (its back edge's values are a later iteration's: the loop cut decides it), and a
// path with no fact at all makes the disjunction say nothing, so none is stated.
// A join J's fresh versions, each equal to the version its predecessor P ends with: inside the
// current disjunct, for the versions the VC names (S's own are VC-wide variables; an inner join's
// are local to this path).
static void jb_join_eqs(JB *B, int32_t J, int32_t P, bool outer) {
    JG *g = B->g;
    for (int32_t c = 0; c < g->ncells; c++) {
        int32_t jv = g->instate[(size_t)J * (size_t)g->ncells + (size_t)c];
        if (jv < 0 || g->ver[jv].kind != JV_JOIN || g->ver[jv].blk != J || B->vver[jv] < 0) continue;
        int32_t pv = g->outstate[(size_t)P * (size_t)g->ncells + (size_t)c];
        if (pv < 0) continue;
        JLT t[2]; t[0].var = outer ? B->vver[jv] : jb_ver(B, jv); t[0].c = 1; t[1].var = jb_ver(B, pv); t[1].c = -1;
        jb_atom(B, NULL, t, 2, J_EQ, 0);
    }
}
// The PATHS from S's dominator d0 into S, each its edges X -> Y from S upward with the case its
// condition takes (-1: the whole condition). A path branches at an inner join (one per forward
// predecessor) and at a condition that is a disjunction (one per case), so no disjunct holds a
// disjunction. False past JJOIN_MAXP paths or JJOIN_MAXL edges, at a loop header or a back edge.
enum { JJOIN_MAXP = 32, JJOIN_MAXL = 64 };
typedef struct { int32_t x, y, cas; } JJStep;
typedef struct { JJStep *st; int32_t n; } JPathE;
// A path's wrap cases (s5), beside the paths only where a join expands: a join with none allocates
// nothing more (JPathE is allocated JJOIN_MAXP at a time, at every join of every VC)
typedef struct { int32_t *wid, *wk, nw; } JPathW;
static bool jb_join_paths(JB *B, int32_t d0, IrBlock *Y, IrBlock *X, JJStep *cur, int32_t n, JPathE *out, int32_t *nout) {
    JG *g = B->g;
    if (n >= JJOIN_MAXL) return false;
    bool cond = X->term.kind == IR_TERM_BR_COND && X->term.a != X->term.b && jg_cond_ok(g, X->term.cond);
    int nc = cond ? jg_cond_ncases(g, X->term.cond, X->term.a == Y) : 1;
    for (int cs = 0; cs < nc; cs++) {
        cur[n].x = X->id; cur[n].y = Y->id; cur[n].cas = nc > 1 ? cs : -1;
        if (X->id == d0 || !X->preds) {                        // a whole path
            if (*nout == JJOIN_MAXP) return false;
            JJStep *cp = JG_NEW(g, JJStep, n + 1);
            if (!cp) { B->fail = true; return false; }
            memcpy(cp, cur, sizeof *cp * (size_t)(n + 1));
            out[*nout].st = cp; out[*nout].n = n + 1; (*nout)++;
            continue;
        }
        if (g->npred[X->id] == 1) { if (!jb_join_paths(B, d0, X, X->preds->block, cur, n + 1, out, nout)) return false; continue; }
        if (g->inloop[(size_t)X->id * (size_t)g->nb + (size_t)X->id]) return false;   // a loop header: not crossed
        for (IrEdge *e = X->preds; e; e = e->next) {
            if (g->rpoix[e->block->id] < 0) continue;
            if (g->rpoix[e->block->id] >= g->rpoix[X->id]) return false;
            if (!jb_join_paths(B, d0, X, e->block, cur, n + 1, out, nout)) return false;
        }
    }
    return true;
}
// s5: a path's WRAPPING ops (jg_wrap_op, two or three cases) made in its own blocks (every block of
// it but the dominator d0, whose values are VC-wide) whose result a store on the path writes into a
// cell: jb_wrap states none of them inside the path's disjunct unless the path says which case it
// took. So each such path becomes one path per combination of their cases, the case kept with it:
// `path and (case 1 or case 2)` is `(path and case 1) or (path and case 2)`, and the cases are
// every k the operands' ranges allow. False, the paths as they were, past JJOIN_MAXP in all or past
// JWRAP_MAXW such ops on one path; `*pw` the cases by path where it expands, else NULL.
enum { JWRAP_MAXW = 8 };
static bool jb_join_wraps(JB *B, int32_t d0, JPathE *paths, int32_t *npath, JPathW **pw) {
    JG *g = B->g;
    if (!g->wraps) {                                         // once per function: most have none
        g->wraps = 1;
        for (IrBlock *b = g->f->blocks; b && g->wraps == 1; b = b->next)
            for (IrInstr *w = b->instrs; w && g->wraps == 1; w = w->next) {
                __int128 lo, hi, M; JRange rr;
                if (jg_wrap_op(w) && w->result->id >= 0 && jg_wrap_ks(g, w, &lo, &hi, &M, &rr) && lo != hi) g->wraps = 2;
            }
    }
    if (g->wraps == 1) return true;
    int32_t ids[JJOIN_MAXP][JWRAP_MAXW]; __int128 klo[JJOIN_MAXP][JWRAP_MAXW]; int32_t nk[JJOIN_MAXP][JWRAP_MAXW], nw[JJOIN_MAXP];
    int64_t total = 0;
    bool any = false;
    // the values a path's stores into cells are computed from, on the path: a wrapping op whose
    // result only FEEDS a stored one (`q = (q -% y) -% 1`: the inner op) is as unstated inside the
    // disjunct as a stored one, and the stored value is then free (LeikeHeizmann-TACAS2014-Ex1's
    // wrap port had no ranking: `q -% y` said nothing on either path)
    // one scratch per function, not per join and VC (a per-call allocation fills the region, s5)
    if (!g->wneed && !(g->wneed = JG_NEW(g, uint8_t, g->nv > 0 ? g->nv : 1))) { B->fail = true; return false; }
    uint8_t *need = g->wneed;
    for (int32_t p = 0; p < *npath; p++) {
        const JPathE *pe = &paths[p];
        nw[p] = 0;
        memset(need, 0, (size_t)(g->nv > 0 ? g->nv : 1));
        for (int32_t j = 0; j < pe->n; j++) {
            IrBlock *Y = g->blk[pe->st[j].x];
            if (Y->id == d0) continue;
            for (IrInstr *st = Y->instrs; st; st = st->next)
                if (st->op == IR_STORE && st->n_operands >= 2 && st->operands[1] && st->operands[0] &&
                    st->operands[0]->id >= 0 && st->operands[0]->id < g->nv && g->cell[st->operands[0]->id] >= 0 &&
                    st->operands[1]->id >= 0 && st->operands[1]->id < g->nv) need[st->operands[1]->id] = 1;
        }
        for (bool grew = true; grew; ) {
            grew = false;
            for (int32_t j = 0; j < pe->n; j++) {
                IrBlock *Y = g->blk[pe->st[j].x];
                if (Y->id == d0) continue;
                for (IrInstr *u = Y->instrs; u; u = u->next) {
                    if (!u->result || u->result->id < 0 || u->result->id >= g->nv || !need[u->result->id]) continue;
                    for (int32_t o = 0; o < u->n_operands; o++) {
                        IrValue *ov = u->operands[o];
                        if (ov && ov->id >= 0 && ov->id < g->nv && !need[ov->id]) { need[ov->id] = 1; grew = true; }
                    }
                }
            }
        }
        for (int32_t i = 0; i < pe->n; i++) {
            IrBlock *X = g->blk[pe->st[i].x];
            if (X->id == d0) continue;
            for (IrInstr *w = X->instrs; w; w = w->next) {
                __int128 lo, hi, M; JRange rr;
                if (!jg_wrap_op(w) || w->result->id < 0 || !jg_wrap_ks(g, w, &lo, &hi, &M, &rr) || lo == hi) continue;
                if (w->result->id >= g->nv || !need[w->result->id]) continue;   // stored, or what a stored value is made of
                if (nw[p] == JWRAP_MAXW) return false;
                ids[p][nw[p]] = w->result->id; klo[p][nw[p]] = lo; nk[p][nw[p]] = (int32_t)(hi - lo + 1); nw[p]++;
            }
        }
        int64_t prod = 1;
        for (int32_t q = 0; q < nw[p]; q++) prod *= nk[p][q];
        total += prod; any |= nw[p] > 0;
        if (total > JJOIN_MAXP) return false;
    }
    if (!any) return true;
    JPathE out[JJOIN_MAXP]; int32_t n = 0;
    JPathW *w = JG_NEW(g, JPathW, total);
    if (!w) { B->fail = true; return false; }
    for (int32_t p = 0; p < *npath; p++) {
        int64_t prod = 1;
        for (int32_t q = 0; q < nw[p]; q++) prod *= nk[p][q];
        for (int64_t c = 0; c < prod; c++) {
            out[n] = paths[p];
            w[n].nw = nw[p]; w[n].wid = w[n].wk = NULL;
            if (nw[p]) {
                w[n].wid = JG_NEW(g, int32_t, nw[p]); w[n].wk = JG_NEW(g, int32_t, nw[p]);
                if (!w[n].wid || !w[n].wk) { B->fail = true; return false; }
                int64_t r = c;
                for (int32_t q = 0; q < nw[p]; q++) { w[n].wid[q] = ids[p][q]; w[n].wk[q] = (int32_t)(klo[p][q] + r % nk[p][q]); r /= nk[p][q]; }
            }
            n++;
        }
    }
    memcpy(paths, out, sizeof *out * (size_t)n); *npath = n; *pw = w;
    return true;
}
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
    // one disjunct per PATH into S where they can all be told (inner joins and split conditions
    // distributed), else one per predecessor, stopping at an inner join, as before
    JPathE *paths = JG_NEW(g, JPathE, JJOIN_MAXP); JJStep *cur = JG_NEW(g, JJStep, JJOIN_MAXL);
    if (!paths || !cur) { B->fail = true; return false; }
    int32_t npath = 0; bool all = true;
    for (int k = 0; k < np && all; k++) all = jb_join_paths(B, d0, S, pr[k], cur, 0, paths, &npath);
    if (B->fail) return false;
    JPathW *pw = NULL;                                       // the paths' wrap cases, where it expands
    if (all) jb_join_wraps(B, d0, paths, &npath, &pw);       // past the caps, the paths as they were
    if (B->fail) return false;
    int32_t nd = all ? npath : np;
    JConj *dis = JG_NEW(g, JConj, nd);
    int32_t *dpred = JG_NEW(g, int32_t, nd);
    if (!dis || !dpred) { B->fail = true; return false; }
    for (int32_t k = 0; k < nd && !B->fail; k++) {
        B->jS = s; B->sink = NULL; B->nsink = B->csink = 0; B->dgen++;
        if (all) {
            const JPathE *pe = &paths[k];
            dpred[k] = pe->st[0].x;
            if (pw) { B->wid = pw[k].wid; B->wk = pw[k].wk; B->nw = pw[k].nw; }
            for (int32_t i = 0; i < pe->n && !B->fail; i++) {
                IrBlock *X = g->blk[pe->st[i].x], *Y = g->blk[pe->st[i].y];
                if (Y->id == s || g->npred[Y->id] >= 2) jb_join_eqs(B, Y->id, X->id, Y->id == s);
                if (X->term.kind == IR_TERM_BR_COND && X->term.a != X->term.b && jg_cond_ok(g, X->term.cond))
                    jb_condk(B, X->term.cond, X->term.a == Y, "", pe->st[i].cas);
            }
        } else {
            dpred[k] = pr[k]->id;
            jb_join_eqs(B, s, pr[k]->id, true);
            IrBlock *Y = S, *X = pr[k];
            for (int hop = 0; hop < 64; hop++) {
                if (X->term.kind == IR_TERM_BR_COND && X->term.a != X->term.b && jg_cond_ok(g, X->term.cond))
                    jb_cond(B, X->term.cond, X->term.a == Y, "");
                if (X->id == d0 || g->npred[X->id] != 1 || !X->preds) break;
                Y = X; X = X->preds->block;
            }
        }
        dis[k].c = B->sink; dis[k].nc = B->nsink;
        B->jS = -1; B->nw = 0;
        if (dis[k].nc == 0) return false;
    }
    B->jS = -1;
    if (B->fail) return false;
    JHyp *h = jb_hyp(B, jb_str(B, "join bb%lld", s, 0, 0));
    if (!h) return false;
    h->disj = true; h->ndis = nd; h->dis = dis;
    if (B->njr == B->cjr) {                                  // which predecessor each disjunct is
        int32_t nc = B->cjr ? B->cjr * 2 : 8;
        int32_t *a = JG_NEW(g, int32_t, nc), *b = JG_NEW(g, int32_t, nc); int32_t **c = JG_NEW(g, int32_t *, nc);
        if (!a || !b || !c) { B->fail = true; return false; }
        if (B->njr) { memcpy(a, B->jhyp, sizeof *a * (size_t)B->njr); memcpy(b, B->jblk, sizeof *b * (size_t)B->njr); memcpy(c, B->jpred, sizeof *c * (size_t)B->njr); }
        B->jhyp = a; B->jblk = b; B->jpred = c; B->cjr = nc;
    }
    B->jhyp[B->njr] = B->nhyps - 1; B->jblk[B->njr] = s; B->jpred[B->njr] = dpred; B->njr++;   // several may share one
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
        if (X->term.kind != IR_TERM_BR_COND || !jg_cond_ok(g, X->term.cond)) continue;
        IrInstr *ic = jg_cond_cmp(g, X->term.cond);
        if (ic ? jb_rel(B, ic->operands[0], 3) || jb_rel(B, ic->operands[1], 3) : jb_rel(B, X->term.cond, 3)) return true;
    }
    return false;
}

typedef struct { IrValue *cv; bool truth; const char *origin; bool used; } JFact;
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
            // and ASSERTs: a program past one has its condition true, its obligation owed there
            if ((i->op != IR_ASSUME && i->op != IR_ASSERT) || i->n_operands < 1 || !jg_cond_ok(g, i->operands[0])) continue;
            if (nf == cf) { JFact *n2 = JG_NEW(g, JFact, cf * 2); if (!n2) { B->fail = true; return; } memcpy(n2, F, sizeof *F * (size_t)nf); F = n2; cf *= 2; }
            F[nf].cv = i->operands[0]; F[nf].truth = true; F[nf].used = false;
            F[nf].origin = i->op == IR_ASSERT ? jb_str(B, "assert bb%lld.%lld", s, k, 0)
                         : jb_str(B, i->aux.imm == 1 ? "assume.trusted bb%lld.%lld" : "assume bb%lld.%lld", s, k, 0);
            nf++;
        }
        if (s == g->f->entry->id) break;
        if (g->npred[s] == 1 && S->preds) {
            IrBlock *P = S->preds->block;
            if (P->term.kind == IR_TERM_BR_COND && P->term.a != P->term.b && jg_cond_ok(g, P->term.cond)) {
                if (nf == cf) { JFact *n2 = JG_NEW(g, JFact, cf * 2); if (!n2) { B->fail = true; return; } memcpy(n2, F, sizeof *F * (size_t)nf); F = n2; cf *= 2; }
                F[nf].cv = P->term.cond; F[nf].truth = P->term.a == S; F[nf].used = false;
                F[nf].origin = jb_str(B, "guard bb%lld->bb%lld", P->id, s, 0);
                nf++;
            }
        }
        if (g->idom[s] == s) break;
    }
    // a claim's obligation is owed on ONE edge out of its block: that edge's condition holds
    if (B->to >= 0) {
        IrBlock *P = g->blk[B->blk];
        if (P->term.kind == IR_TERM_BR_COND && P->term.a != P->term.b && jg_cond_ok(g, P->term.cond)) {
            if (nf == cf) { JFact *n2 = JG_NEW(g, JFact, cf * 2); if (!n2) { B->fail = true; return; } memcpy(n2, F, sizeof *F * (size_t)nf); F = n2; cf *= 2; }
            F[nf].cv = P->term.cond; F[nf].truth = P->term.a->id == B->to; F[nf].used = false;
            F[nf].origin = jb_str(B, "edge bb%lld->bb%lld", P->id, B->to, 0);
            nf++;
        }
    }
    for (bool more = true; more && !B->fail; ) {
        more = false;
        jb_drain(B);
        for (int32_t i = 0; i < nf; i++) {
            if (F[i].used) continue;
            IrInstr *ic = jg_cond_cmp(g, F[i].cv);
            if (!B->allrel && jg_cond_value(g, F[i].cv) < 0 &&  // a fixed one is about every VC
                (ic ? !jb_rel(B, ic->operands[0], 3) && !jb_rel(B, ic->operands[1], 3) : !jb_rel(B, F[i].cv, 3))) continue;
            F[i].used = true;
            if (jb_cond(B, F[i].cv, F[i].truth, F[i].origin)) more = true;
        }
        for (int32_t i = 0; i < nj && !B->fail; i++) {
            if (jused[i]) continue;
            jb_drain(B);
            if (!B->allrel && !jb_join_relevant(B, J[i])) continue;
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
    B.vlenv = JG_NEW(g, int32_t, g->nver ? g->nver : 1);
    B.cst = JG_NEW(g, int32_t, g->ncl ? g->ncl : 1);
    if (!B.vval || !B.vlen || !B.vver || !B.vlenv || !B.cst) { B.fail = true; return B; }
    for (int32_t i = 0; i < g->nv; i++) B.vval[i] = B.vlen[i] = -1;
    for (int32_t i = 0; i < g->nver; i++) B.vver[i] = B.vlenv[i] = -1;
    for (int32_t i = 0; i < g->ncl; i++) B.cst[i] = -2;
    return B;
}
static bool jb_finish(JB *B, IrInstr *at, const char *kind, int ordinal, JCons *goal, JObl *o) {
    // a goal of no variable holds only where its point is never reached (`return 5 /% 0` past a
    // test the path cannot pass): every dominating fact and join is about it, not only those that
    // name its variables. A true one (a path's 0 <= 0) needs none
    if (goal) {                                                // its value where every variable is a constant
        __int128 sum = 0; bool fixed = true;
        for (int32_t i = 0; i < goal->nt && fixed; i++) {
            int32_t v = goal->t[i].var; __int128 c;
            IrValue *gv = B->kind[v] == 0 ? jg_value(B->g, B->key[v]) : NULL;
            fixed = gv && jg_const_of(B->g, gv, &c) && goal->t[i].coef.n <= 1;   // |coef| < 2^32: no overflow
            if (fixed) sum += c * jg_int_get(&goal->t[i].coef);
        }
        __int128 r = jg_int_get(&goal->rhs);
        if (fixed) B->allrel = goal->op == J_LE ? !(sum <= r) : goal->op == J_EQ ? sum != r : sum == r;
    }
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
            k2[i] = B->kind[i] == 1 || B->kind[i] == 4 ? g->cellv[g->ver[B->key[i]].cell]->id : B->key[i];
        o->vkind = B->kind; o->vkey = k2; o->vtype = B->vtype;
        int32_t *off = JG_NEW(g, int32_t, B->nvars ? B->nvars : 1);
        if (!off) return false;
        for (int32_t i = 0; i < B->nvars; i++) {
            int32_t cc = B->kind[i] == 1 || B->kind[i] == 4 ? g->ver[B->key[i]].cell : -1, er, ek;
            off[i] = cc >= 0 && g->csyn[cc] ? g->coff[cc] : -1;
            if (cc >= 0 && !g->csyn[cc] && B->kind[i] == 1 && jg_elem_at(g, cc, &er, &ek)) { k2[i] = er; off[i] = ek; }
        }
        o->voff = off;
        int32_t *aux = JG_NEW(g, int32_t, B->nvars ? B->nvars : 1);
        uint8_t *cur = JG_NEW(g, uint8_t, B->nvars ? B->nvars : 1);
        if (!aux || !cur) return false;
        for (int32_t i = 0; i < B->nvars; i++) {
            aux[i] = -1; cur[i] = 0;
            if (B->kind[i] != 1 && B->kind[i] != 4) continue;
            JVer *v = &g->ver[B->key[i]];
            if (v->kind == JV_STORE && (B->kind[i] == 4 ? v->stored && v->stored->type && v->stored->type->kind == IRT_SLICE : jg_store_exact(v)))
                aux[i] = v->stored->id;
            // the cell's version at the point: the block's entry version, then the versions its
            // instructions before the point make (a store's, or a havoc's)
            int32_t cv = g->instate[(size_t)B->blk * (size_t)g->ncells + (size_t)v->cell];
            for (int32_t j = 0; j < g->nver; j++)
                if (g->ver[j].idx >= 0 && g->ver[j].cell == v->cell && g->ver[j].blk == B->blk && g->ver[j].idx < B->idx) cv = j;
            cur[i] = cv == B->key[i];
        }
        o->vaux = aux; o->vcur = cur; o->vrec = B->vrec;
        // an older version no store gives: a loop's ENTRY version where the point is in that loop
        // (the cell held it as the edge E -> H was taken: ventry), else -1
        int32_t *ent = JG_NEW(g, int32_t, B->nvars ? B->nvars : 1), *ente = JG_NEW(g, int32_t, B->nvars ? B->nvars : 1);
        if (!ent || !ente) return false;
        for (int32_t i = 0; i < B->nvars; i++) {
            ent[i] = ente[i] = -1;
            if (B->kind[i] != 1 || cur[i] || aux[i] >= 0) continue;
            // a header's own version (a HEAD term's, or a back edge's pre value): what the cell held
            // as the header was last entered, by any edge (ente -1)
            if (g->ver[B->key[i]].kind == JV_HEAD && jg_loop_header(g, g->ver[B->key[i]].blk)) { ent[i] = g->ver[B->key[i]].blk; continue; }
            int32_t cell = g->ver[B->key[i]].cell;
            for (int32_t H = 0; H < g->nb && ent[i] < 0; H++) {
                if (!g->blk[H] || !g->inloop[(size_t)H * (size_t)g->nb + (size_t)H] || !g->inloop[(size_t)H * (size_t)g->nb + (size_t)B->blk]) continue;
                int32_t E = jg_entry_pred(g, H);
                if (E >= 0 && g->outstate[(size_t)E * (size_t)g->ncells + (size_t)cell] == B->key[i]) { ent[i] = H; ente[i] = E; }
            }
        }
        o->vent = ent; o->vente = ente;
        int32_t *hj = JG_NEW(g, int32_t, B->nhyps ? B->nhyps : 1);
        const int32_t **hp = JG_NEW(g, const int32_t *, B->nhyps ? B->nhyps : 1);
        if (!hj || !hp) return false;
        for (int32_t i = 0; i < B->nhyps; i++) { hj[i] = -1; hp[i] = NULL; }
        for (int32_t r = 0; r < B->njr; r++) { hj[B->jhyp[r]] = B->jblk[r]; hp[B->jhyp[r]] = B->jpred[r]; }
        o->hjoin = hj; o->hpred = (const int32_t *const *)hp; }
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
// An obligation the generator cannot state, `why`; its id names the KIND it stands for, as a VC's
// does (jb_finish), so a record says which rule went unjudged and an authority maps it to a code.
// `ordinal` as the VC it stands for would have (a call's target, jg_rec_obligations), else 0.
static bool jg_outside_n(JG *g, JObls *L, IrInstr *at, int32_t blk, int32_t idx, const char *kind, const char *why, int32_t ordinal) {
    JObl *o = jg_push(g, L);
    if (!o) return jg_fail(g, "out of memory");
    o->at = at; o->blk = g->blk[blk]; o->idx = idx; o->outside = why; o->claim = -1;
    char pt[48], hs[24];
    snprintf(pt, sizeof pt, "bb%lld.%lld", (long long)blk, (long long)idx);
    snprintf(hs, sizeof hs, "%08llx", (long long)g->hash);
    char *fn = JG_NEW(g, char, (size_t)g->f->name->length + 1), *ps = JG_NEW(g, char, strlen(pt) + 1), *hh = JG_NEW(g, char, strlen(hs) + 1);
    if (!fn || !ps || !hh) return jg_fail(g, "out of memory");
    memcpy(fn, g->f->name->name, (size_t)g->f->name->length); fn[g->f->name->length] = 0;
    strcpy(ps, pt); strcpy(hh, hs);
    o->vc.id.fn = fn; o->vc.id.irhash = hh; o->vc.id.point = ps; o->vc.id.kind = kind;
    char os[24]; snprintf(os, sizeof os, "%lld", (long long)ordinal);
    char *od = JG_NEW(g, char, strlen(os) + 1);
    if (!od) return jg_fail(g, "out of memory");
    strcpy(od, os); o->vc.id.ordinal = od;
    return true;
}
static bool jg_outside(JG *g, JObls *L, IrInstr *at, int32_t blk, int32_t idx, const char *kind, const char *why) {
    return jg_outside_n(g, L, at, blk, idx, kind, why, 0);
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
    o->claim = -1;                                           // an obligation, no claim's
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
// MAKE_SLICE (data, n) over `data = ELEM_PTR(base, lo)`: the subslice's start and its length.
typedef struct { IrValue *lo, *n; JBoundArg a; } JSliceArg;
static bool jgk_slice_lo(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)at; JSliceArg *s = arg; JLT t[1];
    t[0].var = jb_val(B, s->lo); t[0].c = -1;                // 0 <= lo
    return jb_cons(B, t, 1, J_LE, 0, goal);
}
static bool jgk_slice_hi(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)at; JSliceArg *s = arg; JLT t[3];
    t[0].var = jb_val(B, s->lo); t[0].c = 1;                 // lo + n <= len(base)
    t[1].var = jb_val(B, s->n); t[1].c = 1;
    if (s->a.slice) { t[2].var = jb_len(B, s->a.slice); t[2].c = -1; return jb_cons(B, t, 3, J_LE, 0, goal); }
    return jb_cons(B, t, 2, J_LE, (__int128)s->a.clen, goal);
}
static bool jgk_slice_n(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)arg; JLT t[1]; t[0].var = jb_val(B, at->operands[1]); t[0].c = -1;   // 0 <= n
    return jb_cons(B, t, 1, J_LE, 0, goal);
}
static bool jgk_objsize(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)arg; JLT t[1]; t[0].var = jb_val(B, at->operands[1]); t[0].c = 1;    // n <= MAX_LEN(T)
    // A dynamic local of exactly n integer elements (`var a u8[n]`): the emitter probes its every
    // page (I.121), so an allocation that returns IS an object of n elements, and an object spans
    // at most ISIZE_MAX bytes (spec 07, O-1). TRUSTED, listed with the premises. Its C size is the
    // storage width, at least the byte width jg_len_max divides by, so the premise implies the goal.
    IrValue *dv = at->operands[0];
    IrInstr *d = dv && dv->id >= 0 && dv->id < B->g->nv ? B->g->def[dv->id] : NULL;
    if (d && d->op == IR_ALLOCA && !d->data && d->n_operands == 1 && d->operands[0] == at->operands[1] &&
        d->aux.alloca_ty && d->aux.alloca_ty->kind == IRT_INT) {
        int64_t bytes = ir_int_storage_bits(d->aux.alloca_ty->bits) / 8;
        jb_atom(B, jb_str(B, "premise object %%v%lld", dv->id, 0, 0), t, 1, J_LE, (__int128)INT64_MAX / bytes);
    }
    return jb_cons(B, t, 1, J_LE, jg_len_max(at->result ? at->result->type : NULL), goal);
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
    __int128 *min = arg; JLT t[1];                           // dividend = min => divisor != -1
    t[0].var = jb_val(B, at->operands[0]); t[0].c = 1;
    jb_atom(B, jb_str(B, "premise divovf %%v%lld", at->result ? at->result->id : -1, 0, 0), t, 1, J_EQ, *min);
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

// A float conversion's narrowing: its integral part trunc(x), a fresh variable (its recipe the float
// x, for a harness) with only F1's or F2's bounds at the conversion, within the type
static bool jgk_f2i(JB *B, IrInstr *at, void *arg, JCons *goal) {
    JNarrowArg *a = arg; JLT t[1];
    JRange fr; bool hl, hh;
    int32_t tv = jb_fresh(B, "trunc", at->result->id, NULL, 0, 1);
    if (B->fail) return false;
    ((JRecipe *)B->vrec[tv])->fval = a->v->id;
    if (jg_f2i(B->g, B->blk, B->idx, a->v, &fr, &hl, &hh)) {
        const char *o = jb_str(B, "f2i %%v%lld", a->v->id, 0, 0);
        if (hl) { t[0].var = tv; t[0].c = -1; jb_atom(B, o, t, 1, J_LE, -fr.lo); }
        if (hh) { t[0].var = tv; t[0].c = 1;  jb_atom(B, o, t, 1, J_LE, fr.hi); }
    }
    t[0].var = tv;
    if (a->upper) { t[0].c = 1; return jb_cons(B, t, 1, J_LE, a->bound, goal); }
    t[0].c = -1; return jb_cons(B, t, 1, J_LE, -a->bound, goal);
}
typedef struct { __int128 ne; IrValue *v; } JNeArg;
static bool jgk_ne(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)at; JNeArg *a = arg; JLT t[1];
    t[0].var = jb_val(B, a->v); t[0].c = 1;
    return jb_cons(B, t, 1, J_NE, a->ne, goal);
}
// P2.3's LANDING: value v meets a slot of integer type `dt` (a store, a struct field, a sum
// payload, a call argument, the result), owed as the interpreter checks it (ii_land,
// ii_land_refined) and VRA proves it (vra_check_narrow): the slot's range, refinement included,
// on each side the value's type may pass (`narrow.lo`, `narrow.hi`), and the value it excludes
// (`narrow.ne`). A WAIVED landing (`unsafe`) wraps and owes nothing, unless the slot is refined:
// a refinement is never waived (I.60). `ord` tells a call's arguments and a struct's fields apart.
// The obligation is what licenses an exact landing's fact (jg_lands_exact): a program that reaches
// past it has the value in range.
static bool jg_land(JG *g, JObls *L, IrInstr *at, int32_t blk, int32_t idx, IrValue *v, const IrType *dt,
                    bool waived, int ord) {
    if (!v || !v->type || v->type->kind != IRT_INT || !dt || dt->kind != IRT_INT) return true;
    if (waived && !(dt->has_refine || dt->has_ne)) return true;
    JRange vr, dr;
    if (!jg_range(v->type, &vr) || !jg_range(dt, &dr)) return true;
    if (vr.lo < dr.lo) {
        JNarrowArg a = { dr.lo, false, v };
        if (!jg_goal(g, L, at, blk, idx, -1, "narrow.lo", ord, jgk_narrow, &a)) return false;
    }
    if (vr.hi > dr.hi) {
        JNarrowArg a = { dr.hi, true, v };
        if (!jg_goal(g, L, at, blk, idx, -1, "narrow.hi", ord, jgk_narrow, &a)) return false;
    }
    if (dr.has_ne && !(vr.has_ne && vr.ne == dr.ne) && vr.lo <= dr.ne && dr.ne <= vr.hi) {
        JNeArg a = { dr.ne, v };
        if (!jg_goal(g, L, at, blk, idx, -1, "narrow.ne", ord, jgk_ne, &a)) return false;
    }
    return true;
}

// An ASSERT's condition as a goal (P2.6): a comparison read as jb_cmp reads a fact (an equality is
// two VCs, `half` 0 and 1), or any other condition `!= 0`. `ic` NULL: the latter.
typedef struct { IrInstr *ic; IrValue *v; int half; } JAssertArg;
static bool jgk_assert(JB *B, IrInstr *at, void *arg, JCons *goal) {
    (void)at; JAssertArg *a = arg; JLT t[2];
    if (!a->ic) { t[0].var = jb_val(B, a->v); t[0].c = 1; return jb_cons(B, t, 1, J_NE, 0, goal); }
    t[0].var = jb_val(B, a->ic->operands[0]); t[1].var = jb_val(B, a->ic->operands[1]);
    switch (a->ic->aux.cmp) {
        case IR_CMP_SLT: case IR_CMP_ULT: t[0].c = 1;  t[1].c = -1; return jb_cons(B, t, 2, J_LE, -1, goal);
        case IR_CMP_SLE: case IR_CMP_ULE: t[0].c = 1;  t[1].c = -1; return jb_cons(B, t, 2, J_LE, 0, goal);
        case IR_CMP_SGT: case IR_CMP_UGT: t[0].c = -1; t[1].c = 1;  return jb_cons(B, t, 2, J_LE, -1, goal);
        case IR_CMP_SGE: case IR_CMP_UGE: t[0].c = -1; t[1].c = 1;  return jb_cons(B, t, 2, J_LE, 0, goal);
        case IR_CMP_EQ: t[0].c = a->half ? -1 : 1; t[1].c = -t[0].c; return jb_cons(B, t, 2, J_LE, 0, goal);
        default:        t[0].c = 1; t[1].c = -1; return jb_cons(B, t, 2, J_NE, 0, goal);
    }
}
// Can an ASSERT's comparison be a goal? Integers, and an unsigned comparison only of operands that
// are never negative (the interpreter compares as uint64), as jb_cmp states a fact: unsigned, or
// signed with a static range >= 0 (a constant's value; `pos + 1` over a u32 pos is in [1, 2^32]).
// One value compared with itself leaves the empty sum: `0 <= 0` for `m <= m`, `0 != 0` for `z != z`.
static bool jg_assert_cmp(JG *g, IrInstr *ic) {
    if (!ic || ic->op != IR_ICMP || ic->n_operands < 2) return false;
    IrValue *a = ic->operands[0], *b = ic->operands[1];
    if (!a || !b || !jg_is_int(a->type) || !jg_is_int(b->type)) return false;
    IrCmp c = ic->aux.cmp;
    if (c == IR_CMP_ULT || c == IR_CMP_ULE || c == IR_CMP_UGT || c == IR_CMP_UGE)
        for (int k = 0; k < 2; k++) {
            IrValue *o = ic->operands[k];
            JRange r;
            if (o->type->kind == IRT_BOOL || !o->type->is_signed) continue;
            if (!jg_static_range(g, o, &r, 0) || r.lo < 0) return false;
        }
    return true;
}

// The length an ELEM_PTR's base is indexed against (VRA's rules): a local array's, an array
// value's, a pointer to a vector's, a slice's (through SLICE_DATA); neither when it is not known.
static void jg_bound_arg(JG *g, IrInstr *i, JBoundArg *a) {
    IrValue *base = i->operands[0];
    IrInstr *bd = base && base->id >= 0 && base->id < g->nv ? g->def[base->id] : NULL;
    a->w = i->aux.elem_width > 0 ? i->aux.elem_width : 1; a->clen = -1; a->slice = NULL;
    if (bd && bd->op == IR_ALLOCA && bd->aux.alloca_ty &&
        (bd->aux.alloca_ty->kind == IRT_ARRAY || bd->aux.alloca_ty->kind == IRT_VECTOR))
        a->clen = bd->aux.alloca_ty->array_len;
    else if (base->type && (base->type->kind == IRT_ARRAY || base->type->kind == IRT_VECTOR))
        a->clen = base->type->array_len;
    else if (base->type && base->type->kind == IRT_PTR && base->type->elem && base->type->elem->kind == IRT_VECTOR)
        a->clen = base->type->elem->array_len;
    else if (bd && bd->op == IR_SLICE_DATA && bd->n_operands >= 1) a->slice = bd->operands[0];
}
// A slice the lowering builds over an object it made with that length (lower.h's other four
// MAKE_SLICE sites): a string literal (its bytes and the NUL), a local array or a module constant
// table (n a constant within it), a dynamic local of exactly n elements (`var a u8[n]`). Read from
// the IR, not trusted: any other shape is `outside`.
static bool jg_slice_whole(JG *g, IrInstr *ms) {
    IrValue *dv = ms->operands[0], *nv = ms->operands[1];
    IrInstr *d = dv && dv->id >= 0 && dv->id < g->nv ? g->def[dv->id] : NULL;
    IrInstr *nd = nv && nv->id >= 0 && nv->id < g->nv ? g->def[nv->id] : NULL;
    if (!d) return false;
    bool kn = nd && nd->op == IR_CONST && jg_const(nd) >= 0;
    if (d->op == IR_STR_CONST) return kn && jg_const(nd) <= (__int128)d->aux.str.len + 1;
    if (d->op != IR_ALLOCA) return false;
    if (d->data) return kn && jg_const(nd) <= d->data->n;
    if (d->n_operands == 0 && d->aux.alloca_ty && d->aux.alloca_ty->kind == IRT_ARRAY)
        return kn && jg_const(nd) <= d->aux.alloca_ty->array_len;
    return d->n_operands == 1 && d->operands[0] == nv;
}

// What a checked division's RESULT owes, on the interpreter's semantics: it computes x / y
// (truncating) and x % y (the sign of x) exactly, then lands the result in its type (ii_settle).
// - The dividend in the result's range (`divovf` 1 and 2, owed only where the types do not nest:
//   `100 /% x` with an i64 literal and a u64 result, I.56). With it, a remainder lies between 0
//   and x, and |q| <= |x|, so the result lands, but for the quotient's sign:
// - -MIN does not land: x = MIN of a signed result and y = -1 (`divovf` 0, the dividend at MIN
//   the premise; I.198's i32 / i32 at i32 included, while a `/` widened by a bit cannot fail
//   here and its narrowing does). Owed wherever x may reach that MIN, as VRA states it
//   (vra_check_div), an unsigned divisor's type range implying it;
// - a negative divisor makes a negative quotient, which an unsigned result cannot hold: where the
//   divisor's type has negative values, `divovf` 3 owes it non-negative (`(lo + hi) / 2`, the
//   literal 2 an i64);
// - and C traps on MIN % -1 where the operands' C type is their type (32 or 64 bits,
//   storage-exact), which the interpreter checks by itself (I.185): `divovf` 0 again.
typedef struct { bool lo, hi, min, ypos; __int128 rlo, rhi, mval; } JDivOwe;
static bool jg_div_result(const IrInstr *i, JDivOwe *d) {
    const IrType *xt = i->operands[0]->type, *yt = i->operands[1]->type;
    const IrType *rt = i->result ? i->result->type : NULL;
    if (!rt || rt->kind != IRT_INT || rt->bits < 1 || rt->bits > 64) return false;
    __int128 xlo = jg_width_lo(xt), xhi = jg_width_hi(xt), ylo = jg_width_lo(yt);
    d->rlo = jg_width_lo(rt); d->rhi = jg_width_hi(rt);
    d->lo = xlo < d->rlo; d->hi = xhi > d->rhi; d->min = false; d->ypos = false; d->mval = 0;
    if (i->op == IR_SDIV || i->op == IR_UDIV) {
        d->ypos = d->rlo == 0 && ylo < 0;
        if (d->rlo < 0 && xlo <= d->rlo) { d->min = true; d->mval = d->rlo; }
    } else if (xt->is_signed && xt->bits >= 32 && xt->bits == ir_int_storage_bits(xt->bits)) {
        d->min = true; d->mval = xlo;
    }
    return true;
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
                    JBoundArg a; jg_bound_arg(g, i, &a);
                    if (!jg_is_int(i->operands[1]->type)) { if (!jg_outside(g, L, i, b->id, k, "bounds.lo", "an index that is not an integer")) return false; break; }
                    if (!jg_goal(g, L, i, b->id, k, -1, "bounds.lo", 0, jgk_bounds_lo, NULL)) return false;
                    if (a.clen < 0 && !a.slice) { if (!jg_outside(g, L, i, b->id, k, "bounds.hi", "no length is known for the base (a shape or a shared-term cancellation)")) return false; break; }
                    if (!jg_goal(g, L, i, b->id, k, -1, "bounds.hi", 0, jgk_bounds_hi, &a)) return false;
                    break;
                }
                case IR_MAKE_SLICE: {
                    // P2.4. `objsize` at every one, `unsafe` included: every reader of a slice's
                    // length is given len <= MAX_LEN(T) (jb_state), so it is never waived (D1 A3).
                    // Then pointer validity, waived in `unsafe` (spec 18): a subslice's start and
                    // length, both here, where VRA checks the pair (vra_check_subslice; the
                    // interpreter checks the start at the ELEM_PTR, whose operand reaches this
                    // point unchanged); any other data must be an object made with the slice's
                    // length (jg_slice_whole).
                    if (i->n_operands < 2 || !i->result) break;
                    IrValue *n = i->operands[1];
                    if (!jg_is_int(n->type)) { if (!jg_outside(g, L, i, b->id, k, "objsize", "a slice length that is not an integer")) return false; break; }
                    if (!jg_goal(g, L, i, b->id, k, -1, "objsize", 0, jgk_objsize, NULL)) return false;
                    if (i->unchecked) break;
                    if (n->type->is_signed && !jg_goal(g, L, i, b->id, k, -1, "bounds.lo", 1, jgk_slice_n, NULL)) return false;
                    IrValue *dv = i->operands[0];
                    IrInstr *ep = dv && dv->id >= 0 && dv->id < g->nv ? g->def[dv->id] : NULL;
                    if (!ep || ep->op != IR_ELEM_PTR || ep->n_operands < 2) {
                        if (!jg_slice_whole(g, i) && !jg_outside(g, L, i, b->id, k, "bounds.hi", "a slice over data whose length the generator does not know")) return false;
                        break;
                    }
                    if (!jg_is_int(ep->operands[1]->type)) { if (!jg_outside(g, L, i, b->id, k, "bounds.lo", "a subslice start that is not an integer")) return false; break; }
                    JSliceArg sa; sa.lo = ep->operands[1]; sa.n = n; jg_bound_arg(g, ep, &sa.a);
                    if (!jg_goal(g, L, i, b->id, k, -1, "bounds.lo", 0, jgk_slice_lo, &sa)) return false;
                    if (sa.a.clen < 0 && !sa.a.slice) { if (!jg_outside(g, L, i, b->id, k, "bounds.hi", "no length is known for the subslice's base")) return false; break; }
                    if (!jg_goal(g, L, i, b->id, k, -1, "bounds.hi", 0, jgk_slice_hi, &sa)) return false;
                    break;
                }
                case IR_SDIV: case IR_UDIV: case IR_SREM: case IR_UREM: {
                    if (i->unchecked || i->n_operands < 2) break;
                    // a VECTOR divided lane by lane (spec 07): a scalar divisor is owed non-zero
                    // as a scalar's is, one given per lane is refused (no per-lane facts), and
                    // lanes wrap, so no result is owed
                    const IrType *dvt = i->operands[0]->type;
                    if (dvt && dvt->kind == IRT_VECTOR) {
                        if (!dvt->elem || dvt->elem->kind != IRT_INT) break;
                        if (!jg_is_int(i->operands[1]->type)) { if (!jg_outside(g, L, i, b->id, k, "divzero", "a divisor given per lane")) return false; break; }
                        if (!jg_goal(g, L, i, b->id, k, -1, "divzero", 0, jgk_divzero, NULL)) return false;
                        break;
                    }
                    if (!jg_is_int(i->operands[0]->type) || !jg_is_int(i->operands[1]->type)) break;
                    if (!jg_goal(g, L, i, b->id, k, -1, "divzero", 0, jgk_divzero, NULL)) return false;
                    if (i->wrap != IR_WRAP_CHECK) break;             // `/%` and `/|` define the result
                    JDivOwe dw;
                    if (!jg_div_result(i, &dw)) { if (!jg_outside(g, L, i, b->id, k, "divovf", "a division whose result is not an integer of at most 64 bits")) return false; break; }
                    if (dw.min && !jg_goal(g, L, i, b->id, k, -1, "divovf", 0, jgk_divovf, &dw.mval)) return false;
                    JNarrowArg dl = { dw.rlo, false, i->operands[0] }, dh = { dw.rhi, true, i->operands[0] };
                    if (dw.lo && !jg_goal(g, L, i, b->id, k, -1, "divovf", 1, jgk_narrow, &dl)) return false;
                    if (dw.hi && !jg_goal(g, L, i, b->id, k, -1, "divovf", 2, jgk_narrow, &dh)) return false;
                    JNarrowArg dy = { 0, false, i->operands[1] };
                    if (dw.ypos && !jg_goal(g, L, i, b->id, k, -1, "divovf", 3, jgk_narrow, &dy)) return false;
                    break;
                }
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_NEG: {
                    if (i->wrap != IR_WRAP_CHECK || i->unchecked || !i->result || !jg_is_int(i->result->type)) break;
                    JRange rr;
                    if (!jg_range(i->result->type, &rr)) break;
                    if (i->op == IR_MUL) {
                        __int128 kc;                           // as jb states the product: one constant factor
                        bool c0 = jg_const_of(g, i->operands[0], &kc), c1 = jg_const_of(g, i->operands[1], &kc);
                        // two variables: owed where the corners of their static ranges lie in the type
                        // (its narrowing then follows from them), else outside
                        JRange h;
                        if (!c0 && !c1 && (!jg_mul_hull(g, i, &h) || h.lo < rr.lo || h.hi > rr.hi)) {
                            if (!jg_outside(g, L, i, b->id, k, "narrow.lo", "atom.mul: a product of two variables whose corners leave its type")) return false;
                            break;
                        }
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
                    JRange rr;
                    if (!jg_is_int(i->operands[0]->type)) {               // a float: its integral part fits
                        if (!i->operands[0]->type || i->operands[0]->type->kind != IRT_FLOAT || !jg_range(i->result->type, &rr)) break;
                        JNarrowArg flo = { rr.lo, false, i->operands[0] }, fhi = { rr.hi, true, i->operands[0] };
                        if (!jg_goal(g, L, i, b->id, k, -1, "narrow.lo", 0, jgk_f2i, &flo)) return false;
                        if (!jg_goal(g, L, i, b->id, k, -1, "narrow.hi", 0, jgk_f2i, &fhi)) return false;
                        break;
                    }
                    if (!jg_range(i->result->type, &rr)) break;
                    JNarrowArg lo = { rr.lo, false, i->operands[0] }, hi = { rr.hi, true, i->operands[0] };
                    if (!jg_goal(g, L, i, b->id, k, -1, "narrow.lo", 0, jgk_narrow, &lo)) return false;
                    if (!jg_goal(g, L, i, b->id, k, -1, "narrow.hi", 0, jgk_narrow, &hi)) return false;
                    break;
                }
                case IR_SHL: case IR_LSHR: case IR_ASHR: {
                    if (i->unchecked || i->n_operands < 2 || !i->result || !i->result->type) break;
                    // a VECTOR's amount is owed as a scalar's (spec 07: a scalar provably in
                    // [0, w-1] for w-bit lanes, E086; the C shifts lane by lane, I.202); one given
                    // per lane is refused, as VRA refuses it. Lanes wrap: no narrowing is owed
                    const IrType *vt = i->result->type;
                    if (vt->kind == IRT_VECTOR) {
                        if (!vt->elem || vt->elem->kind != IRT_INT) break;
                        if (!jg_is_int(i->operands[1]->type)) { if (!jg_outside(g, L, i, b->id, k, "shift", "a shift amount given per lane")) return false; break; }
                        JNarrowArg vlo = { 0, false, i->operands[1] }, vhi = { vt->elem->bits - 1, true, i->operands[1] };
                        if (!jg_goal(g, L, i, b->id, k, -1, "shift", 0, jgk_narrow, &vlo)) return false;
                        if (!jg_goal(g, L, i, b->id, k, -1, "shift", 1, jgk_narrow, &vhi)) return false;
                        break;
                    }
                    if (vt->kind != IRT_INT || !jg_is_int(i->operands[1]->type)) break;
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
                // an ASSERT (P2.6: a precondition or a user `assert`, code 0; a return refinement,
                // 86; a sized slice, 87; a struct invariant, 121; a refused call, 135 and 137): its
                // condition, owed here, `unsafe` or not (interp.h checks every one). Its kind names
                // the code: `assert.86`. A comparison is read as a fact is; anything else is owed
                // `!= 0` (a comparison that cannot be a goal included: its result has no fact, so
                // the VC is never implied)
                case IR_ASSERT: {
                    if (i->n_operands < 1 || !i->operands[0]) break;
                    char kn[32]; snprintf(kn, sizeof kn, "assert.%lld", (long long)i->aux.imm);
                    char *ks = JG_NEW(g, char, strlen(kn) + 1);
                    if (!ks) return jg_fail(g, "out of memory");
                    strcpy(ks, kn);
                    IrValue *cv = i->operands[0];
                    IrInstr *ic = cv->id >= 0 && cv->id < g->nv ? g->def[cv->id] : NULL;
                    JAssertArg a = { jg_assert_cmp(g, ic) ? ic : NULL, cv, 0 };
                    if (!jg_goal(g, L, i, b->id, k, -1, ks, 0, jgk_assert, &a)) return false;
                    if (a.ic && a.ic->aux.cmp == IR_CMP_EQ) {
                        a.half = 1;
                        if (!jg_goal(g, L, i, b->id, k, -1, ks, 1, jgk_assert, &a)) return false;
                    }
                    break;
                }
                // the LANDINGS (P2.3), where the interpreter lands each value (ii_land)
                case IR_STORE: {                                     // into the address's pointee
                    const IrType *pt = i->n_operands >= 2 ? i->operands[0]->type : NULL;
                    if (pt && pt->kind == IRT_PTR && !jg_land(g, L, i, b->id, k, i->operands[1], pt->elem, i->unchecked, 0)) return false;
                    break;
                }
                case IR_STRUCT_NEW: {                                // each field into its type
                    const IrType *rt = i->result ? i->result->type : NULL;
                    for (int f = 0; rt && rt->kind == IRT_STRUCT && f < i->n_operands && f < rt->n_fields; f++)
                        if (!jg_land(g, L, i, b->id, k, i->operands[f], rt->fields[f], i->unchecked, f)) return false;
                    break;
                }
                case IR_SUM_NEW: {                                   // each payload field into its type
                    const IrType *rt = i->result ? i->result->type : NULL;
                    int vk = i->aux.sum.variant;
                    const IrType *pl = rt && rt->kind == IRT_SUM && vk >= 0 && vk < rt->n_fields ? rt->fields[vk] : NULL;
                    for (int f = 0; pl && f < i->n_operands; f++) {
                        const IrType *ft = pl->kind == IRT_STRUCT ? (f < pl->n_fields ? pl->fields[f] : NULL) : (f == 0 ? pl : NULL);
                        if (!jg_land(g, L, i, b->id, k, i->operands[f], ft, i->unchecked, f)) return false;
                    }
                    break;
                }
                case IR_CALL: {
                    // each argument into its parameter: a direct callee's (none for a builtin,
                    // which the interpreter lands nowhere); an indirect call's, the POINTER type's
                    // (VRA's, I.188), which states at least what the function it holds assumes
                    // (I.187), where the interpreter lands into that function's own
                    if (i->aux.callee) {
                        IrFunc *h = jg_callee(g, i);
                        int a = 0;
                        for (IrParam *pp = h ? h->params : NULL; pp && a < i->n_operands; pp = pp->next, a++)
                            if (pp->value && !jg_land(g, L, i, b->id, k, i->operands[a], pp->value->type, i->unchecked, a)) return false;
                    } else if (i->n_operands >= 1 && i->operands[0]->type) {
                        const IrType *ft = i->operands[0]->type;
                        if (ft->kind == IRT_PTR && ft->elem) ft = ft->elem;
                        for (int a = 1; ft->kind == IRT_FUNC && a < i->n_operands && a - 1 < ft->n_fields; a++)
                            if (!jg_land(g, L, i, b->id, k, i->operands[a], ft->fields[a - 1], i->unchecked, a - 1)) return false;
                    }
                    break;
                }
                default: break;
            }
        }
        // the RETURN's landing, at the block's end: the value returned into the result type, the
        // `return` itself waived or not (interp.h's RET)
        if (b->term.kind == IR_TERM_RET && b->term.cond &&
            !jg_land(g, L, NULL, b->id, k, b->term.cond, g->f->ret_type, b->term.unchecked, 0)) return false;
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
// integer value defined in a block that strictly dominates b; of kind JTERM_LEN, the same of a
// slice, its length.
static bool jg_term_kind(const IrType *t, uint8_t tk) {
    return tk == JTERM_LEN ? t && t->kind == IRT_SLICE : tk == JTERM_VALUE && jg_is_int(t);
}
static bool jg_term_ok(JG *g, int32_t b, int32_t key, uint8_t tk) {
    if (key < 0 || key >= g->nv) return false;
    if (g->cell[key] >= 0) return jg_term_kind(g->cty[g->cell[key]], tk);
    IrValue *v = jg_value(g, key);
    if (!v || !jg_term_kind(v->type, tk)) return false;
    if (!g->def[key]) return true;                                // a parameter
    int32_t d = g->defblk[key];
    return d >= 0 && d != b && jg_dominates(g, d, b);
}
// An EACH term's array (JTERM_EACH): the address `key` of a fixed array of 1 to JELEM_MAXN elements,
// cells[k] element k's cell (an integer one) or -1 where it is no cell: an element without a cell
// carries no fact, so it has no instance. 0 where it is no such array, or none is a cell (refused).
static int32_t jg_each_cells(JG *g, int32_t key, int32_t *cells) {
    if (key < 0 || key >= g->nv || g->aroot[key] < 0) return 0;
    const IrType *t = jg_pointee(g, key);
    if (!t || t->kind != IRT_ARRAY || t->array_len < 1 || t->array_len > JELEM_MAXN) return 0;
    bool any = false;
    for (int32_t k = 0; k < (int32_t)t->array_len; k++) {
        char pth[256]; snprintf(pth, sizeof pth, "%s[%lld]", g->apath[key], (long long)k);
        int32_t c = -1;
        for (int32_t c2 = 0; c2 < g->ncells && c < 0; c2++)
            if (g->croot[c2] == g->aroot[key] && !strcmp(g->cpath[c2], pth)) c = c2;
        if (c >= 0 && !jg_is_int(g->cty[c])) return 0;
        cells[k] = c;
        if (c >= 0) any = true;
    }
    return any ? (int32_t)t->array_len : 0;
}
// Is a version the block makes afresh (a join's or a header's)?
static bool jg_made_at(JG *g, int32_t blk, int32_t cell) {
    int32_t jv = g->instate[(size_t)blk * (size_t)g->ncells + (size_t)cell];
    return jv >= 0 && (g->ver[jv].kind == JV_JOIN || g->ver[jv].kind == JV_HEAD) && g->ver[jv].blk == blk;
}
// A term's version: the block's (0), the loop's entry edge's (1), the trip's start's (2).
static int jg_tk_class(uint8_t tk) { return tk == JTERM_ENTRY ? 1 : tk == JTERM_HEAD ? 2 : 0; }
// Claims are untrusted input: one the generator cannot state AND owe is refused, never skipped.
// Its terms: one to `max`, no key twice (a sum names each term once), but a cell beside its ENTRY
// value (`x - entry(x)`: two terms, its version at the block and the one its loop was entered with).
static bool jg_claim_shape(JG *g, const int32_t *key, const uint8_t *tk, int32_t nt, int32_t max) {
    if (nt < 1 || nt > max) return jg_fail(g, "a claim of no terms, or of more than it may have");
    for (int32_t i = 0; i < nt; i++)
        for (int32_t j = i + 1; j < nt; j++)
            if (key[i] == key[j] && jg_tk_class(tk[i]) == jg_tk_class(tk[j])) return jg_fail(g, "a claim naming one term twice");
    return true;
}
// A return claim of h: its terms, the returned value among them and any other an integer
// parameter of h; h returns an integer. A return whose landing wraps is owed about the value the
// caller receives, the landed one (jg_post_obligations, s8), as the caller's contract states it.
static bool jg_ret_claim_ok(JG *g, const JClaim *c, const IrFunc *h) {
    const __int128 big = (__int128)1 << 66;
    // the returned value is an integer's (JTERM_VALUE), or a returned slice's length (JTERM_LEN)
    bool sret = h && h->ret_type && h->ret_type->kind == IRT_SLICE;
    if (!h || h->is_extern || (!jg_is_int(h->ret_type) && !sret)) return jg_fail(g, "a return claim on a function returning no integer");
    // lowered with a construct dropped, the IR is not the function: it could return anything
    if (h->incomplete) return jg_fail(g, "a return claim on a function lowered incompletely");
    if (!jg_claim_shape(g, c->key, c->tk, c->nt, JCLAIM_MAXT)) return false;
    if (c->rhs > big || c->rhs < -big) return jg_fail(g, "a claim's bound out of range");
    for (int k = 0; k < c->nt; k++) {
        if (c->key[k] != JKEY_RET) { if (c->tk[k] != JTERM_VALUE) return jg_fail(g, "a return claim over a length"); continue; }
        if (c->tk[k] != (sret ? JTERM_LEN : JTERM_VALUE))
            return jg_fail(g, "a return claim's returned term not of its kind: an integer's value, a slice's length");
    }
    bool ret = false;
    for (int k = 0; k < c->nt; k++) {
        if (c->coef[k] == 0 || c->coef[k] > big || c->coef[k] < -big) return jg_fail(g, "a claim's coefficient out of range");
        if (c->key[k] == JKEY_RET) { ret = true; continue; }
        int pk = jg_param_at(h, c->key[k]);
        const IrParam *pp = h->params;
        for (int q = 0; q < pk && pp; q++) pp = pp->next;
        if (pk < 0 || !pp || !pp->value || !jg_is_int(pp->value->type))
            return jg_fail(g, "a return claim's term is neither the returned value nor an integer parameter");
    }
    if (!ret) return jg_fail(g, "a return claim not about the returned value");
    return true;
}
// A guarded return claim as the return claim it guards, for the checks they share
static JClaim jg_guard_as_claim(const JGuardClaim *c) {
    JClaim r; memset(&r, 0, sizeof r);
    r.blk = JCLAIM_RET; r.nt = c->nt; r.rhs = c->rhs; r.fn = c->fn;
    for (int k = 0; k < JCLAIM_MAXT; k++) { r.key[k] = c->key[k]; r.tk[k] = c->tk[k]; r.coef[k] = c->coef[k]; }
    return r;
}
// A guarded return claim's own rules: a return claim's, and one or two guard parameters, each an
// integer parameter of h named once, its constant in the parameter's type.
static bool jg_guard_claim_ok(JG *g, const JGuardClaim *c, const IrFunc *h) {
    JClaim r = jg_guard_as_claim(c);
    if (!jg_ret_claim_ok(g, &r, h)) return false;
    for (int k = 0; k < c->nt; k++) if (c->tk[k] != JTERM_VALUE) return jg_fail(g, "a guarded return claim over a length");
    if (c->ng < 1 || c->ng > JGUARD_MAXP) return jg_fail(g, "a guarded claim of no guard, or of more than it may have");
    for (int k = 0; k < c->ng; k++) {
        for (int j = 0; j < k; j++) if (c->gkey[j] == c->gkey[k]) return jg_fail(g, "a guarded claim naming one parameter twice in its guard");
        int pk = jg_param_at(h, c->gkey[k]);
        const IrParam *pp = h->params;
        for (int q = 0; q < pk && pp; q++) pp = pp->next;
        JRange tr;
        if (pk < 0 || !pp || !pp->value || !jg_is_int(pp->value->type) || !jg_range(pp->value->type, &tr))
            return jg_fail(g, "a guarded claim's guard is not an integer parameter");
        if (c->gval[k] < tr.lo || c->gval[k] > tr.hi) return jg_fail(g, "a guarded claim's constant outside its parameter's type");
    }
    return true;
}
static bool jg_claims_ok(JG *g) {
    const __int128 big = (__int128)1 << 66;
    for (int32_t i = 0; i < g->ngc; i++) {                   // owed here, or a callee's contract
        const JGuardClaim *c = &g->gc[i];
        if (!c->fn) return jg_fail(g, "a guarded claim with no function");
        if ((c->fn == g->f || jg_same_module(c->fn, g->f)) && !jg_guard_claim_ok(g, c, c->fn)) return false;
    }
    for (int32_t ci = 0; ci < g->ncl; ci++) {
        const JClaim *c = &g->cl[ci];
        if (c->blk == JCLAIM_RET) {                          // owed here, or a callee's contract
            if (!c->fn) return jg_fail(g, "a return claim with no function");
            if ((c->fn == g->f || jg_same_module(c->fn, g->f)) && !jg_ret_claim_ok(g, c, c->fn)) return false;
            continue;
        }
        if (!jg_mine(g, c)) continue;                        // another function's cut point
        // a claim is owed on every edge into its block, so any reached block will do but the
        // function's entry, which is entered with no edge; one with a fresh version is a join or
        // a header (the `made` rule below)
        if (c->blk < 0 || c->blk >= g->nb || !g->blk[c->blk] || g->rpoix[c->blk] < 0 || c->blk == g->f->entry->id)
            return jg_fail(g, "a claim not at a block entered by edges only");
        if (!jg_claim_shape(g, c->key, c->tk, c->nt, JCLAIM_MAXT)) return false;
        if (c->rhs > big || c->rhs < -big) return jg_fail(g, "a claim's bound out of range");
        bool made = false;
        int each = 0, upto = 0;
        for (int k = 0; k < c->nt; k++) {
            int32_t key = c->key[k];
            if (c->coef[k] == 0 || c->coef[k] > big || c->coef[k] < -big) return jg_fail(g, "a claim's coefficient out of range");
            if (c->tk[k] == JTERM_EACH) {                    // one instance per element cell
                int32_t ec[JELEM_MAXN], en = jg_each_cells(g, key, ec);
                if (++each > 1) return jg_fail(g, "a claim of two EACH terms");
                if (!en) return jg_fail(g, "a claim's EACH term is not a fixed array with an element cell");
                for (int32_t q = 0; q < en; q++) if (ec[q] >= 0 && jg_made_at(g, c->blk, ec[q])) made = true;
                continue;
            }
            if (c->tk[k] == JTERM_ENTRY) {                   // a cell's version on the loop's entry edge
                if (key < 0 || key >= g->nv || g->cell[key] < 0 || !jg_is_int(g->cty[g->cell[key]])) return jg_fail(g, "a claim's ENTRY term is no integer cell");
                if (!jg_loop_header(g, c->blk)) return jg_fail(g, "a claim's ENTRY term at a block that is no loop header");
                if (jg_entry_pred(g, c->blk) < 0) return jg_fail(g, "a claim's ENTRY term at a loop entered by more than one edge");
                if (!jg_entry_ok(g, c->blk, key)) return jg_fail(g, "a claim's ENTRY term of a cell with no version on the entry edge");
                continue;                                    // fixed: no version the block makes
            }
            if (c->tk[k] == JTERM_HEAD) {                    // a cell's version as the trip began
                if (!jg_head_ok(g, c->blk, key)) return jg_fail(g, "a claim's HEAD term of no integer cell with a version at an enclosing loop's header");
                continue;                                    // fixed in the trip: no version the block makes
            }
            if (c->tk[k] == JTERM_UPTO) {                    // a value guarding each instance
                if (++upto > 1) return jg_fail(g, "a claim of two UPTO terms");
                if (c->coef[k] != 1) return jg_fail(g, "a claim's UPTO term of a coefficient other than 1");
            }
            if (!jg_term_ok(g, c->blk, key, c->tk[k] == JTERM_UPTO ? JTERM_VALUE : c->tk[k]))
                return jg_fail(g, "a claim's term is not a cell or a value fixed at its block");
            if (g->cell[key] >= 0 && jg_made_at(g, c->blk, g->cell[key])) made = true;
        }
        if (upto && !each) return jg_fail(g, "a claim's UPTO term without an EACH term");
        if (!made) return jg_fail(g, "a claim about no version its block makes afresh");
    }
    return true;
}
// Each claim's obligations (P3.5): on every edge P -> H into its block, at the end of P, the edge's
// condition a fact, the claim about the versions P ends with: `hull` into a join, `inv.entry` into
// a header from before its loop, `inv.keep` along a back edge (where H's own claims about its
// previous entry are hypotheses: the induction step). A predecessor that reaches H by both arms
// of its branch owes it once, with no edge condition.
// A return claim's obligation, `post`: at every return, the claim about the value returned (its
// landing in the return type exact: jg_ret_claim_ok) and the parameters' values (their entry ones).
// A GUARDED one's (gd, s7) as its return claim's, its guard a hypothesis (each guard parameter at
// entry is its constant); nothing about any other state at entry (a module variable, what an
// argument points to) is assumed: the generator states the body's facts as for any return claim.
static bool jg_post_obligations(JG *g, JObls *L, const JClaim *c, const JGuardClaim *gd, int ordinal, int32_t claim) {
    for (IrBlock *b = g->f->blocks; b; b = b->next) {
        if (g->rpoix[b->id] < 0 || b->term.kind != IR_TERM_RET || !b->term.cond) continue;
        int32_t n = 0;
        for (IrInstr *i = b->instrs; i; i = i->next) n++;
        JB B = jb_begin(g, b->id, n, -1);
        if (B.fail) return jg_fail(g, "out of memory building a VC");   // its arrays are not there
        for (int k = 0; gd && k < gd->ng; k++) {
            JLT w[1]; w[0].var = jb_val(&B, jg_value(g, gd->gkey[k])); w[0].c = 1;
            jb_atom(&B, jb_str(&B, "guard %%v%lld", gd->gkey[k], 0, 0), w, 1, J_EQ, gd->gval[k]);
        }
        // the value the caller receives: the returned one where its landing in the return type keeps
        // it, else that value WRAPPED into the type (a waived narrowing, as it_wrap makes it): a fresh
        // x = v - k*2^w within the type, its recipe the wrap for a run-time check
        // a returned slice: its length (a return claim's JTERM_LEN of JKEY_RET), the slice landing as it is
        const IrType *rt = g->f->ret_type;
        bool sret = rt && rt->kind == IRT_SLICE;
        int32_t rv = sret ? jb_len(&B, b->term.cond) : jb_val(&B, b->term.cond);
        if (!sret && !jg_lands_exact(b->term.cond->type, rt, b->term.unchecked) && rt && rt->kind == IRT_INT && rt->bits >= 1 && rt->bits <= 64) {
            JLT w[1]; w[0].var = rv; w[0].c = 1;
            int32_t x = jb_fresh(&B, "ret.landed", b->term.cond->id, w, 1, 1);
            if (!B.fail) {
                JRecipe *r = (JRecipe *)B.vrec[x]; r->wbits = (int8_t)rt->bits; r->wsgn = rt->is_signed;
                jb_wrapto(&B, x, rv, rt, "ret.k", b->term.cond->id, jb_str(&B, "return bb%lld wrap", b->id, 0, 0));
                rv = x;
            }
        }
        JLT t[JCLAIM_MAXT]; JCons goal;
        for (int k = 0; k < c->nt; k++) {
            t[k].var = c->key[k] == JKEY_RET ? rv : jb_val(&B, jg_value(g, c->key[k]));
            t[k].c = c->coef[k];
        }
        if (B.fail || !jb_cons(&B, t, c->nt, J_LE, c->rhs, &goal)) return jg_fail(g, "out of memory building a claim");
        JObl *o = jg_push(g, L);
        if (!o || !jb_finish(&B, NULL, "post", ordinal, &goal, o)) return jg_fail(g, "out of memory building a VC");
        o->claim = claim;                                    // no instruction, no edge: as b returns
    }
    return true;
}
static bool jg_claim_obligations(JG *g, JObls *L, int32_t only) {
    // a guarded return claim's `post`, ordinal (ncl + i) * 1000 (past every claim's), index ncl + i
    for (int32_t i = 0; i < g->ngc && only < 0; i++) {
        if (g->gc[i].fn != g->f) continue;
        JClaim r = jg_guard_as_claim(&g->gc[i]);
        if (!jg_post_obligations(g, L, &r, &g->gc[i], (g->ncl + i) * 1000, g->ncl + i)) return false;
    }
    for (int32_t ci = 0; ci < g->ncl; ci++) {
        const JClaim *c = &g->cl[ci];
        if (c->blk == JCLAIM_RET) {
            if (c->fn == g->f && only < 0 && !jg_post_obligations(g, L, c, NULL, ci, ci)) return false;
            continue;
        }
        if (!jg_mine(g, c) || only == JGEN_POST || (only >= 0 && c->blk != only)) continue;
        IrBlock *H = g->blk[c->blk];
        for (IrEdge *e = H->preds; e; e = e->next) {
            IrBlock *P = e->block;
            if (g->rpoix[P->id] < 0) continue;
            bool again = false;
            for (IrEdge *e2 = H->preds; e2 != e; e2 = e2->next) if (e2->block == P) again = true;
            if (again) continue;
            int32_t n = 0;
            for (IrInstr *i = P->instrs; i; i = i->next) n++;
            // an EACH claim owes one VC per element cell (ordinal ci*1000 + k), each about that
            // element; a guarded one (UPTO g) only where g >= k + 1 at the edge's end, a hypothesis
            int32_t ec[JELEM_MAXN], en = 0, ek = -1, gk = -1;
            for (int k = 0; k < c->nt; k++) {
                if (c->tk[k] == JTERM_EACH) { ek = k; en = jg_each_cells(g, c->key[k], ec); }
                if (c->tk[k] == JTERM_UPTO) gk = k;
            }
            const int32_t *row = g->outstate + (size_t)P->id * (size_t)g->ncells;
            for (int32_t q = 0; q < (ek >= 0 ? en : 1); q++) {
                if (ek >= 0 && ec[q] < 0) continue;
                JB B = jb_begin(g, P->id, n, -1);
                if (B.fail) return jg_fail(g, "out of memory building a VC");   // its arrays are not there
                B.to = H->id;
                JLT t[JCLAIM_MAXT]; JCons goal; int nt = 0;
                for (int k = 0; k < c->nt; k++) {
                    if (k == gk) continue;
                    t[nt].var = k == ek ? jb_ver(&B, row[ec[q]]) : jb_cterm(&B, c->key[k], c->tk[k], row, c->blk);
                    t[nt].c = c->coef[k]; nt++;
                }
                int32_t gv = -1;
                if (gk >= 0) {
                    JLT u[1]; u[0].var = gv = jb_term(&B, c->key[gk], JTERM_VALUE, row); u[0].c = -1;
                    jb_atom(&B, jb_str(&B, "claim bb%lld #%lld guard", c->blk, ci, 0), u, 1, J_LE, -(__int128)(q + 1));
                }
                if (B.fail || !jb_cons(&B, t, nt, J_LE, c->rhs, &goal)) return jg_fail(g, "out of memory building a claim");
                const char *kind = jg_forward_join(g, H->id) ? "hull" : g->rpoix[P->id] < g->rpoix[H->id] ? "inv.entry" : "inv.keep";
                JObl *o = jg_push(g, L);
                if (!o || !jb_finish(&B, NULL, kind, ek >= 0 ? ci * 1000 + q : ci, &goal, o)) return jg_fail(g, "out of memory building a VC");
                o->to = H; o->claim = ci;
                if (gk >= 0) { o->guarded = true; o->gvar = gv; o->gmin = q + 1; }
            }
        }
    }
    return true;
}

// ── RANKING claims: a loop's termination ────────────────────────────────────────────────────
// An edge P -> H into loop header H is a BACK edge when P comes no earlier in reverse post-order.
static bool jg_back_edge(const JG *g, int32_t P, int32_t H) { return g->rpoix[P] >= g->rpoix[H]; }
// Component j of ranking claim r over the terms' variables before (pre) and after (post) a trip:
// what 0, `rank.keep`, f(post) - f(pre) <= 0; 1, `rank.fall`, f(post) - f(pre) <= -1; 2,
// `rank.bound`, f(pre) >= 0, as -c.pre <= c0. A term fixed at the header is one variable in both.
static bool jg_rank_goal(JB *B, const JRank *r, int32_t j, const int32_t *pre, const int32_t *post, int what, JCons *goal) {
    JLT t[2 * JRANK_MAXT]; int n = 0;
    for (int32_t i = 0; i < r->nt; i++) {
        if (r->coef[j][i] == 0) continue;
        if (what != 2) { t[n].var = post[i]; t[n].c = r->coef[j][i]; n++; }
        t[n].var = pre[i]; t[n].c = -r->coef[j][i]; n++;
    }
    return jb_cons(B, t, n, J_LE, what == 0 ? 0 : what == 1 ? -1 : r->c0[j], goal);
}
// Component j NOT kept, fallen and bounded, a premise's cases: f(pre) - f(post) <= k (k -1: it rose;
// k 0: it did not fall) or, with `below`, f(pre) <= -1 (it was negative).
static bool jg_rank_not(JB *B, const JRank *r, int32_t j, const int32_t *pre, const int32_t *post, bool below, __int128 k, JCons *c) {
    JLT t[2 * JRANK_MAXT]; int n = 0;
    for (int32_t i = 0; i < r->nt; i++) {
        if (r->coef[j][i] == 0) continue;
        if (!below) { t[n].var = post[i]; t[n].c = -r->coef[j][i]; n++; }
        t[n].var = pre[i]; t[n].c = r->coef[j][i]; n++;
    }
    return jb_cons(B, t, n, J_LE, below ? -1 - r->c0[j] : k, c);
}
// Row a after a trip against row b before it: f_a(post) - f_b(pre) <= k, or with `neg` its
// negation's side, f_b(pre) - f_a(post) <= k (rows of one MAX component).
static bool jg_rank_cross(JB *B, const JRank *r, int32_t a, int32_t b, const int32_t *pre, const int32_t *post, bool neg, __int128 k, JCons *c) {
    JLT t[2 * JRANK_MAXT]; int n = 0; int s = neg ? -1 : 1;
    for (int32_t i = 0; i < r->nt; i++) {
        if (r->coef[a][i] != 0) { t[n].var = post[i]; t[n].c = s * r->coef[a][i]; n++; }
        if (r->coef[b][i] != 0) { t[n].var = pre[i]; t[n].c = -s * r->coef[b][i]; n++; }
    }
    return jb_cons(B, t, n, J_LE, k - s * (r->c0[a] - r->c0[b]), c);
}
// A guard row of a PIECEWISE ranking over `v` (pre or post): f(v) >= 0, or with `neg` f(v) <= -1.
static bool jg_row_atom(JB *B, const JRank *r, int32_t j, const int32_t *v, bool neg, JCons *c) {
    JLT t[JRANK_MAXT]; int n = 0;
    for (int32_t i = 0; i < r->nt; i++) if (r->coef[j][i] != 0) { t[n].var = v[i]; t[n].c = neg ? r->coef[j][i] : -r->coef[j][i]; n++; }
    return jb_cons(B, t, n, J_LE, neg ? -1 - r->c0[j] : r->c0[j], c);
}
// Region k's guard G_k over `v` as premises, one atom each but the last `skip` (`premise rank.region
// k pre` or `post`).
static void jb_region_premises(JB *B, const JRank *r, int32_t k, const int32_t *v, bool post, int32_t skip) {
    for (int32_t a = 0; a + skip < r->reg[k].ng && !B->fail; a++) {
        JCons c;
        if (!jg_row_atom(B, r, r->reg[k].g0 + a, v, false, &c)) { B->fail = true; return; }
        JHyp *h = jb_hyp(B, jb_str(B, post ? "premise rank.region %lld post" : "premise rank.region %lld pre", k, 0, 0));
        if (h) h->cons = c;
    }
}
// The terms' variables before (pre, at H's entry) and after (post, at P's end) one trip.
static void jb_rank_terms(JB *B, const JRank *r, IrBlock *H, IrBlock *P, int32_t *pre, int32_t *post) {
    JG *g = B->g;
    for (int32_t i = 0; i < r->nt; i++) {
        pre[i] = jb_cterm(B, r->key[i], r->tk[i], g->instate + (size_t)H->id * (size_t)g->ncells, H->id);
        post[i] = jb_cterm(B, r->key[i], r->tk[i], g->outstate + (size_t)P->id * (size_t)g->ncells, H->id);
    }
}
// The MAX (kind 1) or MIN (2) of rows a0 .. a0 + n - 1 over `v` (pre or post), a free edge's
// component c (c41): a fresh variable m (`rank.max.<c>.pre`, ...), defined exactly by m >= every row
// (m <= every row for a MIN), atoms, and m <= some row (>= some row), one n-way disjunction (origin
// `def rank.max <c> pre`, ...). One value satisfies them, the max (min) itself, so m names it and
// adds nothing else. Its recipe (mm) is that max or min of the rows' forms, for a run-time checker.
static int32_t jb_rank_mm(JB *B, const JRank *r, int32_t c, int32_t a0, int32_t n, uint8_t kind, const int32_t *v, bool post) {
    static const char *nm[2][2] = { { "rank.max.%lld.pre", "rank.max.%lld.post" }, { "rank.min.%lld.pre", "rank.min.%lld.post" } };
    static const char *og[2][2] = { { "def rank.max %lld pre", "def rank.max %lld post" }, { "def rank.min %lld pre", "def rank.min %lld post" } };
    JRecipe *rec = JG_NEW(B->g, JRecipe, 1);
    JMinMax *mf = JG_NEW(B->g, JMinMax, 1);
    if (!rec || !mf) { B->fail = true; return 0; }
    memset(rec, 0, sizeof *rec); memset(mf, 0, sizeof *mf);
    rec->fval = -1; rec->mm = kind == 1 ? 1 : -1; rec->mmf = mf; mf->nf = n;
    int32_t m = jb_newvar(B, jb_str(B, nm[kind - 1][post], c, 0, 0), 3, c, NULL);
    if (B->fail) return 0;
    B->qt--;                                                   // nothing to state for it
    B->vrec[m] = rec;
    const char *o = jb_str(B, og[kind - 1][post], c, 0, 0);
    int32_t s = kind == 1 ? 1 : -1;                            // a MAX: f_a - m <= -c0_a; a MIN: m - f_a <= c0_a
    JCons cs[JRANK_MAXC];
    for (int32_t a = 0; a < n && !B->fail; a++) {
        JLT t[JRANK_MAXT + 1], u[JRANK_MAXT + 1]; int k = 0, q = 0;
        for (int32_t i = 0; i < r->nt; i++) {
            if (r->coef[a0 + a][i] == 0) continue;
            mf->fvar[a][q] = v[i]; mf->fcoef[a][q] = r->coef[a0 + a][i]; q++;
            t[k].var = v[i]; t[k].c = s * r->coef[a0 + a][i]; k++;
        }
        mf->fn[a] = (uint8_t)q; mf->fc0[a] = r->c0[a0 + a];
        t[k].var = m; t[k].c = -s; k++;
        for (int32_t i = 0; i < k; i++) { u[i] = t[i]; u[i].c = -t[i].c; }   // jb_cons sorts and merges t
        jb_atom(B, o, t, k, J_LE, -s * r->c0[a0 + a]);        // m on the row's far side
        if (!jb_cons(B, u, k, J_LE, s * r->c0[a0 + a], &cs[a])) B->fail = true;   // its near side, in some row
    }
    if (!B->fail) {
        if (n == 1) { JHyp *h = jb_hyp(B, o); if (h) h->cons = cs[0]; }
        else jb_orn(B, o, cs, n);
    }
    return m;
}
// A component of a free edge's tuple: one row (row >= 0), or a MAX or MIN group's values (mpre,
// mpost) as jb_rank_mm names them (row -1, a side not named -1). jg_rank_not and jg_rank_goal for
// it; a side the VC did not name refuses (its first reason, before the caller's).
typedef struct { int32_t row, mpre, mpost; } JLexC;
static bool jg_lexc_named(JB *B, const JLexC *C, bool post) {
    if (C->mpre >= 0 && (!post || C->mpost >= 0)) return true;
    B->fail = true;
    return jg_fail(B->g, "a ranking VC over a max or min of rows it did not define");
}
static bool jg_lexc_not(JB *B, const JRank *r, const JLexC *C, const int32_t *pre, const int32_t *post, bool below, __int128 k, JCons *c) {
    if (C->row >= 0) return jg_rank_not(B, r, C->row, pre, post, below, k, c);
    if (!jg_lexc_named(B, C, !below)) return false;
    JLT t[2]; int n = 0;
    if (!below) { t[n].var = C->mpost; t[n].c = -1; n++; }
    t[n].var = C->mpre; t[n].c = 1; n++;
    return jb_cons(B, t, n, J_LE, below ? -1 : k, c);
}
static bool jg_lexc_goal(JB *B, const JRank *r, const JLexC *C, const int32_t *pre, const int32_t *post, int what, JCons *goal) {
    if (C->row >= 0) return jg_rank_goal(B, r, C->row, pre, post, what, goal);
    if (!jg_lexc_named(B, C, what != 2)) return false;
    JLT t[2]; int n = 0;
    if (what != 2) { t[n].var = C->mpost; t[n].c = 1; n++; }
    t[n].var = C->mpre; t[n].c = -1; n++;
    return jb_cons(B, t, n, J_LE, what == 1 ? -1 : 0, goal);
}
// The components of the tuple rows t0 .. t0 + nr - 1: component k is rows s0[k] .. s0[k] + sn[k] - 1,
// a row no MAX or MIN joins starting one (JRank.cont); their count, and in *cb how many come before
// t0 (a component's number in the whole ranking is cb + k).
static int32_t jg_lex_comps(const JRank *r, int32_t t0, int32_t nr, int32_t *s0, int32_t *sn, int32_t *cb) {
    int32_t n = 0;
    *cb = 0;
    for (int32_t j = 0; j < t0; j++) if (!r->cont[j]) (*cb)++;
    for (int32_t j = t0; j < t0 + nr; j++) {
        if (r->cont[j] && n) { sn[n - 1]++; continue; }
        s0[n] = j; sn[n] = 1; n++;
    }
    return n;
}
// VC q of a FREE edge's lexicographic decrease over the tuple rows t0 .. t0 + nr - 1 (c35), into B,
// whose terms before and after are pre and post: some component j with each one before it kept, j
// fallen by 1 at least and j >= 0 before. Under the premises that no j < n - 1 did (`premise
// rank.lex j`, each a disjunction of how it did not), the last did, each of its conjuncts one VC: q
// < n - 1 component q kept, n - 1 fallen at q = n - 1 and >= 0 at q = n. A MAX or MIN component
// (c41) is the fresh variable jb_rank_mm defines on each side, where the VC uses it: the premises,
// each side of k < last; the goal, its after only where it falls (q = last). A group starts inside
// the tuple (the caller checked cont[t0] == 0). A loop's back edge (jg_lex_vcs) and a recursion's
// call (jgk_rec_lex, c42) state it alike.
static bool jb_lex_vc(JB *B, const JRank *r, const int32_t *pre, const int32_t *post, int32_t t0, int32_t nr, int32_t q, JCons *goal) {
    int32_t s0[JRANK_MAXC], sn[JRANK_MAXC], cb, n = jg_lex_comps(r, t0, nr, s0, sn, &cb);
    int32_t last = n - 1, gc = q < last ? q : last;
    JLexC C[JRANK_MAXC];
    for (int32_t k = 0; k < n && !B->fail; k++) {             // a group is named where its VC uses it
        C[k].row = sn[k] == 1 ? s0[k] : -1; C[k].mpre = C[k].mpost = -1;
        if (sn[k] == 1 || (k == last && k != gc)) continue;
        uint8_t kind = r->cont[s0[k] + 1];
        C[k].mpre = jb_rank_mm(B, r, cb + k, s0[k], sn[k], kind, pre, false);
        if (k < last || q == last) C[k].mpost = jb_rank_mm(B, r, cb + k, s0[k], sn[k], kind, post, true);
    }
    for (int32_t j = 0; j + 1 < n && !B->fail; j++) {
        JCons cs[JRANK_MAXC + 1];
        for (int32_t i = 0; i <= j; i++)                       // i < j rose, or j did not fall
            if (!jg_lexc_not(B, r, &C[i], pre, post, false, i < j ? -1 : 0, &cs[i])) B->fail = true;
        if (!B->fail && !jg_lexc_not(B, r, &C[j], pre, post, true, 0, &cs[j + 1])) B->fail = true;   // or j < 0
        if (!B->fail) jb_orn(B, jb_str(B, "premise rank.lex %lld", j, 0, 0), cs, j + 2);
    }
    return !B->fail && jg_lexc_goal(B, r, &C[gc], pre, post, q < last ? 0 : q == last ? 1 : 2, goal);
}
// A loop's FREE back edge P -> H (fall -1): its n + 1 `rank.lex` VCs (ordinal ob + q), jb_lex_vc's,
// at P's end on the edge; in region `reg` (>= 0) each also under G_reg before and after.
static bool jg_lex_vcs(JG *g, JObls *L, const JRank *r, IrBlock *P, int32_t np, IrBlock *H, int32_t t0, int32_t nr, int32_t reg, int32_t ob) {
    int32_t s0[JRANK_MAXC], sn[JRANK_MAXC], cb, n = jg_lex_comps(r, t0, nr, s0, sn, &cb);
    for (int32_t q = 0; q <= n; q++) {
        JB B = jb_begin(g, P->id, np, -1);
        if (B.fail) return jg_fail(g, "out of memory building a VC");   // its arrays are not there
        B.to = H->id;
        int32_t pre[JRANK_MAXT], post[JRANK_MAXT];
        jb_rank_terms(&B, r, H, P, pre, post);
        if (reg >= 0) { jb_region_premises(&B, r, reg, pre, false, 0); jb_region_premises(&B, r, reg, post, true, 0); }
        JCons goal;
        if (B.fail || !jb_lex_vc(&B, r, pre, post, t0, nr, q, &goal)) return jg_fail(g, "out of memory building a ranking VC");
        JObl *o = jg_push(g, L);
        if (!o || !jb_finish(&B, NULL, "rank.lex", ob + q, &goal, o)) return jg_fail(g, "out of memory building a VC");
        o->to = H; o->claim = -1;
    }
    return true;
}
// A PIECEWISE ranking's back edge P -> H, its regions 0 .. m - 1 (validated by the caller). With
// rho(s) the largest k whose guard holds, the ranking is <rho, tuple_rho>, lexicographic:
// (A) `rank.cover`: the trip starts in some region, under the premises that it is in none of 0 ..
//     m - 2 (`premise rank.cover k`, the disjunction of G_k's atoms negated) G_{m-1}'s atoms, one VC
//     each (ordinal the atom);
// (B) `rank.region`: for each k < l, G_k before and G_l after cannot both hold: under G_k(pre) and
//     G_l(post) but its last atom, that atom fails (ordinal 100*k + l);
// (C) within region k, its tuple falls as a free edge's (`rank.lex`, ordinal 1000*k + q), under
//     G_k before and after.
// For s -> s' with k = rho(s), l = rho(s'): (B) rules out l > k, and l = k is (C)'s case. INCOMPLETE
// where guards overlap: (B) is owed from every guard true before the trip, so a state in G_0 and
// G_1 (rho 1) that stays in G_1 is refused, though legal; overlap is accepted only where no such
// stay exists. A trip that ends in no region (its loop's exit) owes nothing.
typedef char jg_rank_ordinals_fit[(JRANK_MAXC < 1000 && JRANK_MAXR < 100) ? 1 : -1];
// A piecewise ranking's fields (JRank.nreg), every one checked, none skipped (I.197): at most
// JRANK_MAXR regions, each with guard and tuple rows inside the rows, every row in exactly one of
// them, a MAX or MIN only inside a tuple (no guard row joins one, and none starts before its tuple:
// the ranges tile the rows, so none runs past it either); past nreg, every field 0. NULL where they
// hold, else what is wrong. A loop's (jg_rank_obligations) and a recursion's (jg_entry_rank, c43).
static const char *jg_regions_bad(const JRank *r) {
    if (r->nreg > JRANK_MAXR) return "a ranking claim of more regions than it may have";
    for (int32_t k = r->nreg; k < JRANK_MAXR; k++)
        if (r->reg[k].g0 || r->reg[k].ng || r->reg[k].t0 || r->reg[k].nt) return "a ranking claim's region past its count";
    if (!r->nreg) return NULL;
    int32_t uses[JRANK_MAXC] = { 0 };
    for (int32_t k = 0; k < r->nreg; k++) {
        int32_t g0 = r->reg[k].g0, ng = r->reg[k].ng, t0 = r->reg[k].t0, nt = r->reg[k].nt;
        if (ng < 1 || nt < 1 || g0 + ng > r->ncomp || t0 + nt > r->ncomp) return "a ranking claim's region out of shape";
        for (int32_t j = g0; j < g0 + ng; j++) uses[j]++;
        for (int32_t j = t0; j < t0 + nt; j++) uses[j]++;
    }
    for (int32_t j = 0; j < r->ncomp; j++) if (uses[j] != 1) return "a ranking claim's row in no region or in two";
    for (int32_t k = 0; k < r->nreg; k++) {
        for (int32_t j = r->reg[k].g0; j < r->reg[k].g0 + r->reg[k].ng; j++)
            if (r->cont[j]) return "a ranking claim's region guard in a max or min of rows";
        if (r->cont[r->reg[k].t0]) return "a ranking claim's max or min of rows across a region's start";
    }
    return NULL;
}
// (A)'s VC a into B, its terms before the trip pre: under the premises that the trip starts in none
// of regions 0 .. m - 2, region m - 1's atom a.
static bool jb_cover_vc(JB *B, const JRank *r, const int32_t *pre, int32_t a, JCons *goal) {
    int32_t m = r->nreg;
    for (int32_t k = 0; k + 1 < m && !B->fail; k++) {
        JCons cs[JRANK_MAXC];
        for (int32_t i = 0; i < r->reg[k].ng && !B->fail; i++)
            if (!jg_row_atom(B, r, r->reg[k].g0 + i, pre, true, &cs[i])) B->fail = true;
        const char *o = jb_str(B, "premise rank.cover %lld", k, 0, 0);
        if (B->fail) break;
        if (r->reg[k].ng == 1) { JHyp *h = jb_hyp(B, o); if (h) h->cons = cs[0]; }
        else jb_orn(B, o, cs, r->reg[k].ng);
    }
    return !B->fail && jg_row_atom(B, r, r->reg[m - 1].g0 + a, pre, false, goal);
}
// (B)'s VC (k, l): under G_k before and G_l after but its last atom, that atom fails.
static bool jb_region_pair_vc(JB *B, const JRank *r, const int32_t *pre, const int32_t *post, int32_t k, int32_t l, JCons *goal) {
    jb_region_premises(B, r, k, pre, false, 0);
    jb_region_premises(B, r, l, post, true, 1);
    return !B->fail && jg_row_atom(B, r, r->reg[l].g0 + r->reg[l].ng - 1, post, true, goal);
}
static bool jg_region_vcs(JG *g, JObls *L, const JRank *r, IrBlock *P, int32_t np, IrBlock *H) {
    int32_t m = r->nreg;
    for (int32_t a = 0; a < r->reg[m - 1].ng; a++) {           // (A)
        JB B = jb_begin(g, P->id, np, -1);
        if (B.fail) return jg_fail(g, "out of memory building a VC");   // its arrays are not there
        B.to = H->id;
        int32_t pre[JRANK_MAXT], post[JRANK_MAXT];
        jb_rank_terms(&B, r, H, P, pre, post);
        JCons goal;
        if (B.fail || !jb_cover_vc(&B, r, pre, a, &goal)) return jg_fail(g, "out of memory building a ranking VC");
        JObl *o = jg_push(g, L);
        if (!o || !jb_finish(&B, NULL, "rank.cover", a, &goal, o)) return jg_fail(g, "out of memory building a VC");
        o->to = H; o->claim = -1;
    }
    for (int32_t k = 0; k < m; k++)                              // (B)
        for (int32_t l = k + 1; l < m; l++) {
            JB B = jb_begin(g, P->id, np, -1);
            if (B.fail) return jg_fail(g, "out of memory building a VC");   // its arrays are not there
            B.to = H->id;
            int32_t pre[JRANK_MAXT], post[JRANK_MAXT];
            jb_rank_terms(&B, r, H, P, pre, post);
            JCons goal;
            if (B.fail || !jb_region_pair_vc(&B, r, pre, post, k, l, &goal)) return jg_fail(g, "out of memory building a ranking VC");
            JObl *o = jg_push(g, L);
            if (!o || !jb_finish(&B, NULL, "rank.region", 100 * k + l, &goal, o)) return jg_fail(g, "out of memory building a VC");
            o->to = H; o->claim = -1;
        }
    for (int32_t k = 0; k < m; k++)                              // (C)
        if (!jg_lex_vcs(g, L, r, P, np, H, r->reg[k].t0, r->reg[k].nt, k, 1000 * k)) return false;
    return true;
}
// The WRITTEN measure (`while c decreasing m`, D-44), read as a ranking claim's one component:
// lowering computes m at the start of the body, under the guard (lower.h, I.74), so m is a sum over
// what the trip starts from: a load of a cell reading the version the header makes or keeps, a value
// fixed at the header (a parameter, a value defined before the loop), a slice's length of either, a
// constant; through `+`, `-`, unary `-` and `*` by a constant under CHECK, and an integer cast that
// keeps the value (as VRA's vra_lin_of reads it). False on anything else.
static bool jg_rank_add(JRank *r, int32_t key, uint8_t tk, __int128 k) {
    for (int32_t i = 0; i < r->nt; i++) if (r->key[i] == key && r->tk[i] == tk) { r->coef[0][i] += k; return true; }
    if (r->nt == JRANK_MAXT) return false;
    r->key[r->nt] = key; r->tk[r->nt] = tk; r->coef[0][r->nt] = k; r->nt++;
    return true;
}
// a * b into *out, refused (false) when it would pass 2^100 in magnitude: a measure's coefficients
// stay far inside __int128, whose overflow would be undefined in the generator itself
static bool jg_mul_ok(__int128 a, __int128 b, __int128 *out) {
    const __int128 lim = (__int128)1 << 100;
    __int128 ua = a < 0 ? -a : a, ub = b < 0 ? -b : b;
    if (ua > lim || ub > lim || (ub != 0 && ua > lim / ub)) return false;
    *out = a * b; return true;
}
// A QUOTIENT fixed at header b: a division by a nonzero constant (a signed one checked: it exists only
// where it completes) of a value fixed at b. Every trip computes it from the same operands, so it is
// one value before and after a trip, wherever in the loop it is made: a written measure's term.
static bool jg_fixed_quot(JG *g, int32_t b, int32_t key) {
    IrInstr *d = key >= 0 && key < g->nv ? g->def[key] : NULL;
    __int128 c;
    if (!d || (d->op != IR_SDIV && d->op != IR_UDIV) || d->n_operands < 2 || !jg_is_int(d->result->type)) return false;
    if (d->op == IR_SDIV && (d->wrap != IR_WRAP_CHECK || d->unchecked)) return false;
    return jg_const_of(g, d->operands[1], &c) && c != 0 && d->operands[0] && jg_term_ok(g, b, d->operands[0]->id, JTERM_VALUE);
}
static bool jg_measure_lin(JG *g, IrBlock *H, IrValue *v, __int128 k, JRank *r, int depth) {
    const __int128 big = (__int128)1 << 66;
    if (!v || v->id < 0 || v->id >= g->nv || depth > 16 || k > big || k < -big) return false;
    const int32_t *row = g->instate + (size_t)H->id * (size_t)g->ncells;
    if (g->loadver[v->id] >= 0) {                              // a load: the version the trip starts from
        int32_t ver = g->loadver[v->id], c = g->ver[ver].cell;
        return row[c] == ver && jg_is_int(g->cty[c]) && jg_rank_add(r, g->cellv[c]->id, JTERM_VALUE, k);
    }
    IrInstr *d = g->def[v->id];
    if (!d || jg_term_ok(g, H->id, v->id, JTERM_VALUE) || jg_fixed_quot(g, H->id, v->id))
        return jg_is_int(v->type) && jg_rank_add(r, v->id, JTERM_VALUE, k);
    __int128 c;
    switch (d->op) {
        case IR_CONST: {
            __int128 kc;
            if (!jg_is_int(v->type) || !jg_mul_ok(k, jg_const(d), &kc)) return false;
            r->c0[0] += kc;                                    // each term <= 2^100, at most 2^16 of them
            return r->c0[0] <= big && r->c0[0] >= -big;
        }
        case IR_ADD: case IR_SUB:
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 2) return false;
            return jg_measure_lin(g, H, d->operands[0], k, r, depth + 1) &&
                   jg_measure_lin(g, H, d->operands[1], d->op == IR_ADD ? k : -k, r, depth + 1);
        case IR_NEG:
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 1) return false;
            return jg_measure_lin(g, H, d->operands[0], -k, r, depth + 1);
        case IR_MUL:
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 2) return false;
            for (int q = 0; q < 2; q++) {
                __int128 kc;
                if (jg_const_of(g, d->operands[q], &c))
                    return jg_mul_ok(k, c, &kc) && jg_measure_lin(g, H, d->operands[1 - q], kc, r, depth + 1);
            }
            return false;
        case IR_CAST: {
            const IrType *st = d->n_operands ? d->operands[0]->type : NULL, *dt = v->type;
            if (!st || !dt || st->kind != IRT_INT || dt->kind != IRT_INT || st->bits > dt->bits) return false;
            if (st->is_signed && !dt->is_signed) return false;
            if (!st->is_signed && dt->is_signed && st->bits >= dt->bits) return false;
            return jg_measure_lin(g, H, d->operands[0], k, r, depth + 1);
        }
        case IR_SLICE_LEN: {
            IrValue *sv = d->n_operands ? d->operands[0] : NULL;
            if (!sv || sv->id < 0 || sv->id >= g->nv) return false;
            if (g->loadver[sv->id] >= 0) {                     // a slice cell's length, as the trip starts
                int32_t ver = g->loadver[sv->id], sc = g->ver[ver].cell;
                return row[sc] == ver && jg_rank_add(r, g->cellv[sc]->id, JTERM_LEN, k);
            }
            return jg_term_ok(g, H->id, sv->id, JTERM_LEN) && jg_rank_add(r, sv->id, JTERM_LEN, k);
        }
        default: return false;
    }
}
// A loop's termination is owed where VRA owes it (vra.h, D-44): at every loop header, unless the
// function declares `diverge` and the loop has no written measure. With a ranking claim at the
// header, on each back edge P -> H at the end of P, the edge's condition and the header's claims
// about its previous entry facts (as for `inv.keep`), with j* the component the claim says falls
// there: `rank.keep` for each component before j*, `rank.fall` and `rank.bound` for j*. That is
// a lexicographic ranking function (ADFG): each trip lowers the tuple, which is bounded below where
// it falls. With none, `outside`: the judge proves no termination it is not shown. A ranking claim
// the generator cannot state, or one that leaves a back edge out, refuses the function.
static bool jg_rank_obligations(JG *g, JObls *L, const JRank *rk, int32_t nrk) {
    const __int128 big = (__int128)1 << 66;
    // a ranking claim names its function: a block id alone means a block in every function
    for (int32_t k = 0; k < nrk; k++) if (!rk[k].fn) return jg_fail(g, "a ranking claim with no function");
    for (int32_t r0 = 0; r0 < g->nrpo; r0++) {
        IrBlock *H = g->blk[g->rpo[r0]];
        if (!H->is_loop_header || (g->f->may_diverge && !H->has_measure)) continue;
        const JRank *r = NULL;
        for (int32_t k = 0; k < nrk; k++) {
            if (rk[k].blk != H->id || rk[k].fn != g->f) continue;
            if (r) return jg_fail(g, "two ranking claims at one loop header");
            r = &rk[k];
        }
        // a WRITTEN measure is what is owed, `measure.fall` and `measure.bound` on every back edge
        // (E082 where it fails); a ranking claim proposed for the loop is not needed, and not read
        JRank mr; bool measure = H->has_measure;
        if (measure) {
            memset(&mr, 0, sizeof mr);
            mr.blk = H->id; mr.fn = g->f; mr.ncomp = 1;
            if (!H->measure_val || !jg_measure_lin(g, H, H->measure_val, 1, &mr, 0)) {
                if (!jg_outside(g, L, NULL, H->id, 0, "measure.fall", "a written measure the generator cannot read as a sum")) return false;
                continue;
            }
            for (IrEdge *pe = H->preds; pe; pe = pe->next) {
                IrBlock *P = pe->block;
                if (g->rpoix[P->id] < 0 || !jg_back_edge(g, P->id, H->id)) continue;
                bool again = false;
                for (int32_t e2 = 0; e2 < mr.nedge; e2++) if (mr.from[e2] == P->id) again = true;
                if (again) continue;
                if (mr.nedge == JRANK_MAXE) { if (!jg_outside(g, L, NULL, H->id, 0, "measure.fall", "a written measure on a loop of too many back edges")) return false; measure = false; break; }
                mr.from[mr.nedge] = P->id; mr.fall[mr.nedge] = 0; mr.nedge++;
            }
            if (!measure) continue;
            r = &mr;                                           // a constant measure (no terms) cannot fall
        }
        if (!r) { if (!jg_outside(g, L, NULL, H->id, 0, "rank.fall", "no ranking function proposed for this loop")) return false; continue; }
        if (r->ncomp < 1 || r->ncomp > JRANK_MAXC || r->nedge < 0 || r->nedge > JRANK_MAXE)
            return jg_fail(g, "a ranking claim of no components, or of more than it may have");
        if (r->nle != 0) return jg_fail(g, "a loop's ranking claim lists calls (a recursion's measure does)");
        // a CONSTANT one (no terms), written or proposed, falls on no edge that is ever taken: it is
        // how a loop whose back edges are dead (`while false`, a fixed guard) is shown to end
        if (r->nt != 0 && !jg_claim_shape(g, r->key, r->tk, r->nt, JRANK_MAXT)) return false;
        for (int32_t i = 0; i < r->nt; i++)                    // a written measure's: a fixed quotient too
            if (!jg_term_ok(g, H->id, r->key[i], r->tk[i]) && !(r == &mr && r->tk[i] == JTERM_VALUE && jg_fixed_quot(g, H->id, r->key[i])) &&
                !(r->tk[i] == JTERM_ENTRY && jg_entry_ok(g, H->id, r->key[i])))   // ENTRY: fixed in the loop
                return jg_fail(g, "a ranking claim's term is not a cell or a value fixed at its header");
        for (int32_t j = 0; j < r->ncomp; j++) {
            if (r->c0[j] > big || r->c0[j] < -big) return jg_fail(g, "a ranking claim's constant out of range");
            for (int32_t i = 0; i < r->nt; i++)
                if (r->coef[j][i] > big || r->coef[j][i] < -big) return jg_fail(g, "a ranking claim's coefficient out of range");
        }
        // its components: c is rows c0r[c] .. c0r[c] + cn[c] - 1, where cn[c] >= 2 their MAX (gk 1)
        // or their MIN (gk 2), one kind per component
        int32_t c0r[JRANK_MAXC], cn[JRANK_MAXC], gk[JRANK_MAXC], nc = 0;
        for (int32_t j = 0; j < r->ncomp; j++) {
            int32_t k = r->cont[j];
            if (k > 2 || (j == 0 && k) || (k && cn[nc - 1] > 1 && gk[nc - 1] != k)) return jg_fail(g, "a ranking claim's max or min of rows out of shape");
            if (k) { cn[nc - 1]++; gk[nc - 1] = k; } else { c0r[nc] = j; cn[nc] = 1; gk[nc] = 0; nc++; }
        }
        // a PIECEWISE one (JRank.nreg): its fields as jg_regions_bad checks them, every edge free
        const char *rb = jg_regions_bad(r);
        if (rb) return jg_fail(g, rb);
        for (int32_t e = 0; r->nreg && e < r->nedge; e++) if (r->fall[e] != -1) return jg_fail(g, "a piecewise ranking claim with a fixed falling component");
        for (int32_t e = 0; e < r->nedge; e++) {
            if (r->fall[e] < -1 || r->fall[e] >= nc) return jg_fail(g, "a ranking claim's falling component out of range");
            bool is = false;
            for (IrEdge *pe = H->preds; pe; pe = pe->next)
                if (pe->block->id == r->from[e] && g->rpoix[r->from[e]] >= 0 && jg_back_edge(g, r->from[e], H->id)) is = true;
            if (!is) return jg_fail(g, "a ranking claim names an edge that is no back edge of its loop");
            for (int32_t e2 = 0; e2 < e; e2++) if (r->from[e2] == r->from[e]) return jg_fail(g, "a ranking claim names one back edge twice");
        }
        for (IrEdge *pe = H->preds; pe; pe = pe->next) {
            IrBlock *P = pe->block;
            if (g->rpoix[P->id] < 0 || !jg_back_edge(g, P->id, H->id)) continue;
            bool again = false;
            for (IrEdge *e2 = H->preds; e2 != pe; e2 = e2->next) if (e2->block == P) again = true;
            if (again) continue;
            int32_t e = -1;
            for (int32_t k = 0; k < r->nedge; k++) if (r->from[k] == P->id) e = k;
            if (e < 0) return jg_fail(g, "a back edge the ranking claim does not name");
            int32_t np = 0;
            for (IrInstr *i = P->instrs; i; i = i->next) np++;
            int32_t fall = r->fall[e];
            // a FREE edge (fall -1): its trips may lower different components, one per path; owed
            // the lexicographic decrease itself (jg_lex_vcs). Then a run lowers, at its least
            // component falling infinitely often, a value bounded below while every component
            // before it stays: ADFG's argument, for any function of the state, so for a MAX or MIN
            // of rows too. A PIECEWISE ranking's edges are all free (jg_region_vcs)
            if (r->nreg ? !jg_region_vcs(g, L, r, P, np, H) : fall < 0 && !jg_lex_vcs(g, L, r, P, np, H, 0, r->ncomp, -1, 0)) return false;
            // a component that is the MAX of rows R: keeping or falling by d is, for each row a,
            // f_a(post) <= f_b(pre) - d for some b in R: one VC per a (ordinal 1000*(q + 1) + c, a
            // the q-th row), its goal against R's last row under the premises that no other b
            // gave it (`premise rank.max c`); bounded is f_b(pre) >= 0 for some b, one VC (ordinal
            // c) under the premises that no other b is. Atoms all, no fresh variable.
            // The MIN of rows R: keeping or falling by d is, for some row a, f_a(post) <= f_b(pre)
            // - d for every b: under the premises that no a before R's last did (`premise rank.min
            // c`, each the disjunction over b of f_a(post) - f_b(pre) >= 1 - d), the last does,
            // one VC per b (ordinal 1000*(q + 1) + c, b the q-th row); bounded is every
            // f_b(pre) >= 0, one VC per b, no premise
            for (int32_t j = 0; j <= fall; j++)
                for (int what = j < fall ? 0 : 1; what <= (j < fall ? 0 : 2); what++)
                for (int32_t q = 0; q < (cn[j] == 1 || (gk[j] == 1 && what == 2) ? 1 : cn[j]); q++) {
                    JB B = jb_begin(g, P->id, np, -1);
                    if (B.fail) return jg_fail(g, "out of memory building a VC");   // its arrays are not there
                    B.to = H->id;
                    int32_t pre[JRANK_MAXT], post[JRANK_MAXT];
                    for (int32_t i = 0; i < r->nt; i++) {
                        pre[i] = jb_cterm(&B, r->key[i], r->tk[i], g->instate + (size_t)H->id * (size_t)g->ncells, H->id);
                        post[i] = jb_cterm(&B, r->key[i], r->tk[i], g->outstate + (size_t)P->id * (size_t)g->ncells, H->id);
                    }
                    int32_t last = c0r[j] + cn[j] - 1, a = c0r[j] + q, d = what == 1 ? 1 : 0;
                    JCons goal; bool ok;
                    if (gk[j] != 2) {                          // one row, or a MAX
                        for (int32_t b = c0r[j]; b < last && !B.fail; b++) {
                            JCons pc;
                            if (what == 2 ? !jg_rank_not(&B, r, b, pre, post, true, 0, &pc) : !jg_rank_cross(&B, r, a, b, pre, post, true, d - 1, &pc)) B.fail = true;
                            else { JHyp *h = jb_hyp(&B, jb_str(&B, "premise rank.max %lld", j, 0, 0)); if (h) h->cons = pc; }
                        }
                        ok = !B.fail && (cn[j] == 1 || what == 2 ? jg_rank_goal(&B, r, last, pre, post, what, &goal)
                                                                  : jg_rank_cross(&B, r, a, last, pre, post, false, -d, &goal));
                    } else {                                   // a MIN: a is the q-th row as b
                        for (int32_t a2 = c0r[j]; what != 2 && a2 < last && !B.fail; a2++) {
                            JCons cs[JRANK_MAXC];
                            for (int32_t b = c0r[j]; b <= last && !B.fail; b++)
                                if (!jg_rank_cross(&B, r, a2, b, pre, post, true, d - 1, &cs[b - c0r[j]])) B.fail = true;
                            if (!B.fail) jb_orn(&B, jb_str(&B, "premise rank.min %lld", j, 0, 0), cs, cn[j]);
                        }
                        ok = !B.fail && (what == 2 ? jg_rank_goal(&B, r, a, pre, post, what, &goal)
                                                   : jg_rank_cross(&B, r, last, a, pre, post, false, -d, &goal));
                    }
                    if (!ok) return jg_fail(g, "out of memory building a ranking VC");
                    JObl *o = jg_push(g, L);
                    const char *kind = measure ? (what == 1 ? "measure.fall" : "measure.bound")
                                               : what == 0 ? "rank.keep" : what == 1 ? "rank.fall" : "rank.bound";
                    if (!o || !jb_finish(&B, NULL, kind, cn[j] == 1 || (gk[j] == 1 && what == 2) ? j : 1000 * (q + 1) + j, &goal, o))
                        return jg_fail(g, "out of memory building a VC");
                    o->to = H; o->claim = -1;                // an obligation: a ranking claim is never a fact
                }
        }
    }
    return true;
}

// ── RECURSION: a function's termination at the calls that may close a cycle through it ──────
// Owed where VRA owes it (vra.h, D-44): a function on a recursion cycle of the module's call graph
// (src/ir/callgraph.h, the set VRA reads too: a direct call to the callee named, an indirect one to
// every address-taken function of its arity), unless it declares `diverge` and writes no
// `decreasing`. Its measure is a ranking claim at its ENTRY (JRANK_ENTRY): one component over its
// integer parameters, or their slices' lengths, as it was entered. At each call that may reach a
// function of its component, under the facts at the call: the callee's measure at the call's
// arguments falls below the caller's at entry (`rec.fall`), or at a call the caller's measure lists
// does not rise above it (`rec.le`), and the caller's is non-negative (`rec.bound`). The listed
// calls of the whole component close no cycle (jg_rec_cut), so every cycle of calls falls at one
// call at least: spec 12's constraint (`a -> b(n - 1) -> c(n) -> a`, only b -> c listed). Then a
// chain of activations in the component, |component| calls long, lowers a measure bounded below,
// read in Z as every fact is. A self-recursion's measure may have several rows (c42): a tuple, a
// MAX or MIN of rows among its components, every call FREE and owing the lexicographic decrease
// (`rec.lex`, jgk_rec_lex); or regions (c43), owing c38's `rec.cover`, `rec.region` and `rec.lex`
// at each call; refused in a mutual recursion (v1). A
// call through a pointer that may close the cycle, or a member with no measure, is `outside` there.
// A measure the generator cannot state (a term no parameter of its function, a second one for the
// same function) refuses the function. No test of the function's KIND: every lowered body is a
// `func`, and under authority the effect row's `diverge` is read from the row alone (Main Compiler
// Work): an accepted program has every loop and recursion obligation of a function without it
// proved, which holds only if this is owed at every function on a cycle the row does not exempt,
// and an `outside` here refuses.
static const JRank *jg_entry_rank(JG *g, const JRank *rk, int32_t nrk, const IrFunc *h, bool *bad) {
    const JRank *r = NULL; *bad = false;
    for (int32_t k = 0; k < nrk; k++) {
        if (rk[k].blk != JRANK_ENTRY || rk[k].fn != h) continue;
        if (r) { *bad = true; jg_fail(g, "two measures for one function's recursion"); return NULL; }
        r = &rk[k];
    }
    if (!r) return NULL;
    // its listed calls are jg_rec_cut's to check, over the whole component (this function's own
    // too); its edges are not read (the finder fills them as for a loop: they mean nothing here).
    // Its rows: one component, or (c42) several, a MAX or MIN of rows among them (JRank.cont, as a
    // loop's), in regions (c43, as a loop's: jg_regions_bad); every field checked (I.197)
    const __int128 big = (__int128)1 << 66;
    const char *why = NULL;
    if (r->ncomp < 1 || r->ncomp > JRANK_MAXC || r->nt < 0 || r->nt > JRANK_MAXT) why = "a recursion's measure of no rows, or of more than it may have";
    if (!why) why = jg_regions_bad(r);
    for (int32_t j = 0; !why && j < r->ncomp; j++) {
        uint8_t k = r->cont[j];
        if (k > 2 || (j == 0 && k) || (k && j >= 2 && r->cont[j - 1] && r->cont[j - 1] != k)) why = "a recursion's measure's max or min of rows out of shape";
        if (!why && (r->c0[j] > big || r->c0[j] < -big)) why = "a recursion's measure's constant out of range";
        for (int32_t i = 0; !why && i < r->nt; i++)
            if (r->coef[j][i] > big || r->coef[j][i] < -big) why = "a recursion's measure's coefficient out of range";
    }
    for (int32_t i = 0; !why && i < r->nt; i++) {
        int pk = jg_param_at(h, r->key[i]);
        const IrParam *pp = h->params;
        for (int q = 0; q < pk && pp; q++) pp = pp->next;
        bool ok = pk >= 0 && pp && pp->value && jg_term_kind(pp->value->type, r->tk[i]);
        for (int32_t j = 0; ok && j < i; j++) ok = r->key[j] != r->key[i] || r->tk[j] != r->tk[i];
        if (!ok) why = "a recursion's measure is not over its function's parameters";
    }
    if (why) { *bad = true; jg_fail(g, why); return NULL; }
    return r;
}
// The value of h's parameter `key` at the call `c` (an argument), or at h's own entry.
static IrValue *jg_param_value(const IrFunc *h, int32_t key) {
    for (IrParam *p = h->params; p; p = p->next) if (p->value && p->value->id == key) return p->value;
    return NULL;
}
// what: 0 `rec.fall` m_t(args) - m_f <= -1, 1 `rec.bound` -m_f <= 0, 2 `rec.le` m_t(args) - m_f <= 0
// (each with its constants moved right)
typedef struct { const JRank *r, *rt; const IrFunc *f, *t; int what; } JRecArg;
static bool jgk_rec(JB *B, IrInstr *at, void *arg, JCons *goal) {
    JRecArg *a = arg; JLT t[2 * JRANK_MAXT]; int n = 0;
    __int128 rhs = a->what == 1 ? a->r->c0[0] : (a->what == 0 ? -1 : 0) - a->rt->c0[0] + a->r->c0[0];
    if (a->what != 1)                                           // the callee's measure at the arguments
        for (int32_t i = 0; i < a->rt->nt; i++) {
            if (a->rt->coef[0][i] == 0) continue;
            int pk = jg_param_at(a->t, a->rt->key[i]), base = at->aux.callee ? 0 : 1;   // op[0]: the pointer
            // a call's arguments match its callee's parameters (the front end refuses a call of
            // another arity; a pointer's targets have its arity, callgraph.h), so this refusal is
            // unreachable: kept, fail closed
            if (pk < 0 || base + pk >= at->n_operands) return false;
            IrValue *av = at->operands[base + pk];
            t[n].var = a->rt->tk[i] == JTERM_LEN ? jb_len(B, av) : jb_val(B, av); t[n].c = a->rt->coef[0][i]; n++;
        }
    for (int32_t i = 0; i < a->r->nt; i++) {                    // less the caller's at its entry
        if (a->r->coef[0][i] == 0) continue;
        IrValue *pv = jg_param_value(a->f, a->r->key[i]);
        if (!pv) return false;
        t[n].var = a->r->tk[i] == JTERM_LEN ? jb_len(B, pv) : jb_val(B, pv); t[n].c = -a->r->coef[0][i]; n++;
    }
    return jb_cons(B, t, n, J_LE, rhs, goal);
}
// A measure of several components or a MAX or MIN of rows (c42) at a call of its own function:
// every call is FREE, owing the lexicographic decrease (jb_lex_vc, `rec.lex` q) from the caller's
// parameters at entry (pre) to the arguments (post), a MAX or MIN the exact fresh value of c41.
// A chain of activations is then a sequence of entry states, each step a call's relation, along
// which c35's argument holds as along a loop's trips.
// A call's terms: before, f's parameters as it was entered; after, the call's arguments for them.
static bool jb_call_terms(JB *B, IrInstr *at, const JRank *r, const IrFunc *f, int32_t *pre, int32_t *post) {
    int base = at->aux.callee ? 0 : 1;                          // op[0]: the pointer
    for (int32_t i = 0; i < r->nt; i++) {
        IrValue *pv = jg_param_value(f, r->key[i]);
        int pk = jg_param_at(f, r->key[i]);
        if (!pv || pk < 0 || base + pk >= at->n_operands) return false;   // as jgk_rec: fail closed
        IrValue *av = at->operands[base + pk];
        bool len = r->tk[i] == JTERM_LEN;
        pre[i] = len ? jb_len(B, pv) : jb_val(B, pv);
        post[i] = len ? jb_len(B, av) : jb_val(B, av);
    }
    return true;
}
// what: 0 `rec.lex` q over the whole tuple (no regions); in regions (c43, jg_region_vcs's at a call)
// 1 `rec.cover` a, 2 `rec.region` (k, l), 3 `rec.lex` q of region k's tuple under G_k before and after
typedef struct { const JRank *r; const IrFunc *f; int what; int32_t a, k, l, q; } JRecLexArg;
static bool jgk_rec_lex(JB *B, IrInstr *at, void *arg, JCons *goal) {
    JRecLexArg *x = arg; int32_t pre[JRANK_MAXT], post[JRANK_MAXT];
    const JRank *r = x->r;
    if (!jb_call_terms(B, at, r, x->f, pre, post)) return false;
    switch (x->what) {
        case 0: return jb_lex_vc(B, r, pre, post, 0, r->ncomp, x->q, goal);
        case 1: return jb_cover_vc(B, r, pre, x->a, goal);
        case 2: return jb_region_pair_vc(B, r, pre, post, x->k, x->l, goal);
        case 3:
            jb_region_premises(B, r, x->k, pre, false, 0); jb_region_premises(B, r, x->k, post, true, 0);
            return !B->fail && jb_lex_vc(B, r, pre, post, r->reg[x->k].t0, r->reg[x->k].nt, x->q, goal);
        default: return false;
    }
}
// The functions a call listed in u's measure reaches (u is function ku of the call graph): the
// instruction at (blk, idx) of u, a call into u's component, direct or through a pointer (every
// target there, at most cap). The count, or -1 for anything else.
enum { JREC_MAXT = 16 };
static int jg_rec_listed(const IrCallGraph *cg, int ku, const IrFunc *u, int32_t blk, int32_t idx, int *out, int cap) {
    for (IrBlock *b = u->blocks; b; b = b->next) {
        if (b->id != blk) continue;
        int32_t k = 0;
        for (IrInstr *i = b->instrs; i; i = i->next, k++) {
            if (k != idx) continue;
            if (i->op != IR_CALL) return -1;
            int nt = ir_cg_call_targets(cg, ku, i, true, out, cap);
            return nt >= 1 && nt <= cap ? nt : -1;
        }
        return -1;
    }
    return -1;
}
// Spec 12's cut, over function kf's whole component: the calls its measures list (`rec.le`) close
// no cycle, so every cycle falls at one call at least. Each listed call must be a call into the
// component, direct or through a pointer (an edge to each of its targets there); the graph they
// make is then reduced by its sources (Kahn), and a member left is on a cycle of listed calls: the
// claim is refused. Every member checks the whole component, so a bad list refuses them all. A
// self-call listed is a cycle of one.
static bool jg_rec_cut(JG *g, const JRank *rk, int32_t nrk, const IrCallGraph *cg, int kf) {
    int32_t nm = 0, ne = 0;
    int32_t *loc = JG_NEW(g, int32_t, cg->n);                 // call-graph index -> member, or -1
    if (!loc) return jg_fail(g, "out of memory");
    for (int j = 0; j < cg->n; j++) loc[j] = cg->comp[j] == cg->comp[kf] ? nm++ : -1;
    int32_t *off = JG_NEW(g, int32_t, nm + 1), *indeg = JG_NEW(g, int32_t, nm), *q = JG_NEW(g, int32_t, nm);
    if (!off || !indeg || !q) return jg_fail(g, "out of memory");
    for (int32_t c = 0; c <= nm; c++) off[c] = 0;
    for (int32_t k = 0; k < nrk; k++) {                        // how many calls each member lists
        if (rk[k].blk != JRANK_ENTRY || rk[k].nle == 0) continue;
        if (rk[k].nle < 0 || rk[k].nle > JRANK_MAXL) return jg_fail(g, "a recursion's measure with its listed calls out of range");
        int ku = ir_cg_index(cg, rk[k].fn);
        if (ku >= 0 && loc[ku] >= 0) { off[loc[ku] + 1] += rk[k].nle * JREC_MAXT; ne += rk[k].nle * JREC_MAXT; }
    }
    if (ne == 0) return true;
    int32_t *to = JG_NEW(g, int32_t, ne), *at = JG_NEW(g, int32_t, nm);
    if (!to || !at) return jg_fail(g, "out of memory");
    for (int32_t c = 0; c < nm; c++) { off[c + 1] += off[c]; at[c] = off[c]; indeg[c] = 0; }
    for (int32_t k = 0; k < nrk; k++) {
        if (rk[k].blk != JRANK_ENTRY || rk[k].nle == 0) continue;
        int ku = ir_cg_index(cg, rk[k].fn);
        if (ku < 0 || loc[ku] < 0) continue;
        for (int32_t m = 0; m < rk[k].nle; m++) {
            int tg[JREC_MAXT], nt = jg_rec_listed(cg, ku, rk[k].fn, rk[k].le_blk[m], rk[k].le_idx[m], tg, JREC_MAXT);
            if (nt < 0) return jg_fail(g, "a recursion's measure lists what is not a call that may close its cycle");
            for (int q2 = 0; q2 < nt; q2++) { to[at[loc[ku]]++] = loc[tg[q2]]; indeg[loc[tg[q2]]]++; }
        }
    }
    int32_t head = 0, tail = 0;
    for (int32_t c = 0; c < nm; c++) if (indeg[c] == 0) q[tail++] = c;
    while (head < tail) {
        int32_t c = q[head++];
        for (int32_t e = off[c]; e < at[c]; e++) if (--indeg[to[e]] == 0) q[tail++] = to[e];   // the edges written
    }
    if (tail < nm) return jg_fail(g, "the calls a recursion's measures list as not falling close a cycle (spec 12: every cycle falls at one call)");
    return true;
}
// Is the call at (blk, idx) one f's measure lists?
static bool jg_rec_is_listed(const JRank *r, int32_t blk, int32_t idx) {
    for (int32_t m = 0; m < r->nle; m++) if (r->le_blk[m] == blk && r->le_idx[m] == idx) return true;
    return false;
}
// A WRITTEN function measure (I.206), read from the ghost tree the lowering states over h's entry
// parameters (IrFunc.measure_val, a block outside the CFG): a linear form over h's integer
// parameters and its slice parameters' lengths, through constants, checked `+`, `-` (unary minus
// is 0 - x) and `*` by a constant. Each arithmetic node's form is kept with its type: the measure
// is WELL-DEFINED where every node is within its type's bits (a refinement is no part of what the
// arithmetic computes). Anything else, `/` and `%` among them, is no sum.
typedef struct { int32_t nt; int32_t key[JRANK_MAXT]; uint8_t tk[JRANK_MAXT]; __int128 coef[JRANK_MAXT], c0; const IrType *type; } JGForm;
enum { JGHOST_MAXN = 32 };
typedef struct { JGForm root; JGForm node[JGHOST_MAXN]; int nn; } JGhost;
static bool jgf_axpy(JGForm *r, const JGForm *a, __int128 k) {      // r += k * a, each number checked
    const __int128 big = (__int128)1 << 66;
    __int128 kc;
    if (!jg_mul_ok(k, a->c0, &kc)) return false;
    r->c0 += kc;
    if (r->c0 > big || r->c0 < -big) return false;
    for (int32_t i = 0; i < a->nt; i++) {
        int32_t j = 0;
        while (j < r->nt && !(r->key[j] == a->key[i] && r->tk[j] == a->tk[i])) j++;
        if (j == r->nt) {
            if (r->nt == JRANK_MAXT) return false;
            r->key[j] = a->key[i]; r->tk[j] = a->tk[i]; r->coef[j] = 0; r->nt++;
        }
        if (!jg_mul_ok(k, a->coef[i], &kc)) return false;
        r->coef[j] += kc;
        if (r->coef[j] > big || r->coef[j] < -big) return false;
    }
    return true;
}
static bool jg_ghost_form(const IrFunc *h, IrValue *v, JGhost *G, JGForm *out, int depth) {
    memset(out, 0, sizeof *out);
    if (!v || !v->type || depth > 64) return false;
    out->type = v->type;
    for (const IrParam *p = h->params; p; p = p->next)
        if (p->value == v) {
            if (v->type->kind != IRT_INT) return false;
            out->nt = 1; out->key[0] = v->id; out->tk[0] = JTERM_VALUE; out->coef[0] = 1;
            return true;
        }
    IrInstr *d = NULL;
    for (IrInstr *i = h->measure_blk ? h->measure_blk->instrs : NULL; i && !d; i = i->next) if (i->result == v) d = i;
    if (!d) return false;
    switch (d->op) {
        case IR_CONST:
            if (v->type->kind != IRT_INT) return false;
            out->c0 = jg_const(d);
            return true;
        case IR_SLICE_LEN: {                                   // a slice parameter's length
            IrValue *sv = d->n_operands ? d->operands[0] : NULL;
            for (const IrParam *p = h->params; p && sv; p = p->next)
                if (p->value == sv && sv->type && sv->type->kind == IRT_SLICE) {
                    out->nt = 1; out->key[0] = sv->id; out->tk[0] = JTERM_LEN; out->coef[0] = 1;
                    return true;
                }
            return false;
        }
        case IR_ADD: case IR_SUB: case IR_MUL: {
            if (d->wrap != IR_WRAP_CHECK || d->unchecked || d->n_operands < 2 || v->type->kind != IRT_INT ||
                v->type->bits < 1 || v->type->bits > 64) return false;
            JGForm a, b;
            if (!jg_ghost_form(h, d->operands[0], G, &a, depth + 1) || !jg_ghost_form(h, d->operands[1], G, &b, depth + 1))
                return false;
            if (d->op == IR_MUL) {                             // one factor constant, or no sum
                const JGForm *kf = a.nt == 0 ? &a : b.nt == 0 ? &b : NULL, *x = kf == &a ? &b : &a;
                if (!kf || !jgf_axpy(out, x, kf->c0)) return false;
            } else if (!jgf_axpy(out, &a, 1) || !jgf_axpy(out, &b, d->op == IR_ADD ? 1 : -1)) return false;
            if (G->nn == JGHOST_MAXN) return false;
            G->node[G->nn++] = *out;
            return true;
        }
        default: return false;
    }
}
// A written measure's one row (jg_ghost_form's root) as h's entry rank: the measure the calls owe,
// and the one the finder fixes a written member of a cycle to (jgo_measure), read once, here.
static void jg_written_rank(const IrFunc *h, const JGhost *gh, JRank *r) {
    memset(r, 0, sizeof *r);
    r->blk = JRANK_ENTRY; r->fn = (IrFunc *)h; r->ncomp = 1; r->nt = gh->root.nt; r->c0[0] = gh->root.c0;
    for (int32_t i = 0; i < r->nt; i++) { r->key[i] = gh->root.key[i]; r->tk[i] = gh->root.tk[i]; r->coef[0][i] = gh->root.coef[i]; }
}
// h's measure for its recursion: its WRITTEN one where it has one (a proposal's `le` list is read,
// its measure not: the finder fixes a written member's coefficients to it), else the proposed one.
// NULL with `why` where none can be stated (`outside` at the calls); false refuses the function.
static bool jg_rec_measure(JG *g, const JRank *rk, int32_t nrk, const IrFunc *h, const JRank **out, JGhost **G,
                           const char **why) {
    *out = NULL; *G = NULL; *why = NULL;
    if (!h->has_decreasing) {
        bool bad;
        *out = jg_entry_rank(g, rk, nrk, h, &bad);
        if (bad) return false;
        if (!*out) *why = "no measure proposed for this recursion";
        return true;
    }
    const JRank *pr = NULL;
    for (int32_t k = 0; k < nrk; k++) {
        if (rk[k].blk != JRANK_ENTRY || rk[k].fn != h) continue;
        if (pr) return jg_fail(g, "two measures for one function's recursion");
        pr = &rk[k];
    }
    JGhost *gh = JG_NEW(g, JGhost, 1);
    JRank *r = JG_NEW(g, JRank, 1);
    if (!gh || !r) return jg_fail(g, "out of memory");
    gh->nn = 0;
    if (!h->measure_val || !jg_ghost_form(h, h->measure_val, gh, &gh->root, 0)) {
        *why = "a written measure the generator cannot read as a sum";
        return true;
    }
    jg_written_rank(h, gh, r);
    // the list's count is jg_rec_cut's to refuse, over every entry rank of the module; copied bounded
    for (int32_t m = 0; pr && m < pr->nle && m < JRANK_MAXL; m++) { r->le_blk[m] = pr->le_blk[m]; r->le_idx[m] = pr->le_idx[m]; r->nle = m + 1; }
    *out = r; *G = gh;
    return true;
}
// A node of a written measure within its type's bits (`rec.def`): at h's entry values (side 0, h
// the caller) or at the call's arguments (side 1, h the callee); `hi` its upper end, else its lower.
typedef struct { const JGForm *n; const IrFunc *h; int side; bool hi; } JRecDef;
static bool jgk_recdef(JB *B, IrInstr *at, void *arg, JCons *goal) {
    JRecDef *a = arg; JLT t[JRANK_MAXT]; int n = 0;
    const IrType *ty = a->n->type;
    __int128 lo = ty->is_signed ? -((__int128)1 << (ty->bits - 1)) : 0;
    __int128 hi = ty->is_signed ? ((__int128)1 << (ty->bits - 1)) - 1 : ((__int128)1 << ty->bits) - 1;
    for (int32_t i = 0; i < a->n->nt; i++) {
        if (a->n->coef[i] == 0) continue;
        IrValue *v = NULL;
        if (a->side == 0) v = jg_param_value(a->h, a->n->key[i]);
        else {
            int pk = jg_param_at(a->h, a->n->key[i]), base = at->aux.callee ? 0 : 1;
            if (pk < 0 || base + pk >= at->n_operands) return false;
            v = at->operands[base + pk];
        }
        if (!v) return false;
        t[n].var = a->n->tk[i] == JTERM_LEN ? jb_len(B, v) : jb_val(B, v); t[n].c = a->hi ? a->n->coef[i] : -a->n->coef[i]; n++;
    }
    return jb_cons(B, t, n, J_LE, a->hi ? hi - a->n->c0 : a->n->c0 - lo, goal);   // form <= hi, form >= lo
}
static bool jg_rec_obligations(JG *g, JObls *L, const JRank *rk, int32_t nrk, const IrCallGraph *cg) {
    int kf = ir_cg_index(cg, g->f);
    if (kf < 0 || !ir_cg_on_cycle(cg, kf) || (g->f->may_diverge && !g->f->has_decreasing)) return true;
    const JRank *r; JGhost *gf; const char *whyf;
    if (!jg_rec_measure(g, rk, nrk, g->f, &r, &gf, &whyf) || !jg_rec_cut(g, rk, nrk, cg, kf)) return false;
    // v1 (c42): a measure of several components or a MAX or MIN of rows is a SELF-recursion's, its
    // component the function alone. It lists no call: every call it could list is a self-call,
    // which jg_rec_cut has refused (a cycle of one)
    int32_t nmem = 0;
    for (int j = 0; j < cg->n; j++) if (cg->comp[j] == cg->comp[kf]) nmem++;
    bool lexm = r && r->ncomp > 1;                            // regions (c43) have two rows at least
    if (lexm && nmem > 1) return jg_fail(g, "a recursion's measure of several rows in a mutual recursion (v1)");
    for (int32_t ri = 0; ri < g->nrpo; ri++) {
        IrBlock *b = g->blk[g->rpo[ri]];
        int32_t k = 0;
        for (IrInstr *i = b->instrs; i; i = i->next, k++) {
            if (i->op != IR_CALL) continue;
            // every function the call may reach in f's component: one, or through a pointer each
            // address-taken function of its arity there (callgraph.h), at most JREC_MAXT
            int tg[JREC_MAXT];
            int nt = ir_cg_call_targets(cg, kf, i, true, tg, JREC_MAXT);
            if (nt == 0) continue;
            if (nt > JREC_MAXT) {
                if (!jg_outside(g, L, i, b->id, k, "rec.fall", "a call through a pointer to too many functions of a recursion")) return false;
                continue;
            }
            bool le = r && jg_rec_is_listed(r, b->id, k);
            for (int tq = 0; tq < nt; tq++) {                     // per target: its measure at the arguments
                const IrFunc *t = cg->idx[tg[tq]];
                const JRank *rt = r; JGhost *gt = gf; const char *whyt = whyf;
                if (t != g->f && !jg_rec_measure(g, rk, nrk, t, &rt, &gt, &whyt)) return false;
                // unmeasured: the caller (once per call, whatever its targets) or this target (its
                // ordinal, as its rec.fall would have: one ID per obligation)
                if (!r || !rt) { if ((r || tq == 0) && !jg_outside_n(g, L, i, b->id, k, "rec.fall", r ? whyt : whyf, r ? tq : 0)) return false; continue; }
                if (rt->ncomp > 1 && t != g->f) return jg_fail(g, "a recursion's measure of several rows in a mutual recursion (v1)");
                if (lexm && !r->nreg) {                           // t is f: c35's VCs, n + 1 of them
                    int32_t s0[JRANK_MAXC], sn[JRANK_MAXC], cb, n = jg_lex_comps(r, 0, r->ncomp, s0, sn, &cb);
                    for (int32_t q = 0; q <= n; q++) {
                        JRecLexArg a = { r, g->f, 0, 0, 0, 0, q };
                        if (!jg_goal(g, L, i, b->id, k, -1, "rec.lex", tq * 100000 + q, jgk_rec_lex, &a)) return false;
                    }
                    continue;
                }
                if (lexm) {                                       // in regions (c43): c38's, at the call
                    int32_t m = r->nreg, ob = tq * 100000;
                    for (int32_t a = 0; a < r->reg[m - 1].ng; a++) {
                        JRecLexArg x = { r, g->f, 1, a, 0, 0, 0 };
                        if (!jg_goal(g, L, i, b->id, k, -1, "rec.cover", ob + a, jgk_rec_lex, &x)) return false;
                    }
                    for (int32_t k1 = 0; k1 < m; k1++)
                        for (int32_t l1 = k1 + 1; l1 < m; l1++) {
                            JRecLexArg x = { r, g->f, 2, 0, k1, l1, 0 };
                            if (!jg_goal(g, L, i, b->id, k, -1, "rec.region", ob + 100 * k1 + l1, jgk_rec_lex, &x)) return false;
                        }
                    for (int32_t k1 = 0; k1 < m; k1++) {
                        int32_t s0[JRANK_MAXC], sn[JRANK_MAXC], cb, n = jg_lex_comps(r, r->reg[k1].t0, r->reg[k1].nt, s0, sn, &cb);
                        for (int32_t q = 0; q <= n; q++) {
                            JRecLexArg x = { r, g->f, 3, 0, k1, 0, q };
                            if (!jg_goal(g, L, i, b->id, k, -1, "rec.lex", ob + 1000 * k1 + q, jgk_rec_lex, &x)) return false;
                        }
                    }
                    continue;
                }
                for (int w = 0; w < 2; w++) {                     // rec.bound is the caller's alone: once
                    if (w == 1 && tq > 0) continue;
                    JRecArg a = { r, rt, g->f, t, w == 1 ? 1 : le ? 2 : 0 };
                    if (!jg_goal(g, L, i, b->id, k, -1, w == 1 ? "rec.bound" : le ? "rec.le" : "rec.fall", w == 1 ? 0 : tq, jgk_rec, &a)) return false;
                }
                // a written measure's arithmetic, well-defined at the caller's entry (once) and at
                // the arguments (per target)
                for (int side = 0; side < 2; side++) {
                    if (side == 0 && tq > 0) continue;
                    JGhost *G = side ? gt : gf;
                    for (int q = 0; G && q < G->nn; q++)
                        for (int w = 0; w < 2; w++) {
                            JRecDef a = { &G->node[q], side ? t : g->f, side, w == 1 };
                            if (!jg_goal(g, L, i, b->id, k, -1, "rec.def", tq * 100000 + side * 1000 + q * 2 + w, jgk_recdef, &a)) return false;
                        }
                }
            }
        }
    }
    return true;
}

// Obligations by their ID's point, kind and ordinal (the function is one), for jg_ids_unique.
static int jg_id_cmp(const void *x, const void *y) {
    const JId *a = &(*(const JObl *const *)x)->vc.id, *b = &(*(const JObl *const *)y)->vc.id;
    int c = strcmp(a->point, b->point);
    if (!c) c = strcmp(a->kind, b->kind);
    return c ? c : strcmp(a->ordinal, b->ordinal);
}
// Every obligation of a function has its own ID: a record, a certificate and the shadow's matching
// all name an obligation by it, so two of one ID would let one's answer stand for the other's. The
// ordinal schemes make them distinct by construction; this checks it (a block closing two loops'
// back edges at once would give both loops' ranking VCs one point), sorting pointers so the
// obligations keep their order. Refused, never skipped.
static bool jg_ids_unique(JG *g, const JObls *L) {
    if (L->n < 2) return true;
    const JObl **p = JG_NEW(g, const JObl *, L->n);
    if (!p) return jg_fail(g, "out of memory");
    for (int32_t i = 0; i < L->n; i++) p[i] = &L->o[i];
    qsort(p, (size_t)L->n, sizeof *p, jg_id_cmp);
    for (int32_t i = 1; i < L->n; i++)
        if (!jg_id_cmp(&p[i - 1], &p[i])) return jg_fail(g, "two obligations of one ID (point, kind and ordinal)");
    return true;
}
// The obligations of `f`, their VCs in `m`: with `only` JGEN_ALL, every obligation and every
// claim's; JGEN_CLAIMS, the claims' only; JGEN_POST, f's return claims' only; a block id, the
// claims' at that block only. `rk` are the module's ranking claims, owed with the obligations (a
// loop's termination, and at JRANK_ENTRY a recursion's). `cg` is the module's call graph, built once
// by the caller (NULL: the generator builds its own, a cost per function). `cl` are the module's claims (untrusted: see the header): f's cut points and
// returns, and its same-module callees' returns. False with `err` on an IR the generator does not
// read (a phi), a claim it cannot state and owe, or a region that is full.
static bool jgen_func(JMem *m, IrFunc *f, IrFunc *mod, const JClaim *cl, int32_t ncl, const JGuardClaim *gc, int32_t ngc,
                      const JRank *rk, int32_t nrk, const IrCallGraph *cg, int32_t only, JObl **out, int32_t *nout,
                      char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.mod = mod; g.err = err; g.errcap = errcap; err[0] = 0;
    g.cl = cl; g.ncl = cl ? ncl : 0;
    g.gc = gc; g.ngc = gc ? ngc : 0;
    *out = NULL; *nout = 0;
    if (!f || !f->entry || f->is_extern) return g.ncl + g.ngc == 0 || jg_fail(&g, "a claim on a function without a body");
    if (!jg_structure(&g) || !jg_cells(&g) || !jg_claims_ok(&g)) return false;
    JObls L; memset(&L, 0, sizeof L);
    if (only == JGEN_ALL && !jg_obligations(&g, &L)) return false;
    if (!jg_claim_obligations(&g, &L, only)) return false;
    if (only == JGEN_ALL && !jg_rank_obligations(&g, &L, rk, rk ? nrk : 0)) return false;
    if (only == JGEN_ALL) {                                    // a recursion's, on the module's call graph
        IrCallGraph own; memset(&own, 0, sizeof own);
        if (!cg && !ir_cg_build(&own, mod ? mod : f)) return jg_fail(&g, "out of memory building the call graph");
        bool ok = jg_rec_obligations(&g, &L, rk, rk ? nrk : 0, cg ? cg : &own);
        ir_cg_free(&own);
        if (!ok) return false;
    }
    if (!jg_ids_unique(&g, &L)) return false;
    // a claim's VC names it by its index, a guarded one's by ncl + its own: a reader that indexes the
    // claims by it receives no index past them unless it passed guarded claims
    for (int32_t q = 0; q < L.n; q++)
        if (L.o[q].claim >= g.ncl + g.ngc) return jg_fail(&g, "an obligation naming a claim past the claims given");
    *out = L.o; *nout = L.n;
    return true;
}

// For whoever proposes claims (nothing here is trusted: every claim is checked and owed). The
// versions a claim can be about: every forward join's fresh version of an integer cell, and every
// loop header's (`head`), in reverse post-order, so a block's forward predecessors come first.
// (JSlot is format.h's.)
static bool jgen_slots(JMem *m, IrFunc *f, IrFunc *mod, JSlot **out, int32_t *nout, char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.mod = mod; g.err = err; g.errcap = errcap; err[0] = 0;
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
                if (g.csyn[c]) continue;                       // no address: no key (an EACH term names it)
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
static bool jgen_terms(JMem *m, IrFunc *f, IrFunc *mod, int32_t blk, JSlot **out, int32_t *nout, char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.mod = mod; g.err = err; g.errcap = errcap; err[0] = 0;
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
        if (!jg_term_ok(&g, blk, id, JTERM_VALUE)) continue;   // an unnamed element's key (its root) is no term
        K[n].blk = blk; K[n].key = id; K[n].type = g.cty[c]; K[n].head = false; n++; seen[id] = 1;
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
                if (!jg_term_ok(&g, blk, o->id, JTERM_VALUE)) continue;
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
static bool jgen_term_kinds(JMem *m, IrFunc *f, IrFunc *mod, int32_t blk, const int32_t *keys, const uint8_t *tk, int32_t n, uint8_t *kind,
                            char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.mod = mod; g.err = err; g.errcap = errcap; err[0] = 0;
    for (int32_t i = 0; i < n; i++) kind[i] = 0;
    if (!f || !f->entry || f->is_extern) return true;
    if (!jg_structure(&g) || !jg_cells(&g)) return false;
    if (blk < 0 || blk >= g.nb || !g.blk[blk]) return true;
    for (int32_t i = 0; i < n; i++) {
        if (tk && tk[i] == JTERM_EACH) {                       // 1, or 2 where an element is made at blk
            int32_t ec[JELEM_MAXN], en = jg_each_cells(&g, keys[i], ec);
            if (en) kind[i] = 1;
            for (int32_t q = 0; q < en; q++) if (ec[q] >= 0 && jg_made_at(&g, blk, ec[q])) kind[i] = 2;
            continue;
        }
        if (tk && tk[i] == JTERM_ENTRY) { if (jg_entry_ok(&g, blk, keys[i])) kind[i] = 1; continue; }   // never made
        if (tk && tk[i] == JTERM_HEAD) { if (jg_head_ok(&g, blk, keys[i])) kind[i] = 1; continue; }     // never made
        if (!jg_term_ok(&g, blk, keys[i], tk && tk[i] != JTERM_UPTO ? tk[i] : JTERM_VALUE)) continue;
        kind[i] = 1;
        if (g.cell[keys[i]] < 0) continue;
        int32_t jv = g.instate[(size_t)blk * (size_t)g.ncells + (size_t)g.cell[keys[i]]];
        if (jv >= 0 && (g.ver[jv].kind == JV_JOIN || g.ver[jv].kind == JV_HEAD) && g.ver[jv].blk == blk) kind[i] = 2;
    }
    return true;
}

// Is cell c's version v, the one header H makes (JV_HEAD), LIVE in H's loop: does any trip use the
// value c had as it began (c45)? Live where (1) a load in the loop reads v; (2) a block of the loop
// but H joins v in from a predecessor in the loop; (3) the loop havocs c (a call may read it through
// its address); (4) a claim of the function names c beside another term (it may tie v to the rest).
// Dead otherwise: every path stores c before it reads it, and v's variable on a back edge is free in
// its type's box (narrowed by interval claims), so no ranking or relation over the other pairs gains
// from it (a linear one c*v + g + c0 ranks the paths only if g does with its constant moved: on each
// path the box end where c*(v' - v) >= 0 exists, v' being in the type too).
static bool jg_head_live(JG *g, int32_t H, int32_t c, int32_t v) {
    const uint8_t *Lh = g->inloop + (size_t)H * (size_t)g->nb;
    for (int32_t b = 0; b < g->nb; b++) {
        if (!Lh[b] || !g->blk[b]) continue;
        for (IrInstr *i = g->blk[b]->instrs; i; i = i->next)                          // (1)
            if (i->result && i->result->id >= 0 && i->result->id < g->nv && g->loadver[i->result->id] == v) return true;
        if (b != H && g->instate[(size_t)b * (size_t)g->ncells + (size_t)c] != v)       // (2)
            for (IrEdge *e = g->blk[b]->preds; e; e = e->next)
                if (Lh[e->block->id] && g->rpoix[e->block->id] >= 0 && g->outstate[(size_t)e->block->id * (size_t)g->ncells + (size_t)c] == v) return true;
    }
    for (int32_t j = 0; j < g->nver; j++)                                               // (3)
        if (g->ver[j].kind == JV_HAVOC && g->ver[j].cell == c && g->ver[j].blk >= 0 && Lh[g->ver[j].blk]) return true;
    int32_t key = g->cellv[c]->id;
    for (int32_t ci = 0; ci < g->ncl; ci++) {                                           // (4)
        const JClaim *cl = &g->cl[ci];
        if (!jg_mine(g, cl) || cl->nt < 2) continue;                                // a cut point's, not a return's
        for (int32_t k = 0; k < cl->nt; k++) if (cl->key[k] == key) return true;
    }
    return false;
}
// For whoever proposes ranking functions and loop relations (untrusted; PE.5, PE.7): every edge
// into loop header `header` as the generator states it with the claims `cl` (on a back edge the
// header's claims about its previous entry are facts, as for `inv.keep`), and for each version the
// header makes, its variable at the previous entry (-1 on an edge from before the loop) and the one
// the edge ends with. The goal of each path's VC is 0 <= 0 and means nothing. `refused` says why a
// header has no paths; false with `err` when the generator refuses the function.
static bool jgen_paths(JMem *m, IrFunc *f, IrFunc *mod, const JClaim *cl, int32_t ncl, int32_t header, JPaths *out,
                       char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.mod = mod; g.err = err; g.errcap = errcap; err[0] = 0;
    g.cl = cl; g.ncl = cl ? ncl : 0;
    memset(out, 0, sizeof *out);
    if (!f || !f->entry || f->is_extern) { out->refused = "a function without a body"; return true; }
    if (!jg_structure(&g) || !jg_cells(&g) || !jg_claims_ok(&g)) return false;
    if (!jg_loop_header(&g, header)) { out->refused = "not a loop header"; return true; }
    IrBlock *H = g.blk[header];
    int32_t nk = 0, *keys = JG_NEW(&g, int32_t, g.ncells ? 2 * g.ncells : 1);
    uint8_t *tks = JG_NEW(&g, uint8_t, g.ncells ? 2 * g.ncells : 1);
    int32_t np = 0;
    for (IrEdge *e = H->preds; e; e = e->next) np++;
    JPath *P = JG_NEW(&g, JPath, np ? np : 1);
    if (!keys || !tks || !P) return jg_fail(&g, "out of memory");
    // the header's versions: of every cell the loop writes (its JV_HEAD) whose value a trip uses
    // (jg_head_live: not a temporary every path stores before it reads, c45), and of every cell its
    // blocks only read, whose one version holds on every trip (`while b.len < b.cap`, cap never
    // written): its pre and post on a back edge are that version's one variable
    const uint8_t *Lh = g.inloop + (size_t)header * (size_t)g.nb;
    for (int32_t c = 0; c < g.ncells; c++) {
        int32_t jv = g.instate[(size_t)header * (size_t)g.ncells + (size_t)c];
        if (jv < 0 || g.csyn[c]) continue;
        bool head = g.ver[jv].blk == header && g.ver[jv].kind == JV_HEAD, read = false;
        for (int32_t b = 0; b < g.nb && !head && !read; b++) {
            if (!Lh[b] || !g.blk[b]) continue;                 // a loop's blocks are reachable ones
            for (IrInstr *i = g.blk[b]->instrs; i && !read; i = i->next)
                read = i->result && g.loadver[i->result->id] >= 0 && g.ver[g.loadver[i->result->id]].cell == c;
        }
        if (!head && !read) continue;
        if (head && !jg_head_live(&g, header, c, jv)) continue;   // dead: no trip uses it, no pair (c45)
        const IrType *st = g.ver[jv].slot;
        if (jg_is_int(st)) { tks[nk] = JTERM_VALUE; keys[nk++] = g.cellv[c]->id; }
        else if (st && st->kind == IRT_SLICE) { tks[nk] = JTERM_LEN; keys[nk++] = g.cellv[c]->id; }   // its length
    }
    // and each integer cell the loop WRITES at its ENTRY value (a header of one entry edge): fixed in
    // the loop, its pre and post one variable on a back edge, what an equality relative to the
    // entry (Karr) is about; a cell only read is its header version already
    for (int32_t q = 0, n0 = nk; q < n0; q++) {
        int32_t c = g.cell[keys[q]], jv = c >= 0 ? g.instate[(size_t)header * (size_t)g.ncells + (size_t)c] : -1;
        if (tks[q] != JTERM_VALUE || jv < 0 || g.ver[jv].blk != header || g.ver[jv].kind != JV_HEAD || !jg_entry_ok(&g, header, keys[q])) continue;
        tks[nk] = JTERM_ENTRY; keys[nk++] = keys[q];
    }
    int32_t n = 0;
    for (IrEdge *e = H->preds; e; e = e->next) {
        IrBlock *Pb = e->block;
        if (g.rpoix[Pb->id] < 0) continue;
        bool again = false;
        for (IrEdge *e2 = H->preds; e2 != e; e2 = e2->next) if (e2->block == Pb) again = true;
        if (again) continue;
        bool back = jg_back_edge(&g, Pb->id, header);
        int32_t ni = 0;
        for (IrInstr *i = Pb->instrs; i; i = i->next) ni++;
        JB B = jb_begin(&g, Pb->id, ni, -1);
        B.to = header;
        JPathPair *pp = JG_NEW(&g, JPathPair, nk ? nk : 1);
        if (B.fail || !pp) return jg_fail(&g, "out of memory");
        for (int32_t k = 0; k < nk; k++) {
            pp[k].key = keys[k];
            pp[k].post = jb_cterm(&B, keys[k], tks[k], g.outstate + (size_t)Pb->id * (size_t)g.ncells, header);
            pp[k].pre = back ? jb_cterm(&B, keys[k], tks[k], g.instate + (size_t)header * (size_t)g.ncells, header) : -1;
            pp[k].tk = tks[k];
        }
        JLT none[1]; JCons goal; JObl o;
        JVc *vc = JG_NEW(&g, JVc, 1);
        if (B.fail || !vc || !jb_cons(&B, none, 0, J_LE, 0, &goal) || !jb_finish(&B, NULL, "path", 0, &goal, &o))
            return jg_fail(&g, "out of memory building a path");
        *vc = o.vc;
        P[n].vc = vc; P[n].back = back; P[n].from = Pb->id; P[n].pairs = pp; P[n].npairs = nk;
        P[n].at = -1; P[n].callee = NULL; P[n].entry = NULL; P[n].nentry = 0; n++;
    }
    out->p = P; out->n = n;
    return true;
}

// For whoever proposes a recursion's measure (untrusted; the finder): f's DIRECT calls that may
// close a cycle through it, each as the facts at the call (the goal 0 <= 0, meaning nothing) with,
// per integer or slice parameter of the callee, its variable at f's entry (f being the callee;
// else -1) and the argument's at the call, and per one of f's, its variable at f's entry (`entry`).
// `from` and `at` are the call's block and instruction index, `callee` the function it reaches; a
// call through a pointer gives one path per function it may reach in f's component.
static bool jgen_call_paths(JMem *m, IrFunc *f, IrFunc *mod, const JClaim *cl, int32_t ncl, JPaths *out,
                            char *err, size_t errcap) {
    JG g; memset(&g, 0, sizeof g);
    g.m = m; g.f = f; g.mod = mod; g.err = err; g.errcap = errcap; err[0] = 0;
    g.cl = cl; g.ncl = cl ? ncl : 0;
    memset(out, 0, sizeof *out);
    if (!f || !f->entry || f->is_extern) { out->refused = "a function without a body"; return true; }
    if (!jg_structure(&g) || !jg_cells(&g) || !jg_claims_ok(&g)) return false;
    IrCallGraph cg; memset(&cg, 0, sizeof cg);
    if (!ir_cg_build(&cg, mod ? mod : f)) return jg_fail(&g, "out of memory building the call graph");
    int kf = ir_cg_index(&cg, f);
    if (kf < 0 || !ir_cg_on_cycle(&cg, kf)) { ir_cg_free(&cg); out->refused = "not on a recursion cycle"; return true; }
    int32_t cap = 0;
    for (IrBlock *b = f->blocks; b; b = b->next) for (IrInstr *i = b->instrs; i; i = i->next) if (i->op == IR_CALL) cap += JREC_MAXT;
    JPath *P = JG_NEW(&g, JPath, cap ? cap : 1);
    if (!P) { ir_cg_free(&cg); return jg_fail(&g, "out of memory"); }
    int32_t n = 0;
    for (int32_t ri = 0; ri < g.nrpo; ri++) {
        IrBlock *b = g.blk[g.rpo[ri]];
        int32_t k = 0;
        for (IrInstr *i = b->instrs; i; i = i->next, k++) {
            int tg[JREC_MAXT], nt = i->op == IR_CALL ? ir_cg_call_targets(&cg, kf, i, true, tg, JREC_MAXT) : 0;
            int base = i->op == IR_CALL && !i->aux.callee ? 1 : 0;   // a pointer's call: op[0] the pointer
            if (nt > JREC_MAXT) nt = 0;                         // owed `outside`: no path
            for (int tq = 0; tq < nt; tq++) {
                const IrFunc *t = cg.idx[tg[tq]];
                int32_t np = 0, nf = 0;
                for (IrParam *p = t->params; p; p = p->next) np++;
                for (IrParam *p = f->params; p; p = p->next) nf++;
                JPathPair *pp = JG_NEW(&g, JPathPair, np ? np : 1), *pe = JG_NEW(&g, JPathPair, nf ? nf : 1);
                JB B = jb_begin(&g, b->id, k, -1);
                if (!pp || !pe || B.fail) { ir_cg_free(&cg); return jg_fail(&g, "out of memory"); }
                int32_t q = 0, pos = 0;
                for (IrParam *p = t->params; p; p = p->next, pos++) {
                    if (!p->value || base + pos >= i->n_operands) continue;
                    uint8_t tk = jg_is_int(p->value->type) ? JTERM_VALUE : p->value->type && p->value->type->kind == IRT_SLICE ? JTERM_LEN : 2;
                    if (tk > JTERM_LEN) continue;
                    IrValue *av = i->operands[base + pos];
                    pp[q].key = p->value->id; pp[q].tk = tk;
                    pp[q].post = tk == JTERM_LEN ? jb_len(&B, av) : jb_val(&B, av);
                    pp[q].pre = t == f ? (tk == JTERM_LEN ? jb_len(&B, p->value) : jb_val(&B, p->value)) : -1;
                    q++;
                }
                int32_t qe = 0;                                     // the caller's own, at its entry
                for (IrParam *p = f->params; p; p = p->next) {
                    if (!p->value) continue;
                    uint8_t tk = jg_is_int(p->value->type) ? JTERM_VALUE : p->value->type && p->value->type->kind == IRT_SLICE ? JTERM_LEN : 2;
                    if (tk > JTERM_LEN) continue;
                    pe[qe].key = p->value->id; pe[qe].tk = tk; pe[qe].post = -1;
                    pe[qe].pre = tk == JTERM_LEN ? jb_len(&B, p->value) : jb_val(&B, p->value);
                    qe++;
                }
                JLT none[1]; JCons goal; JObl o;
                JVc *vc = JG_NEW(&g, JVc, 1);
                if (B.fail || !vc || !jb_cons(&B, none, 0, J_LE, 0, &goal) || !jb_finish(&B, i, "call", 0, &goal, &o)) {
                    ir_cg_free(&cg); return jg_fail(&g, "out of memory building a call's path");
                }
                *vc = o.vc;
                P[n].vc = vc; P[n].back = true; P[n].from = b->id; P[n].pairs = pp; P[n].npairs = q;
                P[n].at = k; P[n].callee = t; P[n].entry = pe; P[n].nentry = qe; n++;
            }
        }
    }
    ir_cg_free(&cg);
    out->p = P; out->n = n;
    return true;
}

// ── the generator offered to an UNTRUSTED proposer (format.h's JudgeGen) ─────────────────────
// Each call works in the caller's region from its start, so an answer lives until the next call.
typedef struct { char *buf; size_t cap; IrFunc *mod; } JGenOffer;
static bool jgo_claim_vcs(void *ctx, struct IrFunc *f, const JClaim *cl, int32_t ncl, const JGuardClaim *gc, int32_t ngc,
                          int32_t only, JClaimVc **out, int32_t *nout, char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    JObl *o; int32_t n;
    if (!jgen_func(&m, f, x->mod, cl, ncl, gc, ngc, NULL, 0, NULL, only, &o, &n, err, errcap)) return false;
    JClaimVc *v = JMEM_NEW(&m, JClaimVc, n ? n : 1);
    if (!v) { snprintf(err, errcap, "out of memory"); return false; }
    for (int32_t i = 0; i < n; i++) { v[i].vc = o[i].vc; v[i].claim = o[i].claim; }
    *out = v; *nout = n;
    return true;
}
static bool jgo_slots(void *ctx, struct IrFunc *f, JSlot **out, int32_t *nout, char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    return jgen_slots(&m, f, x->mod, out, nout, err, errcap);
}
static bool jgo_terms(void *ctx, struct IrFunc *f, int32_t blk, JSlot **out, int32_t *nout, char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    return jgen_terms(&m, f, x->mod, blk, out, nout, err, errcap);
}
static bool jgo_term_kinds(void *ctx, struct IrFunc *f, int32_t blk, const int32_t *keys, const uint8_t *tk, int32_t n, uint8_t *kind,
                           char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    return jgen_term_kinds(&m, f, x->mod, blk, keys, tk, n, kind, err, errcap);
}
static bool jgo_paths(void *ctx, struct IrFunc *f, const JClaim *cl, int32_t ncl, int32_t header, JPaths *out,
                      char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    return jgen_paths(&m, f, x->mod, cl, ncl, header, out, err, errcap);
}
static bool jgo_call_paths(void *ctx, struct IrFunc *f, const JClaim *cl, int32_t ncl, JPaths *out, char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    return jgen_call_paths(&m, f, x->mod, cl, ncl, out, err, errcap);
}
static bool jgo_measure(void *ctx, struct IrFunc *f, JRank *out, char *err, uint32_t errcap) {
    JGenOffer *x = ctx; JMem m = jmem_over(x->buf, x->cap);
    JGhost *gh = JMEM_NEW(&m, JGhost, 1);
    if (!gh) { snprintf(err, errcap, "out of memory"); return false; }
    gh->nn = 0;
    if (!f->has_decreasing || !f->measure_val || !jg_ghost_form(f, f->measure_val, gh, &gh->root, 0)) {
        snprintf(err, errcap, "no written measure the generator reads as a sum");
        return false;
    }
    jg_written_rank(f, gh, out);
    return true;
}
static JudgeGen jgen_offer(JGenOffer *x) {
    JudgeGen g; g.ctx = x; g.claim_vcs = jgo_claim_vcs; g.slots = jgo_slots; g.terms = jgo_terms; g.term_kinds = jgo_term_kinds;
    g.paths = jgo_paths; g.call_paths = jgo_call_paths; g.measure = jgo_measure;
    return g;
}

#endif // LAIN_JUDGE_GEN_H
