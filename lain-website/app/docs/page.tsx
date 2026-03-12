import styles from "../page.module.css";
import NaviShell from "@/components/NaviShell";
import Link from "next/link";

export default function Docs() {
    const statusLines = [
        "PHILOSOPHY_EXPANSION_LOADED",
        "FORMAT: SYSTEM_CORE",
        "AUTHORITATIVE_SPEC_V0.0"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            {/* CENTER PANEL: CONTENT */}
            <section className={styles.mainContent}>
                <h2 className={styles.title} style={{ fontSize: '4rem' }}>The Five Pillars</h2>
                <span className={styles.subtitle}>non-negotiable design goals</span>

                <div className={styles.pitch}>
                    <p>Lain's design is governed by five pillars in strict priority order. When design decisions conflict, higher-priority pillars take precedence. It is the single authoritative reference for the architecture of control.</p>
                </div>

                <div className={styles.directives}>
                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[01]</span>
                            <h3 className={styles.directiveTitle}>Assembly-Speed Performance</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Zero runtime overhead. No garbage collector, no reference counting, no hidden allocations. Generated code shall be as fast as hand-written C.
                        </p>
                    </div>

                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[02]</span>
                            <h3 className={styles.directiveTitle}>Zero-Cost Memory Safety</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Guaranteed absence of use-after-free, double-free, data races, buffer overflows, and memory leaks — all enforced at compile time with no runtime cost.
                        </p>
                    </div>

                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[03]</span>
                            <h3 className={styles.directiveTitle}>Static Verification</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Compile-time proof of program properties: bounds safety via Value Range Analysis (VRA), constraint satisfaction, and exhaustive pattern matching.
                        </p>
                    </div>

                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[04]</span>
                            <h3 className={styles.directiveTitle}>Determinism</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Pure functions (<code>func</code>) are guaranteed to terminate and produce the same output for the same input. Side effects are confined to procedures (<code>proc</code>).
                        </p>
                    </div>

                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[05]</span>
                            <h3 className={styles.directiveTitle}>Syntactic Simplicity</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Clean, readable syntax with no hidden magic. Explicit ownership annotations at call sites. No implicit conversions, no hidden copies.
                        </p>
                    </div>
                </div>

                <div className={styles.ctaContainer}>
                    <Link href="/spec" className={styles.primaryButton}>
                        Initialize Specification Reader →
                    </Link>
                </div>
            </section>

            {/* RIGHT PANEL: SAFETY GUARANTEES */}
            <aside className={styles.codePanel}>
                <div className={styles.panelHeader}>
                    <span>TARGET: safety_guarantees.tab</span>
                    <span>[VERIFIED]</span>
                </div>

                <div className={styles.codeScroll}>
                    <div className={styles.codeBlock}>
                        <div className={styles.codeComment}>// COMPILE-TIME GUARANTEES</div>
                        <table style={{ width: '100%', borderCollapse: 'collapse', color: 'var(--text-dim)', fontSize: '0.85rem' }}>
                            <thead>
                                <tr style={{ borderBottom: '1px solid var(--text-muted)', textAlign: 'left' }}>
                                    <th style={{ padding: '0.5rem 0' }}>CONCERN</th>
                                    <th style={{ padding: '0.5rem 0' }}>MECHANISM</th>
                                </tr>
                            </thead>
                            <tbody>
                                <tr><td style={{ padding: '0.5rem 0' }}>Buffer Overflows</td><td>VRA</td></tr>
                                <tr><td style={{ padding: '0.5rem 0' }}>Use-After-Free</td><td>Linear Types</td></tr>
                                <tr><td style={{ padding: '0.5rem 0' }}>Double Free</td><td>Linear Types</td></tr>
                                <tr><td style={{ padding: '0.5rem 0' }}>Data Races</td><td>Borrow Checker</td></tr>
                                <tr><td style={{ padding: '0.5rem 0' }}>Memory Leaks</td><td>Linear Types</td></tr>
                                <tr><td style={{ padding: '0.5rem 0' }}>Purity Violations</td><td>func/proc split</td></tr>
                            </tbody>
                        </table>
                    </div>
                </div>
            </aside>
        </NaviShell>
    );
}
