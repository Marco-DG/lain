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

            <div className={styles.verticalKanji}>
                シリアルエクスペリメンツ・レイン
            </div>

            <div className={styles.content}>
                <div className={styles.titleWrapper}>
                    <h1 className={`${styles.title} lain-glitch`} data-text="lain">lain</h1>
                    <span className={styles.subtitle}>programming language</span>
                </div>

                <p className={styles.description}>
                    Close the world. Open the neXt. Let's all love Lain.
                </p>

                <div className={styles.actions}>
                    <a href="#about" className={styles.buttonMain}>Initialize</a>
                    <a href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer" className={styles.buttonWire}>Access Wired Source</a>
                </div>
            </div>
        </section>
    );
}
