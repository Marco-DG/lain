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

## C-3 · unconsumed owned values — the old engine is **inconsistent**, side not yet settled

Not an inversion (yet); recorded because the corpus demands *both* answers for the same
program shape, so at least one of these tests is wrong and the pair must be settled together.

| corpus test | shape | old engine |
|---|---|---|
| `tests/ownership/borrow_pass.ln` | `var fc = take_counter(mov cnt)`, never consumed | **accept** |
| `tests/ownership/rich_diagnostics_fail.ln` | `var r = Resource(42)`, never consumed | **E003** |

Both bind an owned struct of one `i32` and never consume it. Reduced to a minimal pair and
run through the old engine directly:

```lain
type R { id i32 }
var r = R(42)                    // → E003
var r = make(mov seed)           // → ACCEPT     (same type, same non-consumption)
```

The discriminator is not the value, it is **how the local was initialised**: a constructor
gives the local `MODE_OWNED`, a call result does not. Ownership there is an artifact of
inference, not a property of the binding — the "facts keyed by name, no canonical type"
incoherence, surfacing in the linear checker.

**Position taken.** The new engine reports a leak only where a resource would actually be
lost — `lin_has_release_obligation`: does the type transitively own an owned pointer or
slice ("non-trivial destructor" in C++, `impl Drop` in Rust). Under that rule `borrow_pass`
is right to accept and `rich_diagnostics_fail` over-rejects. This keeps the corpus green and
is strictly *more* capable than the rule it replaced: leak-tracking previously covered only
`ptr`/`slice` slots, so a **struct owning a heap pointer, never freed, was silently missed**
— a real leak with no corpus test over it, now caught by construction.

The deeper point is a separation the IR should keep: the IR's job is the FACT *"an owned
value dies unconsumed here."* Whether that is an **error** (strict linear discipline) or a
**site for an implicit drop** (Rust, C++, Swift) is front-end policy, and front ends
genuinely differ. Reporting only where a resource is lost is the intersection all of them
call a bug. Settling `consume_before_return_fail` and `rich_diagnostics_fail` means Lain
deciding its own policy — deferred to Stage V, when the language catches up to the IR.

---

## C-4 · `tests/memory/array_uninit_element_read_fail.ln` — fail ⇒ **pass**

```lain
var a i32[4]
a[0] = 5
return a[0]            // asserted [E005]; actually SAFE
```

The element being read is the element that was just written. The old engine rejects because
it has no per-element state and says so in the diagnostic itself: *"initialize it with a
whole-array value (a literal or comprehension) before reading elements."* That is a
statement about the checker, not about the program.

**New engine.** Tracks array elements at CONSTANT indices, so `a[0]` is initialised and the
read is accepted — while `var a i32[4]; return a[0]`, reading storage nothing ever wrote, is
now CAUGHT. The new engine had missed that entirely (arrays were untracked), so this change
closed a fail-open and an over-rejection at the same time, from opposite directions.

**Still fail-open, and worth stating plainly:** a store at an UNKNOWN index is treated as
initialising the whole array, so a loop that fills only half of one is not caught.
Comprehensions no longer depend on that approximation — lowering states `IR_INIT`, because a
comprehension fills every element by definition — but a hand-written partial fill does.
Proving such a loop total needs the numeric domain to show it covers `0..len`.

---

## C-5 · `tests/match/niche/linear_adt_payload_mov_fail.ln` — fail ⇒ **pass**

```lain
proc t(b mov Box) {                    // type Box { Full { ptr mov *u8 }, Empty }
    case b {
        Full(ptr): mem_free(mov ptr)   // asserted [E003]; actually SAFE
        Empty: noop()
    }
}
```

The `Full` arm releases the payload; the `Empty` arm owns nothing to release. Nothing leaks
on either path.

**The test authorises its own inversion.** Its header says the rejection survives only
because *"consuming the payload does not DISCHARGE the scrutinee"*, and states the condition
for flipping: *"Discharging correctly needs the payload binding to be linear in its own
right — per-field state, which the new IR pass has."* That is now built.

