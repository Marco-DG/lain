# Analysis 14 — Strategic Roadmap: From Borrow Checker to Complete Language

**Date**: 2026-03-05
**Compiler LOC**: ~13,000 (core C99)
**Tests**: 85+ (45 counted by runner, all pass)
**Borrow Checker Progress**: 15/27 features (56%)

---

## 1. Current State Assessment

The Lain compiler has reached a strong foundation. The core language features work, the borrow checker is sound for the implemented cases, and the test suite demonstrates comprehensive coverage. However, to reach "perfection" as a language, the focus needs to shift from **incremental borrow checker features** toward **language completeness and usability**.

### 1.1 What Works Well

| Area | State | Notes |
|------|-------|-------|
| Core syntax (func/proc/type/if/for/while/case) | Solid | All basic constructs functional |
| Ownership system (mov/var/shared) | Solid | Intra-expr, cross-stmt, NLL, per-field |
| Borrow checker (Phases 1-5) | Solid | Re-borrow, transitivity, defer consumption |
| Type constraints & VRA | Solid | Parameter constraints, return constraints, `in` bounds |
| Pattern matching & exhaustiveness | Solid | Enums, ADTs, ranges, character matching |
| Comptime generics (Phase A-B) | Working | `Option(int)`, `max(int, a, b)`, type-level functions |
| C interop | Working | extern func/proc, c_include, opaque types |
| Module system | Working | import with dot notation, namespace aliasing |
| Defer / RAII | Working | LIFO, early return, break/continue integration |
| Definite initialization | Working | Flow-sensitive, branch-aware |
| Unsafe blocks | Working | Pointer deref, address-of, linearity still enforced |

### 1.2 What Needs Work

| Area | Severity | Impact |
|------|----------|--------|
| **Error messages** — minimal, no source context | High | Bad DX |
| **Emit quality** — hardcoded C function overrides, buffer issues | Medium | Fragility |
| **No `elif` borrow checking** — elif chains not fully traced | Medium | Potential unsoundness |
| **Immutability model** — `var x = 42` being immutable is confusing | Medium | Usability |
| **No array literals** — `[1, 2, 3]` not supported | Medium | Missing feature |
| **No string operations** — only raw byte access | Medium | Usability |
| **Borrow checker remaining features** — see Phase 6-9 | Low-Medium | Completeness |
| **No closures** | Low | Missing feature (long-term) |
| **No traits/interfaces** | Low | Missing feature (long-term) |

---

## 2. Strategic Decision: What Matters Most

The borrow checker is the **crown jewel** of Lain — it's what makes the language unique. But spending all effort on Phases 6-9 (two-phase borrows, CFG-based NLL, closure capture) yields diminishing returns. These are precision improvements that reject fewer valid programs, but the language already accepts the vast majority of correct ownership patterns.

**Recommendation**: Split effort between three tracks:

| Track | Weight | Goal |
|-------|--------|------|
| **A: Language Completeness** | 40% | Make Lain usable for real programs |
| **B: Compiler Quality** | 30% | Error messages, robustness, refactoring |
| **C: Borrow Checker Hardening** | 30% | Targeted soundness/precision wins |

---

## 3. Roadmap

### Phase 6 — Compiler Quality & Bug Fixes (Priority: NOW)

These are small, high-impact improvements.

#### 6.1 Fix `elif` Borrow Checking

**Problem**: The current `STMT_IF` handler processes `then_branch` and `else_branch`, but `elif` chains may not properly thread the borrow/linearity state through each condition-body pair.

**Verification needed**: Write a test with `elif` that exercises borrow checking across branches:
```lain
var data = Data(42)
var ref = get_ref(var data)
if false {
    use(var ref)
} elif true {
    use(var ref)     // Is ref tracked here?
} else {
    use(var ref)
}
read(data)           // Should work: ref consumed in all branches
```

**Estimate**: ~2 hours investigation + fix

#### 6.2 Compiler Warning Cleanup

The compiler builds with several warnings:
- `unused variable 'original_mode'` in `generic.h`
- `unused variable 'base_name'` in `typecheck.h`
- Unhandled `DECL_EVAL_IMPORT` in `resolve.h` switch
- `snprintf` truncation warning in `resolve.h`

**Estimate**: ~30 minutes

#### 6.3 Static Buffer Safety Audit

The compiler uses many `char buf[256]` / `char buf[512]` static buffers for name mangling and string operations. While `c_name_for_id` was increased to 1024, other locations may still overflow with long module paths.

