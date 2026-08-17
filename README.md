# usbbog-realidad-mixta

Proyecto de control de un carro (robótica móvil) con Arduino / ESP32 y su respectivo informe en LaTeX.

## Estructura del repositorio

```
.
├── firmware/                  # Código para Arduino / ESP32
│   ├── car_control/           # Sketch principal (car_control.ino)
│   └── README.md              # Instrucciones de hardware y carga del código
├── report/                    # Informe en LaTeX
│   ├── main.tex               # Archivo principal del documento
│   ├── chapters/              # Capítulos del informe
│   ├── figures/               # Imágenes y diagramas
│   ├── references.bib         # Bibliografía (BibTeX)
│   └── README.md              # Cómo compilar el informe
└── README.md                  # Este archivo
```

## Requisitos

- **Firmware**: [Arduino IDE](https://www.arduino.cc/en/software) o [PlatformIO](https://platformio.org/).
- **Informe**: Una distribución de TeX (TeX Live / MiKTeX) y `latexmk` o `pdflatex`.
