<p align="center">
  <img src="assets/logo.png" alt="Lain" width="600">
</p>

Lain is a systems language in which memory safety, resource safety, integer safety and
termination are **proven at compile time** and therefore **not checked at runtime**. A program
that could read out of bounds, overflow, divide by zero, use a moved value or leak a resource
does not compile.

Rejecting programs is only half of it. Every proof is also a fact the backend can use: exclusive
access becomes `restrict`, an empty effect set becomes `const`, a declared length becomes
`access`. And a check shown to be unnecessary is simply never emitted, including checks no
optimiser could have removed on its own.

```
 your.ln ──▶ front end ──▶ Lain-IR ──▶ analyses ──▶ backend ──▶ executable
                          typed SSA    ownership     C99
                           and CFG     borrows       LLVM
                                       bounds
                                       overflow
                                       termination
                                       effects
```

Every analysis runs on the IR. None of them looks at the source text or at the output target,
so the same proofs hold whichever backend emits the code.

Since 2026-09-17 that is also what a plain compile runs. Ownership, linearity, borrows,
definite assignment, bounds, overflow, division and termination are all answered by the IR;
`--engine=legacy` restores the older AST engine for anyone who needs it.

## Guarantees

| | C / C++ | Rust | **Lain** |
|:---|:---|:---|:---|
| Out-of-bounds read/write | UB | Panics at runtime | **Rejected at compile time** |
| Integer overflow | UB (signed) | Panics in debug, wraps in release | **Rejected at compile time** |
| Division by zero | UB | Panics | **Rejected at compile time** |
| Use after free / move | UB | Prevented | **Prevented** |
| Double free | UB | Prevented | **Prevented** |
| Resource leak | Silent | **Allowed** (`mem::forget` is safe) | **Rejected** |
| Dangling reference | UB | Prevented | **Prevented** |
| Uninitialised read | UB | Prevented | **Prevented** |
| Non-termination | Allowed | Allowed | **Rejected in `func`** |
| Data races | UB | Prevented | Single-threaded before 1.0 |
| Array length in the type | No | Const generics only | **`a i32[out.len]`, `a i32[h*w]`** |
| Aliasing told to the optimiser | manual `restrict`, UB if wrong | `noalias` from `&mut` | **Derived from the borrow proof** |
| Sum-type layout | manual | Niche packing, silent | **Niche packing, and it warns when it fails** |

Against the C tooling, five minimal programs, one per error class, put through each tool in
turn. Every one of them is undefined behaviour or a real leak in C, not merely something Lain
dislikes:

| Error class | `gcc -Wall` | `gcc -fanalyzer` | `clang -Wall` | ASan + UBSan | Valgrind | **Lain** |
|:---|:---|:---|:---|:---|:---|:---|
| Use after free | warning | warning | missed | at runtime | at runtime | **compile time** |
| Resource leak | missed | missed | missed | missed | missed | **compile time** |
| Double free | warning | warning | missed | at runtime | at runtime | **compile time** |
| Buffer overflow | missed | warning | warning | at runtime | missed | **compile time** |
| Division by zero | missed | missed | missed | at runtime | missed | **compile time** |

A warning does not stop the build, and the two runtime columns only report a bug on a run that
actually reaches it. Measured with gcc 13.3, clang 18.1 and valgrind 3.22.

## Contents

```
  1  Proof Engine
  2  Linear Type System
  3  Borrow Checking
  4  Value Range Analysis
  5  Termination Analysis
  6  Effect System
  7  Data Layout
  8  Code Generation
  9  Limits
 10  Quick Start
```

---

# 1. Proof Engine

The program below searches a slice of bytes for a value. The slice length is not known when the
program is compiled, so every access into it has to be justified against a number that only
exists at runtime.

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

**Step 1. Lower to a typed SSA/CFG IR.** Lowering attaches every safety obligation to a
specific instruction. `elem_ptr` has to be shown in bounds, `add` has to be shown not to
overflow, and the back edge into a loop header has to be shown to terminate.

<p align="center">
  <img src="assets/figures/cfg_find_ir.png" alt="Lain-IR control-flow graph of find, with the octagon facts proved at each block" width="820">
</p>

