#ifndef LAIN_JUDGE_FORMAT_H
#define LAIN_JUDGE_FORMAT_H
// format.h — the judge's interfaces as plain data (local/internal/design/judge_interfaces.md):
//   - the VC: the trusted generator writes it, the untrusted solver reads it;
//   - the solver's answer: a certificate (which the trusted kernel checks), a model (which the
//     judge evaluates before showing it), or an exhausted budget.
//
// ★ THE ONE FILE THE TWO SIDES SHARE. src/solver/ includes this file and nothing else from
// src/judge/, and src/judge/ includes nothing from src/solver/ or src/analysis/
// (run_ir_tests.sh checks both). So it holds type definitions only: no functions and no
// arithmetic. The text form, its printer and its parser are the judge's (text.h); the
// kernel's arithmetic and the solver's are their own.
//
// Numbering is the text form's: hypotheses are h1..h<nhyps> by position; derived facts f<k> and
// branch facts s<k> carry their numbers, which the kernel checks.
//
// One form only. A sum's terms are strictly ascending by variable, with nonzero coefficients, and
// a number is in canonical JInt form. The kernel REFUSES a structure that is not; it never sorts,
// merges or normalises, so the canonical form stays the only form. The solver sorts and merges
// before it emits.
#include <stdint.h>
#include <stdbool.h>

// A signed integer of at most J_LIMBS 32-bit limbs (512 bits): magnitude little-endian in
// limb[0..n), with limb[n-1] != 0. Zero is n == 0, neg == 0.
#define J_LIMBS 16
typedef struct { uint8_t neg, n; uint32_t limb[J_LIMBS]; } JInt;

// The obligation ID (judge_interfaces §1): `kind` is bounds.lo, bounds.hi, narrow.lo, … .
typedef struct { const char *fn, *irhash, *point, *kind, *ordinal; } JId;

typedef struct { int32_t var; JInt coef; } JTerm;                 // coef * vars[var], coef != 0
typedef enum { J_LE, J_EQ, J_NE } JOp;                            // J_NE only in a goal
typedef struct { JTerm *t; int32_t nt; JOp op; JInt rhs; } JCons; // Σ t op rhs; vars ascending
typedef struct { JCons *c; int32_t nc; } JConj;                   // a disjunct: c[0] and c[1] …
typedef struct {
    bool disj;                  // false: `cons`; true: `dis[0] or dis[1] or …` (ndis >= 2)
    JCons cons;
    JConj *dis; int32_t ndis;
    const char *origin;         // `guard bb1->bb2`, `type %v3 u8`, `lemma L4 atom.and(%v2,%v9)`, …
} JHyp;
typedef struct {
    JId id;
    const char **vars; int32_t nvars;   // names only (A3); a bound is a hypothesis
    JHyp *hyps; int32_t nhyps;          // h1 .. h<nhyps>
    JCons goal;                         // op J_LE or J_NE (an equality goal is two VCs)
} JVc;

// A certificate refutes `hyps and g`, where g is the goal's negation (judge_interfaces §2).
typedef enum { J_REF_H, J_REF_G, J_REF_F, J_REF_S, J_REF_SC } JRefKind;  // h<a> g f<a> s<a> s<a>.<b>
typedef struct { JRefKind k; int32_t a, b; } JRef;
typedef struct { JInt m; JRef r; } JMul;                          // m * r
typedef enum { J_LIN, J_RND, J_SPLIT, J_CASE, J_CLOSE } JStepKind;
typedef struct JStep JStep;
typedef struct { JStep *s; int32_t n; } JSteps;   // a branch: ends in its one terminal step
struct JStep {
    JStepKind k;
    int32_t f;                  // LIN, RND: the fact f<f> defined; CLOSE: the fact closed (an f
                                // only: a contradictory hypothesis costs `f1 = lin 1*h3`, then
                                // `close f1`; there is no `close h`)
    JMul *mul; int32_t nmul;    // LIN
    int32_t of;                 // RND: f<of>
    int32_t s;                  // SPLIT, CASE: the branch fact s<s>
    JCons split;                // SPLIT: in branch `le` s<s> is split, in `gt` its negation
    int32_t hyp;                // CASE: the disjunctive hypothesis h<hyp>
    JSteps *br; int32_t nbr;    // SPLIT: br[0] le, br[1] gt; CASE: br[n-1] for disjunct n
};
typedef struct { JId id; JSteps body; } JCert;    // version 1