**Action**: Audit all static buffers in emit/*.h, sema/resolve.h, sema/linearity.h. Replace critical ones with arena-allocated buffers.

**Estimate**: ~2 hours

---

### Phase 7 — Error Message Revolution (Priority: HIGH)

The single highest-impact improvement for developer experience.

#### 7.1 Source Span Tracking

**Current**: Errors show `Ln X, Col Y` with a message.
**Target**: Show the source line, underline the offending code, show related locations.

```
error: cannot access 'data' because it is mutably borrowed by 'ref'
  --> main.ln:5:20
   |
3  | var ref = get_ref(var data)
   |                  -------- mutable borrow occurs here
5  | var x = read_data(data)
   |                   ^^^^ shared borrow attempted here
```

**Implementation**:
1. Store source file content (or file path + lazy loading) accessible during error reporting
2. Add a `DiagnosticBuilder` that collects primary + secondary spans
3. Format output with source context

**Estimate**: ~300 lines, 1-2 days

#### 7.2 Error Codes

Assign unique error codes to each diagnostic category:
- `E001`: Use after move
- `E002`: Borrow conflict (shared + mutable)
- `E003`: Linear variable not consumed
- `E004`: Constraint violation
- `E005`: Uninitialized variable
- etc.

This enables `--explain E001` in the future.

**Estimate**: ~100 lines

---

### Phase 8 — Language Feature Completion (Priority: HIGH)

#### 8.1 Array Literals

**Current**: Arrays must be initialized element-by-element.
**Target**:
```lain
var arr = [1, 2, 3, 4, 5]     // Type: int[5], inferred
var bytes u8[4] = [0xFF, 0x00, 0x80, 0x01]
```

**Implementation**:
- Parser: Parse `[expr, expr, ...]` as `EXPR_ARRAY_LITERAL`
- TypeCheck: Infer element type from first element, verify homogeneity, set array size
- Emit: Generate `{1, 2, 3, 4, 5}` C initializer

**Estimate**: ~150 lines across parser/typecheck/emit

#### 8.2 Shift Operators in VRA

Check if `<<` and `>>` are properly handled by the Value Range Analysis system. They are important for embedded/systems work.

**Estimate**: ~30 lines in `ranges.h`

#### 8.3 Nested Type Declarations (verify)

The spec mentions `type Token.Kind { ... }`. Verify this works correctly and add tests if missing.

#### 8.4 `else` in `for` Loops (Future consideration)

Some languages support `for ... else` (execute else if loop body never ran). Lain's spec doesn't include this, but it could be useful. Low priority.

---

### Phase 9 — Borrow Checker Hardening (Priority: MEDIUM)

These are the remaining items from the borrow_checker.md roadmap, ordered by impact.

#### 9.1 Two-Phase Borrows (Highest Impact)

**Problem**: `x.method(x.field)` fails because UFCS expands to `method(var x, x.field)` and the mutable borrow of `x` blocks the shared access to `x.field`.

**Implementation**: Add a `phase` field to `BorrowEntry`:
- `PHASE_RESERVE`: Mutable borrow reserved but not activated. Shared reads allowed.
- `PHASE_ACTIVE`: First write through the borrow activates it. No more shared reads.

```c
typedef enum { BORROW_RESERVED, BORROW_ACTIVE } BorrowPhase;
```

**Estimate**: ~200 lines in `region.h` + `linearity.h`

#### 9.2 Block-Level NLL

**Problem**: Borrows created inside `if`/`else` branches leak to the parent scope.

**Current behavior**: `borrow_exit_scope()` now releases scope-local borrows (implemented in Phase 7.1 per region.h:177-194). This may already work — needs testing.

**Action**: Write tests to verify block-level borrow scoping works:
```lain
var data = Data(42)
if condition {
    var ref = get_ref(var data)
    use(var ref)   // ref dies here
}
read(data)         // Should work: ref's borrow expired with if-scope
```

**Estimate**: Possibly 0 lines (already implemented), ~2 hours testing

#### 9.3 Non-Consuming Match

**Problem**: `case shape { Circle(r): ... }` consumes shape. Need `case &shape` for inspection without consumption.

**Implementation**:
- Parser: Accept `&` before case scrutinee
- Linearity: Skip consumption of scrutinee in `&` mode
- Emit: Generate read-only access instead of destructuring

**Estimate**: ~150 lines

#### 9.4 Defer Borrow Verification

**Problem**: Multiple defers might conflict with each other at execution time (LIFO order).

**Implementation**: After processing all statements, simulate defer execution in LIFO order on the linearity table, checking for conflicts.

**Estimate**: ~80 lines

---

### Phase 10 — Standard Library Foundation (Priority: MEDIUM-HIGH)

#### 10.1 `std/math.ln`

```lain
func min(a int, b int) int { if a < b { return a } return b }
func max(a int, b int) int { if a > b { return a } return b }
func abs(x int) int >= 0 { if x < 0 { return 0 - x } return x }
func clamp(x int, lo int, hi int >= lo) int >= lo and <= hi {
    if x < lo { return lo }
    if x > hi { return hi }
    return x
}
```

#### 10.2 `std/string.ln` (basic)

Using C interop:
```lain
extern func strlen(s *u8) usize
extern func strcmp(a *u8, b *u8) int
extern func memcpy(dst var *void, src *void, n usize) var *void
```

#### 10.3 `std/mem.ln` (arena allocator)

A simple bump allocator wrapping malloc:
```lain
type Arena {
    mov base *void
    used usize
    capacity usize
}
```

---

### Phase 11 — Advanced Language Features (Priority: FUTURE)

| Feature | Complexity | Dependencies |
|---------|-----------|--------------|
| Generic `Option(T)` / `Result(T, E)` | Medium | Comptime Phase B (already working) |
| `?` error propagation operator | High | Generic Result type |
| Closures with capture modes | Very High | New AST nodes, borrow capture tracking |
| Traits / Interfaces | Very High | Design decision needed |
| `..=` inclusive ranges | Low | Parser change |
| Multiple return values `(int, int)` | Medium | Tuple type needed |

---

## 4. Recommended Execution Order

### Sprint 1 (Immediate): Compiler Quality
1. Fix compiler warnings (6.2)
2. Static buffer audit (6.3)
3. Verify `elif` borrow checking (6.1)
4. Verify block-level NLL already works (9.2)
5. Write stress tests for edge cases

### Sprint 2 (Short-term): Developer Experience
6. Array literals (8.1)
7. Error message improvements — add source line display (7.1)
8. Error codes (7.2)

### Sprint 3 (Medium-term): Borrow Checker Completeness
9. Two-phase borrows (9.1)
10. Non-consuming match (9.3)
11. Defer borrow verification (9.4)

### Sprint 4 (Medium-term): Standard Library
12. `std/math.ln` (10.1)
13. `std/string.ln` (10.2)
14. `std/mem.ln` arena allocator (10.3)
15. Generic `Option(T)` and `Result(T, E)` (11)

### Sprint 5 (Long-term): Language Evolution
16. `?` operator
17. Closures
18. Traits

---

## 5. Metrics & Success Criteria

| Metric | Current | Target (Sprint 2) | Target (Sprint 4) |
|--------|---------|-------------------|-------------------|
| Test count | 85+ | 100+ | 120+ |
| Borrow checker features | 15/27 (56%) | 17/27 (63%) | 20/27 (74%) |
| Compiler LOC | ~13,000 | ~14,500 | ~17,000 |
| Error message quality | Basic | Source-line context | Full diagnostic spans |
| Std library modules | 3 (c, io, fs) | 3 | 6 (+ math, string, mem) |
| Real-world programs buildable | Toy examples | Small utilities | Non-trivial programs |

---

## 6. Architectural Notes

### 6.1 Compiler Structure — Clean and Minimal

The compiler is well-structured as a single-compilation-unit C99 program. The header-only architecture (`#include` tree from `main.c`) keeps things simple. At ~13K LOC, it's still small enough to hold in one developer's head.

**No major refactoring needed** — the current architecture scales to at least 25K LOC. When/if the compiler exceeds that, consider splitting into separate compilation units.

### 6.2 The AST — Room for Improvement

`ast.h` (974 lines) is the largest data-definition file. It's well-organized with tagged unions. One area to watch: as new expression/statement types are added (array literals, closures, etc.), the Expr/Stmt unions will grow. This is fine for now.

### 6.3 The Emitter — Hardcoded Overrides

`emit/core.h` and `emit/stmt.h` contain hardcoded special cases for C functions like `fopen`, `fclose`, `fputs`. These should eventually be replaced with a proper annotation system. Low priority but worth noting.

---

## 7. Summary

Lain is at an inflection point. The borrow checker foundation is **strong and sound** — it catches the major classes of memory safety bugs. The remaining borrow checker features (Phases 6-9) are precision improvements with diminishing returns.

The path to "perfection" now runs through:
1. **Developer experience** — better errors, array literals, real diagnostics
2. **Standard library** — making the language useful for real programs
3. **Targeted borrow checker improvements** — two-phase borrows and non-consuming match are the highest-impact remaining features

The language design is solid. The compiler architecture is clean. The test suite is comprehensive. The foundation is ready for the next level.

---

*Fine dell'analisi — 5 marzo 2026*
