<p align="center">
  <img src="assets/logo.png" alt="Lain" width="600">
</p>

Lain is a systems language in which memory safety, resource safety, integer safety and
termination are **proven at compile time** and therefore **not checked at runtime**. A program
that could read out of bounds, overflow, divide by zero, use a moved value or leak a resource
does not compile. One that does compile carries none of the machinery that would detect those
things while it runs.

It emits C99. The generated code is what an expert would write by hand — except that every
annotation in it is a theorem rather than a promise.

```
your.ln ──▶ [ lain ] ──▶ out.c ──▶ [ gcc / clang ] ──▶ executable
                │
                └── ownership · borrows · bounds · overflow · division · termination · effects
                    all discharged here, none of them emitted
```

---

## The guarantees, against C, C++ and Rust

| | C | C++ | Rust | **Lain** |
|:---|:---|:---|:---|:---|
| Out-of-bounds read/write | UB | UB | Panics at runtime | **Rejected at compile time** |
| Integer overflow | UB (signed) | UB (signed) | Panics in debug, wraps in release | **Rejected at compile time** † |
| Division by zero | UB | UB | Panics | **Rejected at compile time** |
| Use after free / move | UB | UB | Prevented | **Prevented** |
| Double free | UB | UB | Prevented | **Prevented** |
| Resource leak | Silent | Silent | **Allowed** (`mem::forget` is safe) | **Rejected** |
| Dangling reference | UB | UB | Prevented | **Prevented** |
| Uninitialised read | UB | UB | Prevented | **Prevented** |
| Non-termination | Allowed | Allowed | Allowed | **Rejected in `func`** |
| Array length in the type | No | No | Const generics only | **`a i32[out.len]`, `a i32[h*w]`** |
| Aliasing told to the optimiser | manual `restrict`, UB if wrong | same | `noalias` from `&mut` | **Derived from the borrow proof** |
| Sum-type layout | manual | manual | Niche packing, silent | **Niche packing, and it warns when it fails** |
| Data races | UB | UB | Prevented | Single-threaded before 1.0 |
| Output | — | — | Native | **Portable C99** |

Rust has a concurrency story and an ecosystem. Everything else in that column is a design
difference, not a maturity gap.

