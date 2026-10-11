#!/usr/bin/env python3
"""
Generate hardware/car.kicad_sch for the mixed-reality RC car.

Architecture (see docs/circuit-plan.md):
    ESP32-S3-CAM  = brain: WiFi, OV2640 camera, I2S audio, BMI160, control loop
    RP2040 Pico   = dumb PWM splitter: 4 x 20 kHz PWM + 8 direction lines + STBY

Design notes
------------
* Every pin gets a short stub plus a net label. Nothing is wired symbol-to-symbol,
  so the sheet reads as a labelled wiring plan and the connectivity is unambiguous
  (and regenerate-able). One label name == one net.
* Pin numbers on U1/U2 are physical header positions, not GPIO numbers:
      U1 ESP32-S3-CAM   right row 1-20, left row 21-40
      U2 Raspberry Pi Pico   the real 1-40 header order
  The GPIO / signal name is the pin *name*.
* Pins with no net => drawn as a no-connect flag (spares, straps, dead PSRAM).

Usage
-----
    python3 hardware/gen_schematic.py
    kicad-cli sch export pdf hardware/car.kicad_sch      # or open it in KiCad
"""

import os
import uuid
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "car.kicad_sch")

LIB = "car"
PROJ = "car"
ROOT = str(uuid.uuid4())
FS = 1.27          # default font size (mm)
PINLEN = 2.54      # pin length (mm)
STUB = 5.08        # stub wire length (mm)
GRID = 1.27        # KiCad connection grid (mm) - everything must land on it


def snap(v):
    """Snap a coordinate onto the 1.27 mm connection grid."""
    return round(round(v / GRID) * GRID, 4)


def uid():
    return str(uuid.uuid4())


def eff(size=FS, justify=None, hide=False):
    s = "(effects (font (size %s %s))" % (size, size)
    if justify:
        s += " (justify %s)" % justify
    if hide:
        s += " hide"
    return s + ")"


# --------------------------------------------------------------------------- #
# symbol library
# --------------------------------------------------------------------------- #

def lib_symbol(name, left, right, halfw, refpre="U", halfh=None, prefix=LIB,
               pitch=2.54):
    """Return (sexp_text, geom, halfh).

    left / right : list of (pin_number, pin_name)
    geom         : {pin_number: (x, y, 'L'|'R')} in symbol coordinates
    prefix       : library nickname for the schematic's embedded cache; pass ""
                   when writing a standalone .kicad_sym library file
    pitch        : pin spacing; must stay a multiple of GRID or ERC complains
    """
    n = max(len(left), len(right))
    if halfh is None:
        halfh = round(snap((n - 1) * pitch / 2.0 + 2.54), 4)

    full = ("%s:%s" % (prefix, name)) if prefix else name
    o = []
    o.append('    (symbol "%s" (pin_names (offset 1.016)) (in_bom yes) (on_board yes)'
             % full)
    o.append('      (property "Reference" "%s" (at %s %s 0) %s)'
             % (refpre, -halfw, round(halfh + 2.54, 4), eff()))
    o.append('      (property "Value" "%s" (at %s %s 0) %s)'
             % (name, -halfw, round(-(halfh + 2.54), 4), eff()))
    o.append('      (property "Footprint" "" (at 0 0 0) %s)' % eff(hide=True))
    o.append('      (property "Datasheet" "~" (at 0 0 0) %s)' % eff(hide=True))
    o.append('      (symbol "%s_0_1"' % name)
    o.append('        (rectangle (start %s %s) (end %s %s)'
             % (-halfw, halfh, halfw, -halfh))
    o.append('          (stroke (width 0.254) (type default)) (fill (type background)))')
    o.append('      )')
    o.append('      (symbol "%s_1_1"' % name)

    geom = {}
    for side, items in (("L", left), ("R", right)):
        count = len(items)
        for i, (num, pinname) in enumerate(items):
            y = round(snap(((count - 1) / 2.0 - i) * pitch), 4)
            if side == "L":
                x, ang = round(-(halfw + PINLEN), 4), 0
            else:
                x, ang = round(halfw + PINLEN, 4), 180
            o.append('        (pin passive line (at %s %s %s) (length %s)'
                     % (x, y, ang, PINLEN))
            o.append('          (name "%s" %s)' % (pinname, eff()))
            o.append('          (number "%s" %s)' % (num, eff()))
            o.append('        )')
            geom[num] = (x, y, side)

    o.append('      )')
    o.append('    )')
    return "\n".join(o), geom, halfh


