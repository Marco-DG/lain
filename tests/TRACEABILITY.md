# Lain conformance-suite — traceability & alignment

_Tests = the live conformance suite for `spec/` (the contract); the compiler is one
implementation. One dir per spec chapter → traceability is structural. Reconcile spec↔
compiler in sprints, tests as arbiter, **language philosophy as the judge** (flag a
mismatch, understand why, decide fix-compiler vs correct-spec, ask when intent is unclear)._

## Naming law (enforced)

- `feature_scenario_pass.ln` / `feature_scenario_fail.ln` — every standalone test.
- Every `_fail` carries `// EXPECT: [EXXX]`. Target: a `// spec: §N` header per test.
- No `char_` prefix — the directory names the feature.
- **Exempt** (not pass/fail tests, names must stay stable): *fixtures* (imported helper
  modules, e.g. `stdlib/dummy.ln`) and *snapshots* (`codegen/*` with a `.grep` sidecar).

## Structure & coverage (one dir ↔ one spec chapter)

| dir | spec § | tests | status |
|:--|:--|--:|:--|
| `types/` | 07 | 71 | ok |
| `expressions/` | 08 | 11 | ok |
| `statements/` | 09 | 8 | ok |
| `declarations/` | 10 | 12 | ok |
| `ownership/` | 11 | 88 | ok |
| `functions/` | 12 | 28 | ok |
| `vra/` | 13 | 116 | ok |
| `generics/` | 14 | 13 | ok |
| `match/` | 15 | 28 | ok |
| `modules/` | 16 | 2 | **thin — add tests** |
| `unsafe/` | 18 | 3 | **thin — add tests** |
| `memory/` | 19 | 20 | ok |
| `stdlib/` | 20 | 14 | ok |
| `codegen/` | — | 7 | ok |
| `examples/` | — | 18 | ok |

**439 tests.** Gaps to fill (write tests): `interop`(§17)=0, `modules`(§16) & `unsafe`(§18) thin. Also `lexical`(§05), `basics`(§06) have no dedicated dir yet.

**23 `_fail` tests still missing `// EXPECT:`** (next hygiene pass):
- `tests/examples/simd_vec_lane_oob_fail.ln`
- `tests/functions/func_calls_proc_fail.ln`
- `tests/functions/func_self_recursion_fail.ln`
- `tests/functions/termination_fail.ln`
- `tests/ownership/double_consume_fail.ln`
- `tests/ownership/e087_cross_param_fail.ln`
- `tests/ownership/e087_literal_bound_fail.ln`
- `tests/ownership/elif_borrow_fail.ln`
- `tests/ownership/move_then_use_fail.ln`
- `tests/types/exhaustive_fail.ln`
- `tests/types/sub_slice_bounds_fail.ln`
- `tests/types/unsafe_adt_fail.ln`
- `tests/unsafe/unsafe_deref_fail.ln`
- `tests/vra/bounds/bounds_fail.ln`
- `tests/vra/bounds/dynamic_len_known_idx_fail.ln`
- `tests/vra/bounds/equation_constraints_fail.ln`
- `tests/vra/bounds/in_condition_fail.ln`
- `tests/vra/bounds/in_keyword_fail.ln`
- `tests/vra/bounds/range_assign_check_fail.ln`
- `tests/vra/bounds/range_if_fail.ln`
- `tests/vra/bounds/return_constraints_fail.ln`
- `tests/vra/guards/branch_relation_join_fail.ln`
- `tests/vra/guards/while_no_measure_in_func_fail.ln`

## Reconciliation backlog (findings)

Tracked Tests↔Spec↔Compiler mismatches, for the chapter-by-chapter pass:

- **`comptime_if` / `test_mem`** (`GCC_CHECK_SKIP`) — sentinel `*u8[:0]` printf arg emits fat
  `Slice_u8_0` not `const char*`; `std/mem` `malloc` binding conflict. §17.
