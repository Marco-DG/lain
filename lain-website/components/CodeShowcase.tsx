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

func main() effects io {
    var f = open_file("data.txt", "r")
    process_file(mov f) // f is consumed here

    // ERROR [E001]: f was moved
    // process_file(mov f)
}`
    },
    {
        id: "proofs",
        title: "Mathematical Proofs",
        description: "Instead of runtime checks, Lain uses Value Range Analysis to prove properties at compile time. An index it cannot place in bounds is a compile error, not a runtime panic.",
        code: `// The compiler proves b is never 0
func safe_div(a u32, b u32 != 0) u32 {
    return a / b
}

// i is guaranteed to be a valid index for arr
func get(arr int[10], i int in arr) int {
    return arr[i] // No runtime check emitted
}

// safe_div(10, 0) // ERROR: violates b != 0
//
// Signed division needs more than b != 0, because MIN / -1
// overflows: use \`b int > 0\`, or bound the dividend.`
    },
    {
        id: "determinism",
        title: "Absolute Determinism",
        description: "There is one introducer, func, and an effect row. Silence is the strongest claim a signature can make: no I/O, no allocation, cannot panic, and guaranteed to terminate.",
        code: `// No row at all: pure, and proven to terminate
func fib(n i32 >= 0 and <= 30) i32 {
    if n <= 1 { return n }
    return fib(n - 1) +% fib(n - 2)
}

// Doing anything observable means saying so
func log_result(n i32 >= 0 and <= 30) effects io {
    libc_printf("%d\\n", fib(n))
}

// Omitting the row cannot hide the effect:
// [E011] \`func\` 'greet' has an unacknowledged effect (io)
//        — a \`func\` is pure and total by default.`
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