# --------------------------------------------------------------------------- #
# the parts of the build
# --------------------------------------------------------------------------- #

# --- U1: Freenove ESP32-S3-WROOM carrier, 2x20 ------------------------------
# Drawn as the physical header: L1-L20 down the left row, R1-R20 down the right row,
# with the board's own silkscreen names. Pin numbers are the row position, the same
# convention as the ROB-14450's JP1-x / JP2-x.
ESP32_L = [
    ("L1",  "3V3"),
    ("L2",  "RST"),
    ("L3",  "GPIO4 CAM_SIOD"),
    ("L4",  "GPIO5 CAM_SIOC"),
    ("L5",  "GPIO6 CAM_VSYNC"),
    ("L6",  "GPIO7 CAM_HREF"),
    ("L7",  "GPIO15 CAM_XCLK"),
    ("L8",  "GPIO16 CAM_Y9"),
    ("L9",  "GPIO17 CAM_Y8"),
    ("L10", "GPIO18 CAM_Y7"),
    ("L11", "GPIO8 CAM_Y4"),
    ("L12", "GPIO3 JTAG_EN"),
    ("L13", "GPIO46 LOG"),
    ("L14", "GPIO9 CAM_Y3"),
    ("L15", "GPIO10 CAM_Y5"),
    ("L16", "GPIO11 CAM_Y2"),
    ("L17", "GPIO12 CAM_Y6"),
    ("L18", "GPIO13 CAM_PCLK"),
    ("L19", "GPIO14"),
    ("L20", "5V"),
]
ESP32_R = [
    ("R1",  "GPIO43 U0TXD"),
    ("R2",  "GPIO44 U0RXD"),
    ("R3",  "GPIO1 ADC1_CH0"),
    ("R4",  "GPIO2 ADC1_CH1"),
    ("R5",  "GPIO42 MTMS"),
    ("R6",  "GPIO41 MTDI"),
    ("R7",  "GPIO40 SD_DATA"),
    ("R8",  "GPIO39 SD_CLK"),
    ("R9",  "GPIO38 SD_CMD"),
    ("R10", "GPIO37 PSRAM*"),
    ("R11", "GPIO36 PSRAM*"),
    ("R12", "GPIO35 PSRAM*"),
    ("R13", "GPIO0 BOOT"),
    ("R14", "GPIO45 VSPI"),
    ("R15", "GPIO48 WS2812"),
    ("R16", "GPIO47"),
    ("R17", "GPIO21"),
    ("R18", "GPIO20 USB D+"),
    ("R19", "GPIO19 USB D-"),
    ("R20", "GND"),
]

# --- U2: Raspberry Pi Pico, laid out as on the board -----------------------
# Pin 1 is top-left and the numbering runs down the left then UP the right, so the
# right column reads 40 at the top down to 21 at the bottom - just like holding the
# board with the USB at the top.
PICO_L = [
    ("1", "GP0"), ("2", "GP1"), ("3", "GND"), ("4", "GP2"), ("5", "GP3"),
    ("6", "GP4"), ("7", "GP5"), ("8", "GND"), ("9", "GP6"), ("10", "GP7"),
    ("11", "GP8"), ("12", "GP9"), ("13", "GND"), ("14", "GP10"), ("15", "GP11"),
    ("16", "GP12"), ("17", "GP13"), ("18", "GND"), ("19", "GP14"), ("20", "GP15"),
]
PICO_R = [
    ("40", "VBUS"), ("39", "VSYS"), ("38", "GND"), ("37", "3V3_EN"),
    ("36", "3V3 out"), ("35", "ADC_VREF"), ("34", "GP28 ADC2"), ("33", "AGND"),
    ("32", "GP27 ADC1"), ("31", "GP26 ADC0"), ("30", "RUN"), ("29", "GP22"),
    ("28", "GND"), ("27", "GP21"), ("26", "GP20"), ("25", "GP19"),
    ("24", "GP18"), ("23", "GND"), ("22", "GP17"), ("21", "GP16"),
]

