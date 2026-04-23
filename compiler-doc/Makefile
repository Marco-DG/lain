MAIN    = lain-compiler-doc
LATEX   = pdflatex
FLAGS   = -interaction=nonstopmode -halt-on-error
LATEXMK = latexmk

.PHONY: all clean distclean view

all:
	@if command -v $(LATEXMK) > /dev/null 2>&1; then \
	  $(LATEXMK) -pdf -pdflatex="$(LATEX) $(FLAGS)" $(MAIN).tex; \
	else \
	  $(LATEX) $(FLAGS) $(MAIN).tex; \
	  makeindex $(MAIN).idx 2>/dev/null || true; \
	  $(LATEX) $(FLAGS) $(MAIN).tex; \
	  $(LATEX) $(FLAGS) $(MAIN).tex; \
	fi

view: all
	@xdg-open $(MAIN).pdf 2>/dev/null || open $(MAIN).pdf 2>/dev/null || true

clean:
	rm -f *.aux *.log *.toc *.out *.idx *.ind *.ilg *.bbl *.blg \
	      *.fls *.fdb_latexmk *.synctex.gz *.lof *.lot \
	      chapters/*.aux annexes/*.aux

distclean: clean
	rm -f $(MAIN).pdf
