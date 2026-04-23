# Chapter 14 — Error Handling

## 14.1 Overview

Lain has no exceptions, no `try`/`catch`, and no stack unwinding. All error
handling is explicit through return values and algebraic data types. This
design ensures:

- **Zero runtime overhead** — No unwinding tables, no exception machinery.
- **Explicit error paths** — Every error condition is visible in the type.
- **Determinism** — Error handling follows the same control flow as normal code.

## 14.2 No Exceptions [Implemented]

> **CONSTRAINT:** Lain shall not provide exception-based error handling.
> There is no `throw`, `try`, `catch`, or `finally`.

Rationale:
- Exception handling requires stack unwinding machinery — incompatible with
  embedded and real-time targets.
- Exceptions create invisible control flow paths that undermine reasoning
  about program behavior.
- RAII-style cleanup can be achieved with `defer` (see §5.8).

## 14.3 Return Code Pattern [Implemented]

The simplest error handling pattern uses integer return codes:

```lain
proc try_operation() int {
    if failed { return -1 }
    return 0
}

proc main() {
    var result = try_operation()
    if result != 0 {
        println("Operation failed")
    }
}
```

This pattern is direct and has zero overhead, but it:
- Cannot carry additional error information.
- Can be accidentally ignored.
- Mixes success values with error codes.

## 14.4 The Option Type [Implemented]

The `Option` type represents an optional value — either present or absent:

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
```

### 14.4.1 Usage

```lain
type OptionInt = Option(int)

func find(arr int[10], target int) OptionInt {
    for i in 0..10 {
        if arr[i] == target { return OptionInt.Some(target) }
    }
    return OptionInt.None
}
```

### 14.4.2 Pattern Matching

The exhaustive `case` statement ensures all possibilities are handled:

```lain
case result {
    Some(value): use(value)
    None: handle_missing()
}
```

> The compiler's exhaustiveness checker (§5.7.4) ensures that `None` cannot
> be accidentally forgotten.

## 14.5 The Result Type [Implemented]

The `Result` type represents a computation that may succeed or fail:

```lain
func Result(comptime T type, comptime E type) type {
    return type {
        Ok { value T }
        Err { error E }
    }
}
```

### 14.5.1 Usage

```lain
type FileResult = Result(File, int)

proc try_open(path u8[:0]) FileResult {
    var raw = fopen(path.data, "r")
    if raw == 0 {
        return FileResult.Err(1)
    }
    return FileResult.Ok(File(raw))
}
```

### 14.5.2 Pattern Matching

```lain
var result = try_open("data.txt")
case result {
    Ok(f):    write_file(f, "hello")
    Err(code): libc_printf("Error: %d\n", code)
}
```

## 14.6 Cleanup with Defer [Implemented]

The `defer` statement provides deterministic cleanup without exceptions:

```lain
proc process_file() {
    var f = open_file("data.txt", "r")
    defer {
        close_file(mov f)
    }
    // ... use f ...
    // f is automatically closed on any exit path
}
```

Deferred statements execute in LIFO order on all exit paths (normal return,
early return, break, continue). See §5.8 for full specification.

## 14.7 Linear Types for Resource Safety [Implemented]

Linear types prevent resource leaks at compile time:

```lain
type File {
    mov handle *FILE         // File is linear — must be consumed
}

proc leak() {
    var f = open_file("data.txt", "r")
    // ERROR [E003]: linear variable 'f' was not consumed before end of scope
}
```

The ownership system ensures that every opened file is eventually closed,
every allocated buffer is eventually freed, and every acquired lock is
eventually released. See §7.6 for the full linear type specification.

## 14.8 Error Propagation [Planned]

### 14.8.1 The `?` Operator

A planned `?` operator would provide concise error propagation:

```lain
// Planned
proc read_config() ResultConfig {
    var f = try_open("config.txt")?  // propagates Err upward
    var data = read_all(f)?
    return ResultConfig.Ok(parse(data))
}
```

The `?` operator would:
1. Evaluate the expression (which must return a `Result` type).
2. If `Ok`, unwrap and continue.
3. If `Err`, return the error from the enclosing function.

### 14.8.2 Design Considerations

The `?` operator requires:
- A way to identify `Result`-like types (by convention or trait).
- Automatic conversion between error types.
- Integration with the ownership system (errors may contain linear values).

## 14.9 Summary

| Mechanism | Status | Overhead | Expressiveness |
|:----------|:-------|:---------|:---------------|
| Return codes | Implemented | Zero | Low |
| `Option` type | Implemented | Tag byte | Medium |
| `Result` type | Implemented | Tag byte | High |
| `defer` cleanup | Implemented | Zero | Medium |
| Linear types | Implemented | Zero (compile-time) | High |
| `?` propagation | Planned | Zero | High |

## 14.10 Diagnostic Code Reference [Implemented]

The compiler emits diagnostics tagged with a stable error code in the form
`[EXXX]`. Codes group by concern so a grep over stderr is sufficient to
triage a failure. The table below lists every currently assigned code.

| Code | Meaning | Pillar |
|:-----|:--------|:-------|
| `[E001]` | Use of linear variable after it was moved | P2 |
| `[E002]` | Linear variable already used/consumed (double-consume) | P2 |
| `[E003]` | Linear variable not consumed before end of scope or return | P2 |
| `[E004]` | Borrow conflict (read-write lock invariant violated) | P2 |
| `[E005]` | Use of uninitialized variable | P2/P3 |
| `[E006]` | Consuming a linear variable defined outside a loop from inside a loop | P2 |
| `[E007]` | Moving a linear variable requires explicit `mov` at the call site | P5 |
| `[E008]` | Cannot move while still borrowed, or after partial field consumption | P2 |
| `[E009]` | Cannot assign to an immutable variable | P5 |
| `[E010]` | Returning a mutable reference to a local variable (dangling) | P2 |
| `[E011]` | Purity violation (`func` calling `proc`, unbounded `while` in `func`, mutual recursion, modifying a global) | P4 |
| `[E012]` | Type/arity mismatch at call site or struct constructor | P3 |
| `[E013]` | Redeclaration, shadowing, assignment to undeclared identifier, or `main` typed as `func` | P5 |
| `[E014]` | Non-exhaustive match, comptime condition not constant, or CTFE recursion depth exceeded | P3/P4 |
| `[E015]` | Potential division by zero in a pure function | P2/P4 |
| `[E080]` | Bounded while: measure may be negative when the condition holds | P4 |
| `[E081]` | Bounded while: cannot extract variables from the termination measure | P4 |
| `[E082]` | Bounded while: body does not strictly decrease the measure | P4 |
| `[E100]` | Parse error (unexpected token, forbidden semicolon, malformed construct) | P5 |
| `[VRA]`  | Static bounds analysis rejected an index access | P2/P3 |

> Codes E016–E079 are reserved for future use. The `[EXXX]` prefix is
> part of the diagnostic contract — downstream tools may match it verbatim.

---

*This chapter is normative for implemented features. Sections marked [Planned]
describe future extensions and are not binding.*
