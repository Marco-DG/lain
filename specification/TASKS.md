# Lain Language Specification — Task Tracker

**Created**: 2026-03-06
**Goal**: Produce a complete, normative language specification for Lain, converging
the current implementation, lain.md (target design), README.md (user docs), and
16 analysis documents into a single authoritative reference.

**Approach**: Each chapter is a standalone file. The specification describes what
the language **is and shall be** — marking implemented features as normative and
unimplemented features as planned extensions with clear rationale.

---

## Specification Structure

| # | File | Title | Status | Notes |
|---|------|-------|--------|-------|
| 0 | `00-overview.md` | Overview, Scope & Conformance | TODO | Philosophy, five pillars, target domains, non-goals, document conventions |
| 1 | `01-lexical.md` | Lexical Structure | TODO | Source encoding, comments, identifiers, keywords, literals, operators, punctuation, whitespace rules |
| 2 | `02-types.md` | Type System | TODO | Primitives, structs, enums, ADTs, arrays, slices, pointers, opaque types, void, nested types, type aliases, type equivalence |
| 3 | `03-declarations.md` | Declarations & Scoping | TODO | Variable decl, mutability, type inference, shadowing, block scoping, global variables, `undefined`, definite init analysis |
| 4 | `04-expressions.md` | Expressions | TODO | Arithmetic, comparison, logical, bitwise, compound assignment, type cast, member access, indexing, function call, range, move/mut/address-of, operator precedence, evaluation order |
| 5 | `05-statements.md` | Statements | TODO | Assignment, if/elif/else, for, while, break/continue, case (pattern matching), defer, return, unsafe blocks, expression statements |
| 6 | `06-functions.md` | Functions & Procedures | TODO | func vs proc, parameter modes, return types, UFCS, forward declarations, termination guarantees, purity model |
| 7 | `07-ownership.md` | Ownership & Borrowing | TODO | Ownership modes (shared/mutable/owned), move semantics, borrowing rules, borrow checker invariants, NLL, two-phase borrows, linear types, branch consistency, linear struct fields, non-consuming match |
| 8 | `08-constraints.md` | Type Constraints & Static Verification | TODO | Parameter constraints, return constraints, `in` keyword, relational constraints, VRA algorithm, loop widening, constraint propagation |
| 9 | `09-generics.md` | Compile-Time Evaluation & Generics | TODO | `comptime` parameters, type as value, monomorphization, interaction with ownership, comptime constraints (planned) |
| 10 | `10-modules.md` | Module System | TODO | Import, module path resolution, name resolution, visibility, `export` (planned) |
| 11 | `11-interop.md` | C Interoperability | TODO | `c_include`, extern functions, extern types, type mapping, variadic parameters, name mangling |
| 12 | `12-unsafe.md` | Unsafe Code | TODO | Unsafe blocks, what unsafe allows/doesn't, address-of, pointer dereference, nesting, null pointers |
| 13 | `13-memory.md` | Memory Model | TODO | Stack allocation, heap allocation (via C), no GC, value vs reference semantics, alignment/layout |
| 14 | `14-errors.md` | Error Handling | TODO | No exceptions, return codes, Option/Result pattern, error propagation `?` (planned) |
| 15 | `15-stdlib.md` | Standard Library | TODO | std/c, std/io, std/fs, std/math, std/option, std/result, std/string (planned), std/mem (planned) |
| 16 | `16-diagnostics.md` | Diagnostics & Error Reporting | TODO | Error codes [E001]-[E015], source-line diagnostics, error format, error categories |
| 17 | `17-grammar.md` | Formal Grammar | TODO | Complete BNF/EBNF, production rules for every construct, disambiguation rules |
| A | `A-keywords.md` | Appendix A: Keyword & Type Reference | TODO | Complete keyword list with status, primitive type table, reserved words |
| B | `B-rationale.md` | Appendix B: Design Rationale | TODO | Why func/proc, why mov at call sites, why var, why no lifetime annotations, why C99 backend, why no exceptions, why explicit unsafe |
| C | `C-comparison.md` | Appendix C: Comparison with Other Languages | TODO | Feature matrix vs Rust, C, Zig, Go |
| D | `D-roadmap.md` | Appendix D: Evolution Roadmap | TODO | Planned features with priority, phased implementation plan |

