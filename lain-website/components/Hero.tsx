import React from 'react';
import styles from './Hero.module.css';

export default function Hero() {
    return (
        <section className={styles.hero}>
            {/* Background elements */}
            <div className={styles.hexDump} aria-hidden="true">
                {Array.from({ length: 40 }).map((_, i) => (
                    <div key={i}>
                        [ffffffff810020d0] do_one_initcall+0x80/0x280
                        <br />
                        [ffffffff810c8481] sys_init_module+0xe1/0x250
                        <br />
                        Code: 20 7d 3d a0 74 1d 48 8d 3c 52 49 89 f8 49
                    </div>
                ))}
            </div>

            <div className={styles.container}>
                {/* Left Column: Pitch */}
                <div className={styles.pitch}>
                    <div className={styles.titleWrapper}>
                        <h1 className={`${styles.title} lain-glitch`} data-text="lain">lain</h1>
                        <span className={styles.subtitle}>systems programming language</span>
                    </div>

                    <p className={styles.description}>
                        A statically typed, compiled language offering assembly-speed performance with <strong>zero-cost memory safety</strong> and <strong>absolute determinism</strong>.
                    </p>

                    <div className={styles.actions}>
                        <a href="#install" className={styles.buttonMain}>Install Lain</a>
                        <a href="#docs" className={styles.buttonWire}>Read the Docs</a>
                    </div>
                </div>

                {/* Right Column: Code Showcase */}
                <div className={styles.codeWindow}>
                    <div className={styles.codeHeader}>
                        <span className={styles.fileName}>main.ln</span>
                        <div className={styles.status}>[SAFE]</div>
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
