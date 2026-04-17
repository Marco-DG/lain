import NaviShell from "@/components/NaviShell";
import DocViewer from "@/components/DocViewer";
import { docData } from "./docData";

export default function Documentation() {
    const statusLines = [
        "DOC_SECTOR ACCESSED",
        "FULL_SPEC_V0.1.0_LOADED",
        "[ AUTH_REFERENCE_MODE ]",
        "TECHNICAL_REFERENCE: ACTIVE"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            <DocViewer data={docData} />
        </NaviShell>
    );
}