<p align="center"><sub>The IR of <code>find</code>, drawn from the compiler's own dump. Red marks what a block
has to prove, blue what was proved there. In <code>bb2</code> the index <code>%7</code> is compared
against the length <code>%5</code>, which is what settles the access on the line above.</sub></p>

**Step 2. Work out what the values can be.** Facts are propagated through the graph, and each
loop is walked again until a pass adds nothing new, so what comes out holds on every iteration
rather than only the first.

Two kinds of fact are kept: the range a value can take, and how two values compare. The second
is what settles an array access. A range for `i` is no use when the length is unknown too, but
`i` being below the length is enough whatever either turns out to be.

`--dump-octagon` prints what the compiler settled on at the array access:

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
                                                       ; index − length ≤ −1
```

Read the last line. `%7` is the index and `%5` is the length, so `%7 − %5 ≤ -1` says the index
is at least one below the length. Neither value is known at compile time. The comparison between
them is, and that is enough to settle the bounds check before the program runs.

**Step 3. Check each obligation against those facts.**

```
  find      index bounds   @ 4:13  PROVEN check-free
  find      arith overflow @ 5:4  PROVEN check-free
  find      termination    @ 0:0  PROVEN check-free
3/3 proof obligations discharged check-free
```

**Step 4. Emit C, keeping what was proved.**

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

Nothing in the Lain source asked for `pure`, `nonnull` or `restrict`, and nothing asked for the
length to be passed separately. Each one is a fact the analyses established.

Change `<` to `<=` and the fact becomes `%7 − %5 ≤ 0`. The index can now equal the length, so
the bounds obligation no longer holds and the program is rejected:

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

The analyses in the chapters below depend on one another. Because the borrow checker guarantees
a mutable borrow is exclusive, the numeric analysis can keep a fact about a value across a
function call. Because the effect system knows a function cannot panic, the backend is allowed
to mark it `const`.

---

# 2. Linear Type System

Linear types (Girard, 1987; Wadler, 1990) require a value to be used **exactly once**. Rust is
*affine*: a value is used at most once, which is why leaking is safe there. Lain is linear, so
using a value zero times is an error as well.

Using it twice is rejected:

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

and not zero:

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

The rule applies **per field**, so a struct can be taken apart a piece at a time and each piece
is tracked separately. It is **flow-sensitive**, so consuming a value on one branch but not the
other is caught. `defer` counts as the consumer, and it counts at every exit from the function,
including the exit taken when an error propagates out.

**Rust accepts the second program.** `mem::forget` is safe, `Rc` cycles leak, and Rust states
that leaking is outside its guarantees.

---

# 3. Borrow Checking

The rule is one mutable borrow or many shared ones, never both, and a borrow ends at its last
use rather than at the end of its scope. The regions come from Cyclone (Jim et al., 2002), the
loan discipline from Patina (Reed, 2015) and Oxide (Weiss et al., 2019), and the last-use
refinement from non-lexical lifetimes.

Two things follow from that beyond the safety property.

## No lifetime annotations

Lain has no lifetime syntax. A function returning a borrow is written without one, and the
compiler works out the relationship itself:

```lain
type Data { x i32 }

func pick_x(var a Data, var b Data) var i32 { return var a.x }

proc main() i32 {
    var p = Data(1)
    var q = Data(2)
    var r = pick_x(var p, var q)
    r = 7
    return q.x
}
```

Rust rejects the same signature until a lifetime is written on it:

```
error[E0106]: missing lifetime specifier
2 | fn pick_x(a: &mut Data, b: &mut Data) -> &mut i32 { &mut a.x }
  |              ---------     ---------     ^ expected named lifetime parameter
