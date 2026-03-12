import NaviShell from "@/components/NaviShell";
import SpecViewer from "@/components/SpecViewer";

export default function Spec() {
    const statusLines = [
        "SPEC_SECTOR ACCESSED",
        "DRAFT: v0.1.0",
        "[ STRICT_MODE_ENV ]"
    ];

    return (
        <NaviShell statusLines={statusLines}>
            <SpecViewer />
        </NaviShell>
    );
}
