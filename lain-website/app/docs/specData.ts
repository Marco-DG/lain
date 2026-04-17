import { SpecChapter } from '@/components/SpecViewer';

export const specData: SpecChapter[] = [
    {
        id: "overview",
        title: "00 — Overview & Pillars",
        content: "Lain is a statically typed, compiled programming language designed for systems programming, embedded systems, and safety-critical software. It achieves memory safety and resource safety entirely at compile time, with zero runtime overhead. Its design is governed by five non-negotiable pillars: <b>Assembly-Speed Performance</b>, <b>Zero-Cost Memory Safety</b>, <b>Static Verification</b>, <b>Determinism</b>, and <b>Syntactic Simplicity</b>. Lain compiles to C99, serving as a high-level safety layer over C's raw performance.",
        code: `// Conforming Lain code
proc main() {
    print("Assembly-Speed Performance, Zero-Cost Safety.")
}`
    },
    {
        id: "lexical",
        title: "01 — Lexical Structure",
        content: `
        <section data-code="${encodeURIComponent('// Line comment\n/* Nested /* block */ comment */\n\nvar x = 42\ny = "Lain System" // Implicit semicolon')}">
            Lain source files are UTF-8 encoded. The language supports line (//) and nested block (/* */) comments. Semicolons are optional, as newlines serve as implicit statement terminators.
        </section>
        <br><br>
        <section data-code="${encodeURIComponent('// Keywords demonstration\ntype File { mov handle int }\n\nproc main() {\n    var f = open_file("test.txt")\n    defer close_file(mov f)\n}')}">
            <b>Core Keywords:</b>
            <table>
                <thead>
                    <tr><th>Keyword</th><th>Purpose</th></tr>
                </thead>
                <tbody>
                    <tr><td><code>var</code></td><td>Mutable variable declaration</td></tr>
                    <tr><td><code>mov</code></td><td>Ownership transfer (move semantics)</td></tr>
                    <tr><td><code>type</code></td><td>Type definition (structs, ADTs)</td></tr>
                    <tr><td><code>func</code></td><td>Pure function declaration</td></tr>
                    <tr><td><code>proc</code></td><td>Procedure declaration (side effects)</td></tr>
                    <tr><td><code>case</code></td><td>Pattern matching</td></tr>
                    <tr><td><code>unsafe</code></td><td>Unsafe block escape hatch</td></tr>
                    <tr><td><code>defer</code></td><td>LIFO resource cleanup</td></tr>
                </tbody>
            </table>
        </section>`,
        code: `// Line comment
/* Nested /* block */ comment */

var x = 42
y = "Lain System" // Implicit semicolon`
    },
    {
        id: "types",
        title: "02 — Type System",
        content: `Lain is strictly typed with no implicit narrowing. Primitive types follow C99 binary representation for maximum portability.
        <br><br>
        <b>Primitive Integers:</b>
        <table>
            <thead>
                <tr><th>Type</th><th>Description</th><th>C Equivalent</th></tr>
            </thead>
            <tbody>
                <tr><td><code>i32</code></td><td>Signed 32-bit</td><td><code>int32_t</code></td></tr>
                <tr><td><code>u8</code></td><td>Unsigned 8-bit</td><td><code>uint8_t</code></td></tr>
                <tr><td><code>int</code></td><td>Platform-native</td><td><code>int</code></td></tr>
                <tr><td><code>usize</code></td><td>Pointer-sized</td><td><code>size_t</code></td></tr>
            </tbody>
        </table>`,
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
        content: "Lain enforces a clear distinction between immutable and mutable bindings. <code>var</code> creates a mutable variable, while simple assignment creates an immutable one. Shadowing is allowed. The <code>undefined</code> keyword marks explicit uninitialized memory, which is verified by Definite Initialization Analysis to prevent reads before writes.",
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
        content: `Expressions are evaluated left-to-right. Plain operators work only on identical types; <code>as</code> performs explicit casting.
        <br><br>
        <b>Core Operators:</b>
        <table>
            <thead>
                <tr><th>Operator</th><th>Description</th></tr>
            </thead>
            <tbody>
                <tr><td><code>+ - * / %</code></td><td>Arithmetic</td></tr>
                <tr><td><code>== != < ></code></td><td>Comparison</td></tr>
                <tr><td><code>and or !</code></td><td>Logical</td></tr>
                <tr><td><code>& | ^ ~</code></td><td>Bitwise</td></tr>
                <tr><td><code>in</code></td><td>Bounds check / proof</td></tr>
            </tbody>
        </table>`,
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
        content: "Lain supports standard control flow: if/elif/else, for loops (range-based), and while loops. <code>while</code> is restricted to <code>proc</code> unless it has a <code>decreasing</code> termination measure. <code>defer</code> provides LIFO resource cleanup. <code>case</code> enables exhaustive pattern matching on ADTs, enums, and integers.",
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
        content: "Lain strictly separates pure functions (<code>func</code>) from impure procedures (<code>proc</code>). <code>func</code> is deterministic, guaranteed to terminate, and cannot call <code>proc</code> or access global state. <code>proc</code> allows side effects, recursion, and global access. This split enables formal reasoning about program behavior.",
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
        content: `Lain uses a linear type system to ensure memory safety. Every value has a single owner.
        <br><br>
        <b>Ownership Modes:</b>
        <table>
            <thead>
                <tr><th>Mode</th><th>Syntax</th><th>Semantics</th></tr>
            </thead>
            <tbody>
                <tr><td>Shared</td><td><code>p T</code></td><td>Read-only (Multi)</td></tr>
                <tr><td>Mutable</td><td><code>var p T</code></td><td>Read-Write (Exclusive)</td></tr>
                <tr><td>Owned</td><td><code>mov p T</code></td><td>Move (Transfer)</td></tr>
            </tbody>
        </table>`,
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
        content: "Value Range Analysis (VRA) tracks integer intervals [min, max] at compile time. Constraints like <code>x int != 0</code> are verified statically with zero runtime overhead. Array indexing is proven safe via VRA or <code>in-guards</code>. The analysis is polynomial-time and decidable, avoiding the unpredictability of SMT solvers.",
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
        content: "Lain uses compile-time evaluation (CTFE) for generics. Functions can accept <code>comptime T type</code> and return a new <code>type</code>. The compiler monomorphizes these calls, generating specialized C code for each type instantiation. This ensures generics have no runtime performance cost.",
        code: `func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}

type OptInt = Option(int)`
    },
    {
        id: "interop",
        title: "10 — C Interoperability",
        content: "Lain provides direct C99 interop. <code>c_include</code> injects headers, and <code>extern</code> declares C functions. Ownership annotations can be used on extern symbols (e.g., <code>extern func malloc(...) mov *void</code>), allowing the borrow checker to track C-allocated resources.",
        code: `c_include "<stdio.h>"
extern type FILE
extern proc fopen(p *u8, m *u8) mov *FILE

var f = fopen("test.txt", "r")`
    },
    {
        id: "unsafe",
        title: "11 — Unsafe Code",
        content: "The <code>unsafe</code> block is a controlled escape hatch for raw pointer dereferencing and bypassing ADT tag checks. It does NOT disable ownership or borrow checking — only specific low-level operations are unlocked. The programmer assumes responsibility for memory validity inside unsafe blocks.",
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
        title: "12 — Memory Model",
        content: "Lain uses a deterministic, stack-first memory model. All local variables are destroyed in reverse declaration order (LIFO). Heap memory is managed via manual protocols (malloc/free) that are verified by the linear type system to ensure every allocation is freed exactly once.",
        code: `var buf = malloc(1024)
defer { free(mov buf) } // guaranteed cleanup`
    },
    {
        id: "errors",
        title: "13 — Error Handling",
        content: "Lain rejects exceptions. Error handling is explicit via Option and Result ADTs. This makes all error paths visible in the type system. Pattern matching ensures that error cases are never ignored, and <code>defer</code> handles zero-overhead cleanup regardless of the error path taken.",
        code: `type Res = Result(int, i8)

func try_parse(s u8[:0]) Res {
    if s.len == 0 { return Res.Err(1) }
    return Res.Ok(42)
}`
    },
    {
        id: "stdlib",
        title: "14 — Standard Library",
        content: "The Lain standard library (<code>std</code>) provides core primitives for C bindings, console I/O, resource-safe filesystem access, and pure mathematical utilities. It is designed to be minimal, safe, and efficient, following all of Lain's safety and purity rules.",
        code: `import std.fs
import std.math

proc main() {
    var f = open_file("out.txt", "w")
    defer { close_file(mov f) }
    var x = abs(0 - 5)
}`
    }
];