```

The relationship is read out of the body, so the result is known to borrow `a` and not `b`, and
`b` stays usable while the result is alive. Rust can express that too, but only if you write
`<'a, 'b>` yourself: its own suggested fix ties both parameters to one lifetime and freezes `b`.

This is what a plain compile runs. `--engine=legacy` restores the pre-rebuild checker, which
assumes a returned borrow came from *every* mutable parameter. Move the write to `q` above the
last use of `r`, so that the borrow is still live when `q` is touched, and that checker rejects
the program with `E004`; the one that answers a plain compile accepts it, because it knows the
borrow came from `a`.

## Exclusive borrows become `restrict`

C's `restrict` is a promise that two pointers never refer to the same object, and the C compiler
takes it on trust. In Lain that promise is the borrow checker's output, so it can be emitted.

Below is the same loop compiled twice. The only difference is whether `restrict` was passed
through to gcc (`-O3 -march=x86-64-v3`):

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

Both versions end up with an AVX2 loop. Only the left one goes straight into it. On the right
gcc has to check at runtime whether the three buffers overlap, and has to keep a scalar copy of
the loop to jump to when they do: **59 instructions against 81**.

What that is worth depends on how long the loop runs, and it is worth being exact. The overlap
test happens once per call, so it vanishes into a long loop and dominates a short one:

```
       n    restrict     without     ratio
       8      57.2 ms      72.6 ms    1.27x
      16      47.0 ms      61.8 ms    1.32x
      64      29.7 ms      31.9 ms    1.07x
     256      19.4 ms      19.4 ms    1.00x
    4096      38.4 ms      38.4 ms    1.00x
