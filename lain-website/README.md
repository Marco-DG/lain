# lain-website

The public site for the Lain language. Next.js 14 (App Router), React 18, TypeScript, CSS
Modules. No database, no API, no client state — every page is statically prerendered at build
time.

```bash
npm install
npm run dev     # http://localhost:3000
npm run build   # static prerender of all routes
```

## The one rule that matters here

**Pages that make claims about the language read the real document out of the repository at build
time. They do not paraphrase it.**

| route | renders | source |
|:--|:--|:--|
| `/docs` | the project README | `../README.md`, via `parseReadme()` |
| `/overview` | the language reference manual | `../LANGUAGE.md`, via `parseLanguage()` |
| `/` | a short teaser, hand-written | — |
| `/blog/*` | articles | hand-written |

Both parsers are `app/docs/readmeParser.ts`, which takes a filename and returns `DocSection[]`.

This is not a stylistic preference, it is the fix for a specific failure. Until 2026-10-01
`/overview` rendered `overviewData.ts`, 33 hand-written chapters paraphrasing the manual. Of its
35 Lain code samples, **one still compiled.** It taught the `proc` keyword removed from the
language in September, built its central narrative on a `func`/`proc` split that no longer
exists, and led with a division example the compiler rejects. Over the same five months `/docs`
had zero false claims — because it was already reading the README instead of restating it.

A page that paraphrases the manual is a second copy of the manual, and the second copy is always
the one nobody updates.

Because `README.md` and `LANGUAGE.md` are both checked by `scripts/gates/readme_gate.sh`, which
compiles every ```lain block in them, the two documentation routes cannot contradict the compiler
without failing that gate first.

## If you add a Lain sample anywhere

Compile it. `./lain yourfile.ln -o /dev/null` from the repository root, against a compiler built
from current `src/` — the gates now refuse to run against a stale binary for exactly this reason.
A sample that has not been compiled is a claim nobody has tested, and every false claim found on
this site was in that category.

Samples hand-tokenised into JSX (`app/page.tsx`, and the unused components below) are **not**
reachable by any gate. Verify those by hand and keep them short.

## Unused components

`Hero`, `CodeShowcase`, `FeatureGrid`, `TerminalWindow`, `Navbar` and `Footer` are imported by
nothing. `ROADMAP.md` phases 1–6 intended to wire them up and that never happened. Their Lain
samples were corrected on 2026-10-01 so that wiring one up does not reintroduce a false claim,
but nothing checks them — treat them as drafts.

## Deployment

**Undetermined.** There is no CI configuration anywhere in the repository, no `vercel.json`, no
`netlify.toml`, no Dockerfile and no deploy script. `next.config.mjs` is empty, so there is no
`output: 'export'` and the site needs a Node host rather than static file hosting. How (or
whether) this is currently published is not recorded anywhere, and should be before anything is
shipped.
