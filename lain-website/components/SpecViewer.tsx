'use client';

import React, { useState } from 'react';
import styles from './SpecViewer.module.css';
import { specData } from '@/app/spec/specData';

export default function SpecViewer() {
    const [currentIndex, setCurrentIndex] = useState(0);
    const chapter = specData[currentIndex];

    const next = () => {
        if (currentIndex < specData.length - 1) setCurrentIndex(currentIndex + 1);
    };

    const prev = () => {
        if (currentIndex > 0) setCurrentIndex(currentIndex - 1);
    };

    const highlightLain = (code: string) => {
        // 1. Escape HTML entities first to prevent <stdio.h> from being treated as a tag
        let escaped = code
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');

        // 2. Single pass substitution to avoid recursion
        const regex = /\b(func|proc|var|mov|return|type|let|if|else|while|for|match|case|extern|comptime|undefined|as|import|c_include)\b|\b(int|i8|i16|i32|i64|u8|u16|u32|u64|isize|usize|f32|f64|bool|void|string|File|Data|Buffer|Result|Option)\b|("[^"]*")|(\/\*[\s\S]*?\*\/|\/\/.*)/g;

        return escaped.replace(regex, (match, kw, type, str, com) => {
            if (kw) return `<span class="${styles.kw}">${kw}</span>`;
            if (type) return `<span class="${styles.type}">${type}</span>`;
            if (str) return `<span class="${styles.str}">${str}</span>`;
            if (com) return `<span class="${styles.com}">${com}</span>`;
            return match;
        });
    };

    return (
        <section className={styles.container}>
            <div className={styles.chapter} key={chapter.id}>
                <h2 className={styles.title}>{chapter.title}</h2>
                <div className={styles.content}>
                    {chapter.content}
                </div>
                {chapter.code && (
                    <div className={styles.codeBlock}>
                        <pre><code dangerouslySetInnerHTML={{ __html: highlightLain(chapter.code) }} /></pre>
                    </div>
                )}
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
                    SECTION {String(currentIndex + 1).padStart(2, '0')} // {specData.length}
                </div>
                <button
                    className={styles.navButton}
                    onClick={next}
                    disabled={currentIndex === specData.length - 1}
                >
                    NEXT →
                </button>
            </div>
        </section>
    );
}
