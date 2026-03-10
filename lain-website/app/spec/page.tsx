import styles from "../page.module.css";
import NaviShell from "@/components/NaviShell";

export default function Spec() {
    const statusLines = [
        "SPEC_SECTOR ACCESSED",
        "DRAFT: v0.1.0",
        "[ STRICT_MODE_ENV ]"
    ];

    return (
        <NaviShell statusLines={statusLines}>
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
        </NaviShell>
    );
}
