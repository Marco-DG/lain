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
                <tr><td><b>Buffer Overflows</b></td><td>Impossible</td><td>Value Range Analysis (§8) verifies every array access at compile time</td></tr>
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
        <p>All safety checks happen during compilation. The generated C99 code contains no runtime checks.</p>
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
        content: `<p>Lain source files are UTF-8 encoded. The language supports line and nested block comments.</p>`
    },
    {
        id: "keywords",
        title: "1.1 Keywords",
        level: 3,
        content: `
        <p>Core keywords include <code>var</code>, <code>mov</code>, <code>type</code>, <code>func</code>, <code>proc</code>, <code>case</code>, <code>in</code>, <code>defer</code>, and <code>unsafe</code>.</p>
        <blockquote data-type="important">
            <strong>Note</strong>
            The keyword <code>fun</code> is accepted as an alias for <code>func</code>.
        </blockquote>
        `
    },
    {
        id: "literals",
        title: "1.3 Literals",
        level: 3,
        content: `
        <p><b>Integers:</b> <code>42, 0, -1</code>. <b>Strings:</b> <code>"Hello"</code> (type <code>u8[:0]</code>).</p>
        <pre><code>var s = "Lain"\nlibc_printf("%s", s.data)</code></pre>
        `
    },
    {
        id: "typesystem",
        title: "2. Type System",
        level: 2,
        content: `<p>Lain is strictly typed with no implicit narrowing. Primitive types follow C99 binary representation.</p>`
    },
    {
        id: "primitives",
        title: "2.1 Primitive Types",
        level: 3,
        content: `
        <p><b>Integers:</b> <code>i8, i16, i32, i64, u8, u16, u32, u64, int, usize, isize</code>.</p>
        <p><b>Floats:</b> <code>f32, f64</code>. <b>Boolean:</b> <code>bool</code>.</p>
        `
    },
    {
        id: "pointers",
        title: "2.3 Pointer Types",
        level: 3,
        content: `
        <p>Pointers use <code>*</code> prefix: <code>*int</code> (shared), <code>var *int</code> (mutable), <code>mov *int</code> (owned).</p>
        `
    },
    {
        id: "adts",
        title: "2.8 Algebraic Data Types",
        level: 3,
        content: `
        <p>Unified syntax for enums and tagged unions.</p>
        <pre><code>type Shape {\n    Circle { radius int }\n    Point\n}</code></pre>
        `
    },
    {
        id: "variables",
        title: "3. Variables & Mutability",
        level: 2,
        content: `
        <p>Variables are immutable by default. <code>var</code> creates a mutable binding.</p>
        <pre><code>x = 10        // Immutable\nvar y = 20    // Mutable</code></pre>
        `
    },
    {
        id: "ownership-sec",
        title: "4. Ownership & Borrowing",
        level: 2,
        content: `
        <p>Lain uses linear logic to ensure memory safety. Every value has exactly one owner.</p>
        `
    },
    {
        id: "move-semantics",
        title: "4.2 Move Semantics",
        level: 3,
        content: `
        <p>The <code>mov</code> operator transfers ownership, invalidating the source.</p>
        <pre><code>var b = mov a\n// a is now invalid</code></pre>
        `
    },
    {
        id: "borrowing-rules",
        title: "4.4 Borrowing Rules",
        level: 3,
        content: `
        <p>Multiple shared borrows OR exactly one mutable borrow. They cannot coexist.</p>
        `
    },
    {
        id: "functions-sec",
        title: "5. Functions & Procedures",
        level: 2,
        content: `
        <p>Strict separation: <code>func</code> is pure and total; <code>proc</code> allows side effects.</p>
        `
    },
    {
        id: "control-flow-sec",
        title: "6. Control Flow",
        level: 2,
        content: `
        <p>Supports <code>if/elif/else</code>, <code>for</code> (range-based), <code>while</code> (restricted in <code>func</code>), and <code>case</code>.</p>
        `
    },
    {
        id: "defer-sec",
        title: "6.7 Defer Statement",
        level: 3,
        content: `
        <p>Schedules code for LIFO execution at scope end. Perfect for cleanup.</p>
        <pre><code>var f = open()\ndefer close(mov f)</code></pre>
        `
    },
    {
        id: "constraints-sec",
        title: "8. Type Constraints & VRA",
        level: 2,
        content: `
        <p>Value Range Analysis (VRA) verifies constraints like <code>x != 0</code> statically.</p>
        `
    },
    {
        id: "in-keyword",
        title: "8.3 Index Bounds (in)",
        level: 3,
        content: `
        <p>The <code>in</code> keyword proves array access safety at compile time.</p>
        <pre><code>if idx in arr { return arr[idx] }</code></pre>
        `
    },
    {
        id: "modules-sec",
        title: "9. Module System",
        level: 2,
        content: `
        <p>Dot-notation imports mapping to the filesystem: <code>import std.io</code>.</p>
        `
    },
    {
        id: "interop-sec",
        title: "10. C Interoperability",
        level: 2,
        content: `
        <p>Direct C99 compilation with <code>c_include</code> and <code>extern</code> declarations.</p>
        `
    },
    {
        id: "unsafe-sec",
        title: "11. Unsafe Code",
        level: 2,
        content: `
        <p>Controlled escape hatch for pointer dereferencing and ADT bypass.</p>
        <pre><code>unsafe { *ptr = 10 }</code></pre>
        `
    },
    {
        id: "error-model-sec",
        title: "14. Error Model",
        level: 2,
        content: `
        <p>No exceptions. Explicit error handling via <code>Option</code> and <code>Result</code> ADTs.</p>
        `
    },
    {
        id: "appendix-a",
        title: "Appendix A: Type Summary",
        level: 2,
        content: `
        <table>
            <thead><tr><th>Syntax</th><th>Description</th></tr></thead>
            <tbody>
                <tr><td><code>T</code></td><td>Primitive type</td></tr>
                <tr><td><code>*T</code></td><td>Shared pointer</td></tr>
                <tr><td><code>T[N]</code></td><td>Fixed array</td></tr>
                <tr><td><code>T[]</code></td><td>Slice</td></tr>
            </tbody>
        </table>
        `
    },
    {
        id: "appendix-d",
        title: "Appendix D: Grammar",
        level: 2,
        content: `
        <p>Simplified pseudo-BNF of the Lain language.</p>
        <pre><code>program = { top_level_decl } ;\ntype_decl = "type" IDENT "{" type_body "}" ;</code></pre>
        `
    }
];