# --- drivers, sensors, audio, power -----------------------------------------
# --- SparkFun ROB-14450 "Motor Driver - Dual TB6612FNG (with headers)" --------
# Header order taken from the Eagle source in
# github.com/sparkfun/Motor_Driver-Dual_TB6612FNG  (Hardware/..._v11.sch):
#   JP1  outputs/power : 1 GND, 2 B01, 3 B02, 4 A02, 5 A01, 6 GND, 7 VCC, 8 VM
#   JP2  inputs        : 1 PWMA, 2 AIN2, 3 AIN1, 4 STBY, 5 BIN1, 6 BIN2, 7 PWMB, 8 GND
# Same 8+8 layout as the silkscreen: outputs on one side, inputs on the other.
# Pin numbers are prefixed per header because JP1-1 and JP2-1 are different nets.
TB6612_L = [("JP1-1", "GND"), ("JP1-2", "B01"), ("JP1-3", "B02"), ("JP1-4", "A02"),
            ("JP1-5", "A01"), ("JP1-6", "GND"), ("JP1-7", "VCC"), ("JP1-8", "VM")]
TB6612_R = [("JP2-1", "PWMA"), ("JP2-2", "AIN2"), ("JP2-3", "AIN1"), ("JP2-4", "STBY"),
            ("JP2-5", "BIN1"), ("JP2-6", "BIN2"), ("JP2-7", "PWMB"), ("JP2-8", "GND")]

BMI_L = [("1", "VDD"), ("2", "VDDIO"), ("3", "GND"), ("4", "CS")]
BMI_R = [("5", "SDA"), ("6", "SCL"), ("7", "SDO"), ("8", "INT1")]

# PCM5102A breakout, drawn as the board's own two headers (each listed top to bottom,
# exactly as the silkscreen reads).
#   left  (6): SCK  BCK  DIN  LCK  GND  VIN          -> pads 1-6
#   right (9): FLT  DEMP  XSMT  FMT  A3V3  AGND  ROUT  AGND  LOUT   -> pads 7-15
# SCK / FLT / DEMP / FMT / XSMT are solder bridges already made on the board
# (XSMT -> 3V3, the rest -> GND), so all five are drawn as no-connects.
# VIN is the input to the board's OWN 3.3 V regulator, so it takes **5 V**, not 3V3.
# A3V3 is that regulator's 3.3 V OUTPUT - a different net, left open. Never strap it
# to VIN: the DAC's supply maximum is 3.6 V.
# So only BCK, DIN, LCK, GND, VIN and LOUT get a wire.
# Pad numbers run 1-6 down the left header then 7-15 down the right. Deliberately NOT
# prefixed L/R: "R1" as a pin number would read as the resistor R1 in the netlist.
DAC_L = [("1", "SCK (GND)"), ("2", "BCK"), ("3", "DIN"),
         ("4", "LCK"), ("5", "GND"), ("6", "VIN")]
DAC_R = [("7", "FLT (GND)"), ("8", "DEMP (GND)"), ("9", "XSMT (3V3)"),
         ("10", "FMT (GND)"), ("11", "A3V3 (int 3V3)"), ("12", "AGND"),
         ("13", "ROUT"), ("14", "AGND"), ("15", "LOUT")]

AMP_L = [("1", "VDD"), ("2", "GND"), ("3", "SD")]
AMP_R = [("4", "IN+"), ("5", "IN-"), ("6", "OUT+"), ("7", "OUT-")]

BUCK_L = [("1", "IN+"), ("2", "IN-")]
BUCK_R = [("3", "OUT+"), ("4", "OUT-")]

STRIP_L = [("1", "VDD"), ("2", "GND")]
STRIP_R = [("3", "DIN")]

TWO_L = [("1", "~")]
TWO_R = [("2", "~")]

# All symbols use the standard 2.54 mm pin pitch (double-spacing was tried and gave
# no readability benefit at normal zoom, at twice the height).
PITCH = {}