**The rule, and why the corpus's warning was right but is now satisfied.** The same header
warns — correctly — that consuming the scrutinee wholesale on a non-borrowed `case` is
**unsound**. It was: with the payload untracked, the resource simply vanished, and the rule
accepted both a leak and a double free. What changed is not the warning but its premise.
Destructuring now *moves* the resource out of the scrutinee **into the payload binding**,
which carries the obligation on exactly the path where it exists. The transfer is complete
rather than a hole, so the old rule's two counter-examples are both caught.

**Executed, not argued.** Each of the four was run through the new pass:

| program | expected | new engine |
|---|---|---|
| `Full(ptr): mem_free(mov ptr)` / `Empty: noop()` | accept | **clean** |
| `Full(ptr): noop()` | leak | **E003 on the payload** |
| `Full(ptr): dbl(mov ptr)` (frees twice) | double free | **E001** |
| `proc t(b mov Box) { }` — never destructured | leak | **E003 on the scrutinee** |
| `case b {...}` twice | use after move | **E001** |

The Empty arm reporting nothing is not a special case: the payload is projected only on the
`Full` block, so the created-liveness lattice gives the Empty path no obligation at all.
Per-variant reasoning falls out of the CFG rather than being written down.

---

---

## C-6 · the loop-carried ACCUMULATOR — the old engine's overflow check does not survive a loop

Not an inversion of one test: a verdict on a CLASS of roughly fifty, and the largest single
finding of the numeric triage.

```lain
func up8(n u8) u8 {
    var s u8 = 0
    var i u8 = 0
    while i < n { s = s + i  i = i + 1 }
    return s
}
up8(200)     // true sum of 0..199 = 19900
```

**The old compiler accepts this and the program prints 188.** Two lines, no corner case. The
same shape at i32 — `up(100000)` — prints 704982704 where the answer is 4999950000.

**The mechanism.** Its loop widening clamps the accumulator's range to the type it is stored
into, after which the store trivially "fits". The check is circular, so **no loop-carried
accumulator has ever actually been checked**. The old engine's E086 does fire on a straight-line
`var s i32 = a + b`; it is a loop that defeats it.

**New engine.** `s + i` is exact in the widened result type (Path-F: `+` widens, so an i32+i32
is an i33 and cannot overflow) and the obligation lives at the STORE back into the narrow cell.
Unbounded accumulator ⇒ refused. `tests/vra/overflow/loop_accumulator_fail.ln` pins it.

**What this costs, honestly.** ~50 corpus programs contain an unbounded accumulator while
testing something else entirely (loops, borrows, UFCS). Every one of them is a real overflow
and would need a guard, a wider type, or `+%`. That is not a switchover bug — it is the
language's own promise arriving, and the corpus predating it.

**And the precision frontier it exposes, stated rather than papered over.** The GUARDED form
(`if s < 1000 { s = s + i }`) proves today, from the guard alone —
`loop_accumulator_bounded_pass.ln`. What does NOT prove is the naturally-bounded form: `s`
summing `i < 16` over at most 16 iterations reaches 120, and showing it needs the loop's TRIP
COUNT related to the accumulator's per-iteration growth. That is a PRODUCT, not a difference,
and outside an octagon. It is the sharpest open item on the numeric side.

## Switchover procedure (Stage 3.5)

1. Invert each entry: rename `*_fail.ln` → `*_pass.ln`, drop the `// EXPECT:` line, add a
   comment pointing at the entry here that justifies it.
2. Move the corresponding lines out of `phase3_adjudications.txt` — once the new engine is
   authoritative they are not divergences, they are the behaviour.
3. For C-1, keep `two_phase_borrow_pass.ln` alongside it: the pair documents that the
   nested-call and direct-read forms are now treated identically, which was the whole defect.
4. For C-5, add the four negatives above as fail-tests beside it. The inversion is only
   honest while they keep failing — the accepted program and the rejected ones are one rule.

## Open question deferred to Stage V

Neither correction has an *executable* oracle proving the accepted program is memory-safe under
ASan across all inputs — C-1's demonstration is a hoisted equivalent, C-2's is an argument from
region lifetimes. When the fuzzers run against the new engine (`fuzz_borrow.py` extended to
generate returned-reference shapes), both should become fuzz-backed rather than argued.
