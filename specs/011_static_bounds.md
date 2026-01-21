# Design Note 011: Static Bounds Checking

## 1. Goal
Ensure **zero** runtime index out-of-bounds errors with **zero** runtime overhead.

## 2. Approach: Value Range Analysis (VRA)

The compiler tracks an interval `[min, max]` for every integer variable in the Scope.

### 2.1 Domain
*   **Literals**: `10` -> `[10, 10]`
*   **Types**: `u8` -> `[0, 255]`, `int` -> `[-2^31, 2^31-1]`
*   **Arithmetic**:
    *   `[a, b] + [c, d]` -> `[a+c, b+d]`
    *   `[a, b] - [c, d]` -> `[a-d, b-c]`
    *   ...etc...

### 2.2 Control Flow
*   **If condition**: `if x < 10`
    *   True branch: `x` domain intersected with `[-inf, 9]`
    *   False branch: `x` domain intersected with `[10, +inf]`
*   **Merge**: `phi(A, B)` -> `Union(A, B)` (Conservative Hull)

### 2.3 Verification
At any array access `arr[i]`:
*   Let `arr` have size `N`. Valid range is `[0, N-1]`.
*   Let `i` have computed range `[min_i, max_i]`.
*   **Check**: `min_i >= 0` AND `max_i < N`.
    *   If true: Safe. Compile.
    *   If false/unknown: Compile Error "Index out of bounds".

## 3. The `in` Keyword
To handle dynamic inputs where static analysis is insufficient, we use contract-based assertions:

`func get(arr int[10], i int in arr) int`

*   `i int in arr` desugars to a precondition constraint: `i >= 0 AND i < arr.len`.
*   The caller **must** prove this constraint holds (e.g. by checking `if i >= 0 and i < 10` before calling).

## 4. Loops
Loops are the hardest part.
*   **Canonical Loops**: `for i in 0..N` -> `i` is `[0, N-1]`. Safe.
*   **While Loops / Complex**: Widen to type limits if convergence fails.

## 5. Current Implementation State
The codebase seems to have rudimentary support (`src/sema/bounds.h`, `tests/bounds_pass.ln`). The next step is to rigorous protocol this and expand coverage.