LIBDEFS = {
    "ESP32-S3-CAM":    (ESP32_L, ESP32_R, 25.4, "U"),
    "Pico-RP2040":     (PICO_L, PICO_R, 25.4, "U"),
    "ROB-14450":       (TB6612_L, TB6612_R, 7.62, "U"),
    "BMI160":          (BMI_L, BMI_R, 11.43, "U"),
    "PCM5102A":        (DAC_L, DAC_R, 15.24, "U"),
    "PAM8302A":        (AMP_L, AMP_R, 11.43, "U"),
    "Buck-Module":     (BUCK_L, BUCK_R, 11.43, "PS"),
    "WS2812-Strip":    (STRIP_L, STRIP_R, 10.16, "LED"),
    "R":               (TWO_L, TWO_R, 2.54, "R"),
    "D-Schottky":      (TWO_L, TWO_R, 2.54, "D"),
    "Fuse":            (TWO_L, TWO_R, 2.54, "F"),
    "Battery":         (TWO_L, TWO_R, 2.54, "BT"),
    "Motor":           (TWO_L, TWO_R, 2.54, "M"),
    "Speaker":         (TWO_L, TWO_R, 2.54, "SP"),
    "E-Stop":          (TWO_L, TWO_R, 2.54, "SW"),
}

# --------------------------------------------------------------------------- #
# instances:  (ref, value, lib, x, y, {pin: net})   pin absent => no-connect
# --------------------------------------------------------------------------- #

