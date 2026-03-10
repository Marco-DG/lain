import Link from 'next/link';
import styles from "../page.module.css";

export default function Docs() {
    return (
        <>
            <div className={styles.noise}></div>
            <div className={styles.scanlines}></div>

            <main className={styles.container}>
                <div className={styles.naviShell}>

                    {/* LEFT PANEL: NAVIGATION & STATUS */}
                    <aside className={styles.sidebar}>
                        <div className={styles.sidebarTop}>
                            <h1 className={styles.logo}><Link href="/" style={{ textDecoration: 'none', color: 'inherit' }}>[ sys.lain ]</Link></h1>
                            <nav className={styles.navLinks}>
                                <Link href="/docs" className={styles.navLink} style={{ color: 'var(--border-active)' }}>{">"} DOCUMENTATION</Link>
                                <Link href="/spec" className={styles.navLink}>{">"} SPECIFICATIONS</Link>
                                <a className={styles.navLink} href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer">{">"} SOURCE_CODE</a>
                                <div style={{ height: '1rem' }}></div>
                                <Link href="/install" className={styles.navLink}>{">"} INSTALL_LAIN</Link>
                            </nav>
                        </div>

                        <div className={styles.sysStatus}>
                            DOCS_MODULE LOADED<br />
                            FORMAT: ASCII_TEXT<br />
                            [ INDEXING_COMPLETE ]
                        </div>
                    </aside>

                    {/* CENTER PANEL: CONTENT */}
                    <section className={styles.mainContent}>
                        <h2 className={styles.title} style={{ fontSize: '4rem' }}>Documentation</h2>
                        <span className={styles.subtitle}>core concepts and guides</span>

                        <div className={styles.pitch}>
                            <p>Welcome to the Lain programming language documentation. Navigate the concepts below to understand the <strong>zero-cost memory safety</strong> and <strong>absolute determinism</strong> guarantees.</p>
                        </div>

                        <div className={styles.directives}>
                            <div className={styles.directive}>
                                <div className={styles.directiveHeader}>
                                    <span className={styles.directiveNum}>[01]</span>
                                    <h3 className={styles.directiveTitle}>Core Philosophy</h3>
                                </div>
                                <p className={styles.directiveBody}>
                                    Lain is a critical-safe programming language designed for embedded systems. It focuses on memory safety, type safety, and determinism with completely zero runtime overhead.
                                </p>
                            </div>

                            <div className={styles.directive}>
                                <div className={styles.directiveHeader}>
                                    <span className={styles.directiveNum}>[02]</span>
                                    <h3 className={styles.directiveTitle}>Memory Safety (No GC)</h3>
                                </div>
                                <p className={styles.directiveBody}>
                                    Guaranteed via linear types and borrow checking. No Garbage Collector. The <code>mov</code> keyword transfers ownership explicitly at compile time.
                                </p>
                            </div>

                            <div className={styles.directive}>
                                <div className={styles.directiveHeader}>
                                    <span className={styles.directiveNum}>[03]</span>
                                    <h3 className={styles.directiveTitle}>Purity</h3>
                                </div>
                                <p className={styles.directiveBody}>
                                    Strict distinction between deterministic functions (<code>func</code>) and side-effecting procedures (<code>proc</code>). Pure functions are mathematically guaranteed to terminate and cannot mutate global variables.
                                </p>
                            </div>
                        </div>
                    </section>

                    {/* RIGHT PANEL: CODE DATA WINDOW */}
                    <aside className={styles.codePanel}>
                        <div className={styles.panelHeader}>
                            <span>TARGET: rules.ln</span>
                            <span>[READONLY]</span>
                        </div>

                        <div className={styles.codeScroll}>
                            <div className={styles.codeBlock}>
                                <div className={styles.codeComment}>// RULE_1: PURE TERMINATION</div>
                                <pre><code>
                                    <span className={styles.kw}>func</span> factorial(n <span className={styles.type}>int</span>) <span className={styles.type}>int</span> {'{'}
                                    <span className={styles.err}>// ERROR: Recursion not allowed in func</span>
                                    <span className={styles.err}>// return n * factorial(n - 1)</span>
                                    {'}'}
                                </code></pre>
                            </div>
                        </div>
                    </aside>

                </div>
            </main>
        </>
    );
}
