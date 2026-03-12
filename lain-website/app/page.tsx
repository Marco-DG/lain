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
          A statically-typed, compiled systems language engineered for an unpredictable world. Lain strips away runtime illusions, offering the raw power of memory-mapped reality bounded by mathematically-proven <strong>linearity</strong> and <strong>determinism</strong>.
        </p>

        <div className={styles.directives}>
          <div className={styles.directive}>
            <div className={styles.directiveHeader}>
              <span className={styles.directiveNum}>[01]</span>
              <h3 className={styles.directiveTitle}>The Architecture of Control</h3>
            </div>
            <p className={styles.directiveBody}>
              There is no garbage collector to save you, and none to slow you down. Lain enforces a brutal, elegant linearity. Ownership is absolute. Memory is passed explicitly via the <code>mov</code> directive, ensuring every byte is accounted for at compile time.
            </p>
          </div>

          <div className={styles.directive}>
            <div className={styles.directiveHeader}>
              <span className={styles.directiveNum}>[02]</span>
              <h3 className={styles.directiveTitle}>Provable Truths</h3>
            </div>
            <p className={styles.directiveBody}>
              Bugs are not inevitable; they are a failure of specification. Through rigorous Value Range Analysis (VRA), Lain mathematically proves the absence of undefined behavior. Array bounds and divisions are verified in the compiler. If it builds, it is mathematically sound.
            </p>
          </div>

          <div className={styles.directive}>
            <div className={styles.directiveHeader}>
              <span className={styles.directiveNum}>[03]</span>
              <h3 className={styles.directiveTitle}>The End of Chaos</h3>
            </div>
            <p className={styles.directiveBody}>
              Side-effects are the enemy of reason. Lain establishes a hard boundary between pure mathematical functions (<code>func</code>) and side-effecting procedures (<code>proc</code>). Purity is guaranteed. Execution is deterministic. The machine acts exactly as the math dictates.
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
              <span className={styles.kw}>func</span> process_file(<span className={styles.kw}>mov</span> {'{'}handle{'}'} File) {'{'}{"\n"}
              {'  '}fclose(handle){"\n"}
              {'}'}{"\n"}
              {"\n"}
              <span className={styles.kw}>proc</span> main() {'{'}{"\n"}
              {'  '}<span className={styles.kw}>var</span> f = open_file(<span className={styles.str}>"data.txt"</span>, <span className={styles.str}>"r"</span>){"\n"}
              {'  '}process_file(<span className={styles.kw}>mov</span> f){"\n"}
              {"\n"}
              {'  '}<span className={styles.err}>// ERROR [E001]: f was moved</span>{"\n"}
              {'  '}<span className={styles.err}>// process_file(mov f)</span>{"\n"}
              {'}'}
            </code></pre>
          </div>

          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>// DIRECTIVE 02: VALUE RANGE ANALYSIS</div>
            <pre><code>
              <span className={styles.kw}>func</span> safe_div(a <span className={styles.type}>int</span>, b <span className={styles.type}>int</span> != 0) <span className={styles.type}>int</span> {'{'}{"\n"}
              {'  '}<span className={styles.kw}>return</span> a / b{"\n"}
              {'}'}{"\n"}
              {"\n"}
              <span className={styles.kw}>func</span> get(arr <span className={styles.type}>int</span>[10], i <span className={styles.type}>int</span> in arr) <span className={styles.type}>int</span> {'{'}{"\n"}
              {'  '}<span className={styles.kw}>return</span> arr[i]{"\n"}
              {'}'}
            </code></pre>
          </div>

          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>// DIRECTIVE 03: DETERMINISM</div>
            <pre><code>
              <span className={styles.kw}>func</span> fib(n <span className={styles.type}>int</span>) <span className={styles.type}>int</span> {'>'}= 0 {'{'}{"\n"}
              {'  '}<span className={styles.kw}>if</span> n {'<'} 2 {'{'} <span className={styles.kw}>return</span> n {'}'}{"\n"}
              {'  '}<span className={styles.kw}>return</span> fib(n-1) + fib(n-2){"\n"}
              {'}'}{"\n"}
              {"\n"}
              <span className={styles.err}>// ERROR [E011]: func cannot call proc</span>{"\n"}
              <span className={styles.err}>// func bad() {'{'} io.print(1) {'}'}</span>
            </code></pre>
          </div>
        </div>
      </aside>
    </NaviShell>
  );
}
