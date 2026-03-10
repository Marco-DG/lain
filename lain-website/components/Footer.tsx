import React from 'react';
import styles from './Footer.module.css';

export default function Footer() {
    return (
        <footer className={styles.footer}>
            <div className={styles.noise}></div>
            <div className={styles.content}>
                <div className={styles.warning}>
                    WARNING: SYSTEM SHUTDOWN IMMINENT.<br />
                    PLEASE ENSURE ALL MEMORY IS FREED.
                </div>
                <p className={styles.copy}>
                    <span className={styles.year}>{new Date().getFullYear()}</span>
                    {' // '}
                    LAIN COMPILER PROJECT
                    {' // '}
                    NO WHERE, EVERY WHERE.
                </p>
            </div>
        </footer>
    );
}
