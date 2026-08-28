# Lain VRA — Relational Abstract Interpreter (Octagon Domain)

*Phase 0.3 of the clean-core rebuild (see `REBUILD.md`, `design/architecture.md`,
`design/ir.md`). The design of the new value-range analysis: a sound relational abstract
interpreter over the **octagon** domain, run as a fixpoint over the IR's CFG. This is the
core of Lain's promise — deterministic static proof for safety and zero-cost. When it
stabilizes, its **contract** goes to the normative spec (ch.13) and this **design** becomes
a spec annex (Phase 0.5).*

---

## 1. What the VRA must deliver

For every reachable program point, a sound over-approximation of the possible values (and
*relations between* values) of the live IR values, precise enough to discharge:

- **Bounds** — at every `elem_ptr base, idx`, prove `0 ≤ idx < len(base)`.
- **Overflow** — at every checked arithmetic / `trunc`, prove the ℤ result fits the target type.
- **Division** — at every `sdiv/udiv/srem/urem`, prove the divisor ≠ 0.
- **Refinements** — verify a `func f(...) T <op> e` return refinement, and propagate it to callers.
- **Termination** — supply the loop-invariant facts that make measures provable (the measure
  machinery becomes a consumer of the same fixpoint).

"Relations between values" is the load-bearing phrase: intervals alone cannot prove
`a[n-1-i]` under `i < n/2`, merge's `o = i+j < a.len`, Lomuto's `i ≤ j`, or flattened
`a[i*w+j] < a[h*w]`. These need a **relational** domain. That is the entire reason for the
rebuild.

## 2. The abstract domain: octagons

### 2.1 What an octagon is
An octagon over variables `v₁…vₙ` is a conjunction of constraints of the form

```
    ± vᵢ ± vⱼ ≤ c        (i ≠ j)          and          ± vᵢ ≤ c        (unary)
```

with `c ∈ ℤ ∪ {+∞}`. Geometrically a polyhedron whose faces have slopes in
`{0, ±1, ∞}` — hence "octagon". This is strictly more expressive than intervals
(`vᵢ ≤ c`, `−vᵢ ≤ c`) and than difference-bounds (`vᵢ − vⱼ ≤ c`); the extra `vᵢ + vⱼ ≤ c`
family is exactly what covers the two-pointer / symmetric-index idioms.

Reference: Miné, *The Octagon Abstract Domain* (HOSC 2006). We implement his representation
and algorithms; this section states what we need, not a re-derivation.

### 2.2 Representation — a DBM over ± dimensions
Introduce `2n` **dimensions**: for each `vᵢ`, a positive form (`vᵢ`) and a negative form
(`−vᵢ`). A **Difference Bound Matrix** `m` of size `2n × 2n` stores, in `m[j][k]`, an upper
bound on `dₖ − dⱼ` where `dⱼ, dₖ` range over the ± dimensions. Every octagon constraint is a
difference between two dimensions:

```
    vᵢ − vⱼ ≤ c   ,   vᵢ + vⱼ ≤ c   ,   −vᵢ − vⱼ ≤ c   ,   vᵢ ≤ c   ,   −vᵢ ≤ c
```

all become single DBM entries. `+∞` means "no constraint". `⊥` (empty octagon) is detected
by a negative cycle (a diagonal entry `< 0` after closure).

### 2.3 Meaning (concretization γ) — the soundness anchor
`γ(m) = { (x₁,…,xₙ) ∈ ℤⁿ | every constraint encoded in m holds }`.

**The invariant every transfer function must preserve:** if the concrete state at a program
point is `σ`, and the analysis computes octagon `m` there, then `σ ∈ γ(m)`. Over-approximate
freely; never exclude a reachable state. This one sentence is the yardstick for adding any
rule, forever. (It replaces the folklore soundness reasoning scattered through `src/sema/`.)

### 2.4 Core operations (what `analysis/octagon.h` implements)
- **Closure `m*`** — shortest-path (Floyd–Warshall) closure making all *implied* constraints
  explicit, plus Miné's **strong closure** tightening that exploits `+/−` coherence
  (`vᵢ − vⱼ ≤ a ∧ vⱼ + vⱼ' handled via the unary bounds`). O(n³). Required before join,
  emptiness test, and any query — an unclosed DBM answers queries imprecisely.
