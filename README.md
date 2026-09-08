<p align="center">
  <img src="assets/logo.png" alt="Lain" width="600">
</p>

Lain is a systems language whose safety properties are **theorems about your program**, discharged
by the compiler, and whose generated code therefore contains **no checks to enforce them at
runtime**. A program that could read out of bounds, overflow an integer, divide by zero, use a
moved value, or leak an owned resource does not compile. One that does compile carries none of
the machinery that would otherwise detect those things while it runs.

That is a stronger claim than "memory safe", and a different one from "fast". It is the
combination that is the point: the guarantee is not paid for.

```
your.ln ──▶ [ lain ] ──▶ out.c ──▶ [ gcc / clang ] ──▶ executable
                │
                └── ownership · borrows · bounds · overflow · division · termination
                    all discharged here, none of them emitted
```

---

## What the compiler guarantees

Each row is a *constraint*: the compiler must either prove it or reject the program. There is no
third outcome in which the program compiles and the property is checked later.

| Property | Guarantee | How it is discharged | Rejected as |
|:---|:---|:---|:---|
| Out-of-bounds access | **Impossible** | Every index is proven within its container's length by relational value-range analysis | `E085` |
| Integer overflow | **Impossible** | Every `+` `-` `*` is proven to stay in range; wrapping, saturating and checked forms are separate operators you ask for | `E086` |
| Division by zero | **Impossible** | The divisor is proven non-zero by a refinement or a live guard | `E015` |
| Use after move | **Impossible** | Linear types: an owned value has exactly one consumer | `E001` |
| Double free | **Impossible** | The same rule — a second consume is a second consumer | `E002` |
| Leaking an owned resource | **Impossible** | An owned value that dies unconsumed is a diagnostic, not a warning | `E003` |
| Dangling reference | **Impossible** | Region-based borrow checking with non-lexical lifetimes | `E010` |
| Aliasing violation | **Impossible** | One mutable borrow, or many shared ones — never both | `E004` |
| Reading uninitialised memory | **Impossible** | Definite assignment, per field and per constant array index | `E005` |
| Non-termination of a `func` | **Impossible** | A `func` is total: every loop and recursion needs a measure, and the measure is inferred | `E011` `E082` |
| Null dereference | **Requires `unsafe`** | Raw pointers exist; dereferencing one is a deliberate act | `E060` |
| Data races | **Structurally impossible** | Lain is single-threaded before 1.0; there is no concurrency model to race in |  |

> The compiler is a *prover*, so it refuses programs it cannot prove safe as well as programs it
> can prove unsafe. Where a proof is out of reach you narrow a type, add a guard, or say
> explicitly what you meant — `+%` to wrap, `unsafe` to take responsibility. What you cannot do
> is have the property silently unchecked.

---

## Prove or reject, shown

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

Nothing here is annotated. `haystack[i]` is not checked at runtime — `i < haystack.len` is live at
the access and the compiler carries that relation to the index. The loop needs no termination
clause either: `func` means *total*, and the measure `haystack.len - i` is inferred from the guard
and the step. Here is the whole function as emitted:

```c
__attribute__((pure)) __attribute__((nonnull))
size_t find(size_t __len_haystack, const uint8_t * restrict haystack, uint8_t target) {
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

<sub>Verbatim, except that real output prefixes the symbol with its module (`find.ln` gives
`find_find`) and interleaves `#line` directives back to the Lain source.</sub>

Change one character so the relation no longer holds, and the program stops being a program:

```lain
func find(haystack u8[], target u8) usize {
    var i usize = 0
    while i <= haystack.len {               // <= , not <
        if haystack[i] == target { return i }    // E085
        i = i + 1
    }
    return haystack.len
}
```

```
[E085] bounds error: cannot prove index is within bounds for dynamic-length array
  --> find.ln:4:13
   |
 4 |         if haystack[i] == target { return i }
   |             ^
       index `i`: range [0, MAX]
       array `haystack`: length [unknown]
```

The same shape governs the other guarantees. An owned value has exactly one consumer:

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

…and exactly one, not zero:

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

Integer overflow is the same rule applied to arithmetic. `+` **widens** — the sum of two `i32`
has a type wide enough to hold every result — so the addition itself never overflows and the
obligation attaches to the *narrowing*:

```lain
func widened(a i32, b i32) i64 { return a + b }     // accepted: an i64 holds any i32 + i32
```

```lain
func narrowed(a i32, b i32) i32 {
    var s i32 = a + b       // E086: the sum may not fit an i32
    return s
}
```

Where wrapping is what you mean, `+%` says so and carries no obligation. Where recovery is what
you mean, `a +? b else 0` says that. Where you are taking responsibility yourself, `unsafe`
waives the obligation for the block. The one thing you cannot express is *"I did not think about
it."*

