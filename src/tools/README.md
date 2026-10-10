# src/tools — programs built FROM the compiler, that are not the compiler

Every file here is a separate `main()`, compiled on its own with `-I src`. None of them is
reachable from `src/main.c`, and nothing in the compiler includes them.

They lived in `src/analysis/` and `src/ir/` next to the headers they exercise, which made it
impossible to tell what ships by looking: of nineteen files in `src/analysis/`, eight were
binaries. Keeping them here is the whole point — the compiler directories now contain only the
compiler.

| kind | files | what they are for |
|:---|:---|:---|
| `test_*.c` | vra, linearity, borrow, octagon, definite_init, place, ir | unit tests over a HAND-BUILT IR, so they reach the detection direction the corpus cannot — the front end rejects those programs before the analyses would run |
| `test_judge_format.c` | | the proof engine judge's text form (`src/judge/text.h`): canonical round trips, refusals with their reasons, one-edit mutants, a full region |
| `test_judge_gen.c` | | the judge's VC generator (`src/judge/gen.h`) over a HAND-BUILT IR: the rules no corpus program exercises, and every VC canonical |
| `judge_vc_driver.c` | | the generator's VCs for a Lain file beside VRA's verdicts (`--smt`, `--stats`, `--why`), with claims at forward joins (hulls) and loop headers (invariants) proposed by the untrusted solver and VRA's header states, filtered by Houdini, and owed as `hull`, `inv.entry` and `inv.keep` VCs (`--no-claims` proposes none); `--check-hyps` runs the program on the interpreter and evaluates every hypothesis where it is used, and each claim on every edge taken into its block (`scripts/gates/judge_gate.sh`) |
| `solve_vc.c` | | the proof engine's untrusted solver (`src/solver/lia.h`) on VC texts: a certificate, a model or a refusal per VC, each answer round-tripped through the judge's parser; `scripts/gates/solver_harness.py` checks the answers with its own kernel and brute force |
| `*_driver.c` | vra, linearity, effects, incomplete, lower | drive one analysis over real source and print what it concluded; the surveys and several fuzzers are built on them |
| `fuzz_vra.c` | | the in-process VRA fuzzer |

Build: `gcc -std=c99 -o /tmp/<name> src/tools/<name>.c -I src`
Run the unit tests: `bash scripts/gates/run_ir_tests.sh`
