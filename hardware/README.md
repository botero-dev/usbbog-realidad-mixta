# Hardware — KiCad plan

The KiCad version of [`../docs/circuit-plan.md`](../docs/circuit-plan.md). Open
`car.kicad_pro` (or just `car.kicad_sch`) in KiCad 7 or newer.

| File | What it is | Owner |
| --- | --- | --- |
| `gen_schematic.py` | **the source of truth** — regenerates everything below | you |
| `Makefile` | one-command render/check targets | you |
| `car.kicad_pro` | minimal project file — open this | generated |
| `car.kicad_sch` | the schematic, **A3 sheet** (landscape, drawn tight) | generated |
| `car.kicad_sym` | the `car` symbol library (module symbols) | generated |
| `sym-lib-table` | library table — **merged**, extra entries survive | generated + you |
| `wiring.md` | net-by-net connection table, for the bench | generated |
| `car.pdf` | rendered schematic, for reading without KiCad | generated |
| `library/`, `fp-lib-table`, `design-block-lib-table` | your imported vendor parts | you |

**Generated files say so in their title block.** Edit the Python, not the KiCad
files — `car.kicad_sch` is overwritten wholesale on every run. `sym-lib-table` is
the one exception: it is *merged*, so libraries you add in the KiCad GUI are kept.

## Rendering from the command line

`kicad-cli` does all of it. It ships inside the KiCad AppImage, which on this
machine cannot self-mount, so it has been extracted once to a persistent location
and symlinked onto the PATH:

```bash
kicad-cli --version        # 10.0.7   (~/bin/kicad-cli -> ~/.local/opt/kicad-10.0.7/usr/bin/kicad-cli)
```

Then just use `make`:

```bash
make -C hardware help      # list the targets
make -C hardware all       # regenerate + ERC + PDF + SVG
make -C hardware png       # raster preview -> car.png
make -C hardware netlist   # car.net (kicad s-expr) and car.xml
make -C hardware bom       # car.csv
```

Or call the CLI directly — useful flags are `--black-and-white`,
`--exclude-drawing-sheet` (drops the border and title block, handy for a figure),
`--theme THEME`, `--pages 1`, and `-D key=value` for project variables:

```bash
kicad-cli sch export svg -o out/ --exclude-drawing-sheet car.kicad_sch
kicad-cli sch export pdf -o car.pdf --black-and-white car.kicad_sch
kicad-cli sch erc --severity-all -o erc.rpt car.kicad_sch
```

**Schematics have no raster output.** `sch export` offers `svg pdf ps dxf hpgl
netlist bom python-bom` — for a PNG, plot the PDF or SVG and rasterise that
(`pdftoppm -png -r 150`, `rsvg-convert`, or Inkscape). Also available:
`kicad-cli sym` (symbol libraries), `kicad-cli fp` (footprints), `kicad-cli
jobset` (run a saved job set), and on the PCB side `pcb export svg|pdf|gerbers|drill|pos|step|stl|glb|vrml` plus `pcb render` for raytraced board images.

**Status: 0 ERC violations** (KiCad 10.0.7, `Errors 0  Warnings 0`), 26 symbols,
45 nets, 31 deliberate no-connects.

## The vendor library — removed

`library/ROB-14450/` still holds an imported vendor part (symbol, footprint, STEP
model), and `fp-lib-table` still points at its footprint. The **`puente` symbol
entry was removed from `sym-lib-table`** — it existed only to get the ROB-14450
pinout into the schematic, which `car:ROB-14450` now does properly.

Don't swap one for the other: the vendor symbol numbers its pins `1`…`16`, while
`car:ROB-14450` uses `JP1-1`…`JP2-8` to match the board's silkscreen. Assigning the
vendor footprint to this symbol would need a pin-number remap, or the netlist will
not line up with the pads. `JP1-1` is GND and `JP2-1` is PWMA — a bare `1` on both
would short them.

---

## What it is, and what it is not

It is a **connection plan**, not a PCB. Every module is drawn as one rectangle with
one pin per signal, laid out in labelled bands — MCU blocks, motor drive, glue,
audio, power, mechanical. It is meant to be read, checked against the wiring table,
printed, and included in the report.

It is **not** a board you can send to a fab. There are no footprints, no footprints
assignments and no layout. If you later want a PCB, the netlist is already correct
and every symbol can be given a footprint.

Also not drawn, because they are not connections:

* **Decoupling capacitors** — the two extra the plan calls for: **470 µF bulk on
  each ROB-14450's `VM`**, and **1000 µF at the ESP32 board's 5 V pin** (the LM2596
  cannot answer a WiFi TX burst without it, §7.1). The ROB-14450 already carries
  0.1 µF + 10 µF on `VM` and 0.1 µF on `VCC`, so those are not repeated here, and
  both buck modules have their own in/out capacitors.
