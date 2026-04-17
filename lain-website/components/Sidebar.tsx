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
                    <div className={styles.logoMetadata}>
                        <span>PROTOCOL: v3.3</span>
                        <span>SIG: 0xLAIN</span>
                    </div>
                </div>

                <nav className={styles.navLinks}>
                    <div className={`${styles.navGroup} ${isLinkActive('/overview') ? styles.navGroupActive : ''}`}>
                        <span className={styles.navLabel}>SEC_01 . CORE</span>
                        <Link href="/overview" className={`${styles.navLink} ${isLinkActive('/overview') ? styles.navLinkActive : ''}`}>
                            OVERVIEW
                        </Link>
                    </div>

                    <div className={`${styles.navGroup} ${isLinkActive('/docs') ? styles.navGroupActive : ''}`}>
                        <span className={styles.navLabel}>SEC_02 . DOCS</span>
                        <Link href="/docs" className={`${styles.navLink} ${isLinkActive('/docs') ? styles.navLinkActive : ''}`}>
                            DOCUMENTATION
                        </Link>
                    </div>

                    <div className={styles.navGroup}>
                        <span className={styles.navLabel}>SEC_03 . REPO</span>
                        <a className={styles.navLink} href="https://github.com/Marco-DG/lain" target="_blank" rel="noopener noreferrer">
                            SOURCE_CODE
                        </a>
                    </div>

                    <div className={`${styles.navGroup} ${isLinkActive('/install') ? styles.navGroupActive : ''}`} style={{ marginTop: '2rem' }}>
                        <span className={styles.navLabel} style={{ color: 'var(--accent-teal)' }}>SYS_EXEC . INSTALL</span>
                        <Link href="/install" className={`${styles.navLink} ${isLinkActive('/install') ? styles.navLinkActive : ''}`} style={{ color: 'var(--accent-teal)' }}>
                            INSTALL_LAIN
                        </Link>
                    </div>
                </nav>

                {statusLines && (
                    <div className={styles.sysStatus}>
                        {statusLines.map((line, i) => (
                            <React.Fragment key={i}>
                                {line}
                                {i < statusLines.length - 1 && <br />}
                            </React.Fragment>
                        ))}
                    </div>
                )}
            </div>

            <div className={styles.sidebarBottom}>
                <div className={styles.dataLine}></div>
                <span className={styles.buildInfo}>BUILD: 2026.03.10 // WIRED</span>
            </div>
        </aside>
    );
}