- **Emptiness** — `⊥` iff some `m*[i][i] < 0`.
- **Order `⊑`** — `m₁ ⊑ m₂` iff `m₁* ` entrywise ≤ `m₂*` (m₁ is tighter).
- **Join `⊔`** — on *closed* operands, entrywise **max** (the tightest octagon ⊇ both). This
  is the merge operator at φ-nodes — the thing `src/sema/` lacks.
- **Meet `⊓`** — entrywise **min**, then re-close. Used to add a guard's constraints.
- **Widening `∇`** — `m₁ ∇ m₂` keeps each entry of `m₁` that `m₂` does not exceed, sets the
  rest to `+∞`. Guarantees loop convergence (no infinite ascending chains). Applied only at
  loop headers. An optional **narrowing** pass afterwards recovers precision the widening gave up.
- **Projection (`forget vₖ`)** — set all entries touching `vₖ`'s two dimensions to `+∞`
  (after closing so implied facts among the survivors are retained). Used by assignment.
- **Add constraint / test** — tighten the relevant DBM entry(ies) then close.
- **Interval read** — `[lo,hi]` of `vᵢ` extracted from the unary entries of `m*` (for the
  bounds/overflow consumers and for fallback).

### 2.5 Scalability — variable packing (mandatory)
A single octagon over *all* of a function's values is O(n²) space and O(n³) closure — too
much for large functions. Standard remedy (Astrée, apron): **packing** — partition the
values into small **packs**, keep one octagon per pack, and only variables in the same pack
can be related. Heuristic (finalize during Phase 2): put two values in the same pack when
they **co-occur** in a guard (`i < n`), an index (`a[i]`), or an assignment (`o = i + j`);
transitively close small. Values never co-occurring stay in interval-only singleton packs.
This keeps closure over small packs, recovering near-linear behavior while retaining the
relational facts that actually matter for proofs.

### 2.6 Integers over ℤ; overflow as a separate obligation
The domain reasons over mathematical integers. Machine wraparound is **not** modeled in the
octagon; instead every checked arithmetic op emits an **overflow obligation** discharged by
querying the ℤ result range against the type interval. Rationale: Lain's promise is *no
silent wrap*, so the sound thing is to prove no-wrap (or force `.wrap`/`.sat`), and mixing
wrap into the numeric domain would both weaken it and complicate soundness. `.wrap` ops skip
the obligation and the domain widens their result to the type interval; `.sat` clamps.

## 3. Transfer functions (IR instruction → domain update)

Each is a total function `⟦instr⟧ : Oct → Oct`, sound w.r.t. §2.3. Highlights:

- `const K` → new value pinned to `[K,K]`.
- `add v = x + y` → `forget v`, then add `v − x − y ≤ 0 ∧ x + y − v ≤ 0` (exact: `v = x+y`);
  `sub` similarly; `neg v = −x` exact. **Overflow obligation** recorded on `v`.
- `x + c`, `x − c` (constant) → exact octagon assignment (`v − x ≤ c ∧ x − v ≤ −c`).
- `mul, div, shift, mod` → generally **non-octagonal**; compute the *interval* of the result
  (from operands' intervals) and pin `v` to it; where one operand is a constant power/divisor,
  add the sound one-sided relation (`v ≤ x` for `x/D`, `D≥1, x≥0`; the mask/`&` bound; etc.).
  These are the few genuinely nonlinear spots — see §6.
- `icmp` → a `Bool` value; its *use* is in `br_cond`, where the guard is applied.
- `br_cond c, T, E` → on edge→T, meet with the constraint `c`; on edge→E, meet with `¬c`.
  `c` of the form `x < y`, `x ≤ y`, `x < c`, `x ≠ c` (boundary) map directly to octagon
  constraints. **This is the sole home of conditional refinement** — no early-return or
  post-loop special cases; those are just other edges.
- `phi [(x,T),(y,E)]` → `⟦T's state⟧ ⊔ ⟦E's state⟧` with `v` related to the incoming values.
  **The join is where merge precision comes from, for free.**
- `elem_ptr a, i` → *query only* (no state change): prove `0 ≤ i` and `i < slice_len a`
  (or `< N` for a fixed array). `slice_len a` is a live value the domain relates to `i`.
- `call f, args` → `v`'s range from `f`'s return refinement with the actual args substituted
  (the O-004 mechanism, now a clean domain operation); plus effect/purity handled by the
  effects pass.
- `cast.trunc/narrow` → **overflow obligation** (result fits target); `zext/sext` exact.

## 4. The fixpoint

`analysis/fixpoint.h` is a generic worklist iteration over the CFG:

1. Start entry with the parameters' type intervals (params seeded from `irtype_int_range`,
   plus any input refinements). All other blocks ⊥.
