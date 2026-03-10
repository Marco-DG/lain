import React from 'react';
import styles from './TerminalWindow.module.css';

export default function TerminalWindow() {
    return (
        <section id="about" className={styles.section}>
            <div className={styles.terminal}>
                <div className={styles.header}>
                    <div className={styles.title}>NAVI_OS // TERMINAL</div>
                    <div className={styles.controls}>
                        <span>[_]</span>
                        <span>[X]</span>
                    </div>
                </div>
                <div className={styles.body}>
                    <div className={styles.line}>
                        <span className={styles.prompt}>navi@local:~#</span>
                        <span className={styles.command}>cat hello.ln</span>
                    </div>
                    <div className={styles.output}>
                        <span className={styles.keyword}>func</span> main() -{'>'} <span className={styles.type}>int</span> {'{'}
                        <br />
                        {'  '}io.print(<span className={styles.string}>"Hello, Wired.\\n"</span>)
                        <br />
                        {'  '}<span className={styles.keyword}>return</span> <span className={styles.number}>0</span>
                        <br />
                        {'}'}
                    </div>
                    <br />
                    <div className={styles.line}>
                        <span className={styles.prompt}>navi@local:~#</span>
                        <span className={styles.command}>lain build hello.ln --release</span>
                    </div>
                    <div className={styles.output}>
                        [SYS] Compiling module: hello.ln
                        <br />
                        [CHK] Pass 1: Lexical analysis... <span className={styles.success}>OK</span>
                        <br />
                        [CHK] Pass 4: Type checking & constraints... <span className={styles.success}>OK</span>
                        <br />
                        [CHK] Pass 7: Linearity & ownership... <span className={styles.success}>OK</span>
                        <br />
                        [GEN] Emitting C99 target... <span className={styles.success}>OK</span>
                        <br />
                        === SYSTEM SECURE. ZERO SAFETY VIOLATIONS. ===
                    </div>
                    <br />
                    <div className={styles.line}>
                        <span className={styles.prompt}>navi@local:~#</span>
                        <span className={styles.cursor}>_</span>
                    </div>
                </div>
                <div className={styles.footer}>
                    <span>Lain protocol active</span>
                    <span>MEM: 0x00000000</span>
                </div>
            </div>
        </section>
    );
}
