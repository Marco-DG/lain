# Branchless-padded vs guarded wide scan — a measurement that says "don't bother"

The constraint-chaining proof (commit 1100125) lets a wide `@load` run **branchless**
over a padded buffer: `while i < n { @load(u8x16, buf, i); i += 16 }` on a `u8[4112]`
(4096 usable + 16 zero pad), proven check-free because `i < n` chains with `n < 4097`
to bound the load's last byte `buf[i+15] ≤ 4110 < 4112`. One compare per 16-byte block,
**no per-block in-guard, no scalar tail**.

The doctrine (`proof_licensed_performance.md` §5.2) long held this branchless tail up as
"the real payoff" of the padded-buffer invariant. So — per the doctrine's own measurement
contract — we measured it against the **guarded** P2b form (`while (i+15) in buf and
i +% 16 <= n { … }` + a scalar tail), on a whitespace count.

```
bash bench/simd_padded/run.sh
```

## Result: branchless is NOT faster (equal, or slightly slower)

`-O3 -march=native`, best-of-5, 4 KB buffer (L1-resident):

| input length      | guarded | branchless |
|:------------------|--------:|-----------:|
| n=4080 (no tail)  | 26.98 GB/s | 27.13 GB/s (**1.01×**) |
| n=4090 (10-B tail)| 28.30 GB/s | 25.82 GB/s (**0.91×**) |

Both correct (a correctness check in the driver caught — and forced the fix of — a
subtle bug in the *guarded* tail during development).

## Why — and what it means

At ~27 GB/s the scan is **bandwidth-bound** (the 4 KB buffer sits in L1; a real
MB-scale file would be L2/L3/DRAM-bound — *more* bandwidth-limited, not less). The
load latency dominates, so the guarded form's extra loop-condition work and its tiny
scalar tail (≤15 bytes out of thousands) **hide completely**. The branchless form's
one *extra* full SIMD block (it rounds the length *up* to a 16-multiple, over-reading
into the pad) is, if anything, marginally *more* work than the guarded tail — hence
the 0.91× with a partial tail.

**Upshot:** the guarded P2b scan — which needs **no padding** and works on the natural
`u8[4096]` — is already optimal. There is **no performance reason** to pad the buffer,
go branchless, or (for the lexer's sake) build a padded `Source(N)` type. Const-generics
remain worthwhile for API/expressiveness, but this retires "branchless tail throughput"
as a motivation. The doctrine's "measure, don't assume" did its job a third time: after
rejecting naive per-token SIMD and confirming conditional SIMD, it now rejects branchless
padding as a micro-optimization that isn't one.
