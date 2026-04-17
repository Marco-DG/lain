'use client';

import React from 'react';
import styles from './NaviShell.module.css';
import Sidebar from './Sidebar';

interface NaviShellProps {
    children: React.ReactNode;
    statusLines?: string[];
}

export default function NaviShell({ children, statusLines }: NaviShellProps) {
    return (
        <>
            <div className={styles.noise}></div>
            <div className={styles.scanlines}></div>
            <div className={styles.vignette}></div>
            <div className={styles.flicker}></div>

            <main className={styles.container}>
                <div className={styles.naviShell}>
                    <Sidebar statusLines={statusLines} />
                    {children}
                </div>
            </main>
        </>
    );
}
