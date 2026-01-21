# Spec 004: Ownership, Linearity & Mutability (Detailed)

This specification refines the rules based on the strict checking logic of Lain and addresses contradictions found in previous drafts.

## 1. The Three Core Concepts

To understand the syntax, we must distinguish between three concepts that are often confused:
1.  **Binding Mutability**: Can I change which value `x` points to? (Reassignment: `x = new_value`)
2.  **Data Mutability**: Can I change the fields of the value `x` points to? (Modification: `x.field = 5`)
3.  **Linearity**: Must I use this value exactly once? (Resource Safety)

## 2. Declarations & Bindings

### 2.1 Immutable Binding (`val`)
The default. You create a value and give it a name. You cannot rebind it, nor modify it.
*   **Syntax**: `x = value` (Implicitly immutable)

```lain
p = Point{1, 2}
// p = Point{3, 4}  <-- ERROR: Cannot reassign immutable binding 'p'
// p.x = 5          <-- ERROR: Cannot modify immutable data 'p'
```

### 2.2 Mutable Binding (`var`)
You declare that this variable intends to change over time.
*   **Syntax**: `var x = value`
*   **Capability**: Grants the "Mutability" capability.

```lain
var p = Point{1, 2}
p = Point{3, 4}     // OK: Reassignment
p.x = 5             // OK: Modification
```

## 3. Resolving Your Doubts

### 3.1 Doubt 1: Reassigning Linear Variables
> *"In strict theory, I cannot drop a linear resource, so how can I reassign `r`?"*

**Rule**: You generally **cannot** simply overwrite a live linear variable.
If `r` holds an active resource (e.g., an open file), doing `r = Resource.new()` would verify "leak" (destroy) the old file, which strict linearity forbids (you must close it explicitly).

**Correct flow in Lain:**
To reassign a `var` linear variable, the previous value must be **consumed** (moved or closed) *before* the assignment.

```lain
var r = File.open("a.txt")
// r = File.open("b.txt")  <-- ERROR: Previous value of 'r' is not consumed! Leaking resource.

close(mov r)               // Correctly consume/close 'r'.
r = File.open("b.txt")     // OK: 'r' was effectively empty/consumed, safe to reuse name.
```
*So, `var` with linear types is rare and strict. Usually, you just use immutable bindings (`r1`, then `r2`).*

### 3.2 Doubt 2: Relationship between `var` and `mut`
> *"Why have both `var` and `mut`? What happens if I pass an immutable `u` to `rename(mut u)`?"*

**Answer**: `var` creates the **capability**. `mut` exercises the **action**.
You cannot perform a `mut` action on a variable that lacks the `var` capability.

```lain
// Case A: Immutable
u = User{name: "Bob"}   // Created without 'var'
// rename(mut u)        // ERROR: Cannot take mutable borrow of immutable binding 'u'.

// Case B: Mutable
var v = User{name: "Bob"} // Created with 'var'
rename(mut v)             // OK: 'v' has capability, 'mut' asserts intent.
```

**Why keep both?**
Because explicit is better than implicit at the call site.
*   If we just wrote `rename(v)`, it looks like a read-only call.
*   By forcing `rename(mut v)`, the reader knows: *"Attention! This function call modifies `v`!"*.
*   But `mut v` is only legal if `v` was declared with `var`. They validate each other.

### 3.3 Doubt 3: Destructuring Syntax
> *"You used `mov r = ...` in the example but said `mov` is not a declarator."*

**Correction**: You are absolutely right, that was a mistake in the previous text.
The syntax for destructuring should follow the standard binding rules (`x = ...` or `var x = ...`).

**Correct Destructuring:**
```lain
// Create a linear resource
r = Resource.new()

// Destructure (moves fields out)
// We just use intrinsic matching, no 'mov' keyword on the left.
{ ptr } = r  
// 'ptr' is now a new binding. 'r' is consumed.
```
If we want the specific fields to be mutable, we use `var`:
```lain
var { ptr } = r 
// ptr is mutable.
```

