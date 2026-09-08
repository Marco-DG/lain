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

gates: $(BIN)
	bash scripts/gates/run_tests.sh
	bash scripts/gates/readme_gate.sh
	bash scripts/gates/spec_gate.sh
	bash scripts/gates/run_ir_tests.sh
	bash scripts/gates/check_build_warnings.sh

fuzz: $(BIN)
	@for f in scripts/fuzz/fuzz_*.sh; do echo "== $$f"; bash $$f || exit 1; done

figures: $(BIN)
	bash assets/figures/make_figures.sh

clean:
	rm -f $(BIN) out.c
