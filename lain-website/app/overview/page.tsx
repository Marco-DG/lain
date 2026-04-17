import NaviShell from "@/components/NaviShell";
import SpecViewer from "@/components/SpecViewer";
import { overviewData } from "./overviewData";

export default function Overview() {
    const statusLines = [
        "SPEC_SECTOR ACCESSED",
        "DRAFT: v0.1.0",
        "[ STRICT_MODE_ENV ]",
        "MODE: OVERVIEW"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            <SpecViewer data={overviewData} />
        </NaviShell>
    );
}
