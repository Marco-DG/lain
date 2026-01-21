# Spec 003: Ownership & Syntax (Final Clean)

## 1. Philosophy: "Clean & Keyword-Driven"
This specification prioritizes readability and meaningful keywords over symbols. It adopts an "immutable by default" approach for simple bindings and explicit keywords for mutability and ownership transfer.

## 2. Variable Declarations

We distinguish between creating a **binding** (assigning a name) and the **properties** of that binding.

### 2.1 Immutable Binding (Default)
Standard binding. You cannot reassign `x`.
*   **Syntax**: `identifier = value`
*   **Behavior**:
    *   For **Primitive types** (int, bool): Value is copied.
    *   For **Linear types** (Resource): `x` becomes the Owner. You can *move* from `x` (consuming it), but you cannot *reassign* `x` to a new value.

```lain
x = 10
// x = 20  <-- ERROR: Immutable binding

r = Resource.new()
// r is the Owner.
// r = Resource.new() <-- ERROR: Immutable binding
```

### 2.2 Mutable Binding
Allows reassignment. Required if you want to swap the value held by the variable.
*   **Syntax**: `var identifier = value`

```lain
var y = 10
y = 20     // OK

var r2 = Resource.new()
r2 = Resource.other() // OK: Old resource 'r2' is dropped/consumed implicit?, new one replaces it.
// Note: In strict linear systems, implicit drop on reassign requires care. 
// Lain enforces "Must Use", so you might need to manually dispose valid linear values before reassignment.
```

## 3. Function Parameters (The Interface)

We use **Keywords** before the Type to specify the access mode. 

### 3.1 Shared Borrow (Implicit / Default)
*   **Syntax**: `func name(arg Type)`
*   **Meaning**: Read-Only access. Use this for 90% of parameters.
*   **Why implementation is not ambiguous**: The compiler knows `Point` is a type. If it sees just `Point`, it defaults to "Shared View". It does NOT mean "Copy" (like C) or "Move" (like C++ `std::move`). In Lain, `T` in arguments *always* means "Borrow".

```lain
// "I just want to look at the User"
func print_user(u User) {
    print(u.name)
    // u.name = "Bob" <-- ERROR: u is immutable (shared)
}
```

### 3.2 Mutable Borrow
*   **Syntax**: `func name(arg mut Type)`
*   **Meaning**: Read-Write access. The function will modify the caller's data.

```lain
// "I need to modify the User"
func rename(u mut User) {
    u.name = "Alice"
}
```

### 3.3 Ownership Transfer (Move)
*   **Syntax**: `func name(arg mov Type)`
*   **Meaning**: The function takes full ownership. The caller loses access.

```lain
// "I am taking this User forever (e.g. putting it in a DB)"
func save(u mov User) {
    db.insert(u)
    // u dies here
}
```

## 4. Call Site (Invoking Functions)

To keep the code explicit about *costs* and *side effects*, the call site mirrors the rules.

### 4.1 Passing Shared
No keyword needed. Default behavior.

```lain
u = User.new()
print_user(u) // OK, implicit borrow
```

### 4.2 Passing Mutable
Must say `mut` to warn the reader: "This function might change my variable!".

```lain
rename(mut u) // Explicit: I permit modification
```

### 4.3 Passing Ownership (Move)
Must say `mov` to warn the reader: "I am giving this away, I can't use it anymore".

```lain
save(mov u)   // Explicit: u is gone after this
// print_user(u) <-- ERROR: Use after move
```

## 5. Structs & Data

### 5.1 Defining Structs
Structs define the *shape* of data. Linearity is a property of the logical resource (does it have a destructor?), not necessarily the struct definition syntax, though `destruct` makes it Linear.

```lain
type User {
    name string
    age  int
}
```

### 5.2 Destructuring
To look inside a Linear type properly, we might need to destructure it.

```lain
mov r = Resource.new()
// If we want to dismantle 'r' into its parts:
mov { id } = r
// r is consumed. id is effectively a "mov int" (if int was linear) or just int.
```

## 6. Summary of Rules

| Action | Syntax | Meaning |
| :--- | :--- | :--- |
| **New Constant** | `x = 10` | Immutable binding |
| **New Variable** | `var x = 10` | Mutable binding |
| **Def: Shared Param** | `func f(a T)` | Read-Only View |
| **Def: Mut Param** | `func f(a mut T)` | Read-Write View |
| **Def: Owned Param** | `func f(a mov T)` | Transfer Ownership |
| **Call: Shared** | `f(x)` | Borrow |
| **Call: Mut** | `f(mut x)` | Allow modification |
| **Call: Move** | `f(mov x)` | Give away |

## 7. Addressing Your "Ambiguity" Concern
You asked: *"Is implicit shared borrow (`arg Type`) really ambiguous?"*

**Answer: No/Yes.**
*   **No**, functionally: The compiler can handle it perfectly. It's the cleanest options.
*   **Yes**, potentially for the programmer coming from C/C++: In C, `f(Point p)` copies the struct. In Lain, `f(Point p)` borrows it.
*   **Decision**: We chose cleanliness. `f(Point p)` borrows. If you want to copy a value, you explicitly make a new one or `mov` it (if it's a value type, `mov` is a copy). Actually, for primitive types (`int`), `func(i int)` copies. For aggregate types (`struct`), `func(s S)` borrows. 
    *   *Correction/Refinement*: To be truly consistent, `func(x T)` should **always** borrow. But borrowing an `int` is inefficient (pointer overhead). 
    *   *Pragmatic Rule*: Primitives (int, bool, float) Copy. Aggregates (struct, array) Borrow. This is how many high-level languages (like Java/Python) work implicitly, but Lain makes the referencing safer.

This spec (003) is the cleanest version yet. No symbols (`&`), just english keywords (`var`, `mut`, `mov`) in predictable places.
