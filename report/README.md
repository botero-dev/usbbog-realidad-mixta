# Informe en LaTeX

Informe del proyecto en LaTeX con formato **IEEE** (clase `IEEEtran`).

> La clase `IEEEtran.cls` y el estilo de bibliografía `IEEEtran.bst` están
> incluidos en esta carpeta, por lo que no es necesario instalar paquetes
> adicionales del sistema.

## Estructura

- `main.tex`: archivo principal (compilar este).
- `IEEEtran.cls`: clase del documento (IEEE).
- `IEEEtran.bst`: estilo de bibliografía (IEEE).
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
