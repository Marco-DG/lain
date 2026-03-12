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
        content: "Lain is a statically typed, compiled programming language designed for systems programming and safety-critical software. It achieves memory safety and resource safety entirely at compile time, with zero runtime overhead. Its design is governed by five pillars: Assembly-Speed Performance, Zero-Cost Memory Safety, Static Verification, Determinism, and Syntactic Simplicity.",
        code: `// Conforming Lain code\nproc main() {\n    print("Hello, systems programming")\n}`
    },
    {
        id: "lexical",
        title: "01 — Lexical Structure",
        content: "Lain source files are UTF-8 encoded. The language uses maximal munch for tokenization and supports line (//) and nested block (/* */) comments. Semicolons are optional, as newlines serve as implicit statement terminators. The keyword system is minimal but strict, prioritizing clarity and predictable parsing.",
        code: `// Line comment\n/* Nested /* block */ comment */\n\nvar x = 42\ny = "string" // implied semicolon`
    },
    {
        id: "types",
        title: "02 — Type System",
        content: "Lain is strictly typed and forbids all implicit type conversions. The system comprises primitive types (int, i8-i64, u8-u64, f32, f64), composite types (structs, enums, ADTs, arrays, slices, pointers), and opaque types for C interop. Nominal type equivalence is used: two types are the same only if they share the same name.",
        code: `type Point { x int, y int }\ntype Vec2 { x int, y int }\n\n// Point != Vec2 (nominal equivalence)\nvar p = Point(10, 20)\nvar v Vec2 = p // ERROR: Type mismatch`
    },
    {
        id: "declarations",
        title: "03 — Declarations & Scoping",
        content: "Lain uses lexical block scoping and supports shadowing. Variables are declared with 'var' for mutability or via direct assignment for immutability. Top-level declarations (functions, types) are order-independent, while local variables must be declared before use. Definite initialization analysis ensures 'undefined' variables are never read.",
        code: `var x = 10 // mutable\ny = 20     // immutable\n\nproc example() {\n    var x = 5 // shadows outer x\n    var z int = undefined\n    z = 10    // must initialize before use\n}`
    },
    {
        id: "expressions",
        title: "04 — Expressions",
        content: "Expressions are strictly evaluated left-to-right. Plain operators (+, -, *, /) work only on identical types; explicit casting with 'as' is required for widening or truncation. Compound assignment operators are provided as syntactic sugar. Move (mov) and mutable borrow (var) can be used as expression-level operators.",
        code: `var x = 10 as u8\nvar y = x + 5 // x is u8, 5 is inferred as u8\n\nvar big = (x as i64) << 32\nconsume(mov big) // big is invalidated here`
    },
    {
        id: "statements",
        title: "05 — Statements",
        content: "Statements are terminated by newlines or semicolons. Lain supports rich control flow including LIFO deferred blocks (defer) for resource cleanup, and exhaustive pattern matching (case). While loops are restricted to procedures to guarantee termination of pure functions. Range-based 'for' loops provide built-in safety for iteration.",
        code: `if condition {\n    defer { close() }\n    for i in 0..10 {\n        if i == 5 { continue }\n        process(i)\n    }\n}`
    },
    {
        id: "functions",
        title: "06 — Functions & Procedures",
        content: "Lain enforces a strict separation between pure functions (func) and side-effecting procedures (proc). Pure functions are guaranteed to terminate and produce the same output for same inputs; they cannot call procedures, access global state, use unbounded loops, or recurse.",
        code: `func add(a int, b int) int {\n    return a + b\n}\n\nproc log(msg string) {\n    print(msg) // Procedures can have side effects\n}`
    },
    {
        id: "ownership",
        title: "07 — Ownership & Borrowing",
        content: "Lain's safety is built on ownership modes: Shared (read-only borrow), Mutable (exclusive borrow), and Owned (transfer). The borrow checker enforces the Read-Write Lock invariant: either many shared borrows or exactly one mutable borrow may exist for any variable at a given point.",
        code: `var data = Data(42)\nvar ref = get_ref(var data) // mutable borrow\n\n// ERROR: cannot use data while mutably borrowed\n// print(data.value)\n\nuse(var ref) // last use of ref\nprint(data.value) // OK now: borrow expired`
    },
    {
        id: "linearity",
        title: "07.6 — Linear Types",
        content: "A type is linear if it contains an owned field (mov). Linear values must be consumed exactly once. This prevents memory leaks and ensures resource lifecycle integrity (e.g., file handles must be closed). Branch consistency ensures they are consumed regardless of the code path.",
        code: `type File { mov handle *FILE }\n\nproc process(mov f File) {\n    // f is consumed here\n}\n\nproc main() {\n    var f = open_file("data.txt")\n    process(mov f)\n    // read_file(f) // ERROR: f already moved\n}`
    },
    {
        id: "constraints",
        title: "08 — Verification & VRA",
        content: "Value Range Analysis (VRA) enables static verification of program properties like array bounds and division-by-zero without a runtime cost. Equation-style constraints can be applied to parameters and return types, ensuring that invalid arguments are caught at compile time. It tracks integer intervals through control flow and arithmetic.",
        code: `func safe_div(a int, b int != 0) int {\n    return a / b\n}\n\nfunc get(arr int[10], i int in arr) int {\n    return arr[i] // guaranteed safe\n}`
    },
    {
        id: "generics",
        title: "09 — Generics & CTFE",
        content: "Lain uses Compile-Time Function Evaluation (CTFE) for generics. Instead of specialized syntax, it uses ordinary 'func' declarations with 'comptime' parameters. At call sites, the compiler monomorphizes the function, creating a concrete specialization for each unique set of type arguments. This unifies generics, type constructors, and static assertions into one powerful mechanism.",
        code: `func Option(comptime T type) type {\n    return type { Some { value T }, None }\n}\n\ntype OptionInt = Option(int)\nvar x = OptionInt.Some(42)`
    },
    {
        id: "modules",
        title: "10 — Module System",
        content: "Lain uses a dot-separated module system that maps directly to the filesystem (e.g., 'import std.io'). It follows a unity build model: all imported modules are inlined into a single AST. To prevent name collisions, identifiers are mangled with their module path prefix in the generated C code. All top-level declarations are currently visible to importers.",
        code: `import std.io\nimport mylib.utils\n\nproc main() {\n    std.io.print("Hello module")\n}`
    },
    {
        id: "interop",
        title: "11 — C Interoperability",
        content: "Lain provides zero-overhead C interop via direct C99 compilation. The 'c_include' directive injects headers, while 'extern' declarations map C functions and opaque types into the Lain namespace. Ownership annotations can be applied to extern parameters, allowing the borrow checker to track resources from C libraries like file handles.",
        code: `c_include "<stdio.h>"\nextern type FILE\nextern proc fopen(path *u8, mode *u8) mov *FILE\n\nvar f = fopen("log.txt", "w")\n// f is tracked by borrow checker`
    },
    {
        id: "unsafe",
        title: "12 — Unsafe Code",
        content: "The 'unsafe' block provides an escape hatch for operations the compiler cannot verify, primarily raw pointer manipulation and direct ADT field access. Unsafe code is narrowly scoped and does not disable ownership or borrow checking — only pointer dereferencing and variant bypass are unlocked.",
        code: `var x = 10\nunsafe {\n    var p = &x // address-of\n    *p = 20    // dereference\n}\n\n// direct ADT bypass\nunsafe { var r = shape.radius }`
    },
    {
        id: "memory",
        title: "13 — Memory Model",
        content: "Lain's memory model is designed for zero runtime overhead and deterministic allocation. It defaults to stack allocation for all variables, with explicit heap management via C interop (malloc/free). The ownership system integrates directly with heap pointers, turning memory leaks and double-frees into compile-time errors without a garbage collector.",
        code: `extern proc malloc(size usize) mov *void\nextern proc free(ptr mov *void)\n\nproc main() {\n    var ptr = malloc(1024)\n    // ... use ptr ...\n    free(mov ptr) // explicit deallocation\n}`
    },
    {
        id: "errors",
        title: "14 — Error Handling",
        content: "Lain rejects exceptions in favor of explicit error handling via algebraic data types (Option and Result). This ensures all error paths are visible in the type system and follow deterministic control flow. Zero-overhead cleanup is managed through deferred blocks (defer) and linear types that guarantee resource release.",
        code: `type ResultInt = Result(int, int)\n\nproc try_open(path u8[:0]) ResultInt {\n    var raw = fopen(path.data, "r")\n    if raw == 0 { return ResultInt.Err(1) }\n    return ResultInt.Ok(42)\n}`
    },
    {
        id: "stdlib",
        title: "15 — Standard Library",
        content: "The Lain standard library provides essential, composable building blocks under the 'std' namespace. It includes low-level C bindings (std.c), IO operations (std.io), resource-safe file system access (std.fs), and core mathematical utilities (std.math). All modules are built using Lain's ownership and purity rules.",
        code: `import std.io\nimport std.fs\n\nproc main() {\n    var f = open_file("data.txt", "w")\n    defer { close_file(mov f) }\n    write_file(f, "Lain Standard Lib")\n}`
    },
    {
        id: "rationale",
        title: "App. B — Design Rationale",
        content: "Lain's design prioritizes auditability and predictability. The func/proc separation guarantees termination in pure code, while caller-site annotations (mov/var) make resource transfers explicit. By compiling to C99 and using Value Range Analysis instead of SMT solvers, Lain achieves maximum portability and developer-scrutable safety.",
        code: `// Why mov at call site?\nclose_file(mov f) // Explicit consumption\n\n// Why func vs proc?\nfunc add(a int, b int) int // Guaranteed pure`
    },
    {
        id: "comparison",
        title: "App. C — Comparisons",
        content: "Lain occupies a unique niche between Rust's safety and C's simplicity. Unlike Rust, it requires no lifetime annotations, using implicit NLL instead. Compared to C, it prevents null dereferences, buffer overflows, and memory leaks at compile time. It shares comptime generics with Zig but adds a formal borrow checker.",
        code: `/* Feature Matrix */\n// Lain: Ownership + No GC + C Backend\n// Rust: Ownership + No GC + LLVM\n// C: Manual + No GC + Native\n// Zig: Manual + No GC + LLVM`
    },
    {
        id: "roadmap",
        title: "App. D — Evolution Roadmap",
        content: "The Lain roadmap focuses on enhancing ergonomics without compromising zero-cost principles. Planned features include two-phase borrows to reduce false positives, an error propagation operator (?), and a char type for better string interop. Long-term goals include a trait system and stackless async/concurrency.",
        code: `// Planned: Error Propagation\nvar val = try_op()? \n\n// Planned: Traits\ntrait Printable { func print(self) }`
    },
    {
        id: "borrowchecker",
        title: "Deep Dive — Borrow Checker",
        content: "The borrow checker enforces the Read-Write Lock invariant: multiple shared borrows or exactly one mutable borrow—never both. It uses Non-Lexical Lifetimes (NLL) to track borrow expiration at the last use, and ensures linear types are consumed exactly once. This eliminates data races and use-after-free bugs at source.",
        code: `var x = Data(10)\nvar ref = get_ref(var x) // Exclusive\n\n// print(x.val) // ERROR: x is borrowed\nuse(ref)        // Last use\nprint(x.val)  // OK: borrow released`
    }
];
