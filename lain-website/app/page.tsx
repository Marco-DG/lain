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
          Lain is a statically-typed, ahead-of-time compiled systems programming language designed for deterministic resource management and formal safety. By unifying linear logic with value-range analysis, the Lain compiler enforces memory-safe constructs and eliminates undefined behavior at compile-time with zero runtime overhead.
        </p>

      </section>

      {/* RIGHT PANEL: CODE DATA WINDOW */}
      <aside className={styles.codePanel}>

        <div className={styles.codeScroll}>
          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>LINEAR OWNERSHIP</div>
            <pre><code>
              <span className={styles.kw}>type</span> File {'{'} <span className={styles.kw}>mov</span> handle <span className={styles.type}>int</span> {'}'}{"\n"}
              {"\n"}
              <span className={styles.kw}>proc</span> close_file(<span className={styles.kw}>mov</span> {'{'}handle{'}'} File) {'{'}{"\n"}
              {'  '}fclose(handle){"\n"}
              {'}'}{"\n"}
              {"\n"}
              <span className={styles.kw}>proc</span> main() {'{'}{"\n"}
              {'  '}<span className={styles.kw}>var</span> f = open_file(<span className={styles.str}>"data.txt"</span>, <span className={styles.str}>"r"</span>){"\n"}
              {'  '}close_file(<span className={styles.kw}>mov</span> f){"\n"}
              {"\n"}
              {'  '}<span className={styles.err}>// ERROR [E001]: f was moved</span>{"\n"}
              {'  '}<span className={styles.err}>// close_file(mov f)</span>{"\n"}
              {'}'}
            </code></pre>
          </div>

          <div className={styles.codeBlock}>
            <div className={styles.codeComment}>VALUE RANGE ANALYSIS</div>
            <pre><code>
              <span className={styles.kw}>func</span> safe_div(a <span className={styles.type}>int</span>, b <span className={styles.type}>int</span> != 0) <span className={styles.type}>int</span> {'{'}{"\n"}
              {'  '}<span className={styles.kw}>return</span> a / b{"\n"}
              {'}'}{"\n"}
              {"\n"}
              <span className={styles.kw}>func</span> get(arr <span className={styles.type}>int</span>[10], i <span className={styles.type}>int</span> <span className={styles.kw}>in</span> arr) <span className={styles.type}>int</span> {'{'}{"\n"}
              {'  '}<span className={styles.kw}>return</span> arr[i]{"\n"}
              {'}'}
            </code></pre>
          </div>
        </div>

      </aside>
    </NaviShell>
  );
}
