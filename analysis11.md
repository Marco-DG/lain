# Analysis 11 — Phase 3 Borrow Checker: Re-borrow Transitivity & Owner Reassignment

## Overview

Phase 3 of the borrow checker advances the soundness guarantees implemented in Phase 2 (NLL). This work closes two real soundness gaps and documents a third that requires language-level changes before it can be fixed.

### Changes Summary

| Feature | Status | Files Modified |
|---------|--------|---------------|
| Re-borrow transitivity | ✅ Implemented | `region.h`, `linearity.h` |
| Full owner reassignment check | ✅ Implemented | `linearity.h` |
| Shared persistent borrows | ⏸ Deferred | — |

---

## Feature 1: Re-borrow Transitivity

### Problem

When `ref = get_ref(var data)` creates a mutable borrow of `data`, and `ref2 = transform(var ref)` creates a re-borrow through `ref`, the borrow checker must understand that `ref2` *transitively* borrows `data`. If `ref` expires via NLL but `ref2` is still alive, accessing `data` would be unsound.

### Solution

#### `root_owner` field on `BorrowEntry`

Added a `root_owner` field to `BorrowEntry` (in `region.h`) that tracks the ultimate non-reference owner through borrow chains:

```c
typedef struct BorrowEntry {
    Id *var;
    Id *owner_var;
    Id *root_owner;    // Phase 3: ultimate non-reference owner
    Id *binding_id;
    // ...
} BorrowEntry;
```

#### `borrow_find_by_binding()` helper

New function to look up a `BorrowEntry` by its `binding_id`. When registering a persistent borrow for `ref2 = transform(var ref)`, this function checks whether `ref` is itself a persistent borrow binding, and if so, inherits its `root_owner`:

```c
static BorrowEntry *borrow_find_by_binding(BorrowTable *t, Id *binding_id);
```

#### Transitive re-borrow detection in `borrow_register_persistent()`

Updated signature to accept `Id *root_owner` parameter. When `root_owner` is NULL, the function auto-detects re-borrow chains:

```c
// Before: borrow_register_persistent(arena, t, binding, owner, mode, region, lu)
// After:  borrow_register_persistent(arena, t, binding, owner, mode, region, lu, root_owner)
```

If `owner` is itself a persistent borrow binding, the new borrow inherits that entry's `root_owner`. This means conflict checks and expiration checks all target the ultimate owner (`data`), not the intermediate reference (`ref`).

#### Fixpoint release in `borrow_release_expired()`

Added `borrow_has_transitive_dependent()` which checks if any other active persistent borrow shares the same `root_owner` and is still alive. `borrow_release_expired()` now uses a fixpoint loop:

```
ref(lu=2), ref2(lu=3), ref3(lu=4), current_stmt=4
Iteration 1: ref3 released (no dependents), ref2 blocked (ref3 alive? no, just released)
Iteration 2: ref2 released (no dependents left), ref released
Iteration 3: stable → done
```

Also skipped conflict checks for re-borrows (extending an existing chain is not a parallel conflict).

### Test Results

| Test | Expected | Result |
|------|----------|--------|
| `reborrow_pass.ln` | ✅ Pass | ✅ Pass |
| `reborrow_fail.ln` | ❌ Fail | ❌ Fail correctly |
| `reborrow_chain_pass.ln` | ✅ Pass | ✅ Pass |
| `reborrow_same_stmt_fail.ln` | ❌ Fail | ❌ Fail correctly |

---

## Feature 2: Shared Persistent Borrows — DEFERRED

### Problem

Functions returning with `MODE_SHARED` return types should create shared persistent borrows that block mutation of the owner.

### Why Deferred

`MODE_SHARED` is the **default ownership mode for all types** in Lain. Every `int`, every struct field, every function return type defaults to `MODE_SHARED`. Extending persistent borrow detection to `MODE_SHARED` returns would cause every function call result bound to a variable to create a persistent borrow — a catastrophic false positive rate.

### Prerequisite

This feature requires an **explicit shared reference type** in the language, analogous to how `var T` marks a mutable reference. Possible syntax:
- `ref T` or `& T` for explicit shared references
- A new `MODE_REF` or `TYPE_REF` in the type system

This is tracked as a Phase 4 feature in `borrow_checker.md` §9.4.

---

## Feature 3: Full Owner Reassignment Check

### Problem

The `STMT_ASSIGN` handler in `linearity.h` walked through `EXPR_MEMBER` and `EXPR_INDEX` chains to find the root identifier for borrow conflict checks. However, a **direct identifier assignment** (`data = new_data`) skipped this logic because `data` is directly an `EXPR_IDENTIFIER`, not wrapped in member/index expressions.

### Solution

Unified the base identifier extraction in `STMT_ASSIGN`:

```c
Id *base_id = NULL;
if (lhs && lhs->kind == EXPR_IDENTIFIER) {
    // Direct assignment: data = new_data
    base_id = lhs->as.identifier_expr.id;
} else {
    // Member/Index path: data.value = 99
    // ... walk chain to find root ...
}
if (base_id) {
    borrow_check_owner_access(tbl->borrows, base_id, MODE_MUTABLE, ...);
}
```

### Test Results

| Test | Expected | Result |
|------|----------|--------|
| `owner_reassign_fail.ln` | ❌ Fail | ❌ Fail correctly |
| `owner_reassign_after_release_pass.ln` | ✅ Pass | ✅ Pass |

---

## Full Regression Results

All 70+ existing tests still pass. No regressions were introduced.

**Positive tests**: 40 (all pass — compile + run + exit 0)
**Negative tests**: ~30 (all fail correctly — compile error as expected)

---

## Issues Discovered

1. **C codegen double-pointer**: `func transform(var r int) var int { return var r }` — passing `var ref` where `ref : var int` creates `int**` incompatibility in the generated C code. This is a codegen limitation, not a borrow checker issue. Re-borrow test cases were written to avoid this pattern.

2. **NLL release timing**: `borrow_release_expired` only runs after complete top-level statement processing. This means two borrows registered on the same statement (e.g., `var ref = get_ref(var data)` in stmt N, then `var ref2 = get_ref(var data)` in stmt N+1) work correctly since `ref`'s borrow is released after stmt N. But if both were on the same statement, the conflict would be caught by the temporary borrow system.

---

## Future Work / Phase 4

1. **Explicit shared reference types** (`ref T` or similar) — prerequisite for shared persistent borrows
2. **Per-field linearity tracking** — allow `mov data.field` without consuming the entire struct  
3. **Block-level NLL** — analyze borrow lifetimes within nested blocks, not just top-level statements
4. **Two-phase borrows** — allow `data.method(var data.field)` by splitting the borrow into a reservation phase