INSTANCES = [
    # ---- MCUs --------------------------------------------------------------
    ("U1", "ESP32-S3-CAM (N16R8) - Freenove 2x20 header", "ESP32-S3-CAM", 60, 68, {
        # left row: only the two power pins are used; the rest is the camera.
        "L1":  "+3V3_S3",
        "L20": "+5V",
        # right row
        "R1":  "S3_TXD",       # GPIO43 -> R1 -> Pico GP17
        "R2":  "LINK_RX_S3",   # <- Pico GP16 -> R2
        "R3":  "I2C_SDA", "R4": "I2C_SCL",
        # I2S sits on 48/47/21 = header positions 15/16/17, three ADJACENT pins, and
        # in the same top-to-bottom order as the DAC board's own header, so the
        # ribbon runs straight across with no crossings.
        "R15": "I2S_BCK",      # GPIO48 -> DAC pad 2. WARNING: the on-board WS2812
                               # data input is on 48 too - unbuffered, same net.
        "R16": "I2S_DIN",      # GPIO47 -> DAC pad 3
        "R17": "I2S_LRCK",     # GPIO21 -> DAC pad 4
        "R14": "S3_LED",       # GPIO45 -> R6 (330R) -> strip. 45 is the VDD_SPI
                               # strap; the strip's data input is high-Z so it
                               # cannot fight the internal pull-down.
        "R20": "GND",
        # 38/39/40 and 39-42 are now FREE (on-board microSD slot, and JTAG) because
        # I2S no longer uses 40/41/42.
        # everything else is a deliberate no-connect: the 14 camera pins on the left
        # row, the dead PSRAM pins, BOOT/USB, and the strapping pins.
    }),
    ("U2", "Raspberry Pi Pico (RP2040) - 1-40 header", "Pico-RP2040", 185, 68, {
        # Each driver gets a CONTIGUOUS 6-pin block - GP0-5 for #1, GP6-11 for #2 - and
        # both blocks use the SAME order: PWMA, AIN1, AIN2, BIN2, BIN1, PWMB. The B
        # pair is deliberately reversed relative to the A pair so the two harnesses
        # come out as mirror images, which suits two mirrored driver boards.
        "1":  "DRV1_PWMA",    # GP0  PWM slice 0A
        "2":  "DRV1_AIN1",    # GP1
        "4":  "DRV1_AIN2",    # GP2
        "5":  "DRV1_BIN2",    # GP3
        "6":  "DRV1_BIN1",    # GP4
        "7":  "DRV1_PWMB",    # GP5  PWM slice 2B
        "9":  "DRV2_PWMA",    # GP6  PWM slice 3A
        "10": "DRV2_AIN1",    # GP7
        "11": "DRV2_AIN2",    # GP8
        "12": "DRV2_BIN2",    # GP9
        "14": "DRV2_BIN1",    # GP10
        "15": "DRV2_PWMB",    # GP11 PWM slice 5B
        # Link: UART0's TX/RX is routable ONLY to GP0/1, GP12/13, GP16/17 and GP28/29.
        # GP14/GP15 are UART0's CTS/RTS and cannot carry the link at all, so this is
        # the next genuine pair after the motor block.
        "21": "PICO_TXD",     # GP16 -> R2 -> S3 GPIO44
        "22": "LINK_RX_PICO",  # GP17 <- R1 <- S3 GPIO43
        "34": "STBY_PICO",    # GP28 -> R5 (1k) -> MOT_STBY. GP28 is in GPIO0-29,
                               # so its pad resets with PDE=1 and STBY is held low
                               # before a single line of firmware runs.
        "3": "GND", "8": "GND", "13": "GND", "18": "GND",
        "23": "GND", "28": "GND", "33": "GND", "38": "GND",
        "36": "+3V3_PICO",
        "39": "VSYS",
        # Free: GP12-GP15 (4) and GP18-GP22 (5), plus GP26/GP27 (ADC0/1).
        # GP23-25 are not on the header; GP25 is the LED on a non-W Pico.
        # NB the link costs the old GP14-GP22 contiguous block - it is now split.
        # 19-22,24-27,29-32,35,37,40 spares ... but note pin 35 is ADC_VREF, not a GPIO.
    }),

    # ---- motor drive -------------------------------------------------------
    ("U3", "SparkFun ROB-14450  #1  (left: FL=A, RL=B)", "ROB-14450", 55, 114, {
        "JP1-8": "VM_MOTOR", "JP1-7": "+3V3_PICO",
        "JP1-1": "GND", "JP1-6": "GND", "JP2-8": "GND",
        "JP2-4": "MOT_STBY",
        "JP2-1": "DRV1_PWMA", "JP2-3": "DRV1_AIN1", "JP2-2": "DRV1_AIN2",
        "JP2-7": "DRV1_PWMB", "JP2-5": "DRV1_BIN1", "JP2-6": "DRV1_BIN2",
        "JP1-5": "MOT_FL_A", "JP1-4": "MOT_FL_B",
        "JP1-2": "MOT_RL_A", "JP1-3": "MOT_RL_B",
    }),
    ("U4", "SparkFun ROB-14450  #2  (right: RR=A, FR=B)", "ROB-14450", 140, 114, {
        "JP1-8": "VM_MOTOR", "JP1-7": "+3V3_PICO",
        "JP1-1": "GND", "JP1-6": "GND", "JP2-8": "GND",
        "JP2-4": "MOT_STBY",
        "JP2-1": "DRV2_PWMA", "JP2-3": "DRV2_AIN1", "JP2-2": "DRV2_AIN2",
        "JP2-7": "DRV2_PWMB", "JP2-5": "DRV2_BIN1", "JP2-6": "DRV2_BIN2",
        "JP1-5": "MOT_RR_A", "JP1-4": "MOT_RR_B",
        "JP1-2": "MOT_FR_A", "JP1-3": "MOT_FR_B",
    }),

    # ---- sensors -----------------------------------------------------------
    ("U5", "BMI160 breakout", "BMI160", 230, 114, {
        "1": "+3V3_S3", "2": "+3V3_S3", "3": "GND",
        "4": "+3V3_S3",          # CS high => I2C mode
        "5": "I2C_SDA", "6": "I2C_SCL",
        "7": "GND",              # SDO low  => address 0x68
        # 8 INT1 polled, not wired
    }),

    # ---- audio -------------------------------------------------------------
    ("U6", "PCM5102A DAC (on-board 3V3 reg, VIN = 5 V)", "PCM5102A", 55, 212, {
        "2": "I2S_BCK", "3": "I2S_DIN", "4": "I2S_LRCK",
        "5": "GND", "6": "+5V",
        "12": "GND", "14": "GND",
        "15": "AUDIO_L",
        # no-connects, on purpose:
        #   1 SCK, 7 FLT, 8 DEMP, 9 XSMT, 10 FMT - solder bridges, already made
        #   11 A3V3 - the board's own 3V3 regulator output; DO NOT strap to VIN
        #   13 ROUT - unused, mono build
    }),
    ("U7", "PAM8302A class-D amp", "PAM8302A", 155, 212, {
        "1": "+5V", "2": "GND",
        # 3 SD: board dependent, leave open
        "4": "AUDIO_L", "5": "GND",
        "6": "SPK_A", "7": "SPK_B",
    }),

    # ---- power -------------------------------------------------------------
    # Two SEPARATE buck modules: motor rail and logic rail come off the battery in
    # parallel, so motor return current never flows through the logic ground.
    ("PS1", "XL4015 buck 5 A -> MOTOR RAIL = 8.0 V", "Buck-Module",
     160, 242, {"1": "VBAT", "2": "GND", "3": "VM_MOTOR", "4": "GND"}),
    ("PS2", "LM2596 buck 3 A -> 5.0 V LOGIC", "Buck-Module",
     240, 242, {"1": "VBAT", "2": "GND", "3": "+5V", "4": "GND"}),
    ("BT1", "3S LiPo 11.1 V (9.0-12.6 V)", "Battery", 30, 242,
     {"1": "VBAT_RAW", "2": "GND"}),
    ("F1", "fuse 7.5 A", "Fuse", 92, 242, {"1": "VBAT_RAW", "2": "VBAT"}),
    ("D1", "BAT54 Schottky", "D-Schottky", 100, 182, {"1": "+5V", "2": "VSYS"}),

    # ---- series / pull-up glue --------------------------------------------
    # No I2S series resistors here: the PCM5102A board already has one in line with
    # each of BCK / DIN / LRCK, so the three I2S lines run straight from the S3.
    ("R1", "1k  UART series", "R", 35, 150, {"1": "S3_TXD", "2": "LINK_RX_PICO"}),
    ("R2", "1k  UART series", "R", 97, 150, {"1": "PICO_TXD", "2": "LINK_RX_S3"}),
    ("R3", "4.7k I2C pull-up (DNP?)", "R", 159, 150, {"1": "+3V3_S3", "2": "I2C_SDA"}),
    ("R4", "4.7k I2C pull-up (DNP?)", "R", 221, 150, {"1": "+3V3_S3", "2": "I2C_SCL"}),
    ("R5", "1k  STBY series", "R", 283, 150, {"1": "STBY_PICO", "2": "MOT_STBY"}),
    ("R6", "330R strip series", "R", 345, 150, {"1": "S3_LED", "2": "LED_STRIP_DIN"}),
    ("SW1", "E-stop (NC, optional)", "E-Stop", 35, 182, {"1": "MOT_STBY", "2": "GND"}),

    # ---- outputs / mechanical ---------------------------------------------
    ("M1", "motor FL", "Motor", 30, 278, {"1": "MOT_FL_A", "2": "MOT_FL_B"}),
    ("M2", "motor RL", "Motor", 82, 278, {"1": "MOT_RL_A", "2": "MOT_RL_B"}),
    ("M3", "motor RR", "Motor", 134, 278, {"1": "MOT_RR_A", "2": "MOT_RR_B"}),
    ("M4", "motor FR", "Motor", 186, 278, {"1": "MOT_FR_A", "2": "MOT_FR_B"}),
    ("LED1", "WS2812 strip (5 V)", "WS2812-Strip", 250, 212, {
        "1": "+5V", "2": "GND", "3": "LED_STRIP_DIN"}),
    ("SP1", "speaker 8R", "Speaker", 335, 212, {"1": "SPK_A", "2": "SPK_B"}),
]

