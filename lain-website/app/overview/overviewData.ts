import { SpecChapter } from '@/components/SpecViewer';

export const overviewData: SpecChapter[] = [
    {
        id: "overview",
        title: "00 — Overview & Pillars",
        content: "Lain is a statically typed, compiled programming language designed for systems programming and safety-critical software. It achieves memory safety and resource safety entirely at compile time, with zero runtime overhead. Its design is governed by five pillars: Assembly-Speed Performance, Zero-Cost Memory Safety, Static Verification, Determinism, and Syntactic Simplicity.",
        code: `// Conforming Lain code\nproc main() {\n    print("Hello, systems programming")\n}`
    },
    {
        id: "lexical",
        title: "01 — Lexical Structure",
        content: `
        <section data-code="${encodeURIComponent('// Line comment\n/* Nested /* block */ comment */\n\nvar x = 42\ny = "Lain System" // Implicit semicolon')}">
            Lain source files are UTF-8 encoded. The language uses maximal munch for tokenization and supports line (//) and nested block (/* */) comments. Semicolons are optional, as newlines serve as implicit statement terminators.
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
        code: `// Line comment\n/* Nested /* block */ comment */\n\nvar x = 42\ny = "string" // implied semicolon`
    },
    {
        id: "types",
        title: "02 — Type System",
        content: "Lain is strictly typed and forbids all implicit conversions. Primitives follow C99 binary representation: i32 is 4-byte two's complement, f64 is IEEE 754. Nominal type equivalence is used: two types are identical only if they share the same name. This rigor ensures that low-level data layouts are predictable and portable.",
        code: `type Point { x i32, y i32 }\ntype Vector { x i32, y i32 }\n\nvar p = Point(1, 2)\n// var v Vector = p // ERROR: Nominal mismatch`
    },
    {
        id: "integers",
        title: "03 — Primitive Integers",
        content: `Primitive types follow C99 binary representation for maximum portability. No implicit narrowing is allowed.
        <br><br>
        <b>Integer Map:</b>
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
        code: `var a u8 = 255\nvar b i32 = a as i32 // Explicit widening required`
    },
    {
        id: "adt",
        title: "04 — Structs & ADTs",
        content: "Structs are packed with natural alignment, ensuring zero padding between fields where possible. Algebraic Data Types (ADTs) provide safe variants, where only one field is active at a time. The compiler enforces exhaustive checking when accessing ADT variants to prevent invalid state access.",
        code: `type Point { x i32, y i32 }\n\ntype Shape {\n    Circle { radius int }\n    Point\n}\n\nvar s = Shape.Circle(5)`
    },
    {
        id: "mutability",
        title: "05 — Mutability Principles",
        content: "Lain enforces a clear distinction between immutable and mutable bindings. A binding is immutable by default unless declared with 'var'. This extends to pointers and borrows: you cannot mutate a value unless you hold a mutable reference (var p T). This distinction is the foundation of the linearity analysis.",
        code: `x = 10     // Immutable binding\nvar y = 20 // Mutable variable\n\n// y = 30  // OK\n// x = 11  // ERROR: Immutable`
    },
    {
        id: "scoping",
        title: "06 — Declarations & Scoping",
        content: "Lain uses lexical block scoping and supports shadowing. Top-level declarations (functions, types) are order-independent, while local variables must be declared before use. Definite initialization analysis ensures that 'undefined' variables are never read before being explicitly written to.",
        code: `var x = 10\nproc scope() {\n    var x = 5 // shadows outer x\n    {\n        var z = x + 1\n    } // z dies here\n}`
    },
    {
        id: "expressions",
        title: "07 — Expressions",
        content: "Expressions are strictly evaluated left-to-right. All operators require operand types to be identical. Move (mov) and mutable borrow (var) can be used as expression-level operators to signal ownership transitions or permission shifts directly within complex statements.",
        code: `var x = 10 as u8\nvar y = x + 5 // 5 inferred as u8\n\nconsume(mov x) // x is invalidated`
    },
    {
        id: "operators",
        title: "08 — Core Operators",
        content: `Operators are grouped by priority and semantics. Bitwise and logical operators are distinct.
        <br><br>
        <b>Operator Map:</b>
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
        code: `var ok = 10.is_even() // UFCS notation\nif idx in arr {\n    var v = arr[idx] // verified safe\n}`
    },
    {
        id: "statements",
        title: "09 — Statements",
        content: "Statements are terminated by newlines or semicolons. Control flow is designed to be deterministic. 'while' loops are restricted to procedures unless a termination measure is proven. This prevents pure functions from entering infinite loops, facilitating formal verification.",
        code: `if x > 0 {\n    print("positive")\n} else {\n    print("non-positive")\n}`
    },
    {
        id: "controlflow",
        title: "10 — Control Flow",
        content: "Lain supports standard control flow with extra safety. Range-based 'for' loops iterate over intervals, where the bounds are checked by the compiler. 'break' and 'continue' provide loop control. The absence of 'goto' ensures a clean, reducible control flow graph (CFG).",
        code: `for i in 0..10 {\n    if i == 5 { break }\n    print(i)\n}`
    },
    {
        id: "case",
        title: "11 — Pattern Matching (Case)",
        content: "The 'case' statement enables exhaustive pattern matching on ADTs, enums, and integers. The compiler verifies that all possible branches are handled. This eliminates 'unhandled variant' bugs common in C union types and ensures total functions over data structures.",
        code: `case shape {\n    Circle(r): print(r)\n    Point: print("dot")\n    else: print("other")\n}`
    },
    {
        id: "functions",
        title: "12 — Functions vs Procedures",
        content: "Lain strictly separates pure functions (func) from procedures (proc). Pure functions are deterministic and total; they cannot have side effects, call procedures, access global state, or recurse. Procedures are for I/O, state mutation, and non-deterministic logic.",
        code: `func add(a int, b int) int {\n    return a + b\n}\n\nproc log(msg string) {\n    libc_printf("%s\\n", msg)\n}`
    },
    {
        id: "purity",
        title: "13 — Functional Purity",
        content: "Purity is enforced to enable aggressive compiler optimizations and formal reasoning. Since pure functions have no side effects, their calls can be reordered, memoized, or even removed if the result is unused, without changing program behavior.",
        code: `func complex(a int) int {\n    // No globals, no side effects\n    return a * a + 2\n}`
    },
    {
        id: "ownership",
        title: "14 — Ownership Model",
        content: "Memory safety is achieved through a strict ownership model. Every value has a single owner. When the owner goes out of scope, the value is destroyed. This system eliminates data races and use-after-free bugs at compile time with zero runtime cost.",
        code: `var f = open_file("a.txt")\nclose_file(mov f) // ownership transfer\n// f is now invalid`
    },
    {
        id: "borrowing",
        title: "15 — Borrowing (Shared)",
        content: "Shared borrows allow multiple readers but no writers. A shared reference (p T) is a pointer that guarantees the underlying data will not change as long as the reference is alive. The borrow checker ensures that no mutable references exist while shared references are active.",
        code: `var x = 10\nref = &x // shared borrow\nprint(*ref)\n// x = 20 // ERROR: active borrow`
    },
    {
        id: "mutborrowing",
        title: "16 — Borrowing (Mutable)",
        content: "Mutable borrows (var p T) provide exclusive read-write access. Only one mutable reference can exist at a time for any given data. This exclusivity prevents data races and ensures that mutation is always safe and predictable.",
        code: `var x = 10\nproc mutate(var p int) {\n    *p += 1\n}\nmutate(var x)`
    },
    {
        id: "move",
        title: "17 — Move Semantics",
        content: "Transferring ownership is performed via the 'mov' operator. Moving a value invalidates the source variable, preventing use-after-move errors. This is used for passing values into procedures that take ownership (like 'free' or 'close_file').",
        code: `var buf = malloc(1024)\nconsume(mov buf)\n// buf[0] = 1 // ERROR: moved`
    },
    {
        id: "memory",
        title: "18 — Memory Model",
        content: "Lain uses a deterministic, stack-first memory model. Variables are destroyed in the exact reverse order of their declaration (LIFO). This predictable destruction order is used to manage resources like file handles and memory buffers safely.",
        code: `var a = Resource()\nvar b = Resource()\n// b destroyed, then a`
    },
    {
        id: "defer",
        title: "19 — LIFO Defer",
        content: "The 'defer' statement schedules a block of code to run at the end of the current scope. Multiple defers are executed in LIFO (Last-In-First-Out) order. This is perfect for ensuring resources are freed regardless of how a function exits.",
        code: `var f = open_file("t.txt")\ndefer { close_file(mov f) }\n// process file...`
    },
    {
        id: "constraints",
        title: "20 — Verification & Constraints",
        content: "Constraints like 'x int != 0' are verified statically at compile time. This allows the compiler to prove that certain error conditions (like division by zero) can never happen at runtime, eliminating the need for expensive runtime checks.",
        code: `func safe_div(a int, b int != 0) int {\n    return a / b\n}`
    },
    {
        id: "vra",
        title: "21 — Value Range Analysis",
        content: "Value Range Analysis (VRA) tracks the intervals of integer values through the code. By knowing that 'i' is always in the range [0, 9], the compiler can prove that 'arr[i]' is a safe operation if 'arr' has size 10.",
        code: `var arr int[10]\nfor i in 0..10 {\n    arr[i] = 0 // proven safe\n}`
    },
    {
        id: "indexsafety",
        title: "22 — Index & Bounds Safety",
        content: "Bounds checking in Lain is proactive. Instead of checking every access at runtime, the compiler uses VRA and 'in-guards' to prove safety. If the compiler cannot prove safety, it rejects the program, forcing the developer to add a check or constraint.",
        code: `if idx in arr {\n    var v = arr[idx] // provably safe\n}`
    },
    {
        id: "generics",
        title: "23 — Generics & CTFE",
        content: "Generics utilize monomorphization: the compiler generates specialized C code for every distinct type argument. This preserves performance as no runtime dispatch occurs. Pure functions can be evaluated at compile time to compute types or constants.",
        code: `func Option(comptime T type) type {\n    return type { Some { v T }, None }\n}`
    },
    {
        id: "monomorphization",
        title: "24 — Monomorphization",
        content: "Monomorphization means that 'List(int)' and 'List(f32)' become two separate, optimized structs in the generated C code. This leads to code that is as fast as hand-written C but with the safety and abstraction of high-level generics.",
        code: `type IntList = List(int)\ntype FloatList = List(f32)`
    },
    {
        id: "ctfe",
        title: "25 — Compile-Time Logic",
        content: "CTFE (Compile-Time Function Execution) allows complex logic to be performed during compilation. This can be used for building custom data structures or calculating lookup tables that are embedded directly into the binary.",
        code: `comptime var table = generate_sine_table()\nvar val = table[i]`
    },
    {
        id: "modules",
        title: "26 — Module System",
        content: "Lain uses a dot-separated module system that maps to the filesystem. To prevent name collisions, identifiers are mangled with their module path prefix. This allows for a clean, hierarchical namespace without the overhead of dynamic linking.",
        code: `import std.io\nstd.io.print("hello")`
    },
    {
        id: "interop",
        title: "27 — C Interoperability",
        content: "Lain compiles directly to C99, making interop trivial. The 'c_include' directive allows including C headers directly, while 'extern' declarations expose C functions and types to Lain code with zero overhead.",
        code: `c_include "<math.h>"\nextern func cos(f f64) f64`
    },
    {
        id: "extern",
        title: "28 — Extern Declarations",
        content: "Extern declarations can be enriched with Lain's ownership and constraint annotations. This allows the borrow checker to track resources allocated by C libraries and the VRA to understand the constraints of C function parameters.",
        code: `extern proc malloc(s usize) mov *void`
    },
    {
        id: "stdlib",
        title: "29 — Standard Library",
        content: "The standard library (std) provides safe, Lain-idiomatic wrappers around common systems operations. It includes modules for IO, file systems, math, and core memory primitives, all verified by the borrow checker.",
        code: `import std.fs\nvar f = open_file("data.bin", "rb")`
    },
    {
        id: "unsafe",
        title: "30 — Unsafe Code",
        content: "The 'unsafe' block is a controlled escape hatch. It unlocks pointer dereferencing and ADT field access but does not disable the borrow checker. This ensures that even 'unsafe' code remains within the bounds of ownership rules.",
        code: `unsafe {\n    var p = &x\n    *p = 100\n}`
    },
    {
        id: "errors",
        title: "31 — Error Handling",
        content: "Error handling is explicit via Option and Result ADTs. This eliminates hidden control flow paths and ensures that every possible error is accounted for. The compiler forces handling of the 'Err' or 'None' variants via pattern matching.",
        code: `case result {\n    Ok(v): print(v)\n    Err(e): handle_error(e)\n}`
    },
    {
        id: "architecture",
        title: "32 — Compiler Architecture",
        content: "The compiler follows a multi-pass pipeline: Parser -> Resolver -> Typechecker/VRA -> NLL/Linearity -> Emitter. This structured approach ensures that each safety invariant is verified independently and efficiently before C code generation.",
        code: `/* source -> AST -> IR -> C99 */`
    }
];