† One class escapes the shipping binary today — the loop-carried accumulator. It is stated in
full, with the program that miscomputes, under [what Lain does not do](#what-lain-does-not-do).

---

# Inside a proof

Take the smallest interesting program — a byte search over a slice of unknown length.

```lain
func find(haystack u8[], target u8) usize {
    var i usize = 0
    while i < haystack.len {
        if haystack[i] == target { return i }
        i = i + 1
    }
    return haystack.len
}
```

**Step 1 — lower to a typed SSA/CFG IR.** Every obligation gets a home: `elem_ptr` is where a
bounds proof is owed, `add` is where an overflow proof is owed, the back edge to a loop header
is where a termination measure is owed.

```
func find(%0: []u8, %1: u8) -> u64 {
bb0:
  %2 = alloca  : *var u64          ; i
  %3 = const 0 : i32
  store %2, %3
  br bb1
bb1:   ; loop header               ; ← termination obligation
  %4 = load %2 : u64
  %5 = slice_len %0 : u64
  %6 = icmp.ult %4, %5 : bool
  br_cond %6, bb2, bb3
bb2:
  %7 = load %2 : u64
  %8 = slice_data %0 : *u8
  %9 = elem_ptr %8, %7 : *u8       ; ← bounds obligation
  %10 = load %9 : u8
  ...
bb5:
  %15 = add %13, %14 : u64         ; ← overflow obligation
  store %2, %15
  br bb1                           ; ← back edge
}
```

**Step 2 — run an abstract interpreter to fixpoint over that graph.** The domain is
*relational*: it holds not only each value's interval but the differences between values. This
is the state it reaches at the access, printed by the compiler itself (`--dump-octagon`):

```
── octagon state: find ──
  bb2
      %2 ∈ [0, +inf]                                   ; i
      %4 ∈ [0, +inf]
      %5 ∈ [1, +inf]                                   ; haystack.len
      …
      %2 − %5 ≤ -1                                     ; the guard, carried in
      %4 − %5 ≤ -1
      …
    · %7 ∈ [0, +inf]   %7−%2≤0   %7−%4≤0   %7−%5≤-1
                                                       ; ↑ index − length ≤ −1
```

**`%7 − %5 ≤ -1`** is the whole proof: the index is strictly below the length. Not "i is in
[0, 255]" — a *relation* between two runtime values, neither of which is known.

**Step 3 — discharge, and report.**

```
  find      index bounds   @ 4:13  PROVEN check-free
  find      arith overflow @ 5:4  PROVEN check-free
  find      termination    @ 0:0  PROVEN check-free
3/3 proof obligations discharged check-free
```

**Step 4 — emit C, carrying the facts the proof established.**

```c
__attribute__((pure)) __attribute__((nonnull))
size_t bytes_find(size_t __len_haystack, const uint8_t * restrict haystack, uint8_t target) {
    size_t i = 0;
    while (i < __len_haystack) {
        if (haystack[i] == target) {
            return i;
        }
        i = i + 1;
    }
    return __len_haystack;
}
```

`pure`, `nonnull`, `restrict` and the length parameter are all consequences of proofs that were
just discharged — not of anything written in the source.

Change `<` to `<=` and the relation becomes `%7 − %5 ≤ 0`, the obligation fails, and there is no
program:

```
[E085] bounds error: cannot prove index is within bounds for dynamic-length array
  --> bytes.ln:4:13
   |
 4 |         if haystack[i] == target { return i }
   |             ^
       index `i`: range [0, MAX]
       array `haystack`: length [unknown]
       hint: use `for i in 0..arr.len`, a fixed-length type `[N]`, or a `p in arr` guard
```

---

# The five mechanisms

They are not independent. The borrow checker's exclusivity is what lets the numeric domain keep
a fact across a call; the effect row is what makes an annotation on the C output safe to emit.

| Mechanism | Discharges | Rejects |
|:---|:---|:---|
| **Linear types** | Every owned value is consumed exactly once | use-after-move, double free, leak |
| **Regions & borrows** | No reference outlives what it points into; no aliasing violation | dangling reference, `&mut` aliasing |
| **Value range analysis** | Every index, every arithmetic operation, every divisor | out-of-bounds, overflow, division by zero |
| **Termination measures** | Every `func` halts | non-terminating "pure" code |
| **Effect rows** | What a function may do, transitively | understated `effects` declarations |

## Linear types

An owned value has **exactly one** consumer — not two, and not zero.

```lain
type Buffer {
    mov data *u8
    len usize
}
func release(mov {data, len} Buffer) { }

proc main() i32 {
    var p *u8 = 0
    var b = Buffer(p, 0)
    release(mov b)
    release(mov b)          // E002: 'b' was already used/consumed
    return 0
}
```

```lain
type Buffer {
    mov data *u8
    len usize
}
func release(mov {data, len} Buffer) { }

proc main() i32 {
    var p *u8 = 0
    var b = Buffer(p, 0)    // E003: linear field 'data' of 'b' was not consumed
    return 0
}
```

Linearity is **per field**, so a struct is taken apart piecewise and the obligation follows each
piece; **flow-sensitive**, so consuming on one branch and not the other is caught; and `defer`
participates, so a deferred release counts as the consumer at every exit — including the exit
taken by an error propagating out.

**Rust allows the second program.** `mem::forget` is safe, `Rc` cycles leak, and leaking sits
explicitly outside Rust's guarantees.

## Regions and borrows

One mutable borrow, or many shared ones, never both — with **non-lexical lifetimes**, so a
borrow ends at its last use. Two consequences beyond the safety property itself:

**Lifetimes are inferred, not annotated.** The compiler lowers the whole module, so which
parameter a returned reference borrows is read out of the *body*:

```lain
type Data { x i32 }

func pick_x(var a Data, var b Data) var i32 { return var a.x }

proc main() i32 {
    var p = Data(1)
    var q = Data(2)
    var r = pick_x(var p, var q)     // r borrows p — inferred, not written
    r = 7
    return q.x                       // q is still free
}
```

Rust rejects that signature outright, because it checks each function against its signature
alone:

```
error[E0106]: missing lifetime specifier
2 | fn pick_x(a: &mut Data, b: &mut Data) -> &mut i32 { &mut a.x }
  |              ---------     ---------     ^ expected named lifetime parameter
  = help: this function's return type contains a borrowed value, but the signature
          does not say whether it is borrowed from `a` or `b`
help: consider introducing a named lifetime parameter
2 | fn pick_x<'a>(a: &'a mut Data, b: &'a mut Data) -> &'a mut i32 { &mut a.x }
```

The suggested fix is also *weaker* than the truth: it ties `a` and `b` to one lifetime, so `b`
stays frozen for as long as the result lives. Lain reads the body, sees that the result borrows
`a` only, and leaves `b` free.

**Exclusivity is `restrict`.** A mutable borrow is exclusive by proof, which is exactly what C's
`restrict` asserts by fiat. So it is emitted — see [what the proofs buy](#what-the-proofs-buy),
where it is worth an entire vectorised loop.

## Value range analysis

A relational abstract interpreter over the CFG. The relational part is what reaches real code,
because the fact that makes an access safe is almost never a constant. All of these compile
check-free:

| the access | proven by |
|:---|:---|
| scan `a[i]` under `i < a.len` | `i − len ≤ −1`, straight from the guard |
| two-pointer `a[i]` under `i ≤ j < a.len` | transitive closure: `i − j ≤ 0` and `j − len ≤ −1` |
| reverse `a[a.len-i-1]` | `len − i ≥ 1`, from `i < len` |
| masked gather `a[x & (N-1)]`, **any** `x` | `x & (N−1) ∈ [0, N−1]` for every `x`, no assumption on the data |
| sliding window `a[i+1]` over `a i32[n]` under `i < n-1` | the guard as `i − n ≤ −2`, so `(i+1) − n ≤ −1` |
| insertion sort `a[j-1]` under `j > 0 ∧ j < a.len` | offset by −1; the lower bound comes from `j > 0` |
| 2D `a[i*w + j]` over `i32[h*w]` | per dimension: `i < h ∧ j < w` |
| binary search `a[lo + (hi-lo)/2]` over `a i32[8]` | an inductive loop invariant recovers `0 ≤ lo < hi ≤ len`, which the loop's blanket widening had thrown away; then `mid < hi` |

Every row is a program in the corpus, and acceptance *is* the proof — an index Lain cannot place
in bounds is an `E085`, not a runtime check. The binary search row is the honest edge: it proves
over a fixed-length array, and the same search over a runtime length is still rejected.

**Lengths can be part of the type**, because the analysis reasons about lengths:

```lain
proc vadd(var out i32[], a i32[out.len], b i32[out.len]) {
    for i in 0..out.len {
        out[i] = a[i] + b[i]
    }
}
```

`a` and `b` are arrays *of the same length as `out`* — discharged at every call site (`E087`),
free inside the body. Length expressions are arbitrary, so a matrix is a flat buffer with a
shape, and `a[i*w + j]` is check-free with `h` and `w` unknown at compile time:

```lain
proc msum(a i32[h * w], h usize, w usize) i32 {
    var s i32 = 0
    var i usize = 0
    while i < h {
        var j usize = 0
        while j < w {
            s = s +% a[i * w + j]
            j = j + 1
        }
        i = i + 1
    }
    return s
}
```

C cannot state that precondition. Rust's const generics cannot take `h * w`.

**Accesses carry a width**, so a 16-byte SIMD load owes `i + 16 ≤ len` rather than `i < len` —
and the same numeric domain discharges it, with no SIMD-specific reasoning anywhere in the
prover:

```lain
proc scan(src u8[4096], n u32 < 4097, start u32 < 4097, term u8) u32 {
    var i u32 = start
    while (i + 15) in src and i +% 16 <= n {
        var hit u32 = @movemask(@load(u8x16, src, i) == term) & (65535 as u32)
        if hit != 0 {
            i = i +% (@ctz(hit) as u32)
            break
        }
        i = i +% 16
    }
    while i in src and i < n and src[i] != term { i = i +% 1 }
    return i
}
```

No `unsafe`. No runtime bounds check.

**Arithmetic is the same discipline.** `+` widens — the sum of two `i32` cannot overflow — so
the obligation attaches to the narrowing:

```lain
func widened(a i32, b i32) i64 { return a + b }     // accepted
```

```lain
func narrowed(a i32, b i32) i32 {
    var s i32 = a + b       // E086: the sum may not fit an i32
    return s
}
```

`+%` wraps, `+|` saturates, `a +? b else 0` recovers, `unsafe { }` waives. The one thing you
cannot express is *"I did not think about it."*

Divisors work the same way, and the diagnostic names every way out:

```
[E015] Error: division/modulo by a divisor whose range [-2147483648, 2147483647]
       includes zero. Guard it (`if d != 0`), constrain it (`d int != 0`), or wrap
       the divide in an `unsafe` block.
```

Both remedies are real signatures: `func divide(a i32, b i32 != 0) i32` carries the constraint in
the type and discharges it at each call site; a dominating `if b != 0` discharges it locally.

## Termination measures

A `func` is **total**. Every loop and recursion needs a measure that provably decreases toward a
bound — and the measure is **inferred**: `while i < n` gives `n − i`, `while i > 0` gives `i`,
`while lo < hi` gives `hi − lo`.

The step need not be constant, because "this decreases" is a fact the numeric domain already
derives. Halving and shifting need nothing written at all:

```lain
func bits(x u32) u32 {
    var n u32 = x
    var c u32 = 0
    while n > 0 {           // measure `n` inferred; n/2 < n for n > 0
        n = n / 2
        c = c + 1
    }
    return c
}
```

Euclid's algorithm needs the measure named, but not proved — `b = a % b` lands in `[0, b-1]`, so
the divisor is its own measure:

```lain
func gcd(a0 usize, b0 usize) usize {
    var a usize = a0
    var b usize = b0
    while b > 0 decreasing b {
        var t usize = b
        b = a % b
        a = t
    }
    return a
}
```

```lain
func factorial(n int) int {
    if n <= 1 { return 1 }
    return n *% factorial(n - 1)      // accepted: n shrinks toward the base case
}
```

```lain
func collatz(n int) int {
    if n <= 1 { return 0 }
    if n % 2 == 0 { return collatz(n / 2) }
    return collatz(3 * n + 1)         // E011: no decreasing measure
}
```

A `proc` may loop forever. That is the difference between the two.

## Effect rows

Each function's effects — `{Write, Diverge, Raises, IO, Alloc}` — are computed transitively over
the call graph. Writing `effects …` declares an upper bound, and the compiler **checks** it
rather than believing it:

```
[E130] Error: 'talk' declares `effects` that do not cover what its body does —
       it also has: io.
```

The row is not decoration: it is what makes `pure` and `const` safe to put on the C output. An
empty row includes *no panic*, and annotating a function that can abort would license the
optimiser to delete a call that had to happen.

## Layout: zero cost, and it says when it isn't

```lain
type OptionByte {
    Some { v *u8 }
    None
}
```

```c
typedef const uint8_t * niche_OptionByte;

static inline niche_OptionByte niche_OptionByte_Some(const uint8_t * v) {
    return v;
}
static inline niche_OptionByte niche_OptionByte_None(void) {
    return (const uint8_t *)(uintptr_t)0LL;
}
```

Not a struct with a tag — **the pointer itself**, `None` as the null pattern. The compiler
searches the payload for spare bit patterns and uses them as discriminants.

The same machinery carries errors. `*u8 | NotFound | Denied` is still one pointer wide:

```c
typedef const uint8_t * __U_ptr_u8_NotFound_Denied;

static inline __U_ptr_u8_NotFound_Denied ..._NotFound(void) {
    return (const uint8_t *)(uintptr_t)0LL;
}
static inline __U_ptr_u8_NotFound_Denied ..._Denied(void) {
    return (const uint8_t *)(uintptr_t)8LL;
}
```

Two unmapped low addresses stand in for the two error cases. No discriminant word, no heap, no
unwinding — and for a union whose markers carry no payload, zero-cost is not an optimisation the
compiler attempts but a **requirement it enforces**:

```
[E064] Error: the union `i32 | ...` cannot be zero-cost — 'i32' has no spare
       bit-patterns for its 2 marker(s). Give the value type niche room (a
       refinement like `u8 < 200`, a pointer, or a slice), or use fewer markers.
```

Where a marker does carry a payload, a tag byte is unavoidable and is allowed — and then the
same principle applies to plain enums, where the fallback is reported rather than taken quietly:

```
[W120] Warning: enum 'OptI32' not fully zero-cost.
       Payload provides 0 sentinel slot(s); 1 empty variant(s) require 1.
       Layout falls back to 1 tag byte + payload union.
       To eliminate the tag byte: reduce empty variants, constrain the
       payload type with a refinement, or change payload to a type with
       larger sentinel space (i8: 255, i16: 65535, *T: 8192).
```

A silent layout optimisation is a performance cliff you find with a profiler. This one reports
itself.

---

# What the proofs buy

## 1 · Exclusivity → vectorisation

The same loop, compiled twice: once with the borrow proof handed to gcc as `restrict`, once with
it stripped out. `gcc -O3 -march=x86-64-v3`.

```lain
proc vadd(out var i32[], a i32[out.len], b i32[out.len]) {
    var i usize = 0
    while i < out.len {
        out[i] = a[i] +% b[i]
        i = i + 1
    }
}
```

```asm
; with the proof                          ; proof withheld
vadds_vadd:                               vadds_vadd:
  endbr64                                   endbr64
  test  %rdi,%rdi                           mov   %rdi,%r8
  je    cb                                  mov   %rdx,%rdi
  lea   -0x1(%rdi),%rax                     test  %r8,%r8
  cmp   $0x6,%rax                           je    11e
  jbe   d4                                  lea   -0x1(%r8),%rax
  mov   %rdi,%r8                            cmp   $0x2,%rax
  xor   %eax,%eax                           jbe   100          ─┐ to the scalar copy
  shr   $0x3,%r8                            lea   0x4(%rdx),%r9 │
  shl   $0x5,%r8                            mov   %rsi,%rdx     │ overlap test 1
                                            sub   %r9,%rdx      │
  vmovdqu (%rdx,%rax,1),%ymm1               cmp   $0x18,%rdx    │
  vpaddd  (%rcx,%rax,1),%ymm1,%ymm0         jbe   100          ─┤
  vmovdqu %ymm0,(%rsi,%rax,1)               lea   0x4(%rcx),%r9 │
  add     $0x20,%rax                        mov   %rsi,%rdx     │ overlap test 2
  cmp     %r8,%rax                          sub   %r9,%rdx      │
  jne     <loop>                            cmp   $0x18,%rdx    │
                                            jbe   100          ─┘
                                            ...
                                          100:                  ; the scalar copy
                                            mov   (%rcx,%rax,4),%edx
                                            add   (%rdi,%rax,4),%edx
                                            mov   %edx,(%rsi,%rax,4)
                                            add   $0x1,%rax
```

Both reach an AVX2 loop. Only one reaches it unconditionally: without the proof gcc must test at
runtime whether the three buffers overlap, and must keep a scalar copy of the loop to jump to
when they do. **59 instructions against 81.**

## 2 · Bounds → a proof gcc cannot make

A data-dependent gather. The index is not bounded by the loop — it comes out of memory and is
masked:

```lain
proc kernel(a i32[4096], b i32[4096], idx u32[4096]) i64 {
    var acc i64 = 0
    var i usize = 0
    while i < 4096 {
        var j usize = (idx[i] & 4095) as usize
        acc = acc +% ((a[j] *% b[i]) as i64)
        i = i + 1
    }
    return acc
}
```

VRA proves `idx[i] & 4095 ∈ [0, 4095]` for **any** value of `idx[i]` — no assumption about the
data. So there is no check to emit, and gcc vectorises eight elements at a time. The `vpand`
*is* the bounds proof, executed as one instruction on eight indices:

```asm
  vpand     (%r8,%rdx,1),%ymm2,%ymm0    ; mask 8 indices — the proof
  vmovd     %xmm0,%esi                  ; ┐
  vpextrd   $0x1,%xmm0,%ecx             ; │ gather the 8 elements
  ...                                   ; ┘
  vpmulld   (%rdi,%rdx,1),%ymm0,%ymm0   ; 8 multiplies
  vpaddq    %ymm4,%ymm1,%ymm1           ; widen and accumulate
  cmp       $0x4000,%rdx
  jne       <loop>
```

The identical C under `-fsanitize=undefined,bounds`. Scalar, with pointer-overflow, null and
alignment tests standing in front of every access:

```asm
  mov    %r13,%r12
  add    %rbx,%r12
  jb     c5                    ; pointer overflow
  test   %r12,%r12
  je     1a1                   ; null
  test   $0x3,%r12b
  jne    1a1                   ; alignment
  mov    0x0(%r13,%rbx,1),%eax ; ...and only now the load
  and    $0xfff,%eax
  lea    (%rcx,%rax,4),%r12
  cmp    %rcx,%r12
  jb     182                   ; and the same again for the gather
```

**57 instructions against 111**, and the vectoriser is gone. End to end (`bench/thesis/`, same
kernel both sides, `gcc -O3 -march=native`):

```
  (A) proven safe, check-free (what Lain emits)    :   1468.1 ms
  (B) runtime-checked safety (-fsanitize=undefined):   6290.0 ms
  --> 4.28x to learn at runtime what the compiler already knew
```

This is the case gcc cannot rescue. Where a bounds check is *redundant* — `a[i]` under
`i < n` — gcc removes it on its own, and Lain's advantage is nil. Where it is
**data-dependent**, no optimiser can discharge it, and only a proof about the program's
values will do.

## 3 · An empty effect row → one call, not two

Effects are computed over the whole call graph. An empty row means no write, no I/O, no
allocation and **no panic** — which is exactly `__attribute__((const))`:

```lain
func mix(x i32, y i32) i32 {
    var a i32 = (x *% 2654435761) ^ (y *% 40503)
    a = a ^ (a >> 13)
    return a *% 1274126177
}

func twice(p i32, q i32) i32 { return mix(p, q) +% mix(p, q) }
```

```c
__attribute__((const)) int32_t eff_mix(int32_t, int32_t);
```

Compiled in a separate translation unit, so the annotation is the only thing gcc knows about
`mix`:

```asm
; effect row known                    ; effect row withheld
eff_twice:                            eff_twice:
  endbr64                               endbr64
  sub    $0x8,%rsp                      push   %r12
  call   <eff_mix>                      mov    %esi,%r12d
  add    $0x8,%rsp                      push   %rbp
  add    %eax,%eax   ; ← CSE'd          mov    %edi,%ebp
  ret                                   push   %rbx
                                        call   <eff_mix>
                                        mov    %r12d,%esi
                                        mov    %ebp,%edi
                                        mov    %eax,%ebx
                                        call   <eff_mix>   ; ← twice
                                        add    %ebx,%eax
                                        pop    %rbx
                                        pop    %rbp
                                        pop    %r12
                                        ret
```

One call instead of two, and the callee-saved spills vanish with it — **6 instructions against
13**. Across a translation-unit boundary gcc has nothing but the annotation to go on, and in C
that annotation is a promise. Here it is a consequence of the effect row, which the compiler
checked.

The row includes *panics* for a reason: `const` licenses gcc to delete a call whose result is
unused. Annotating a function that can abort turns a program that aborted at `-O0` into one that
does not at `-O3`. That is a miscompile this project shipped once, and the effect row is what
fixed it.

## Annotations the compiler derives

A sliding-window sum over a runtime length — nine lines of Lain, no annotation written — reaches
the C compiler like this:

```c
__attribute__((access(read_only, 1, 2)))
__attribute__((pure))
__attribute__((nonnull))
int32_t window(const int32_t * restrict a, size_t n);
```

Five facts about the function, none of them stated by the programmer:

| C annotation | Derived from | What it is in C |
|:---|:---|:---|
| `restrict` | The borrow checker refused every program where two reference parameters could name the same object | An assertion. **UB if wrong** |
| `access(read_write, n, m)` | The dependent length — parameter *n* is *m* elements long | An assertion |
| `const T *` | The computed write footprint | Unchecked across the call |
| `pure` / `const` | An empty effect row — no writes, no I/O, no allocation, **no panic** | An assertion. A wrong one lets the optimiser delete a call that had to happen |
| `nonnull` | A borrow is not a nullable pointer | An assertion |

---

## How much of this actually holds

Not a benchmark — the whole test suite:

```
corpus                             658 programs, pass and fail alike
bounds obligations proven          833 / 846  check-free
programs with zero bounds checks   104 / 111
```

Validated at three levels, all executable:

- **The abstract domain**, by brute force against its concretisation — 40 000 randomised trials
  per run confirming closure, join, meet and widening over-approximate.
- **The analyses**, by differential fuzzers that *execute what the compiler claimed to prove*
  under ASan and UBSan. A program proven check-free that then reads out of bounds is a false
  proof and is reported as one. Seventeen fuzzers, each verified to have teeth by reintroducing
  the bug it was written for and confirming it fires.
- **The corpus**, where every `_fail.ln` program names the diagnostic it must produce.

The middle-end is being rebuilt around this relational interpreter — octagons over the typed
SSA/CFG IR shown above. Under `--engine=ir` the new analyses already answer for ownership,
borrows and definite assignment on the real user-facing path, at **402 programs accepted, 0 false
positives, 241 rejections caught**. Both engines are gated against the corpus on every change.

## What Lain does not do

- **No concurrency.** Single-threaded before 1.0 — "no data races" is structural, not an
  achievement. An interrupt-aware model is roadmapped.
- **The loop-carried accumulator is a live hole in the shipping compiler.** Its loop widening
  clamps an accumulator to the type it is stored in, so the overflow check does not survive the
  loop:

  ```lain
  func up8(n u8) u8 {
      var s u8 = 0
      var i u8 = 0
      while i < n {
          s = s + i          // accepted today; 0+1+…+199 = 19900 does not fit a u8
          i = i + 1
      }
      return s
  }
  ```

  `up8(200)` compiles and prints **188**. The rebuilt engine rejects it —
  `--engine=ir-full` gives `[E086] arithmetic is not provably free of overflow` — and closing
  this class on the default path is the reason the middle-end is being replaced. It is the
  largest known gap between what this page claims and what the current binary enforces.

- **Even once fixed, an unbounded accumulator is *rejected*, not checked.** Guard it, widen the
  type, or use `+%`. This is the rule most likely to cost you.
- **`unsafe` exists**, and inside it the numeric and bounds obligations are waived — lexical and
  greppable.
- **Generics are monomorphised and duck-typed.** Errors surface at the instantiation; no trait
  bounds.
- **The proof engine is not machine-checked.** Fuzz-validated and brute-force-validated against
  the domain's concretisation; a mechanised soundness proof is future work, not a claim being
  made today.

---

## Quick start

```bash
gcc -std=c99 -Wall -Wextra -o lain src/main.c -I src     # build the compiler
./lain my_program.ln -o out.c                            # Lain  -> C99
gcc out.c -o my_program -Dlibc_printf=printf -w          # C99   -> executable
```

Run from the repository root when a program imports from `std/` — module paths resolve relative
to the source file's directory.

```bash
bash run_tests.sh        # the corpus
bash readme_gate.sh      # every example on this page, through the compiler
bash spec_gate.sh        # every diagnostic the compiler emits, against the specification
```

Every Lain example above is extracted and compiled by `readme_gate.sh`. A block claiming to be
an error must fail; every other must compile. Documentation nothing checks is how a README comes
to describe a language that no longer exists.
