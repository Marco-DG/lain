import NaviShell from "@/components/NaviShell";
import SpecViewer, { SpecChapter } from "@/components/SpecViewer";
import { parseLanguage } from "../docs/readmeParser";

// /overview renders LANGUAGE.md out of the repository at build time, the same way /docs renders
// README.md. It used to render `overviewData.ts`, 33 hand-written chapters paraphrasing the
// manual, and by 2026-10-01 only 1 of its 35 Lain samples still compiled: it taught the removed
// `proc` keyword, a func/proc duality the language no longer has, and a `safe_div` sample the
// compiler rejects. A paraphrase of the manual is a second copy of the manual, and the second
// copy is the one nobody updates.
export default function Overview() {
    const data: SpecChapter[] = parseLanguage().map(s => ({
        id: s.id,
        title: s.title,
        content: s.content ?? "",
        code: s.code,
    }));

    return (
        <NaviShell>
            <SpecViewer data={data} />
        </NaviShell>
    );
}
