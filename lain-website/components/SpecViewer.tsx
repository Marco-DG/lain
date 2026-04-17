'use client';

import React, { useState, useEffect, useRef } from 'react';
import styles from './SpecViewer.module.css';

export interface SpecChapter {
    id: string;
    title: string;
    content: string; // HTML content, can contain <section data-code="..."> tags
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

    useEffect(() => {
        setActiveCode(chapter.code || null);
    }, [currentIndex, chapter.code]);

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
                        SECTION {String(currentIndex + 1).padStart(2, '0')} // {data.length}
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
                <div className={styles.panelHeader}>
                    <span>Source Visualizer</span>
                    <span>{chapter.id}.ln</span>
                </div>
                <div className={styles.codeScroll}>
                    {activeCode ? (
                        <div className={styles.codeBlockContainer}>
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

