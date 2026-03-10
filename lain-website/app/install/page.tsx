import styles from "../page.module.css";
import NaviShell from "@/components/NaviShell";

export default function Install() {
    const statusLines = [
        "INSTALL_SEQ INITIATED",
        "ARCH: X86_64 / ARM64",
        "[ WAITING_FOR_USER ]"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            {/* CENTER PANEL: CONTENT */}
            <section className={styles.mainContent}>
                <h2 className={styles.title} style={{ fontSize: '4rem' }}>Installation</h2>
                <span className={styles.subtitle}>setup your environment</span>

                <div className={styles.pitch}>
                    <p>Lain is distributed as source code and relies on <strong>Cosmopolitan Libc (cosmocc)</strong> to compile into Actually Portable Executables (APE). Follow the steps below to initialize the compiler locally.</p>
                </div>

                <div className={styles.directives}>
                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[01]</span>
                            <h3 className={styles.directiveTitle}>Clone Repository</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Fetch the latest source code directly from the master branch.
                        </p>
                        <div className={styles.codeBlock} style={{ marginBottom: 0, marginTop: '2rem' }}>
                            <pre style={{ padding: '1rem', background: '#09090b' }}><code><span className={styles.kw}>git</span> clone https://github.com/Marco-DG/lain.git<br /><span className={styles.kw}>cd</span> lain</code></pre>
                        </div>
                    </div>

                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[02]</span>
                            <h3 className={styles.directiveTitle}>Build Compiler</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            Ensure <code>make</code> and a standard C compiler toolkit are installed, then run the build target.
                        </p>
                        <div className={styles.codeBlock} style={{ marginBottom: 0, marginTop: '2rem' }}>
                            <pre style={{ padding: '1rem', background: '#09090b' }}><code><span className={styles.kw}>make</span> build</code></pre>
                        </div>
                    </div>

                    <div className={styles.directive}>
                        <div className={styles.directiveHeader}>
                            <span className={styles.directiveNum}>[03]</span>
                            <h3 className={styles.directiveTitle}>Execute lain</h3>
                        </div>
                        <p className={styles.directiveBody}>
                            The resulting <code>lain</code> binary is self-contained. You can now compile <code>.ln</code> source files.
                        </p>
                        <div className={styles.codeBlock} style={{ marginBottom: 0, marginTop: '2rem' }}>
                            <pre style={{ padding: '1rem', background: '#09090b' }}><code>./lain build <span className={styles.str}>src/main.ln</span></code></pre>
                        </div>
                    </div>
                </div>
            </section>

            {/* RIGHT PANEL: CODE DATA WINDOW */}
            <aside className={styles.codePanel}>
                <div className={styles.panelHeader}>
                    <span>TARGET: makefile</span>
                    <span>[BUILD]</span>
                </div>

                <div className={styles.codeScroll}>
                    <div className={styles.codeBlock}>
                        <div className={styles.codeComment}>// BUILD_OUTPUT</div>
                        <pre><code>
                            <span className={styles.str}>CC src/lexer.c</span><br />
                            <span className={styles.str}>CC src/parser.c</span><br />
                            <span className={styles.str}>CC src/typecheck.c</span><br />
                            <span className={styles.str}>CC src/borrowck.c</span><br />
                            <span className={styles.kw}>LD lain</span><br />
                            <span className={styles.type}>Build successful.</span>
                        </code></pre>
                    </div>
                </div>
            </aside>
        </NaviShell>
    );
}
