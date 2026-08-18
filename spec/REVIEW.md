# Spec review — page-by-page reconciliation against the compiler

A deep pass over every chapter + annex, checking each concrete claim (syntax,
C-mappings, constraints, diagnostic codes, examples) against what the compiler
in `src/` actually does. Three outcome kinds:

- **FIXED** — the spec stated something untrue/stale; corrected in place.
- **GAP** — the spec's intent is right (matches the language philosophy) but the
  compiler doesn't enforce it. Spec left as-is; the compiler needs hardening.
  These are decisions to confirm (fix the compiler, or relax the spec?).
- **OPEN** — needs a design call from the author.

Verification method: live probes with the built `./lain` + grep over `src/`,
`tests/`, `std/`. Every code attribution below was checked by compiling a snippet
and reading the actual emitted diagnostic, not inferred.

---

## §05 Lexical conventions

### FIXED (spec was wrong)
1. **`undefined` keyword** — listed in the keyword table but the keyword was
   **removed entirely** in commit 799f4cf (P3). Not a token in `src/token.h`.
   Removed the row.
2. **`panic` keyword** — listed as a keyword, but it is **not** a lexical keyword
   (`src/token.h` has no `panic` token). It's a builtin *identifier* of type
   `Never`, recognized in `resolve.h:863` (`strcmp(raw,"panic")`), documented as a
   builtin in §12. Removed it from the keyword table.
3. **`not` operator** — the operator table listed `not` as logical-NOT (prefix)
   with `!` as an "alternate form". The compiler has **no `not`** (`not b` →
   `[E100]`, parsed as an identifier); only `!` (`TOKEN_BANG`) works. Removed `not`,
   made `!` the sole prefix-NOT.
4. **String-literal coercion `[u8]`** — Lain slice syntax is **postfix `u8[]`**
   (18 uses in tests/std; prefix `[u8]` appears 0 times). Fixed `[u8]` → `u8[]`.
5. **`*` "future: pointer dereference"** — deref is **implemented** (unary prefix
   `*p`, allowed inside `unsafe`; see `tests/unsafe/unsafe_valid_pass.ln`). Dropped
   the stale "(future:)" note.
6. **Integer-literal-out-of-range → `E100`** — the compiler emits **`E086`**
   (`var c u8 = 256` → `[E086]`; routed through the same range mechanism as
   arithmetic overflow). Fixed the constraint and example (2 spots).

### GAP (compiler doesn't enforce what the spec promises — spec kept)
- **G-05.1 — semicolons silently accepted.** §05 says `;` "shall not appear …
  shall emit `[E100]`", and the rationale explicitly wants to *prevent
  multi-statement lines*. But the parser accepts `;` as a statement terminator
  (its own error text reads "Expected ';' or newline after statement"), so
  `var x = 5; return 0` compiles cleanly (exit 0). The spec intent is correct and
  desirable — this is a **parser hole to close** (make stray `;` → `E100`), not a
  spec error. **Decision needed.**
- **G-05.2 — unrecognized escape silently accepted.** §05 says a backslash not in
  the escape table is ill-formed `[E100]`. But `"a\q"` compiles cleanly (exit 0) —
  the lexer passes the unknown escape through instead of diagnosing it. A lexer
  hole that can mask typos. **Decision needed.**

### Verified correct (no change)
- `comptime` **is** a reserved keyword (`TOKEN_KEYWORD_COMPTIME`), even though
  comptime *blocks* are deferred — keep it listed.
- `&` as borrowed-match prefix (`case &x`) is real (`tests/ownership/match_borrow_pass.ln`).
- `int` is a real default integer type name (`typecheck.h`, `parser/type.h`).
- `E100` for: empty/multi char literal, keyword-as-identifier, unterminated block
  comment — all confirmed by probe.

---

## §06 Basic concepts

### FIXED (spec was wrong)
1. **Shadowing described as "permitted"** — §06 said a name may shadow an outer
   one, "innermost takes precedence", impl "may issue a warning". The compiler
   **forbids all shadowing**: `resolve.h:478` rejects any name already visible
   (`sema_lookup`) with `[E013] "Redeclaration or shadowing … is forbidden"`.
   Verified across inner-`if`, `while`, and param-shadowed-by-local — all `[E013]`.
   Rewrote §6.2/§6.3 to state shadowing is forbidden (with a rationale), and the
   name-resolution "innermost wins" paragraph accordingly.
