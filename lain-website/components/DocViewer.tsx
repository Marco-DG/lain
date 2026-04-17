'use client';

import React, { useState, useEffect, useRef } from 'react';
import styles from './DocViewer.module.css';

export interface DocSection {
    id: string;
    title: string;
    level: 1 | 2 | 3;
    content?: string;
}

interface DocViewerProps {
    data: DocSection[];
}

export default function DocViewer({ data }: DocViewerProps) {
    const [activeId, setActiveCode] = useState<string>('');
    const containerRef = useRef<HTMLDivElement>(null);

    useEffect(() => {
        const observerOptions = {
            root: containerRef.current,
            threshold: 0.1,
            rootMargin: '-10% 0px -70% 0px'
        };

        const observer = new IntersectionObserver((entries) => {
            entries.forEach((entry) => {
                if (entry.isIntersecting) {
                    setActiveCode(entry.target.id);
                }
            });
        }, observerOptions);

        const sections = document.querySelectorAll('[data-doc-section]');
        sections.forEach(el => observer.observe(el));

        return () => observer.disconnect();
    }, []);

    const scrollTo = (id: string) => {
        const element = document.getElementById(id);
        if (element && containerRef.current) {
            element.scrollIntoView({ behavior: 'smooth' });
        }
    };

    return (
        <>
            {/* COLUMN 2: FULL DOCUMENTATION */}
            <section className={styles.container} ref={containerRef}>
                <div className={styles.document}>
                    {data.map((section) => (
                        <div 
                            key={section.id} 
                            id={section.id} 
                            data-doc-section 
                            className={`${styles.section} ${styles[`level${section.level}`]}`}
                        >
                            {section.level === 1 && <h1 className={styles.h1}>{section.title}</h1>}
                            {section.level === 2 && <h2 className={styles.h2}>{section.title}</h2>}
                            {section.level === 3 && <h3 className={styles.h3}>{section.title}</h3>}
                            
                            {section.content && (
                                <div 
                                    className={styles.content} 
                                    dangerouslySetInnerHTML={{ __html: section.content }} 
                                />
                            )}
                        </div>
                    ))}
                </div>
            </section>

            {/* COLUMN 3: TABLE OF CONTENTS */}
            <aside className={styles.tocPanel}>
                <div className={styles.tocHeader}>Index</div>
                <nav className={styles.tocList}>
                    {data.map((section) => (
                        <button
                            key={section.id}
                            onClick={() => scrollTo(section.id)}
                            className={`${styles.tocItem} ${styles[`tocLevel${section.level}`]} ${activeId === section.id ? styles.tocActive : ''}`}
                        >
                            {section.title}
                        </button>
                    ))}
                </nav>
            </aside>
        </>
    );
}
