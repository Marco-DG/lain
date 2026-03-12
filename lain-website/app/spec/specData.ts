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
        id: "types",
        title: "02 — Type System",
        content: "Lain is strictly typed and forbids all implicit type conversions. The system comprises primitive types (int, i8-i64, u8-u64, f32, f64), composite types (structs, enums, ADTs, arrays, slices, pointers), and opaque types for C interop. Nominal type equivalence is used: two types are the same only if they share the same name.",
        code: `type Point { x int, y int }\ntype Vec2 { x int, y int }\n\n// Point != Vec2 (nominal equivalence)\nvar p = Point(10, 20)\nvar v: Vec2 = p // ERROR: Type mismatch`
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
    }
];
