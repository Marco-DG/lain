# Lain Compiler — Target Architecture (clean core)

*Phase 0.1 of the clean-core rebuild (see `REBUILD.md`). This is the engineering
blueprint: the pipeline, the module boundaries, and the coexistence strategy. The IR
is specified in `design/ir.md` (0.2); the abstract interpreter in the VRA design annex
(0.3).*

---

## 1. Why

The language is settled and the frontend is sound. The problem is exclusively the
**middle-end**: `src/sema/` analyses (VRA, borrow, linearity, effects, termination) run
**directly over the AST**, key facts **by identifier name** (string compare), execute in
**one top-down pass with no control-flow graph**, and recover precision through **~70
hand-coded syntactic recognizers**. This is sound (fuzz-hardened) but it does not
compound: every new real-world idiom costs another recognizer, and the design cannot
express *relational* facts (`i ≤ j`, `o = i+j`, `i*w+j < h*w`) that are the actual
frontier of Lain's proof power.

The rebuild replaces the middle-end with **a typed SSA/CFG intermediate representation**
and **analyses expressed as proper dataflow passes over it** — chiefly a **relational
abstract interpreter (octagons)** for the VRA. Everything else in this document exists to
make that change *safe*: it is done in place, beside the old engine, and proven correct by
the existing corpus before the old engine is retired.

## 2. Principles

1. **Proof is the product.** The IR and analyses are designed for *provability first* —
   value identity, explicit control flow, a relational numeric domain — because that is
   what licenses Lain's safety guarantees and check-free codegen.
2. **One representation, clean transfer functions.** Analyses are total functions
   `transfer : Instr × AbstractState → AbstractState`, composed by a generic fixpoint
   engine over the CFG. No global mutable table, no save/restore scoping hacks, no
   syntactic pattern-matching in the analysis core.
3. **Soundness is a written invariant, not folklore.** Each abstract domain states what
   its elements *mean* (a concretization γ); each transfer function is justified against
   it. New rules are added against the invariant, not by analogy.
4. **The corpus is the specification of behavior.** Anything the old engine proves, the
   new one must prove (or better); anything the old engine rejects as unsafe, the new one
   must reject. Divergence is a bug in exactly one of them, found by differential testing.

## 3. The pipeline

```
                        ┌───────────────────────── KEEP ─────────────────────────┐
 source ──► lexer ──► parser ──► AST ──► name resolution ──► type checking ──► typed AST
                        └─────────────────────────────────────────────────────────┘
                                                                       │
                                                            (NEW) lowering
                                                                       ▼
                                            ┌──────────────── IR (src/ir/) ───────────────┐
                                            │  module = { functions }                      │
                                            │  function = typed signature + CFG            │
                                            │  CFG = basic blocks + edges                  │
                                            │  block = list of SSA instructions            │
                                            │  value = SSA name with a type and identity   │
                                            └──────────────────────────────────────────────┘
                                                                       │
                                          (NEW) analysis passes (src/analysis/)
                                          run by one generic CFG fixpoint engine
                                            • VRA  : octagon abstract interpreter
                                            • borrow / region                     (NLL falls out)
                                            • linearity / ownership
                                            • effects, termination
                                                                       ▼
                                                                 checked IR
                                                                       │
                                                     (NEW) backend (src/ir/emit_c.h)
                                                                       ▼
                                                          C   (later: LLVM seam)
```

### 3.1 What is reused verbatim
- **Lexer, parser, AST** (`src/lexer.h`, `src/parser/`, `src/ast.h`). No changes.
- **Name resolution + type checking** — reused to *produce* the typed AST that lowering
  consumes. These are ported into the new path but not redesigned; long-term some of their
  responsibilities (e.g. purity classification) move to IR passes.

### 3.2 What is new
- **`src/ir/`** — the IR data structures, AST→IR lowering, IR→C backend, IR dumper.
- **`src/analysis/`** — the octagon domain, the CFG fixpoint engine, and each analysis pass
  as a consumer of abstract state.

### 3.3 What is retired (at the end of Phase 3)
- **`src/sema/`** — every analysis it holds is re-expressed as an IR pass and deleted.

## 4. Coexistence and differential bring-up

