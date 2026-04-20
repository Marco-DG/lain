import NaviShell from "@/components/NaviShell";
import Link from "next/link";
import styles from "./Article.module.css";

export default function EmpiricalResultsArticle() {
    return (
        <NaviShell layout="blog">
            <div className={styles.container}>
                <div className={styles.inner}>
                    <header className={styles.header}>
                    <span className={styles.date}>Apr 20, 2026</span>
                    <h1 className={styles.title}>Lain vs C/C++: The Empirical Results on Memory Safety at Compile Time</h1>
                    <p className={styles.intro}>
                        In our latest chapter of development, we put the Lain compiler&apos;s deterministic borrow-checker to the test against industry-standard tools: GCC, Clang, ASan, UBSan, and Valgrind. Here is what we found.
                    </p>
                </header>

                <div className={styles.content}>
                    <h2>The Blind Spots of Runtime Diagnostics</h2>
                    <p>
                        C and C++ have relied heavily on a combination of compiler warnings and dynamic runtime checks (like Address Sanitizer or Valgrind) to catch memory leaks, use-after-free bugs, and buffer overflows. While these tools are invaluable, they share a common flaw: they often require the code path to be executed at runtime, introducing significant overhead. Sometimes, static analysis passes over critical flaws, leaving developers with a false sense of security.
                    </p>
                    <p>
                        Lain fundamentally changes this paradigm. By employing strict linear types and deterministic compile-time checks, Lain turns runtime panics into immediate compile-time errors.
                    </p>

                    <h2>Test Environment</h2>
                    <p>
                        We ran our tests on an Ubuntu environment using <code>gcc 13.3.0</code>, <code>clang 18.1.3</code>, and <code>valgrind 3.22.0</code>. We pitted them against the current build of the Lain compiler to see which tools caught common memory anti-patterns.
                    </p>

                    <h2>The Results</h2>
                    <p>
                        The table below summarizes our findings. A &quot;Warning&quot; means the build proceeded despite throwing diagnostics, &quot;Runtime&quot; means the bug was caught during execution with overhead, and &quot;Compile-Time&quot; means the code rightfully failed to compile.
                    </p>

                    <div className={styles.tableWrapper}>
                        <table className={styles.table}>
                            <thead>
                                <tr>
                                    <th>Class of Error</th>
                                    <th>GCC (-Wall)</th>
                                    <th>GCC (-fanalyzer)</th>
                                    <th>Clang (-Wall)</th>
                                    <th>ASan+UBSan</th>
                                    <th>Valgrind</th>
                                    <th>Lain</th>
                                </tr>
                            </thead>
                            <tbody>
                                <tr>
                                    <td>Use-after-free</td>
                                    <td><span className={styles.statusWarning}>warning</span></td>
                                    <td><span className={styles.statusWarning}>warning</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusRuntime}>runtime</span></td>
                                    <td><span className={styles.statusRuntime}>runtime</span></td>
                                    <td><span className={styles.statusLain}>compile-time</span></td>
                                </tr>
                                <tr>
                                    <td>Resource leak</td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusLain}>compile-time</span></td>
                                </tr>
                                <tr>
                                    <td>Double-free</td>
                                    <td><span className={styles.statusWarning}>warning</span></td>
                                    <td><span className={styles.statusWarning}>warning</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusRuntime}>runtime</span></td>
                                    <td><span className={styles.statusRuntime}>runtime</span></td>
                                    <td><span className={styles.statusLain}>compile-time</span></td>
                                </tr>
                                <tr>
                                    <td>Aliasing violation</td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusLain}>compile-time</span></td>
                                </tr>
                                <tr>
                                    <td>Buffer overflow</td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusWarning}>warning</span></td>
                                    <td><span className={styles.statusWarning}>warning</span></td>
                                    <td><span className={styles.statusRuntime}>runtime</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusLain}>compile-time</span></td>
                                </tr>
                                <tr>
                                    <td>Division by zero</td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusRuntime}>runtime</span></td>
                                    <td><span className={styles.statusMissed}>missed</span></td>
                                    <td><span className={styles.statusLain}>compile-time</span></td>
                                </tr>
                            </tbody>
                        </table>
                    </div>

                    <div className={styles.conclusion}>
                        <h2>Conclusion</h2>
                        <p>
                            What stands out the most is the behavior with <strong>Aliasing violations</strong> and <strong>Resource leaks</strong>: deeply ingrained tools like GCC and Clang silently allow these to slip into the codebase without so much as a warning, while Valgrind completely ignores aliasing issues. ASan and UBSan catch a subset at runtime, which is unacceptable for systems programming where performance and deterministic behavior are paramount.
                        </p>
                        <p>
                            Lain enforces absolute strictness for these memory paradigms. The compiler simply refuses to output binaries for unsafe operations, ensuring zero overhead at runtime and total guarantees during development.
                        </p>
                    </div>
                </div>
            </div>
        </div>
    </NaviShell>
    );
}