**Total**: 18 chapters + 4 appendices = 22 files

---

## Task Queue

### Phase 1 — Core Language (Chapters 0-5)

These chapters have the least open design questions and can be written
directly from the implementation + existing docs.

- [x] **T01**: Write `00-overview.md` — Philosophy, scope, conformance, document conventions
- [x] **T02**: Write `01-lexical.md` — Tokens, keywords, literals, operators (source: token.h, lexer.h)
- [x] **T03**: Write `02-types.md` — Complete type system (source: ast.h Type, typecheck.h, tests/types/)
- [x] **T04**: Write `03-declarations.md` — Variables, scoping, initialization (source: resolve.h, parser/decl.h)
- [x] **T05**: Write `04-expressions.md` — All expression forms (source: ast.h Expr*, parser/expr.h, emit/expr.h)
- [x] **T06**: Write `05-statements.md` — All statement forms (source: ast.h Stmt*, parser/stmt.h, emit/stmt.h)

### Phase 2 — Semantic System (Chapters 6-9)

These chapters define Lain's unique value proposition. They require the
most careful specification and may surface design decisions to resolve.

- [x] **T07**: Write `06-functions.md` — func/proc distinction, purity, UFCS (source: resolve.h, typecheck.h, tests/core/func_proc.ln)
- [x] **T08**: Write `07-ownership.md` — THE core chapter. Ownership, borrowing, linearity, NLL, two-phase (source: linearity.h, region.h, use_analysis.h, tests/safety/ownership/)
- [x] **T09**: Write `08-constraints.md` — VRA, constraints, bounds checking (source: ranges.h, bounds.h, tests/safety/bounds/)
- [x] **T10**: Write `09-generics.md` — Comptime generics (source: comptime.h, generic.h, tests/comptime*.ln)

### Phase 3 — Infrastructure (Chapters 10-14)

- [x] **T11**: Write `10-modules.md` — Module system (source: module.h, tests/stdlib/)
- [x] **T12**: Write `11-interop.md` — C interop (source: emit/core.h, emit/decl.h, std/c.ln)
- [x] **T13**: Write `12-unsafe.md` — Unsafe code (source: resolve.h unsafe handling, tests/safety/unsafe*.ln)
- [x] **T14**: Write `13-memory.md` — Memory model (source: emit output analysis, lain.md §14)
- [x] **T15**: Write `14-errors.md` — Error handling model (source: std/option.ln, std/result.ln)

### Phase 4 — Reference (Chapters 15-17, Appendices)