2. Worklist of blocks. For a block: `in = ⊔ over predecessors of (out_pred refined by the
   edge guard)`; `out = ⟦block body⟧(in)`. If `out` changed, enqueue successors.
3. **Widening at loop headers:** at a header `H`, instead of plain join use
   `in_H := in_H ∇ (new in_H)` once the loop has been visited, forcing convergence.
4. Optional **narrowing** pass to tighten post-widening.
5. At convergence, every program point has its octagon; consumers run their queries.

Loop invariants — including today's hand-built "bounded counter" `n ≤ m` — are now **the
fixpoint's output**, discovered, not pattern-matched.

## 5. Consumers (proof obligations discharged against the converged state)

- **Bounds:** at `elem_ptr a, i`, query `i ≥ 0` and `i < slice_len a` (or `< N`). Emit E085
  with the same helpful hints as today when it fails.
- **Overflow:** at each obligation, query the ℤ result range vs. the type interval → E086.
- **Division:** query divisor ≠ 0 → E015.
- **Refinements:** at `ret`, verify the refinement holds in the converged state; a call
  substitutes args (return-range propagation).

## 6. Where octagons stop — and the disciplined escape hatch

Octagons are linear with unit coefficients. They do **not** natively prove genuinely
nonlinear facts — chiefly the flattened-2D `i*w + j < h*w` where `w,h` are runtime values.
Policy:

- Keep the domain pure (octagon + intervals for nonlinear results). Deterministic, O(n³) per
  small pack, fast.
- Add **targeted, sound nonlinear lemmas** as explicit, catalogued domain extensions *only*
  when a concrete high-value idiom demands it — e.g. the `i<h ∧ j<w ⟹ i*w+j < h*w` lemma,
  applied at `elem_ptr` when the index matches the flattened shape. Each such lemma is a named
  rule with a soundness proof and tests, exactly like a transfer function — **not** a syntactic
  recognizer bolted onto a walk.
- Explicitly **out of scope:** a general SMT/polyhedra/Presburger engine in the hot path
  (nondeterministic cost, undecidability). Revisit only with a compelling, measured case.

The distinction from the current ~70 recognizers is the whole point: there, the *domain* is
too weak so idioms are matched by syntax; here, the domain is strong enough that idioms fall
out, and the handful of true nonlinear facts are *principled lemmas*, few and reviewed.

## 7. Soundness methodology (unchanged discipline, cleaner target)

- The γ-invariant (§2.3) is the written yardstick; every transfer function is justified against it.
- The **execute-accepted fuzzers** carry over verbatim: any program the new engine accepts is
  compiled and run under ASan/UBSan against adversarial inputs; a trap is an unsound transfer
  function. The domain operations (`octagon.h`) additionally get **algebraic unit tests**
  (closure idempotence, join ⊒ operands, widening termination, γ-monotonicity spot-checks).
- Differential testing vs. the old engine (`diff_engines.sh`) is the bring-up gate.

## 8. Mapping from the current engine (→ Phase 0.4 catalog)
The ~70 recognizers in `src/sema/ranges.h` + `bounds.h` are enumerated in the **recognizer
catalog** (0.4), each classified as: **(a)** falls out of the octagon fixpoint (most —
constraint-chaining scans, `.len` guards, offset guards, bounded counter, clamp-join,
post-loop negation, `==`/`!=` refinement, difference measures); **(b)** an interval/one-sided
fact on a nonlinear op (mask `x&C`, `x/D`, `x%v`, shift) — kept as small sound transfer cases;
**(c)** a genuine nonlinear lemma (flattened-2D) — kept as a catalogued extension (§6). That
catalog is the acceptance checklist for Phase 2.8.

## 9. Open items (finalize entering Phase 2)
- Strong-closure implementation details (integer tightening `⌊c/2⌋` steps) — follow Miné exactly.
- Packing policy parameters (max pack size; co-occurrence transitivity depth).
- φ vs. block-parameter handling in the join (align with the IR's final choice).
- Whether to land a **closed difference-bound** domain first (subset: only `vᵢ − vⱼ`), then
  add the `+` family for full octagons — de-risks the engine; most current cases are DBM-only.
- Narrowing on/off by default (precision vs. determinism of results).
