import Link from 'next/link';
import styles from "../page.module.css";

export default function Spec() {
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
                                <Link href="/docs" className={styles.navLink}>{">"} DOCUMENTATION</Link>
                                <Link href="/spec" className={styles.navLink} style={{ color: 'var(--border-active)' }}>{">"} SPECIFICATIONS</Link>
                                <a className={styles.navLink} href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer">{">"} SOURCE_CODE</a>
                                <div style={{ height: '1rem' }}></div>
                                <Link href="/install" className={styles.navLink}>{">"} INSTALL_LAIN</Link>
                            </nav>
                        </div>

                        <div className={styles.sysStatus}>
                            SPEC_SECTOR ACCESSED<br />
                            DRAFT: v0.1.0<br />
                            [ STRICT_MODE_ENV ]
                        </div>
                    </aside>

                    {/* CENTER PANEL: CONTENT */}
                    <section className={styles.mainContent}>
                        <h2 className={styles.title} style={{ fontSize: '4rem' }}>Specifications</h2>
                        <span className={styles.subtitle}>language syntax & grammar</span>

                        <div className={styles.directives}>
                            <div className={styles.directive}>
                                <div className={styles.directiveHeader}>
                                    <span className={styles.directiveNum}>[01]</span>
                                    <h3 className={styles.directiveTitle}>Lexical Structure</h3>
                                </div>
                                <p className={styles.directiveBody}>
                                    Lain syntax is designed to be minimal and explicit. Valid keywords include <code>var</code>, <code>mov</code>, <code>type</code>, <code>func</code>, <code>proc</code>.
                                </p>
                            </div>

                            <div className={styles.directive}>
                                <div className={styles.directiveHeader}>
                                    <span className={styles.directiveNum}>[02]</span>
                                    <h3 className={styles.directiveTitle}>Type System</h3>
                                </div>
                                <p className={styles.directiveBody}>
                                    Primitive types strictly correspond to fixed-width implementations (<code>i32</code>, <code>u64</code>). Implicit conversions between integer types are strictly forbidden. Algebraic Data Types (ADTs) enforce exhaustiveness in pattern matching.
                                </p>
                            </div>

                            <div className={styles.directive}>
                                <div className={styles.directiveHeader}>
                                    <span className={styles.directiveNum}>[03]</span>
                                    <h3 className={styles.directiveTitle}>Variables & Mutability</h3>
                                </div>
                                <p className={styles.directiveBody}>
                                    Bindings are immutable by default. Mutable bindings require the explicit <code>var</code> keyword. Partial initialization is not permitted outside of <code>undefined</code> escape hatches.
                                </p>
                            </div>
                        </div>
                    </section>

                    {/* RIGHT PANEL: CODE DATA WINDOW */}
                    <aside className={styles.codePanel}>
                        <div className={styles.panelHeader}>
                            <span>TARGET: AST_spec.ln</span>
                            <span>[ANALYSIS]</span>
                        </div>

                        <div className={styles.codeScroll}>
                            <div className={styles.codeBlock}>
                                <div className={styles.codeComment}>// SPEC: ADT MATCHING</div>
                                <pre><code>
                                    <span className={styles.kw}>type</span> Shape {'{'}
                                    Circle {'{'} r <span className={styles.type}>int</span> {'}'}
                                    Rectangle {'{'} w <span className={styles.type}>int</span>, h <span className={styles.type}>int</span> {'}'}
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
