import React from 'react';
import styles from './FeatureGrid.module.css';

const features = [
    {
        id: '01',
        title: 'Speed / Execution',
        description: 'Zero runtime overhead. No garbage collector, no reference counting, no hidden allocations. Generated code is as fast as hand-written C.',
    },
    {
        id: '02',
        title: 'Absolute Control',
        description: 'Guaranteed absence of use-after-free, double-free, data races, buffer overflows, and memory leaks — enforced entirely at compile time.',
    },
    {
        id: '03',
        title: 'Static Proof',
        description: 'Compile-time proof of program properties via Value Range Analysis and exhaustive pattern matching. No SMT solver required.',
    },
    {
        id: '04',
        title: 'Determinism',
        description: 'Pure functions are guaranteed to terminate. The output is predictable, the side effects are controlled. Chaos is isolated.',
    },
    {
        id: '05',
        title: 'Bare Syntax',
        description: 'Clean, transparent syntax with no hidden magic. Explicit ownership. No implicit conversions. You see exactly what happens.',
    }
];

export default function FeatureGrid() {
    return (
        <section id="features" className={styles.section}>
            <div className={styles.headerArea}>
                <h2 className={styles.sectionTitle}>System.Properties</h2>
                <span className={styles.barcode}>01011001 01101111 01110101</span>
            </div>
            <div className={styles.grid}>
                {features.map((feature) => (
                    <div key={feature.id} className={styles.card}>
                        <div className={styles.cardTop}>
                            <span className={styles.id}>FILE_{feature.id}</span>
                            <div className={styles.separator}></div>
                        </div>
                        <h3 className={styles.title}>{feature.title}</h3>
                        <p className={styles.description}>{feature.description}</p>
                    </div>
                ))}
            </div>
        </section>
    );
}
