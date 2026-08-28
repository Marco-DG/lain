# VRA Recognizer Catalog — the Phase-2 acceptance checklist

*Phase 0.4 of the clean-core rebuild (see `REBUILD.md`, `design/vra-octagon.md` §8). Every
special-case in the current VRA (`src/sema/ranges.h`, `src/sema/bounds.h`, and the VRA parts
of `src/sema.h`), enumerated and classified by how the octagon abstract interpreter subsumes
it. This is the checklist the new engine must satisfy before the old one is retired (2.8): a
program the old engine proves via row X must be proven by the new engine either because the
fact **(a) falls out** of the general fixpoint, **(b)** is discharged by a small **sound
transfer case** on a nonlinear op, or **(c)** is a catalogued **nonlinear lemma**.*

**Headline result of this enumeration:** of ~60 distinct recognizers, **~50 are (a)** — they
disappear into the octagon + CFG fixpoint + join + widening; **~9 are (b)** — a small,
reviewable set of interval/one-sided transfer functions on nonlinear ops; **exactly 1 is
(c)** — the flattened-2D lemma. The epicycle pile collapses to roughly *one screen* of
principled domain code. That is the thesis of the rebuild, made concrete and checkable.

Legend: **(a)** falls out of the domain · **(b)** sound transfer case (kept, small) ·
**(c)** nonlinear lemma (kept, catalogued).

---

## Group 1 — Interval arithmetic & seeding  *(a, baseline)*
The octagon carries a per-variable interval natively (its unary bounds); these become the
base arithmetic transfer functions.
- `range_add/sub/mul/div/mod` saturating (ranges.h:60) — **(a)** `add`/`sub`/`neg` are exact
  octagon assignments; `mul`/`div`/`mod` fall to interval transfer (see Group 7).
- Param type-range seeding; narrow-element-array read `u8[N]→[0,255]` (ranges.h:278) — **(a)**
  entry seeding + element type interval.
- Top-level constant contributes its value/type range (ranges.h:256) — **(a)** constant folding.
- Wrap/saturate result range (ranges.h:299) — **(a)** `.wrap`/`.sat` instruction flags.
- Checked-op arithmetic range ∩ T (ranges.h:455) — **(a)** the overflow obligation on the op.

## Group 2 — Guard capture / conditional refinement  *(a)*
All of these are the `br_cond` guard transfer (meet the constraint on the true edge, its
negation on the false edge) + the join at merges. The octagon expresses every one directly.
- `i < n`, `i <= n`, `i > k`, `i >= k`, `i == c`, `i != c` (apply_constraint literal/ident) — **(a)**.
- `i < n ± k` and `i + k < n` — symbolic-offset guards (ranges.h:1070, 1093) — **(a)**
  octagon constraints `i − n ≤ −1−k` etc.
- `i < n / D` ⇒ `i < n` for `n ≥ 0` (two-pointer under `i < n/2`) (ranges.h:1117) — **(a)**
  the `/D` is nonlinear but the *implied* octagon fact `i < n` is added soundly (a one-sided
  transfer; borderline (b), but the consumed fact is octagonal).
- `==`/`!=` boundary narrowing (`n != 0 ⇒ n ≥ 1`) (ranges.h:1273, 1283) — **(a)** guard meet.
- Negated compound guard / De Morgan `if i<0 or i>=len {return}` (ranges.h:1184) — **(a)**
  the false-edge of `br_cond` carries `¬cond`; nothing special.
- `and`-chain refinement (ranges.h:971) — **(a)** sequential meets along the edge.
- Length-equality `if a.len != b.len {return}` ⇒ `a.len == b.len` (ranges.h:1223) — **(a)**
  guard relating two `slice_len` values (octagon `a.len − b.len = 0`).
- nonzero marker `v != 0` for division (ranges.h:775) — **(a)** the octagon can't hole-punch
  an interval either, so this stays as a tiny side-fact on the state (a division precondition,
  not a range) — trivially carried.

