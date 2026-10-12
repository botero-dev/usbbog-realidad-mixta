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

There are **two environments**, and the difference between them is one flag:

```bash
pio run -d firmware/pico_body                        # build the SELF TEST (default env)
pio run -d firmware/pico_body -t upload              # flash the SELF TEST
pio run -d firmware/pico_body -e picolink -t upload  # flash LINK mode -- the real job
pio run -d firmware/pico_body -t monitor             # USB serial
```

**The self-test is the default on purpose.** It is still load-bearing: bring-up steps 4
and 5 (matching the printed `FL`/`RL`/`RR`/`FR` names to the actual wheels) are not
closed, and they need the sweep. A plain `pio run -t upload` should not silently replace
the tool that closes them, so putting the car into link mode is a deliberate choice.

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
| `include/link.h` `src/link.cpp` | **LINK mode.** Frame hunting, the 100 ms dead-man, the slew limit, the reply. `main()` is only a banner around `link::run()`. |
| `include/log.h` | `logf()` — `printf` plus a `fflush`, so a crash between two lines does not lose the one explaining why. |
| `include/status_led.h` | The on-board `GP25` LED as the "powered and running" light, guarded so it compiles to nothing on a Pico W (see below). |
| `src/main.cpp` | Two modes selected by `PICO_MODE_SELFTEST`: the bench self-test, or LINK. |

Two constant-time checks worth knowing about:

* `protocol.h` has a `static_assert` that CRC-8 (poly `0x07`) gives `0xF4` over
  `"123456789"`. A typo in the polynomial becomes a **compile error** instead of a link
  that silently rejects every frame. It is written as **recursion, not loops**, because a
  loop in a `constexpr` body is C++14 and the brain's Arduino core builds at
  `-std=gnu++11` — see the note in the file before "simplifying" it back.
* The self-test prints `GP28` *before* `motors::init()` as well as after. Before, it
  should read `0` because the RP2040 pad resets with its pull-down enabled — that is the
  property the whole fail-safe chain rests on, so it is worth seeing rather than trusting.
* The wire format has host-side tests that need no hardware at all:
  `firmware/brain/tools/test_link.cpp` (see the brain's README). They cover the CRC
  against an independent implementation, the round-trips, **every single-bit corruption**,
  the sync hunt in noise, and the mixer.

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

## LINK mode — the real job

`-e picolink` selects it. `main()` drives `STBY` low *before* the 1.5 s USB wait (the car
must be incapable of moving from the instant the firmware starts, not when a host happens
to attach), prints a short banner and calls `link::run()`, which never returns.

**UART0, `GP16` TX / `GP17` RX, 921600 8N1, no flow control.** Nothing about the link is
automatic: the Pico sends nothing until a valid frame arrives, starts with `STBY` low, and
there is no way to make the motors move except by sending frames.

The three jobs, in the order they matter:

1. **The dead-man.** No *motion command* for **100 ms** → `STBY` low, everything high-Z,
   one loud line in the log. This is the link's whole reason for existing: it is what
   stops the car when the S3 crashes, reboots, or the TX wire falls off.
2. **Framing.** `proto::SyncHunt` (in `protocol.h`, shared with the brain's end) hunts a
   13-byte frame out of the byte stream and accepts it only on a matching CRC.
3. **The slew limit.** `±100` permille per frame on `SET` frames. One bad frame cannot
   command a full-scale reversal.

The reply goes back at **10 Hz**: `STBY` state, link-good, watchdog-armed, a one-shot
fault code (`FAULT_CRC` / `FAULT_TIMEOUT`) and uptime in ms. The debug log is on the USB
port, so it never touches the link.

A `[link]` line every 2 s is the evidence:

```
[link]  200 frames/2s  ok  1406  crc 0  timeouts 0  stby 0  seq 125 type 04  duty    +0    +0    +0    +0
[link]    0 frames/2s  ok 14766  crc 0  timeouts 1  stby 0  seq 173 type 04  duty    +0    +0    +0    +0
*** DEAD-MAN: no motion command for 100 ms -> STBY LOW ***
```

These are **real captured lines** (2026-10-11), not an illustration:

* `200 frames/2s` is exactly 100 Hz, and `seq` advances `+200` per window — so the counter
  is a rate check as well as a liveness check.
* `type 04` is `DISABLE`, the brain's safe start state, which is why every duty reads `+0`
  and `stby` is `0`. A *driving* link would show `type 01` with non-zero duties; **that has
  not been exercised yet.**
* The second line is the S3 stopped underneath the Pico: `frames/2s` falls to zero,
  `timeouts` goes to `1`, and the dead-man announces itself. `timeouts` then **stays** at 1
  when the frames resume — it is one-shot per outage and re-arms on the next motion command.

### Two readings the wire format does not spell out

Both are settled in `protocol.h` and both are deliberate, because they are the kind of
thing that looks like an implementation detail and is actually a safety decision:

* **`PING` does not feed the dead-man.** Only `SET`/`BRAKE`/`COAST`/`DISABLE` do. If
  `PING` refreshed the timer, a brain that kept pinging but stopped sending duties would
  hold the last commanded duty **forever** — the timer running and protecting nothing.
  The reply still reports `PING` as *link-good*, because "is the S3 talking" and "does the
  S3 still own the motors" are different questions.
* **`DISABLE` is not latched.** It is a frame, not a mode: the next `SET` re-arms the
  drivers. A latch would need a re-enable message this protocol does not have, which would
  make `DISABLE` a one-way trip whose only exit was a reboot.

---

## Bring-up status

| Stage | State |
| --- | --- |
| 1 — PWM layer + self-test | ✅ **flashed and verified on a real Pico** (2026-10-11) |
| 2 — UART framing, CRC, decode | ✅ **verified, real S3 on the other end** (2026-10-11) |
| 3 — dead-man, `STBY` sequencing | ✅ **verified — provoked and fired** (2026-10-11) |
| 3 — slew limit | ⚠️ clamp covered by host tests; **never exercised** — no non-zero `SET` yet |
| 4 — reply frame + status counters | ✅ **verified** — the brain saw 10 Hz replies, `st 0x06` |

Stages 2–4 were closed in one bench session on 2026-10-11, with both boards wired to each
other: 100 Hz frames in with `crc 0`, 10 Hz replies out, and the dead-man **provoked
deliberately** by holding the S3 in its ROM bootloader. Three things that session did *not*
cover, listed so the ✅s above are not read as more than they say:

* **the literal wire-pull** — a stopped sender and a pulled wire both look like "no bytes",
  so the timer logic is identical; but a *floating* input injects noise, and noise could in
  principle produce a false-accept frame that refreshes the timer.
* **the slew limit** — see the table.
* **the motors** — `STBY` is LOW and nothing has been commanded to move.

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

**This paragraph is about stage 1 only.** The link is a separate story and it has its own
section above — verified on 2026-10-11. What the self-test establishes is the Pico's own
pins, so the scope checks on `GP0`/`GP5`/`GP10`/`GP15` (bring-up step 4 in §11) still stand
on their own.

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
