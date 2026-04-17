'use client';

import React, { useState, useEffect, useRef } from 'react';
import styles from './SpecViewer.module.css';

export interface SpecChapter {
    id: string;
    title: string;
    content: string; // HTML content, can contain tags with data-code="..."
    code?: string;   // Default code if no section is active
}

interface SpecViewerProps {
    data: SpecChapter[];
}

export default function SpecViewer({ data }: SpecViewerProps) {
    const [currentIndex, setCurrentIndex] = useState(0);
    const [activeCode, setActiveCode] = useState<string | null>(null);
    const chapter = data[currentIndex];
    const contentRef = useRef<HTMLDivElement>(null);
    const containerRef = useRef<HTMLDivElement>(null);

    const next = () => {
        if (currentIndex < data.length - 1) {
            setCurrentIndex(currentIndex + 1);
            if (containerRef.current) containerRef.current.scrollTop = 0;
        }
    };

    const prev = () => {
        if (currentIndex > 0) {
            setCurrentIndex(currentIndex - 1);
            if (containerRef.current) containerRef.current.scrollTop = 0;
        }
    };

    // Reset and Initial Code
    useEffect(() => {
        setActiveCode(chapter.code || null);
    }, [currentIndex, chapter.code]);

    // Intersection Observer for scroll-triggered code
    useEffect(() => {
        const observerOptions = {
            root: containerRef.current,
            threshold: 0.6, // Trigger when 60% of the element is visible
            rootMargin: '-10% 0px -40% 0px' // Focus on the upper-middle part of the view
        };

        const observer = new IntersectionObserver((entries) => {
            entries.forEach((entry) => {
                if (entry.isIntersecting) {
                    const code = entry.target.getAttribute('data-code');
                    if (code) {
                        // We use a small trick: the attribute contains the code directly 
                        // or we could use an ID to look up in a dictionary.
                        // For simplicity, we'll assume the code is passed or handled via a map.
                        setActiveCode(decodeURIComponent(code));
                    }
                }
            });
        }, observerOptions);

        // Find all elements with data-code in the rendered HTML
        if (contentRef.current) {
            const triggerElements = contentRef.current.querySelectorAll('[data-code]');
            triggerElements.forEach(el => observer.observe(el));
        }

        return () => observer.disconnect();
    }, [currentIndex, chapter.content]);

    const highlightLain = (code: string) => {
        let escaped = code
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');

        const regex = /\b(func|proc|fun|var|mov|return|type|let|if|elif|else|while|for|match|case|extern|comptime|undefined|as|import|c_include|defer|unsafe|and|or|break|continue|in|true|false)\b|\b(int|i8|i16|i32|i64|u8|u16|u32|u64|isize|usize|f32|f64|bool|void|string|File|Data|Buffer|Result|Option)\b|("[^"]*")|(\/\*[\s\S]*?\*\/|\/\/.*)/g;

        return escaped.replace(regex, (match, kw, type, str, com) => {
            if (kw) return `<span class="${styles.kw}">${kw}</span>`;
            if (type) return `<span class="${styles.type}">${type}</span>`;
            if (str) return `<span class="${styles.str}">${str}</span>`;
            if (com) return `<span class="${styles.com}">${com}</span>`;
            return match;
        });
    };

    return (
        <>
            {/* COLUMN 2: TEXT CONTENT */}
            <section className={styles.container} ref={containerRef}>
                <div className={styles.chapter} key={chapter.id} ref={contentRef}>
                    <h2 className={styles.title}>{chapter.title}</h2>
                    <div className={styles.content} dangerouslySetInnerHTML={{ __html: chapter.content }} />
                </div>

                <div className={styles.navigation}>
                    <button
                        className={styles.navButton}
                        onClick={prev}
                        disabled={currentIndex === 0}
                    >
                        ← PREV
                    </button>
                    <div className={styles.pageInfo}>
                        <span className={styles.pageCurrent}>{String(currentIndex + 1).padStart(2, '0')}</span>
                        <span className={styles.pageDivider}>/</span>
                        <span className={styles.pageTotal}>{data.length}</span>
                    </div>
                    <button
                        className={styles.navButton}
                        onClick={next}
                        disabled={currentIndex === data.length - 1}
                    >
                        NEXT →
                    </button>
                </div>
            </section>

            {/* COLUMN 3: CODE PANEL */}
            <aside className={styles.codePanel}>
                <div className={styles.codeScroll}>
                    {activeCode ? (
                        <div className={styles.codeBlockContainer} key={activeCode}>
                            <pre><code dangerouslySetInnerHTML={{ __html: highlightLain(activeCode) }} /></pre>
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

