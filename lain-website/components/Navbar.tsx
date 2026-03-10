import React from 'react';
import styles from './Navbar.module.css';

export default function Navbar() {
    return (
        <nav className={styles.nav}>
            <div className={styles.logo}>
                <span className={styles.logoPrefix}>sys.</span>lain
            </div>
            <div className={styles.links}>
                <a href="#about" className={styles.link}>[ Documentation ]</a>
                <a href="#features" className={styles.link}>[ Specifications ]</a>
                <a href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer" className={styles.link}>[ Source_Code ]</a>
            </div>
        </nav>
    );
}