* **Setting the regulators.** Both buck modules ship with the pot anywhere, and can
  come up at 30 V. Set and measure them on the bench before wiring anything.
* **Ferrite beads** on the motor leads, twisted pairs, the star-ground topology.
  A schematic cannot express any of these; §7 and §8 of the plan do.
* **The I²S series resistors.** The PCM5102A board carries its own, in line with
  `BCK`, `DIN` and `LRCK` — so the schematic wires all three straight through and
  there is nothing to add. (If your DAC board has none, fit ~100 Ω at the DAC end.)
* **The DAC's strapping bridges.** `SCK`, `FLT`, `DEMP`, `FMT` and `XSMT` are solder
  pads on the DAC board itself and are **already bridged** (`XSMT`→3V3, the rest→GND),
  so they are drawn as no-connects.
* **The DAC runs off 5 V.** Its board has its own 3.3 V regulator, so `VIN` → `+5V` and
  `A3V3` (the regulator's *output*) is left open — **not** the same net as `VIN`, and
  never to be strapped to it (the DAC's supply maximum is 3.6 V). Six of that board's
  fifteen pads get a wire: `BCK`, `DIN`, `LCK`, `GND`, `VIN`, `LOUT`.
* **The WS2812 level shifter** — **required** for a 5 V strip: its `Vih` is `0.7 × VDD`,
  i.e. **3.5 V**, so the S3's 3.3 V data line is below spec. Fit one between
  `LED_STRIP_DIN` and the strip's first pixel (74AHCT125 / 74HCT245 — HCT inputs are
  TTL-threshold, so they take 3.3 V in and drive 5 V out); modelled as a direct wire here.
* **The battery voltage divider** (optional, still an open item in the plan).
* **The camera.** The OV3660 is on the board's own FPC connector — no wiring.

---

## Conventions

**Both MCU symbols are drawn as the physical board**, so you can hold the board next
to the screen and count pins.

| Symbol | Drawn as | Pin numbers |
| --- | --- | --- |
| `U1` ESP32-S3-CAM | the Freenove 2×20 header: left row down the left, right row down the right, with the board's own silkscreen names (`CAM_SIOD`, `U0TXD`, `WS2812`…) | `L1`–`L20`, `R1`–`R20` |
| `U2` Pico | the real 40-pin header: `1` at top-left, numbering down the left then **up** the right — so the right column reads `40` at the top, `21` at the bottom | `1`–`40` |
| `U3`, `U4` ROB-14450 | outputs on one side, inputs on the other, as the board is laid out | `JP1-1`…`JP1-8`, `JP2-1`…`JP2-8` |
| `U6` PCM5102A | two headers: `1`–`6` down the left (`SCK`…`VIN`), `7`–`15` down the right (`FLT`…`LOUT`) | `1`–`15` |

Both sets use a prefix because the two headers on one board each have a position
`1`: `JP1-1` is GND and `JP2-1` is PWMA, and the ESP32's `L1` is 3V3 while `R1` is
GPIO43. Without the prefix KiCad would treat the pair as one net and short them.
The ROB-14450 order comes from SparkFun's Eagle source rather than a photo —
`docs/circuit-plan.md` §3.3 has the table.

So `U1.R3` is **GPIO1** and `U2.16` is **GP12**. The GPIO or signal name is always
the pin *name* on the symbol, so there is nothing to memorise — but the layout
means the schematic doubles as a picture of the physical connectors. Because the
Pico's 8 ground pins are separate pins, the schematic tells you to connect all of
them.

**Nets join by name.** Nothing is wired symbol-to-symbol; every pin gets a short
stub and a net label. Two labels with the same text are the same net. This is what
makes the sheet readable and the generator simple, and it means moving a block
around in KiCad cannot break connectivity.

**Series parts split a net.** That is deliberate, and it is the point of the
`GLUE` band: `S3_TXD` → `R1` → `LINK_RX_PICO` is one wire pair, so you can always
see which side of a resistor you are on.

**The UART series `1 kΩ` (`R1`, `R2`) is not for signal integrity.** It is
transparent at 921600 baud (1 kΩ against ~10–50 pF of pad and cable capacitance is
~10–50 ns of edge, versus a 1.085 µs bit). It is there for the case where **one
board is powered and the other is not**: every GPIO has a protection diode from the
pin up to its own `VDD` rail, so driving an unpowered board's RX pin forward-biases
that diode and feeds current into its rail — *phantom powering*, which can half-start
the "off" board. With 1 kΩ the injected current is capped at 3.3 V / 1 kΩ ≈ **3 mA**,
below what either chip needs to run, so it cannot bring the board up. **One per line,
because either board can be the unpowered one.** In this build both boards share the
5 V rail, so it only matters on the bench — running one board alone, or one of them
browning out while the other is live.

> Do not confuse it with `R5`, which is also 1 kΩ and has nothing to do with any of
> this: it lets the optional E-stop switch short the `STBY` node to ground while
> sinking only 3.3 mA, instead of shorting a GPIO output directly.

**Spares are no-connects.** A pin with a no-connect flag is intentional. With both
MCU symbols drawn as the real boards this is now a visible map of what *not* to
touch: the 14 camera pins on the ESP32's left row, the dead PSRAM pins, BOOT, USB,
the strapping pins, and the unused breakout pins. The full list is at the bottom of
`wiring.md`.

**All pins are `passive`.** ERC therefore reports no power-driver conflicts — the
rails are named nets, not power symbols. Do not read "0 violations" as "ERC checked
my power design"; it checked the connectivity.

---

## Cross-check against the plan

The net table in `wiring.md` should match `circuit-plan.md` §3 and §4. Spot checks:

| Plan says | Schematic net |
| --- | --- |
| S3 `GPIO1` → R → Pico `GP17` | `S3_TXD` (`U1.R3`, GPIO1) → `R1` → `LINK_RX_PICO` (`U2.22`, GP17) |
| Pico `GP16` → R → S3 `GPIO2` | `PICO_TXD` (`U2.21`, GP16) → `R2` → `LINK_RX_S3` (`U1.R4`, GPIO2) |
| ~~S3 `GPIO43` / `GPIO44`~~ are **not** the link | `U1.R1` / `U1.R2` (`U0TXD` / `U0RXD`) are **deliberate no-connects**. On this carrier the USB-C port's bridge is wired to exactly these pins, so they are the serial console; the link moved off them on 2026-10-11 (plan §2.3). Cost of the move: `GPIO1`/`GPIO2` were `ADC1_CH0`/`CH1`, so the S3 has no spare analog inputs |
| BMI160 on `GPIO42` (`SCL`) / `GPIO41` (`SDA`), addr **0x69** | **Four wires only**: `+5V` → `U5.VIN`, GND, `I2C_SDA` → `U5.SDA`, `I2C_SCL` → `U5.SCL`, from `U1.R6` / `U1.R5`. `U5.CS` and `U5.SA0` are **left open** — the module pulls both high, so `CS` gives I²C mode and the address is **0x69**, not 0x68 |
| `I²S` `45`/`47`/`21` — signals permuted 2026-10-11 | `I2S_BCK` (from `U1.R16` / `GPIO47`) → `U6.2`, `I2S_DIN` (`U1.R17` / `GPIO21`) → `U6.3`, `I2S_LRCK` (`U1.R14` / `GPIO45`) → `U6.4` — all **direct**, no external series parts. Header order maps to pads 4/2/3, so the ribbon **crosses** |
| `STBY` on `GP28` via 1 kΩ | `STBY_PICO` (`U2.34`) → `R5` → `MOT_STBY` → `U3.JP2-4`, `U4.JP2-4`, `SW1.1` |
| Strip on `48`; `45` carries `LRCK` | `S3_LED` → `R6` → `LED_STRIP_DIN`; `U1.R15` (`GPIO48`) is the strip **and** the on-board WS2812, same net. `U1.R14` (`GPIO45`) carries `I2S_LRCK` — `45` is the `VDD_SPI` strap |
| ROB-14450 `VCC` from the Pico's 3V3 | `+3V3_PICO` → `U3.JP1-7`, `U4.JP1-7` |
| Pico on 5 V through a 0 Ω link | `+5V` (LM2596) → `R7` → `VSYS` → `U2.39` — **the Schottky is gone**, so either USB port powers both boards (§7.2 of the plan) |
| Motor rail is separate and regulated | `VBAT` → `PS2` (LM2596) → `+5V`; `VBAT` → `PS1` (XL4015, **8.0 V**, set for 1 A/motor stall) → `VM_MOTOR` → `U3.JP1-8`, `U4.JP1-8` |

If a row here disagrees with `wiring.md`, the plan document is wrong and the
schematic is right — the schematic is machine-checked, the prose is not.

---

## Before you trust it

Verified: KiCad parses it, ERC is clean, and the netlist was independently
re-derived from the file and compared net-by-net against `circuit-plan.md`.

Not verified: **no pin here has been confirmed against a physical board.** The
ESP32 header positions come from the Freenove pinout image, the Pico numbering from
the RP2040 datasheet's Figure 4, and the footprint-free part of the plan has never
been built. Meter-continuity check the two module pinouts before you solder
anything — that is step 1 of the bring-up order in `circuit-plan.md` §11.