## Group 3 — Length / slice identity  *(a — this whole machinery EVAPORATES)*
The ugliest, most name-keyed code in the engine. In the IR a slice's length is a **value**
(`slice_len`), so it is just another octagon variable related to indices. All the synthetic
`__len_x`/`__mk_path` key-plumbing disappears.
- `__len_PARAM` synthetic seeding; `range_member_len_id` (ranges.h:514, 841) — **(a)**.
- `member_len_size_var` (`a.len` → size-expr `n`) (ranges.h:846) — **(a)** `slice_len a` = the value.
- `member_len_const` (fixed array `a.len` → constant N) (ranges.h:866) — **(a)** `Array{n}` len.
- member-path `__mk_l.src.len` keys for struct-field slices (ranges.h:903, 953) — **(a)** the
  field's `slice_len` is a value; no string keys.
- sized-slice `.len` non-negativity / fixed-length-string count (ranges.h:533, 544) — **(a)**.

## Group 4 — Bounds proof paths  *(a — become "query the octagon at `elem_ptr`")*
`sema_check_bounds`'s cascade of L1–L4 paths collapses to a single query.
- Fast interval path for fixed/known-len (bounds.h:280) — **(a)**.
- Constraint-chaining `while i<n {a[i]}` (bounds.h:297) — **(a)** the loop invariant `i<n` is
  the fixpoint's output; the query `i < len` succeeds.
- Offset index `a[i±k]` / base±k threshold shift (bounds.h:321, 354) — **(a)** octagon relates
  `i±k` to `len`.
- Sub-slice `arr[a..b]` bounds (bounds.h:161) — **(a)** `subslice` transfer + query.
- Dynamic-slice constraint proof `idx − __len ≤ −1` (bounds.h:349) — **(a)**.
- Struct-field / member-path slice `l.src[i]` (bounds.h:498) — **(a)**.
- **Omega Test** linear fallback (bounds.h:610) — **(a)** the octagon *is* a (weaker but
  closed) linear domain; the difference-constraint fragment Omega solves is exactly what
  closure gives. The rare general-linear size_expr (`out[a.len+b.len]`) is octagon-expressible
  as a sum bound. *(If a real case needs full linear arithmetic beyond octagons, it becomes a
  (c) lemma — none observed today.)*
- VRA#3 synthesize `arr.len` for plain `T[]` (bounds.h:621) — **(a)** `slice_len` value.

## Group 5 — Termination / measure  *(a; nonlinear decreases are (b))*
The measure machinery becomes a **consumer** of the fixpoint: the invariants it needs are
discovered; the "decreases" check is a query on successive states.
- `assignment_direction`: `x ± K`, invariant-step `x ± E`, difference measures (sema.h) — **(a)**.
- constant-decrease `j = 0` under `j > 0` (sema.h) — **(a)** it's a join fact
  (`j` after ∈ join([0,0], guard) — proven by the fixpoint, not the ad-hoc `cond_target_min`).
- loop-measure inference `while i<n ⇒ n−i` (sema.h `synth_measure_from_cond`) — **(a)** the
  invariant/measure both come from the CFG; break-branch exemption is just an edge.
- recursion-measure inference (sema.h `infer_recursion_measure`) — **(a)** function-summary +
  descent check on the call graph.
- halving `v = v/D`, modulo `v = X%v`, shift `v = v>>C` DECREASE proofs (sema.h) — **(b)** the
  *op* is nonlinear (Group 7); the decrease is a small monotonicity fact on it.

## Group 6 — Loop-invariant recovery  *(a — REPLACED WHOLESALE by fixpoint + widening)*
This is the hackiest, highest-value category and it **all** becomes standard abstract
interpretation. This is the single biggest simplification of the rebuild.
- Inductive loop-invariant VRA `liv_*` (binary search `0≤lo<hi≤len`) (sema.h) — **(a)** widening
  + narrowing at the loop header.
- L3 affine recap / monotone-variable post-loop bounds (sema.h `l3_scan_affine`) — **(a)**.
- monotone pointer table (`p in arr`, decrement) (sema.h) — **(a)** octagon on `p − &arr[0]`.
- **bounded-counter** `n≤m` (my `bc_scan`, sema.h) — **(a)** the fixpoint discovers `n ≤ m`
  as the loop invariant; the whole guard-stack scanner + pre-loop capture disappears.
