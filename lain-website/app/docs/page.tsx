import NaviShell from "@/components/NaviShell";
import SpecViewer from "@/components/SpecViewer";
import { specData } from "./specData";
import styles from "../page.module.css";

export default function Documentation() {
    const statusLines = [
        "DOC_SECTOR ACCESSED",
        "FULL_SPEC_V0.1.0_LOADED",
        "[ AUTH_REFERENCE_MODE ]",
        "TECHNICAL_REFERENCE: ACTIVE"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            <section className={styles.heroSection}>
                <div className={styles.glitchContainer}>
                    <h1 className={styles.heroTitle} data-text="DOCUMENTATION">DOCUMENTATION</h1>
                </div>
                <p className={styles.heroSubtitle}>
                    The complete, authoritative technical reference for the Lain programming language.
                </p>

                <SpecViewer data={specData} />
            </section>
        </NaviShell>
    );
}
