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
                        <h1 className={styles.title}>Comparative Analysis of Memory Safety: Lain vs. C/C++ Tooling</h1>
                        <p className={styles.intro}>
                            Evaluation of the Lain compiler detection capabilities against industry-standard static and dynamic analysis tools across six fundamental bug classes.
                        </p>
                    </header>

                    <div className={styles.content}>
                        <p>
                            Formal validation of the Lain compiler involved a systematic comparison with established C/C++ diagnostic tools. The study utilized six distinct code specimens, each implementing a specific category of Undefined Behavior (UB) or resource management failure. 
                        </p>
                        
                        <p>
                            Detection was attempted using standard compiler diagnostics (GCC and Clang with maximum warning levels), GCC's static analyzer, and runtime instrumentation (AddressSanitizer, UndefinedBehaviorSanitizer, and Valgrind). The table below summarizes the results.
                        </p>

                        <div className={styles.tableWrapper}>
                            <table className={styles.table}>
                                <thead>
                                    <tr>
                                        <th className={styles.bugClass}>Bug Category</th>
                                        <th>gcc -Wall</th>
                                        <th>gcc -analyzer</th>
                                        <th>ASan+UBSan</th>
                                        <th>Valgrind</th>
                                        <th>Lain</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    <tr>
                                        <td className={styles.bugClass}>Use-after-free</td>
                                        <td className={styles.resWarning}>Warning</td>
                                        <td className={styles.resWarning}>Warning</td>
                                        <td className={styles.resRuntime}>Runtime</td>
                                        <td className={styles.resRuntime}>Runtime</td>
                                        <td className={styles.resLain}>Static</td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Resource Leak</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resLain}>Static</td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Double-free</td>
                                        <td className={styles.resWarning}>Warning</td>
                                        <td className={styles.resWarning}>Warning</td>
                                        <td className={styles.resRuntime}>Runtime</td>
                                        <td className={styles.resRuntime}>Runtime</td>
                                        <td className={styles.resLain}>Static</td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Aliasing Violation</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resLain}>Static</td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Buffer Overflow</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resWarning}>Warning</td>
                                        <td className={styles.resRuntime}>Runtime</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resLain}>Static</td>
                                    </tr>
                                    <tr>
                                        <td className={styles.bugClass}>Division by Zero</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resRuntime}>Runtime</td>
                                        <td className={styles.resMissed}>—</td>
                                        <td className={styles.resLain}>Static</td>
                                    </tr>
                                </tbody>
                            </table>
                        </div>

                        <h2>Analysis of Detection Gaps</h2>
                        <p>
                            The empirical results indicate significant detection gaps in standard C/C++ tooling, particularly regarding <strong>Resource Leaks</strong> and <strong>Aliasing Violations</strong>. 
                        </p>
                        <p>
                            In the C standard, the failure to invoke <code>fclose()</code> is not a memory safety violation but a resource management failure. Consequently, dynamic tools such as Valgrind or ASan do not flag it as an error. Lain addresses this by implementing an affine type system where resource handles must be consumed exactly once, effectively lifting resource management to a compile-time requirement.
                        </p>
                        <p>
                            Aliasing violations present a similar challenge. Since they often involve valid memory addresses but violate pointer exclusivity rules, they remain invisible to runtime tools. Lain's borrow checker enforces exclusivity at the type level, preventing the creation of conflicting references that would lead to Undefined Behavior during optimization.
                        </p>

                        <h2>Static Verification vs. Runtime Instrumentation</h2>
                        <p>
                            While GCC's <code>-fanalyzer</code> provides static detection for specific memory errors, these are reported as non-blocking warnings. This allows for the production of compromised binaries. In contrast, Lain's safety guarantees are enforced as mandatory constraints during the semantic analysis phase.
                        </p>
                        <p>
                            Runtime solutions like AddressSanitizer are effective but limited to the execution paths covered during testing. They also introduce performance overhead that may be unacceptable in safety-critical systems. Lain's static approach ensures 100% coverage of all possible execution paths with zero runtime cost.
                        </p>

                        <section className={styles.conclusion}>
                            <h2>Conclusion</h2>
                            <p>
                                The data confirms that an ownership-based type system provides a more robust safety foundation than post-hoc analysis tools. By integrating safety invariants into the language core, Lain eliminates entire classes of vulnerabilities before code execution.
                            </p>
                        </section>
                    </div>
                </div>
            </article>
        </NaviShell>
    );
}
