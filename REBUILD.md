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

- [ ] **2.1 Octagon domain** (`src/analysis/octagon.h`): DBM representation, incremental +
      full closure, join, meet, widening, projection, is-bottom. Unit-tested in isolation.
- [ ] **2.2 Variable packing** strategy (cluster vars that co-occur in constraints/indexing).
- [ ] **2.3 Fixpoint engine** over the CFG: worklist, widening at loop headers, narrowing.
- [ ] **2.4 Transfer functions** per IR instruction (assign, arithmetic, φ, branch/guard
      refinement, calls incl. return refinements).
- [ ] **2.5 Consumers:** bounds check (index < len), overflow, div-by-zero, refinement types,
      termination measure (now a natural fixpoint by-product).
- [ ] **2.6 Differential harness** (`diff_engines.sh`): old vs. new accept/reject over the
      whole corpus + fuzzers; classify + report divergences.
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
