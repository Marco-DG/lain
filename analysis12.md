# Analysis 12 — Phase 4 Borrow Checker: Precision & Completeness

## Overview

Phase 4 closes three precision gaps in the borrow checker, building on Phase 3's re-borrow transitivity and owner reassignment.

| Feature | Status | Files Modified |
|---------|--------|---------------|
| Direct `var`-expression persistent borrows | ✅ Implemented | `linearity.h` |
| Multi-`var`-param lifetime inference | ✅ Verified | `linearity.h` (already worked) |
| Defer + linearity consumption tracking | ✅ Implemented | `linearity.h` |

---

## Feature 1: Direct `var`-Expression Persistent Borrows

### Problem

Previously, persistent borrows were only created when `var ref = func_call(var data)` where the function returns `var T`. But a direct `var ref = var data.field` also creates a mutable borrow that should persist across statements.

### Solution

In `STMT_VAR`, added detection for `EXPR_MUT` initializer expressions. When `init->kind == EXPR_MUT`, the owner is extracted from the inner expression (handling both identifiers and member chains) and a persistent borrow is registered with NLL tracking.

```c
if (init && init->kind == EXPR_MUT && tbl->borrows && tbl->arena) {
    Id *binding_id = s->as.var_stmt.name;
    Expr *inner = init->as.mut_expr.expr;
    // ... extract owner_id ...
    borrow_register_persistent(tbl->arena, tbl->borrows,
        binding_id, owner_id, MODE_MUTABLE, owner_region, lu, NULL);
}
```

### Test Results

| Test | Expected | Result |
|------|----------|--------|
| `direct_var_borrow_pass.ln` | ✅ Pass | ✅ |
| `direct_var_borrow_fail.ln` | ❌ Fail | ❌ Correctly fails |

---

## Feature 2: Multi-`var`-Parameter Lifetime Inference

### Verification

The existing `for` loop in `STMT_VAR`'s persistent borrow registration already iterates over ALL parameters and registers borrows for every `var` parameter. No code changes were needed — the feature was already implicitly working.

### Test Results

| Test | Expected | Result |
|------|----------|--------|
| `multi_var_param_pass.ln` | ✅ Pass | ✅ |
| `multi_var_param_fail.ln` | ❌ Fail | ❌ Correctly fails |

---

## Feature 3: Defer + Linearity Consumption Tracking

### Problem

`defer drop(mov r)` was immediately consuming `r` during the walker's traversal of the defer statement. This caused false "use after move" errors for any subsequent uses of `r` after the defer declaration.

### Solution

Added `is_defer_consumed` field to `LEntry` and implemented a save/restore strategy in `STMT_DEFER`:

1. **Save**: Before walking the defer body, save the state of all tracked variables
2. **Walk**: Process the defer body normally (EXPR_MOVE sets CONSUMED)
3. **Detect & Restore**: Compare states afterward — any variable that went UNCONSUMED → CONSUMED was consumed inside defer. Mark it `is_defer_consumed = true` and restore to UNCONSUMED

Both `ltable_ensure_all_consumed` and `ltable_pop_scope` now treat `is_defer_consumed` as equivalent to consumed.

### Key Bug Fix

The initial implementation only saved `must_consume` (linear) entries. But `EXPR_MOVE` consumes ANY mutable variable, not just linear ones. Fixed by saving all tracked entries.

### Test Results

| Test | Expected | Result |
|------|----------|--------|
| `defer_consume_pass.ln` | ✅ Pass | ✅ |
| `defer_consume_fail.ln` | ❌ Fail | ❌ Correctly fails |

---

## Full Regression Results

All 80+ tests pass with zero regressions.

---

## Future Work

1. **Shared persistent borrows** — requires explicit `ref T` type in the language
2. **Per-field linearity tracking** — allow `mov data.field` without consuming entire struct
3. **Block-level NLL** — analyze borrow lifetimes within nested blocks
4. **Non-consuming match** — `match &x` borrow-through-match pattern
