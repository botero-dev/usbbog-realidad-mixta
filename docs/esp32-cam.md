# ESP32-S3-CAM: pin budget for the car

Notes for moving the car firmware onto an **ESP32-S3-CAM board** (module:
**ESP32-S3-WROOM-1 N16R8** — 16 MB quad flash, 8 MB **octal** PSRAM — with an
OV2640/OV3660 on a DVP connector, a microSD slot, and native USB-C), adding a
**PCM5102A I2S DAC** for sound, and a **BMI160/BMI270 6-axis IMU** for odometry
(§13).

> 📄 **The architecture was settled on 2026-10-10: see [`circuit-plan.md`](circuit-plan.md).**
> The S3-CAM is the only brain (WiFi + camera + audio + IMU + control loop) and an
> **RP2040 acts as a PWM splitter** for the two TB6612s. Everything below is the
> evidence and the rejected alternatives that led there; §7 Option 3 is the plan
> that was chosen, with the UART link on **GP16/GP17** and `STBY` on **GP28**.

The whole point of this document is the pin arithmetic: the camera eats a fixed
block of GPIOs, so what is left over has to cover the motor drivers and the DAC.

> ✅ **Board identified (2026-10-10):** the 40-pin module matches the **Freenove
> ESP32-S3-WROOM** carrier (2 × 20-pin header) —
> [Freenove/Freenove_ESP32_S3_WROOM_Board · `ESP32S3_Pinout.png`](https://github.com/Freenove/Freenove_ESP32_S3_WROOM_Board/blob/main/ESP32S3_Pinout.png).
> The physical header is reproduced in §11.1 and every "is it on the header?"
> answer in this document comes from that image.
>
> **N16R8 vs N8R8 changes nothing about pins**: the flash is still quad-SPI on
> 26–32 and the 8 MB PSRAM is still *octal*, so 33–37 stay reserved. Only the
> flash size (and therefore the partition scheme) differs.
>
> ⚠️ **Headline finding:** the camera pins `4–18` *are* broken out on the left
> header row — this carrier is a plain breakout, not a camera-only module. They
> are the same nets as the camera connector, so while a camera is fitted they are
> still off-limits; but if vision is ever dropped, those 14 pins come back.

---

## 1. Pins the camera already owns (fixed)

| Signal | GPIO | Notes |
| --- | --- | --- |
| SIOD (SCCB SDA) | **4** | |
| SIOC (SCCB SCL) | **5** | |
| VSYNC | **6** | |
| HREF | **7** | |
| PCLK | **13** | |
| XCLK | **15** | XTAL_32K_P |
| Y2 / D0 | **11** | |
| Y3 / D1 | **9** | |
| Y4 / D2 | **8** | |
| Y5 / D3 | **10** | |
| Y6 / D4 | **12** | |
| Y7 / D5 | **18** | |
| Y8 / D6 | **17** | |
| Y9 / D7 | **16** | XTAL_32K_N |
| PWDN / RESET | – | tied on-board, `-1` in software |

**14 GPIOs: `4,5,6,7,8,9,10,11,12,13,15,16,17,18`.**

On this carrier they **are** present on the left header row (`CAM_SIOD`,
`CAM_SIOC`, … in the Freenove pinout), but they are the same nets as the camera
connector — so while a camera is fitted they are unusable for anything else. They
come back only if you give up the camera.

## 2. TF slot (microSD, SDMMC 1-bit)

| Signal | GPIO |
| --- | --- |
| CMD | **38** |
| CLK | **39** |
| DAT0 | **40** |

`DAT1/2/3` are not connected. **Not used (decision, 2026-10-10) — so these 3
GPIOs are yours.**

## 3. Permanently unavailable

| GPIO | Why |
| --- | --- |
| 26, 27, 28, 29, 30, 31, 32 | SPI0/1 — internal flash |
| 33, 34, 35, 36, 37 | **Octal** PSRAM — 35/36/37 sit on the right header row but are dead because of the R8 part; 33/34 are not exposed at all |
| 19, 20 | native USB D− / D+ — the only flashing path (the Freenove image prints the D−/D+ labels swapped; Espressif's mapping is 19 = D−, 20 = D+) |
| 0 | BOOT button + strapping |
| 22, 23, 24, 25 | **do not exist** on ESP32-S3 |

Everything else on the header is usable. The ones with strings attached:

| GPIO | Caveat |
| --- | --- |
| 3, 45, 46 | strapping pins, sampled at reset — normal GPIOs afterwards. `3` = JTAG source, `45` = VDD_SPI voltage, `46` = ROM log printing |
| 41, 42 | JTAG MTDI/MTMS — normal GPIO if you don't debug over pins |
| 43, 44 | U0TXD / U0RXD — usable as GPIO, but the console then lives on USB CDC only |
| 48 | on-board **WS2812** LED — the LED follows whatever you drive on the pin |
| 38, 39, 40 | microSD `CMD` / `CLK` / `DAT0` — free as long as the TF slot stays unused |

## 4. The free pool

> **Decision (2026-10-10):** no microSD, no UART console, PSRAM not required.
> The first three tiers below are therefore in the pool.

| Tier | GPIOs | Count | Cost |
| --- | --- | --- | --- |
| **Clean** | `1, 2, 14, 21, 41, 42, 47, 48` | **8** | `41/42` lose JTAG; `48` blinks the on-board WS2812 |
| + UART console | `43, 44` | +2 → **10** | *accepted* — log only over USB CDC |
| + microSD | `38, 39, 40` | +3 → **13** | *accepted* — no TF card |
| + strapping | `3, 45, 46` | +3 → **16** | all three read low at reset via the TB6612's internal 200 kΩ input pull-downs |

**Usable pool: 13 pins**, plus `3`/`45`/`46` → **16 total**.

⚠️ **PSRAM cannot be traded back.** On an **R8** module GPIO33–37 are bonded to
the octal PSRAM die *inside* the module — not initialising PSRAM in software does
not turn them into GPIOs. `35/36/37` sit on the right header row and stay dead.
Recovering those three pins would mean fitting a **non-PSRAM module** (N16/N8
without the `R`), at the cost of camera framebuffer headroom.

## 5. What the car needs

| Block | Pins | Detail |
| --- | --- | --- |
| 4 motors via H-bridges (TB6612 style) | 12 | `AIN1, AIN2, PWMA, BIN1, BIN2, PWMB` × 2 chips |
| H-bridge `STBY` | 1 | shared between both chips |
| PCM5102A I2S | 3 | `BCK`, `LRCK/WS`, `DIN` (SCK→GND, no MCLK) |
| BMI160/270 IMU | 2 | `SDA` + `SCL` on a real I²C bus; +1 if you wire `INT` (§13) |
| **Total** | **18** | **19** with the IMU interrupt |

**18 needed (19 with the IMU's `INT`) vs 16 available → 2 pins over.** The motor
half already needs the 3 strapping pins (`3`, `45`, `46`) to close, and they must
read **low** during reset. No external resistors are needed for that: the
TB6612FNG datasheet specifies a **200 kΩ internal pull-down on every logic input**
(`AIN1`, `AIN2`, `PWMA`, `BIN1`, `BIN2`, `PWMB` *and* `STBY`), so the driver holds
those lines low while the ESP32 pins are still high-Z. The **BMI sensor is what
tips it over**: motors + I²S fill the board exactly — 0 spare pins, no I²C bus,
and whatever signal lands on `48` makes the WS2812 flicker — so the 2 pins the IMU
wants are simply not there. §6 and §7 lay out the ways to find them.

### ⚠️ Your current motor pins are mostly invalid on the S3-CAM

From `firmware/car_control/motor.h`:

| Current pin | Verdict on ESP32-S3-CAM |
| --- | --- |
| 23, 22, 25 | **GPIO does not exist** on ESP32-S3 |
| 4, 18 | camera (SIOD, Y7/D5) |
| 19 | USB D- |
| 26, 27, 32, 33 | flash (26–32) / octal PSRAM (33) |
| 21 | free ✔ |
| 14 | free ✔ |

→ A full re-map is mandatory, not optional.

## 6. Ways to close the gap

**A. Cut the pin count (still one MCU, still the camera)**

| Trick | Motor pins | Total with I²S | Fits the 13-pin pool? |
| --- | --- | --- | --- |
| 3 pins/motor (`IN1,IN2,PWM`) + `STBY` | 13 | 16 | ✔ exactly — the entire usable right row |
| tie `STBY` to 3V3 | 12 | 15 | ✔ needs 2 straps (`45`, `46`) |
| PWM the `IN` pins (`PWMA/B` tied high) | 8 + `STBY` | 12 | ✔ **4 spare** |
| **74HC595 for the 8 direction lines** | 3 + 4 PWM | **10** | ✔ 3 spare |
| **PCA9685 (I²C, 16 channels)** | 2 | **5** | ✔ 8 spare |

Three of these are comfortable single-MCU answers:

* **PCA9685** — `SDA` + `SCL` carry all 13 motor signals (12 outputs + `STBY`),
  with the PWM generated in hardware inside the chip (write once per change).
  Costs: PWM range is 200 Hz – 1.5 kHz (200 Hz whines, set ~1 kHz), and an I²C bus
  on a car is an EMI antenna — keep it short, use strong pull-ups. Bonus: the same
  bus can carry an IMU or an OLED, and the 16 channels leave 3 spare.
* **74HC595** — 3 pins (`SER`, `SRCLK`, `RCLK`) drive the 8 direction lines, 4 GPIOs
  stay on LEDC for the PWM. No bus, no extra silicon in the signal path, but three
  pins worse than the PCA9685. Direction changes are slow and that is fine here.
* **8-pin motor drive** — tie `PWMA`/`PWMB` high and PWM the `IN` pins instead. All
  four modes survive (drive via `IN1`/`IN2`, coast = both low, brake = both high);
  the only difference is that the off-portion of each cycle chops to **coast**
  rather than the short brake you get when the PWM pin is the chopped one. Frees
  4 pins — the roomiest option that still keeps `STBY` on a GPIO.

**Where the BMI sensor goes.** The totals in that table are motors + `STBY` + I²S
only; the IMU wants 2 more (3 with `INT`), which changes the picture:

| Base design | IMU pins needed | Verdict |
| --- | --- | --- |
| Option 1 as drawn (§7) | — | ✘ every usable pin is committed |
| Option 1 + 8-pin motor drive | 2 | ✔ 8 + `STBY` + 3 I²S + 2 I²C = 14 of 16 |
| Option 1 + 74HC595 | 2 | ✔ 12 of 16 → 4 spare |
| Option 1b (PCA9685) | **0** | ✔ it joins the existing `1`/`2` I²C bus |
| Option 2 / 3 (two MCUs) | 0 on the S3 | ✔ the IMU hangs off the **body** MCU |

**B. Two MCUs (the simplest way out of the pin squeeze)**
Keep the ESP32-CAM as the *head* (camera + audio + WiFi) and give the *body*
(motors, battery, IMU) its own controller. Two candidates for the body:

* **RP2040 / Pico** — see Option 3 in §7. Most capable: silent 20 kHz PWM, encoders
  counted in PIO, a local fail-safe, and it can own the LED strip.
* **the existing `esp32dev`** — reuses `motor.h` untouched; link over UART (`43/44`
  on the S3 ↔ `16/17` on the classic ESP32, the pair your `car_control/main.cpp`
  already drives) or ESP-NOW (0 pins).

Either way the S3 stops needing motor pins at all, so `48` becomes free for the
on-board WS2812 / a strip (§12) and `1, 2, 21, 38–45, 47` become spare.

## 7. Suggested assignments

Split by function: **the motors own the whole usable right row, I²S owns the left
row** (`3`, `46`, `14`), and the camera keeps the rest of the left row (4–13,
15–18). That is possible because the right row exposes exactly 13 usable pins,
which is exactly what 12 motor lines + `STBY` need.

### Option 1 — all 16 signals on plain GPIOs (no extra silicon)

Uses the whole 13-pin right row for the motors and three left-row pins for I²S.
**No external pull-down resistors are needed** for the straps, because every
TB6612 logic input has a **200 kΩ internal pull-down** (datasheet pin table).

| Function | GPIO | Header row |
| --- | --- | --- |
| I2S BCK | **3** | left |
| I2S LRCK/WS | **46** | left |
| I2S DIN | **14** | left — bottom-of-row pin, diagonally next to GND |
| `STBY` (both chips) | **45** | right — its own pull-down straps VDD_SPI = 3.3 V |
| TB6612 #1 — `PWMA` / `AIN1` / `AIN2` | 43 / 44 / 1 | right, header #1–#3 |
| TB6612 #1 — `PWMB` / `BIN1` / `BIN2` | 2 / 42 / 41 | right, header #4–#6 |
| TB6612 #2 — `PWMA` / `AIN1` / `AIN2` | 40 / 39 / 38 | right, header #7–#9 |
| TB6612 #2 — `PWMB` / `BIN1` / `BIN2` | 48 / 47 / 21 | right, header #15–#17 |
| Kept intact | `0` (BOOT button) · `19/20` (USB) | right |
| Spare | **none** — every usable right-row pin is used |

Each TB6612 gets two contiguous triples of header pins (`43/44/1` + `2/42/41` for
chip #1, `40/39/38` + `48/47/21` for chip #2), so a chip's six wires come off the
row as two groups. `45` (`STBY`) sits between the two chips.

Order within the three I²S pins is arbitrary — on the S3 the GPIO matrix lets any
pin carry any I²S signal. It is arranged as above so the clock pair (`3`, `46`) is
adjacent and `DIN` sits at the bottom of the left row, near the right-row `GND`.

**Strap check.** The only strap that *must* be low at reset is `45`, and `S‍TBY`'s
internal pull-down guarantees it:

| Strap | Needs at reset | Supplied by |
| --- | --- | --- |
| `45` — VDD_SPI voltage | **0** (mandatory: the flash is 3.3 V) | `STBY`'s internal pull-down |
| `3`, `46` — JTAG source / ROM log | either level | nothing — the DAC inputs are high-Z, so these float |

Floating `3`/`46` is harmless: both straps accept either level (`3` only picks
which pins JTAG comes from, `46` only affects ROM log printing). If you would
rather have them defined, add one **10 kΩ pull-down each** — otherwise they are
the only undefined nets in the design. Driving `45` high after boot does **not**
change `VDD_SPI` — that voltage is latched at reset only.

> **Bus-exploit:** the four pins the `3` strap can hand to JTAG are `39–42`, and
> in this design those are two motor lines and an unused SD pin. Irrelevant in
> practice — JTAG only takes them over if a debugger is attached, and `STBY` is
> low throughout boot, so the drivers stay disabled while the pins settle.

> **One resistor is still worth fitting:** 200 kΩ is weak, so a stray pull-up, a
> long wire or moisture on the `45` net could make it read high at reset — and that
> is the one strap failure that bricks the boot (VDD_SPI = 1.8 V, 3.3 V flash
> unpowered, board looks dead). A single **10 kΩ from `45` to GND** costs 0.33 mA
> while `STBY` is high and removes the risk entirely.

Note `0` is *not* used: the BOOT button and USB stay untouched. If a sensor, IMU or
display is ever wanted, there is no pin left — that is the argument for 1b.

> **No room for the BMI sensor here.** This variant spends all 16 usable pins, so
> an IMU has to arrive by one of the routes in §6: the 8-pin motor drive, the
> 74HC595, the PCA9685 (1b), or by moving the motors to a second MCU (Options 2/3).

### Option 1b — same thing with a PCA9685

| Function | GPIO |
| --- | --- |
| I²C SDA → PCA9685 | 1 |
| I²C SCL → PCA9685 | 2 |
| I2S BCK / LRCK / DIN | 3 / 46 / 14 |
| PCA9685 ch 0–5 | TB6612 #1 (`PWMA, AIN1, AIN2, PWMB, BIN1, BIN2`) |
| PCA9685 ch 6–11 | TB6612 #2 (same order) |
| PCA9685 ch 12 | `STBY` (both chips) |
| **BMI160/270 IMU** | **shares `1`/`2`** — same bus, no extra pin |
| Still free | `1, 2, 21, 38–48` (minus USB/BOOT) on the right row · ch 13–15 · the IMU's `INT` if you want it |

Identical function, all 13 right-row pins free for sensors, and `45` never touched.
**This is the only single-MCU variant that hosts the IMU without changing the
motor wiring** — the BMI sensor just joins the PCA9685 on `1`/`2`. Addresses are
safe by default (PCA9685 = 0x40, BMI160/270 = 0x68/0x69); you only have to think
about it if you stack several PCA9685s. Keep that bus short and at ≤ 400 kHz, and
test early: the PCA9685 stretches the clock, and the classic ESP32's I²C controller
has a known clock-stretching bug. The S3's peripheral is newer, but verify before
you commit the layout.

### Option 2 — two MCUs (head / body)

| Side | Pins |
| --- | --- |
| S3-CAM → link | `43/44` (UART) or ESP-NOW (0 pins) |
| S3-CAM audio | I2S `BCK 3`, `LRCK 46`, `DIN 14` |
| S3-CAM spare | `1, 2, 21, 38–48` (minus USB/BOOT) + microSD + UART console + all of the left-row spares |
| classic `esp32dev` body | keeps `motor.h` as-is — all 12 of its pins are valid there; the **BMI sensor and the encoders hang off its spare pins** |

This is the only variant that leaves room for encoders, an IMU, a display or a
battery monitor without more pin gymnastics — and it is where the BMI sensor
belongs anyway: odometry and tilt are read next to the motors, not next to the
camera.

### Option 3 — two MCUs with an **RP2040 (Pico)** body

The PCA9685 only existed to work around the S3's pin shortage. An RP2040 does the
same job *better*: real hardware PWM at any frequency, no I²C bus, and enough pins
left for encoders.

| Function | Pico pin | Note |
| --- | --- | --- |
| TB6612 #1 — `PWMA` … `PWMB` | GP0–GP5 | PWM 0A/2B + 4 direction lines |
| TB6612 #2 — `PWMA` … `PWMB` | GP6–GP11 | PWM 3A/5B + 4 direction lines |
| `STBY` (both chips) | GP28 | software kill |
| Link to the S3-CAM | GP16 TX → S3 RX · GP17 RX ← S3 TX | UART0's *third* pin pair |
| Left over | GP12–GP15, GP18–GP22, GP25 (LED), GP26/GP27 (ADC) | encoders, battery/current sense |

**15 pins used of the 26 exposed on a Pico — 11 spare**, and the S3 still needs none
of them. Final pinout is `circuit-plan.md` §3; this table is the option as first
sketched, with the three rows that changed during review corrected in place.

The BMI160 ended up on the **S3's** I²C, not the Pico's (§1 of the plan), which is why
no Pico I²C bus appears above — and just as well, because `GP16`/`GP17` are the link.

> ⚠️ The link **cannot** go on `GP14`/`GP15`: those are UART0's `CTS`/`RTS`, not TX/RX.
> UART0 TX/RX is routable only to `GP0`/`GP1`, `GP12`/`GP13`, `GP16`/`GP17` and
> `GP28`/`GP29`.

**PWM channels.** A GPIO's PWM slice is fixed: `GPn` → slice `n/2`, channel `n%2`,
and pins in the same slice must share one frequency. The four motor PWMs sit on
slices 0 and 1, so there are four independent duties at a common frequency — set
**20 kHz** and the motors are silent. (The PCA9685 tops out around 1.6 kHz, which
is squarely in the audible whine band.)

**What the RP2040 buys you over the PCA9685**

* silent 20 kHz PWM, no I²C traffic subject to motor-induced noise
* a **home for the BMI sensor** — GPIO16/GPIO17 plus an interrupt pin, all local
  to the motors, with no S3 pin pressure at all
* **encoders**: 4 motors × 2 channels fits easily, and `PIO` can count quadrature
  edges in hardware with zero CPU load
* a **local fail-safe** — the Pico cuts `STBY` itself if the S3 stops talking, which
  the single-MCU design cannot do at all
* it can drive the WS2812 strip over PIO too, so `48` stays free on the S3
* ADC on `GP26–GP28` for battery voltage and motor current sense

**What it costs**

* a second firmware (Arduino-Pico core, MicroPython or the Pico SDK) and a defined
  serial protocol
* 2–4 extra wires (TX, RX, GND, or I²C with the Pico as slave)
* the RP2040 has no radio — WiFi/ESP-NOW stays the S3's job. A **Pico W** works
  equally well: its wireless chip takes `GP23`, `GP24`, `GP25` and `GP29`, so
  `GP0–GP22` remain free as mapped above.

**Link protocol, minimum viable:** fixed-size frames (`0xA5 | seq | 4×int16 duty |
uint16 crc`), 115200–1 Mbaud, plus a 200 ms "no valid frame" timeout on the Pico
that drops `STBY`. That gives you a dead-man switch for the whole drivetrain — worth
having on a car that also runs a camera and a class-D speaker.

## 8. PCM5102A wiring notes

* **3-wire I²S is enough**: `BCK`, `LRCK`, `DIN`. On the breakout, `SCK` may be
  strapped so the DAC derives its system clock from `BCK` (avoids a 4th pin).
* Strapping pads on the common purple PCM5102A board: `FLT`→GND (normal filter),
  `DEMP`→GND (de-emphasis off), `XSMT`→3V3 (unmute), `FMT`→GND (I²S), `SCK`→GND (clock
  from BCK). **These are solder bridges on the board itself, not wires you run** — the
  only DAC pads that get a wire are `BCK`, `DIN`, `LCK`, `GND`, `VIN` and `LOUT`, since
  `ROUT` is unused in a mono build.
* **`VIN` takes 5 V, not 3.3 V** — the board carries its own 3.3 V regulator. `A3V3` is
  that regulator's *output*, a different net: never strap the two together, as the
  DAC's supply maximum is 3.6 V. This also keeps the DAC off the S3 board's 3.3 V LDO.
* Verify the bridges with a meter rather than re-wiring: a floated `XSMT` mutes the DAC
  silently, and looks exactly like a firmware bug.
* **Series resistors on the I²S lines: check the DAC board first.** These boards
  commonly have one already in line with `BCK`, `DIN` and `LRCK` (marked `220`), and
  if yours does, adding a second pair buys nothing. Only fit ~100 Ω yourself if the
  board has none.
* ESP32-S3 has a GPIO matrix, so any of the free pins above can carry I²S — pick
  pins that are physically adjacent on the header.
* The PCM5102A is a **line-level DAC**: it needs an amplifier or a powered speaker
  (PAM8302A, TPA2016…) unless you feed an amp input. A MAX98357A (I²S + class-D,
  no separate DAC board) would use the same 3 pins and drive a speaker directly.
* Bandwidth note: camera DMA (from PSRAM) + I2S DMA share the octal PSRAM bus;
  expect occasional audio underruns at high camera resolutions unless you pin the
  audio buffers to internal RAM.

## 9. Build settings for N16R8 (PlatformIO / Arduino)

The module must be told it has **quad flash + octal PSRAM**, otherwise PSRAM is
never initialised and the camera driver runs out of framebuffer memory:

```ini
[env:esp32-s3-cam]
platform      = espressif32
board         = esp32-s3-devkitc-1
framework     = arduino
board_build.flash_size   = 16MB
board_build.flash_mode   = qio
board_build.arduino.memory_type = qio_opi   ; quad flash + OPI (octal) PSRAM
board_build.partitions   = default_16MB.csv
build_flags   = -DBOARD_HAS_PSRAM -DARDUINO_USB_CDC_ON_BOOT=1
monitor_speed = 115200
```

In the Arduino IDE the equivalents are **Flash Size = 16MB**, **PSRAM = OPI
PSRAM**, **USB CDC On Boot = Enabled**, **Flash Mode = QIO** (do *not* select an
OPI flash mode — the flash is quad). Note this uses `esp32-s3-devkitc-1` as the
closest generic target; a dedicated board definition may exist for your carrier.

**Running without PSRAM** (your decision): drop `-DBOARD_HAS_PSRAM` and don't
select an OPI PSRAM mode. The camera then allocates framebuffers from the 512 KB of
internal SRAM — fine for one buffer at QVGA/VGA JPEG, but the larger `framesize`
presets and double/triple buffering will fail. GPIO33–37 stay reserved either way,
as they are bonded to the PSRAM die inside the module.

## 10. Open questions

1. ✅ Board + header layout confirmed (Freenove 2 × 20 pinout).
2. ✅ No microSD, no UART console, PSRAM not required.
3. One MCU (Options 1/1b) or two (Options 2/3)? If two: **RP2040 body or the
   existing `esp32dev`?**
4. ✅ TB6612FNG it is, with `STBY` on `45` (its internal 200 kΩ pull-down does the
   `VDD_SPI` strap as a side effect). Any other driver needs re-checking.
5. PCM5102A + amplifier/speaker, or a MAX98357A straight into a speaker?
6. ✅ `48` carries the WS2812 — **resolved in favour of freeing it for I²S**, not for an
   LED. `48` became I²S `BCK` (part of an adjacent trio at header #15–#17 with `47`
   and `21`), the strip moved to `45`, and the on-board LED was given up. The payoff
   was `38`/`39`/`40` back as the on-board microSD slot plus `39`–`42` back as JTAG —
   see `circuit-plan.md` §2.1. This is the option §7 listed as "pick pins that are
   physically adjacent on the header", taken literally.
7. BMI sensor: **BMI160** (no config blob, easiest to bring up) or **BMI270**
   (Bosch's current part, needs an ~8 KB config file at every boot)? And poll it
   over I²C, or spend a third pin on `INT1` for wake-on-motion? Also: which MCU
   reads it — see §13.

## 11. Complete pinout table (all 45 ESP32-S3 GPIOs)

> **Provenance.** Taken from the Freenove pinout image for this carrier
> (`Freenove/Freenove_ESP32_S3_WROOM_Board · ESP32S3_Pinout.png`), cross-checked
> against the generic ESP32-S3-CAM references (`nulllaborg/esp32s3-cam`, the
> Freenove ESP32-S3-CAM N16R8 map) and Espressif's ESP32-S3 datasheet for the
> flash / PSRAM / strapping reservations. Camera, PSRAM and SD pin choices agree
> across all of them.

### 11.1 Physical header (2 × 20)

| # | Left row | Right row |
| --- | --- | --- |
| 1 | 3V3 | GPIO43 — U0TXD |
| 2 | RST | GPIO44 — U0RXD |
| 3 | GPIO4 — CAM_SIOD | GPIO1 — ADC1_CH0 |
| 4 | GPIO5 — CAM_SIOC | GPIO2 — ADC1_CH1 |
| 5 | GPIO6 — CAM_VSYNC | GPIO42 — MTMS |
| 6 | GPIO7 — CAM_HREF | GPIO41 — MTDI |
| 7 | GPIO15 — CAM_XCLK | GPIO40 — SD_DATA |
| 8 | GPIO16 — CAM_Y9 | GPIO39 — SD_CLK |
| 9 | GPIO17 — CAM_Y8 | GPIO38 — SD_CMD |
| 10 | GPIO18 — CAM_Y7 | GPIO37 — PSRAM |
| 11 | GPIO8 — CAM_Y4 | GPIO36 — PSRAM |
| 12 | GPIO3 — JTAG_EN | GPIO35 — PSRAM |
| 13 | GPIO46 — LOG | GPIO0 — BOOT |
| 14 | GPIO9 — CAM_Y3 | GPIO45 — VSPI |
| 15 | GPIO10 — CAM_Y5 | GPIO48 — WS2812 |
| 16 | GPIO11 — CAM_Y2 | GPIO47 |
| 17 | GPIO12 — CAM_Y6 | GPIO21 |
| 18 | GPIO13 — CAM_PCLK | GPIO20 — USB |
| 19 | GPIO14 | GPIO19 — USB |
| 20 | 5V | GND |

Read the left row as "the camera row" and the right row as "everything else".

### 11.2 GPIO by GPIO

Legend: **✔** free for the car · **△** usable but costs something · **✘** keep away.

| GPIO | What owns it on the S3-CAM | Free? | Notes |
| --- | --- | --- | --- |
| 0 | BOOT button + strapping | △ | button lost; must read high at reset |
| 1 | right row | ✔ | motor: TB6612 #1 `AIN2` |
| 2 | right row | ✔ | motor: TB6612 #1 `PWMB` |
| 3 | strapping — `JTAG EN` | ✔ | **I2S BCK**; floats at reset, either strap level is fine |
| 4 | camera SIOD (SCCB SDA) | ✘ | |
| 5 | camera SIOC (SCCB SCL) | ✘ | |
| 6 | camera VSYNC | ✘ | |
| 7 | camera HREF | ✘ | |
| 8 | camera D2 (Y4) | ✘ | |
| 9 | camera D1 (Y3) | ✘ | |
| 10 | camera D3 (Y5) | ✘ | |
| 11 | camera D0 (Y2) | ✘ | |
| 12 | camera D4 (Y6) | ✘ | |
| 13 | camera PCLK | ✘ | |
| 14 | left row | ✔ | **I2S DIN** |
| 15 | camera XCLK | ✘ | XTAL_32K_P |
| 16 | camera D7 (Y9) | ✘ | XTAL_32K_N |
| 17 | camera D6 (Y8) | ✘ | |
| 18 | camera D5 (Y7) | ✘ | |
| 19 | USB D− | ✘ | only flashing path — leave alone |
| 20 | USB D+ | ✘ | Freenove image swaps the D−/D+ labels |
| 21 | right row | ✔ | motor: TB6612 #2 `BIN2` |
| 22–25 | – | n/a | **do not exist** on ESP32-S3 |
| 26 | internal flash — SPICS1 | ✘ | |
| 27 | internal flash — SPIHD | ✘ | |
| 28 | internal flash — SPIWP | ✘ | |
| 29 | internal flash — SPICS0 | ✘ | |
| 30 | internal flash — SPICLK | ✘ | |
| 31 | internal flash — SPIQ | ✘ | |
| 32 | internal flash — SPID | ✘ | |
| 33, 34 | octal PSRAM — SPIIO4/5 | ✘ | not even exposed on the header |
| 35, 36, 37 | octal PSRAM — SPIIO6/7, SPIDQS | ✘ | on the right row, dead because of the R8 part |
| 38 | microSD — CMD | ✔ | motor: TB6612 #2 `AIN2` (TF slot unused) |
| 39 | microSD — CLK | ✔ | motor: TB6612 #2 `AIN1` (TF slot unused) |
| 40 | microSD — DAT0 | ✔ | motor: TB6612 #2 `PWMA` (TF slot unused) |
| 41 | header (JTAG MTDI) | ✔ | motor: TB6612 #1 `BIN2`; JTAG-over-pins lost |
| 42 | header (JTAG MTMS) | ✔ | motor: TB6612 #1 `BIN1`; JTAG-over-pins lost |
| 43 | U0TXD | ✔ | motor: TB6612 #1 `PWMA` (console on USB CDC only) |
| 44 | U0RXD | ✔ | motor: TB6612 #1 `AIN1` |
| 45 | strapping — VDD_SPI voltage | ✔ | **`STBY` goes here** — the driver's internal pull-down *is* the strap |
| 46 | strapping — ROM log printing | ✔ | **I2S LRCK**; floats at reset, either strap level is fine |
| 47 | right row | ✔ | motor: TB6612 #2 `BIN1` |
| 48 | on-board WS2812 LED | ✔ | motor: TB6612 #2 `PWMB` — the LED follows this PWM (see §12) |

**Summary:** the 13 usable right-row pins drive the 12 motor lines + `STBY`, and
`3 / 46 / 14` on the left carry I²S — 16 signals, 0 spare. The **BMI sensor's 2
pins are not in this count**; they only exist in the variants in §6.

### Camera FPC connector (24-pin, on-board)

| Signal | GPIO | Signal | GPIO |
| --- | --- | --- | --- |
| SIOD | 4 | Y6 / D4 | 12 |
| SIOC | 5 | Y7 / D5 | 18 |
| VSYNC | 6 | Y8 / D6 | 17 |
| HREF | 7 | Y9 / D7 | 16 |
| Y2 / D0 | 11 | PCLK | 13 |
| Y3 / D1 | 9 | XCLK | 15 |
| Y4 / D2 | 8 | PWDN | tied (n/c) |
| Y5 / D3 | 10 | RESET | tied (n/c) |

### microSD slot (SDMMC 1-bit)

| Signal | GPIO | Signal | GPIO |
| --- | --- | --- | --- |
| CMD | 38 | VDD | 3V3 |
| CLK | 39 | VSS | GND |
| DAT0 | 40 | DAT1/2/3 | not connected |

---

## 12. On-board WS2812 (`GPIO48`) and adding LED strips

**What the pin actually is.** On this carrier `GPIO48` *is* the data input of the
on-board addressable RGB LED — the header pin and the LED's `DIN` are the same net,
with no buffer in between. Worth confirming once with a meter (continuity from
header `48` to the LED's data pad), but the consequences are what matter:

1. **Anything you drive on `48` also feeds that LED.** A 2 kHz motor PWM on it makes
   the LED flash erratically and draw a few extra mA. That is not a dark LED, it is a
   broken indicator.
2. **The LED's `DOUT` is not on the header**, so a strip wired to `48` sits in
   *parallel* with the on-board LED rather than continuing from it: both see the same
   data stream and both display "pixel 0". You get a duplicated first pixel, not a
   chain.

**Driving it is cheap.** The `RMT` peripheral implements the 800 kHz single-wire
protocol in hardware — `neopixelWrite(48, r, g, b)` in the Arduino core, or
Adafruit_NeoPixel / FastLED for a strip. `RMT` is independent of `LEDC` (motors) and
`I2S` (audio), so nothing conflicts. A long strip takes ~30 µs per LED to refresh and
the bit timing is interrupt-sensitive, so update it from the main loop — not from an
ISR, and not inside the motor-ramp loop's tight window.

**Wiring a 5 V strip** — ~330–470 Ω in series with the data line, ≥1000 µF across the
strip's 5 V/GND, common ground with the board, and a level shifter if you want to stay
in spec: 3.3 V into a 5 V-powered WS2812 is below the 0.7 × V<sub>DD</sub> (3.5 V)
threshold. Budget ~60 mA per LED at full white, drawn from your 5 V rail rather than
from the ESP32 pin.

### Getting a pin for it

The layout in §7 uses **every one of the 16 available pins**, so freeing `48` costs a
signal somewhere.

*Can a motor line live on `45`?* **Yes.** `PWMB` carries the same 200 kΩ internal
pull-down as `STBY`, so it holds the `VDD_SPI` strap low at reset, and chopping `45`
at 2 kHz afterwards is harmless — that strap is latched at reset only. The trick that
works for `STBY` works for any TB6612 input.

*But `45` is currently `STBY`,* so the swap only buys you `48` if `STBY` stops being a
GPIO:

| Plan | Change | Result | Cost |
| --- | --- | --- | --- |
| **A** | tie `STBY` to 3V3, move `PWMB` from `48` → `45` | `48` free for LED data: 12 motors + 3 I²S + 1 LED = 16 | no software standby; "stop" = PWM 0 with `IN1`=`IN2`=low (coast) |
| **B** | 8-pin motor drive (§6): tie `PWMA`/`PWMB` high, PWM the `IN` pins | `STBY` stays on `45`, LED on `48`, **3 pins spare** | two wires to 3V3; off-time chops to coast instead of short brake |
| **C** | change nothing | `48` stays `PWMB`; the on-board LED is unusable | — |

> **Cheapest LED of all:** if a *status* light is the goal rather than a strip, plan A
> or B turns the on-board LED into exactly that — no extra hardware, no extra pin.
>
> On the **two-MCU** plans (§7 Options 2/3) this whole question disappears: with no
> motor pins on the S3, `48` stays the on-board LED's data line **and the strip gets a
> pin of its own** — in `circuit-plan.md` that is `47`, with `48` left alone. Hanging
> a strip on `48` itself still works, but it sits in *parallel* with the on-board LED
> (both show pixel 0), not in a chain.

---

## 13. BMI sensor (BMI160 / BMI270, 6-axis)

**What it is.** A Bosch 6-axis MEMS IMU — 3-axis accelerometer + 3-axis gyroscope
in one LGA-14 package. **No magnetometer**, so it measures attitude (pitch/roll)
and rotation rate well but has **no absolute heading**: yaw is dead-reckoned and
drifts. That is acceptable on a car — the wheels and the camera supply the heading
reference.

**Which part.**

| Part | I²C address | Notes |
| --- | --- | --- |
| **BMI160** | 0x68 / 0x69 | No configuration blob — FIFO, step counter, any-motion, tap and orientation all run from ROM. Easiest to bring up. |
| **BMI270** | 0x68 / 0x69 | Bosch's newer part, largely register-compatible, but it **must have an ~8 KB configuration file downloaded into RAM at every boot** before it produces data. The Bosch SensorAPI / SparkFun libraries do this; a hand-rolled register-poke driver will not work. |
| BMI088 | accel 0x18/0x19, gyro 0x68/0x69 | Two separate dies, drone-grade vibration behaviour, external I²C pull-ups required. Overkill here. |

Both BMI160 and BMI270 are **I²C *or* SPI** and both read out as 12 bytes
(`ax, ay, az, gx, gy, gz`). `SDO` selects the address, so two of them can share one
bus.

**Bus choice: I²C at 400 kHz.** SPI would cost 4 pins (`SCK`/`MOSI`/`MISO`/`CS`);
I²C costs 2. Check that your breakout has I²C pull-ups (most do) and that only one
set is fitted on the bus. `CS` must be pulled high for I²C mode — the common
GY-BMI160 board does this with an on-board resistor, while the Adafruit/SparkFun
breakouts use a solder jumper.

> ⚠️ **Do not hang it on the camera's SCCB pins (`4`/`5`).** The addresses do not
> collide (OV2640 = 0x30, BMI = 0x68), but `esp32-camera` owns those two pads with
> its own driver instance. That is a driver conflict, not a protocol one — use a
> real I²C controller on its own pins.

**Poll or interrupt?** A 12-byte read plus the register address is ~13 bytes ≈
330 µs at 400 kHz. Polling at 200 Hz costs ~7 % of the bus and **no pin at all**.
The `INT1`/`INT2` lines only earn a GPIO when you want a **wake-on-motion,
free-fall or tap** event, or a hardware-timed read inside a control loop. For
odometry: budget 2 pins, add the third only when you need it.

**Rates.** Gyro ODR 200 Hz for wheel odometry, accel 100 Hz for tilt. `±2000 dps`
and `±8 g` cover a car well. Enable the gyro low-pass filter.

**Mounting matters more than the part.** Rigid-mount it to the chassis, not to a
motor bracket; keep it clear of motor and speaker wiring; put a piece of soft foam
or gel under the PCB. Motor and gearbox vibration is what actually ruins readings.
Align the axes with the chassis before soldering so the code needs no rotation
matrix.

**In code.**

* **Gyro bias:** hold still ~1–2 s at boot and average, or the yaw rate integrates
  into a permanent turn.
* Gyro-only yaw drift is ~1–3 °/min after bias calibration — fine for a few seconds
  of odometry, not a compass.
* Want an absolute reference? Add a **BMM150 magnetometer** on the same bus (the
  classic Bosch 9-axis pair) — or sidestep the work entirely with a **BNO055**,
  which fuses on-chip and hands you a quaternion, at roughly twice the price.
* On the S3 the read is a plain I²C transaction in a task: no DMA, no ULP, no
  PSRAM involvement. Putting it on the Pico would be the same, but the pins that
  idea used (`GP16`/`GP17`) are now the link to the S3 — so the IMU stays on the
  S3's `1`/`2` bus, as the plan has it.

**Where it goes — summary.**

| Architecture | BMI sensor connected to | Extra pins |
| --- | --- | --- |
| Option 1 as drawn | — | ✘ impossible |
| Option 1 + 8-pin motor drive | S3 I²C | 2 of the 4 freed pins |
| Option 1 + 74HC595 | S3 I²C | 2 of the 4 freed pins |
| Option 1b (PCA9685) | **the existing `1`/`2` bus** | 0 |
| Option 3 (RP2040 body) | S3 I²C `1`/`2` — not the Pico; `GP16`/`GP17` became the link | 2, on the S3 |
