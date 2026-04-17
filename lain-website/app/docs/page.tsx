import NaviShell from "@/components/NaviShell";
import SpecViewer from "@/components/SpecViewer";
import { specData } from "./specData";
import styles from "../page.module.css";

export default function Documentation() {
    const statusLines: string[] = [];

    return (
        <NaviShell statusLines={statusLines}>
            <SpecViewer data={specData} />
        </NaviShell>
    );
}
