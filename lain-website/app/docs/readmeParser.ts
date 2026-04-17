import fs from 'fs';
import path from 'path';
import type { DocSection } from '@/components/DocViewer';

// ── HTML escape ──────────────────────────────────────────────────────────────

function escHtml(s: string): string {
    return s
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// For code blocks: only escape &, <, > — not quotes (avoids breaking string literals)
function escCode(s: string): string {
    return s
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;');
}

// ── Lain syntax highlighting ──────────────────────────────────────────────────
// Operates on already-escCode'd text; adds <span class="hl-*"> wrappers.

function highlightLain(code: string): string {
    return code.replace(
        // Order matters: strings and comments first (to prevent keyword matches inside them)
        /("(?:[^"\\]|\\.)*")|(\/\/[^\n]*)|\b(func|proc|fun|var|mov|return|type|if|elif|else|while|for|case|extern|comptime|undefined|as|import|c_include|defer|unsafe|and|or|break|continue|in|true|false|decreasing)\b|\b(int|i8|i16|i32|i64|u8|u16|u32|u64|isize|usize|f32|f64|bool|void)\b|(?<!\w)(\d+)(?!\w)/g,
        (match, str, com, kw, type, num) => {
            if (str) return `<span class="hl-str">${match}</span>`;
            if (com) return `<span class="hl-com">${match}</span>`;
            if (kw)  return `<span class="hl-kw">${match}</span>`;
            if (type) return `<span class="hl-type">${match}</span>`;
            if (num)  return `<span class="hl-num">${match}</span>`;
            return match;
        }
    );
}

// ── Inline markdown → HTML ───────────────────────────────────────────────────

function inlineFormat(text: string): string {
    // 1. Extract inline code spans first (prevent double-processing)
    const fragments: string[] = [];
    text = text.replace(/`([^`]+)`/g, (_, code) => {
        const idx = fragments.length;
        fragments.push(`<code>${escHtml(code)}</code>`);
        return `\x00${idx}\x00`;
    });

    // 2. HTML-escape plain text
    text = escHtml(text);

    // 3. Bold
    text = text.replace(/\*\*(.+?)\*\*/g, '<strong>$1</strong>');

    // 4. Italic (avoid matching inside bold remnants)
    text = text.replace(/(?<!\*)\*([^*\x00]+)\*(?!\*)/g, '<em>$1</em>');

    // 5. Restore inline code fragments
    text = text.replace(/\x00(\d+)\x00/g, (_, i) => fragments[parseInt(i)]);

    return text;
}

// ── Table builder ─────────────────────────────────────────────────────────────

function buildTable(tableLines: string[]): string {
    // Filter out separator rows (e.g.  |:------|:-------|)
    const rows = tableLines.filter(l => !/^\s*\|[\s\-:|]+\|\s*$/.test(l));
    if (rows.length === 0) return '';

    const parseCells = (line: string) =>
        line.split('|').slice(1, -1).map(c => c.trim());

    const [header, ...body] = rows;
    const ths = parseCells(header)
        .map(c => `<th>${inlineFormat(c)}</th>`)
        .join('');
    const trs = body
        .map(row =>
            `<tr>${parseCells(row)
                .map(c => `<td>${inlineFormat(c)}</td>`)
                .join('')}</tr>`)
        .join('');

    return `<table><thead><tr>${ths}</tr></thead><tbody>${trs}</tbody></table>`;
}

// ── Alert block builder ───────────────────────────────────────────────────────

function buildAlert(type: string, alertLines: string[]): string {
    const dataType = (type === 'note' || type === 'tip')
        ? ''
        : ` data-type="${type}"`;
    const label = type.charAt(0).toUpperCase() + type.slice(1);

    // Group lines into paragraphs split by blank (>) lines
    const paragraphs: string[] = [];
    let currentPara: string[] = [];
    for (const line of alertLines) {
        if (line === '') {
            if (currentPara.length > 0) {
                paragraphs.push(currentPara.join(' '));
                currentPara = [];
            }
        } else {
            currentPara.push(line);
        }
    }
    if (currentPara.length > 0) paragraphs.push(currentPara.join(' '));

    const content = paragraphs
        .map(p => `<p>${inlineFormat(p)}</p>`)
        .join('');

    return `<blockquote${dataType}><strong>${label}</strong>${content}</blockquote>`;
}

// ── Markdown body → HTML ──────────────────────────────────────────────────────

function mdBodyToHtml(md: string): string {
    const lines = md.split('\n');
    let html = '';
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
            i++; // skip closing ```
            const escaped = escCode(codeLines.join('\n'));
            const body = lang === 'lain' ? highlightLain(escaped) : escaped;
            html += `<pre data-lang="${lang}"><code>${body}</code></pre>`;
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

            // Content on the same line as [!TYPE]
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
            while (
                i < lines.length &&
                (lines[i].trim().startsWith('- ') || lines[i].trim().startsWith('* '))
            ) {
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

        // ── h4 sub-heading (#### inside a section body) ──────────────────────
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

    return html;
}

// ── Heading id from title ─────────────────────────────────────────────────────

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
        const content = mdBodyToHtml(currentBodyLines.join('\n')).trim();
        sections.push({
            id: currentId,
            title: currentTitle,
            level: currentLevel,
            content: content || undefined,
        });
        currentBodyLines = [];
    }

    // Skip the HTML logo block at the very top (lines starting with < or blank)
    let i = 0;
    while (i < lines.length) {
        const t = lines[i].trim();
        if (t === '' || t.startsWith('<')) {
            i++;
        } else {
            break;
        }
    }

    for (; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();

        // Track code-block open/close so we don't mistake content for headings
        if (trimmed.startsWith('```')) {
            inCodeBlock = !inCodeBlock;
        }

        if (!inCodeBlock) {
            const m3 = trimmed.match(/^###\s+(.+)/);
            const m2 = trimmed.match(/^##\s+(.+)/);

            if (m3) {
                flush();
                currentTitle = m3[1].trim();
                currentId = titleToId(currentTitle);
                currentLevel = 3;
                continue; // don't add heading line to body
            }

            if (m2) {
                flush();
                const title = m2[1].trim();
                currentTitle = title;
                currentId = titleToId(title);
                // "Language Reference" acts as a level-1 chapter divider
                currentLevel = title === 'Language Reference' ? 1 : 2;
                continue;
            }
        }

        currentBodyLines.push(line);
    }

    flush(); // save the last section

    return sections;
}
