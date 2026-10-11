# `pico_body` — RP2040 firmware

The **body**: receives signed duties from the ESP32-S3 *brain* over a UART and turns
them into 4 × 20 kHz PWM + 8 direction lines + a shared `STBY`, with a 100 ms dead-man.

Authority for every pin and every number here is `docs/circuit-plan.md` — §3 (pin map),
§3.1 (PWM maths), §4 (frame format), §9 (fail-safe), §10.2 (firmware notes).

---

## Why the bare Pico SDK, and not Arduino

PlatformIO's stock `platformio/platform-raspberrypi` ships **only** the Mbed Arduino
core (`framework-arduino-mbed`) and bundles **no Pico SDK at all** — that was verified by
reading its `platform.json`, not assumed. The community fork
[`maxgerhardt/platform-raspberrypi`](https://github.com/maxgerhardt/platform-raspberrypi)
is the same platform with `framework = picosdk` added, so that is what this project
uses. It compiles the SDK with SCons directly — no CMake — and defaults stdio to USB.

The measured difference on this exact program:

| Core | RAM | Notes |
| --- | --- | --- |
| Arduino Mbed OS | **40 776 B** | Mbed RTOS + driver layer, for a program with no dynamic allocation |
| **bare Pico SDK** | **6 700 B** | no RTOS, no scheduler, deterministic |

Flash is not comparable between the two — the Mbed build reported only the sketch's own
contribution, while this build statically links the whole SDK (30 KB total).

> The SDK figure is from the most recent build (`RAM: 2.6%`). An earlier measurement in
> the same session read **9 708 B** RAM / **33 216 B** flash, and I never reconciled the
> ~3 KB difference — the same SDK build, with only `main.cpp` changing in between. The
> current number is the one to trust; the Mbed number is indicative either way.

The code is written against the SDK's own names (`pwm_set_clkdiv`, `pwm_set_wrap`,
`pwm_set_chan_level`, `gpio_init`), which is what §10.2 specifies.

---

## Build, flash, watch

```bash
pio run -d firmware/pico_body                 # build
pio run -d firmware/pico_body -t upload       # flash
pio run -d firmware/pico_body -t monitor      # USB serial
```

`printf()` goes out of the **USB** port and claims no GPIO. That is deliberate: UART0's
default pins are `GP0`/`GP1`, which are `DRV1_PWMA` and `DRV1_AIN1` on this board, so a
console that silently became UART0 would fight the motors. It also leaves UART0 free for
the link to the S3 on `GP16`/`GP17`.

First flash of a blank board needs **BOOTSEL** held (or drag `firmware.uf2` onto the
`RPI-RP2` drive). After that `-t upload` resets it via picotool on its own.

> Losing the boot banner to a slow USB attach? Uncomment
> `-DPICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=3000` in `platformio.ini`.

---

## Layout

| File | Role |
| --- | --- |
| `include/protocol.h` | **The contract.** Frame layout, CRC-8, duty limits, motor order. Pure C++, no SDK or Arduino dependency, so it compiles on the host for testing. |
| `include/motors.h` `src/motors.cpp` | PWM + direction layer. Owns every electrical subtlety. |
| `include/log.h` | `logf()` — `printf` plus a `fflush`, so a crash between two lines does not lose the one explaining why. |
| `include/status_led.h` | The on-board `GP25` LED as the "powered and running" light, guarded so it compiles to nothing on a Pico W (see below). |
| `src/main.cpp` | Self-test today; the link layer next. |

Two constant-time checks worth knowing about:

* `protocol.h` has a `static_assert` that CRC-8 (poly `0x07`) gives `0xF4` over
  `"123456789"`. A typo in the polynomial becomes a **compile error** instead of a link
  that silently rejects every frame.
* The self-test prints `GP28` *before* `motors::init()` as well as after. Before, it
  should read `0` because the RP2040 pad resets with its pull-down enabled — that is the
  property the whole fail-safe chain rests on, so it is worth seeing rather than trusting.

---

## Two details that are easy to get wrong

**`printf` has no float support.** The Pico SDK's newlib is built without it, so a
`%.1f` prints as **nothing at all** — not a wrong number, just absent. This bit the
self-test's percentage column, which came out as `(%)`. Compute with integers instead
(as `sweep_motor()` now does), or enable float printf and pay the flash and stack for
it. Nothing in this firmware needs floats.

**Coast needs the PWM line HIGH.** On a TB6612, `IN1=IN2=L` with `PWM=H` is coast
(high-Z); the same pins with `PWM=L` are **short brake**. So `duty == 0` programmes the
compare register to 6250 (always high), not 0. Getting this backwards silently turns
every "coast" into a brake and is hard to spot by watching the car.

**Order matters on a direction change.** `apply_state()` zeroes the PWM *before* moving
the `IN` pins, then applies the new level. Reversing the pins while the PWM is high puts
a full-voltage reverse pulse through the motor and into the supply.

---

## The status LED

The plain Pico has **no power LED**. The one LED it does have sits behind `GP25`, and it
is a *user* LED: dark from power-up until firmware drives it. So a board that is powered
and perfectly happy looks identical to a dead one — which is the wrong thing to stare at
while debugging.

`main()` now lights it as its **first instruction**, before `stdio_init_all()` and before
the 1.5 s USB wait, so it does not depend on a terminal being attached:

```c
status_led::init();
status_led::on();
```

| LED | Means |
| --- | --- |
| solid | the RP2040 booted and reached `main()` |
| dark | no power, **or** powered with blank/corrupt flash, **or** a Pico W build |

