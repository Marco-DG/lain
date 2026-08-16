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

## Safety split (exactly per the doctrine)

- The scalar reads `src[i]` are **`in`-guarded**, so VRA *proves* them — no bounds
  checks are emitted.
- The SIMD whitespace loads sit in an **`unsafe`** block — the documented P1
  escape hatch — until **P2b**'s padded `Source` (`readable ≥ len + 64`) makes
  them provably safe too. Each such `unsafe` is a logged debt, not a silent one.

## Status: correct, and a measurement that reshapes the design

`count_tokens(src u8[4096], n)` is verified correct against a scalar reference
across empty / whitespace-only / 32+-run / tabs+newlines / full-function inputs.

But the **throughput measurement is the real result** — and it says the naive
design is wrong:

| input | whitespace | Lain SIMD | scalar C | ratio |
|:------|:-----------|----------:|---------:|------:|
| dense code       |  4% | 0.19 GB/s | 0.62 GB/s | **0.31×** |
| indented code    | 38% | 0.64 GB/s | 1.13 GB/s | **0.56×** |
| heavy whitespace | 90% | 5.13 GB/s | 1.91 GB/s | 2.69× |

**On real code the per-token SIMD skip is *slower* than scalar.** A 16-byte
load + compare + movemask + ctz *per token* is pure overhead when whitespace runs
are 1–2 bytes, which is the common case in code. SIMD only wins once a run exceeds
~16 bytes (heavy indentation, block comments, long strings) — exactly the JSON-vs-code
distinction the design doc called out, now confirmed with numbers.

**The lesson (measure, don't assume):** the lexer core should be a **tight scalar
jump-table** (fast on dense code), with SIMD invoked **conditionally on the
genuinely-long runs** — skip a block comment `/* … */`, a long string, or a ≥16-byte
whitespace/indent block in one `movemask`+`ctz`. That is the correct hybrid; this
first cut applied SIMD unconditionally and paid for it.

**Next:** rebuild on a scalar core + conditional-SIMD long-run skipper; SoA output
(`kinds[]`+`starts[]`, no length stored); `@shuffle` classification; and P2b's padded
`Source` so the SIMD long-run loads are provably safe with a branchless tail.
