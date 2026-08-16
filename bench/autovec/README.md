# Tier-1: auto-vectorization via proofs

Evidence for the general-programmer SIMD path (`internal/design/simd_strategy.md`
tier 1): an **ordinary loop with no SIMD in the source** vectorizes because Lain's
borrow checker *proves* the pointers don't alias and stamps `restrict`.

```
bash bench/autovec/run.sh
```

## What happens

```lain
proc add_slices(n usize, a i32[n], b i32[n], var out i32[n]) {
    for i in 0..n { out[i] = a[i] +% b[i] }
}
```

emits `const int32_t * restrict a, ... * restrict b, int32_t * restrict out` (+ `access`
size attributes), and the backend reports *"loop vectorized using 32 byte vectors."*

## Result (16384-element i32 add, `-O3 -march=native`)

| | GB/s | note |
|:--|--:|:--|
| **Lain (proven `restrict`)** | **41.83** | safe *and* clean codegen |
| C no-restrict (honest) | 40.08 | gcc *versions*: runtime alias check + scalar fallback |
| C hand-restrict (UB-risk) | 41.50 | fast, but UB if a caller ever aliases |

Lain matches/edges the hand-`restrict` C **and** carries the safety the honest C
lacks: the `restrict` is *proven*, not a gamble. The general programmer writes a
plain loop and gets clean AVX2 with a soundness guarantee.

## The caveat (why the flag matters)

The `restrict` is Lain's contribution and it's always sound. Whether SIMD *actually
happens* is the **backend's** call: clang `-O2` vectorizes this; gcc needs `-O3` (or
`-fvect-cost-model=cheap`) because its `-O2` cost model rejects runtime-trip-count
loops. Per-loop `#pragma ivdep`/`unroll` don't move it. So compile generated Lain C
with a vectorizing cost model — and, long-term, this backend-dependence is the case
for a Lain-IR vectorizer that can *promise* vectorization rather than hope.
