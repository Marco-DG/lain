# Using the Lain compiler

How to build the compiler, compile a program, run one without compiling it, and read what comes
back. This is about the **compiler**; for the language see [LANGUAGE.md](LANGUAGE.md), and for what
the language *is* rather than what this implementation does, the specification in [spec/](spec/).

Every command and every exit code on this page was taken from a run, and the flag table is checked
against `src/frontends/lain/args.h` by `scripts/gates/readme_gate.sh` in both directions: a flag
named here must be accepted, and a flag the compiler accepts must appear here.

---

## 1. Build the compiler

```bash
make
```

That is `gcc -std=c99 -O2 -Wall -Wextra` over a single translation unit. The equivalent by hand:

```bash
gcc -std=c99 -O2 -o lain src/frontends/lain/main.c -I src
```

`-O2` is not cosmetic: at `-O0` the compiler is roughly 122× slower on `bench/simd_lexer`, for
identical verdicts.

## 2. Compile a program

Lain emits C99. There are two steps, and the second is an ordinary C compile:

```bash
./lain my_program.ln -o out.c                       # Lain -> C99
gcc out.c -o my_program -Dlibc_printf=printf -w     # C99  -> executable
```

**Why `-D`.** The standard library declares C functions under a `libc_` prefix — today exactly
`libc_printf` and `libc_puts` — so that a Lain program's names cannot collide with C's during
compilation. The emitted C still calls them by those names, so each one used must be mapped back:

```bash
gcc out.c -o prog -Dlibc_printf=printf -Dlibc_puts=puts -w
```

Omit a mapping a program needs and the linker reports an undefined symbol.

**Why `-w`.** Without it gcc warns `conflicting types for built-in function 'printf'`, because the
emitted declaration is `int32_t printf(uint8_t*, ...)`. The mismatch is real rather than spurious —
it is the prefix convention meeting gcc's built-in prototype — and it is a warning, not an error, so
the program still builds. `-w` silences it.

**Where the output goes.** `-o` is relative to the **working directory**, not to the source file.
Without `-o` the C is written to `out.c`.

## 3. Run a program without emitting C

`--interpret` runs an accepted program on the IR's own semantics instead of emitting anything, and
**every proof the compiler discharged is checked as it is used**:

```bash
./lain my_program.ln --interpret ; echo "exit: $?"
```

The process exits with the value `main` returns (see §4 for the one catch). `--check-invariants`
implies `--interpret` and adds a stronger check: the range analysis's state at each block must
contain the running program's, so a proof that does not describe the real execution is caught rather
than believed.

> [!NOTE]
> The interpreter runs the IR's semantics, so it cannot see a wrongly-typed **IR** — it would
> execute the same mistake faithfully. It catches a wrong *analysis*, not a wrong *lowering*.

## 4. Exit codes

| code | meaning |
|:-----|:--------|
| `0` | success — the program was accepted and the C written |
| `1` | the program was refused, or the input could not be read, or a flag was not recognised |
| `70` | **internal error**: a proof obligation reached code generation undischarged. This is a compiler bug, not a program error — please report it with the source. With `--check-certificate`, also a certificate that does not check, which prints `internal error: the certificate for '<function>' does not check:` and the reason. If this compiler wrote that certificate for this same program, it is a compiler bug as well (the search and the check disagree); if the certificate was edited, or came from another build or program, the certificate is wrong |
| *other* | with `--interpret` only: the value `main` returned |

Two things worth knowing about the last row. An exit status is **8 bits**, so a returned value is
seen modulo 256: `main` returning 256 exits 0, and 300 exits 44. And a compile failure also exits 1,
so a program whose `main` legitimately returns 1 is indistinguishable from a refused one by status
alone — check stderr, which is empty on success.

## 5. Flags

```bash
./lain                      # prints a usage summary
./lain FILE.ln [options]
```

