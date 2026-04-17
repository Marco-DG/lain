'use client';

import React from 'react';
import styles from './NaviShell.module.css';
import Sidebar from './Sidebar';

interface NaviShellProps {
    children: React.ReactNode;
    statusLines?: string[];
    layout?: 'default' | 'docs';
}

export default function NaviShell({ children, statusLines, layout = 'default' }: NaviShellProps) {
    const shellClass = layout === 'docs'
        ? `${styles.naviShell} ${styles.naviShellDocs}`
        : styles.naviShell;

    return (
        <>
            <div className={styles.noise}></div>
            <div className={styles.scanlines}></div>
            <div className={styles.vignette}></div>
            <div className={styles.flicker}></div>

            <main className={styles.container}>
                <div className={shellClass}>
                    <Sidebar statusLines={statusLines} />
                    {children}
                </div>
            </main>
        </>
    );
}
