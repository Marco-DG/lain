# Design Note 010: Parameter Syntax Analysis (Infix vs Prefix)

**Topic**: Changing parameter declarations from `name modifier type` to `modifier name type`.
**Current**: `func foo(p var Point, x mov Resource)`
**Proposed**: `func foo(var p Point, mov x Resource)`

## 1. The Argument for Prefix (`var p T`)

### 1.1 Consistency with Declarations
*   **Variable Decl**: `var x = 10`
*   **Call Site**: `foo(var x)`
*   **Proposed Param**: `func(var x type)`
*   **Alignment**: In all three cases, `var` precedes the identifier `x`. This creates a strong visual pattern: "`var x` always means mutable x".

### 1.2 "English" Reading order
*   "Variable p of type Point" or "Mutable p of type Point".
*   Reads naturally left-to-right.

## 2. The Argument for Infix/Postfix (`p var T`) (Current)

### 2.1 "Name First" Principle (Go, Odin, Pascal, Rust)
*   Lain (and recent modern languages) prioritize the **name** over the type/storage class.
*   **Scanning**: When scanning a function signature, you want to see *what* the arguments are called first.
    *   `func(x int, y int, z int)` -> Names aligned.
    *   `func(x int, var y int, mov z int)` -> Names "zig-zag" slightly, but still start the clause.
*   **Proposed**: `func(x int, var y int, mov z int)` -> The names are `x`, `y`, `z`. But `y` and `z` are pushed right. In a long list, the names are not vertically aligned.

### 2.2 Type Integration
*   Currently, `var` is treated almost like part of the type.
    *   `p Point` -> p is a Point check.
    *   `p var Point` -> p is a pointer to Point.
    *   `p mov Point` -> p is an owned Point.
*   This matches how types work in C/C++ (`int`, `int*`), just with reversed name/type order.

## 3. Comparative Examples

### Scenario A: Mixed modifiers
**Current**:
```lain
func process(
    ctx    var Context,
    data   mov Data,
    config Config
)
```
*   **Pros**: Names (`ctx`, `data`, `config`) are perfectly aligned on the left.
*   **Cons**: Modifiers are in the middle.

**Proposed**:
```lain
func process(
    var ctx Context,
    mov data Data,
    config Config
)
```
*   **Pros**: Modifiers stand out at the start of the line.
*   **Cons**: Names (`ctx`, `data`, `config`) are jagged.

## 4. Technical Feasibility
Both are trivial to parse.
*   Current: `Expect(Id) -> Match(var/mov) -> ParseType`.
*   Proposed: `Match(var/mov) -> Expect(Id) -> ParseType`.

## 5. Recommendation

**Weak Preference for Current (`p var T`)**, but **Prefix (`var p T`) is acceptable.**

The "Name First" principle is usually preferred in modern syntactic design (Go, Odin, Kotlin, Swift's external names) to aid API discovery/readability.
However, if the user strongly values consistency with `var x = 10`, the Prefix style is a valid choice (C-style).

**Wait, slight correction on "Inconsistency"**:
Currently:
`var x = 10` (Declaration) vs `p var Point` (Parameter).
This **IS** inconsistent.
If we change to `var p Point`, we gain consistency with Declarations, but we lose "Name First".

**Alternative**: Postfix types with prefix modifiers?
Go does: `x *int`.
Lain could be: `p var Point`. This is what we have. It interprets `var` as a type prefix (pointer-like).

**Decision Framework**:
1.  Is `var` a property of the **name** (binding)? -> `var p Point`
2.  Is `var` a property of the **type** (reference)? -> `p var Point` (i.e. p has type `var Point`)

Given that local `var x` creates a mutable binding, and a parameter `var p` creates a mutable binding (reference), treating it as a property of the **name** (Option 1) is semantically sound.

**Verdict**: The proposal makes sense and increases consistency with `var x = 10` and `call(var x)`. The loss of "Name First" alignment is the trade-off.
