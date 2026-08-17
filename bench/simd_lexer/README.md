# SIMD lexer core (P4 first cut)

The hybrid lexer of the *proof-licensed performance* doctrine
(`internal/design/proof_licensed_performance.md` §6), written **in Lain**:

- **SIMD** skips the long whitespace runs — `@load` 16 bytes → detect ws by
  elementwise compares → `@movemask` → `@ctz` to the first non-whitespace byte,
  jumping whole all-whitespace 16-byte blocks in a few instructions.
- **Scalar** scans the short, dense token bodies (identifiers / numbers /
  single-char operators).

```
bash bench/simd_lexer/run.sh
```

## Safety split (exactly per the doctrine) — and the safe code is the *natural* code

- The scalar reads `src[i]` need **no `in`-guard**: the loop/branch bound `i < n`
  — chained with the content refinement `n < 4097` on the `u8[4096]` buffer —
  *proves* them. Lookahead reads are natural too: `... and j < n and src[j]`
  proves `src[j]` because `&&` short-circuits (the compiler applies the `j < n`
  conjunct before the read). VRA emits no bounds checks. The corrected lexer has
  **zero `in`-guards on plain reads** — it reads exactly like ordinary C, and is
  proven safe.
- The SIMD scan loads (`scan_to`) are proven by a last-byte in-guard
  `(i + 15) in src` — **zero `unsafe`, zero runtime checks**. (A fully branchless
  padded-buffer tail — `while i < n { @load }` with `n < CAP-15` — now also proves
  via constraint-chaining; `scan_to` keeps the last-byte guard because it scans an
  unpadded buffer to `n`.)
- The deprecated naive `count_tokens` keeps its `in`-guards and one `unsafe` block
  (its `n` is unbounded) — the "before" form, standing next to the natural one.

## Status: two lexers, and a measurement that found the right architecture

Both are verified correct against scalar references (`run.sh` → `ALL OK`):

- `count_tokens` — **naive**: SIMD whitespace-skip on *every* token.
- `count_tokens_smart` — **corrected**: a tight scalar core, with SIMD invoked
  **only on the long runs** — **string bodies, `//` line comments, and `/* … */`
  block comments (incl. multi-line)** — via `scan_to` = `@movemask`+`@ctz` to the
  terminator. A real language subset; SIMD on every long run.
- `tokenize` — the real thing: **SoA output**, `kinds[]` (1 B/token) + `starts[]`
  (4 B/token), **no length stored** (Zig-style — length is the gap to the next start,
  or a cheap re-lex). 5 B/token vs 8 for `{kind,pos,len}` AoS, and the parser streams
  `kinds[]` at 1 B/token — cache-optimal. The borrow checker proves `kinds`/`starts`/
  `src` don't alias, so all three get `restrict`. **Keyword recognition** (`kw_kind`):
  identifiers are matched against `func`/`proc`/`if`/`else`/`for`/`var`/`while`/`type`/
  `return` and tagged `KIND_KEYWORD` — scalar (short tokens), reads proven by a `start`
  refinement. `run.sh` prints a live token stream distinguishing keywords from idents.

Throughput (GB/s, `-O3 -march=native`, vs a scalar reference; small buffers, so
dense/normal are ~parity within noise — the signal is the long-run rows):

| input | scalar | naive per-token SIMD | **corrected (conditional SIMD)** |
|:------|-------:|---------------------:|--------------------------------:|
| dense code    | 0.52 | 0.21 (0.40×) | **0.60 (1.14×)** |
| normal code   | 1.04 | 0.70 (0.67×) | **1.15 (1.10×)** |
| string-heavy  | 2.68 | 0.71 (0.27×) | **4.57 (1.70×)** |
| comment-heavy | 1.50 | 0.71 (0.48×) | **3.68 (2.44×)** |

**The measurement journey, complete:**

1. Naive per-token SIMD **always loses** (0.26–0.57×) — a 16-byte
   load+movemask+ctz per token is pure overhead when runs are 1–2 bytes.
2. The **corrected** design (SIMD *only* on genuinely-long runs) **wins 1.65× on
   string-heavy code** and roughly matches scalar elsewhere. This is SIMD used the
   way it should be: amortized over a long body, not paid per token.
3. The residual gap on dense code (0.87×) is **Lain scalar-core tightness**, not
   architecture — the classifier `func`s and `in`-guarded loop cost a little versus
   the hand-inlined C reference. Inlining the classifiers / a jump-table core closes
   it; that's an optimization, not a redesign.

So the doctrine's "measure, don't assume" did its job **twice**: it rejected the
naive SIMD lexer, then confirmed the conditional-SIMD one — with the exact
JSON-vs-code distinction the design predicted.

**Next:** SoA output (`kinds[]`+`starts[]`, no length stored); `@shuffle`
classification; block/line comments (more long runs for SIMD); and P2b's padded
`Source` so the SIMD long-run loads are provably safe with a branchless tail.
