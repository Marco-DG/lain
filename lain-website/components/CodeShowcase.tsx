import React from 'react';
import styles from './CodeShowcase.module.css';

const showcases = [
    {
        id: "memory",
        title: "Zero-Cost Memory Safety",
        description: "Lain uses a linear type system and a strict borrow checker to guarantee memory safety without garbage collection. Ownership transfers are mandatory and explicit.",
        code: `// Compile-time guaranteed use-after-free prevention
func process_file(mov {handle} File) {
    // ...
    fclose(handle)
}

proc main() {
    var f = open_file("data.txt", "r")
    process_file(mov f) // f is consumed here

    // ERROR [E001]: f was moved
    // process_file(mov f) 
}`
    },
    {
        id: "proofs",
        title: "Mathematical Proofs",
        description: "Instead of runtime checks, Lain uses Value Range Analysis (VRA) to prove properties at compile time. Array bounds and division by zero are mathematically impossible.",
        code: `// The compiler proves b is never 0
func safe_div(a int, b int != 0) int {
    return a / b
}

// i is guaranteed to be a valid index for arr
func get(arr int[10], i int in arr) int {
    return arr[i] // No runtime panic possible
}

safe_div(10, 2) // OK
// safe_div(10, 0) // ERROR: violates b != 0`
    },
    {
        id: "determinism",
        title: "Absolute Determinism",
        description: "Lain enforces a strict type-level separation between pure mathematical functions (func) and side-effecting procedures (proc).",
        code: `// Pure function: no globals, no I/O, guaranteed to terminate
func fib(n int) int >= 0 {
    if n <= 1 { return n }
    return fib(n-1) + fib(n-2)
}

// Procedure: allowed to side-effect
proc log_result(n int) {
    var result = fib(n)
    io.print(result) 
}

// func bad() { io.print(1) } // ERROR [E011]: func cannot call proc`
    }
];

export default function CodeShowcase() {
    return (
        <section id="docs" className={styles.section}>
            <h2 className={styles.sectionTitle}>SYSTEM.ARCHITECTURE</h2>

            <div className={styles.showcaseList}>
                {showcases.map((sc, index) => (
                    <div key={sc.id} className={styles.row}>
                        <div className={styles.textColumn}>
                            <div className={styles.index}>0{index + 1} //</div>
                            <h3 className={styles.title}>{sc.title}</h3>
                            <p className={styles.description}>{sc.description}</p>
                        </div>

                        <div className={styles.codeColumn}>
                            <div className={styles.editorShell}>
                                <div className={styles.editorTop}>
                                    <span className={styles.dots}></span>
                                    <span className={styles.dots}></span>
                                    <span className={styles.dots}></span>
                                    <span className={styles.filename}>example_{sc.id}.ln</span>
                                </div>
                                <pre className={styles.editorBody}>
                                    <code>{sc.code}</code>
                                </pre>
                            </div>
                        </div>
                    </div>
                ))}
            </div>
        </section>
    );
}
