# Analysis 13 — Phase 5 Borrow Checker: Per-Field Linearity

## Overview

Phase 5 adds per-field linearity tracking to the borrow checker, enabling field-level consumption of structs with linear (`mov`) fields.

## Feature: Per-Field Linearity (One-Level)

### Problem

Previously, `mov p.left` consumed the *entire* variable `p`. After `mov p.left`, `p.right` was inaccessible — a false positive.

### Solution

Added `FieldState` linked list to `LEntry` tracking individual linear field consumption:

| Component | Purpose |
|-----------|---------|
| `FieldState` struct | Tracks `field_name` + `is_consumed` per linear field |
| `ltable_init_field_states()` | Resolves struct type, creates FieldState for each linear field |
| `ltable_consume_field()` | Consumes a specific field; auto-completes whole-var when all done |
| `ltable_is_partially_consumed()` | Detects mixed consumed/unconsumed state |
| `EXPR_MOVE` update | Tries field-level first; blocks whole-move if partially consumed |
| `ltable_ensure_all_consumed` / `ltable_pop_scope` | Reports per-field error messages |

### Error Message Improvement

Before (Phase 4):
```
Error Ln 12, Col 4: linear variable 'h1' was not consumed before return.
```

After (Phase 5):
```
Error Ln 12, Col 4: linear field 'ptr' of 'h1' was not consumed before return.
```

### Test Results

| Test | Expected | Result |
|------|----------|--------|
| `field_consume_pass.ln` | ✅ Pass | ✅ |
| `field_consume_fail.ln` | ❌ Fail | ❌ Correctly fails |
| `field_partial_move_fail.ln` | ❌ Fail | ❌ Correctly fails |

### Full Regression

All 85+ tests pass with zero regressions.

## Files Modified

| File | Changes |
|------|---------|
| `linearity.h` | `FieldState`, `ltable_init_field_states`, `ltable_consume_field`, `ltable_is_partially_consumed`, updated `EXPR_MOVE`, `ltable_ensure_all_consumed`, `ltable_pop_scope` |

## Future Work

- Per-field borrow tracking (§11.4)  
- Multi-level field paths (`p.inner.handle`)
- Defer borrow verification (§15.2)