The single most important architectural decision: **the new core is built beside the old,
in the same repository, and the two are run against each other.**

- A selector — `--engine=old|ir` (env `LAIN_ENGINE`) — chooses the pipeline. `old` is the
  default until the new engine dominates; then the default flips; then `old` is removed.
- **`old` is the oracle.** For every program in the corpus (and every fuzzer-generated
  program), we compare the two engines' *accept/reject* decision and, for accepted
  programs, the *runtime behavior* of the emitted C (under ASan/UBSan).
- **Divergence classification** (harness `diff_engines.sh`, Phase 2.6):
  - `old accept / new reject` → **precision regression** in the new engine — fix (usually a
    missing transfer-function case or a domain that needs an extension).
  - `old reject / new accept` → **soundness alarm** — execute the accepted program under
    sanitizers; if it traps, the new engine is unsound; if it is genuinely safe, the *old*
    engine was over-conservative (a win to record).
  - `both accept, different runtime` → a lowering/codegen infidelity — fix in `src/ir/`.
- The new engine is promoted only when it **dominates**: it accepts a superset of the old's
  accepted programs, rejects a superset of the old's *sound* rejections, and every accepted
  program is sanitizer-clean across the whole corpus and all fuzzers.

This is what makes a clean rewrite tractable rather than reckless: the enormous,
fuzz-hardened corpus is a mechanical proof that the new core is at least as good as the old
before anything is thrown away.

## 5. Module layout

```
src/
  lexer.h                 (keep)
  parser/                 (keep)
  ast.h                   (keep)
  sema/                   (keep as oracle → delete in Phase 3)
  ir/
    ir.h                  IR types, values, instructions, blocks, CFG, functions
    lower.h               typed AST → IR  (SSA construction)
    dump.h                IR pretty-printer (debugging + golden tests)
    emit_c.h              IR → C backend
  analysis/
    octagon.h             the relational numeric domain (DBM over ±vars) + closure/join/widen
    fixpoint.h            generic worklist fixpoint over a CFG, with widening/narrowing
    vra.h                 VRA pass: transfer functions + consumers (bounds, overflow, refine)
    borrow.h              region/borrow pass (Phase 3)
    linearity.h           ownership pass (Phase 3)
    effects.h             effect pass (Phase 3)
  main.c                  (wire the engine selector)
```

## 6. Boundaries and contracts

- **AST → IR** is the only place that reads the AST for the new core. Downstream passes see
  *only* the IR. This is what kills name-keying: an SSA value is identified by its
  definition site, not its source name.
- **IR is typed and total.** Every value has a type; every block ends in a terminator; the
  CFG is explicit. Analyses never re-parse or re-resolve.
- **The abstract domain has a concretization.** `octagon.h` documents γ(octagon) ⊆ ℤⁿ; every
  transfer function is sound w.r.t. γ. Consumers (bounds/overflow) query the domain, never
  the AST.
- **The backend is dumb by default.** `emit_c.h` produces correct C from checked IR;
  optimization (Phase 4) is a separate IR→IR pass, so a bug there can never affect
  soundness.

## 7. Non-goals (explicitly out of scope)

- Redesigning the **language** (analysis18 is closed; the spec stands).
- Rewriting the **frontend**.
- A general **SMT / polyhedra / Presburger** engine in the analysis core — determinism and
  compile speed are requirements; octagons + packing + a few targeted nonlinear lemmas are
  the sweet spot. (Revisit only if a concrete, high-value idiom demands it.)
- A **big-bang** switch — every phase leaves a working, corpus-green compiler.

## 8. Open questions for Phase 0 review (resolve in the docs, log in REBUILD.md)

- SSA now, or CFG + domain-join-at-merges first with SSA following? (Leaning: CFG+join
  first — simpler, and SSA is an optimization of the same idea.)
- Octagon from the start, or a clean, fully-*closed* difference-bound domain first, then
  generalize? (Leaning: DBM-closed first — it already subsumes most current recognizers and
  de-risks the fixpoint engine; octagon adds the `x+y ≤ c` family.)
- Variable packing policy (which variables share an octagon) — syntactic co-occurrence in
  guards/indexing is the standard heuristic; finalize in the VRA design.
- Where exactly the purity/effect classification lives (frontend vs. IR pass).
