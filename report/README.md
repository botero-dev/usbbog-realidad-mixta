# Informe en LaTeX

Informe del proyecto en LaTeX.

## Estructura

- `main.tex`: archivo principal (compilar este).
- `chapters/`: capítulos del documento.
- `figures/`: imágenes y diagramas.
- `references.bib`: bibliografía.

## Compilación

Con `latexmk` (recomendado):

```bash
cd report
latexmk -pdf main.tex
```

O manualmente con `pdflatex` + `bibtex`:

```bash
cd report
pdflatex main.tex
bibtex main
pdflatex main.tex
pdflatex main.tex
```
