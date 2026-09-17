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
| `*_driver.c` | vra, linearity, effects, incomplete, lower | drive one analysis over real source and print what it concluded; the surveys and several fuzzers are built on them |
| `fuzz_vra.c` | | the in-process VRA fuzzer |

Build: `gcc -std=c99 -o /tmp/<name> src/tools/<name>.c -I src`
Run the unit tests: `bash scripts/gates/run_ir_tests.sh`
