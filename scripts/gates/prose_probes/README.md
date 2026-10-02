# Prose probes — making documentation prose testable

`readme_gate` compiles every ```lain block in README.md and LANGUAGE.md. `spec_gate` does the
same for the spec and checks Annex B against the compiler. Between them, **every example is
checked and no sentence is.**

That is where the defects are. In two days, every false claim found in these documents was in
prose that carried no example:

| the claim | where | reality |
|:--|:--|:--|
| generics take a `comptime T type` parameter | LANGUAGE §20 | `[E100] Expected parameter name` |
| a generic type is a `func` returning `type` | LANGUAGE §20 | `[E100]`; it is `type Option(T type)` |
| `fun` is an alias for `func` | LANGUAGE §1.1 | it is not, and is not even reserved |
| `undefined` is a keyword | LANGUAGE §1.1 | it is an ordinary identifier |
| `macro`/`expr`/`pre`/`post`/`end`/`export` are reserved | LANGUAGE §1.1 | only `use` is |
| a chained branch is `elif` | LANGUAGE §6.1 | it is `else if`; `elif` does not parse |
| "each error includes source-line context with a caret" | LANGUAGE §12 | it was true of some errors only (now of all, since `5a1f543`) |
| module paths resolve relative to the source file | README §10 | they resolve relative to the **working directory** |
| an `if`/`while` condition "shall have type bool" | spec §9 ×2 | a bool, an integer, or a narrowing optional |
| E013 is issued for "a `main` declared as `func`" | Annex B | that rule died with `proc` |

Nine of those ten sat inside documents whose *examples* were green at the time.

## What a probe is

One `.ln` file per claim. Its header states the claim and where the claim lives, so a failure
tells you which sentence to fix rather than only that something broke:

    // CLAIM: LANGUAGE.md §20 — a type parameter is written `T type`, with no keyword.
    // EXPECT: COMPILES

    func identity(T type, x T) T { return x }
    func main() i32 { return identity(i32, 7) }

`EXPECT:` is either `COMPILES` or a diagnostic code (`[E100]`). A probe asserting a refusal must
name the code, so that a program refused for an unrelated reason is not mistaken for a passing
probe — the failure mode that makes a green suite worthless.

## Running

    bash scripts/gates/prose_gate.sh

It needs a compiler no older than `src/`, like the gates, and refuses otherwise.

## Probing the SPECIFICATION's constraints (p16–p26)

The probes above came from false claims. These came from the opposite direction, and the difference
is worth recording because it changes what a green suite means.

The spec's chapters were measured for `\begin{constraint}` blocks against `laincode` examples, to
find where normative rules sit with nothing to check them. **Chapter 8, expressions, was the
outlier: 23 constraints and 3 examples.** So each of the 23 was probed.

**All 23 held.** The ratio predicted where examples were missing, not where the compiler was wrong —
which is the honest result and the reason to record it: an unexampled rule is *unchecked*, not
presumed false, and a measurement that looks like a risk ranking is only a ranking of visibility.

Eleven became permanent probes, chosen for what a future change could plausibly break rather than
for being hard to verify once:

- `%` on floats, `==` on a struct / array / enum, a struct-to-integer cast (the type rules that a
  new coercion path would quietly widen);
- **`idx in arr` covering `arr[idx]` EXACTLY** — p20 is the one to keep if only one survives, because
  a guard accidentally widened to `arr[idx + k]` is *unsound*, not merely imprecise;
- a field write needing a mutable lvalue; a reversed literal sub-slice; the call-site mode matching
  the parameter; the argument count;
- **an optional as a condition** — the third arm of "bool, or an integer, or an optional that
  narrows", which no other document mentions anywhere, so p26 is its only check in the project.

Each was confirmed to be refused at the intended line with a message naming the intended check, not
merely refused: a probe that fails for an unrelated reason is a probe that tests nothing, and the
code alone does not distinguish the two.

## The limit, stated

A probe can only test a claim that a program can falsify. "The IR is the sole analysis authority"
is true, load-bearing, and unprobeable. This instrument shrinks the unchecked surface; it does
not eliminate it, and a sentence with no probe beside it is still a sentence nobody is testing.
