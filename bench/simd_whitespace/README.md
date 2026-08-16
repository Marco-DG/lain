# SIMD whitespace-count benchmark

The P1 milestone for the *proof-licensed performance* doctrine
(`internal/design/proof_licensed_performance.md`): a SIMD kernel written **in
Lain** that counts space bytes 32-at-a-time, measured against a scalar C loop.

```
bash bench/simd_whitespace/run.sh
```

## Kernel (`wsbench.ln`)

```lain
proc count_ws(data *u8, n usize) u64 {
    var total u64 = 0
    var i usize = 0
    while i +% 32 <= n {
        var bits u32 = @movemask(@load(u8x32, data, i) == 32)
        total = total +% (@popcount(bits) as u64)
        i = i +% 32
    }
    return total
}
```

No `unsafe`, no bounds checks emitted. Lowers to `vmovdqu` + `vpcmpeqb` +
`vpmovmskb` + `popcnt`.

## Results (256 MB buffer, ~14% spaces, 8 iterations)

| Compiler (`-O2 -march=native`) | Lain SIMD | C scalar loop | Speedup |
|:-------------------------------|----------:|--------------:|--------:|
| gcc                            | 13.81 GB/s | 4.22 GB/s    | 3.27×   |
| clang                          | 13.68 GB/s | 6.00 GB/s    | 2.28×   |

The scalar loop is auto-vectorized by both compilers at `-march=native`, so this
is SIMD-vs-auto-vectorized-scalar. The Lain kernel produces the *verified-equal*
count and wins 2.3–3.3×. A C programmer could hand-write the same intrinsics to
match — but with no safety analysis; the Lain version keeps its proofs (lane
ranges today, the padded-buffer load-safety proof once P2 lands).

*Numbers are machine-dependent; re-run `run.sh` on your hardware.*