// A model: one value per VC variable, val[i] for vars[i]. It satisfies the hypotheses and g
// (the judge checks that), and claims nothing about reachability (G7).
typedef struct { JId id; JInt *val; int32_t n; } JModel;

// The solver's work, in the three counters the budget limits (judge_interfaces §3).
typedef struct { uint64_t pivot, node, leaf; } JWork;
// J_ANS_CAPACITY: the solver found no certificate it can state in 512-bit numbers (an unwound
// multiplier is wider). It is not a budget answer with every counter under its limit: the record
// says `exhausted=capacity`, so a reader is not left with a refusal no counter explains.
typedef enum { J_ANS_CERT, J_ANS_MODEL, J_ANS_BUDGET, J_ANS_CAPACITY } JAnswerKind;
typedef struct {
    JAnswerKind k;
    JCert cert;                 // J_ANS_CERT
    JModel model;               // J_ANS_MODEL
    JWork used;                 // every answer; for J_ANS_BUDGET, the counter at its limit says which
} JAnswer;

// ── claims: what an untrusted PROPOSER hands the judge, and what the judge offers it ──────────
// (judge_interfaces §8; the proposer is src/solver/finder.h). The IR's types are opaque here.
struct IrFunc; struct IrType;
// A claim at a cut point (blk >= 0: a forward join or a loop header), `sum(coef * term) <= rhs`
// over one to JCLAIM_MAXT terms by value id (a cell's ALLOCA id is its version at the block); or a
// RETURN claim (blk JCLAIM_RET) over the value returned (JKEY_RET) and integer parameters. `fn` is
// the claim's function. A term's kind `tk` is JTERM_VALUE (the value, or the cell's version),
// JTERM_LEN (the length of that slice value, or of the slice cell's version), or, at a cut point,
// JTERM_EACH: EACH element cell of the fixed array at address `key` (at most one such term a
// claim): the claim is one instance per element cell k, each about that element's version. With
// it, at most one JTERM_UPTO term g (a value term, coefficient 1, not in the sum) GUARDS each
// instance: it holds where g <= k or the sum holds, a prefix `for each k < g`. gen.h states and
// owes them; nothing here is believed.
enum { JCLAIM_RET = -1, JKEY_RET = -2, JCLAIM_MAXT = 4 };
enum { JTERM_VALUE = 0, JTERM_LEN = 1, JTERM_EACH = 2, JTERM_UPTO = 3 };
typedef struct { int32_t blk, nt; int32_t key[JCLAIM_MAXT]; uint8_t tk[JCLAIM_MAXT]; __int128 coef[JCLAIM_MAXT]; __int128 rhs;
                 struct IrFunc *fn; } JClaim;
