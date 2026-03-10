import React from 'react';
import Link from 'next/link';
import styles from './Navbar.module.css';

export default function Navbar() {
    return (
        <nav className={styles.nav}>
            <div className={styles.logo}>
                <span className={styles.logoPrefix}>sys.</span><Link href="/">lain</Link>
            </div>
            <div className={styles.links}>
                <Link href="/docs" className={styles.link}>[ Documentation ]</Link>
                <Link href="/spec" className={styles.link}>[ Specifications ]</Link>
                <Link href="/install" className={styles.link}>[ Install ]</Link>
                <a href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer" className={styles.link}>[ Source ]</a>
            </div>
        </nav>
    );
}