```

On long vectors gcc's runtime versioning recovers everything and the proof buys only smaller
code. On the short ones that fill inner loops it is worth about a third. What does not vary with
`n` is that in C you have to write `restrict` yourself, and it is undefined behaviour if you are
wrong.

---

# 4. Value Range Analysis

This is abstract interpretation (Cousot and Cousot, 1977) using the octagon domain (Miné, 2006),
which stores facts of the form `±x ±y ≤ c`, widened against thresholds as in Astrée.

Storing comparisons between values, rather than only ranges, is what makes it useful on real
code. The fact that makes an array access safe is hardly ever a constant. It is usually a
comparison with a length that is itself unknown.

| the access | proven by |
|:---|:---|
| scan `a[i]` under `i < a.len` | `i − len ≤ −1`, straight from the guard |
| two-pointer `a[i]` under `i ≤ j < a.len` | transitive closure: `i − j ≤ 0` and `j − len ≤ −1` |
| reverse `a[a.len-i-1]` | `len − i ≥ 1`, from `i < len` |
| masked gather `a[x & (N-1)]`, **any** `x` | `x & (N−1) ∈ [0, N−1]` for every `x`, no assumption on the data |
| sliding window `a[i+1]` over `a i32[n]` under `i < n-1` | the guard as `i − n ≤ −2`, so `(i+1) − n ≤ −1` |
| insertion sort `a[j-1]` under `j > 0 ∧ j < a.len` | offset by −1, lower bound from `j > 0` |
| 2D `a[i*w + j]` over `i32[h*w]` | per dimension: `i < h ∧ j < w` |
| binary search `a[lo + (hi-lo)/2]` over `a i32[8]` | an inductive loop invariant recovers `0 ≤ lo < hi ≤ len`, which the loop's blanket widening had thrown away, then `mid < hi` |

Each row is a program in the test corpus. Since an index the compiler cannot place in bounds is
an `E085` rather than a runtime check, a program compiling is what tells you the bound was
proved.

The last row is the current edge: binary search proves over a fixed-length array, and the same
search over a runtime length is still rejected.

## Lengths can be part of a type

A parameter's length can be written as an expression over the other parameters:

```lain
proc vadd(var out i32[], a i32[out.len], b i32[out.len]) {
    for i in 0..out.len {
        out[i] = a[i] +% b[i]
    }
}
```

`a` and `b` are arrays the same length as `out`. Callers have to satisfy that, and are told so
with `E087` if they do not; inside the body it comes for free.

The addition wraps (`+%`) because the elements are unbounded `i32` and their sum is a real
overflow — the lengths being proven says nothing about the values. Writing `+` here is rejected:
proving where a value *lives* says nothing about how large it *is*, and the two obligations are
separate.

The expression can be anything, so a matrix is just a flat buffer with a shape. Here `h` and `w`
are unknown at compile time and `a[i*w + j]` still needs no check. They are bounded because the
declared length is itself arithmetic: `h * w` on two unbounded `usize`s overflows, and a bound
needs a named parameter to attach to.

```lain
proc msum(h usize < 4096, w usize < 4096, a i32[h * w]) i32 {
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

C has no way to state that precondition. Rust's const generics cannot take `h * w`.

## Wide accesses are checked at their real width

A 16-byte SIMD load has to satisfy `i + 16 ≤ len`, not `i < len`. That is still an ordinary
comparison, so the same analysis handles it and there is no SIMD-specific reasoning anywhere in
the compiler:

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

## Arithmetic works the same way

`+` widens its result, so adding two `i32` values cannot overflow. What can fail is storing the
result somewhere narrower, and that is where the check goes:

```lain
func widened(a i32, b i32) i64 { return a + b }     // accepted
```

```lain
func narrowed(a i32, b i32) i32 {
    var s i32 = a + b       // E086: the sum may not fit an i32
    return s
}
```

When overflow is what you want, say which kind: `+%` wraps, `+|` saturates, `a +? b else 0`
gives you a fallback value, and `unsafe { }` turns the check off. What you cannot do is leave the
question unanswered.

Division is handled the same way, and the error lists the ways out:

```
[E015] Error: division/modulo by a divisor whose range [-2147483648, 2147483647]
       includes zero. Guard it (`if d != 0`), constrain it (`d int != 0`), or wrap
       the divide in an `unsafe` block.
```

Both of those are real. `func divide(a i32, b i32 != 0) i32` puts the constraint in the
signature, where every caller has to satisfy it, and an `if b != 0` that dominates the division
settles it locally.

## Where no optimiser can follow

An optimiser can only use facts it can see. When the fact that makes an access safe lives at the
call site, and the function is compiled separately, it is out of reach. Preconditions in Lain
are part of the signature, so the caller discharges them and the callee is free of them:

```lain
func get(a i32[n], n usize, i usize < n) i32 {
    return a[i]
}
```

`i usize < n` is checked at every call. Inside, nothing is left to check, and the proof is
handed on to gcc as an assumption:

```c
__attribute__((access(read_only, 1, 2))) __attribute__((pure)) __attribute__((nonnull))
int32_t ip_get(const int32_t * restrict a, size_t n, size_t i) {
    if (i >= n) __builtin_unreachable();
    return a[i];
}
```

The same guarantee in C has to be a runtime check, because the callee cannot see who called it:

```asm
; Lain                          ; C: if (i >= n) abort();      ; Rust: a[i]
ip_get:                         get_checked:                    get_rs:
  endbr64                         endbr64                         cmp  %rsi,%rdx
  mov  (%rdi,%rdx,4),%eax         cmp  %rsi,%rdx                  jae  <panic>
  ret                             jae  <abort>                    mov  (%rdi,%rdx,4),%eax
                                  mov  (%rdi,%rdx,4),%eax         ret
                                  ret                             ...panic landing pad
```

gcc and LLVM both keep their check, and neither is doing anything wrong: the information that
would remove it is in another translation unit.

**This is the honest shape of the advantage.** Inside a single function an optimiser is often as
good. The masked index in the table above, `a[x & 4095]` over a 4096-element array, is proved by
gcc and by LLVM as readily as by Lain, and all three vectorise it. What an optimiser cannot do
is carry a fact across a boundary it cannot see through, or reject the program when the fact
does not hold.

---

# 5. Termination Analysis

A `func` always finishes. Every loop and every recursive call needs some quantity that shrinks
on each step and cannot shrink forever, and in most cases the compiler works out what that
quantity is: `while i < n` gives `n − i`, `while i > 0` gives `i`, `while lo < hi` gives
`hi − lo`.

The step does not have to be by one. Whether a quantity shrinks is something the numeric
analysis already knows, so halving and shifting need nothing written:

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

Euclid's algorithm needs the quantity named, though not justified. Since `b = a % b` lands in
`[0, b-1]`, the divisor shrinks on every iteration:

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

Recursion is held to the same standard:

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

A `proc` is allowed to loop forever. That is what separates the two.

---

# 6. Effect System

Every function gets a set of effects drawn from `{Write, Diverge, Raises, IO, Alloc}`, worked
out by following the call graph. You can declare what a function is allowed to do with
`effects …`, and the compiler checks the declaration instead of trusting it:

```
[E130] Error: 'talk' declares `effects` that do not cover what its body does —
       it also has: io.
```

## An empty effect set lifts a call out of a loop

A function with no effects writes no global state, does no I/O, allocates nothing and cannot
panic. Its result depends on its arguments and nothing else, which is what
`__attribute__((const))` claims in C.

A compiler that knows this can move the call. If the arguments do not change inside a loop,
neither can the result, so the call belongs outside it:

```lain
func scale(x i32, y i32) i32 {
    var a i32 = (x *% 2654435761) ^ (y *% 40503)
    a = a ^ (a >> 13)
    return a *% 1274126177
}

proc apply(out var i32[], p i32, q i32) {
    var i usize = 0
    while i < out.len {
        out[i] = out[i] +% scale(p, q)
        i = i + 1
    }
}
```

`scale` has no effects, so it is declared to the C compiler as `const`:

```c
__attribute__((const)) int32_t licm_scale(int32_t, int32_t);
```

Compiled separately, so the annotation is all gcc has to go on:

```asm
; effect set known                       ; effect set withheld
licm_apply:                              licm_apply:
  ...                                      ...
  call  <licm_scale>   ; once, here      <loop>:
<loop>:                                    mov   (%r15,%rbx,4),%ebp
  add   %eax,(%rdx)    ; reuse result      mov   %r14d,%esi
  add   %eax,0x4(%rdx)                     mov   %r13d,%edi
  add   $0x8,%rdx                          call  <licm_scale>   ; once per element
  cmp   %rcx,%rdx                          add   %eax,%ebp
  jne   <loop>                             mov   %ebp,(%r15,%rbx,4)
                                           add   $0x1,%rbx
                                           cmp   %rbx,%r12
                                           jne   <loop>
```

On the left the call happens once, before the loop starts, and the loop reuses the value in
`%eax`. On the right it happens on every iteration: over a 4096-element array that is 4096 calls
instead of one.

Written by hand in C, `const` is a claim nobody verifies. Here it is the effect set, which the
compiler worked out.

Panics are tracked for a specific reason. `const` also lets gcc delete a call whose result is
never used, so putting it on a function that can abort turns a program that aborts at `-O0` into
one that runs clean at `-O3`. This project shipped that miscompile once, and treating a panic as
an effect is what fixed it.

---

# 7. Data Layout

A sum type whose payload has spare bit patterns is laid out without a tag. `Option(*u8)` is one
pointer, with `None` as the null pointer:

```lain
type OptionByte {
    Some { v *u8 }
    None
}
```

```c
typedef const uint8_t * niche_OptionByte;

static inline niche_OptionByte niche_OptionByte_Some(const uint8_t * v) { return v; }
static inline niche_OptionByte niche_OptionByte_None(void) {
    return (const uint8_t *)(uintptr_t)0LL;
}
```

Rust does this too, and calls it the same thing. Errors get the same treatment, so
`*u8 | NotFound | Denied` is still one pointer wide, with the two error cases at addresses 0 and
8 where no real object can sit. No tag word, no allocation, no unwinding.

**The difference is what happens when it does not fit.** For a union whose error cases carry no
payload, the packing is not an optimisation the compiler attempts. It is required, and the
program is rejected when it cannot be done:

```
[E064] Error: the union `i32 | ...` cannot be zero-cost — 'i32' has no spare
       bit-patterns for its 2 marker(s). Give the value type niche room (a
       refinement like `u8 < 200`, a pointer, or a slice), or use fewer markers.
```

That is a deliberate trade, and it cuts both ways: a program that would have compiled elsewhere
does not compile here. Where a tag byte is genuinely unavoidable, because an error case carries
a payload, it is allowed and reported rather than added quietly:

```
[W120] Warning: enum 'OptI32' not fully zero-cost.
       Payload provides 0 sentinel slot(s); 1 empty variant(s) require 1.
       Layout falls back to 1 tag byte + payload union.
       To eliminate the tag byte: reduce empty variants, constrain the
       payload type with a refinement, or change payload to a type with
       larger sentinel space (i8: 255, i16: 65535, *T: 8192).
```

A layout optimisation that quietly fails is a cost you find later with a profiler. This one is
either guaranteed or refused, and says which.

---

# 8. Code Generation

## What the C compiler is told

Here is a nine-line sliding-window sum over a runtime length. Its Lain source carries no
annotations at all. This is the declaration it produces:

```c
__attribute__((access(read_only, 1, 2)))
__attribute__((pure))
__attribute__((nonnull))
int32_t window(const int32_t * restrict a, size_t n);
```

Five separate facts about the function, none of them written by the programmer:

| C annotation | Derived from | What it is in C |
|:---|:---|:---|
| `restrict` | The borrow checker rejects any program where two reference parameters could name the same object | A claim the C compiler trusts. **UB if wrong** |
| `access(read_write, n, m)` | The declared length: parameter *n* holds *m* elements | A claim the C compiler trusts |
| `const T *` | The set of things the function writes, computed from its body | Not checked across the call |
| `pure` / `const` | An empty effect set: no writes, no I/O, no allocation, **no panic** | A claim the C compiler trusts. If it is wrong, calls that had to happen get deleted |
| `nonnull` | A borrow can never be null | A claim the C compiler trusts |

## Targets

C99 is the backend that works. It compiles the whole corpus, and every listing on this page came
out of it. The output is portable and readable, and ordinary debuggers understand it.

There is also an LLVM path, and it is a demonstration rather than a backend. `--emit-llvm`
lowers integer functions built from `+ - *`, the comparisons, `if` and `return`, and passes the
proved ranges through as `@llvm.assume` — which is where more of the value eventually is, since
it reaches optimisations that going through C cannot express at all.

Outside that subset it now **refuses**, and it did not always. It used to emit a comment and
carry on, which sounds harmless and was not: `return a +% b` came out as `add i32 %a, 0`, so the
function returned `a`. Wrong output that looks like valid LLVM IR, from a compiler whose whole
claim is prove-or-reject — and measured across the corpus, **120 of 120 programs** contained at
least one such placeholder. It reports what it cannot model and translates nothing, the same way
the C backend refuses a construct it has not modelled.

So C is the path that runs, and the honest summary of the LLVM one is that the seam exists and
the backend does not.

Neither target owns the proofs. They are settled on the IR, which is why adding a backend is
ordinary work rather than a redesign.

---

# 9. Limits

- **A running total with no bound is rejected, not checked.** This is the rule that will cost
  you the most, and it is worth seeing why it has to be one:

  ```lain
  func up8(n u8) u8 {
      var s u8 = 0
      var i u8 = 0
      while i < n {
          s = s + i          // ERROR: [E086] this running total can overflow the type
          i = i + 1
      }
      return s
  }
  ```

  Until 2026-09-17 that compiled, and `up8(200)` printed **188** instead of 19900. A total's
  bound is `start + trips × step`, which is a PRODUCT — not something a relational domain can
  hold — so the engine asks you to bound one of the three: the count (a length with a
  refinement, `f(a i32[n], n usize < 4096)`), the element (a narrower type, or a refinement on
  it), or the total (a wider accumulator, widening the addend too). Or say which arithmetic you
  meant: `+%` wraps, `+|` saturates. The diagnostic names all four.

- **No concurrency.** Single-threaded before 1.0, so "no data races" is a consequence of that
  rather than something achieved. An interrupt-aware model is planned.

- **`unsafe` turns off the numeric and bounds checks** inside it. It is a block, so it is easy
  to find.

- **Generics are monomorphised with no trait bounds.** Mistakes show up when a generic is
  instantiated rather than where it is defined.

- **Two precision gaps have names.** An array filled by a loop or a comprehension keeps its
  length but loses its element VALUES — the seed that records them admits only literal stores
  and runs before the fixpoint — so `[i * i for i in 0..8]` then `sq[7] - 49` is refused
  although every value is known. And a fact relating three quantities at once, such as an
  allocator's `pos + size <= cap`, is outside an octagon by construction: it holds `x ± y <= c`
  with a constant on the right. Both are documented rather than hidden; `--engine=legacy`
  compiles such a program if you need it today.

- **None of this is machine-checked.** The analyses are fuzz-tested and the domain is validated
  by brute force, but there is no mechanised soundness proof. That is future work, and not
  something being claimed here.

---

# 10. Quick Start

```bash
make                                                     # build the compiler
./lain my_program.ln -o out.c                            # Lain  -> C99
gcc out.c -o my_program -Dlibc_printf=printf -w          # C99   -> executable
```

Run from the repository root if a program imports from `std/`, since module paths are resolved
relative to the source file.

```bash
make test                # the corpus
make gates               # the corpus, the examples on this page, the spec, the IR units
make fuzz                # the fuzzers: they run what the compiler claimed was safe
```

`scripts/gates/readme_gate.sh` pulls every Lain example off this page and compiles it. Examples labelled with
an error have to fail; the rest have to compile. It exists because the previous version of this
file spent months describing a language the compiler had stopped implementing.