NOTES = [
    (12, 14, "RC car  -  ESP32-S3-CAM brain + RP2040 PWM splitter"),
    (12, 19, "U1 = Freenove L1-L20 / R1-R20"),
    (12, 24, "U2 = Pico 1-40 as on board"),
    (12, 29, "U3/U4 = ROB-14450 JP1-x/JP2-x"),
    (108, 46, "MCU BLOCKS"),
    (300, 102, "MOTOR DRIVE + IMU"),
    (35, 138, "GLUE"),
    (35, 168, "E-STOP / VSYS"),
    (35, 191, "AUDIO + LED"),
    (300, 232, "POWER"),
    (30, 266, "MECHANICAL"),
]


def main():
    lib_text = []
    lib_text_plain = []
    geoms = {}
    halfh = {}
    for name, (l, r, hw, rp) in LIBDEFS.items():
        pit = PITCH.get(name, 2.54)
        t, g, hh = lib_symbol(name, l, r, hw, refpre=rp, pitch=pit)
        lib_text.append(t)
        plain, _, _ = lib_symbol(name, l, r, hw, refpre=rp, prefix="", pitch=pit)
        lib_text_plain.append(plain)
        geoms[name] = g
        halfh[name] = hh

    body = []

    # graphics: notes
    for (x, y, txt) in NOTES:
        x, y = snap(x), snap(y)
        body.append('  (text "%s" (at %s %s 0)' % (txt, x, y))
        body.append('    %s' % eff(2.0, justify="left bottom"))
        body.append('    (uuid %s)' % uid())
        body.append('  )')

    n_wires = n_labels = n_nc = 0

    for ref, value, lib, x, y, nets in INSTANCES:
        x, y = snap(x), snap(y)
        g = geoms[lib]
        hh = halfh[lib]
        body.append('  (symbol (lib_id "%s:%s") (at %s %s 0) (unit 1)' % (LIB, lib, x, y))
        body.append('    (in_bom yes) (on_board yes) (dnp no)')
        body.append('    (uuid %s)' % uid())
        body.append('    (property "Reference" "%s" (at %s %s 0) %s)'
                    % (ref, x, round(y - hh - 2.54, 4), eff(justify="left bottom")))
        body.append('    (property "Value" "%s" (at %s %s 0) %s)'
                    % (value, x, round(y - hh - 5.08, 4), eff(justify="left bottom")))
        body.append('    (property "Footprint" "" (at %s %s 0) %s)'
                    % (x, y, eff(hide=True)))
        body.append('    (property "Datasheet" "~" (at %s %s 0) %s)'
                    % (x, y, eff(hide=True)))
        for num in sorted(g, key=lambda k: (int(k) if k.isdigit() else 0, k)):
            body.append('    (pin "%s" (uuid %s))' % (num, uid()))
        body.append('    (instances')
        body.append('      (project "%s"' % PROJ)
        body.append('        (path "/%s" (reference "%s") (unit 1))' % (ROOT, ref))
        body.append('      )')
        body.append('    )')
        body.append('  )')

        # stubs + labels / no-connects
        for num, (px, py, side) in g.items():
            net = nets.get(num)
            sx = round(x + px, 4)
            sy = round(y - py, 4)
            if net:
                ex = round(sx - STUB, 4) if side == "L" else round(sx + STUB, 4)
                just = "right bottom" if side == "L" else "left bottom"
                body.append('  (wire (pts (xy %s %s) (xy %s %s))' % (sx, sy, ex, sy))
                body.append('    (stroke (width 0) (type default))')
                body.append('    (uuid %s)' % uid())
                body.append('  )')
                body.append('  (label "%s" (at %s %s 0)' % (net, ex, sy))
                body.append('    %s' % eff(justify=just))
                body.append('    (uuid %s)' % uid())
                body.append('  )')
                n_wires += 1
                n_labels += 1
            else:
                body.append('  (no_connect (at %s %s) (uuid %s))' % (sx, sy, uid()))
                n_nc += 1

    out = []
    out.append('(kicad_sch (version 20230121) (generator eeschema)')
    out.append('')
    out.append('  (uuid %s)' % ROOT)
    out.append('')
    out.append('  (paper "A3")')
    out.append('')
    out.append('  (title_block')
    out.append('    (title "RC car - ESP32-S3-CAM brain + RP2040 PWM splitter")')
    out.append('    (date "2026-10-10")')
    out.append('    (rev "A")')
    out.append('    (comment 1 "Auto-generated by hardware/gen_schematic.py - do not hand-edit")')
    out.append('    (comment 2 "Architecture and rationale: docs/circuit-plan.md")')
    out.append('  )')
    out.append('')
    out.append('  (lib_symbols')
    out.append("\n".join(lib_text))
    out.append('  )')
    out.append('')
    out.append("\n".join(body))
    out.append('')
    out.append('  (sheet_instances')
    out.append('    (path "/" (page "1"))')
    out.append('  )')
    out.append(')')

    with open(OUT, "w") as fh:
        fh.write("\n".join(out) + "\n")

    # the same symbols as a real, reusable KiCad library so the schematic opens
    # as a self-contained project with no "library not found" warnings
    with open(os.path.join(HERE, "car.kicad_sym"), "w") as fh:
        fh.write('(kicad_symbol_lib (version 20211014) (generator gen_schematic.py)\n')
        fh.write("\n".join(lib_text_plain) + "\n")
        fh.write(")\n")

    # sym-lib-table: MERGE, never overwrite. Libraries added by hand in the KiCad
    # GUI (e.g. a vendor part) must survive regenerating this schematic.
    slt = os.path.join(HERE, "sym-lib-table")
    car_entry = ('  (lib (name "car")(type "KiCad")(uri "${KIPRJMOD}/car.kicad_sym")'
                 '(options "")(descr "RC car module symbols (generated)"))')
    kept = []
    if os.path.exists(slt):
        with open(slt) as fh:
            for line in fh:
                s = line.strip()
                if s.startswith('(lib ') and '(name "car")' not in s:
                    kept.append("  " + s)
    with open(slt, "w") as fh:
        fh.write("(sym_lib_table\n")
        fh.write("\t(version 7)\n")
        fh.write(car_entry + "\n")
        for k in kept:
            fh.write(k + "\n")
        fh.write(")\n")

    with open(os.path.join(HERE, "car.kicad_pro"), "w") as fh:
        fh.write('{\n'
                 '  "board": {},\n'
                 '  "boards": [],\n'
                 '  "cvpcb": {},\n'
                 '  "erc": {},\n'
                 '  "libraries": { "pinned_footprint_libs": [], "pinned_symbol_libs": [] },\n'
                 '  "meta": { "filename": "car.kicad_pro", "version": 1 },\n'
                 '  "net_settings": {},\n'
                 '  "pcbnew": {},\n'
                 '  "schematic": {},\n'
                 '  "sheets": [],\n'
                 '  "text_variables": {}\n'
                 '}\n')

    # ---- printable wiring table (same source data, cannot drift) -----------
    pinname = {}
    for name, (l, r, hw, rp) in LIBDEFS.items():
        for num, nm in list(l) + list(r):
            pinname[(name, num)] = nm

    netmap = defaultdict(list)
    for ref, value, lib, x, y, nets in INSTANCES:
        for num, net in nets.items():
            if net:
                nm = pinname.get((lib, num), num)
                netmap[net].append("%s.%s %s" % (ref, num, nm if nm != "~" else ""))

    ORDER = ["GND", "VBAT_RAW", "VBAT", "+5V", "VSYS", "+3V3_S3", "+3V3_PICO"]

    def rank(n):
        return (ORDER.index(n), n) if n in ORDER else (len(ORDER), n)

    with open(os.path.join(HERE, "wiring.md"), "w") as fh:
        fh.write("<!-- generated by hardware/gen_schematic.py - do not hand-edit -->\n\n")
        fh.write("# Wiring table - %d nets\n\n" % len(netmap))
        fh.write("Derived from `car.kicad_sch`. Build to this list, one row at a time.\n\n")
        fh.write("## Reference designators\n\n")
        fh.write("| Ref | Part |\n| --- | --- |\n")
        for ref, value, lib, x, y, nets in INSTANCES:
            fh.write("| `%s` | %s |\n" % (ref, value))
        fh.write("\n## Nets\n\n")
        fh.write("| Net | Connections |\n| --- | --- |\n")
        for net in sorted(netmap, key=rank):
            fh.write("| `%s` | %s |\n" % (net, " · ".join(netmap[net])))
        fh.write("\nUnlisted pins are deliberate no-connects: spares, strapping pins,\n")
        fh.write("the dead PSRAM pins and the unused breakout pins.\n")

    print("wrote %s" % OUT)
    print("wrote hardware/car.kicad_sym, hardware/sym-lib-table, hardware/car.kicad_pro")
    print("wrote hardware/wiring.md  (%d nets)" % len(netmap))
    print("  %d symbols, %d nets stubs, %d no-connects"
          % (len(INSTANCES), n_labels, n_nc))


if __name__ == "__main__":
    main()
