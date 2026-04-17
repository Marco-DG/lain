# Lain Website — Roadmap

## Current State Diagnosis

### What works well
- **CRT aesthetic is unique and on-brand**: noise, scanlines, vignette, flicker — perfectly fits "Lain"
- **Color palette**: teal (#0d9488), purple (#8b5cf6), sepia (#c5a68a) on dark — distinctive and cohesive
- **Typography**: Special Elite (logo), VT323 (mono), Crimson Pro (body) — excellent choices
- **NaviShell layout**: the 3-column CRT interface is memorable for docs/spec
- **Spec viewer**: 26 chapters, navigable — solid foundation
- **Component quality**: clean React, modular CSS, good separation

### Critical problem: two orphaned design systems

The site has **6 polished components that are not imported anywhere**:

| Component | Purpose | Used? |
|-----------|---------|-------|
| `Hero.tsx` | Full-screen landing hero with glitch title, CTA buttons, code window | **NO** |
| `CodeShowcase.tsx` | 3 side-by-side feature demos (memory, proofs, determinism) | **NO** |
| `FeatureGrid.tsx` | 5 feature cards (the pillars) | **NO** |
| `TerminalWindow.tsx` | Fake terminal showing `lain build` output | **NO** |
| `Footer.tsx` | "SYSTEM SHUTDOWN" footer | **NO** |
| `Navbar.tsx` | Top navigation bar | **NO** |

These were built for a **scrolling marketing page** but the homepage currently uses NaviShell (3-column grid) instead. This means:
- The landing page has no clear CTA above the fold
- No visual storytelling — just a paragraph and static code
- The 3-column layout wastes space for a first impression
- A visitor can't quickly understand "what is this and why should I care?"

### Content gaps

| What's missing | Impact |
|----------------|--------|
| No tutorial / getting started | New visitors can't learn the language |
| No code examples page | No way to see real Lain code |
| No "Why Lain?" narrative | No emotional hook, no positioning vs. alternatives |
| Docs page is only "Five Pillars" | Not real documentation |
| No comparison section | How does Lain differ from Rust/Zig/C? |
| Code examples are hardcoded JSX | Painful to maintain, not real code |

### Architecture issues
- Landing page constrained by NaviShell 3-column grid
- No shared syntax highlighting component (each component re-implements it)
- `specData.ts` duplicates `specification/*.md` content
- NaviShell 3-column grid breaks on mobile (stacks vertically but loses coherence)
- Navbar and Sidebar both exist but serve different contexts

---

## The Plan

### Phase 1 — Landing Page Revival
**Goal**: Wire up the orphaned components into a compelling scroll-based landing page.

The landing page should NOT use NaviShell. It should be a vertical marketing flow:

```
/  →  Navbar
      Hero          (glitch title, one-liner, CTA: Install / Docs)
      CodeShowcase  (3 feature demos with real Lain code)
      FeatureGrid   (5 pillars as cards)
      TerminalWindow (compiler demo)
      Footer
```

CRT effects (noise, scanlines, vignette) still apply globally via `globals.css`.
NaviShell stays for interior pages (docs, spec, install).

**Tasks**:
1. Rewrite `app/page.tsx` to compose: Navbar → Hero → CodeShowcase → FeatureGrid → TerminalWindow → Footer
2. Update Hero pitch text — less academic, more compelling:
   - Current: "statically-typed, ahead-of-time compiled systems programming language designed for deterministic resource management and formal safety..."
   - Better: Lead with what makes Lain different. "Memory safety without garbage collection. Determinism without compromise. Zero runtime cost." Then explain.
3. Update CodeShowcase examples with **real, compilable Lain code** from the test suite
4. Fix Navbar links to match actual site structure
5. Ensure CRT effects work without NaviShell wrapper (extract to a global `<CRTOverlay />` component)
6. Responsive: Hero stacks vertically on mobile (already partly done in Hero.module.css)

### Phase 2 — Documentation Hub
**Goal**: Transform `/docs` from a single page into a proper documentation section.

```
/docs                → Hub page with cards linking to sub-sections
/docs/getting-started → "Hello, Wired" — first program tutorial
/docs/tour            → Language tour (types, ownership, functions, matching, etc.)
/docs/examples        → Curated real code from tests/
/docs/philosophy      → Current "Five Pillars" content (expanded)
```

All sub-pages use NaviShell layout (sidebar + content + code panel).

**Tasks**:
1. Create `/docs` hub page with 4 cards linking to sub-sections
2. **Getting Started** (`/docs/getting-started`):
   - Install (link to /install)
   - Hello World (`proc main() int { ... }`)
   - Variables and types
   - Your first function
   - Understanding compiler errors
3. **Language Tour** (`/docs/tour`):
   - Organized in chapters, each with explanation + code
   - Topics: Types (primitives, structs, enums, ADTs), Functions (func vs proc),
     Ownership (mov, linear types), Borrowing (& references), Pattern Matching (case),
     Control Flow (if/for/while), Arrays & Slices, Modules, Unsafe, C Interop
4. **Examples** (`/docs/examples`):
   - Curated from `tests/` directory — real code that compiles
   - Categories: Basics, Types, Ownership, Safety, Algorithms
5. **Philosophy** (`/docs/philosophy`):
   - Move current Five Pillars content here
   - Expand with "Why not Rust?", "Why not Zig?", "Design trade-offs"

### Phase 3 — Shared Code Infrastructure
**Goal**: One syntax highlighting system, maintainable code examples.

**Tasks**:
1. Create a `<LainCode>` component that takes a string and applies consistent syntax highlighting
   - Keywords: purple (`func`, `proc`, `var`, `mov`, `type`, `case`, `return`, `for`, `while`, `in`, `if`, `else`, `elif`, `as`, `unsafe`, `defer`, `import`, `extern`, `and`, `or`, `true`, `false`, `decreasing`, `break`, `continue`)
   - Types: sepia (`int`, `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `usize`, `isize`, `float`, `bool`, `*T`, `T[N]`)
   - Strings: muted gray (italic)
   - Comments: dim (`//`)
   - Numbers: teal
   - Error comments: teal highlight
2. Replace all hardcoded JSX code blocks with `<LainCode code={...} />`
3. Optional: load code examples from `.ln` files at build time (Next.js static imports)

### Phase 4 — Spec & Install Polish
**Goal**: Improve existing pages.

**Tasks**:
1. **Spec viewer improvements**:
   - Add table of contents / chapter list sidebar (within the NaviShell center panel)
   - Add URL hash routing (`/spec#ownership`) so chapters are linkable
   - Better code rendering using `<LainCode>` component
   - Consider loading from `specification/*.md` instead of `specData.ts`
2. **Install page improvements**:
   - Add prerequisites section (gcc, make, git)
   - Add platform-specific notes (Linux, macOS, Windows/WSL)
   - Add "Verify installation" step with expected output
   - Right panel: show actual `lain --help` output instead of fake build stages

### Phase 5 — Navigation & Layout
**Goal**: Clear, consistent navigation throughout.

**Tasks**:
1. **Landing page**: Navbar (top bar) with: Logo | Docs | Spec | Install | GitHub
2. **Interior pages**: NaviShell with Sidebar (as-is, it works well)
3. Add breadcrumbs to interior pages (`Docs > Tour > Ownership`)
4. Sidebar: expand nav to include new docs sub-pages
5. Mobile: hamburger menu for Navbar, collapsible Sidebar

### Phase 6 — SEO & Meta
**Goal**: Make the site discoverable and shareable.

**Tasks**:
1. Per-page metadata (title, description, OpenGraph image)
2. Generate a sitemap (`next-sitemap` or manual)
3. Add a favicon (currently uses default Next.js favicon)
4. OpenGraph image: dark background with "lain" in Special Elite + glitch effect
5. Canonical URLs

### Phase 7 — Visual Polish (Optional)
**Goal**: Refinements that elevate the experience.

**Tasks**:
1. Page transition animations (fade-in on route change)
2. Scroll-triggered animations on landing page (elements appear as you scroll)
3. Code typing animation in Hero (typewriter effect for the code example)
4. Animated terminal in TerminalWindow (lines appear sequentially)
5. Dark/light theme? — probably NO, the dark CRT aesthetic IS the brand
6. Custom 404 page in CRT style ("SIGNAL LOST // LAYER NOT FOUND")

---

## Priority Order

```
Phase 1 (Landing Page)     ████████████ HIGH — first impression
Phase 2 (Docs Hub)         ████████████ HIGH — visitor retention
Phase 3 (Code Infra)       ████████░░░░ MEDIUM — enables maintainability
Phase 4 (Spec/Install)     ██████░░░░░░ MEDIUM — polish existing content
Phase 5 (Navigation)       ██████░░░░░░ MEDIUM — usability
Phase 6 (SEO)              ████░░░░░░░░ LOW — once content is solid
Phase 7 (Visual Polish)    ████░░░░░░░░ LOW — nice to have
```

---

## Site Map (Target)

```
/                          Landing page (scroll: Hero → Showcase → Grid → Terminal → Footer)
├── /docs                  Documentation hub (cards)
│   ├── /docs/getting-started   First program tutorial
│   ├── /docs/tour              Language tour (chapters)
│   ├── /docs/examples          Real code examples
│   └── /docs/philosophy        Design philosophy & pillars
├── /spec                  Specification viewer (26 chapters)
└── /install               Installation guide
```