**It is a firmware-alive light, not a power light.** It cannot light before the RP2040
boots, so with a blank flash it says nothing at all about whether power is present. A
light that means "this board has 3.3 V" on its own has to be an LED and a resistor across
`3V3` and `GND` in hardware. That option was considered and **declined** — see the decision
in `docs/circuit-plan.md` §13 item 8 — so the on-board light is the only indication for
now. Worth remembering both limits: it has no header pin, so it is invisible inside an
enclosure, and it goes dark if the flash is blank.

`status_led.h` is guarded on `PICO_DEFAULT_LED_PIN`, which the SDK defines for a plain
Pico and deliberately does **not** define for a Pico W (there the LED is reached through
the wireless driver). That guard is not decoration: `pico_w.h` also defines
`CYW43_DEFAULT_PIN_WL_CS 25u`, so on a W, `GP25` is the wireless chip's SPI **chip
select**. Better to compile the whole header away than to assert a line that belongs to
the radio. Building for `board = pico_w` therefore prints `status LED : ABSENT (...)`
rather than claiming a light that is not there.

**Re-flashed 2026-10-11 and confirmed in the banner:** `status LED : GP25 lit solid
(plain Pico)`. That line is only reached on the branch where the guard found
`PICO_DEFAULT_LED_PIN`, and `main()` calls `status_led::on()` before printing it, so the
pin is driven from the first instruction. Whether it *looks* lit is the one thing a
terminal cannot tell you — that part is eyes only.

---

## Bring-up status

| Stage | State |
| --- | --- |
| 1 — PWM layer + self-test | ✅ **flashed and verified on a real Pico** (2026-10-11) |
| 2 — UART framing, CRC, decode | not started |
| 3 — dead-man, slew limit, `STBY` sequencing | not started |
| 4 — reply frame + status counters | not started |

**Stage 1 was verified on hardware on 2026-10-11**, flashed with `pio run -t upload` via
picotool with the board in BOOTSEL. Actual output:

```
pico_body  --  SELF TEST (no link, no S3 required)
=================================================
system clock   : 125000000 Hz
PWM            : clkdiv 1.0, wrap 6249 -> 20000 Hz, 6250 steps
status LED     : GP25 lit solid (plain Pico)
GP28 before init: 0  (expect 0 -- pad pull-down)
GP28 after init : 0  (expect 0 -- driven low)
state             FL    level  RL    level  RR    level  FR    level
after init        coast L   0  coast L   0  coast L   0  coast L   0   STBY=0
stby raised       coast L   0  coast L   0  coast L   0  coast L   0   STBY=1
```

What that actually establishes, in order of importance:

* **`GP28 before init: 0`** — before firmware touches the pin, the RP2040's pad
  pull-down already holds `STBY` low. That is the property the entire fail-safe chain
  rests on, and it is now **measured** rather than inferred from the datasheet.
* **All four TB6612 modes observed**: `fwd`, `rev`, `brake`, `coast` — with
  **`coast` at level 6250**, the PWM-high case that is easy to get backwards.
* Levels track duty exactly: `+250 → 1562`, `+500 → 3125`, `+750 → 4687`,
  `+1000 → 6250`.
* `disable_all` lands at `STBY=0` with every level zeroed — the dead-man's end state.
* The clock and PWM readouts confirm the arithmetic on the silicon, not just on paper.

**Not verified: the link.** No frames have been received and the dead-man has never
fired. Scope checks on `GP0`/`GP5`/`GP10`/`GP15` (bring-up step 4 in §11) still stand.

### Then it ran on the car

On the same day the same firmware was run with **all four motors wired to both
ROB-14450s and powered from the real 3S pack through the XL4015 and the LM2596**, and the
motors turned. So this self-test has since exercised the whole power tree under motor
load, not just the Pico's own pins.

That run leaves three things open. They are worth stating plainly rather than letting
"it worked" cover them:

| Open | Why it matters |
| --- | --- |
| **which channel is which wheel** | Nobody has yet watched the wheels against the printed sweep names, so the channel table is only as good as the wiring assumption behind it |
| **direction** | That all four turn the same way on `+duty` is unchecked — a reversed motor stays invisible until the car drives crooked |
| **brownout** | Nobody was watching for a reset during the all-four ramp. The on-board LED makes this easy now: **if it goes dark and the banner reprints, that was a reset** |

Close the first two **before** the link work — a swapped pair or a reversed motor makes
every later symptom ambiguous. The firmware already flashed does the job:

**Identifying the motors, ~30 seconds, no rebuild.** The self-test sweeps one motor at a
time in channel-table order and prints the name first — `-- FL sweep (driver #1 channel
A) --`, then `RL`, `RR`, `FR`. Each sweep runs about 6 s, and within it the **first half
is positive duty (forward)** and the second half negative. So: watch which wheel moves
during each 6-second window, and which way it turns during that first half.

* four windows, four wheels, printed names matching → the channel table is right
* a wheel moves but the printed name is a different position → tell me which, and it is
  one line in `src/motors.cpp`
* the positive half drives a wheel *backwards* → swap that motor's two leads at the
  ROB-14450, or that channel's `IN` pins in the table

The log also says which driver board each sweep uses (`driver #1` = `GP0`–`GP5`, `#2` =
`GP10`–`GP15`), so a swapped pair shows up as the wrong board twitching.

The sweep passes each motor through `0 → +1000 → 0 → −1000 → 0`, which exercises the
state-change path in both directions. **Wheels off the ground.**
