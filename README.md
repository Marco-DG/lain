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
    while i < haystack.len decreasing haystack.len - i {
        if haystack[i] == target { return i }
        i = i + 1
    }
    return haystack.len
}
```

`haystack[i]` is not checked at runtime. It does not need to be: `i < haystack.len` is live at the
access, and the compiler carries that relation to the index. Here is the whole function as emitted:

```c
__attribute__((pure)) __attribute__((nonnull))
size_t find(size_t __len_haystack, const uint8_t * restrict haystack, uint8_t target) {
    size_t i = 0;
    while (i < __len_haystack) {
        if (haystack[i] == target) { return i; }
        i = i + 1;
    }
    return __len_haystack;
}
```

Three things in that signature were *derived*, not written: `pure` (the effect row is empty),
`nonnull` (a borrow is never null), and `restrict` (the borrow checker has already refused every
program in which two reference parameters could name the same object). In C each of those is a
promise you make and the compiler believes; here each is a proof.

Change the loop so the relation no longer holds and the program stops compiling:

```lain
func find(haystack u8[], target u8) usize {
    var i usize = 0
    while i <= haystack.len decreasing haystack.len - i {   // <= , not <
        if haystack[i] == target { return i }               // E085
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
```

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

---

## Where things are

| | |
|:---|:---|
| [`LANGUAGE.md`](LANGUAGE.md) | The language reference — every construct, with examples that are checked by `readme_gate.sh` |
| [`spec/`](spec/) | The normative specification (LaTeX, 135 pages), including the diagnostics annex |
| [`tests/`](tests/) | The corpus. A `_fail.ln` program names the diagnostic it must produce |
| [`bench/thesis/`](bench/thesis/) | The cost measurement, with its caveats written down next to it |
| [`std/`](std/) | The standard library |

---

> **This page is being rewritten.** The sections above are final; still to come are the
> comparison against C, C++ and Rust, the derived-annotation walkthrough, and an honest
> account of what Lain does not do. The old README is preserved in full as
> [`LANGUAGE.md`](LANGUAGE.md).