## 4. Comprehensive Syntax Table (Final)

| Concept | Syntax | Meaning | Valid Context |
| :--- | :--- | :--- | :--- |
| **Immutable Decl** | `x = val` | Define `x`. Cannot reassign, cannot mutate. | Global / Statement |
| **Mutable Decl** | `var x = val` | Define `x`. Can reassign, can mutate. | Statement |
| **Reassignment** | `x = val` | Update `x`. Only if `var`. If linear, old val must be consumed first. | Statement |
| **Shared Param** | `func f(x T)` | Read-Only View of `x`. default. | Function Param |
| **Mut Param** | `func f(x mut T)` | Mutable Reference. Caller sees changes. | Function Param |
| **Owned Param** | `func f(x mov T)` | Transfer Ownership. `x` is moved. | Function Param |
| **Call (Shared)**| `f(x)` | Borrow `x`. Safe. | Expression |
| **Call (Mut)** | `f(mut x)` | Borrow `x` mutably. Requires `var x`. | Expression |
| **Call (Move)** | `f(mov x)` | Move `x`. `x` becomes invalid. | Expression |

## 5. Extensive Examples

### 5.1 The Lifecycle of a Resource (Linearity)
```lain
type File { fd int } // Linear, non-copyable

func open(path string) mov File { ... }
func close(f mov File) { ... }
func write(f mut File, data string) { ... }

func main() {
    // 1. Creation (Owner)
    // Immutable binding 'f'. We own the file, but we won't swap 'f' for another file variable.
    f = open("log.txt") 
    
    // 2. Usage (Mutable Borrow)
    // Wait... if 'f' is immutable binding, can we modify the file content?
    // STRICT RULE: To use 'mut' borrow, binding must be 'var'.
    // So actually, we likely need 'var' if we want to mutate internal state via 'mut' reference?
    // NUANCE: 'mut' on the struct usually implies changing fields. writing to a file changes EXTERNAL state (OS), 
    // but often internal buffering too. So yes, let's require 'var'.
    
    // CORRECTED:
    var f2 = open("log.txt")
    write(mut f2, "Hello") // OK, explicit mutable borrow
    
    // 3. Destruction (Move)
    close(mov f2) // Explicit move. 'f2' is now uninitialized.
    
    // write(mut f2, "More") // ERROR: Use after move.
}
```

### 5.2 The Swapping Game (Reassignment)
```lain
func main() {
    var x = 10
    var y = 20
    
    // Reassign x
    x = 30 
    
    // Swap?
    // To swap using a temp variable:
    tmp = x
    x = y
    y = tmp
    // This works fine for primitives (Copy semantics).
}
```

### 5.3 Linear Swapping (Hard Mode)
```lain
func main() {
    var r1 = Resource.new() // id=1
    var r2 = Resource.new() // id=2
    
    // r1 = r2 
    // ^ ERROR: 'r1' is linear and currently holds id=1. Overwriting it would leak id=1.
    // ^ ERROR: 'r2' is linear. Copying it to 'r1' would duplicate ownership.
    
    // Correct Swap for Linear Types requires a swap function or destructuring
    // Or explicit destruction:
    destroy(mov r1) // r1 is now empty (conceptually void?)
    // Actually, Lain parser tracks "Initialization State".
    // After move, r1 is "Uninitialized".
    
    // So:
    var r3 = Resource.new()
    destroy(mov r3) // r3 is Uninitialized.
    r3 = mov r2     // OK: Assigning to uninitialized variable. r2 is moved to r3.
}
```

## 6. Conclusion
This draft should completely clarify the roles:
*   **`var` vs `val`**: Defined at creation. Determines valid operations (reassign/mutate).
*   **`mut` vs `mov`**: Operators at call site/signature. Mutate vs Consume.
*   **Linearity**: A type property that enforces "Must Use" and "No Drop".

This eliminates the "contradiction" by establishing a hierarchy: Type Rules > Binding Rules > Usage Rules.
