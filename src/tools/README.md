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
| `judge_vc_driver.c` | | the generator's VCs for a Lain file beside VRA's verdicts (`--smt`, `--stats`, `--why`), with claims at forward joins (hulls), loop headers (invariants) and returns (callee contracts) proposed by the untrusted finder (`src/solver/finder.h`) from the solver's bounds and VRA's header states and return intervals, filtered by Houdini over the module, and owed as `hull`, `inv.entry`, `inv.keep` and `post` VCs (`--no-claims` proposes none); `--check-hyps` runs the program on the interpreter and evaluates every hypothesis where it is used, and each claim on every edge taken into its block (`scripts/gates/judge_gate.sh`); `--witness` (PE.12) prints, for each loop the finder does not rank, the inputs the finder proposes as non-termination witnesses and what running each did, `HANG` only when the loop's header came back to a state it was in, and for each function on a recursion cycle the small inputs on which a call is made while the same call is still running (`tests/ir/judge_witness.sh`) |
| `test_judge_kernel.c` | | the judge's KERNEL (`src/judge/kernel.h`) against good and bad certificates: each rule it checks, by a certificate it must accept and one it must refuse with that rule's reason |
| `judge_check.c` | | the judge's verdict on `solve_vc`'s answers: every certificate through the kernel, one line per VC (`implied`, `REFUSED`, `not-implied`, `budget`, `capacity`); `scripts/gates/judge_gate.sh` counts only what the kernel accepts |
| `solve_vc.c` | | the proof engine's untrusted solver (`src/solver/lia.h`) on VC texts: a certificate, a model or a refusal per VC, each answer round-tripped through the judge's parser; `scripts/gates/solver_harness.py` checks the answers with its own kernel and brute force |
| `test_rank.c` | | the untrusted ranking-function synthesis (`src/solver/rank.h`) on hand-built loops: a ranking function found is checked by brute force over a box, and a loop with none must say so with a certificate |
| `*_driver.c` | vra, linearity, effects, incomplete, lower | drive one analysis over real source and print what it concluded; the surveys and several fuzzers are built on them |
| `drv_lower.h` | | how every `*_driver.c` lowers a module: static evaluation first, then `ir_lower_module`, as `main.c` does, so a driver analyses the IR the compiler emits |
| `fuzz_vra.c` | | the in-process VRA fuzzer |

Build: `gcc -std=c99 -o /tmp/<name> src/tools/<name>.c -I src`
Run the unit tests: `bash scripts/gates/run_ir_tests.sh`
