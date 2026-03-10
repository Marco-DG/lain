import styles from "./page.module.css";
import NaviShell from "@/components/NaviShell";

export default function Home() {
  return (
    <NaviShell>
      {/* CENTER PANEL: PITCH & FEATURES */}
      <section className={styles.mainContent}>
        <h2 className={styles.title}>lain</h2>
        <span className={styles.subtitle}>systems programming language</span>

        <p className={styles.pitch}>
          A statically typed, compiled language offering assembly-speed performance with <strong>zero-cost memory safety</strong> and <strong>absolute determinism</strong>.
        </p>

        <div className={styles.directives}>
          <div className={styles.directive}>
            <div className={styles.directiveHeader}>
              <span className={styles.directiveNum}>[01]</span>
              <h3 className={styles.directiveTitle}>Zero-Cost Memory Safety</h3>
            </div>
            <p className={styles.directiveBody}>
              Lain uses a linear type system and a strict borrow checker to guarantee memory safety without garbage collection. Ownership transfers are mandatory and explicit via the <code>mov</code> keyword.
            </p>
          </div>

          <div className={styles.directive}>
            <div className={styles.directiveHeader}>
              <span className={styles.directiveNum}>[02]</span>
              <h3 className={styles.directiveTitle}>Mathematical Proofs</h3>
            </div>
            <p className={styles.directiveBody}>
              Instead of runtime checks, Lain relies on Value Range Analysis (VRA) to mathematically prove safety at compile time. Array bounds and division by zero are impossible to violate.
            </p>
          </div>

          <div className={styles.directive}>
            <div className={styles.directiveHeader}>
              <span className={styles.directiveNum}>[03]</span>
              <h3 className={styles.directiveTitle}>Absolute Determinism</h3>
            </div>
            <p className={styles.directiveBody}>
              Lain enforces strict type-level separation between pure mathematical functions (<code>func</code>) and side-effecting procedures (<code>proc</code>). Pure functions are mathematically guaranteed to terminate.
            </p>
          </div>
        </div>
      </section>

      {/* RIGHT PANEL: CODE DATA WINDOW */}
      <aside className={styles.codePanel}>
        <div className={styles.panelHeader}>
          <span>TARGET: source_specs.ln</span>
          <span>[SECURE]</span>
        </div>

        <div className={styles.codeScroll}>
          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>// DIRECTIVE 01: LINEAR OWNERSHIP</div>
            <pre><code>
              <span className={styles.kw}>func process_file(<span className={styles.kw}>mov</span> {'{'}handle{'}'} File) {'{'}</span>
              fclose(handle)
              {'}'}

              <span className={styles.kw}>proc</span> main() {'{'}
              <span className={styles.kw}>var</span> f = open_file(<span className={styles.str}>"data.txt"</span>, <span className={styles.str}>"r"</span>)
              process_file(<span className={styles.kw}>mov</span> f)

              <span className={styles.err}>// ERROR [E001]: f was moved</span>
              <span className={styles.err}>// process_file(mov f)</span>
              {'}'}
            </code></pre>
          </div>

          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>// DIRECTIVE 02: VALUE RANGE ANALYSIS</div>
            <pre><code>
              <span className={styles.kw}>func</span> safe_div(a <span className={styles.type}>int</span>, b <span className={styles.type}>int</span> != 0) <span className={styles.type}>int</span> {'{'}
              <span className={styles.kw}>return</span> a / b
              {'}'}

              <span className={styles.kw}>func</span> get(arr <span className={styles.type}>int</span>[10], i <span className={styles.type}>int</span> in arr) <span className={styles.type}>int</span> {'{'}
              <span className={styles.kw}>return</span> arr[i]
              {'}'}
            </code></pre>
          </div>

          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>// DIRECTIVE 03: DETERMINISM</div>
            <pre><code>
              <span className={styles.kw}>func</span> fib(n <span className={styles.type}>int</span>) <span className={styles.type}>int</span> {'>'}= 0 {'{'}
              <span className={styles.kw}>if</span> n {'<'} 2 {'{'} <span className={styles.kw}>return</span> n {'}'}
              <span className={styles.kw}>return</span> fib(n-1) + fib(n-2)
              {'}'}

              <span className={styles.err}>// ERROR [E011]: func cannot call proc</span>
              <span className={styles.err}>// func bad() {'{'} io.print(1) {'}'}</span>
            </code></pre>
          </div>
        </div>
      </aside>
    </NaviShell>
  );
}
