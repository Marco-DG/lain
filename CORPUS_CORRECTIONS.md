# CORPUS CORRECTIONS — tests that encode a BUG, not a requirement

The 625-test corpus is the compiler's specification of record. It is also *written against the
old engine*, so wherever the old engine is wrong the corpus faithfully records the wrong answer
— and a fail-test asserting `// EXPECT: [E004]` on a **safe** program is not a specification,
it is a bug with a test protecting it.

This file is the ledger of those. Each entry is a test whose verdict **inverts** when the
sovereign IR becomes authoritative (REBUILD.md Stage 3.5). Without this record, every one of
these looks like a regression at switchover and gets "fixed" by making the new engine wrong.

> **Rule.** Nothing lands here on a hunch. An entry requires (a) an argument that the program
> is safe, (b) where possible an *executed* demonstration, and (c) a statement of what the old
> engine's actual rule is and why it misfires. An entry that only says "the new engine
> disagrees" belongs in `phase3_adjudications.txt` as a gap, not here.

---

## C-1 · `tests/ownership/mut_plus_nested_shared_fail.ln` — fail ⇒ **pass**

```lain
proc scale(var v Vec, factor i32) { v.x = v.x *% factor }
func length(v Vec) i32 { return v.x }
scale(var v, length(v))            // asserted [E004]; actually SAFE
```

This is `v.push(v.len())` — the canonical **two-phase borrow**. `length(v)` is evaluated to an
`i32` *before* `scale` is entered; the mutable borrow is reserved during argument evaluation and
activates only at the call. There is no point at which a read and a write of `v` are both live.

**Executed proof.** The hoisted form is semantically identical and compiles and runs:
```lain
var f = length(v)                  // same reads, same order
scale(var v, f)                    // exit 4, as expected
```

**The corpus contradicts itself.** `tests/ownership/two_phase_borrow_pass.ln` *requires*
`push_n(var v, v.cap)` — the same shape — to be **accepted**. The only difference is that the
shared read goes through a call rather than a field access, which is exactly the old engine's
documented blind spot: *"two-phase borrow scope — covers direct-read args, NOT nested-call
args; weaker than Rust."* The test encodes the blind spot as a requirement.

**New engine.** Accepts by construction, no special case: a by-value `i32` argument is a COPY,
and a copy is not a loan. (Rust's own two-phase is restricted to method autoref and would
reject the explicit form — but Lain already chose otherwise in `two_phase_borrow_pass`, so
rejecting here is inconsistent regardless of which rule you prefer.)

---

## C-2 · `tests/ownership/multi_var_param_fail.ln` — fail ⇒ **pass**

```lain
func pick_x(var a Data, var b Data) var i32 { return var a.x }
var ref = pick_x(var da, var db)
var y = read_data(db)              // asserted [E004]; actually SAFE
var x = use_ref(var ref)
```

The test's stated premise is *"db is still borrowed by ref"*. That is **false**. Both parameters
are mutably borrowed *for the duration of the call*; only the returned reference's loan outlives
it, and that loan roots in `a`. `db`'s borrow ends when `pick_x` returns, so the later shared
read of `db` conflicts with nothing. Rust accepts the analogue with the natural annotation:

```rust
fn pick_x<'a>(a: &'a mut Data, b: &mut Data) -> &'a mut i32 { &mut a.x }
```

**The old engine's rule** is that a returned reference borrows *every* reference parameter —
sound, but over-strict, and it cannot be refined without knowing which parameter the body
actually returns.

**New engine.** Infers the source from the body (`bor_ret_borrow_mask`), so the loan is charged
to `a` alone and the read of `db` is accepted.

---

## How C-2 found a bug in the *checker* — the point of the exercise

Judging C-2 required knowing which parameter the return borrows from. The new engine was
**guessing** it from the signature ("the first mutable reference param"). That guess is not
merely imprecise, it is **unsound**:

| body | conflicting use | before |
|---|---|---|
| `return var a.x` | `mutate(var da)` | caught |
| `return var b.x` | `mutate(var db)` | **MISSED** |

The loan was charged to `a` while the real borrow was of `b`, so every conflict on `b` went
unreported. Fixed by rooting each returned reference to its parameter (exact where provenance
is traceable; falls back to *all* reference params where it is not). Pinned by four cases in
`src/analysis/test_borrow.c` — isolation of each param, the branch union, and the opaque
fallback.

This is the argument for the whole exercise. Treating the corpus as ground truth would have
left both the corpus error *and* the checker unsoundness in place; the second was only visible
from inside the first. **And note the shape of it: Rust cannot infer this at all** — it has no
whole-program view across the signature, so it rejects the ambiguous signature outright
(*"missing lifetime specifier"*). Inferring the mask is not catching up to Rust here; it is
doing something Rust declines to do.

---

## Switchover procedure (Stage 3.5)

1. Invert each entry: rename `*_fail.ln` → `*_pass.ln`, drop the `// EXPECT:` line, add a
   comment pointing at the entry here that justifies it.
2. Move the corresponding lines out of `phase3_adjudications.txt` — once the new engine is
   authoritative they are not divergences, they are the behaviour.
3. For C-1, keep `two_phase_borrow_pass.ln` alongside it: the pair documents that the
   nested-call and direct-read forms are now treated identically, which was the whole defect.

## Open question deferred to Stage V

Neither correction has an *executable* oracle proving the accepted program is memory-safe under
ASan across all inputs — C-1's demonstration is a hoisted equivalent, C-2's is an argument from
region lifetimes. When the fuzzers run against the new engine (`fuzz_borrow.py` extended to
generate returned-reference shapes), both should become fuzz-backed rather than argued.
