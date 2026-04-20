import fs from 'fs';
import path from 'path';
import type { DocSection } from '@/components/DocViewer';

// ── HTML escape (for inline text) ────────────────────────────────────────────

function escHtml(s: string): string {
    return s
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// For code in <pre> blocks: don't escape quotes (breaks string literal display)
function escCode(s: string): string {
    return s
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
}

// ── Inline markdown → HTML ───────────────────────────────────────────────────

function inlineFormat(text: string): string {
    const fragments: string[] = [];
    text = text.replace(/`([^`]+)`/g, (_, code) => {
        const idx = fragments.length;
        fragments.push(`<code>${escHtml(code)}</code>`);
        return `\x00${idx}\x00`;
    });
    text = escHtml(text);
    text = text.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');
    text = text.replace(/(?<!\*)\*([^*\x00]+)\*(?!\*)/g, '<em>$1</em>');
    text = text.replace(/\\\|/g, '|');
    text = text.replace(/\x00(\d+)\x00/g, (_, i) => fragments[parseInt(i)]);
    return text;
}

// ── Table builder ─────────────────────────────────────────────────────────────

function buildTable(tableLines: string[]): string {
    const rows = tableLines.filter(l => !/^\s*\|[\s\-:|]+\|\s*$/.test(l));
    if (rows.length === 0) return '';
    // Protect escaped pipes (\|) before splitting on |, then restore
    const parseCells = (line: string) =>
        line.replace(/\\\|/g, '\x01')
            .split('|').slice(1, -1)
            .map(c => c.trim().replace(/\x01/g, '|'));
    const [header, ...body] = rows;
    const ths = parseCells(header).map(c => `<th>${inlineFormat(c)}</th>`).join('');
    const trs = body.map(row =>
        `<tr>${parseCells(row).map(c => `<td>${inlineFormat(c)}</td>`).join('')}</tr>`
    ).join('');
    return `<table><thead><tr>${ths}</tr></thead><tbody>${trs}</tbody></table>`;
}

// ── Alert block builder ───────────────────────────────────────────────────────

function buildAlert(type: string, alertLines: string[]): string {
    const dataType = (type === 'note' || type === 'tip') ? '' : ` data-type="${type}"`;
    const label = type.charAt(0).toUpperCase() + type.slice(1);
    const paragraphs: string[] = [];
    let cur: string[] = [];
    for (const line of alertLines) {
        if (line === '') { if (cur.length) { paragraphs.push(cur.join(' ')); cur = []; } }
        else cur.push(line);
    }
    if (cur.length) paragraphs.push(cur.join(' '));
    const content = paragraphs.map(p => `<p>${inlineFormat(p)}</p>`).join('');
    return `<blockquote${dataType}><strong>${label}</strong>${content}</blockquote>`;
}

// ── Markdown body → HTML + extracted Lain code ───────────────────────────────
//
// Lain code blocks are NOT rendered inline — they are returned separately as
// `lainCode` (the first one found) so DocViewer can display them in the panel.
// Non-lain blocks (bash, shell, plain) stay inline.

function mdBodyToHtmlAndCode(md: string): { html: string; code: string | null } {
    const lines = md.split('\n');
    let html = '';
    let firstLainCode: string | null = null;
    let i = 0;

    while (i < lines.length) {
        const line = lines[i];
        const trimmed = line.trim();

        // ── Code block ────────────────────────────────────────────────────────
        if (trimmed.startsWith('```')) {
            const lang = trimmed.slice(3).trim().toLowerCase() || 'lain';
            i++;
            const codeLines: string[] = [];
            while (i < lines.length && !lines[i].trim().startsWith('```')) {
                codeLines.push(lines[i]);
                i++;
            }
            i++;

            if (lang === 'lain') {
                // Goes to the code panel — not rendered inline
                if (firstLainCode === null) {
                    firstLainCode = codeLines.join('\n').trim();
                } else {
                    firstLainCode += '\n\n' + codeLines.join('\n').trim();
                }
            } else {
                // bash / shell / plain — rendered inline (terminal-style)
                html += `<pre data-lang="${lang}"><code>${escCode(codeLines.join('\n'))}</code></pre>`;
            }
        }

        // ── Table ─────────────────────────────────────────────────────────────
        else if (trimmed.startsWith('|')) {
            const tableLines: string[] = [];
            while (i < lines.length && lines[i].trim().startsWith('|')) {
                tableLines.push(lines[i]);
                i++;
            }
            html += buildTable(tableLines);
        }

        // ── GitHub-style alert block ──────────────────────────────────────────
        else if (/^> \[!(\w+)\]/.test(trimmed)) {
            const typeMatch = trimmed.match(/^> \[!(\w+)\]/);
            const type = typeMatch ? typeMatch[1].toLowerCase() : 'note';
            const alertLines: string[] = [];
            const restOfLine = trimmed.slice(typeMatch![0].length).trim();
            if (restOfLine) alertLines.push(restOfLine);
            i++;
            while (i < lines.length && lines[i].trim().startsWith('>')) {
                alertLines.push(lines[i].trim().slice(1).trim());
                i++;
            }
            html += buildAlert(type, alertLines);
        }

        // ── Regular blockquote ────────────────────────────────────────────────
        else if (trimmed.startsWith('>')) {
            const quoteLines: string[] = [];
            while (i < lines.length && lines[i].trim().startsWith('>')) {
                quoteLines.push(lines[i].trim().slice(1).trim());
                i++;
            }
            html += `<blockquote><p>${inlineFormat(quoteLines.join(' '))}</p></blockquote>`;
        }

        // ── Unordered list ────────────────────────────────────────────────────
        else if (trimmed.startsWith('- ') || trimmed.startsWith('* ')) {
            let items = '';
            while (i < lines.length &&
                (lines[i].trim().startsWith('- ') || lines[i].trim().startsWith('* '))) {
                items += `<li>${inlineFormat(lines[i].trim().slice(2))}</li>`;
                i++;
            }
            html += `<ul>${items}</ul>`;
        }

        // ── Ordered list ──────────────────────────────────────────────────────
        else if (/^\d+\.\s/.test(trimmed)) {
            let items = '';
            while (i < lines.length && /^\s*\d+\.\s/.test(lines[i])) {
                items += `<li>${inlineFormat(lines[i].trim().replace(/^\d+\.\s+/, ''))}</li>`;
                i++;
            }
            html += `<ol>${items}</ol>`;
        }

        // ── Horizontal rule (decorative, skip) ───────────────────────────────
        else if (trimmed === '---' || trimmed === '***' || trimmed === '___') {
            i++;
        }

        // ── h4 inside section body ────────────────────────────────────────────
        else if (trimmed.startsWith('#### ')) {
            html += `<h4>${inlineFormat(trimmed.slice(5))}</h4>`;
            i++;
        }

        // ── Empty line ────────────────────────────────────────────────────────
        else if (!trimmed) {
            i++;
        }

        // ── Paragraph ─────────────────────────────────────────────────────────
        else {
            const paraLines: string[] = [trimmed];
            i++;
            while (
                i < lines.length &&
                lines[i].trim() &&
                !lines[i].trim().startsWith('#') &&
                !lines[i].trim().startsWith('|') &&
                !lines[i].trim().startsWith('```') &&
                !lines[i].trim().startsWith('>') &&
                !lines[i].trim().startsWith('- ') &&
                !lines[i].trim().startsWith('* ') &&
                !/^\s*\d+\.\s/.test(lines[i]) &&
                lines[i].trim() !== '---'
            ) {
                paraLines.push(lines[i].trim());
                i++;
            }
            html += `<p>${inlineFormat(paraLines.join(' '))}</p>`;
        }
    }

    return { html, code: firstLainCode };
}

