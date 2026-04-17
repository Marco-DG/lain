import { DocSection } from '@/components/DocViewer';

export const docData: DocSection[] = [
    {
        id: "intro",
        title: "Introduction",
        level: 1,
        content: `
        <p>Lain is a statically typed, compiled programming language designed for embedded systems, safety-critical software, and systems programming. Memory safety and resource safety are guaranteed entirely at compile time: no garbage collector, no reference counting, no runtime bounds checks.</p>
        `
    },
    {
        id: "safety",
        title: "Safety Guarantees",
        level: 2,
        content: `
        <table>
            <thead>
                <tr><th>Safety Concern</th><th>Guarantee</th><th>Mechanism</th></tr>
            </thead>
            <tbody>
                <tr><td><b>Buffer Overflows</b></td><td>Impossible</td><td>Value Range Analysis verifies array access at compile time</td></tr>
                <tr><td><b>Use-After-Free</b></td><td>Impossible</td><td>Linear types (<code>mov</code>) ensure resources are consumed exactly once</td></tr>
                <tr><td><b>Double Free</b></td><td>Impossible</td><td>Ownership is linear; a resource is consumed exactly once</td></tr>
                <tr><td><b>Data Races</b></td><td>Impossible</td><td>Borrow checker enforces exclusive mutability</td></tr>
                <tr><td><b>Null Dereference</b></td><td>Prevented</td><td>Pointer dereference requires <code>unsafe</code></td></tr>
                <tr><td><b>Memory Leaks</b></td><td>Prevented</td><td>Linear variables must be consumed; forgetting is a compile error</td></tr>
            </tbody>
        </table>
        `
    },
    {
        id: "model",
        title: "Compilation Model",
        level: 2,
        content: `
        <pre><code>.ln source -> [Lain Compiler] -> out.c -> [gcc/clang] -> executable</code></pre>
        <p>All safety checks (ownership, borrowing, bounds, purity, pattern exhaustiveness) happen during compilation. The generated C99 code contains no runtime checks.</p>
        `
    },
    {
        id: "quickstart",
        title: "Quick Start",
        level: 2,
        content: `
        <b>Build the compiler:</b>
        <pre><code>gcc src/main.c -o compiler -std=c99 -Wall -Wextra</code></pre>
        <b>Compile a Lain program:</b>
        <pre><code># Step 1: Lain -> C\n./compiler my_program.ln\n\n# Step 2: C -> Executable\ngcc out.c -o my_program -Dlibc_printf=printf -Dlibc_puts=puts -w</code></pre>
        `
    },
    {
        id: "reference",
        title: "Language Reference",
        level: 1
    },
    {
        id: "lexical",
        title: "1. Lexical Structure",
        level: 2,
        content: `
        <p>Lain source files are UTF-8 encoded. The language supports line (//) and nested block (/* */) comments. Semicolons are optional, as newlines serve as implicit statement terminators.</p>
        `
    },
    {
        id: "keywords",
        title: "1.1 Keywords",
        level: 3,
        content: `
        <table>
            <thead><tr><th>Keyword</th><th>Purpose</th></tr></thead>
            <tbody>
                <tr><td><code>var</code></td><td>Mutable variable declaration</td></tr>
                <tr><td><code>mov</code></td><td>Ownership transfer (move semantics)</td></tr>
                <tr><td><code>type</code></td><td>Type definition (structs, ADTs)</td></tr>
                <tr><td><code>func</code></td><td>Pure function declaration</td></tr>
                <tr><td><code>proc</code></td><td>Procedure declaration (side effects)</td></tr>
                <tr><td><code>case</code></td><td>Pattern matching</td></tr>
                <tr><td><code>in</code></td><td>Bounds check / proof</td></tr>
                <tr><td><code>defer</code></td><td>LIFO resource cleanup</td></tr>
            </tbody>
        </table>
        `
    },
    {
        id: "typesystem",
        title: "2. Type System",
        level: 2,
        content: `
        <p>Lain is strictly typed and forbids all implicit conversions. Primitives follow C99 binary representation for maximum portability.</p>
        `
    },
    {
        id: "primitives",
        title: "2.1 Primitive Types",
        level: 3,
        content: `
        <p><b>Integers:</b> <code>i8, i16, i32, i64, u8, u16, u32, u64, int, usize, isize</code>.</p>
        <p><b>Floats:</b> <code>f32, f64</code>.</p>
        <p><b>Boolean:</b> <code>bool</code> (true/false).</p>
        `
    },
    {
        id: "ownership-sec",
        title: "4. Ownership & Borrowing",
        level: 2,
        content: `
        <p>Memory safety is achieved through a strict ownership model based on linear logic. Every value has a single owner. When the owner goes out of scope, the value is destroyed.</p>
        `
    },
    {
        id: "borrowing-sec",
        title: "4.4 Borrowing Rules",
        level: 3,
        content: `
        <p>Lain enforces a "Read-Write Lock" model at compile time:</p>
        <ul>
            <li>Multiple shared borrows are allowed simultaneously.</li>
            <li>Exactly one mutable borrow is allowed at a time.</li>
            <li>Shared and mutable borrows cannot coexist for the same variable.</li>
        </ul>
        `
    },
    {
        id: "vra-sec",
        title: "8. Static Verification (VRA)",
        level: 2,
        content: `
        <p>The compiler uses Value Range Analysis (VRA), a decidable, polynomial-time static analysis to verify array indexing and type constraints without runtime overhead.</p>
        `
    },
    {
        id: "interop-sec",
        title: "10. C Interoperability",
        level: 2,
        content: `
        <p>Lain compiles directly to C99. The <code>c_include</code> directive injects headers, and <code>extern</code> declares C functions. Ownership annotations can be used on extern symbols to track C-allocated resources.</p>
        `
    },
    {
        id: "unsafe-sec",
        title: "11. Unsafe Code",
        level: 2,
        content: `
        <p>The <code>unsafe</code> block is a controlled escape hatch for raw pointer dereferencing and ADT field access. It does NOT disable ownership or borrow checking.</p>
        `
    },
    {
        id: "errors-sec",
        title: "12.1 Error Codes",
        level: 2,
        content: `
        <p>Compiler errors are prefixed with codes for easy reference: <code>[E001]</code> (Use after move), <code>[E004]</code> (Borrow conflict), <code>[E014]</code> (Non-exhaustive match).</p>
        `
    }
];
