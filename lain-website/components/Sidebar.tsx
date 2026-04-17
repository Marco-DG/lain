'use client';

import React from 'react';
import Link from 'next/link';
import { usePathname } from 'next/navigation';
import styles from './NaviShell.module.css';

interface SidebarProps {
    statusLines?: string[];
}

export default function Sidebar({ statusLines }: SidebarProps) {
    const pathname = usePathname();

    const isLinkActive = (href: string) => {
        if (href === '/') return pathname === '/';
        return pathname.startsWith(href);
    };

    return (
        <aside className={styles.sidebar}>
            <div className={styles.sidebarTop}>
                <div className={styles.logoContainer}>
                    <Link href="/" style={{ textDecoration: 'none' }}>
                        <h1 className={styles.logo}>[ sys.lain ]</h1>
                    </Link>
                </div>

                <nav className={styles.navLinks}>
                    <div className={`${styles.navGroup} ${isLinkActive('/overview') ? styles.navGroupActive : ''}`}>
                        <Link href="/overview" className={`${styles.navLink} ${isLinkActive('/overview') ? styles.navLinkActive : ''}`}>
                            OVERVIEW
                        </Link>
                    </div>

                    <div className={`${styles.navGroup} ${isLinkActive('/docs') ? styles.navGroupActive : ''}`}>
                        <Link href="/docs" className={`${styles.navLink} ${isLinkActive('/docs') ? styles.navLinkActive : ''}`}>
                            DOCUMENTATION
                        </Link>
                    </div>

                    <div className={styles.navGroup}>
                        <a className={styles.navLink} href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer">
                            GITHUB <span style={{ fontSize: '0.8em', verticalAlign: 'middle', opacity: 0.7 }}>↗</span>
                        </a>
                    </div>
                </nav>

                {statusLines && statusLines.length > 0 && (
                    <div className={styles.sysStatus}>
                        {statusLines.map((line, i) => (
                            <div key={i}>{line}</div>
                        ))}
                    </div>
                )}
            </div>

            <div className={styles.sidebarBottom}>
                <div className={styles.dataLine}></div>
                <span className={styles.buildInfo}>2026.03.10</span>
            </div>
        </aside>
    );
}
