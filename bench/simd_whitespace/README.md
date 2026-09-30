# SIMD whitespace-count benchmark

The P1 milestone for the *proof-licensed performance* doctrine
(`local/internal/design/proof_licensed_performance.md`): a SIMD kernel written **in
Lain** that counts space bytes 32-at-a-time, measured against a scalar C loop.

```
bash bench/simd_whitespace/run.sh
```

## Kernel (`wsbench.ln`)

```lain
func count_ws(data u8[<= 1073741824]) u64 {
    var total u64 = 0
    var i usize = 0
    while i + 32 <= data.len {
        var bits u32 = @movemask(@load(u8x32, data, i) == 32)
        total = total + (@popcount(bits) as u64)
        i = i + 32
    }
    return total
}
```

No `unsafe`, and no bounds check emitted, because every load is PROVEN: the guard
`i + 32 <= data.len` bounds it, and the stated length bound (1 GiB; the benchmark passes
256 MB) is what keeps `i + 32` itself from overflowing and gives the loop a trip count,
so `total` needs no wrapping either. Lowers to `vmovdqu` + `vpcmpeqb` + `vpmovmskb` +
`popcnt`.

The previous kernel took `data *u8, n usize` and said the same thing, but a raw pointer
has no length, so nothing could have proven its loads: that "no bounds checks" was the
compiler not checking. The compiler now refuses it (E085, "no length is known here").

## Results (256 MB buffer, ~14% spaces, 8 iterations)

| Compiler (`-O2 -march=native`) | Lain SIMD | C scalar loop | Speedup |
|:-------------------------------|----------:|--------------:|--------:|
| gcc 13.3                       | 15.64 GB/s | 4.20 GB/s    | 3.65×   |
| clang 18.1                     | 15.89 GB/s | 6.53 GB/s    | 2.36×   |

AMD Ryzen 5 5500U, median of three `run.sh` runs, 2026-09-30.

The scalar loop is auto-vectorized by both compilers at `-march=native`, so this
is SIMD-vs-auto-vectorized-scalar. The Lain kernel produces the *verified-equal*
count and wins 2.4–3.7×. A C programmer could hand-write the same intrinsics to
match — but with no safety analysis; the Lain version keeps its proofs (every
32-byte load proven inside the slice, and no overflow anywhere in the loop).

*Numbers are machine-dependent; re-run `run.sh` on your hardware.*
