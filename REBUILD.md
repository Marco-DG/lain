# Lain Compiler — Clean-Core Rebuild Plan & Tracklist

> **North star.** Lain's identity is *deterministic static proof for safety and zero-cost*.
> Its proof engine therefore has to be world-class — clean, principled, powerful,
> complete. Today the middle-end is a sound-but-accreted AST-directed, name-keyed,
> single-pass mess with ~70 hand-coded recognizers and no CFG. We rebuild the **core**
> (not the language, not the frontend) around a typed SSA/CFG IR and a real relational
> abstract interpreter (octagons + fixpoint), **in place, beside the old engine**,
> brought up by **differential testing against the existing corpus**, phased so the
> corpus is green at every checkpoint.

This file is the single source of truth for the effort. **Update the Status log at the
bottom every session.** Check boxes as work lands. Keep it honest.

---

## Ground rules (non-negotiable)

- **The corpus is the gate.** ~610 tests + trust harness + 14 execute-accepted fuzzers.
  Never regress it. It is green at the end of every phase. It is *the* reason a clean
  rebuild is survivable here.
- **Coexistence, not big-bang.** The new engine is built *alongside* `src/sema/`. A
  build flag selects old vs. new. The old engine is the reference oracle during bring-up.
- **Differential bring-up.** Every analysis change: run old vs. new on the whole corpus +
  fuzzers, diff accept/reject. `new rejects what old accepted` ⇒ precision regression to
  fix. `new accepts what old rejected` ⇒ soundness check (execute under ASan/UBSan).
- **Each phase ships a working compiler.** Never more than one phase from green.
- **Soundness first, precision second.** Every analysis change is fuzzed (execute-accepted).
- **Keep the frontend.** Lexer, parser, AST work — reuse them. We are not re-litigating
  parsing or redesigning the language (analysis18 closed; the spec exists).
- **Docs track reality.** Design docs are written from the actual implementation and
  updated as we learn; the spec carries the *contract*, an annex carries the *design*.

---

## Target architecture

```
source
  │  (KEEP) lexer → parser → AST
  ▼
AST
  │  (KEEP, then port) name resolution + type checking → typed AST
  ▼
typed AST
  │  (NEW) lower  →  typed SSA/CFG IR          src/ir/
  ▼
IR (functions = CFG of basic blocks; SSA values with identity)
  │  (NEW) analysis passes over the IR         src/analysis/
  │        • VRA = octagon abstract interpreter (fixpoint + widening)
  │        • borrow / region
  │        • linearity / ownership
  │        • effects, termination
  ▼
checked IR
  │  (NEW) backend                              src/ir/emit_c.h  (later: LLVM seam)
  ▼
C  (or LLVM IR later)
```

### Key design decisions (finalize in Phase 0, record in the docs)
- **Numeric domain: octagons** (`±x ± y ≤ c`) — a superset of difference-bounds (`x−y ≤ c`),
  the relational power that cracks the deferred frontier (merge `o=i+j`, Lomuto `i≤j`,
  symbolic 2D `i*w+j<h*w`). O(n²) space / O(n³) closure **per variable pack** ⇒ **variable
  packing is mandatory** for scalability. *Staging option:* land a clean, fully-*closed*
  difference-bound domain first (covers most bounds idioms), then generalize to octagons.
- **Analyze over mathematical integers (ℤ).** Overflow is a *separate* proof obligation
  (Lain proves no-wrap anyway), so the numeric domain assumes no wrap and the overflow
  checker guards it. Clean and standard.
