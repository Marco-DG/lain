import NaviShell from "@/components/NaviShell";
import DocViewer from "@/components/DocViewer";
import { parseReadme } from "./readmeParser";

export default function Documentation() {
    const data = parseReadme();
    return (
        <NaviShell layout="docs">
            <DocViewer data={data} />
        </NaviShell>
    );
}