2. **`E013` = "undeclared identifier"** — wrong. In the compiler `E013` is
   **redeclaration / duplicate parameter / `main`-must-be-`proc`** (`sema.h:2815`,
   `resolve.h:182,478`). Re-pointed the constraint + example at the real meaning
   (redeclaration), and split out undeclared-use as its own rule.

### GAP (compiler doesn't enforce what the spec promises — spec kept)
- **G-06.1 — undeclared identifiers are not diagnosed (broken-C hole).** HIGH.
  `func f() i32 { return zzz }` compiles clean at the Lain level and emits
  `return zzz;`, which **gcc rejects** ("`zzz` undeclared"). This is exactly the
  broken-C codegen class the project drove to zero — it just isn't exercised by
  any `_pass` test (no test references a name it doesn't declare). The spec keeps
  "undeclared is ill-formed" (with no code cited, since the compiler emits none).
  **Recommend: add a resolver diagnostic** (careful of UFCS / forward refs /
  builtins like `panic`). **Decision needed.**

---

## §07 Types

### FIXED (spec was wrong)
1. **Array type `[T; N]`** (Rust spelling) — Lain is **postfix `T[N]`** (its own
   examples use `int[5]`). Fixed.
2. **Array OOB → `E014`** — the compiler emits **`E085`** (`a[5]` on `i32[5]` →
   `[E085]`). `E014` is *non-exhaustive match*, unrelated. Fixed constraint +
   example.
3. **Slice `*T[]`, "the `*` prefix is mandatory"** — **false**. `T[]` and `*T[]`
   compile to **identical C** (`size_t __len + T* restrict`), so the `*` is a
   tolerated synonym, not mandatory. Established the coherent, compiler-verified
   model and rewrote §7.5: **fat slice = `T[]` (no star, `{len,data}`); thin/raw
   views carry the star** (`*T[N]` ptr-only, `*T` raw). Confirmed by emission:
   `*T[N]`→`const T*` (ptr only), `T[N]`→`Fixed_T_N*` (inline), fat slice struct is
   `{ .len, .data }` (len first). Fixed the taxonomy table, the sized-slice table
   (it used `*T[…]` while its own examples used `T[…]`), and the string-literal
   coercion (`*u8[]`→`u8[]`, matching the §05 fix).
4. **Struct field `name [u8]`** (prefix bracket) — prefix `[u8]` is **rejected**
   (`[E100]`). Fixed to `name u8[]`.
5. **Pointer deref in safe code → `E012`** — the compiler emits **`E060`**
   ("pointer dereference outside 'unsafe' block", `typecheck.h:2551`). Fixed.

### OPEN (design call for the author)
- **O-07.1 — slice spelling `T[]` vs `*T[]`.** Both compile identically today. I
  canonicalized the spec to **`T[]`** (fat = no star; star = thin/raw) — coherent,
  matches majority test usage (30 bare vs 20 star) and your "senza asterisco"
  instinct. If instead you want the star **required** or **rejected**, that's a
  compiler change (parser). Confirm the canonical form, and whether `*T[]` should
  eventually warn/deprecate.

### Verified correct (no change)
- `int` alias of `i32`; `iN`/`uN` for N∈[1,64]; rank/widening; refinement aliases.
- Struct linear-field must be `mov` → **`E083`** (confirmed); `mov name type` and
  `name mov type` field orders both parse (one-liner `{ … }` needs a newline).
- Pointer-mode table (`*T` const, `var *T`, `mov *T`) matches emission.

---

## §08 Expressions

### FIXED (spec was wrong)
1. **Unary-operator row lists `not`, `ref`, `mut`** — none exist. `not` is not a
   token; `ref x` / `mut x` → `[E100]` (parsed as identifiers). Dropped them;
   kept `-`, `!`, `mov`.
2. **Entire §8.9 "Borrow expressions (`ref`, `mut`)"** — stale. Lain has no prefix
   borrow operator; borrows are expressed at the call site (`f(x)`/`f(var x)`,
   `mov` for transfer). Rewrote the section accordingly (kept the label).
3. **Identifier expr "shall be declared … (E013)"** — `E013` is redeclaration, not
   undeclared. Re-pointed to §6.3 (undeclared is ill-formed; and see G-06.1).
4. **`not`/`!` in logical-operator prose + constraint** — dropped `not`.
5. **Shift amount out of range → `E100`** — the compiler emits **`E086`** (even
   `0u8 << 20`, no result overflow, → `[E086]`); it's the boundary mechanism.
   Fixed.
6. **Index expression OOB → `E014`** — **`E085`**. Fixed (same error as §07).
7. **Call-site table "parameter mode must be `shared`"** — no `shared` keyword;
   shared is the default (no mode keyword). Reworded.

### Verified correct (no change)
- E005 use-before-init; E015 static div-by-zero; E012 pointer **cast** outside
  `unsafe` (distinct from E060 for **deref**); E001 use-after-move; E087 sub-slice
  `a>b` literal ordering; E085 sub-slice start/end bounds. All confirmed by probe.
- Precedence/associativity table (minus the removed unary tokens); `in` operator
  and in-guard semantics; two-phase borrow note.

---

## §09 Statements

### FIXED (spec was wrong)
1. **`undefined` references** (removed entirely in commit 799f4cf, P3) — the
   immutable-decl constraint ("initializer shall not be `undefined`") and the
   `var buf u8[256]` comment ("defaults to undefined"). Reworded both: immutable
   needs a definite initializer; an unassigned `var` must be written before read
   (`[E005]`).
2. **§9.7 Q-009: "`return mov x` / `return var x` are syntactically ill-formed
   (E100)"** — **false now.** Both are accepted: `return mov x` = explicit
   ownership transfer, `return var x` = return a mutable borrow (the deliberate
   "Q-004 option C" pattern, `return_borrow_var_param_pass.ln`). Escaping a borrow
   of a *local* → `[E010]` (dangling). Rewrote the paragraph + constraint.
3. **Decreasing-measure `E081` description** — spec said "references ≥1 variable
   assigned in body". Actual `E081` = "cannot extract variables from termination
   measure" (unanalyzable form); the no-decrease / constant / no-assigned-var
   cases are `E080` ("cannot verify measure"). Reworded to match the real split.

### Verified correct (no change)
- `E009` assign-to-immutable; `E003` linear overwrite / branch-consistency /
  live-at-return; `E011` unbounded `while` in `func`; `E080`/`E082` decreasing;
  `comptime if` accepted; `for i in a..b` accepted; `var buf u8[256]` uninitialized
  local accepted (read-before-write is E005).

---

## §10 Declarations

### FIXED (spec was wrong)
1. **`func dot(a [f64], b [f64], …)`** — prefix bracket; fixed to `f64[]`.
2. **`comptime` type parameters** — the syntax is `name type` (no `comptime`
   prefix): `func max(T type, a T, b T) T` compiles; `func id(comptime T type,…)`
   → `[E100]`. Fixed §10.4 signature prose and the type-parameter paragraph.
3. **Zero-field struct → `E100`** — **accepted**: `type Empty {}` + `Empty()`
   compile (exit 0). Replaced the constraint with a note that unit structs are
   permitted.
4. **"program without `proc main` → `E100`"** — a `main`-less file **compiles**
   (exit 0): a module/library needs no `main`. Reworded — the requirement is on
   the linked *program*, and a missing entry point surfaces at link.
5. **Top-level `comptime { … }` declarations** — **not implemented** (a top-level
   `comptime` block is `[E100]`). Marked as a planned feature (the keyword is
   reserved; `comptime if` works).

### Verified correct (no change)
- Attributes: `[private]`, `[fast_math]` accepted; unknown `[bogus]` → `E103`;
  empty `[]` → `E102`. Param ABI: shared struct → `const T*`; `extern func`
  allowed. `mov name T` makes the param **linear** (must be consumed, `return
  mov p`, else `E003`).

---

## §11 Ownership and linearity

### FIXED (spec had the wrong diagnostic code on three rules)
Verified against the actual `fprintf` strings in `src/` **and** the `_fail`
test oracles:
1. **`E005` = "move a variable with active borrow"** — **wrong.** `E005` is
   **use of an uninitialized value** (`uninit_*`, `field_uninit_read`,
   `uninit_scalar_read`, `array_uninit_element_read` — ~9 tests). Rewrote the
   constraint. (Move-while-borrowed is `E008`, already stated separately.)
2. **`E007` = "consumed in one branch but not all"** — **wrong** (that is
   `E016`). `E007` is **implicit move: a linear value passed to a `mov` param
   without the `mov` keyword** (`move_needs_mov_fail`, `close_without_mov_fail`).
   Rewrote.
3. **`E001` "Consumed *or Uninit* → E001"** — Uninit is `E005`, not `E001`.
   Narrowed E001 to use-after-move (Consumed) and cross-referenced E005.
4. **`E002` "double-move"** — the compiler's `E002` fires for a **field**
   consumed twice ("field '…' was already consumed"); reworded to field-level
   double-consume.
5. **Var-locals "hold linear values that must be consumed"** — only *linear*
   locals carry the E003 obligation; unrestricted locals don't. Clarified.

### Verified correct (no change)
- `E003` leak, `E004` borrow-aliasing, `E006` consume-in-loop, `E008`
  move-while-borrowed, `E016` branch-inconsistency (incl. field-level) — all
  match. `var name T` primitive → by-pointer `T*` write-through; two-phase
  borrows; NLL last-use; `case &expr` borrowed match → E004 on mutation.

---

## §12 Functions and procedures

### FIXED (spec was wrong)
1. **§12.6 Q-009 duplicate** — same stale "`return mov x`/`return var x` → E100"
   as §09; both are accepted. Replaced with the correct behavior + the E010
   dangling rule (mirrors §09).
2. **`func is_empty(s [u8])` and `func head(s [u8])`** — prefix bracket; → `u8[]`.
3. **UFCS not-found → `E013`** — `E013` is redeclaration; and an unresolved UFCS
   call is in fact *not* diagnosed (`x.nonexistent()` compiles — same undeclared
   gap, G-06.1). Reworded step 4.
4. **"Generic functions (`func` with `comptime` parameters)"** — type params, not
   `comptime`. Fixed.

### Verified correct (no change)
- func/proc distinction table; purity → E011 (proc-call, unbounded-while,
  recursion); UFCS ownership desugaring; ABI table; `panic` builtin (Never type,
  abort, no defer/unwind); W130 (proc-could-be-func) and W120 exist.

---

## §13 Value range constraints (VRA)

### FIXED (spec was wrong)
1. **Return-constraint violation → `E012`** — actually **`E086`** (`func nz() int
   >= 1 { return 0 }` → `[E086]`). The *parameter*-constraint violation at a call
   site **is** `E012` (`f(0)` for `int>=1` → `[E012]`) — so the two were conflated.
   Fixed the return-constraint constraint; noted the split.
2. **"~280 LOC in src/sema/omega.h"** — the file is **414 lines**. Changed to
   "roughly 410 lines".
3. **Limitation "VRA does not track ranges of struct fields"** — over-broad since
   the field-sensitive work (`bounds.h`/`ranges.h`/`linearity.h`): still true for
   general field *value ranges*, but per-field initialization and member-path
   *slice length* (within an `and`-scope) now are tracked. Refined.
4. **Sized-slice example declared `var empty i32[0] = []`** — `i32[0]` is itself
   rejected (zero-size array), so the example wouldn't build. Rewrote it around
   `pair_sum(arr i32[>= 2])` with `i32[1]`/`i32[2]`.

### Verified correct (no change)
- Range-propagation table; conditional refinement; in-guard → measure
  non-negativity; E015 div-by-zero in the range table; E087 sized-slice
  call-site (provable violation, e.g. `pair_sum(single)`); E085 variable
  endpoints; the four VRA levels + Omega Test / Fourier-Motzkin description.

---

## §14 Generics and compile-time evaluation

The chapter describes a fuller generics system than is built. Verified what
compiles:

### FIXED (spec presented planned features as working)
- **Works** (kept): generic `func` with `type` params (`func max(T type, a T, b
  T) T`) by inference *and* explicit type arg (`identity(i32, 42)`); generic
  types `type Box(T type) = type { ... }` with instantiation `Box(i32)(v)`; E101
  (CTFE calls a `proc`); `comptime if`.
- **Planned, NOT yet implemented** (each now flagged; all currently `[E100]`):
  compile-time *value* parameters / const generics (`comptime N int`,
  `type Buf(comptime N usize)=…`); `func`-returning-`type`
  (`func Option(T type) type`); `comptime { expr }` expression blocks.
- Added an **implementation-status note** to §14.1 drawing the working/planned
  line.
- **§14.8 comptime recursion depth → `E014`** — `E014` is *non-exhaustive match*.
  Reworded: the depth bound belongs to the not-yet-built comptime engine; no
  depth diagnostic is emitted today.

---

## §15 Pattern matching

### FIXED (spec was wrong / examples wouldn't build)
1. **`type Color { Red, Green, Blue }`** (single-line, comma) and **§10
   `type Direction { North, South, East, West }`** — single-line comma-separated
   enum variants are **`[E100]`**; the compiler needs **one variant per line**
   (matching the no-multi-statement-line rule). Rewrote both multi-line.
   (ADT-variant *internal* fields may stay comma-inline — `Rectangle { w i32, h
   i32 }` is fine; top-level struct fields may not.)
2. **Range pattern `1..10` inclusivity** — the spec said "inclusive"; confirmed
   by emission (`>= 1 && <= 10`). Added a note that `..` is **inclusive in a
   pattern** but **exclusive** in expression ranges/sub-slices — a genuine
   two-meanings-for-`..` wart (flagged as O-15.1).

### GAP / limitation (compiler)
- **G-15.1 — two boolean-literal arms don't parse.** `case b { true: … false: …
  }` → `[E100]`. So §15's "bool: true and false covered, or else" is not
  achievable via true+false; a `bool` match needs an `else` arm. Added a note;
  the parser limitation is worth fixing (bool has exactly two values).

### OPEN
- **O-15.1 — `..` means inclusive in patterns, exclusive in expressions.**
  Documented as-is; consider unifying (e.g. require `..=` for inclusive patterns)
  — a compiler change.

### Verified correct (no change)
- `else` is the wildcard (`_` is not — `_:` → `[E014]`); enum/ADT all-variants
  exhaustiveness (no else) works; non-exhaustive → `E014`; `case &expr` mutation
  → `E004`; string patterns accepted; literal + range + comma-multi patterns.

---

## §16 Module system

### FIXED (spec was wrong)
1. **"top-level `module path.name` declaration"** — there is **no `module`
   keyword** (`module foo.bar` → `[E100]`); module identity is derived from the
   file path only. Retitled §16.2 "Module identity" and rewrote it.
2. **Selective import undocumented** — `import module.path.{a, b}` is used
   throughout the stdlib (`std.mem`, `std.fs`) but §16 only had whole-module
   `import`. Added the selective form. Also noted the `use` statement (local-scope
   alias, a real keyword, semantics impl-defined this revision, unused by stdlib).

### Verified correct (no change)
- Path resolution (dots→dirs + `.ln`, relative to cwd); dedup / circular-import
  no-op; `[private]` → module-private → `E084`; std module paths.

---

## §17 C interoperability

### FIXED (spec was wrong)
1. **`bool` → `int`** — the compiler emits **`_Bool`** (C99 bool, 1 byte;
   consistent with §07 "single byte"). Fixed.
2. **`[T]` fat-pointer `struct { T* data; size_t len; }`** — two errors: prefix
   bracket (→ `T[]`) and field order (the struct is **`{ size_t len; T* data; }`**
   — len first, confirmed by emission). Fixed both.
3. **"Lain strings (`[u8]`)"** — → `u8[]`.

### Verified correct (no change)
- `c_include`, `extern func`/`extern proc` (→ E011 from a `func`), the trust-
  boundary note, struct/primitive ABI table, pointer casts → E012, `extern type`
  opaque types (accepted).

---

## §18 Unsafe code

### FIXED / improved
1. **"Direct ADT field access"** — documented the concept but not the code or the
   form. E125 fires on the **qualified** access `value.Variant.field` outside
   `unsafe` (`tests/types/unsafe_adt_fail.ln`: `s.Circle.radius`). Added the form
   + `E125`, and cited the codes on the other "disabled by unsafe" rows (deref
   E060, casts E012, bounds E085).

### Verified correct (no change)
- unsafe enables deref / address-of / casts / direct-ADT / bounds-suppression;
  does NOT disable ownership/borrow/type/div0/exhaustiveness/purity; UB list;
  block borrow-scope; nesting.
