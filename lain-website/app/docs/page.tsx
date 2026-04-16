import NaviShell from "@/components/NaviShell";
import SpecViewer from "@/components/SpecViewer";

export default function Documentation() {
    const statusLines = [
        "DOC_SECTOR ACCESSED",
        "FULL_SPEC_V0.1.0_LOADED",
        "[ AUTH_REFERENCE_MODE ]"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            <SpecViewer />
        </NaviShell>
    );
}
