# Thesis benchmark — proof-licensed performance

Lain's claim is **safety at zero runtime cost**: it proves indices in bounds and
arithmetic non-overflowing *at compile time* (prove-or-reject), so the emitted C is
check-free. A programmer who wants the same guarantees without such a proof pays for
them at **runtime**. This directory measures that gap honestly.

Run: `./run.sh` (needs `gcc`; builds the Lain compiler is not required — it only
*checks* that `kernel.ln` still proves).

## The kernel

`kernel.ln` is a gather-accumulate: `acc += a[idx[i] & (N-1)] * b[i]`. It compiles in
Lain **check-free** — the mask `idx[i] & (N-1)` is proven in `[0, N)` (VRA), so there
is no bounds check on the gather, and the accumulation is proven/`+%` so there is no
overflow check. `bench.c` contains the identical C kernel.

## Result (representative; `gcc -O3 -march=native`, one machine)

```
(A) proven safe, check-free (what Lain emits)     :   ~600 ms
(B) runtime-checked safety (-fsanitize=undefined) :  ~2800 ms
--> runtime-checked safety costs ~4.6x
```

The two binaries compile the **same** kernel; (B) only adds the runtime bounds +
overflow checks that make it safe. Lain discharges exactly those checks at compile
time — **the same safety, at check-free speed.**

## Honest caveats (this is the real thesis, not a cherry-pick)

- **The win is over *runtime-checked* safety, not over *expert* C.** Lain's emitted C
  equals what an expert C programmer writes by hand (`restrict`, no redundant checks);
  the value is that Lain derives those annotations **automatically and soundly** (each
  is a proof, where hand-written C is UB-if-wrong).
- **`restrict` alone is often marginal.** For a simple `saxpy`, gcc recovers most of the
  no-alias vectorization via runtime *versioning*, so the proof-derived `restrict` is
  only ~1.02x there. The large, reliable win is eliminating the runtime *checks* a safe
  language/build would otherwise insert — which is what (A) vs (B) measures.
- **gcc already eliminates *simple* bounds checks** (a plain `for i<n` over a fixed
  array). Lain's advantage is proving the checks gcc *cannot* — data-dependent indices
  like the masked gather here — and doing it for *every* access, soundly, by default.
