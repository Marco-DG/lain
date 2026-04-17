import NaviShell from "@/components/NaviShell";
import SpecViewer from "@/components/SpecViewer";
import { overviewData } from "./overviewData";

export default function Overview() {
    return (
        <NaviShell>
            <SpecViewer data={overviewData} />
        </NaviShell>
    );
}