| Flag | Effect |
|:-----|:-------|
| `--help` | Print the option table and exit 0. |
| `-o <file>` | Write the emitted C to `<file>` (default `out.c`). A relative path resolves against the working directory. |
| `--target=<triple>` | Set the target triple used for layout and emission. |
| `--interpret` | Run the accepted program on the IR's own semantics (`src/ir/interp.h`) instead of emitting C. Every discharged proof is checked as it is used, and the process exits with the value `main` returns. |
| `--check-invariants` | Implies `--interpret`. The range analysis's state at each block must contain the running program's, so a proof that does not describe the real execution is caught. |
| `--dump-ast` | Print the parsed syntax tree. |
| `--dump-effects` | Print each function's inferred effect row. |
| `--dump-niche` | Print the niche-packing decision for each sum type. |
| `--dump-octagon` | Print the converged octagon state per basic block. A value that came from a named local prints as `%2:i`; the rest are temporaries. |
| `--dump-measures` | Print the measure behind each termination proof, in the program's own names, one line per loop or recursion — and `no measure found` for a loop that is about to be refused. |
| `--emit-certificate <file>` | Write what the range analysis found, per function, to `<file>`: the loop-header octagon states, each loop's and recursion's termination measure, element and return ranges, call-site ranges with the callee's certificate nested, and accumulator bounds. It is text meant to be read by a person. |
| `--certificate-roundtrip <file>` | Parse a certificate and print it back. This is the format's own test — emitting, parsing and re-emitting must give the same text — rather than a tool for everyday use. |
| `--check-certificate <file>` | Compile while CHECKING the certificates in `<file>` instead of searching for the proofs: each function's loop-header states and side facts are read from the file, every other state is rebuilt from them in one pass, and every stated fact is checked. A certificate that does not check is an internal error (exit 70), not a diagnostic. It attests the derivation, not the transfer functions the check shares with the analysis. |
| `--no-line-directives` | Omit `#line` directives from the emitted C. |
| `--emit-llvm` | Lower to proof-carrying LLVM-IR. A demonstration seam, not a backend: outside the subset it models it **refuses** rather than emitting a placeholder. C is the backend that works. |

### Accepted and ignored

Three flags are accepted and do nothing. They are listed here rather than in the table above,
because a no-op documented as a feature is worse than an absent one:

- `--engine=…` and `--backend=…` — legacy selectors. There is one engine and one backend.
- `--no-w130` — `W120` is the only warning the compiler emits; the warning this suppressed was
  removed along with `proc`.

An unrecognised flag is an error rather than a filename, so a mistyped flag fails the build instead
of being read as a source path.

`--help` prints the same table and exits 0, and so does running `lain` with no arguments. An
unrecognised option prints it to stderr and exits 1. All three come from one table in the compiler,
so they cannot disagree with each other — and this page is checked against the flags the compiler
accepts, in both directions, so it cannot disagree with them either.

## 6. Reading a diagnostic

```lain
func narrowed(a i32, b i32) i32 {
    var s i32 = a + b     // ERROR [E086]
    return s
}
```

```
[E086] Error Ln 2, Col 5: arithmetic is not provably free of overflow
   |
 2 |     var s i32 = a + b     // ERROR [E086]
   |     ^
```

The code, the line and column, the message, the file position, and an excerpt with a caret under the
construct.

**The caret points at the operation that carries the obligation, which is not always the one you
expect.** Here it is under `var`, not under `a + b`: `+` *widens*, so the addition cannot overflow,
and what needs proving is the **narrowing** — storing a value that may not fit back into an `i32`.
Read the caret as "this is the step that needs a proof", not as "this is the suspicious-looking
operator".

A code is stable and means one thing: `LANGUAGE.md` §12.1 indexes them, and **Annex B of the
specification is the authority**, reconciled against the compiler's own strings by
`scripts/gates/spec_gate.sh`.

## 7. Checking the project itself

```bash
make test        # the test corpus: pass tests exit 0, fail tests exit non-zero
make gates       # every gate, including the documentation gates
make fuzz        # the fuzzers
```

The documentation gates are worth knowing individually, because each reads a different *kind* of
claim and "the gates are green" means much less than it sounds:

| gate | what it checks |
|:-----|:---------------|
| `readme_gate.sh` | every ```lain block on this page, README and LANGUAGE compiles — or fails, if it says it illustrates an error; every flag in both directions; every diagnostic code named; and every block marked `// VERIFY: exit N` is **run** |
| `spec_gate.sh` | Annex B against the codes the compiler emits, the PDF against its sources, and the specification's own examples |
| `quoted_diag_gate.sh` | a diagnostic quoted in a document is still what the compiler prints |
| `prose_gate.sh` | a probe per documented claim, each naming the claim and the code it expects |
