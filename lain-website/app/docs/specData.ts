export interface SpecChapter {
    id: string;
    title: string;
    content: string;
    code?: string;
}

export const specData: SpecChapter[] = [
    {
        id: "overview",
        title: "00 — Overview & Pillars",
        content: "Lain is a statically typed, compiled programming language designed for systems programming, embedded systems, and safety-critical software. It achieves memory safety and resource safety entirely at compile time, with zero runtime overhead. Its design is governed by five non-negotiable pillars: Assembly-Speed Performance, Zero-Cost Memory Safety, Static Verification, Determinism, and Syntactic Simplicity. Lain compiles to C99, serving as a high-level safety layer over C's performance.",
        code: `// Conforming Lain code
proc main() {
    print("Assembly-Speed Performance, Zero-Cost Safety.")
}`
    },
    {
        id: "lexical",
        title: "01 — Lexical Structure",
        content: "Lain source files are UTF-8 encoded. The language supports line (//) and nested block (/* */) comments. Identifiers follow [a-zA-Z_][a-zA-Z0-9_]* and are case-sensitive. Semicolons are optional, as newlines serve as implicit statement terminators. Keywords like 'func', 'proc', 'var', and 'mov' are reserved. Escape sequences like \\n, \\t, and \\0 are supported in character and string literals.",
        code: `// Line comment
/* Nested /* block */ comment */

var x = 42
y = "Lain System" // Implicit semicolon`
    },
    {
        id: "types",
        title: "02 — Type System",
        content: "Lain is strictly typed with no implicit narrowing. Primitive types include fixed-width integers (i8, u16, i32, u64), floats (f32, f64), and bool. The 'int' type is platform-dependent (typically 32-bit). Structs are defined with 'type' and can be linear if they contain 'mov' fields. Algebraic Data Types (ADTs) provide safe tagged unions. Slices (T[]) and sentinel slices (T[:S]) offer safe views into memory.",
        code: `type Point { x i32, y i32 }
type Shape {
    Circle { radius int }
    Point
}

var p = Point(10, 20)
var s = Shape.Circle(5)`
    },
    {
        id: "declarations",
        title: "03 — Declarations & Mutability",
        content: "Lain enforces a clear distinction between immutable and mutable bindings. 'var' creates a mutable variable, while simple assignment creates an immutable one. Shadowing is allowed. The 'undefined' keyword marks explicit uninitialized memory, which is verified by Definite Initialization Analysis to prevent reads before writes.",
        code: `var x = 10 // mutable
y = 20     // immutable binding

proc example() {
    var x = 5 // shadows outer x
    var buf u8[256] = undefined
    buf[0] = 1 // initialized before use
}`
    },
    {
        id: "expressions",
        title: "04 — Expressions & Operators",
        content: "Expressions are evaluated left-to-right. Plain operators (+, -, *, /, %) work only on identical types; 'as' performs explicit casting. The 'in' operator checks array bounds. 'mov' and 'var' are expression-level operators for ownership control. Universal Function Call Syntax (UFCS) allows 'x.f()' as sugar for 'f(x)'.",
        code: `var x = 10 as u8
var y = x + 5 // 5 inferred as u8
var ok = 10.is_even() // UFCS: is_even(10)

if idx in arr {
    var val = arr[idx]
}`
    },
    {
        id: "statements",
        title: "05 — Statements & Control Flow",
        content: "Lain supports standard control flow: if/elif/else, for loops (range-based), and while loops. 'while' is restricted to 'proc' unless it has a 'decreasing' termination measure. 'defer' provides LIFO resource cleanup. 'case' enables exhaustive pattern matching on ADTs, enums, and integers.",
        code: `if x > 0 {
    defer { cleanup() }
    for i in 0..10 {
        if i == 5 { break }
    }
}

case shape {
    Circle(r): print(r)
    else: print("Other")
}`
    },
    {
        id: "functions",
        title: "06 — Functions vs Procedures",
        content: "Lain strictly separates pure functions (func) from impure procedures (proc). 'func' is deterministic, guaranteed to terminate, and cannot call 'proc' or access global state. 'proc' allows side effects, recursion, and global access. This split enables formal reasoning about program behavior.",
        code: `func add(a int, b int) int {
    return a + b // Pure, terminates
}

proc log(msg u8[:0]) {
    libc_printf("%s\\n", msg.data) // Side effect
}`
    },
    {
        id: "ownership",
        title: "07 — Ownership & Borrowing",
        content: "Lain uses a linear type system. 'mov' transfers ownership, invalidating the source. The borrow checker enforces a Read-Write Lock invariant: either many shared borrows or one mutable borrow (var) per variable. Non-Lexical Lifetimes (NLL) expire borrows at their last use. Two-phase borrows allow methods like 'v.push(v.len)'.",
        code: `var f = open_file("a.txt")
close_file(mov f) // ownership transfer
// f is now invalid

var x = 10
mutate(var x) // mutable borrow
read(x)       // shared borrow`
    },
    {
        id: "constraints",
        title: "08 — Type Constraints & VRA",
        content: "Value Range Analysis (VRA) tracks integer intervals [min, max] at compile time. Constraints like 'x int != 0' are verified statically with zero runtime overhead. Array indexing is proven safe via VRA or 'in-guards'. The analysis is polynomial-time and decidable, avoiding the unpredictability of SMT solvers.",
        code: `func safe_div(a int, b int != 0) int {
    return a / b
}

func get(arr int[10], i int in arr) int {
    return arr[i] // proven i in [0, 9]
}`
    },
    {
        id: "generics",
        title: "09 — Generics & CTFE",
        content: "Lain uses compile-time evaluation (CTFE) for generics. Functions can accept 'comptime T type' and return a new 'type'. The compiler monomorphizes these calls, generating specialized C code for each type instantiation. This ensures generics have no runtime performance cost.",
        code: `func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}

type OptInt = Option(int)`
    },
    {
        id: "modules",
        title: "10 — Module System",
        content: "Lain uses a dot-separated module system (e.g., 'import std.io') that maps to the filesystem. It follows a unity build model: all imports are fused into a single AST. Symbols are mangled with their module prefix in the generated C code to prevent collisions.",
        code: `import std.io
import math.vec as v

proc main() {
    std.io.print("Lain Modules")
}`
    },
    {
        id: "interop",
        title: "11 — C Interoperability",
        content: "Lain provides direct C99 interop. 'c_include' injects headers, and 'extern' declares C functions. Ownership annotations can be used on extern symbols (e.g., 'extern func malloc(...) mov *void'), allowing the borrow checker to track C-allocated resources.",
        code: `c_include "<stdio.h>"
extern type FILE
extern proc fopen(p *u8, m *u8) mov *FILE

var f = fopen("test.txt", "r")`
    },
    {
        id: "unsafe",
        title: "12 — Unsafe Code",
        content: "The 'unsafe' block is a controlled escape hatch for raw pointer dereferencing and bypassing ADT tag checks. It does NOT disable ownership or borrow checking — only specific low-level operations are unlocked. The programmer assumes responsibility for memory validity inside unsafe.",
        code: `var x = 10
unsafe {
    var p = &x // address-of
    *p = 20    // dereference
}

// ADT field bypass
unsafe { var r = shape.Circle.radius }`
    },
    {
        id: "memory",
        title: "13 — Memory Model",
        content: "Lain uses a deterministic, stack-first memory model. All local variables are destroyed in reverse declaration order (LIFO). Heap memory is managed via manual protocols (malloc/free) that are verified by the linear type system to ensure every allocation is freed exactly once.",
        code: `var buf = malloc(1024)
defer { free(mov buf) } // guaranteed cleanup`
    },
    {
        id: "errors",
        title: "14 — Error Handling",
        content: "Lain rejects exceptions. Error handling is explicit via Option and Result ADTs. This makes all error paths visible in the type system. Pattern matching ensures that error cases are never ignored, and 'defer' handles zero-overhead cleanup regardless of the error path taken.",
        code: `type Res = Result(int, i8)

func try_parse(s u8[:0]) Res {
    if s.len == 0 { return Res.Err(1) }
    return Res.Ok(42)
}`
    },
    {
        id: "stdlib",
        title: "15 — Standard Library",
        content: "The Lain standard library ('std') provides core primitives for C bindings, console I/O, resource-safe filesystem access, and pure mathematical utilities. It is designed to be minimal, safe, and efficient, following all of Lain's safety and purity rules.",
        code: `import std.fs
import std.math

proc main() {
    var f = open_file("out.txt", "w")
    defer { close_file(mov f) }
    var x = abs(0 - 5)
}`
    },
    {
        id: "appendix-a",
        title: "App. A — Keyword Reference",
        content: "Active keywords include: var, mov, func, proc, type, case, in, and, or, as, extern, comptime, unsafe, import, defer, true, false, undefined. Reserved for future use: macro, pre, post, use, end, export, expr.",
        code: `/* Selection */
mov x    // Ownership
var y    // Mutability
func f() // Purity
proc p() // Impurity`
    },
    {
        id: "rationale",
        title: "App. B — Design Rationale",
        content: "Lain prioritizes auditability. By using VRA instead of SMT, safety proofs are fast and predictable. By separating func/proc, termination and purity are guaranteed. Every decision, from explicit 'mov' at call sites to C99 compilation, serves the goal of absolute control.",
        code: `// Why explicit mov?
take(mov resource) // Readable consumption`
    },
    {
        id: "comparison",
        title: "App. C — Comparisons",
        content: "Lain is safer than C (prevents buffer overflows and leaks), simpler than Rust (no lifetime annotations, uses implicit NLL), and more formal than Zig (has a borrow checker). It occupies a unique niche for safety-critical embedded systems.",
        code: `/* Safety Spectrum */
// C < Zig < Rust < Lain (Verification)`
    },
    {
        id: "roadmap",
        title: "App. D — Evolution Roadmap",
        content: "Planned features: Error propagation operator (?), inclusive ranges (..=), native 'char' type for better C interop, trait-based composition, and stackless async. The core focus remains on zero-cost safety and embedded excellence.",
        code: `// Future syntax:
var val = try_op()?
type Show = trait { func show() }`
    }
];
