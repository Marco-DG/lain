'use client';

import React, { useState, useEffect, useRef } from 'react';
import styles from './DocViewer.module.css';

export interface DocSection {
    id: string;
    title: string;
    level: 1 | 2 | 3;
    content?: string;  // HTML prose — no lain code blocks
    code?: string;     // Raw lain code for the panel
}

interface DocViewerProps {
    data: DocSection[];
}

export default function DocViewer({ data }: DocViewerProps) {
    const [activeCode, setActiveCode] = useState<string | null>(null);
    const containerRef = useRef<HTMLDivElement>(null);

    // ── Syntax highlighting (identical to SpecViewer) ─────────────────────────
    const highlightLain = (code: string): string => {
        let s = code
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');

        const regex = /\b(func|proc|fun|var|mov|return|type|if|elif|else|while|for|case|extern|comptime|undefined|as|import|c_include|defer|unsafe|and|or|break|continue|in|true|false|decreasing)\b|\b(int|i8|i16|i32|i64|u8|u16|u32|u64|isize|usize|f32|f64|bool|void)\b|("(?:[^"\\]|\\.)*")|(\/\/[^\n]*|\/\*[\s\S]*?\*\/)/g;

        return s.replace(regex, (match, kw, type, str, com) => {
            if (kw)   return `<span class="${styles.kw}">${kw}</span>`;
            if (type) return `<span class="${styles.type}">${type}</span>`;
            if (str)  return `<span class="${styles.str}">${str}</span>`;
            if (com)  return `<span class="${styles.com}">${com}</span>`;
            return match;
        });
    };

    // ── Seed panel with first section that has code ───────────────────────────
    useEffect(() => {
        const first = data.find(s => s.code);
        if (first?.code) setActiveCode(first.code);
    }, [data]);

    // ── Intersection Observer: update panel as sections scroll into view ──────
    useEffect(() => {
        const observer = new IntersectionObserver(
            (entries) => {
                entries.forEach(entry => {
                    if (entry.isIntersecting) {
                        const encoded = entry.target.getAttribute('data-code');
                        if (encoded) setActiveCode(decodeURIComponent(encoded));
                    }
                });
            },
            {
                root: containerRef.current,
                threshold: 0.15,
                rootMargin: '-5% 0px -60% 0px',
            }
        );

        const sectionsWithCode = containerRef.current?.querySelectorAll('[data-code]') ?? [];
        sectionsWithCode.forEach(el => observer.observe(el));

        return () => observer.disconnect();
    }, [data]);

    return (
        <>
            {/* ── Column 2: Scrollable documentation ──────────────────────── */}
            <section className={styles.container} ref={containerRef}>
                <div className={styles.document}>
                    {data.map(section => (
                        <div
                            key={section.id}
                            id={section.id}
                            data-doc-section
                            {...(section.code
                                ? { 'data-code': encodeURIComponent(section.code) }
                                : {})}
                            className={`${styles.section} ${styles[`level${section.level}`]}`}
                        >
                            {section.level === 1 && (
                                <h1 className={styles.h1}>{section.title}</h1>
                            )}
                            {section.level === 2 && (
                                <h2 className={styles.h2}>{section.title}</h2>
                            )}
                            {section.level === 3 && (
                                <h3 className={styles.h3}>{section.title}</h3>
                            )}

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

            {/* ── Column 3: Live code panel ────────────────────────────────── */}
            <aside className={styles.codePanel}>
                <div className={styles.codeScroll}>
                    {activeCode ? (
                        <div className={styles.codeBlockContainer} key={activeCode}>
                            <pre>
                                <code
                                    dangerouslySetInnerHTML={{
                                        __html: highlightLain(activeCode),
                                    }}
                                />
                            </pre>
                        </div>
                    ) : (
                        <div className={styles.noCode}>
                            // NO SPECIMEN DETECTED
                        </div>
                    )}
                </div>
            </aside>
        </>
    );
}
