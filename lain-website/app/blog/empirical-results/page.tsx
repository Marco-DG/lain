'use client';

import React from 'react';
import NaviShell from "@/components/NaviShell";
import styles from "./Article.module.css";
import Link from 'next/link';

export default function EmpiricalResults() {
    return (
        <NaviShell layout="blog">
            <article className={styles.container}>
                <div className={styles.inner}>
                    <header className={styles.header}>
                        <Link href="/blog" className={styles.backLink}>← Back to Blog</Link>
                        <span className={styles.date}>APR 20, 2026 // COMPILER RESEARCH</span>
                        <h1 className={styles.title}>Lain vs C/C++: Empirical Results on Memory Safety</h1>
                        <p className={styles.intro}>
                            To validate the Lain compiler, we tested it against GCC, Clang, ASan, and Valgrind on six common bug classes. The goal was to see which tools catch what, and when.
                        </p>
                    </header>

                    <div className={styles.content}>
                        <p>
                            We wrote six C snippets, each containing a specific class of Undefined Behavior (UB) or resource mismanagement. We then attempted to detect these bugs using standard static analysis (compilers with maximum warnings), dynamic analysis (AddressSanitizer and UndefinedBehaviorSanitizer), and Valgrind. Finally, we implemented the same logic in Lain.
                        </p>

                        <div className={styles.tableWrapper}>
                            <table className={styles.table}>
                                <thead>
                                    <tr>
                                        <th>Bug Class</th>
                                        <th>gcc -Wall</th>
                                        <th>gcc -fanalyzer</th>
                                        <th>ASan + UBSan</th>
                                        <th>Valgrind</th>
                                        <th>Lain</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr>
                                        <td><strong>Use-after-free</strong></td>
                                        <td className={styles.statusWarning}>Warning</td>
                                        <td className={styles.statusWarning}>Warning</td>
                                        <td className={styles.statusRuntime}>Runtime Error</td>
                                        <td className={styles.statusRuntime}>Runtime Error</td>
                                        <td className={styles.statusLain}>Compile-Time</td>
                                    </tr>
                                    <tr>
                                        <td><strong>Resource Leak</strong></td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusLain}>Compile-Time</td>
                                    </tr>
                                    <tr>
                                        <td><strong>Double-free</strong></td>
                                        <td className={styles.statusWarning}>Warning</td>
                                        <td className={styles.statusWarning}>Warning</td>
                                        <td className={styles.statusRuntime}>Runtime Error</td>
                                        <td className={styles.statusRuntime}>Runtime Error</td>
                                        <td className={styles.statusLain}>Compile-Time</td>
                                    </tr>
                                    <tr>
                                        <td><strong>Aliasing Violation</strong></td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusLain}>Compile-Time</td>
                                    </tr>
                                    <tr>
                                        <td><strong>Buffer Overflow</strong></td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusWarning}>Warning</td>
                                        <td className={styles.statusRuntime}>Runtime Error</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusLain}>Compile-Time</td>
                                    </tr>
                                    <tr>
                                        <td><strong>Division by Zero</strong></td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusRuntime}>Runtime Error</td>
                                        <td className={styles.statusMissed}>Missed</td>
                                        <td className={styles.statusLain}>Compile-Time</td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>

                        <h2>The "Blind Spots"</h2>
                        <p>
                            The most striking result is the complete failure of C tools to detect <strong>Resource Leaks</strong> and <strong>Aliasing Violations</strong>. 
                        </p>
                        <p>
                            In C, <code>fclose()</code> is technically an optional call for safety tools. Since it doesn't cause immediate memory corruption, neither Valgrind nor ASan flag it as an error. Lain, however, treats file handles as linear types: they <em>must</em> be consumed exactly once. Forgetting to close a file is a compiler error.
                        </p>
                        <p>
                            Similarly, aliasing violations (passing the same mutable pointer as two different arguments) are virtually impossible for runtime tools to catch because they don't involve "illegal" memory access, just "illegal" logic that breaks optimizer assumptions. Lain's borrow checker catches this by enforcing that only one mutable reference can exist at any time.
                        </p>

                        <h2>Static vs. Dynamic Detection</h2>
                        <p>
                            GCC with <code>-fanalyzer</code> has made progress, catching simple cases of use-after-free and double-free. However, these are warnings, not errors. A developer can still ship a binary with these bugs. 
                        </p>
                        <p>
                            Sanitizers (ASan/UBSan) are powerful but come with a cost: they require the code to be executed (missing bugs in untested paths) and add significant runtime overhead.
                        </p>

                        <section className={styles.conclusion}>
                            <h2>Conclusion</h2>
                            <p>
                                The empirical data shows that Lain provides a level of protection that standard C tooling cannot achieve without manual, exhaustive testing and runtime monitoring. By encoding safety rules directly into the type system, Lain moves the burden of verification from the developer's test suite to the compiler itself.
                            </p>
                        </section>
                    </div>
                </div>
            </article>
        </NaviShell>
    );
}