- **`Option(*T)` + `mov` in `match`** may crash (`type_move(NULL)`, per the old char README).
  Verify against current niche×linearity×match; if live → `match/niche/option_ptr_mov_no_crash_pass`. §15.
- **Near-duplicates to review for pruning** (disambiguated, not yet merged):
  `ownership/{use_after_move_fail, use_after_move_drop_fail}`,
  `ownership/{defer_consume_pass, defer_consume_scope_pass}`,
  `memory/{uninit_fail, uninit_basic_fail}`.
- **Resolved**: single-path `decreasing` non-termination now correctly rejected `[E082]`
  (was an open P0 in the old characterization README).

**Context**: the former `char_*` tests are *regression oracles* — they lock hard-won soundness
behavior. The reorg co-located each with its feature and dropped the opaque prefix; the value is
the assertion, now findable by feature. Pre-reorg snapshot: tag `v0-pre-coherence`.


## Reconciliation backlog — additions (hygiene pass)

- **Diagnostic-code inconsistency**: 6 `_fail` tests fail with an **un-coded** error
  (a bare `sema error:`, `Error: Index out of bounds`, or `[VRA]` tag) instead of a
  `[EXXX]` code, so they can't carry a `// EXPECT: [E…]`. The error taxonomy should be
  uniform — every rejection gets a code. Affected:
  `examples/simd_vec_lane_oob_fail` (bounds/vector → should be E085-family),
  `functions/{func_self_recursion_fail, termination_fail}` (purity/termination),
  `types/{exhaustive_fail, unsafe_adt_fail}` (match exhaustiveness; ADT access),
  `vra/bounds/in_keyword_fail` (bounds → E085). Fix in reconciliation (§14-errors),
  then add the EXPECT tags.
- **Naming**: `stdlib/math_test_pass` overlaps `math_pass` + `math_smoke_pass` (3 math
  tests) — review/merge in the pruning pass.
- **Snapshot infra fixed**: the reorg moved `emit/` → `codegen/` but `run_tests.sh`
  hardcoded `tests/emit/*`, silently disabling 7 snapshot assertions. Handler now keys
  off the `.grep` sidecar (dir-independent); stale `tests_emit_` mangled prefixes updated.

## Reconciliation log — §11 ownership + diagnostics (sprint 1)

- **§11 conformance check**: spec E001–E008, E016 all have tests except **E006**
  (move-in-loop) — gap to fill (compiler DOES emit it, linearity.h:490). `E010`
  (dangling) lives in §19-memory, not §11 — spec is fine, just split. `E087`
  (dependent-length mismatch) is mis-homed in `ownership/` → belongs in `vra/`/`types/`.
- **Diagnostic-taxonomy consistency (fixed 5/6)**: uncoded rejections given codes —
  constant-index & vector-lane bounds → `[E085]` (were bare `Error:` / `[VRA]`);
  `non-exhaustive match` → `[E014]` (one site emitted it, one didn't); recursion-in-`func`
  → `[E011]` (func-totality family). EXPECT tags added. **Remaining**: `types/unsafe_adt_fail`
  — "direct ADT field access requires unsafe" still bare; needs a fresh code + annex entry.

## Reconciliation log — §17 C-string interop (sprint 2)

- **FIXED**: a null-terminated `u8[:0]` (or `*u8[:0]`) at an extern C boundary now
  emits a thin `const char*`/`char*`, not the fat `Slice_u8_0` struct. This was the
  `comptime_if` finding (`printf(fmt *u8[:0], …)` conflicted with C's `printf`); the
  emitter's `*u8`→`const char*` hack only matched TYPE_SIMPLE elements, so a sentinel
  *slice* fell through. `comptime_if_pass` now gcc-compiles and is un-skipped.
- **Remaining §17/§20**: `mem_smoke_pass` (`std/mem` malloc binding vs system malloc).
