# The Lain Borrow Checker — Complete Specification

**Version**: 1.0.0 (Draft)
**Date**: 2026-03-03
**Status**: Normative Specification — Target Design Document

> This document specifies the **complete, target-state** borrow checker that Lain shall implement. It covers the current implementation, identifies its limitations, and defines the rules, algorithms, and semantics for the final system. Sections marked with 🟢 are implemented. Sections marked with 🔴 are not yet implemented and represent the target design.

---

## Table of Contents

1. [Introduction & Goals](#1-introduction--goals)
2. [Foundational Concepts](#2-foundational-concepts)
3. [The Three Ownership Modes](#3-the-three-ownership-modes)
4. [Borrowing Rules — The Read-Write Lock Invariant](#4-borrowing-rules--the-read-write-lock-invariant)
5. [Lifetimes & Regions](#5-lifetimes--regions)
6. [Non-Lexical Lifetimes (NLL)](#6-non-lexical-lifetimes-nll)
7. [Move Semantics & Linearity](#7-move-semantics--linearity)
8. [Borrow Checking Through Control Flow](#8-borrow-checking-through-control-flow)
9. [References as Values — `return var` and Persistent Borrows](#9-references-as-values--return-var-and-persistent-borrows)
10. [Re-Borrowing & Transitivity](#10-re-borrowing--transitivity)
11. [Struct Field Granularity](#11-struct-field-granularity)
12. [Slices, Arrays & Borrowed Views](#12-slices-arrays--borrowed-views)
13. [ADTs & Pattern Matching Under Ownership](#13-adts--pattern-matching-under-ownership)
14. [Unsafe Code & The Escape Hatch](#14-unsafe-code--the-escape-hatch)
15. [Interaction with `defer`](#15-interaction-with-defer)
16. [Interaction with Generics](#16-interaction-with-generics)
17. [Diagnostic Requirements](#17-diagnostic-requirements)
18. [Formal Model — Typing Rules](#18-formal-model--typing-rules)
19. [Implementation Architecture](#19-implementation-architecture)
20. [Test Matrix](#20-test-matrix)
21. [Phased Implementation Roadmap](#21-phased-implementation-roadmap)

---

## 1. Introduction & Goals

### 1.1 Purpose

The borrow checker is the **central safety mechanism** of the Lain programming language. It enforces, at compile time, the following invariants:

1. **No Use-After-Free** — A reference (borrow) to a value cannot be used after the value is moved or destroyed.
2. **No Data Races** — At any given program point, either multiple shared (read-only) borrows OR exactly one mutable (read-write) borrow may exist for a given value — never both.
3. **No Dangling References** — A borrow cannot outlive the value it borrows from.
4. **Affine Resource Consumption** — Values marked as linear (`mov`) must be consumed exactly once; forgetting or double-consuming is a compile error.

These guarantees are achieved with **zero runtime overhead**: no reference counting, no garbage collection, no runtime checks. The borrow checker operates entirely at compile time.

### 1.2 Design Principles

| Principle | Description |
|-----------|-------------|
| **Soundness over completeness** | The checker may reject valid programs (false positives) but must never accept invalid programs (false negatives). |
| **Explicitness at call sites** | The caller annotates `var` (mutable borrow) and `mov` (ownership transfer) explicitly. Shared borrows are the implicit default. |
| **Structured control flow** | Lain has no `goto`. All control flow is structured (`if`/`for`/`while`/`case`/`match`). This simplifies lifetime analysis compared to CFG-based approaches. |
| **No lifetime annotations** | Unlike Rust, Lain does not require the programmer to write lifetime parameters (`'a`). Lifetimes are inferred from the structured scope and liveness analysis. |
| **Compositional checking** | Each function is checked independently. Cross-function reasoning is done via function signatures (parameter modes and return modes). |

### 1.3 Relationship to Rust

Lain's ownership model is inspired by Rust but differs in several key ways:

| Aspect | Rust | Lain |
|--------|------|------|
| Lifetime annotations | Required for complex signatures (`&'a T`) | Never required — inferred from scope + NLL |
| Borrow syntax | `&T`, `&mut T` | `p T` (shared), `var p T` (mutable) |
| Move syntax | Implicit (by default for non-Copy) | Explicit `mov` keyword required |
| Call-site annotations | None (compiler infers) | Required: `var x`, `mov x` |
| Trait system (Send/Sync) | Required for concurrency | Not yet applicable (no concurrency) |
| Interior mutability | `Cell`, `RefCell`, `UnsafeCell` | Not yet defined |
| Two-phase borrows | Yes | 🟢 Implemented (RESERVED/ACTIVE phases) |
| NLL granularity | MIR instruction-level | Statement-level (top-level) |

### 1.4 Scope of This Document

This document covers:
- **What is implemented** (marked 🟢)
- **What must be implemented** for soundness (marked 🔴)
- **What is desirable** for ergonomics (marked 🟡)

---

## 2. Foundational Concepts

### 2.1 Values and Places

A **value** is any piece of data in a Lain program: an integer, a struct, a pointer, an array element. Values exist at compile time as typed data.

A **place** is a location in memory where a value resides. Places form a hierarchy:

```
Place ::= Variable                  // x
        | Place.field               // x.name
        | Place[index]              // x[i]
        | *Place                    // dereference (unsafe only)
```

Examples:
```lain
var p = Point(10, 20)     // Place: p
p.x                       // Place: p.x
var arr int[5]            // Place: arr
arr[2]                    // Place: arr[2]
```

The borrow checker tracks ownership and borrows at the **place** level. A borrow of `p.x` should not, in the target system, conflict with a borrow of `p.y` (see §11 for field-granular tracking).

### 2.2 Ownership

Every place has exactly one **owner** at any point in the program. The owner is responsible for the value's lifetime. When the owner goes out of scope or is moved, the value is destroyed (or transferred).

Ownership follows these rules:
1. When a variable is declared, the variable owns the value: `var x = 42` → `x` owns `42`.
2. When ownership is transferred via `mov`, the source is invalidated: `var y = mov x` → `y` owns the value, `x` is dead.
3. When a scope exits, all owned values in that scope are destroyed (unless moved out).

### 2.3 Borrows

A **borrow** is a temporary permission to access a value without taking ownership. Borrows come in two flavors:

| Kind | Syntax | Permission | Exclusivity |
|------|--------|------------|-------------|
| **Shared** | `f(x)` | Read-only | Multiple allowed simultaneously |
| **Mutable** | `f(var x)` | Read + Write | Exclusive — no other borrows allowed |

A borrow has a **lifetime**: the span of program execution during which the borrow is valid. The borrow checker ensures that:
- A borrow's lifetime does not exceed the owner's lifetime.
- The exclusivity rules are not violated during the borrow's lifetime.

### 2.4 Linear Types

A type is **linear** if it must be consumed exactly once. In Lain, linearity is indicated by the `mov` annotation on struct fields or type modes:

```lain
type File {
    mov handle *FILE     // linear field → File is linear
}
```

A linear value cannot be:
- **Copied** — `var f2 = f` is a compile error (it would create two owners of the same handle).
- **Ignored** — letting `f` go out of scope without consuming it is a compile error.
- **Double-consumed** — `close(mov f); close(mov f)` is a compile error.

Linearity and borrowing interact: a linear value can be borrowed (shared or mutable) without consuming it, but it must be eventually consumed via `mov`.

---

## 3. The Three Ownership Modes

### 3.1 Shared Mode (Default) — `p T`

The default parameter mode. The callee receives a read-only view of the value.

**Semantics:**
- The value is passed as `const T*` in the generated C code.
- Multiple shared borrows of the same variable may be active simultaneously.
- The callee cannot modify the value.
- The caller retains ownership — the value is not consumed.

```lain
func length(s u8[:0]) int {      // shared borrow of s
    return s.len
}

proc main() {
    var msg = "hello"
    var n = length(msg)          // shared borrow — msg still valid
    var m = length(msg)          // another shared borrow — OK
}
```

**At the call site:** No annotation is needed. `f(x)` implies a shared borrow.

### 3.2 Mutable Mode — `var p T`

The callee receives an exclusive read-write reference to the value.

**Semantics:**
- The value is passed as `T*` in the generated C code.
- At most one mutable borrow of a variable may be active at any time.
- While a mutable borrow is active, no other borrow (shared or mutable) of the same variable is permitted.
- The callee can modify the value. Modifications are visible to the caller.

```lain
func increment(var d Data) {
    d.value = d.value + 1        // modifies caller's data
}

proc main() {
    var data = Data(0)
    increment(var data)          // mutable borrow — exclusive
    // data.value is now 1
}
```

**At the call site:** The `var` keyword is required: `f(var x)`.

### 3.3 Owned Mode — `mov p T`

The callee takes ownership of the value. The caller's variable is invalidated after the call.

**Semantics:**
- The value is passed as `T` (by value) in the generated C code.
- The caller's binding is marked as consumed (dead) after the call.
- The callee is responsible for the value's fate: it can use it, transform it, or consume it.
- For linear types, the callee must eventually consume the value (or return it).

```lain
proc close_file(mov {handle} File) {
    fclose(handle)               // consumes the handle
}

proc main() {
    var f = open_file("data.txt", "r")
    close_file(mov f)            // ownership transferred — f is dead
    // f is no longer valid here
}
```

**At the call site:** The `mov` keyword is required: `f(mov x)`.

### 3.4 Return Modes

Functions can also specify the ownership mode of their return value:

| Return Type | Semantics | C Emission |
|-------------|-----------|------------|
| `func f() T` | Returns a copy (value semantics) | `T f()` |
| `func f() mov T` | Returns an owned value (caller takes ownership) | `T f()` |
| `func f() var T` | Returns a mutable reference (borrow extends to caller) | `T* f()` |

The `return var` form is the most complex: it creates a **persistent borrow** that extends beyond the callee's scope into the caller's scope. See §9 for detailed rules.

---

## 4. Borrowing Rules — The Read-Write Lock Invariant

### 4.1 The Core Invariant

At every program point, for every place `P`, the following **Read-Write Lock** invariant must hold:

> **Either** there are zero or more active shared borrows of `P`, **or** there is exactly one active mutable borrow of `P` — **never both**.

This can be expressed as a state machine:

```
                ┌─────────────┐
                │   UNBORROWED │
                │  (no borrows)│
                └──────┬──────┘
                       │
           ┌───────────┼───────────┐
           │ shared    │           │ mutable
           ▼ borrow    │           ▼ borrow
    ┌──────────────┐   │    ┌──────────────┐
    │ SHARED(n)    │   │    │ MUTABLE(1)   │
    │ n ≥ 1 readers│   │    │ 1 writer     │
    │              │   │    │              │
    │ +shared: OK  │   │    │ +shared: ❌  │
    │ +mutable: ❌ │   │    │ +mutable: ❌ │
    │ +move: ❌    │   │    │ +move: ❌    │
    └──────────────┘   │    └──────────────┘
           │           │           │
           │ all shared│           │ mutable
           │ released  │           │ released
           └───────────┼───────────┘
                       │
                       ▼
                ┌─────────────┐
                │   UNBORROWED │
                └─────────────┘
```

### 4.2 Conflict Detection Rules

Given an existing set of active borrows for a place `P`, the following table defines whether a new operation is permitted:

| Existing State | New Operation | Result |
|----------------|---------------|--------|
| Unborrowed | Shared borrow | ✅ Transitions to SHARED(1) |
| Unborrowed | Mutable borrow | ✅ Transitions to MUTABLE(1) |
| Unborrowed | Move (`mov`) | ✅ Value consumed, all done |
| SHARED(n) | Shared borrow | ✅ SHARED(n+1) |
| SHARED(n) | Mutable borrow | ❌ Compile error |
| SHARED(n) | Move (`mov`) | ❌ Compile error — cannot move while borrowed |
| MUTABLE(1) | Shared borrow | ❌ Compile error |
| MUTABLE(1) | Mutable borrow | ❌ Compile error |
| MUTABLE(1) | Move (`mov`) | ❌ Compile error — cannot move while borrowed |

### 4.3 Intra-Expression Conflicts 🟢

The simplest form of conflict detection: multiple borrows of the same variable within a single function call.

```lain
// ❌ COMPILE ERROR: shared + mutable borrow in same expression
modify_both(data, var data)

// ❌ COMPILE ERROR: two mutable borrows in same expression
double_mut(var data, var data)

// ✅ OK: two shared borrows
read_both(data, data)
```

**Implementation**: 🟢 Implemented. During expression traversal in `linearity.h`, each argument's borrow is registered in the `BorrowTable`. `borrow_check_conflict()` detects same-expression conflicts. Temporary borrows are cleared after each statement via `borrow_clear_temporaries()`.

### 4.4 Cross-Statement Conflicts 🟢

When a borrow is captured into a variable (via `return var`), it persists beyond the originating statement:

```lain
var data = Data(42)
var ref = get_ref(var data)    // persistent mutable borrow of 'data'
var x = read_data(data)        // ❌ ERROR if ref is still alive
consume_ref(var ref)           // ref's last use
```

**Implementation**: 🟢 Implemented via NLL (see §6). The `use_analysis.h` pre-pass computes the last-use statement index of `ref`. The borrow of `data` is released after `ref`'s last use.

### 4.5 Condition-Expression Borrows 🟢

Borrows created in loop/if conditions are **temporary** and do not extend into the body:

```lain
var data = Buffer(0)

// ✅ OK: shared borrow in condition expires before body executes
while read(data) < 10 {
    mutate(var data)           // mutable borrow in body — no conflict
}
```

**Implementation**: 🟢 Implemented. Condition expressions are checked, then `borrow_clear_temporaries()` is called before the body is traversed.

### 4.6 Owner Reassignment While Borrowed 🟢

Reassigning or mutating the owner while a borrow is active is an error:

```lain
var data = Data(42)
var ref = get_ref(var data)     // mutable borrow of data
data.value = 99                 // ❌ ERROR: writing to borrowed data
use(ref)
```

**Implementation**: 🟢 Implemented. `borrow_check_owner_access()` and `borrow_check_owner_access_field()` in `region.h` check STMT_ASSIGN targets against active persistent borrows. Both field writes (`data.value = ...`) and full reassignment (`data = new_data`) are caught. Field-granular checks allow non-overlapping field writes when only a specific field is borrowed.

---

## 5. Lifetimes & Regions

### 5.1 What is a Lifetime?

A **lifetime** is the span of program execution during which a borrow is valid. Every borrow has an associated lifetime, and the borrow checker ensures that:

1. The lifetime of a borrow does not exceed the lifetime of the borrowed value (the owner).
2. No conflicting borrows overlap in their lifetimes.

In Lain, lifetimes are **never written by the programmer**. They are inferred by the compiler from two sources:

- **Scope structure** — A variable's maximum possible lifetime is its declaring scope.
- **Liveness analysis** — A borrow's actual lifetime extends only to its **last use** (NLL semantics, §6).

### 5.2 Regions

A **region** represents a lexical scope in the program. Regions form a tree:

```
Region 0 (function body)
├── Region 1 (if-then block)
│   └── Region 3 (nested if)
├── Region 2 (if-else block)
└── Region 4 (for-loop body)
```

Every variable is associated with the region where it is declared. A borrow's region is the region where the borrow is created.

**Containment rule**: A borrow is valid only if its region is **contained within** (or equal to) the owner's region. This means a reference cannot escape to an outer scope where the owner might not exist.

```lain
proc example() {
    var outer_ref *int          // Region 0
    if true {
        var local = 42          // Region 1
        // outer_ref = &local   // ❌ Region 0 outlives Region 1 — dangling!
    }
    // local is dead here — outer_ref would be dangling
}
```

### 5.3 Region Hierarchy Operations 🟢

The current `region.h` implements regions as a linked list of parent pointers:

| Operation | Description | Complexity |
|-----------|-------------|------------|
| `region_new(arena, parent)` | Create a child region | O(1) |
| `region_contains(outer, inner)` | Check if outer contains inner (walks parent chain) | O(depth) |
| `borrow_enter_scope()` | Push a new child region | O(1) |
| `borrow_exit_scope()` | Pop to parent region | O(1) |

### 5.4 Lifetime Bounds on Return Values 🟢 (partial)

When a function returns a reference (`return var`), the compiler must ensure the returned reference does not point to a local variable:

```lain
func get_ref(var ctx Context) var int {
    return var ctx.counter     // ✅ OK: ctx outlives the function
}

func dangerous() var int {
    var local = 42
    return var local            // ❌ ERROR: local dies at function exit
}
```

**Rule**: `return var expr` is valid only if `expr` refers to a place whose lifetime extends beyond the function body — i.e., it must be a field of a `var` parameter (which is borrowed from the caller's scope).

**Implementation**: 🟢 Implemented. `linearity.h` checks in `STMT_RETURN` whether the inner expression is an `EXPR_MUT` wrapping a local variable identifier. If `!is_parameter`, it emits "cannot return mutable reference to local variable".

### 5.5 Lifetime Inference for `return var` 🔴

When a function returns `var T`, the compiler must infer which parameters the return value borrows from. This is the **lifetime elision** problem.

**Current rule** (implicit): If a function takes `var p T` and returns `var U`, the returned reference is assumed to borrow from the first `var` parameter.

**Target rule**: The compiler should walk the `return var expr` expression and identify which `var` parameter(s) are in the path. If ambiguous, a compile error should be raised.

```lain
// Unambiguous: returns a borrow of 'a'
func first(var a Data, var b Data) var int {
    return var a.value
}

// 🔴 Ambiguous: which parameter is borrowed?
func pick(cond bool, var a Data, var b Data) var int {
    if cond { return var a.value }
    return var b.value
}
// Target behavior: both 'a' and 'b' are considered borrowed at the call site
```

---

## 6. Non-Lexical Lifetimes (NLL)

### 6.1 Motivation

Before NLL, borrows were tied to **lexical scope** — a borrow lived until the end of the block where the borrowing variable was declared. This caused unnecessary rejections:

```lain
var data = Data(42)
var ref = get_ref(var data)     // borrow starts here
var x = read_data(data)         // ❌ Pre-NLL: ERROR (ref in scope)
// ref is never used again!
return 0                        // borrow dies here (end of scope)
```

With NLL, the borrow of `data` through `ref` ends at `ref`'s **last use**, not at the end of the scope. If `ref` is never used after `var ref = get_ref(var data)`, the borrow is immediately released.

### 6.2 Liveness Analysis — The `UseTable` 🟢

The NLL system performs a **pre-pass** over the function body that computes, for each identifier, the index of the last top-level statement where it appears.

**Algorithm** (implemented in `use_analysis.h`):

```
FUNCTION use_compute_last_uses(body: StmtList) → UseTable:
    table = new UseTable
    stmt_idx = 0
    FOR EACH stmt IN body:
        use_walk_stmt(stmt, table, stmt_idx)
        stmt_idx += 1
    RETURN table

FUNCTION use_walk_stmt(stmt, table, stmt_idx):
    // Walk all expressions in the statement recursively
    // For each EXPR_IDENTIFIER found, update:
    //   table[id] = max(table[id], stmt_idx)
    //
    // For nested blocks (if/for/while/match):
    //   Use the PARENT statement's stmt_idx (conservative)
```

**Key design decisions**:
- The variable **declared** in `STMT_VAR` is NOT counted as a "use" — only the initializer expression is walked. This means `var ref = get_ref(var data)` registers a use of `data` and `get_ref`, but `ref`'s last_use comes from subsequent statements.
- Uses inside control flow blocks (`if`/`for`/`while`/`match`) map to the **enclosing top-level statement's index**. This is conservative but sound.

### 6.3 Borrow Release Algorithm 🟢

After each top-level statement in the linearity walk, expired borrows are released:

```
FUNCTION borrow_release_expired(table: BorrowTable, current_stmt_idx: int):
    FOR EACH borrow IN table.active_borrows:
        IF borrow.is_persistent AND borrow.binding_id IS NOT NULL:
            IF borrow.last_use_stmt_idx < 0:        // never used
                RELEASE borrow
            ELIF current_stmt_idx >= borrow.last_use_stmt_idx:  // past last use
                RELEASE borrow
```

**Release semantics**: When a persistent borrow is released, the owner's place transitions from MUTABLE(1) (or SHARED(n)) back to UNBORROWED. Subsequent operations on the owner are now permitted.

### 6.4 NLL Examples

**Example 1: Immediate release (never used)** 🟢
```lain
var data = Data(42)
var ref = get_ref(var data)    // borrow created, last_use(ref) = -1 (never used)
// → borrow released immediately after this statement
var x = read_data(data)        // ✅ OK: no active borrows
return x
```

**Example 2: Borrow still active** 🟢
```lain
var data = Data(42)
var ref = get_ref(var data)    // borrow created, last_use(ref) = 2 (stmt index 2)
var x = read_data(data)        // stmt index 1 — ❌ ERROR: borrow active until stmt 2
consume_ref(var ref)           // stmt index 2 — last use of ref
```

**Example 3: Borrow expires before conflict** 🟢
```lain
var data = Data(42)
var ref = get_ref(var data)    // borrow created, last_use(ref) = 1
consume_ref(var ref)           // stmt index 1 — last use of ref
// → borrow released after stmt 1
var x = read_data(data)        // stmt index 2 — ✅ OK: borrow expired
```

### 6.5 NLL Granularity — Current vs Target

| Granularity | Description | Status |
|-------------|-------------|--------|
| **Statement-level** | Each top-level statement gets an index. Uses in nested blocks map to enclosing statement. | 🟢 Implemented |
| **Block-level** | Uses in different `if` branches are tracked independently. | 🔴 Target Phase 2 |
| **Point-level** | Each individual expression is a program point (like Rust MIR). | 🟡 Long-term goal |

**Block-level NLL** would enable patterns like:

```lain
var data = Data(42)
var ref = get_ref(var data)
if condition {
    use(ref)                   // ref used only in this branch
}
// With block-level NLL: borrow expired (ref not used in else or after)
read_data(data)                // ✅ Would be OK with block-level NLL
                               // ❌ Currently rejected (conservative)
```

---

## 7. Move Semantics & Linearity

### 7.1 Linear Types in Lain

A type is **linear** if any of the following conditions holds:

1. The type has `mode == MODE_OWNED` (explicit `mov` annotation on the field or variable).
2. The type is `TYPE_ARRAY` and the element type is linear.
3. The type is `TYPE_SIMPLE` (struct) and any field is linear.
4. The type is an ADT (enum) and any variant has a linear field.

Linearity is **transitive**: a struct containing a linear field is itself linear. An array of linear elements is linear.

**Implementation**: 🟢 `sema_type_is_linear()` in `linearity.h` performs this recursive check.

### 7.2 The Linear Consumption Rule

For every linear variable `v` declared in a scope:

> `v` must be consumed **exactly once** before the scope exits.

"Consumed" means one of:
- Passed as `mov v` to a function call.
- Returned via `return mov v`.
- Destructured via `mov {fields} v` in a parameter.
- Moved into another variable: `var w = mov v`.

**Not consumed** means:
- Used as a shared borrow: `f(v)` — non-consuming.
- Used as a mutable borrow: `f(var v)` — non-consuming.
- Accessed for reading: `v.field` — non-consuming.

### 7.3 Consumption Tracking 🟢

The `LTable` tracks the state of each linear variable:

```
State Machine per linear variable:

    UNCONSUMED  →(mov)→  CONSUMED
         ↑                   │
         │    (use after      │
         │     move = ERROR)  │
         └────────────────────┘
```

| State | Meaning |
|-------|---------|
| `LSTATE_UNCONSUMED` | Variable is live and not yet consumed |
| `LSTATE_CONSUMED` | Variable has been moved; any subsequent use is an error |

### 7.4 Loop-Depth Rule 🟢

A linear variable defined outside a loop cannot be consumed inside the loop, because the loop body may execute zero or more times:

```lain
var resource = create_resource()
for i in 0..10 {
    consume(mov resource)        // ❌ ERROR: defined outside loop
    // If loop runs 10 times, resource would be consumed 10 times!
    // If loop runs 0 times, resource would never be consumed!
}
```

**Rule**: `ltable_consume()` checks that `e->defined_loop_depth == current_loop_depth`.

### 7.5 Branch Consistency 🟢

Across branches of an `if`/`else` or `match`, linear variables must be consumed consistently — either consumed in **all** branches or in **none**:

```lain
var resource = create_resource()
if condition {
    consume(mov resource)        // consumed in then-branch
} else {
    // ❌ ERROR: resource not consumed in else-branch
    // Both branches must agree on consumption
}
```

**Implementation**: 🟢 `ltable_check_branch_consistency()` compares the states of each linear variable across branches. Inconsistency results in a compile error.

### 7.6 Move While Borrowed — Prohibited 🟢

Moving a value that is currently borrowed is an error:

```lain
var data = Data(42)
var ref = get_ref(var data)     // mutable borrow of data
consume(mov data)               // ❌ ERROR: cannot move while borrowed
use(ref)
```

**Implementation**: 🟢 Before consuming a variable via `ltable_consume()`, the code checks `borrow_is_borrowed()` to see if any active borrows exist for the variable.

### 7.7 Discarding Linear Values — Prohibited 🟢

A linear value cannot be silently discarded:

```lain
proc example() {
    var f = open_file("data.txt", "r")
    // ← function returns without consuming f
    // ❌ ERROR: linear variable 'f' was not consumed before return
}
```

**Implementation**: 🟢 `ltable_ensure_all_consumed()` is called at function exit, and `ltable_pop_scope()` is called at block exit. Both check for unconsumed linear variables.

---

## 8. Borrow Checking Through Control Flow

### 8.1 If/Else Branching 🟢

Borrows and linear states are checked independently in each branch, then merged:

```
ALGORITHM check_if_stmt(if_stmt, table):
    1. Check condition expression
    2. Clear temporary borrows from condition
    3. snapshot = clone(table)
    4. then_table = clone(snapshot)
    5. Walk then-branch with then_table
    6. Pop scope (check linear vars consumed in then-scope)
    7. else_table = clone(snapshot)
    8. Walk else-branch with else_table
    9. Pop scope (check linear vars consumed in else-scope)
    10. Check branch consistency for parent-scope linear vars
    11. Merge results back into parent table
    12. Intersect initialization state (definite init only if BOTH branches init)
```

### 8.2 For/While Loops 🟢

Loops require special handling because the body may execute zero or more times:

**For loops** (`for i in start..end`):
- The loop variable `i` has range `[start, end-1]` within the body.
- Linear variables defined outside the loop cannot be consumed inside.
- Borrows created in the loop condition are temporary (released before body).

**While loops** (`while condition`):
- Same linear consumption rules as `for`.
- Borrows in the condition are temporary.
- The body is checked once (conservative: assumes the body executes).

### 8.3 Match/Case Statements 🟢

Match arms are checked like `if`/`else` branches: each arm is independently checked, then branch consistency is verified.

```lain
case shape {
    Circle(r): consume(mov resource)     // consumed in this arm
    Rectangle(w, h): consume(mov resource)  // must also be consumed
    Point: consume(mov resource)            // must also be consumed
}
```

**Destructured bindings** in match arms (e.g., `Circle(r)`) are local to the arm scope.

### 8.4 Early Return & `defer` Interaction

When a function contains `return` statements inside nested blocks, the borrow checker must verify:

1. All linear variables are consumed at each `return` point.
2. `defer` blocks execute before the function exits (see §15).
3. Borrows are not active past their last use on any execution path.

```lain
proc process(mov resource Resource) {
    if error_condition {
        consume(mov resource)
        return                    // ✅ resource consumed before return
    }
    consume(mov resource)         // ✅ resource consumed on non-error path
}
```

---

## 9. References as Values — `return var` and Persistent Borrows

### 9.1 The `return var` Mechanism

Lain supports returning **mutable references** from functions. This is the primary mechanism for accessing interior data without copying:

```lain
func get_counter(var ctx Context) var int {
    return var ctx.counter
}
```

The generated C code returns `int*` — a pointer into the caller's `ctx` variable.

### 9.2 Persistent Borrow Semantics 🟢

When the caller writes `var ref = get_ref(var data)`, the following occurs:

1. A **temporary mutable borrow** of `data` is created for the `get_ref` call.
2. The temporary borrow is cleared (standard behavior).
3. The compiler detects that `get_ref` returns `var T` (MODE_MUTABLE return type).
4. A **persistent borrow** is registered: `ref` borrows `data` mutably.
5. The persistent borrow has `last_use_stmt_idx` set from the `UseTable` pre-pass.
6. Until `ref`'s last use, any access to `data` triggers a conflict check.

### 9.3 Persistent Borrow Rules

| Rule | Description | Status |
|------|-------------|--------|
| While a persistent mutable borrow is active, the owner cannot be read | `borrow_check_owner_access(MODE_SHARED)` returns error | 🟢 |
| While a persistent mutable borrow is active, the owner cannot be written | `borrow_check_owner_access(MODE_MUTABLE)` returns error | 🟢 |
| While a persistent mutable borrow is active, the owner cannot be moved | `borrow_is_borrowed()` prevents `ltable_consume()` | 🟢 |
| The persistent borrow is released at the last use of the binding | `borrow_release_expired()` checks `last_use_stmt_idx` | 🟢 |
| `return var local` is a compile error (dangling reference) | Check in `STMT_RETURN` for local identifiers | 🟢 |

### 9.4 Shared Persistent Borrows 🔴

Currently, only **mutable** persistent borrows are tracked (i.e., when a function returns `var T`). A function returning a shared reference does not create a persistent borrow, which means:

```lain
func get_shared_ref(ctx Context) int {
    return ctx.counter        // returns by value (copy), not reference
}
```

Lain's current type system does not distinguish "return by shared reference" from "return by value" for non-`var` return types. This is a **design gap** — in the target system:

🔴 **Target**: A `return` (without `var`) always returns by value (copy). A `return var` returns a mutable reference. Shared references are not yet supported as return values. When they are, they should follow the same persistent borrow rules with MODE_SHARED.

### 9.5 Use-Through-Reference Semantics 🔴

A critical open question: what does it mean to **use** a variable of type `var int` (a mutable reference to int)?

```lain
var ref = get_ref(var data)
// ref has type var int → in C, ref is int*
// How does the programmer:
//   1. Read through ref?   → *ref (requires unsafe) or auto-deref?
//   2. Write through ref?  → *ref = 99 (requires unsafe) or ref = 99 (auto-deref)?
//   3. Pass ref to a function? → f(ref) or f(var ref)?
```

**Current status**: 🔴 Not formally defined. The compiler generates `int*` for `var int` return types, but there is no safe syntax for dereferencing. Test files use `consume_ref(var ref)` as a workaround.

**Target design options**:

| Option | Read | Write | Pro | Con |
|--------|------|-------|-----|-----|
| Auto-dereference | `x = ref` reads `*ref` | `ref = 99` writes `*ref = 99` | Ergonomic | Ambiguous with reassignment |
| Explicit deref operator | `x = ref.*` or `x = *ref` | `ref.* = 99` | Clear | Verbose |
| Only struct fields | N/A for primitives | N/A for primitives | Simple | Severe limitation |

**Recommendation**: Option B (explicit deref) with `ref.*` syntax (not `*ref`, which conflicts with pointer deref in `unsafe`). This keeps safe code unambiguous while allowing references to primitives.

---

## 10. Re-Borrowing & Transitivity

### 10.1 The Re-Borrow Problem 🟢

When a reference variable is itself passed as a borrow to another function, a chain of dependencies forms:

```lain
var data = Data(42)
var ref = get_ref(var data)        // ref borrows data
var ref2 = transform(var ref)      // ref2 borrows ref, which borrows data
// The borrow on 'data' is tracked transitively through ref → ref2
```

**Implementation**: 🟢 Implemented. `borrow_register_persistent()` detects when the owner is itself a persistent borrow binding and registers a `root_owner` on the new entry, creating a transitive chain.

### 10.2 Transitive Borrow Chain 🟢

The system maintains a **borrow dependency graph** via `root_owner` pointers:

```
data ←(borrows)— ref ←(borrows)— ref2
```

The borrow on `data` is alive as long as **any transitive dependent** (`ref` or `ref2`) is alive. Release happens only when the entire chain's last use is past.

### 10.3 Algorithm 🟢

```
WHEN registering persistent borrow (binding_id borrows owner_id):
    1. Check if owner_id is itself a persistent borrow binding
    2. IF yes: also register binding_id as transitively borrowing
       owner_id's transitive owner (root_owner)
    3. When computing borrow release: a borrow of X is releasable only when
       ALL bindings transitively depending on X have expired

DATA STRUCTURE:
    BorrowEntry {
        ...
        Id *root_owner;     // the original, non-reference owner
        Id *direct_owner;   // the immediate source of the borrow
    }
```

**Implementation**: 🟢 Implemented in `borrow_register_persistent()` (region.h). The `borrow_release_expired()` function uses a fixpoint loop that checks for transitive dependents before releasing a root borrow.

### 10.4 Example — Correct Behavior Under Transitive Borrows 🟢

```lain
var data = Data(42)
var ref = get_ref(var data)       // ref borrows data
var ref2 = transform(var ref)     // ref2 transitively borrows data
// ref is no longer used after this point
// BUT ref2 is still alive → data's borrow must persist!
read_data(data)                   // ❌ ERROR: data is still transitively borrowed by ref2
use(ref2)                         // last use of ref2
read_data(data)                   // ✅ OK: all transitive borrows expired
```

### 10.5 Complexity Considerations

Transitive borrow tracking is handled by the fixpoint loop in `borrow_release_expired()` which scans for dependent borrows before releasing a root borrow. The `UseTable` pre-pass computes the **effective last use** and the release algorithm extends it transitively at release time.

---

## 11. Struct Field Granularity

### 11.1 One-Level Field Tracking 🟢

The linearity checker tracks per-field consumption for structs with linear fields via `FieldState` entries in the `LTable`:

```lain
type Pair {
    mov left *int
    mov right *int
}

var p = Pair(ptr1, ptr2)
consume(mov p.left)         // Consumes p.left only
use(p.right)                // ✅ OK: p.right is still alive
consume(mov p.right)        // Now all fields consumed → p is consumed
```

**Implementation**: 🟢 Implemented. `ltable_init_field_states()` initializes per-field tracking when a struct with linear fields is declared. `ltable_consume_field()` marks individual fields consumed. `ltable_is_partially_consumed()` detects partial consumption. Branch consistency also checks field-level states.

### 11.2 Path-Sensitive Tracking Rules 🟢

Linearity is tracked at the **path level** (variable + one-level field):

| Place | State |
|-------|-------|
| `p` | PARTIALLY_CONSUMED |
| `p.left` | CONSUMED |
| `p.right` | UNCONSUMED |

**Rules for partial consumption**:
1. If all linear fields of a struct are consumed, the struct itself is considered consumed.
2. If some but not all linear fields are consumed, the struct is PARTIALLY_CONSUMED.
3. A PARTIALLY_CONSUMED struct cannot be moved as a whole (`mov p` is an error).
4. A PARTIALLY_CONSUMED struct at scope exit is an error (unconsumed fields).

### 11.3 Path Representation

A **place path** can be represented as a sequence of field accesses:

```
PlacePath ::= Id                      // root variable
            | PlacePath '.' FieldId   // struct field
            | PlacePath '[' Index ']' // array element (future)
```

The `LTable` would be extended from `Map<Id, LState>` to `Map<PlacePath, LState>`.

### 11.4 Borrowing at Field Granularity 🟢

The borrow checker supports field-granular borrows via the `borrowed_field` entry in `BorrowEntry` and the `fields_overlap()` helper:

```lain
type Pair { x int, y int }
var p = Pair(10, 20)
var ref_x = get_x_ref(var p)    // borrows p.x mutably
p.y = 30                        // ✅ OK: p.y is not borrowed
use(ref_x)
```

**Implementation**: 🟢 Implemented. `BorrowEntry` stores a `borrowed_field` name. `borrow_check_conflict_field()` and `borrow_check_owner_access_field()` use `fields_overlap()` to allow non-overlapping field borrows of the same struct.

### 11.5 Design Considerations

| Approach | Precision | Complexity | Recommendation |
|----------|-----------|------------|----------------|
| Whole-variable tracking | Low (conservative) | Low | ✅ Available as fallback |
| One-level field tracking | Medium | Medium | 🟢 Implemented |
| Full path tracking (arbitrary depth) | High | High | 🟡 Long-term goal |

---

## 12. Slices, Arrays & Borrowed Views

### 12.1 Array Ownership

Arrays are value types. An `int[5]` is copied on assignment (unless the element type is linear):

```lain
var a int[5]
var b = a              // copies all 5 elements
b[0] = 99             // does not affect a
```

If the element type is linear, the array is linear and must be moved:

```lain
var arr Resource[3]
var arr2 = mov arr     // moves the whole array
// arr is now dead
```

### 12.2 Slice Borrowing

Slices (`T[]`) are **borrowed views** into arrays. Creating a slice borrows the underlying array:

```lain
var arr int[10]
var s = arr[0..5]       // s borrows arr[0..4] (shared borrow)
```

**Target rules** 🔴:
- A shared slice borrows the underlying array as shared.
- A `var` slice (`var arr[0..5]`) borrows the underlying array as mutable.
- While a slice is alive, the array follows the standard borrow rules.
- Overlapping mutable slices of the same array are an error.

### 12.3 Bounds Interaction

The bounds checker (§9 of README) verifies array accesses statically. The borrow checker ensures that the slice does not outlive its source array. These two systems are orthogonal but both must pass for an access to be safe.

---

## 13. ADTs & Pattern Matching Under Ownership

### 13.1 ADT Ownership 🟢

An ADT instance with any linear variant is itself linear:

```lain
type Resource {
    Active { mov handle *FILE }
    Closed
}
// Resource is linear because Active has a mov field
```

### 13.2 Consumption via Pattern Matching 🟢

`case` (pattern matching) is the primary way to consume linear ADTs:

```lain
case resource {
    Active(handle): fclose(handle)   // consumes handle
    Closed: { }                      // nothing to consume
}
// resource is consumed (all variants handled)
```

**Branch consistency**: Every branch must agree on the consumption state of variables from the enclosing scope (same rule as `if`/`else`).

### 13.3 Destructured Binding Ownership

When a variant is destructured, the extracted fields inherit the ownership mode of the original field:

```lain
case resource {
    Active(handle):
        // handle has type mov *FILE — it is linear
        // must be consumed before this arm exits
        fclose(mov handle)    // ✅ consumed
    Closed: { }
}
```

### 13.4 Non-Consuming Match 🟢

A match that borrows the scrutinee (shared) without consuming it uses the `case &expr` syntax:

```lain
// ✅ Implemented: shared match (non-consuming)
case &shape {
    Circle(r):    print_radius(r)    // r is a shared borrow of shape.radius
    Rectangle(w, h): print_dims(w, h)
}
// shape is still alive (not consumed)
```

**Implementation**: 🟢 Implemented. The parser recognizes `case &expr` syntax (both statement and expression forms). The `is_borrowed` flag on `StmtMatch`/`ExprMatch` AST nodes signals the linearity checker to skip consumption of the scrutinee and instead register a temporary shared borrow. The borrow is removed when the match completes.

---

## 14. Unsafe Code & The Escape Hatch

### 14.1 What `unsafe` Suspends

Inside an `unsafe` block, the following borrow checker rules are **relaxed**:

| Rule | Safe Code | Inside `unsafe` |
|------|-----------|-----------------|
| Pointer dereference | ❌ Forbidden | ✅ Allowed |
| Address-of (`&x`) | ❌ Forbidden | ✅ Allowed |
| ADT direct field access | ❌ Forbidden | ✅ Allowed |
| Linearity | ✅ Enforced | ✅ **Still enforced** |
| Borrow conflicts | ✅ Enforced | ✅ **Still enforced** |
| Move tracking | ✅ Enforced | ✅ **Still enforced** |

**Critical**: Linearity, borrow conflicts, and move tracking are **NOT** suspended by `unsafe`. Only raw pointer operations and certain type-unsafe accesses are unlocked.

### 14.2 Raw Pointers vs References

| Concept | References (borrows) | Raw Pointers |
|---------|---------------------|--------------|
| Created by | `f(var x)`, `return var x.field` | `&x` (unsafe), `malloc()` |
| Tracked by borrow checker | ✅ Yes | ❌ No |
| Can be null | ❌ No | ✅ Yes |
| Can dangle | ❌ Prevented | ✅ Possible |
| Dereference | Auto (struct fields) or `ref.*` (🔴 target) | `*ptr` (unsafe only) |

### 14.3 Safety Boundary Obligations 🔴

When raw pointers cross the safe/unsafe boundary, the programmer must uphold the following obligations:

1. **Non-null**: If a raw pointer is used to create a safe reference, it must not be null.
2. **Valid**: The pointed-to memory must be allocated and initialized.
3. **Aliasing**: The raw pointer must not alias with any active safe borrow.
4. **Lifetime**: The pointed-to memory must outlive the safe reference.

These obligations are **not checked** by the compiler. They are the programmer's responsibility.

---

## 15. Interaction with `defer`

### 15.1 Defer and Linearity 🟢

`defer` blocks are the primary RAII mechanism in Lain. They interact with the borrow checker as follows:

```lain
proc process_file() {
    var f = open_file("data.txt", "r")
    defer {
        close_file(mov f)          // consumes f
    }
    // ... use f (shared/mutable borrows) ...
    // When scope exits, defer block runs: f is consumed
}
```

The borrow checker treats the `defer` block's consumption as happening at scope exit. The variable `f` is not marked as consumed immediately; instead, the `defer` ensures it will be consumed before the scope ends.

### 15.2 Defer Ordering and Borrow Conflicts 🟢

Multiple `defer` blocks execute in LIFO order. The borrow checker tracks deferred consumption via `is_defer_consumed`:

```lain
var a = Resource(1)
var b = Resource(2)
defer { consume(mov a) }      // defer 1 — executes second
defer { consume(mov b) }      // defer 2 — executes first
// At scope exit: consume(mov b) runs, then consume(mov a)
// Both resources are consumed exactly once ✅
```

**Implementation**: 🟢 Implemented. The `STMT_DEFER` handler in `linearity.h` walks the defer body, detects any consumption inside it, marks the consumed variable with `is_defer_consumed = true`, then restores the variable to UNCONSUMED state so it remains usable in the main body. At scope exit, `ltable_ensure_all_consumed()` accepts variables that are defer-consumed.

### 15.3 Defer and Early Return 🟢

When a function has early returns, `defer` blocks ensure cleanup on all paths:

```lain
proc example() {
    var f = open_file("data.txt", "r")
    defer { close_file(mov f) }

    if error_condition {
        return                   // defer block runs, closing f ✅
    }
    use_file(f)
    return                       // defer block runs, closing f ✅
}
```

The compiler emits the defer block's code before each `return`, `break`, and `continue` statement that exits the defer's scope.

---

## 16. Interaction with Generics

### 16.1 Monomorphized Ownership 🔴

When Lain implements full generics (Phase C), the borrow checker must handle type parameters:

```lain
func swap(comptime T type, var a T, var b T) {
    var tmp = mov a.*       // 🔴 target syntax for deref
    a.* = mov b.*
    b.* = mov tmp
}
```

**Key rules**:
- If `T` is instantiated with a linear type, the monomorphized function must obey linearity rules.
- If `T` is instantiated with a copyable type, moves are implicit copies.
- The borrow checker runs **after** monomorphization — it sees concrete types, not type parameters.

### 16.2 Generic Containers and Ownership 🔴

Generic containers like `Option(T)` and `Result(T, E)` must propagate linearity:

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
// If T is linear → Option(T) is linear
// If T is copyable → Option(T) is copyable
```

The `sema_type_is_linear()` function already handles this recursively — no special generic handling is needed, as long as monomorphization produces concrete ADT types before the linearity check.

---

## 17. Diagnostic Requirements

### 17.1 Error Message Quality 🔴

The borrow checker's error messages should include:

1. **Where the borrow was created**: file, line, column of the borrow origin.
2. **What the borrow mode is**: shared vs mutable.
3. **Where the conflict occurs**: file, line, column of the conflicting access.
4. **When the borrow expires**: the last use of the binding (NLL info).
5. **Suggestion**: how to fix the issue.

**Target error format**:
```
error[E0502]: cannot borrow `data` as shared because it is mutably borrowed
  --> src/main.ln:5:20
   |
3  | var ref = get_ref(var data)
   |                  -------- mutable borrow of `data` occurs here
4  |
5  | var x = read_data(data)
   |                   ^^^^ shared borrow of `data` occurs here
...
7  | consume_ref(var ref)
   |             ------- mutable borrow is used here (last use)
   |
   = help: consider moving the use of `ref` before the access to `data`
```

### 17.2 Current Diagnostic State 🟢

Current error messages are minimal:
```
Error Ln 5, Col 20: cannot access 'data' because it is mutably borrowed by 'ref'.
```

This provides the basic information but lacks context about where the borrow was created and where it will be released. Improving diagnostics is a priority but does not affect soundness.

### 17.3 Warning Categories 🔴

The borrow checker should emit warnings for non-error patterns:

| Warning | Description |
|---------|-------------|
| Unused borrow | `var ref = get_ref(var data)` where `ref` is never used |
| Immediately released | A persistent borrow with `last_use == -1` (suggests the borrow is unnecessary) |
| Shadowed linear | A linear variable shadowed in an inner scope (outer variable may be leaked) |

---

## 18. Formal Model — Typing Rules

### 18.1 Judgment Form

The borrow checker's core judgments:

```
Γ; Σ; B ⊢ e : τ ⊣ Γ'; Σ'; B'
```

Where:
- **Γ** (Gamma) — Variable environment: maps names to types and ownership modes.
- **Σ** (Sigma) — Linear state table: maps linear variable names to {UNCONSUMED, CONSUMED}.
- **B** — Borrow set: set of active borrows (owner, mode, binding).
- **e** — Expression being checked.
- **τ** — Result type.
- **Γ', Σ', B'** — Updated environments after checking.

### 18.2 Key Rules

**Rule: Shared Borrow at Call Site**
```
Γ(x) = T   B has no mutable borrow of x
─────────────────────────────────────────
Γ; Σ; B ⊢ f(x) : ... ⊣ Γ; Σ; B ∪ {(x, SHARED, temp)}
```

**Rule: Mutable Borrow at Call Site**
```
Γ(x) = var T   B has no borrow of x
─────────────────────────────────────────
Γ; Σ; B ⊢ f(var x) : ... ⊣ Γ; Σ; B ∪ {(x, MUTABLE, temp)}
```

**Rule: Move at Call Site**
```
Γ(x) = mov T   Σ(x) = UNCONSUMED   B has no borrow of x
─────────────────────────────────────────
Γ; Σ; B ⊢ f(mov x) : ... ⊣ Γ; Σ[x ↦ CONSUMED]; B
```

**Rule: Statement Sequencing**
```
Γ; Σ; B ⊢ s₁ ⊣ Γ₁; Σ₁; B₁
  release_expired(B₁, i) = B₁'
  Γ₁; Σ₁; B₁' ⊢ s₂ ⊣ Γ₂; Σ₂; B₂
─────────────────────────────────────────
Γ; Σ; B ⊢ s₁; s₂ ⊣ Γ₂; Σ₂; B₂
```

**Rule: Branch Consistency**
```
Γ; Σ; B ⊢ then_branch ⊣ Γ₁; Σ₁; B₁
  Γ; Σ; B ⊢ else_branch ⊣ Γ₂; Σ₂; B₂
  ∀x ∈ dom(Σ). Σ₁(x) = Σ₂(x)          // Consistency check
─────────────────────────────────────────
Γ; Σ; B ⊢ if c { then } else { else } ⊣ Γ₁; Σ₁; B₁
```

### 18.3 Soundness Property

The borrow checker guarantees the following **soundness theorem** (informally):

> If a program type-checks under the borrow checker (no errors emitted), then at runtime:
> 1. No value is accessed after being moved.
> 2. No two mutable references to the same memory location exist simultaneously.
> 3. No mutable reference and shared reference to the same memory location exist simultaneously.
> 4. Every linear resource is consumed exactly once.
> 5. No reference outlives the value it borrows from.

**Caveat**: This guarantee holds only for safe code. `unsafe` blocks may violate any of these properties.

---

## 19. Implementation Architecture

### 19.1 Module Structure

```
src/sema/
├── region.h          // Region tree + BorrowTable + conflict detection
├── linearity.h       // LTable + expression/statement traversal
├── use_analysis.h    // NLL pre-pass (last-use computation)
├── scope.h           // Scope management (name resolution)
├── resolve.h         // Symbol resolution
└── typecheck.h       // Type checking + VRA
```

### 19.2 Data Flow

```
┌─────────────┐
│ Parser       │ → AST (Decl, Stmt, Expr trees)
└──────┬──────┘
       ▼
┌─────────────┐
│ Resolver     │ → Name resolution, scope building
│ (resolve.h)  │
└──────┬──────┘
       ▼
┌─────────────┐
│ Type Check   │ → Type inference, VRA, constraints
│ (typecheck.h)│
└──────┬──────┘
       ▼
┌─────────────┐
│ NLL Pre-pass │ → UseTable (last-use per identifier)
│ (use_analysis│
│  .h)         │
└──────┬──────┘
       ▼
┌─────────────┐
│ Linearity    │ → LTable + BorrowTable walk
│ (linearity.h)│   Checks: move, borrow, linearity, NLL release
└──────┬──────┘
       ▼
┌─────────────┐
│ Emitter      │ → C code generation
└─────────────┘
```

### 19.3 Complexity Analysis

| Operation | Current Complexity | Notes |
|-----------|-------------------|-------|
| `ltable_find(id)` | O(n) linear scan | Could be O(1) with hash map |
| `borrow_check_conflict(owner)` | O(b) where b = active borrows | Typically small |
| `borrow_release_expired(idx)` | O(b) scan all borrows | Called per statement |
| `use_compute_last_uses(body)` | O(S × E) where S=stmts, E=exprs per stmt | Single pass |
| `ltable_clone(table)` | O(n + b) | Called per if/match branch |
| **Total per function** | O(S × (n + b)) | Polynomial — no exponential blowup |

The entire borrow checker is **polynomial-time**: no backtracking, no SAT solving, no exponential search. This is a design goal of Lain.

---

## 20. Test Matrix

### 20.1 Required Test Coverage

Each rule in this specification should have at least one **positive test** (accepted program) and one **negative test** (rejected program).

| Rule | Positive Test | Negative Test | Status |
|------|--------------|---------------|--------|
| Multiple shared borrows | `borrow_pass.ln` | — | 🟢 |
| Shared + mutable conflict | — | `borrow_conflict_fail.ln` | 🟢 |
| Use after move | — | `use_after_move_fail.ln` | 🟢 |
| Double close (linear) | — | `double_close_fail.ln` | 🟢 |
| Forgot close (linear) | — | `forgot_close_fail.ln` | 🟢 |
| Move syntax required | — | `close_without_mov_fail.ln` | 🟢 |
| Block scope cleanup | `block_scope.ln` | `block_scope_fail.ln` | 🟢 |
| Sequential borrows (NLL) | `sequential_borrow_pass.ln` | — | 🟢 |
| Cross-stmt mutable borrow | — | `cross_stmt_borrow_fail.ln` | 🟢 |
| Cross-stmt write conflict | — | `cross_stmt_borrow_write_fail.ln` | 🟢 |
| NLL last-use release | `nll_last_use_pass.ln` | — | 🟢 |
| NLL still active | — | `nll_still_active_fail.ln` | 🟢 |
| NLL loop borrow | — | `nll_loop_borrow_fail.ln` | 🟢 |
| Return var dangling | — | `return_var_local_fail.ln` | 🟢 |
| Immutable reassignment | — | `immutable_fail.ln` | 🟢 |
| Re-borrow transitivity | `reborrow_pass.ln` | `reborrow_fail.ln` | 🟢 |
| Per-field linearity | `field_consume_pass.ln` | `field_consume_fail.ln` | 🟢 |
| Slice borrow tracking | `slice_borrow_pass.ln` | `slice_borrow_fail.ln` | 🔴 |
| Defer + linearity | `defer_linear_pass.ln` | `defer_linear_fail.ln` | 🟢 |
| Non-consuming match | `match_borrow_pass.ln` | — | 🟢 |

### 20.2 Stress Tests 🔴

Beyond unit tests, the borrow checker needs stress tests for:

- **Deep nesting**: 10+ levels of `if`/`for`/`while` with borrows at each level.
- **Many borrows**: 50+ active shared borrows of different variables.
- **Long borrow chains**: 5+ levels of re-borrow transitivity.
- **Large functions**: 500+ statements with complex ownership flow.

---

## 21. Phased Implementation Roadmap

### Phase 1 — Foundation (✅ Complete)

| Feature | Status | Files |
|---------|--------|-------|
| Intra-expression borrow conflict detection | ✅ | `region.h` |
| Linear type tracking (LTable) | ✅ | `linearity.h` |
| Move semantics (EXPR_MOVE, ltable_consume) | ✅ | `linearity.h` |
| Branch consistency checking | ✅ | `linearity.h` |
| Loop-depth consumption rule | ✅ | `linearity.h` |
| Temporary borrow clearing | ✅ | `region.h` |
| `return var local` dangling check | ✅ | `linearity.h` |
| Definite initialization analysis | ✅ | `linearity.h` |

### Phase 2 — NLL (✅ Complete)

| Feature | Status | Files |
|---------|--------|-------|
| Use-analysis pre-pass | ✅ | `use_analysis.h` |
| Persistent borrow registration with last-use | ✅ | `region.h`, `linearity.h` |
| Borrow release at last-use | ✅ | `region.h` |
| Owner access conflict with persistent borrows | ✅ | `region.h` |

### Phase 3 — Soundness Completion (✅ Complete)

| Feature | Status | Files |
|---------|--------|-------|
| Re-borrow transitivity (§10) | ✅ | `region.h` |
| Per-field linearity (§11.2, one-level) | ✅ | `linearity.h` |
| Per-field borrow tracking (§11.4) | ✅ | `region.h` |
| Two-phase borrows (RESERVED/ACTIVE) | ✅ | `region.h`, `linearity.h` |
| Non-consuming match `case &expr` (§13.4) | ✅ | `linearity.h`, `ast.h`, `parser/stmt.h`, `parser/expr.h` |
| Defer + linearity interaction (§15) | ✅ | `linearity.h` |
| Owner reassignment check (§4.6, full) | ✅ | `region.h` |

### Phase 4 — Precision & Ergonomics (🔴 Next)

| Feature | Priority | Estimated Lines | Dependencies |
|---------|----------|----------------|--------------|
| Block-level NLL (§6.5) | 🟡 Medium | ~300 | None |
| Reference dereference semantics (§9.5) | 🟡 Medium | ~200 | Design decision |
| Shared persistent borrows (§9.4) | 🟠 High | ~50 | None |
| Lifetime inference for multi-var-param (§5.5) | 🟠 High | ~100 | None |
| Rich diagnostics (§17) | 🟡 Medium | ~200 | None |
| Slice borrow tracking (§12.2) | 🟡 Low | ~100 | None |

### Phase 5 — Advanced (🟡 Long-term)

| Feature | Priority | Dependencies |
|---------|----------|--------------|
| CFG-based NLL (point-level) | Low | Phase 4 |
| Full path tracking (arbitrary depth) | Low | None |
| Concurrency model (Send/Sync) | Low | Language design |

---

## Appendix A: Glossary

| Term | Definition |
|------|------------|
| **Borrow** | A temporary permission to access a value without owning it |
| **Lifetime** | The span of program execution during which a borrow is valid |
| **Region** | A lexical scope in the program |
| **Linear type** | A type whose values must be consumed exactly once |
| **NLL** | Non-Lexical Lifetimes — borrows expire at last use, not scope end |
| **Persistent borrow** | A borrow that survives across statements (created by `return var`) |
| **Temporary borrow** | A borrow that expires at the end of the current statement |
| **Place** | A location in memory: a variable, field, or array element |
| **Consumption** | Transfer of ownership via `mov` (the value is invalidated at the source) |
| **Branch consistency** | Requirement that all branches of `if`/`match` agree on consumption state |
| **Soundness** | Property that the checker never accepts an invalid program |
| **Completeness** | Property that the checker never rejects a valid program (not guaranteed) |

---

## Appendix B: Differences from Rust

| Feature | Rust | Lain | Rationale |
|---------|------|------|-----------|
| Lifetime annotations (`'a`) | Required for complex signatures | Never required | Simplicity — Lain's structured control flow makes inference sufficient |
| `Copy` trait | Opt-in for types | Default for non-`mov` types | Simplicity — most types are copyable |
| `Drop` trait | Automatic cleanup | `defer` statement | Explicit > Implicit |
| Interior mutability (`Cell`, `RefCell`) | Available | Not yet | Future design space |
| Closures capturing borrows | Full support | Not yet | Requires closures feature |
| `Pin` | For self-referential types | Not needed | No async, no self-referential types |
| `Send`/`Sync` | Thread safety traits | Not needed yet | No concurrency |
| Borrow-through-match | `match &x` | 🟢 `case &expr` | §13.4 |
| Two-phase borrows | `v.push(v.len())` | 🟢 RESERVED/ACTIVE | Phase 3 |

---

*End of Specification*