- **clamp-join** `if x>C {x=C}` (my one-armed-if patch, sema.h) — **(a)** the φ-join computes
  `join([C,C], ¬(x>C)) = [.., C]` for free; the patch disappears.
- **post-loop negation** `after while j>0 ⇒ j==0` (my patch, sema.h) — **(a)** the exit edge
  of `br_cond` carries `¬cond`; nothing special.

## Group 7 — Nonlinear-op transfer functions  *(b — KEPT, small & reviewable, ~6 rules)*
These are genuinely non-octagonal ops; each is a small, sound interval/one-sided transfer,
carried as a named transfer function in the domain (NOT a syntactic recognizer).
- **mask** `x & C` (C≥0) ∈ `[0, C]` for any x (ranges.h:120, `range_bitand`) — **(b)**.
- `x / D` (D≥1): `0 ≤ x/D ≤ x` for `x ≥ 0` (ranges.h:348) — **(b)**.
- `x % m` ∈ `[0, m−1]` for `m > 0` (`arr[i % arr.len]`) (bounds.h:534) — **(b)**.
- shift `x << c` / `x >> c` result range — **(b)**.
- `@movemask` ∈ `[0, 2^N−1]`; `@popcount/@ctz/@clz` ∈ `[0,64]` (ranges.h:493, 496) — **(b)**.
- `mul` general → interval product (ranges.h:60) — **(b)** (exact octagon only when a factor
  is constant, which is `x*c` = octagon-scalable → (a)).

## Group 8 — Genuine nonlinear lemmas  *(c — KEPT, catalogued, exactly 1 today)*
- **flattened-2D** `a[i*w+j]` over `a[h*w]`: `i<h ∧ j<w ⟹ i*w+j < h*w = len` and the `i*w`
  no-overflow (bounds.h:90, `bounds_recognize_2d`) — **(c)** a named domain lemma applied at
  `elem_ptr` when the index matches the flattened shape, with its soundness proof and tests
  (`fuzz_2d.sh`, matrix_* tests). The ONE place the domain is deliberately extended past
  linear-unit-coefficient reasoning.

## Group 9 — Return refinements / function summaries  *(a)*
- literal refinement `f() int >= 0` (ranges.h:644) — **(a)** a function summary consumed at calls.
- body-level return-range inference (ranges.h:618) — **(a)** the callee's converged exit state.
- **parametric** refinement `f(m) usize <= m` + call-arg substitution (O-004, ranges.h:1418,
  `sema_range_from_call`) — **(a)** summary with a relational post-condition (`result ≤ m`),
  instantiated per call.

---

## Tally & implication
| classification | count (approx) | disposition in the new engine |
|---|---|---|
| **(a) falls out** | ~50 | disappears into octagon + CFG fixpoint + join + widening + summaries |
| **(b) transfer case** | ~9 | a named, sound transfer function per nonlinear op (one small file) |
| **(c) nonlinear lemma** | 1 | one catalogued domain extension (flattened-2D) with proof + tests |

So the new VRA core is: the octagon domain + closure/join/widening (Group 0, ~1 file), the
CFG fixpoint (~1 file), the transfer functions (Groups 1–6, 9 — mostly generic), a handful of
nonlinear transfers (Group 7), and one lemma (Group 8). **The 70-recognizer pile is not
ported — it is *proven redundant* by this table, then deleted.**

## How this is used (Phase 2.8 gate)
1. For each row, ensure a test exists that exercises it (most already do — noted inline; fill
   gaps during 0.4 follow-up).
2. Bring up the new engine; run `diff_engines.sh` over the whole corpus + fuzzers.
3. Any `old-accept / new-reject` divergence maps to a row here — confirm the (a)/(b)/(c)
   mechanism covers it; if not, that row is a real domain gap to close before promotion.
4. Promote only when every row is covered and the new engine dominates.
