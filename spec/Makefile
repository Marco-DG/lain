# Makefile — Lain Language Specification
#
# Targets:
#   make          — build PDF (default)
#   make clean    — remove generated files
#   make distclean — remove PDF too
#   make view     — build and open PDF

MAIN    := lain-spec
TEX     := $(MAIN).tex
PDF     := $(MAIN).pdf
LATEXMK := latexmk
PDFLATEX:= pdflatex

.PHONY: all clean distclean view

all: $(PDF)

$(PDF): $(TEX) lain-spec.sty grammar.tex \
        $(wildcard chapters/*.tex) \
        $(wildcard annexes/*.tex) \
        $(wildcard figures/*.tikz)
	$(LATEXMK) -pdf -interaction=nonstopmode -halt-on-error $(TEX) || \
	  { $(PDFLATEX) -interaction=nonstopmode $(TEX) && \
	    $(PDFLATEX) -interaction=nonstopmode $(TEX); }
# The fallback runs pdflatex WITHOUT -halt-on-error, which is what makes multi-pass builds
# work — and also what let two LaTeX errors sit in the sources unnoticed: a PDF came out, so
# nothing complained. A build that produced errors is not a build.
	@if grep -qE '^! ' $(MAIN).log; then \
	  echo "=== LaTeX errors (the PDF may be stale or wrong) ==="; \
	  grep -E '^! ' -A3 $(MAIN).log | head -40; \
	  exit 1; \
	fi

clean:
	$(LATEXMK) -c $(TEX) 2>/dev/null || true
	rm -f *.aux *.log *.toc *.out *.idx *.ind *.ilg *.lot *.lof \
	      *.fls *.fdb_latexmk *.synctex.gz *.bbl *.blg

distclean: clean
	rm -f $(PDF)

view: $(PDF)
	xdg-open $(PDF) 2>/dev/null || open $(PDF) 2>/dev/null || evince $(PDF)
