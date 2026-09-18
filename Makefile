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
SRC      = src/main.c
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
	bash scripts/gates/phase3_differential.sh
	bash scripts/gates/check_build_warnings.sh

# PROGRESS MEASURES, not gates: their answer is a DISTANCE rather than a verdict. Putting them
# in `gates` would add noise and teach everyone to ignore it; leaving them unrun is how the
# emitter's four miscompiles went unnoticed.
#
# `annot_gate` is the third, and it exists because the second was being misread: emit_gate
# compares the two backends' BEHAVIOUR, so it cannot see that the new one emits none of the
# `nonnull`, `returns_nonnull` or `access(...)` the old one derives from proofs. Behaviourally
# identical, and it has thrown the proofs away. "emit_gate is at 399/0/0, so src/emit/ can go"
# was wrong for exactly that reason.
#
# ★ The comment here used to claim these "always exit 0 by design". They do not — each script
# ends on its own readiness predicate, so `emit_gate` exits 1 precisely when it has differences
# to report, and make then abandoned the target before `engine_ir_gate` ever ran. A progress
# meter that stops being printed exactly when it has something to say is worse than no meter,
# so the exit status is absorbed HERE, where the target's contract is "print both distances",
# and left intact in the scripts, where "are we there yet" is still a useful question to ask.
measure: $(BIN)
	-bash scripts/gates/emit_gate.sh
	-bash scripts/gates/annot_gate.sh
	-bash scripts/gates/engine_ir_gate.sh

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
