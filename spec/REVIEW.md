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
