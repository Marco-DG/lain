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
                            The validation of the Lain compiler involved systematic testing against industry-standard tools including GCC, Clang, ASan, and Valgrind. Six common bug classes were analyzed to compare detection capabilities across different environments.
                        </p>
                    </header>

                    <div className={styles.content}>
                        <p>
                            Six C snippets were developed, each containing a specific class of Undefined Behavior (UB) or resource mismanagement. Detection was attempted using standard static analysis (compilers with maximum warnings), dynamic analysis (AddressSanitizer and UndefinedBehaviorSanitizer), and Valgrind. Finally, the same logic was implemented in Lain to verify compile-time enforcement.
                        </p>

                        <div className={styles.tableWrapper}>
                            <table className={styles.table}>
                                <thead>
                                    <tr>
                                        <th>Bug Class</th>
                                        <th>gcc -Wall</th>
                                        <th>gcc -analyzer</th>
                                        <th>ASan + UBSan</th>
                                        <th>Valgrind</th>
                                        <th className={styles.lainHeader}>Lain</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr>
                                        <td className={styles.bugClass}>Use-after-free</td>
                                        <td><span className={styles.badgeWarning}>Warning</span></td>
                                        <td><span className={styles.badgeWarning}>Warning</span></td>
                                        <td><span className={styles.badgeRuntime}>Runtime</span></td>
                                        <td><span className={styles.badgeRuntime}>Runtime</span></td>
                                        <td className={styles.lainCell}><span className={styles.badgeLain}>Compile-Time</span></td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Resource Leak</td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td className={styles.lainCell}><span className={styles.badgeLain}>Compile-Time</span></td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Double-free</td>
                                        <td><span className={styles.badgeWarning}>Warning</span></td>
                                        <td><span className={styles.badgeWarning}>Warning</span></td>
                                        <td><span className={styles.badgeRuntime}>Runtime</span></td>
                                        <td><span className={styles.badgeRuntime}>Runtime</span></td>
                                        <td className={styles.lainCell}><span className={styles.badgeLain}>Compile-Time</span></td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Aliasing Violation</td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td className={styles.lainCell}><span className={styles.badgeLain}>Compile-Time</span></td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Buffer Overflow</td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeWarning}>Warning</span></td>
                                        <td><span className={styles.badgeRuntime}>Runtime</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td className={styles.lainCell}><span className={styles.badgeLain}>Compile-Time</span></td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Division by Zero</td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td><span className={styles.badgeRuntime}>Runtime</span></td>
                                        <td><span className={styles.badgeMissed}>Missed</span></td>
                                        <td className={styles.lainCell}><span className={styles.badgeLain}>Compile-Time</span></td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>

                        <h2>The "Blind Spots"</h2>
                        <p>
                            A striking result is the complete failure of C tools to detect <strong>Resource Leaks</strong> and <strong>Aliasing Violations</strong>. 
                        </p>
                        <p>
                            In C, <code>fclose()</code> is technically an optional call for safety tools. Since it does not cause immediate memory corruption, neither Valgrind nor ASan flag it as an error. Lain, however, treats file handles as linear types that must be consumed exactly once. Forgetting to close a file results in a mandatory compiler error.
                        </p>
                        <p>
                            Similarly, aliasing violations (passing the same mutable pointer as two different arguments) are virtually impossible for runtime tools to catch because they do not involve illegal memory access, but rather illegal logic that violates optimizer assumptions. The Lain borrow checker prevents this by enforcing that only one mutable reference can exist at any given time.
                        </p>

                        <h2>Static vs. Dynamic Detection</h2>
                        <p>
                            GCC with <code>-fanalyzer</code> catches simple cases of use-after-free and double-free, but these remain as warnings rather than hard errors. Consequently, binaries containing these bugs can still be produced and deployed.
                        </p>
                        <p>
                            Sanitizers (ASan/UBSan) are effective but require code execution, meaning bugs in untested execution paths remain undetected. Furthermore, they introduce significant runtime overhead.
                        </p>

                        <section className={styles.conclusion}>
                            <h2>Conclusion</h2>
                            <p>
                                Empirical data demonstrates that Lain provides a level of protection that standard C tooling cannot achieve without exhaustive testing and runtime monitoring. By encoding safety rules directly into the type system, the burden of verification is shifted from the test suite to the compiler.
                            </p>
                        </section>
                    </div>
                </div>
            </article>
        </NaviShell>
    );
}
