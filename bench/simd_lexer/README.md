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

## Status

`count_tokens(src u8[4096], n)` tokenizes a fixed 4096-byte buffer and returns the
token count. Verified correct against a scalar reference tokenizer across empty /
whitespace-only / 32+-whitespace-run / tabs+newlines / full-function inputs
(`run.sh` prints `ALL OK`).

**Next:** SoA output (`kinds[]` + `starts[]`, no length stored), `@shuffle`-based
branchless byte classification, keyword recognition, and — once P2b lands — a
dynamic padded `Source` so the whole scan is provably safe with a branchless tail.
