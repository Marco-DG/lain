# Lain — build the compiler, and check that it still deserves its claims.
#
#   make           build ./lain
#   make test      the corpus
#   make gates     every gate: corpus, README examples, spec, IR units, warnings
#   make fuzz      the fuzzers (slow; they execute what the compiler proved)
#   make figures   regenerate the README figures from the compiler's own dumps
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra
SRC      = src/frontends/lain/main.c
BIN      = lain

.PHONY: all test gates fuzz figures clean

all: $(BIN)

$(BIN): $(shell find src -name '*.h' -o -name '*.c')
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) -I src

test: $(BIN)
	bash scripts/gates/run_tests.sh

# Every gate that FAILS on error. Five of these were not run by this target: the sovereignty
# gate had been red for weeks because a diagnostic code changed under it and nobody looked,
# and run_trust — the harness that EXECUTES accepted programs against .expected oracles — was
# not here either, though it is the one that catches data corruption an exit-code suite cannot.
gates: $(BIN)
	bash scripts/gates/run_tests.sh
	bash scripts/gates/run_trust.sh
	bash scripts/gates/readme_gate.sh
	bash scripts/gates/spec_gate.sh
	bash scripts/gates/run_ir_tests.sh
	bash scripts/gates/cmin_gate.sh
	bash scripts/gates/check_build_warnings.sh
	bash scripts/gates/baseline_gate.sh
	bash scripts/gates/bench_gate.sh

# ── THE INSTRUMENTS AFTER THE DELETIONS, 2026-09-23 ──────────────────────────────────────
# Two engines and two backends became one of each, and SEVEN differentials went with them:
#   emit_gate, annot_gate, backend_corpus, layout_gate   (compared the two BACKENDS)
#   phase3_differential, engine_ir_gate, reverse_differential (compared the two ENGINES)
#
# They were not noise. Between them they found the niche-packing regression (D-62: 29 sums
# silently unpacked while ten gates stayed green), the slice-niche broken-C hole (D-63), the
# nine codegen defects the backend flip surfaced, and every adjudicated engine divergence for
# a month. Deleting the reference implementations would normally delete that capability — so
# the capability was preserved FIRST and the implementations removed after.
#
# ★ WHAT REPLACED THEM. `baseline_gate` (tests/BASELINE.txt) records, per corpus program, its
# output, its exit code, and HOW EVERY TYPE IT DECLARES IS REPRESENTED — captured from the AST
# emitter before it was deleted, verified against the IR backend, then re-recorded from the
# backend we keep. It still asks the question that caught D-62, the one no behavioural
# instrument can ask. And the corpus itself is now the engine differential: every one of its
# 728 programs is judged by the sovereign engine, with no `--engine=legacy` pin left anywhere.
#
# ⚠ A differential whose two legs become the same thing does not fail — it PASSES, loudly and
# meaninglessly (D-60: backend_corpus comparing the IR backend to itself and reporting "423
# agree"). That is why these were deleted rather than left running against a retired flag.
#
# PROGRESS MEASURES, not gates: their answer is a DISTANCE rather than a verdict. The exit
# status is absorbed HERE, where the target's contract is "print the distance", and left
# intact in the scripts, where "are we there yet" is still a useful question.
measure: $(BIN)
	@echo "no distances left to report: the engine and backend differentials closed"
	@echo "and were retired with the implementations they compared against."

fuzz: $(BIN)
	@for f in scripts/fuzz/fuzz_*.sh; do echo "== $$f"; bash $$f || exit 1; done

figures: $(BIN)
	bash assets/figures/make_figures.sh

clean:
	rm -f $(BIN) out.c lain.h *.o *.dot
	rm -f arena_list_out lain_dbg lain_new lexer_test out_test ownership \
	      test_destruct test_main unsafe_nested unsafe_valid
	rm -rf out test_out fuzz_bugs
	find tests -name out -o -name out.c -o -name lain.h | xargs -r rm -rf