// A RANKING claim at a loop header: a tuple of components f_j = sum coef[j][i] * term_i + c0[j]
// over terms by value id (a cell is its version at the header; any other value dominates it, so
// its value before and after a trip is one). On each back edge, named by its predecessor `from`,
// the components before fall[e] do not rise and component fall[e] falls by at least 1 while it is
// non-negative (lexicographic, ADFG); fall[e] -1, a FREE edge, owes that some component does so,
// which may differ from trip to trip (`rank.lex`). A component may be the MAX of several rows: a
// row j with cont[j] 1 joins the rows before it (cont[0] is 0), and fall[e] counts components, not
// rows; all cont 0 is one row per component. It is never a hypothesis: gen.h owes it on every
// back edge (`rank.keep`, `rank.fall`, `rank.bound`), and a loop of a function that must terminate
// with none is `outside`. A back edge it does not list refuses it.
// At blk JRANK_ENTRY it is a function's measure for its recursion: one component over its integer
// parameters (or their slices' lengths) at entry, owed at every call that may close a cycle
// through it (`rec.fall`, `rec.bound`); its edges are not read. The calls `le` lists, each by its
// block and instruction index, owe only that the callee's measure not rise (`rec.le`); over the
// whole component the calls listed must close no cycle (spec 12), or the claim is refused. A loop's
// ranking claim lists none.
enum { JRANK_MAXC = 8, JRANK_MAXT = 8, JRANK_MAXE = 8, JRANK_MAXL = 16, JRANK_ENTRY = -1 };
typedef struct {
    int32_t blk, ncomp, nt, nedge;
    int32_t key[JRANK_MAXT]; uint8_t tk[JRANK_MAXT];             // as a claim's terms
    __int128 coef[JRANK_MAXC][JRANK_MAXT], c0[JRANK_MAXC];
    int32_t from[JRANK_MAXE]; int8_t fall[JRANK_MAXE];
    uint8_t cont[JRANK_MAXC];                                    // row j continues row j - 1's MAX
    int32_t nle, le_blk[JRANK_MAXL], le_idx[JRANK_MAXL];      // at JRANK_ENTRY: the calls owed `<=`
    struct IrFunc *fn;
} JRank;
// A version a claim can be about: a join's or a loop header's (`head`) fresh version of a cell.
typedef struct { int32_t blk, key; const struct IrType *type; bool head; } JSlot;
// A VC a claim owes, and which claim (an index into the set it was generated with).
typedef struct { JVc vc; int32_t claim; } JClaimVc;
// One edge into a loop header as the generator states it (its VC's hypotheses; the goal means
// nothing), and the header's versions, of the cells the loop writes and of those it only reads:
// for each, `pre` is its variable at the header's previous entry (-1 on an edge from before the
// loop) and `post` the one the edge ends with, both in THIS path's variables (one variable, for a
// cell only read). `key` is the version's cell, so one key names one quantity on every path; `tk`
// JTERM_LEN for a slice cell, whose quantity is its length.
// A CALL's path (call_paths) is the facts at a call: `from` and `at` its block and instruction
// index (as a recursion measure lists a call), `callee` the function it reaches, `pairs` the
// callee's parameters (pre: at the caller's entry when the call is a self-call, else -1; post: the
// argument), and `entry` the caller's own parameters at its entry (pre; post -1). A loop's path has
// at -1, callee NULL and no entry.
typedef struct { int32_t key, pre, post; uint8_t tk; } JPathPair;
typedef struct {
    const JVc *vc; bool back; int32_t from; const JPathPair *pairs; int32_t npairs;
    int32_t at; const struct IrFunc *callee; const JPathPair *entry; int32_t nentry;
} JPath;
typedef struct { const JPath *p; int32_t n; const char *refused; } JPaths;
// What a claim VC generation covers: every claim's VCs and every obligation (ALL), the claims'
// only (CLAIMS), the function's return claims' only (POST), or a block id's claims only.
enum { JGEN_ALL = -1, JGEN_CLAIMS = -2, JGEN_POST = -3 };
// The judge's generator, offered to the proposer as calls (gen.h's jgen_offer). Each answer lives
// in the judge's region until the next call. False with `err` when the generator refuses.
typedef struct {
    void *ctx;
    bool (*claim_vcs)(void *ctx, struct IrFunc *f, const JClaim *cl, int32_t ncl, int32_t only,
                      JClaimVc **out, int32_t *nout, char *err, uint32_t errcap);
    bool (*slots)(void *ctx, struct IrFunc *f, JSlot **out, int32_t *nout, char *err, uint32_t errcap);
    bool (*terms)(void *ctx, struct IrFunc *f, int32_t blk, JSlot **out, int32_t *nout, char *err, uint32_t errcap);
    // tk: each key's kind (JTERM_VALUE, JTERM_LEN), or NULL when every key is a value
    bool (*term_kinds)(void *ctx, struct IrFunc *f, int32_t blk, const int32_t *keys, const uint8_t *tk, int32_t n, uint8_t *kind,
                       char *err, uint32_t errcap);
    // the edges into loop header `header`, with the claims `cl` as the generator states them
    bool (*paths)(void *ctx, struct IrFunc *f, const JClaim *cl, int32_t ncl, int32_t header, JPaths *out,
                  char *err, uint32_t errcap);
    // f's calls that may close a recursion cycle through it: per direct one, the facts at the call,
    // and per parameter of the callee its variable at f's entry (when f is the callee) and the
    // argument's at the call
    bool (*call_paths)(void *ctx, struct IrFunc *f, const JClaim *cl, int32_t ncl, JPaths *out, char *err, uint32_t errcap);
} JudgeGen;

#endif // LAIN_JUDGE_FORMAT_H