- [ ] **T16**: Write `15-stdlib.md` — Standard library reference (source: std/*.ln)
- [ ] **T17**: Write `16-diagnostics.md` — Error codes and reporting (source: diagnostic_show_line, all fprintf sites)
- [ ] **T18**: Write `17-grammar.md` — Formal EBNF grammar (source: parser/*.h, existing BNF in README)
- [ ] **T19**: Write `A-keywords.md` — Keyword reference (source: token.h)
- [ ] **T20**: Write `B-rationale.md` — Design decisions (source: lain.md §23, analysis docs)
- [ ] **T21**: Write `C-comparison.md` — Language comparison matrix (source: lain.md Appendix C)
- [ ] **T22**: Write `D-roadmap.md` — Evolution roadmap (source: analysis15.md §9, analysis16.md §8, lain.md §24)

### Phase 5 — Integration & Review

- [ ] **T23**: Create `index.md` — Master table of contents with cross-references
- [ ] **T24**: Review pass — verify all chapters are consistent with implementation
- [ ] **T25**: Verify every test file is covered by at least one spec rule
- [ ] **T26**: Identify spec-implementation gaps and document them

---

## Open Design Questions

These are points where the existing documents (lain.md, README, analysis docs)
disagree or are ambiguous. They must be resolved during specification writing.

### Q1: `var` keyword semantics
**lain.md §5.1-5.2** describes `var x = 42` as immutable and `var x int = 42` as
potentially mutable, which is confusing. **README §4** describes `var` as creating
a mutable binding. The implementation in resolve.h treats `var` declarations with
explicit type as mutable. **Decision needed**: Clarify the exact rule for when `var`
creates a mutable vs immutable binding.

### Q2: Should `fun` remain as alias for `func`?
token.h line 117 has `fun` mapped to TOKEN_KEYWORD_FUNC with `/* FIXME */`.
**Decision needed**: Keep, deprecate, or remove?

### Q3: Semicolons
lain.md §2.5 lists `;` as a separator with note "optional in Lain". The actual
implementation uses newlines as statement separators. **Decision needed**: Are
semicolons truly optional statement terminators, or are they never used?

### Q4: `..=` inclusive range
TOKEN_DOT_DOT_EQUAL exists in token.h. ExprRange has `inclusive` field.
lain.md lists `..=` as reserved. **Decision needed**: Is inclusive range implemented
or just lexed? Specify exact semantics.

### Q5: `char` type for C interop
analysis16.md identifies the `*u8` vs `char*` problem blocking std/string.ln.
**Decision needed**: Should Lain add a native `char` type that maps to C's `char`,
or solve this with an annotation system?

### Q6: Expression-form match codegen
analysis16.md §7.1 documents that `var x = case adt { Variant(v): v }` has broken
codegen. **Decision needed**: Is expression-form match a normative feature that
needs fixing, or should it be specified differently?

### Q7: Struct matching
analysis15.md §10.3 documents a crash when using `case` on a plain struct.
**Decision needed**: Should pattern matching on structs be specified? If so, what
is the syntax and semantics?

### Q8: Nested type syntax
README §3.10 shows `type Token.Kind { ... }`. **Decision needed**: Is this a
first-class feature with defined scoping rules, or a naming convention?

### Q9: `use` statement
AST has STMT_USE with StmtUse struct. It doesn't appear in any test or documentation.
**Decision needed**: Is `use` a dead feature to remove, or an unfinished feature to specify?

### Q10: Return annotations — `return mov` vs `return var`
README §5.7 documents both forms. **Decision needed**: Should `return var` create a
persistent borrow? What are the exact lifetime rules?

---

## Source Material Index

| Source | Purpose | Lines |
|--------|---------|-------|
| `lain.md` | Target spec with 🟢/🔴/🟡 markers | 1909 |
| `README.md` | User-facing documentation | 2151 |
| `analysis1-16.md` | Development history & decisions | ~5000 |
| `src/token.h` | Lexical tokens (exact keyword list) | 332 |
| `src/ast.h` | AST nodes (every language construct) | 991 |
| `src/lexer.h` | Lexer implementation | 345 |
| `src/parser/*.h` | Parser (grammar as code) | 2158 |
| `src/sema/*.h` | Semantic analysis (ownership, types, VRA) | 5497 |
| `src/emit/*.h` | C99 code generation | 2687 |
| `src/module.h` | Module system | 137 |
| `tests/**/*.ln` | 109 test files (behavioral specification) | ~2000 |
| `std/*.ln` | Standard library (7 modules) | ~80 |

---

## Conventions for Specification Writing

1. **Normative language**: Use "shall" for requirements, "shall not" for prohibitions,
   "may" for permissions, "should" for recommendations.

2. **Implementation status**: Each section header includes one of:
   - `[Implemented]` — current compiler enforces this rule
   - `[Planned]` — specified here but not yet implemented
   - `[Extension]` — future direction, not binding

3. **Examples**: Every rule includes at least one valid example and one invalid
   example (with the expected diagnostic).

4. **Cross-references**: Use `(see §X.Y)` format for internal references.

5. **Constraints**: Rules that impose compile-time constraints are marked with
   `CONSTRAINT:` prefix.

6. **Undefined behavior**: Explicitly called out as `UNDEFINED BEHAVIOR:` with
   rationale for why it cannot be statically prevented.

---

*Last updated: 2026-03-06*