---

## What the guarantees cost

Nothing you can see in the output. This is the same loop compiled twice — once with the
compiler's proof handed to gcc, once with the proof withheld. Same source, same flags
(`-O3 -march=x86-64-v3`):

```lain
proc vadd(out var i32[1024], a i32[1024], b i32[1024]) {
    var i usize = 0
    while i < 1024 {
        out[i] = a[i] +% b[i]
        i = i + 1
    }
}
```

**With the proof** — one vectorised loop, eleven instructions:

```asm
vadd:
    endbr64
    xor    %eax,%eax
.L:
    vmovdqu (%rdx,%rax,1),%ymm1
    vpaddd  (%rsi,%rax,1),%ymm1,%ymm0
    vmovdqu %ymm0,(%rdi,%rax,1)
    add     $0x20,%rax
    cmp     $0x1000,%rax
    jne     .L
    vzeroupper
    ret
```

**Proof withheld** — gcc must assume the three arrays may overlap, so it emits two runtime
overlap tests, keeps the vector loop *and* a scalar fallback, and enters the fast path only if
the tests pass:

```asm
vadd:
    endbr64
    lea    0x4(%rsi),%rcx        ; ┐
    mov    %rdi,%rax             ; │ overlap test 1
    sub    %rcx,%rax             ; │
    cmp    $0x18,%rax            ; │
    jbe    .Lscalar              ; ┘
    lea    0x4(%rdx),%rcx        ; ┐
    mov    %rdi,%rax             ; │ overlap test 2
    sub    %rcx,%rax             ; │
    cmp    $0x18,%rax            ; │
    jbe    .Lscalar              ; ┘
    ...                          ; the same vector loop
.Lscalar:
    ...                          ; and a whole scalar loop kept for the aliasing case
```

The difference is not the vectoriser being cleverer. It is one fact — *these do not alias* —
that the borrow checker established while checking the program for other reasons, and that C has
no way to state without the programmer asserting it on their own authority.

---

## The same safety, enforced at runtime instead

The alternative to proving a property is checking it while the program runs. Here is what that
costs, on a data-dependent gather where the index is masked rather than bounded by the loop:

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

`idx[i] & 4095` is proven to lie in `[0, 4095]` for *any* input, so `a[j]` needs no check. What
gcc then makes of the loop — vectorised, four masked indices per iteration:

```asm
    movdqu (%rdx,%rax,1),%xmm0     ; load four raw indices
    pand   %xmm4,%xmm0             ; mask all four at once — this IS the bounds proof
    pshufd $0x55,%xmm0,%xmm1
    movd   %xmm0,%edi
    movd   %xmm1,%r8d
    ...
```

The identical C, compiled with the checks that would make it safe without a proof
(`-fsanitize=undefined,bounds`) — scalar, with an alignment, null and overflow test in front of
every access:

```asm
    mov    %r13,%r12
    add    %rbx,%r12
    jb     c5 <gather+0xc5>        ; ┐ pointer-overflow check
    test   %r12,%r12               ; │ null check
    je     1a1 <gather+0x1a1>      ; │
    test   $0x3,%r12b              ; │ alignment check
    jne    1a1 <gather+0x1a1>      ; ┘
    mov    0x0(%r13,%rbx,1),%eax   ; ...and only now the load
    and    $0xfff,%eax
    lea    (%rcx,%rax,4),%r12
    cmp    %rcx,%r12               ; ┐ and the whole thing again
    jb     182 <gather+0x182>      ; │ for the second access
    ...                            ; ┘
```

**50 instructions against 111**, and the vectoriser is gone. Measured end to end
(`bench/thesis/`, `-O3 -march=native`, same kernel both sides):

```
(A) proven safe, check-free (what Lain emits)     :   498.6 ms
(B) runtime-checked safety (-fsanitize=undefined) :  2228.8 ms
                                                     ─────────
                                        4.47x, to learn at runtime
                                        what the compiler already knew
```

That benchmark ships with its own caveats written next to it, and they are worth repeating here:
**the win is over runtime-*checked* safety, not over expert C.** Where a C programmer writes the
`restrict` by hand and hoists the checks themselves, the machine code is the same. The difference
is what that annotation *is*.

---

## What the compiler derives that you would otherwise assert

Look again at the emitted signature from the first example:

```c
__attribute__((pure)) __attribute__((nonnull))
size_t find(size_t __len_haystack, const uint8_t * restrict haystack, uint8_t target)
```

Note also that the slice was **flattened**: a `u8[]` parameter becomes a length and a pointer,
not a struct passed by value.

Nothing in the Lain source said any of that. Each attribute is a fact some analysis had already
established for its own reasons, handed to the C compiler on the way out:

| C annotation | Derived from | What it is in C |
|:---|:---|:---|
| `restrict` | The borrow checker has refused every program in which two reference parameters could name the same object | An assertion by the programmer. **Undefined behaviour if wrong** |
| `const T *` | The computed write footprint — this function never writes through that parameter | A declaration, unchecked across the call |
| `pure` / `const` | The effect row, computed from the call graph. Empty means no writes, no I/O, no allocation, and — critically — no panic | An assertion. A wrong one lets the optimiser delete a call that had to happen |
| `nonnull` | A borrow is not a nullable pointer in the first place | An assertion |

This is the actual thesis of the language. C's ceiling, reached by an expert who annotates
everything correctly, is the same ceiling. But in C every one of those annotations is a promise
the compiler believes; here every one is a proof the compiler produced. One of them is checkable
and the other is a bet.

---

## Against the alternatives

Lain's target is the space where C is still the default — embedded, safety-critical, real-time —
and where the usual answers each give up something.

| | Covers all six classes | When | Runtime cost | Output |
|:---|:---|:---|:---|:---|
| **MISRA C** + review | No — a coding standard, not a proof | Review time | None | C |
| **ASan / UBSan / Valgrind** | Detects, does not prevent | Runtime, on the paths you exercise | Large (≈4.5x above) | Instrumented binary |
| **Certified static analysers** | Partially; unsound or incomplete in practice | Compile time | None | C |
| **Ada / SPARK** | Yes, and more — it proves functional contracts too | Compile time, via a separate prover | None | Ada |
| **Rust** | Memory safety yes; bounds are *checked*, not proven away | Compile time + runtime | Bounds checks, elided when LLVM manages it | Native |
| **Lain** | Yes, plus integer overflow and termination | Compile time | **None** | Portable C99 |

Two honest notes on that table, because it is the part that would be easiest to overstate:

**SPARK proves more than Lain does.** It will verify functional contracts, not just the absence
of runtime errors. What it asks in return is an annotation burden and a separate proof
obligation workflow. Lain's proofs are automatic and its failure mode is a compile error you
read in a second — a much smaller claim, discharged with much less ceremony.

**Rust is memory-safe, and Lain is not "safer" than Rust in that respect.** The differences are
elsewhere. A Rust bounds check is *elided when the optimiser can manage it*; a Lain bounds check
never existed, because the program that needed one does not compile. Rust does not prove
termination or integer overflow at all — `i32::MAX + 1` panics in debug and wraps in release.
And Rust requires lifetime annotations where Lain infers the relation from the body: Lain lowers
the whole module and reads which parameter a returned reference borrows out of the code, which
Rust declines to do and rejects the ambiguous signature instead.

---

## What Lain does not do

- **No concurrency.** Single-threaded before 1.0. "No data races" is a structural fact, not an
  achievement — there is nothing to race. An interrupt-aware model is roadmapped.
- **An unbounded accumulator is rejected.** `while i < n { s = s + a[i] }` can overflow `s` for a
  large enough `n`, and the compiler says so. Give it a guard, a wider type, or `+%`. This is the
  rule people meet first and it is the one that costs the most to live with.
- **`unsafe` exists**, and inside it the numeric and bounds obligations are waived. The escape
  hatch is real, lexical, and greppable.
- **Generics are monomorphised and duck-typed.** Instantiation errors surface at the
  instantiation, not at the definition; trait bounds are not implemented.
- **The proof engine is not machine-checked.** The abstract domain is validated by brute force
  against its concretisation (40 000 randomised trials per run) and by differential fuzzers that
  execute what the compiler claims to have proven — but a mechanised soundness proof is future
  work, not a claim being made today.
- **It is a young language.** The compiler is a single C99 translation unit; the corpus is 658
  programs, pass and fail alike; the middle-end is being rebuilt around a relational abstract
  interpreter, with both engines gated against that corpus on every change.

---

## Quick start

```bash
gcc -std=c99 -Wall -Wextra -o lain src/main.c -I src     # build the compiler
./lain my_program.ln -o out.c                            # Lain  -> C99
gcc out.c -o my_program -Dlibc_printf=printf -w          # C99   -> executable
```

Run the compiler from the repository root when a program imports from `std/` — module paths
resolve relative to the source file's directory.

```bash
bash run_tests.sh        # the corpus: 658 programs, pass and fail alike
bash readme_gate.sh      # every example on this page, put through the compiler
bash spec_gate.sh        # every diagnostic the compiler emits, against the specification
```

Every Lain example above is extracted and compiled by `readme_gate.sh`. A block that claims to
be an error must fail, and a block that does not must compile — because documentation that
nothing checks is how a README comes to describe a language that no longer exists.
