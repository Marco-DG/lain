import React from 'react';
import styles from './Hero.module.css';

export default function Hero() {
    return (
        <section className={styles.hero}>
            <div className={styles.container}>
                {/* Left Column: Pitch */}
                <div className={styles.pitch}>
                    <div className={styles.titleWrapper}>
                        <h1 className={`${styles.title} lain-glitch`} data-text="lain">lain</h1>
                        <span className={styles.subtitle}>systems language</span>
                    </div>

                    <p className={styles.description}>
                        A statically typed, compiled language offering assembly-speed performance with <strong>zero-cost memory safety</strong> and <strong>absolute determinism</strong>.
                    </p>

                    <div className={styles.actions}>
                        <a href="#install" className={styles.buttonMain}>Install Lain</a>
                        <a href="#docs" className={styles.buttonWire}>Documentation</a>
                    </div>
                </div>

                {/* Right Column: Code Showcase */}
                <div className={styles.codeWindow}>
                    <div className={styles.codeHeader}>
                        <div className={styles.dots}>
                            <span className={`${styles.dot} ${styles.dotRed}`}></span>
                            <span className={`${styles.dot} ${styles.dotYellow}`}></span>
                            <span className={`${styles.dot} ${styles.dotGreen}`}></span>
                        </div>
                        <span className={styles.fileName}>main.ln</span>
                        <div className={styles.status}>SAFE</div>
                    </div>
                    <pre className={styles.codeBlock}>
                        <code>
                            <span className={styles.cmt}>// Compile-time proof of safety via VRA</span>
                            <span className={styles.kw}>func</span> <span className={styles.fn}>safe_div</span>(a <span className={styles.ty}>int</span>, b <span className={styles.ty}>int</span> != <span className={styles.num}>0</span>) <span className={styles.ty}>int</span> {'{'}
                            <span className={styles.kw}>return</span> a / b
                            {'}'}

                            <span className={styles.kw}>proc</span> <span className={styles.fn}>main</span>() {'{'}
                            <span className={styles.cmt}>// Ownership transfer is explicit</span>
                            <span className={styles.kw}>var</span> f = open_file(<span className={styles.str}>"data.txt"</span>, <span className={styles.str}>"r"</span>)
                            close_file(<span className={styles.kw}>mov</span> f)

                            <span className={styles.cmt}>// ERROR [E001]: f was moved</span>
                            <span className={styles.cmt}>// read_file(f)</span>
                            {'}'}
                        </code>
                    </pre>
                </div>
            </div>
        </section>
    );
}
