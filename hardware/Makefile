# Rendering and checking the KiCad plan.
#
#   make -C hardware help      list the targets
#   make -C hardware all       regenerate the schematic, check it, render it
#
# Requires `kicad-cli` on PATH (it ships inside the KiCad AppImage - see README.md).

KICAD_CLI ?= kicad-cli
SCH       := car.kicad_sch
PDFTOPPM  ?= pdftoppm

.PHONY: help all sch erc pdf svg png netlist bom check clean

help:
	@echo "make sch       regenerate car.kicad_sch / .kicad_sym / wiring.md from gen_schematic.py"
	@echo "make erc       electrical rule check -> erc.rpt (and print it)"
	@echo "make pdf       A2 schematic -> car.pdf"
	@echo "make svg       A2 schematic -> svg/"
	@echo "make png       raster preview -> car.png (via the PDF; kicad-cli has no raster output)"
	@echo "make netlist   netlist -> car.net (kicad s-expr) and car.xml"
	@echo "make bom       bill of materials -> car.csv"
	@echo "make all       sch erc pdf svg"
	@echo "make clean     remove everything this file generates"

all: sch erc pdf svg

sch:
	python3 gen_schematic.py

erc:
	$(KICAD_CLI) sch erc --severity-all --output erc.rpt $(SCH)
	@echo "--- erc.rpt ---"
	@cat erc.rpt

pdf:
	$(KICAD_CLI) sch export pdf --output car.pdf $(SCH)

svg:
	mkdir -p svg
	$(KICAD_CLI) sch export svg --output svg $(SCH)

# kicad-cli can plot SVG/PDF/PS/DXF for schematics but not raster images,
# so go through the PDF and let poppler do the last step.
png: pdf
	$(PDFTOPPM) -png -r 150 -singlefile car.pdf car

netlist:
	$(KICAD_CLI) sch export netlist --format kicadsexpr --output car.net $(SCH)
	$(KICAD_CLI) sch export netlist --format kicadxml --output car.xml $(SCH)

bom:
	$(KICAD_CLI) sch export bom --output car.csv $(SCH)

clean:
	rm -rf svg car.net car.xml car.csv car.png erc.rpt