// ── title → DOM id ────────────────────────────────────────────────────────────

function titleToId(title: string): string {
    return title
        .toLowerCase()
        .replace(/[^a-z0-9]+/g, '-')
        .replace(/-+/g, '-')
        .replace(/^-|-$/g, '');
}

// ── README → DocSection[] ─────────────────────────────────────────────────────

export function parseReadme(): DocSection[] {
    const readmePath = path.join(process.cwd(), '..', 'README.md');
    const raw = fs.readFileSync(readmePath, 'utf-8');
    const lines = raw.split('\n');

    const sections: DocSection[] = [];

    let currentId = 'intro';
    let currentTitle = 'Introduction';
    let currentLevel: 1 | 2 | 3 = 1;
    let currentBodyLines: string[] = [];
    let inCodeBlock = false;

    function flush() {
        const { html, code } = mdBodyToHtmlAndCode(currentBodyLines.join('\n'));
        sections.push({
            id: currentId,
            title: currentTitle,
            level: currentLevel,
            content: html.trim() || undefined,
            code: code ?? undefined,
        });
        currentBodyLines = [];
    }

    // Skip HTML logo block at the top
    let i = 0;
    while (i < lines.length) {
        const t = lines[i].trim();
        if (t === '' || t.startsWith('<')) { i++; } else { break; }
    }

    for (; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();

        if (trimmed.startsWith('```')) inCodeBlock = !inCodeBlock;

        if (!inCodeBlock) {
            const m3 = trimmed.match(/^###\s+(.+)/);
            const m2 = trimmed.match(/^##\s+(.+)/);

            if (m3) {
                flush();
                currentTitle = m3[1].trim();
                currentId = titleToId(currentTitle);
                currentLevel = 3;
                continue;
            }
            if (m2) {
                flush();
                const title = m2[1].trim();
                currentTitle = title;
                currentId = titleToId(title);
                currentLevel = title === 'Language Reference' ? 1 : 2;
                continue;
            }
        }

        currentBodyLines.push(line);
    }

    flush();
    return sections;
}
