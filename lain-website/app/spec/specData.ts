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
        content: "Lain is strictly typed and forbids all implicit conversions. Primitives follow C99 binary representation: i32 is 4-byte two's complement, f64 is IEEE 754. Structs are packed with natural alignment, ensuring zero padding between fields where possible. Nominal type equivalence is used: two types are identical only if they share the same name.",
        code: `type Point { x i32, y i32 } // size: 8, align: 4\ntype Data  { b u8, i i32 }  // size: 8, align: 4 (3 bytes padding)\n\nvar p = Point(10, 20)\nvar d = Data(1, 10)`
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
        content: "Lain ensures memory safety through a strict ownership model. Every value has a single owner. When the owner goes out of scope, the value is destroyed. Shared borrows (default) allow multi-reader access, while mutable borrows (var) provide exclusive read-write access. This zero-cost system eliminates data races and use-after-free bugs at compile time.",
        code: `var x = Data(10)\nread(x)       // Shared borrow\nmove_data(mov x) // Ownership transfer\n// read(x) // ERROR: x is dead`
    },
    {
        id: "nll",
        title: "07.1 — Non-Lexical Lifetimes",
        content: "Unlike early safety models, Lain uses Non-Lexical Lifetimes (NLL). A borrow's lifetime is determined by its last use, not by lexical scope. This allows the compiler to accept complex but safe patterns where a borrow and a subsequent move of the same data appear in the same block, provided their usage spans do not overlap.",
        code: `var data = Data(42)\nvar ref = get_ref(var data)\nuse(ref) // last use of ref\n\n// Borrow expires here\nconsume(mov data) // ✅ OK under NLL`
    },
    {
        id: "linearity-deep",
        title: "07.2 — Linear Integrity",
        content: "Linearity in Lain is transitive and fine-grained. Structs with 'mov' fields are linear and must be consumed exactly once. The compiler tracks individual field consumption, allowing you to move one part of a struct (e.g., a file handle) while still accessing non-linear metadata in other fields.",
        code: `type File { mov fd int, path string }\n\nproc close(mov f File) {\n    os.close(mov f.fd) // consume resource\n    log("Closed: ", f.path) // still accessible!\n}`
    },
    {
        id: "safety-advanced",
        title: "07.3 — Advanced Safety: Persistence",
        content: "Lain supports 'persistent borrows' through function return values (return var). When a function returns a reference to its parameter's interior, the compiler registers a transitive borrow in the caller's scope. This borrow persists until its last use, effectively locking the source object from conflicting access or moves across statement boundaries.",
        code: `func get_val(var d Data) var int {\n    return var d.value\n}\n\nproc main() {\n    var d = Data(10)\n    var r = get_val(var d)\n    // d.value = 20 // ERROR: d is borrowed\n    use(r)\n}`
    },
    {
        id: "transitivity",
        title: "07.4 — Transitive Borrowing",
        content: "Borrowing is reflexive and transitive. If 'refB' borrows 'refA', and 'refA' borrows 'Root', the borrow checker maintains a dependency chain. The 'Root' object remains locked until the entire chain of dependents has expired. This prevents dangling pointers in complex data transformations.",
        code: `var a = Data(10)\nvar r1 = &a\nvar r2 = &r1 // transitively borrows a\n\n// a is locked by r2\nuse(r2)\n// a is unlocked`
    },
    {
        id: "formalisms",
        title: "07.5 — Safety Formalisms",
        content: "Lain's safety is defined by the judgment 'Γ; Σ; B ⊢ e : τ ⊣ Γ'; Σ'; B'', where Γ is the environment, Σ the linear state, and B the active borrow set. This formal model ensures that every valid program preserves the RW-Lock invariant and Linear Integrity. The system is verified to be sound: no safe program can trigger a data race or use-after-free.",
        code: `/* Typing Judgment */\nΓ; Σ; B ⊢ e : τ\n- Γ: Env (Types)\n- Σ: Linear State (Owned)\n- B: Borrow Set (RW-Lock)`
    },
    {
        id: "constraints",
        title: "08 — Verification & VRA",
        content: "Value Range Analysis (VRA) enables compile-time verification of constraints like 'x != 0' or 'i in 0..len'. It tracks absolute integer intervals through arithmetic and control flow. Unlike SMT-based systems, VRA is polynomial-time and decidable. Constraints can be combined using 'and'/'or' to express complex invariants for buffer safety and mathematical correctness.",
        code: `func safe_div(a int, b int != 0) int {\n    return a / b\n}\n\nfunc get(arr int[10], i int in 0..10) int {\n    return arr[i] // verified safe\n}\n\n// Compound: i int in 0..10 and i != 5`
    },
    {
        id: "generics",
        title: "09 — Generics & CTFE",
        content: "Generics utilize monomorphization: the compiler generates a unique C function for every distinct type argument set. This preserves 'Assembly-Speed' performance as no runtime dispatch or type erasure occurs. Comptime evaluation is restricted to pure, terminating 'func' blocks, ensuring the compiler remains polynomial-time and decidable.",
        code: `func Box(comptime T type) type {\n    return type { value T }\n}\n\n// Specializations generated in C:\n// Box_int { int value; };\n// Box_f32 { float value; };`
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
        content: "Lain uses a deterministic, stack-first memory model. Variables are destroyed in the exact reverse order of their declaration (LIFO). For heap memory, the ownership system turns manual management into a verified protocol: 'malloc' returns an owned value, and 'free' must consume it exactly once, eliminating leaks and double-frees at compile time.",
        code: `var a = malloc(10)\nvar b = malloc(20)\n\ndefer { free(mov a) } // executed 2nd\ndefer { free(mov b) } // executed 1st`
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
        id: "architecture",
        title: "16 — Compiler Architecture",
        content: "The compiler follows a strict multi-pass pipeline: 1. Parser (AST generation), 2. Resolver (scoping), 3. Typechecker (VRA & Constraints), 4. NLL Pre-pass (liveness), 5. Linearity (borrow check & move tracking), 6. Emitter (C99 generation). This linear pipeline ensures predictable build times and facilitates modular verification of each safety invariant.",
        code: `/* Pipeline Flow */\nsource -> [Parser]\n       -> [Resolver]\n       -> [NLL Analysis]\n       -> [Sema/BorrowCheck]\n       -> [C99 Emitter]`
    },
    {
        id: "appendix-a",
        title: "App. A — Keyword Reference",
        content: "Lain's keyword system is designed for clarity and ease of parsing. Key active keywords include 'mov' (ownership transfer), 'var' (mutable binding/borrow), 'proc' (procedures), and 'func' (pure functions). A strict set of 'Reserved' keywords (macro, pre, post, expr) are set aside for planned extensions like contracts and meta-programming.",
        code: `/* Selection of Keywords */\nmov x      // Ownership transfer\nvar y = 10 // Mutable variable\nfunc f()   // Pure function\nproc p()   // Side-effecting procedure\ncomptime T // Generic parameter`
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
        id: "appendix-e",
        title: "App. E — Technical Status",
        content: "The Lain compiler is actively evolving through a 5-phase plan. Ph 1 (Core) and Ph 2 (Semantic System) are largely implemented, including the borrow checker and monomorphized generics. Current work (Ph 3) focuses on formalizing the memory model and error handling. Open design questions include the exact semantics of inclusive ranges (..=) and the implementation of native 'char' for C interop.",
        code: `// Current Task Queue:\n- [x] VRA Constraints\n- [x] Monomorphization\n- [/] Ph 3: Memory Model\n- [/] Ph 4: Stdlib Reference\n- [ ] Ph 5: Grammar Formalization`
    },
    {
        id: "diagnostics",
        title: "App. F — Diagnostic System",
        content: "Lain's diagnostic system provides precise feedback on safety violations. It identifies where a borrow was created, why it conflicts with a current operation, and where it is expected to expire. This helps developers resolve spatial and temporal safety errors without needing explicit lifetime markers in their code.",
        code: `/* Example Error Reporting */\nE0502: cannot borrow 'data' as shared\nbecause it is mutably borrowed.\n\n3 | var r = get(var data) // mutable here\n5 | read(data)             // shared ERROR`
    }
];
