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

clean:
	$(LATEXMK) -c $(TEX) 2>/dev/null || true
	rm -f *.aux *.log *.toc *.out *.idx *.ind *.ilg *.lot *.lof \
	      *.fls *.fdb_latexmk *.synctex.gz *.bbl *.blg

distclean: clean
	rm -f $(PDF)

view: $(PDF)
	xdg-open $(PDF) 2>/dev/null || open $(PDF) 2>/dev/null || evince $(PDF)