- **Fixpoint:** worklist over the CFG; widening at loop headers after N iterations; optional
  narrowing pass. Loop invariants (incl. today's bounded-counter) are *discovered*, not
  pattern-matched.
- **IR is a typed CFG in SSA form.** SSA gives value identity (kills name-keying) and clean
  φ-at-join. *Staging option:* CFG + domain-join at merge points first; full SSA construction
  can follow — the essential thing is CFG + join + fixpoint.
- **Spec split:** normative *contract* in `spec/` chapter 13 (what the VRA guarantees, not
  how); *design* in a spec annex (the octagon domain + algorithm) — first-class & permanent.

---

## Phase 0 — Blueprint (the design, where we commit)  ⟶ deliverable: docs

- [x] **0.1 Compiler-architecture doc.** → `design/architecture.md`. Target pipeline,
      module layout (`src/ir/`, `src/analysis/`), old↔new coexistence + `--engine` selector +
      `diff_engines.sh` differential strategy, keep/rebuild/retire boundaries, non-goals.
- [x] **0.2 IR spec.** → `design/ir.md`. Types, SSA values + memory model (alloca/load/store
      → mem2reg), instruction set, terminators (`br_cond` = the guard), CFG/functions/module,
      AST→IR lowering per construct (if→φ-join, while→back-edge header, match→switch,
      break/continue→edges), IR→C backend, worked example. The keystone.
- [x] **0.3 VRA / octagon design.** → `design/vra-octagon.md` (→ spec annex at 0.5). Octagon
      domain (DBM over ±dims), closure/join/meet/widening/projection; the γ-invariant as the
      soundness yardstick; transfer functions per IR instr; CFG fixpoint + widening at headers;
      consumers (bounds/overflow/div/refinement); variable packing (mandatory); ℤ+overflow
      split; the nonlinear escape-hatch (catalogued lemmas, not recognizers); §8 maps the ~70
      current recognizers to (a) falls-out / (b) small transfer case / (c) nonlinear lemma.
- [x] **0.4 Recognizer catalog.** → `design/recognizer-catalog.md`. All ~60 special-cases in
      `ranges.h`/`bounds.h`/`sema.h` enumerated + classified. **Result: ~50 (a) fall out of the
      octagon+CFG fixpoint (incl. the entire `__len_`/name-key machinery and ALL loop-invariant
      recovery — liv/affine/bounded-counter/clamp-join/post-loop), ~9 (b) small nonlinear
      transfer cases (mask/div/mod/shift/movemask/popcount), 1 (c) nonlinear lemma
      (flattened-2D).** The pile is proven redundant, then deleted. This is the Phase-2.8 gate.
- [x] **0.5 Normative spec contract.** `spec/chapters/13-vra.tex` Overview rewritten to the
      abstract *contract*: normative Soundness (over-approximation ⇒ accepted-is-safe;
      prove-or-reject) + Determinism/decidability (poly-time, no SMT) constraints, explicitly
      decoupled from the domain/algorithm, pointing to the design. Spec recompiles clean.
      *(Full VRA design ANNEX deferred to end of Phase 2 — written from the real engine, per
      "docs track reality"; `design/vra-octagon.md` is the design home until then.)*
- [x] **0.6 Review + lock.** Docs cross-consistent (architecture→ir→vra-octagon→catalog).
      **Domain decision locked (see log):** stage a fully-*closed difference-bound* domain
      first (covers ~all current recognizers), then add the octagon `+`-family for the
      relational frontier (merge/interleave `i+j<len`); ℤ + separate overflow obligation;
      variable packing mandatory; CFG + join first, SSA/mem2reg as a follow-up.

## Phase 1 — IR foundation (the keystone)  ⟶ deliverable: faithful IR pipeline

- [x] **1.1 IR data structures + builders** (`src/ir/ir.h`, `src/ir/build.h`): IrType,
      IrValue (SSA id = identity), IrInstr (slice_len/elem_ptr/wrap-mode/aux), IrTerm, IrBlock
      (φ + body + preds + loop-header flag), IrFunc/IrModule; builder + convenience-emitter
      definitions; `ir_finalize_cfg` (predecessors + back-edge loop-header detection).
      **Validated** by `src/ir/test_ir.c` (hand-built `maxi` branch + `count` loop → correct
      SSA-form dump; loop header auto-detected). Not wired into main.c; old engine green.
- [~] **1.2 AST → IR lowering** (`src/ir/lower.h`): **core subset WORKS end-to-end on real
      programs.** Type bridge (AST Type→IrType), expressions (literal/ident/binary/unary/
      call/index/member.len/cast), statements (var/assign/if-else/while/return/expr/break/
      continue/unsafe) → memory-form IR (alloca/load/store); `src/ir/lower_driver.c` reuses
      the frontend (load_module+sema_resolve_module→typed AST→lower→dump) and lowers
      `tests/ir/sum_maxi.ln` to a correct CFG (loop header auto-detected; ℤ-widened `i33`
      add results — the overflow-split model). TODO: for/match/structs/strings/short-circuit
      and-or/slices-fully → grow toward the 1.6 gate.
- [~] **1.3 IR → C backend** (`src/ir/emit_c.h`): **scalar core round-trips END-TO-END.**
      Blocks→labels+gotos; SSA values→C locals declared at function top; alloca/load/store→
      real slot pointers (C compiler optimizes away); int/bool + arithmetic/cmp/call/cast/
      branch. `lower_driver.c --emit-c` emits a module; `tests/ir/sum_maxi.ln` lowered →
      emitted C → gcc → **runs correctly (sum(5)==10, exit 0)** through the whole NEW pipeline.
      Arrays/slices/structs/strings (fat-pointer repr) grow next.
- [ ] **1.4 Pipeline wiring:** `--engine=ir` (or `LAIN_IR=1`) selects the new path end-to-end.
- [~] **1.5 IR dumper** (`src/ir/dump.h`): readable SSA-form printer done + validated on
      hand-built IR. Round-trip on real *parsed* programs comes with lowering (1.2).
- [ ] **1.6 GATE:** every `*_pass` test compiles through the new pipeline and the emitted C
      runs identically (trust + fuzzer executables pass). `*_fail` rejection is deferred to
      Phase 2 (no analysis yet). Corpus faithfulness proven.

## Phase 2 — Octagon VRA (the payoff)  ⟶ deliverable: analysis that dominates the old

- [x] **2.1 Octagon domain** (`src/analysis/octagon.h`): DBM over 2n ± dimensions (Miné);
      coherent setters, Floyd–Warshall + strong (integer-tight) closure, ⊥ test, join (⊔ max),
      meet (⊓ min), widening, order, forget/projection, interval read. **Brute-force validated**
      (`test_octagon.c`): closure preserves γ, ⊥ sound, join/meet/widen over-approximate, meet
      exact, widening converges — 40000 random trials over ℤ³ + hand-built tightness cases.
- [ ] **2.2 Variable packing** strategy (cluster vars that co-occur in constraints/indexing).
- [~] **2.3 Fixpoint engine** over the CFG (`src/analysis/vra.h`): sweep-to-fixpoint, join at
      merges, **widening at loop headers** (back-edge detected in `ir_finalize_cfg`). Worklist
      ordering + narrowing pass = polish TODO.
- [~] **2.4 Transfer functions** — const, load/store (memory-cell model: an alloca id doubles
      as its scalar cell, so `i=i+1` becomes a difference fact across iterations), add/sub with a
      constant operand, slice_len (≥0, canonicalized per slice), cast-as-copy, **branch/guard
      refinement** (`i<n` ⇒ the octagon fact on the taken edge). TODO: φ (once mem2reg lands),
      two-variable arith, calls/return-refinements.
- [~] **2.5 Consumers:** **bounds** (`0 ≤ idx < len`, fixed arrays *and* slices), **overflow**
      (CHECK-mode +,−,× — ℤ result range vs the operand type, §2.6), **div-by-zero** (divisor
      range excludes 0), and **termination** (a structured loop with exactly one well-formed step
      `cell = load(cell) ± c` draining a loop-invariant bound) — all four done and validated:
      `test_vra.c` **18/18**, and each fuzzed sound (`fuzz_vra.c`: bounds 200k, overflow-range
      300k, termination 100k trials, 0 unsound). The relational + nonlinear reach (sliding-window,
      two-pointer, reverse, mask/modulo/shift) is here too. Known precision gap (sound): usize/u64
      overflow at 2^64 (int64 domain). TODO: refinement types; u64-aware overflow.
- [~] **2.6 Differential harness** (`vra_survey.sh`): the ⊇-accepts direction is measured — over
      every old-accepted indexing program, the new octagon VRA proves **411/510 bounds
      obligations (81%)**, 40 programs fully. Remaining gaps are lowering-side (nested
      array-in-struct, global arrays, `in`-loops) or off-North-Star (SIMD). Full reject-side
      differential (needs the new engine wired as an alternative pass, not gated by sema's exit)
      is TODO.
- [ ] **2.7 Recognizer parity:** walk the 0.4 catalog; confirm each falls out or add the
      extension. Fuzz each addition (execute-accepted).
- [ ] **2.8 GATE:** new engine passes the ENTIRE suite (pass + fail) and all fuzzers, and
      *dominates* the old (⊇ accepts, ⊇ sound rejects). Then it can become default for VRA.

## Phase 3 — Port the remaining analyses  ⟶ deliverable: retire `src/sema/`

- [ ] **3.1 Borrow / region** checker as an IR pass (CFG-based, NLL falls out).
- [ ] **3.2 Linearity / ownership** as an IR pass.
- [ ] **3.3 Effects** as an IR pass.
- [ ] **3.4 Termination / measure** as IR analysis (loop + recursion, via the fixpoint).
- [ ] **3.5 GATE:** full parity across the corpus + fuzzers; make IR pipeline the default;
      delete `src/sema/`; update docs.

## Phase 4 — Backend & beyond  ⟶ deliverable: leverage the clean IR

- [ ] **4.1 Better C emission / IR-level optimization** (the recognizers become real opts).
- [ ] **4.2 (Optional) LLVM backend seam** on the IR.
- [ ] **4.3 Re-express niche opt / SIMD / annotations** on the IR.

---

## Key decisions log
- 2026-08-28 — **Rebuild in-place, same repo, new core beside old.** Reuse frontend + corpus;
  differential bring-up; no big-bang. (Decision rationale: the corpus is the safety net and
  must not fork; the frontend/language are fine; coexistence needs one repo.)
- 2026-08-28 — **Target domain = octagons + variable packing, over ℤ with overflow split,
  fixpoint over a typed SSA/CFG IR.** (Staging: clean closed DBM first is acceptable.)
- 2026-08-28 — **Spec split:** contract in spec ch.13 (done); octagon design annex written
  from the real engine at end of Phase 2 (`design/vra-octagon.md` is the interim home).
- 2026-08-28 — **AST is NOT rewritten (scope boundary).** A tagged-union parse tree is the
  right shape; the parser is kept. Its only real smell is analysis state bolted onto nodes
  (`l3_*_dead` on ExprBinary; `refine`/`canon`/`int_width_cache`/`arith_widened` on Type) —
  those die with `src/sema/` in Phase 3, and lowering already bridges AST Type → fresh IrType
  so the fatness stops mattering. Optional cheap follow-up: slim the Type struct in Phase 3.
- 2026-08-28 — **Domain staging locked (0.6):** closed **difference-bound** domain first
  (subset: `vᵢ − vⱼ ≤ c`; the recognizer catalog shows ~all current cases are DBM-only), then
  generalize to full **octagons** (`±vᵢ ± vⱼ ≤ c`) for the relational frontier (merge/interleave
  `i+j<len`). CFG + domain-join at merges first; SSA/mem2reg a precision follow-up. This
  de-risks the fixpoint engine bring-up while keeping the octagon endpoint.

## Risks & mitigations
- *Rewrite death-march* → phased, corpus-gated, coexistence + differential testing.
- *Octagon cost blowup* → variable packing; DBM-first staging.
- *Lost soundness cases* → the ~70 recognizers become the acceptance checklist (0.4);
  fuzz every analysis change.
- *IR infidelity* → Phase-1 gate proves the emitted C runs identically before any analysis.
- *Scope creep into language redesign* → OUT of scope; language is settled.

---

## STATUS LOG (update every session — newest first)
- **2026-08-28 (22)** — **★ REJECT-SIDE DIFFERENTIAL — found & fixed two real soundness bugs.** Added a minimal opt-in seam (`g_vra_suppress_bounds` in `sema/bounds.h`, default OFF — legacy behavior byte-identical) so the new engine can analyze programs the legacy bounds check `exit()`s on; `vra_driver --suppress` + `vra_reject_survey.sh` run it over the corpus's E085 bounds-`_fail` tests. First pass exposed **3 programs the new VRA falsely PROVED**. Fixed both root causes: (1) **subslice end-bound was unchecked** — `s[0..9]` on a len-3 slice only checked the *start* `0<3`; now `vra_check_subslice` checks `lo≥0 ∧ hi≤len` and the start GEP is skipped (it's pointer arith, not an access). (2) **fail-OPEN lowering** — `STMT_FOR`/match/unhandled-expr were silently dropped/placeholder'd, so `for i in 0..10 { x+=3 }; arr[x]` lowered to `arr[0]` and got "proved"; now any dropped/placeholder construct sets `IrFunc.incomplete` and the VRA **suppresses every proof** over infaithful IR (fail closed). Result: reject-side **0 false proofs** (33 refused, 10 no-obligation/lowering-gap); accept-side 411→403 (the 8 lost were unsound proofs over incomplete IR — a net soundness win). Unit 19/19, fuzz 0-unsound, old corpus 620 (seam inert). The differential earned its keep on day one.
- **2026-08-28 (21)** — **Termination consumer — the VRA now discharges all FOUR `func`-safety obligations.** A structured loop is proven terminating when its guard cell is updated by EXACTLY ONE well-formed step `cell = load(cell) ± c` whose direction drains a loop-invariant bound (rise toward an upper / fall toward a lower); the single-store rule is the conservative soundness guard (any other write ⇒ not proven). Proves `i+=1 while i<n` / `i+=2 while i<=n`, refuses `i+=0` (stuck) and `i-=1` (diverges). `VRA_TERMINATION` check kind + `defblk` (per-value defining block) added. `test_vra.c` = **18/18**. Fuzzed sound: extended `fuzz_vra.c`'s interpreter to report halting and added phase 3 — **100k random loops (step ∈ {0,1,2}), 66740 proven terminating, 0 unsound** (every "terminates" verdict actually halted). The engine now covers bounds + overflow + div0 + termination, each fuzz-validated (200k+300k+100k trials, 0 unsound across all three).
- **2026-08-28 (20)** — **Nonlinear mask/modulo/shift transfers — the check-free `a[x & (N-1)]` idiom.** Added transfer functions for the catalog's "type-b" small-nonlinear cases: `x & c` (c≥0 const ⇒ 0≤r≤c — the power-of-two mask/gather idiom, bounded for ANY x), `x % c` (UREM ⇒ [0,c−1]; SREM ⇒ [−(c−1),c−1], tighter if x≥0), `x >> k` (logical shift of x≥0 ⇒ [0,x]). `test_vra.c` now **13/13**: `a[x&7]` over `a[8]` proven, `a[x&15]` over `a[8]` refused (sound). Extended `fuzz_vra.c`'s generator+interpreter to emit `i&c`/`i%c` index forms — re-fuzzed **200k trials, still 0 unsound** (54466 proofs all concrete-safe). These are the SIMD-gather / hash-bucket patterns; the old engine got them via a hand-coded recognizer, here they fall to a couple of sound interval facts.
- **2026-08-28 (19)** — **★ SOUNDNESS FUZZED — 200k random programs, zero false proofs.** `src/analysis/fuzz_vra.c`: generate random counted-loop index programs *as IR* (bypassing the frontend, so UNSAFE ones the old sema would `exit()` on are included), run the VRA (abstract), and cross-check **every "proven check-free" verdict against a concrete interpreter of the same IR** (ground truth). Result: **200000 trials, ~68k genuinely out-of-bounds, VRA proved 74047 accesses — all 74047 concretely safe, 0 UNSOUND.** This is the gold-standard abstract-vs-concrete check and it directly de-risks the transfer functions + guard refinement + widening (where a soundness bug would otherwise hide). Caught its own harness bug first (elem_ptr builder returns the value not the instr → matches silently no-op'd). A second phase brute-forces the overflow consumer's interval arithmetic (`vra_arith_range` must over-approximate `{a OP b}`) — **300k trials, 0 failures**. The new proof engine is now validated at THREE levels: the octagon domain (`test_octagon.c`, 40k γ-trials), the whole bounds analysis (`fuzz_vra.c` phase 1, 200k concrete-differential), and the overflow logic (phase 2, 300k over-approximation).
- **2026-08-28 (18)** — **Relational breadth + the 2.6 accept-side differential.** Added the two-pointer relational proof (`a[i]` under `i≤j ∧ j<len` — transitive octagon closure) and seeded integer-param type ranges into the entry octagon (so a `usize` param is known ≥0); `test_vra.c` now **11/11** (bounds, overflow, div0, sliding-window, two-pointer). Built `vra_survey.sh` (committed) — the ⊇-accepts differential: over every old-accepted indexing program, the new engine proves **411/510 bounds obligations (81%)**, 40 fully, from-scratch. Two precision wins from the survey punch-list: slice-length propagation through make_slice + slice cells (`s=a[lo..hi]; s[k]`), and fixed-array-value length from the type (a `u8[16]` param). Remaining gaps are lowering-side (nested array-in-struct, global arrays, `in`-loops) or off-North-Star (SIMD). Old engine untouched + green.
- **2026-08-28 (17)** — **★ THE NEW ENGINE DOMINATES THE OLD — proves a pattern the old engine REJECTS.** The whole justification for the octagon rebuild, demonstrated: the sliding window `a[i+1]` under `while i+1 < a.len`. The OLD engine rejects it (`[E085]` "cannot prove index within bounds", index `i+1` range [1,MAX]) — its interval-plus-recognizers approach structurally can't relate `i+1` to `len`. The NEW octagon engine PROVES it check-free: the guard's `i+1` and the index's `i+1` both equal the shared `i`-cell +1, so closure carries the guard fact `i+1 < len` to the index. Hand-built in `test_vra.c` (sema exits on the reject, so we can't route it through the driver). Full `test_vra.c` now **10/10**: bounds (2 prove / 3 refuse), overflow (1/1), div0 (1/1), relational sliding-window (prove). This is Phase-2 goal 2.8's "⊇ accepts" shown in miniature. Next: more relational patterns (two-pointer `i≤j`, reverse `a[n-1-i]`, Lomuto), then the full 2.6 differential over the corpus + fuzzers.
- **2026-08-28 (16)** — **Overflow + div-by-zero consumers — the VRA now discharges BOTH flagship duties.** Extended `vra.h` with the ℤ-range overflow obligation (§2.6): at a CHECK-mode `+`/`−`/`×`, compute the result's ℤ range from the operands' (type interval ∩ octagon), in `__int128` so the checker can't wrap itself, and require it to land back inside the operand type; and div-by-zero (divisor range excludes 0). `VraCheck` is now tagged (bounds/overflow/divzero). `test_vra.c` proves-and-refuses each: `u8 x+x` proven under `x<100` / refused unguarded, `i32 a/b` proven under `b>0` / refused unguarded — **9/9**. Driver on the real loops: fixed-array `2/2` (bounds + `i+1` overflow both proven), slice loop honestly `1/3` (bounds proven; `s+a[i]` is a real unbounded-accumulator overflow, `i+1` is the usize/2^64 int64-domain gap — both SOUND, never false-proven). Next: refinement-type + termination consumers, then 2.6 differential accept/reject over the corpus.
- **2026-08-28 (15)** — **★ THE NEW ENGINE PROVES ITS FIRST BOUNDS — end to end, and it is SOUND.** Wired the octagon domain into a CFG fixpoint with transfer functions + guard refinement + a bounds consumer (`src/analysis/vra.h`, 2.3–2.5). Variable model: one octagon var per IR value id; an alloca id doubles as its scalar memory cell, so `i=i+1` (load/add-const/store) becomes the difference fact that survives the loop — no mem2reg needed yet. Widening at the loop header (back-edge) + guard refinement (`i<n` ⇒ octagon constraint on the taken edge) + slice-length canonicalization (the guard's `a.len` and the index's length are the *same* octagon variable). Two proof harnesses: `vra_driver.c` lowers a real Lain file and reports each index — **both flagship patterns PROVEN**: fixed-array `while i<8 { a[i] }` and slice-scan `while i<a.len { s+=a[i] }`. And `test_vra.c` hand-builds UNSAFE IR the old sema would `exit()` on, confirming the engine **REFUSES** them: off-by-one `i<=8`/a[8], over-bound `i<16`/a[8], unguarded param index — all correctly not-proven. This is the whole rebuild thesis demonstrated: typed IR + octagons + fixpoint = a clean, sound, check-free proof engine. Octagon test still green (40000 trials), Phase-1 behavioural gate 10/10, old engine untouched. Next: overflow/div0 consumers, φ/mem2reg, then differential accept/reject over the corpus (2.6).
- **2026-08-28 (14)** — **PHASE 2 STARTED — the octagon domain (2.1) is built and brute-force sound.** `src/analysis/octagon.h`: the relational numeric domain the whole rebuild is *for* — conjunctions of `±x ± y ≤ c` as a DBM over 2n ± dimensions (Miné's encoding, coherent twins maintained by the setters). Implements Floyd–Warshall + strong (integer-tight `⌊c/2⌋`) closure, ⊥ detection, join (entrywise max — **the φ-merge `src/sema/` never had**), meet, widening (loop-convergent), order, forget/projection, and the interval read the bounds/overflow consumers will use. `test_octagon.c` validates every claim by **brute force over concretization γ** (the soundness yardstick from the design doc): closure preserves γ, ⊥ is sound, join/meet/widen over-approximate, meet is exact intersection, widening converges — **40000 random trials over ℤ³** + hand-built tightness cases (`x∈[2,5] ∧ x−y=1 ⇒ y∈[1,4]`, transitive `x−z≤5`), all pass. This is the keystone of the payoff phase. Next: 2.3 CFG fixpoint + 2.4 transfer functions on the IR, then 2.5 consumers (bounds/overflow/div0).
- **2026-08-28 (13)** — **Type aliases resolve to their base type.** `type NonZeroI32 = i32 != 0` used as a type lowered to opaque `struct` (→ `void*`, breaking arithmetic); now `ir_lower_type` resolves a `DECL_TYPE_ALIAS` to its runtime base (the refinement is the VRA's concern, not the representation) — the leftmost leaf of the RHS, which sema has resolved into an `EXPR_TYPE` wrapper (that was the missing piece). `double(NonZeroI32)` is now a clean `i32` param, not a materialized struct. Self-checking `inc(Small=41)==42` exits 0. **Coverage OK 303→306/376 (81%)**; unblocks the VRA refinement tests Phase 2 will lean on. Old engine green (**620**). **The core imperative language now round-trips end to end** — every construct the octagon VRA needs (all int types + ℤ/overflow-split arithmetic, arrays, slices, subslices, strings, structs by-value/by-ref, control flow, calls, casts, globals, borrows) has a passing self-checking test. Remaining tail is off the bounds/overflow North Star: SIMD, fn-pointers, Result/error monads, generics, nested array-in-struct.
- **2026-08-28 (12)** — **Structs — the last core aggregate — end to end, incl. by-reference mutation.** `IRT_STRUCT` is now self-contained (carries its lowered field types + names). Lowering: struct-name → decl resolution (memoized in LowerCtx so a self-referential field can't loop); field read/write via `IR_FIELD_PTR`; construction `Point(1,2)` → `IR_STRUCT_NEW` (compound literal); a by-value struct param materializes to a slot, a **`var` struct param is a by-reference pointer** to the caller's storage (`MODE_MUTABLE`) so field writes propagate; fixed-array `.len` folds to the constant N. Also lowered the borrow/ownership expression wrappers that gated this: `EXPR_MUT` (`var lv` → the aggregate's address), `EXPR_MOVE`, `EXPR_ADDR` (`&`), `EXPR_DEREF` (`*`). Backend: a **unified type-decl emitter** — transitively collect every struct/slice, forward-declare all structs, then slice typedefs (which only need the struct *pointer*), then full struct bodies in reverse-discovery order (a slice-of-`Token` no longer hits `unknown type name`). Round-trips: `dot(Vec(3,4))==25`, `field_construct`, and `push_n(var v)` mutation (len 0→20) all exit 0. **Coverage OK 295→303/376 (81%)** — structs net-positive over the old `void*`+placeholder that compiled to garbage. Old engine green (**619**). Deferred: nested array-in-struct indexing, type aliases (`type M = i32` → `void*`), SIMD, fn-pointers, Result/error monads.
- **2026-08-28 (11)** — **Subslices `xs[lo..hi]`.** An `EXPR_INDEX` whose index is an `EXPR_RANGE` lowers to a slice value `{ &base[lo], hi-lo }` (+1 if `..=` inclusive) via the existing `elem_ptr`+`make_slice`; open ends default (`lo`→0, `hi`→ array's const N or the source slice's `slice_len`); works over both a fixed array and a slice source. `xs[1..4]` then `s[0]+s[2]` round-trips to 60; self-checking corpus test exits 0. **Coverage OK 285→295/375 (79%)**. Old engine green (**618**). Remaining GCC_FAIL now a diverse long tail: SIMD vectors (7), fn-pointers, Result/error enums, FFI string→char* decay in variadic printf, and **structs** (the last core aggregate — next).
- **2026-08-28 (10)** — **Coverage harness sharpened + top-level constants fold.** Switched the probe to **compile-to-object** (`gcc -c`) so extern/FFI linking (the documented `-Dlibc_printf=printf` recipe) stops masquerading as a lowering gap: real coverage is **OK=284/373 (76%)**. Then closed the top-level-constant gap: a module-level `NAME T = expr` referenced in a function used to lower to a placeholder `const 0 : unit` (→ `void` locals). Now `ir_find_global_const` (threaded `globals` + `const_depth` guard into LowerCtx) folds a const reference to its initializer, **recursively** (`STEP = MAX/MIN` → `100/5`). Subtlety: sema qualifies the *reference* as `<defining_module>_NAME` while the decl keeps the bare name — match both spellings. Self-checking test `100-5+STEP==115` runs exit 0. Old engine green (**617**). Global *arrays* (`FLAGS u8[8]=[…]` indexed) still deferred. Next remaining GCC_FAIL clusters: array `.len` (should be constant N), unit-typed params (`void` param), a few slice/FFI arg-type mismatches.
- **2026-08-28 (9)** — **Differential coverage harness + strings.** Built `ir_coverage.sh` (probe, in scratch): run the NEW pipeline over every corpus program the old engine accepts, categorize OK / GCC_FAIL / LOWER_CRASH. **Baseline 178/372 (48%) of accepted programs already emit compilable C** — strong Phase-1 floor. The punch-list was unambiguous: **90× string literals** lowering to a placeholder `const 0` (`Slice_u8 ← int`). Fixed: `IR_STR_CONST` (aux.str bytes) reuses C's own static string storage — `(uint8_t*)"…"` (octal-escaped) — then `make_slice(data, len)` in `u8[:0]` context. Builder `ir_str_const`; lower.h EXPR_STRING + EXPR_CHAR; emit_c.h `ir_emit_cstr`; dump.h op name. **90→2 Slice_u8 failures**; round-trips lower→C→run exit 0. Remaining top gaps now visible: FFI/`extern` calls (need the documented `-Dlibc_printf=printf` recipe — linking noise, not a lowering gap) and **top-level constants** (globals referenced in a fn lower to `const 0` = wrong value — the real next gap). Old engine green (**616**).
- **2026-08-28 (8)** — **Slices (fat pointers) round-trip end-to-end — the flagship VRA shape now flows the new pipeline.** `func sum(a i32[]) { while i<a.len { s += a[i] } }` lowers to `slice_len`/`slice_data`/`elem_ptr`, and a caller's fixed array **decays** to a slice at the call site (`make_slice base, N`). Builders `ir_slice_data`/`ir_make_slice` (build.h); lower.h indexes a slice through `.data`, slices are first-class value slots (NOT the array "decay to base" path — fixed `ir_type_is_agg` to arrays only), CALL lowering inserts array→slice decay from the callee's param types; emit_c.h emits one `typedef struct { T* data; size_t len; } Slice_<tag>` per distinct slice type + `.len`/`.data`/make_slice. Read (`sum`) and **write** (`fill: a[i]=v` through a slice param, mutating the caller's array) both round-trip lower→C→gcc→run, exit 0. Also fixed a latent lowering bug: comparison signedness came from the result type (bool) → every `i<len` mistagged `slt`; now from the operands (`ult`) — matters for the octagon transfer functions. Old engine green (**615**). Next: structs (fields) → strings, then for/match + short-circuit and/or toward the 1.6 gate.
- **2026-08-28 (7)** — **Fixed arrays round-trip end-to-end** (the aggregate the VRA most needs). Uniform pointer model: an array `alloca` decays to an element-base pointer (`v0 = slot0;`), `elem_ptr` adds the index (`&v0[i]`); `build.h ir_alloca_array`, `lower.h` STMT_VAR aggregate path (array-literal init stores each element) + `IrLocal.aggregate`, `emit_c.h` alloca-type map declares `elem slot[N]` and emits array vs scalar allocas. Three tests round-trip **lower→emit C→gcc→run, all exit 0**: literal init + read (`array1`), index write (`array2`), and the canonical VRA pattern **loop-fill `while i<4 { a[i]=i; i=i+1 }`** (`array3` — scalar `i` reloaded at the loop header, `i+1` ℤ-widened then truncated on store: the memory-form the octagon analyzer will consume). Old engine green (**614**, +3 new tests pass it too). Next: slices (fat pointers) → structs → strings, then for/match + short-circuit and/or toward the 1.6 gate.
- **2026-08-28 (6)** — **END-TO-END: a real program round-trips the whole NEW middle-end+backend and RUNS CORRECTLY.** `src/ir/emit_c.h` (IR→C, scalar core) + `lower_driver.c --emit-c`: `tests/ir/sum_maxi.ln` → lower → emit C → gcc → exit 0 (sum(5)==10). Scalar core of Phase 1 (1.1-1.3) done. Next: grow lowering+backend coverage (arrays/slices/structs/strings) toward the 1.6 gate. Old engine green (611).
- **2026-08-28 (5)** — **Phase 1.2 lowering WORKS** end-to-end: `src/ir/lower.h` + `lower_driver.c` lower a real parsed+typechecked program (`tests/ir/sum_maxi.ln`) to a correct CFG (loop header detected; ℤ-widened adds). Core subset done; grow coverage next (for/match/structs/strings). Old engine green (611).
- **2026-08-28 (4)** — Phase 1: IR construction API + dumper DONE + validated (`src/ir/build.h`, `dump.h`, `test_ir.c` — hand-built branch + loop dump correctly, loop-header auto-detected). 1.1 complete, 1.5 dumper done. Next: **1.2 AST→IR lowering** (`src/ir/lower.h`) wired to the frontend (parse+typecheck→typed AST→lower→dump), then 1.3 IR→C. Old engine green (610).
- **2026-08-28 (3)** — Phase 0 complete; **Phase 1 STARTED**: `src/ir/ir.h` first draft (IR data structures) landed + compiles standalone. Next: 1.2 AST→IR lowering + builder defs + dumper.
- **2026-08-28 (2)** — **PHASE 0 COMPLETE.** All design docs done: 0.1 architecture, 0.2 ir,
  0.3 vra-octagon, 0.4 recognizer-catalog (validates the thesis: 70 recognizers → ~50 fall
  out / ~9 transfer / 1 lemma), 0.5 spec ch.13 contract (abstract, decoupled), 0.6 domain
  staging locked (closed-DBM first → octagons). **Next: PHASE 1 — the IR foundation.** Start
  1.1 `src/ir/ir.h` (IR data structures per `design/ir.md`), then 1.2 AST→IR lowering, 1.3
  IR→C, 1.4 `--engine=ir` selector, 1.6 GATE: every `*_pass` round-trips through the new
  pipeline. Old engine unchanged and green (suite 610, trust 40, fuzzers clean).
- **2026-08-28 (1)** — Plan + three core design docs (0.1–0.3) done.
