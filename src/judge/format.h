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

#endif // LAIN_JUDGE_FORMAT_H
