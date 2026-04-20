import NaviShell from "@/components/NaviShell";
import Link from "next/link";
import styles from "./Blog.module.css";

export default function BlogList() {
    return (
        <NaviShell>
            <div className={styles.container}>
                <h1 className={styles.title}>sys.log</h1>
                <p className={styles.subtitle}>Technical insights, compiler development, and research from the Lain project.</p>
                
                <div className={styles.articles}>
                    <Link href="/blog/empirical-results" className={styles.articleCard}>
                        <div className={styles.articleDate}>Apr 20, 2026</div>
                        <h2 className={styles.articleTitle}>Empirical Results: Memory Safety at Compile Time</h2>
                        <p className={styles.articleExcerpt}>
                            A comparative analysis of how Lain detects memory errors (Use-after-free, leaks, buffer overflows) at compile time versus C/C++ tools like GCC, Clang, and Valgrind. Uncovering the blind spots of traditional runtime diagnostics.
                        </p>
                    </Link>
                </div>
            </div>
        </NaviShell>
    );
}
