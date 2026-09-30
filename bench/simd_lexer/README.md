# SIMD lexer core (P4 first cut)

The hybrid lexer of the *proof-licensed performance* doctrine
(`local/internal/design/proof_licensed_performance.md` §6), written **in Lain**:

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
- The deprecated naive `count_tokens` keeps its `in`-guards and one `unsafe` block —
  the "before" form, standing next to the natural one. Its `n` is now bounded like the
  others' (`n u32 < 4097`): unbounded, its SIMD guard `i +% 16 <= n` could wrap and the
  loop never end, which the termination analysis rightly refused.

## Every loop is proven to terminate

A `func` must finish, and this program stopped compiling when that became checked: six
loops had no measure. Three facts, stated in the source, prove them all:

- `scan_to` returns at least its start — `func scan_to(…, start u32 < 4097, …) u32 >= start`,
  proven in its body — so `i = scan_to(src, n, i, 42)` never moves `i` back, and the
  `i = i + 1` after it moves it on (d0745b4 reads that order from the numeric domain);
- the block-comment loop leaves with `break` instead of a `done` flag;
- an identifier or number branch consumes its first character, which it has already
  classified, before looping over the rest.

None of these changes what a lexer does: `run.sh` checks all three against the scalar
reference.

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

Throughput (GB/s, `run.sh`: gcc 13.3 `-O2 -march=native`, AMD Ryzen 5 5500U, median of
three runs, 2026-09-30; vs the scalar reference in `driver.c`):

| input | scalar | naive per-token SIMD | **corrected (conditional SIMD)** |
|:------|-------:|---------------------:|--------------------------------:|
| dense code    | 0.41 | 0.19 (0.46×) | **0.70 (1.73×)** |
| normal code   | 0.86 | 0.58 (0.68×) | **1.01 (1.18×)** |
| string-heavy  | 2.13 | 0.55 (0.26×) | **4.17 (1.98×)** |
| comment-heavy | 1.78 | 0.56 (0.31×) | **4.06 (2.22×)** |

(The previous table said `-O3`; `run.sh` has always built with `-O2`.)

**The measurement journey, complete:**

1. Naive per-token SIMD **always loses** (0.26–0.57×) — a 16-byte
   load+movemask+ctz per token is pure overhead when runs are 1–2 bytes.
2. The **corrected** design (SIMD *only* on genuinely-long runs) **wins about 2× on
   string- and comment-heavy code**, and now beats the scalar reference on dense and
   normal code too (1.7× and 1.2×). This is SIMD used the way it should be: amortized
   over a long body, not paid per token.
3. The dense-code gap an earlier measurement showed (0.87×) is gone. The scalar core is
   the same Lain; the emitted C now carries the proven facts (`n < 4097`) as hints
   (18292b8), and the loops are the restructured ones above.

So the doctrine's "measure, don't assume" did its job **twice**: it rejected the
naive SIMD lexer, then confirmed the conditional-SIMD one — with the exact
JSON-vs-code distinction the design predicted.

**Next:** `@shuffle` classification (the builtin exists since 0a6e832: a 16-entry
nibble table, `@shuffle(tbl, (x >> 4) & 15)` is `psrlw; pand; pshufb`); and P2b's padded
`Source` so the SIMD long-run loads are provably safe with a branchless tail.
