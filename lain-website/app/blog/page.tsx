import NaviShell from "@/components/NaviShell";
import Link from "next/link";
import styles from "./Blog.module.css";

export default function BlogList() {
    return (
        <NaviShell layout="blog">
            <div className={styles.container}>
                <div className={styles.inner}>
                    <div className={styles.articles}>
                        <Link href="/blog/comparative-analysis-memory-safety" className={styles.articleCard}>
                            <div className={styles.articleDate}>Apr 20, 2026</div>
                            <h2 className={styles.articleTitle}>Comparative Analysis of Memory Safety: Lain vs. C/C++ Tooling</h2>
                            <p className={styles.articleExcerpt}>
                                A comparative analysis of how Lain detects memory errors (Use-after-free, leaks, buffer overflows) at compile time versus C/C++ tools like GCC, Clang, and Valgrind. Uncovering the blind spots of traditional runtime diagnostics.
                            </p>
                        </Link>
                    </div>
                </div>
            </div>
        </NaviShell>
    );
}
