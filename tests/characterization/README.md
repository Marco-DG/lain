# Characterization tests — safety net for the coherence refactor

These tests lock the **current, verified behavior** of the core mechanisms that the
coherence refactor (`internal/design/deep_analysis.md`) will rewrite. They are the
regression oracle: the rewrite MUST keep them green, or update them with an explicit
rationale. This is how the 3 years of convergence are preserved — as behavior, not as code.

Snapshot of the pre-refactor tree: git tag `v0-pre-coherence`, branch `archive/pre-coherence`.

## What each test locks

| Test | Locks |
|---|---|
| `char_use_after_move_fail` (E001) | use of a linear variable after `mov` is rejected |
| `char_move_needs_mov_fail` (E007) | passing a linear value to an owned param needs explicit `mov` |
| `char_consume_before_return_fail` (E003) | an owned linear param must be consumed before return |
| `char_two_shared_same_call_pass` | two simultaneous shared borrows of the same var are allowed (N readers) |
| `char_mut_plus_nested_shared_fail` (E004) | mutable borrow + nested-call shared borrow of the same var = aliasing violation |
| `char_recv_plus_shared_pass` | two-phase borrow: mutable receiver + direct shared arg of same var compiles |
| `char_dependent_array_pass` | value-dependent array sizing `i32[out.len]` — the DML feature that MUST survive Fase 0 |
| `char_nonlinear_index_fail` (E085) | non-linear index (`r*cols+c`) can't be proven in bounds (Fourier-Motzkin is linear) |
| `char_overflow_static_fail` (E086) | statically-provable overflow into a narrower type is rejected |
| `char_decreasing_ok_pass` | a well-formed `decreasing` measure is accepted |

## Known P0 bugs — NOT yet encoded as tests (kept out to keep the suite green)

Ready to become **passing** tests once fixed (see `internal/design/deep_analysis.md`):

1. **Termination soundness.** This is wrongly ACCEPTED but never terminates when `flag <= 0`:
   ```
   func loopy(n i32, flag i32) i32 {
       var i = 0
       while i < n decreasing n - i {
           if flag > 0 { i = i + 1 }
       }
       return i
   }
   ```
   Root: `measure_scan_body` (`src/sema.h`) accepts "some assignment decreases, none
   increases" instead of "strictly decreases on every path". After the fix this should be
   rejected (E082) → becomes `char_decreasing_per_path_fail.ln`.

2. **`Option(*T)` compiler crash.** Matching an Option-like enum with a pointer payload and
   consuming the binding with `mov` aborts the compiler (`type_move(NULL)`, `src/ast.h:582`).
   After the fix (or after Fase 0 removes generic Option), the niche × linearity × match
   interaction must not crash → becomes `char_option_ptr_no_crash.ln`.

## Blast radius of Fase 0 (cutting comptime/generics/Option-Result/niche)

~22 existing tests are tied to the cut stack and will be removed or updated in Fase 0:
`tests/niche/` (15), `tests/comptime_*` (5), `tests/generics*` (2). The ~245 core tests
(ownership, borrow, bounds, vra, refinement, overflow, types) survive unchanged.
