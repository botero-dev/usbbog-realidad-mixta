# `brain` — ESP32-S3 firmware

The **S3 side** of the project: the camera, the audio chain, and the Wi-Fi access point
that serves both — in one firmware, as of 2026-10-11.

This is the **merge** of `firmware/cam-test` back into `brain`. Each was brought up and
verified on its own, on purpose, so that a failure had exactly one candidate. Merged, they
answer the question neither could ask alone: **do the camera and the audio chain
coexist?** §6 predicts they might not, because the camera's DMA and the I²S DMA both reach
into the octal PSRAM bus.

It is also now the **brain of the car**: it drives the motors over a UART link to the
RP2040 Pico (§ *The motors, and the link to the Pico* below), and it has absorbed the
mixing code that used to sit in `firmware/car_control` (§10.3).

> ✅ **Both halves verified separately, 2026-10-11.**
> * **Audio:** three beeps out of the PCM5102A + class-D amp + speaker, first try once the
>   build configuration was fixed — so `S3 → DAC → amp → speaker` is verified end to end,
>   and `i2s_driver_install()` / `i2s_set_pin()` both succeed with the permuted pins
>   (`47`/`21`/`45`).
> * **Camera:** in `firmware/cam-test` — the sensor is detected as an **OV3660**
>   (`PID 0x3660`, confirmed on hardware), the MJPEG stream runs, and the frame buffers sit
>   in PSRAM. It is mounted rotated, so a vertical flip is applied at boot.
>
> What is **still** not verified is §11's other half: *"and still clean while the camera
> streams"*. That is exactly what this merge makes testable, and it is the next thing to
> check by ear.
>
> Volume is `VOLUME` in `src/audio_player.cpp` — a tenth of the first revision, which was
> much too loud into a small speaker.

Authority for every pin and every number here is `docs/circuit-plan.md` — §6 (audio
chain), §10.1 (S3 firmware notes), §11 (bring-up order). It is intended to grow into the
"brain" of §1 — WiFi, camera, IMU and the control loop — and to absorb the mixing code
in `firmware/car_control` (§10.3).

---

## Audio is silent until asked

Two sounds, both requested from the page or from `/play`, and **nothing plays by default**:

| Button | What it is | Entry point |
| --- | --- | --- |
| **beeps** | the 440 / 660 / 880 Hz test tones | `audio_request_beeps()` |
| **sound file** | the embedded 4.14 s clip (`clip.pcm` → `audio_clip.h`) | `audio_request_clip()` |
| **very low · low · medium · high · very high** | select the level for the sound buttons: 1 / 2 / 5 / 10 / 20 % of full scale | `/control?vol=N` |

The level buttons **only select** — they play nothing, so a level can be chosen without
making a noise. Press **beeps** to hear it. They exist because *"does it distort at this
level?"* turned out to be the single most useful question while chasing a crackle: they
make one press an A/B test instead of a reflash. `vol` is in **per-mille** (`vol=50` → 5 % →
about -26 dBFS), because percent cannot express the quiet end and floats in a query string
are extra code for nothing.

The page also keeps its own copy of the five levels, so it can *name* the level it is
actually at — including one set by `curl` that matches no button, which is then shown as a
bare percentage rather than mislabelled.

The level is **deferred, not applied immediately**: changing it rebuilds the 1024-entry
sine table, and doing that while the audio task is reading it would be an audible click.
So the HTTP side records the level and the audio task applies it just before the next
sound. `/status` reports it as `volume_milli`, and `/control?vol=N` sets it without
playing anything.

Two reasons for no autoplay. A board that makes noise the moment it powers up is a board
you cannot debug — *"is the amp hissing?"* is unanswerable while the firmware is also
beeping. And on a car, sound should be a decision rather than a side effect of booting.

**Playback runs in its own task, never in the HTTP handler.** `i2s_write()` blocks for the
length of the sound, and that handler's task is the one serving `/stream`, so playing
inline would freeze the video every time a button was pressed — a confusing thing to debug.
A new request supersedes one already playing, at the next ~6 ms buffer boundary.

The reason the first bring-up was a *tone* still holds, and it is why the button is
labelled "beeps": a tone is a chain you can verify, whereas playing a file exercises the
filesystem, the decoder, the I²S format, the DAC and the amp at once, and a failure tells
you nothing about which one.

---

## The web page and its endpoints

| Endpoint | What it does |
| --- | --- |
| `GET /` | The page: the live stream, the sound buttons and the beep-level buttons. |
| `GET /stream` | The video — `multipart/x-mixed-replace`, one JPEG per part. **On port 81** — see below. |
| `GET /capture` | One JPEG snapshot. |
| `GET /status` | JSON: heap, PSRAM, stream clients, fps, `audio`, sensor settings, LED. |
| `GET\|POST /play` | `?src=beeps` or `?src=clip`, plus an optional `&vol=0..1000` (per-mille) to set the level with it. Returns immediately; playback happens in the audio task. |
| `GET\|POST /control` | Sensor settings, plus the lights: `mode=run\|torch\|hazard\|off`, `group=<name>&r/g/b` for one named group, `led_r/g/b` for the whole strip, and `vol=0..1000`. |
| `GET\|POST /drive` | The motors: `?th=..&turn=..&twist=..` (mixer, per-mille ±1000) **or** `?fl=..&rl=..&rr=..&fr=..` (the raw four duties, all four required). |
| `GET\|POST /stop` | `?mode=brake` (**default**) / `coast` / `disable`. A stop that coasts is not a stop, so brake is the default. |

**Two ports, and it is not cosmetic.** `esp_http_server` runs *all* of a server's handlers
in **one** FreeRTOS task — `httpd_config_t` has a single `task_priority` — and our
`/stream` handler never returns while a client is watching, because that is what MJPEG is.
So if the stream shared port 80, **every other endpoint would go dead the moment anyone
opened the video**, and queued button presses would sit unanswered until the stream was
torn down (which is why "press the button, nothing happens, then reset the board and they
all fire at once" is the exact symptom). Priority cannot help: the task is not spinning,
it is parked inside the stream loop, so there is nothing to preempt. The stream therefore
gets its own server on **port 81**, where blocking forever is harmless. The page builds
the URL from `location.hostname`, so it follows whatever address the board is reached on.

> ⚠️ **If you ever add a third server, set its `ctrl_port` too.** `httpd_config_t` has a
> second, less obvious port used internally, and `HTTPD_DEFAULT_CONFIG()` hands *every*
> instance the same value. Two instances therefore collide and the second one fails at boot
> with `error in creating ctrl socket (112)` — where 112 is `EADDRINUSE` and the message
> points at neither the port you set nor the socket you think you asked for. The fix is one
> line: `config.ctrl_port = 32768 + port;`.

The buttons use `fetch()`, not links, and that is not decoration: a plain
`<a href="/play?...">` would **navigate away** from the page and kill the stream's socket,
so pressing "beeps" would look like it had stopped the video.

**The strip on `GPIO48` is the car's lighting**, not a status indicator: it comes on with the
car and stays on. The pixels are named groups (see *The layout* below), and the firmware's
default state is an always-on scene.

| State | Set with | What it is |
| --- | --- | --- |
| `run` | `?mode=run` | **the boot default**: head lights white and tail lights red, always on at 20/255, with the floor sweeping the hue wheel at the same level |
| `torch` | `?mode=torch` | that same scene with the head lights at **128/255** |
| `hazard` | `?mode=hazard` | that same scene with the head and tail **side** lights flashing yellow at **64/255**, ~1.25 Hz || `off` | `?mode=off` | every pixel to zero — all lights dark || `custom` | `?group=..&r/g/b`, or `?led_r/g/b` | a manual per-group scene — the override underneath the three automatic states |

Run, Torch and Hazard are **the same scene differing in one override**, and they share a
single builder (`scene_render()`). That is deliberate, not tidiness: there is exactly one
place the tail lights are turned on, so there is no state in which they can be forgotten.

`/status` reports the state as `led_mode`, and the page's **run / torch / hazard / off**
buttons are *indicators*, not toggles — none of them assumes a state, so a state set by `curl`
shows up on the page correctly. `hazard` flashes **off and on** rather than switching colour:
falling back to the running lights for the off half would read as a colour change, not a
flash.

`off` is a **state**, not a one-off command, so "the lights are off" is something `/status`
reports and the page can show rather than something you have to remember. It is re-sent on
every 30 ms tick instead of once: that costs a 1.4 ms write to hold a dark strip, and buys
back the ability to **recover** — a strip that loses a frame (a marginal data line, a supply
dip) is repainted 30 ms later rather than staying wrong until the next command. Worth more
than 5 % of an otherwise empty `loop()` while the strip's data level is still unverified.

> **`off` is not a useful corruption test**, precisely because it re-sends and because a
> wrong colour on a strip that should be dark is invisible. To test the data path, hold a
> *visible* static frame in **custom** instead — `?led_r=20&led_g=20&led_b=20` sends once and
> then the firmware stops writing entirely, so any flicker after that is hardware.

**What is gone, deliberately.** The old status indication (dim blue idle / green streaming /
white audio) and the whole-strip rainbow. The lights are on whenever the car is, so the strip
can no longer double as a debug indicator — *"is anyone streaming?"* is answered by the page
or by `/status`.

### Levels, and the ceiling

Every brightness is a **named level** applied through `level_channel()`, so a colour and a
brightness stay independent — *"make the tail red"* and *"make the tail dimmer"* are separate
one-line changes rather than a hunt for a literal:

| Level | Value | What it lights |
| --- | --- | --- |
| `kAmbientLevel` | 20/255 | head + tail, always on |
| `kFloorLevel` | 20/255 | the floor's sweep |
| `kHazardLevel` | 64/255 | the flashing side lights |
| `kTorchLevel` | 128/255 | the head lights, torch on |

`kLedMaxChannel = 128` is the **ceiling**, enforced in `led_push()` — the single gate every
frame from every state passes through, so no mode and no query string can get around it. It is
a safety limit rather than a brightness setting: it is chosen by the brightest thing in the
catalogue and only stops something from going past it.

> **The sequence matters, so record it: the ceiling used to be 20.** A global cap cannot
express "head at 128, floor at 20", so when the torch arrived the cap moved to 128 and 20
*became a per-mode level* instead of a global one. The guarantee is now "nothing exceeds
128", **not** "nothing exceeds 20".
>
> Max draw with everything lit: 4 head pixels at 128 (~60 mA) plus 25 at 20 (~120 mA) —
> **about 180 mA**, inside a USB 2.0 port's budget. The per-frame cost is below.

It is driven over the **RMT peripheral** — never PWM, which on a WS2812 data line is not
"dim" but corrupted bit timing. Not with the core's `neopixelWrite()` though: that sends
exactly **one** pixel and core 2.0.17 has no N-pixel API, so a strip would show a single
LED and stale colour after it. `strip_show()` drives RMT through the same wrapper the core
uses, with the core's own bit timing (100 ns/tick, `T0H` 0.4 µs, `T1H` 0.8 µs — lifted from
`esp32-hal-rgb-led.c`, not re-invented), and sends the latch explicitly (600 µs) so both
strip variants agree — the classic wants ≥50 µs, the ECO/V5 ≥280 µs.

**All 29 pixels are addressed every frame.** The strip's own first pixel is **dead** —
reverse-polarity damage on 2026-10-12, before the wiring was corrected — and it is bypassed,
so the data lead feeds the second chip and these 29 are the pixels that still work. §2.1 still
applies: the on-board WS2812 is on the same net, so it shows **pixel 0** of whatever is sent.

The floor's hue step is `(p * 256) / count`, **not** `p * (256 / count)`. At 17 pixels the two
differ by one hue unit and it would not matter; the correct form is there because at 29 pixels
the naive one steps by `floor(256/29) = 8`, spans only 224 of the 256 hue units, and leaves a
**32-unit seam** against a step of 8 — which is visible. Written right once, it cannot be
wrong for a group of a different length.

The hue is carried in **byte units** (0-255 is a full turn), so it wraps with a plain
`uint8_t` add and there is no 360-vs-0 branch to get wrong.

### The layout: 29 pixels, named

Labelled by the user, 2026-10-12. `kGroups[]` in `app_httpd.cpp` is the single source of
truth: the boot banner prints it, `/control?group=` resolves against it, and the compiler
checks it.

| Group | Indices | Pixels |
| --- | --- | --- |
| `tail_right` | 0–1 | 2 |
| `tail_centre` | 2–5 | 4 |
| `tail_left` | 6–7 | 2 |
| `floor` | 8–24 | 17 |
| `head_left` | 25–26 | 2 |
| `head_right` | 27–28 | 2 |

The banner states it on every boot, and from the table rather than from a hand-copied
string — so it cannot drift:

```
[led] 29 pixels:  tail_right 0-1  tail_centre 2-5  tail_left 6-7  floor 8-24  head_left 25-26  head_right 27-28
```

**Index 0 is not the strip's first chip.** Pixel 1 of the strip is dead and bypassed, so
firmware index 0 lands on its *second* chip. Rewire the strip and every label moves.

`?group=<name>&r=..&g=..&b=..` lights one group and opens the **Custom** mode, where the
frame is a per-pixel scene that group writes modify. The groups **compose**:

```bash
curl 'http://192.168.4.1/control?led_r=255&led_g=0&led_b=0'   # the whole strip red
curl 'http://192.168.4.1/control?group=floor&b=255'           # floor blue, rest still red
```

Missing `r`/`g`/`b` in a group write are 0, and a request carrying `group` is *always* a group
write — handled on its own — so it can never be confused with the whole-strip `led_r/g/b`.
A group write and a whole-strip colour both land in **Custom** and stay there: the renderer
deliberately leaves that state alone, because rebuilding it 30 ms later would throw the
user's work away. `?mode=run` (or torch, or hazard) is how you hand it back to the renderer.

To check the order is real, light the groups one at a time and count them:
`?group=tail_right&r=255`, then `tail_centre`, `tail_left`, `floor` (17 pixels — the long one),
`head_left`, `head_right`.

> **The tiling is checked by the compiler, not by eye.** A recursive `constexpr` — a loop
> inside a `constexpr` body is C++14, and this core builds with `-std=gnu++11`, the same trap
> that stopped the link's CRC compiling — asserts that each group starts where the previous
> one ended and the last ends exactly at `kStripLeds`. A gap or an overlap is a lighting bug
> that would otherwise only appear on the car at night. **Verified by deliberately breaking
> it:** changing the floor to 16 pixels fails the build with
> *"LED groups must tile 0..kStripLeds-1: no gap, no overlap, no extras"*. Prove a guard
> fires once, or it is decoration.

> ⚠️ **What it costs now.** Uncapped, full white across 29 pixels would be ~1.7 A (60 mA per
> WS2812B) — well past the LM2596's comfort and 3.4× a USB 2.0 port's entire budget. With the
> levels above the whole strip lit at once is **~180 mA** (4 head pixels at 128 plus 25 at 20),
> which stays inside the budget.
>
> The write **blocks the caller for ~1.4 ms** (836 µs of bits + the 600 µs latch), so it is
> called from `loop()` and never from an ISR — and the renderer calls it **every 30 ms**, not
> once per state change, because the floor moves. That is ~5 % of `loop()` forever. It is
> affordable while `loop()` is empty; when the control loop arrives (§10.1) this has to move
> off the control path or go non-blocking.
>
> **One unverified margin.** The RMT channel is given one memory block — **48 items =
> 57.6 µs** — and `rmt_write_items()` refills it from its ISR, so a 697-item frame needs that
> ISR to run every 57.6 µs. Nothing has glitched, but if the *far* end of the strip ever
> shows a wrong colour under Wi-Fi load, that is the mechanism. The one-argument fix is
> `RMT_MEM_192` (3 blocks, since `RMT_DEFAULT_ARD_CONFIG_TX` sets `mem_block_num = buffers`,
> giving 173 µs of slack) — worth testing before trusting, because `rmtInit()` returning
> NULL costs the LED entirely.
>
> The write **blocks the caller for ~830 µs** (230 µs of bits + the 600 µs latch), so it is
> called from `loop()` and never from an ISR. When the control loop arrives (§10.1) this
> has to move off the control path or go non-blocking.

---

## The motors, and the link to the Pico

The S3 does not touch a motor pin. It encodes four signed duties into a **13-byte frame at
100 Hz** and sends them to the Pico on **UART1**; the Pico turns them into PWM. The wire
format is `protocol.h` — one file, living in `firmware/pico_body/include/` and compiled by
*both* projects (`-I${PROJECT_DIR}/../pico_body/include`), so the two ends cannot drift
apart.

### It is NOT on GPIO43/44, whatever the plan said

Plan §2 put the link on `43`/`44` (`U0TXD`/`U0RXD`). On this carrier those two pins **are
the USB console** — the USB-C port goes through a bridge to them — so a link there would
have cost the serial log, which is how every diagnosis in this file was made. Measured,
twice: a build with `-DARDUINO_USB_CDC_ON_BOOT=1` (asking for a native-USB console
instead) produced **no output anywhere**, and that is what settles it.

The ESP32-S3's GPIO matrix routes a UART to any pin, so the link moved to **UART1 on
`GPIO1` (TX) / `GPIO2` (RX)** — the two pins the I²C move had just freed.

| | |
| --- | --- |
| `GPIO1` TX → | Pico `GP17` |
| `GPIO2` RX ← | Pico `GP16` |

* **Cost:** the S3 is now out of spare analog inputs — `GPIO1`/`GPIO2` were `ADC1_CH0`
  and `ADC1_CH1`.
* **Gain, beyond the log:** the ROM bootloader banners on UART0 *only*, so the Pico no
  longer sees ~30 bytes of garbage on every S3 reset. Plan §2.3's banner note no longer
  describes this build.

### Two things the Pico's dead-man does NOT cover

The Pico drops `STBY` if frames stop arriving for 100 ms. That covers the brain crashing,
rebooting, or the TX wire coming off — and nothing else.

**It does not cover a lost client.** If the phone sleeps, leaves Wi-Fi range, or the page
is closed, the brain is perfectly healthy and keeps sending the last duty it was told: the
car keeps driving, at speed, with nobody holding a control, and the Pico cannot tell the
difference — from its side nothing has changed. So the brain has its own watchdog:
**no `/drive` for 250 ms → `BRAKE`** (`kControlTimeoutMs` in `motor_link.h`). Deliberately
brake and not coast, because a coasting car keeps rolling, and the point of the watchdog is
to stop it.

That is also why **the page re-sends at 10 Hz while a control is held** — the watchdog
makes that necessary rather than merely harmless, and it is what turns a dropped Wi-Fi link
into a stopped car. Releasing a button sends `/stop` explicitly instead of waiting for the
watchdog, because a quarter second of coast on every release is not what "stop" means.

### The page controls

Hold a direction; releasing brakes. Arrow keys and WASD work too, the speed slider scales
the command, and `STOP` is always reachable. The buttons use **pointer events with
`setPointerCapture`**, not clicks, for a specific reason: without capture, a finger that
slides off a button never delivers `pointerup`, and the car would keep driving with nothing
held down. `blur` clears everything for the same reason — a key held while the tab loses
focus never gets its `keyup`.

> ⚠️ **The direction signs are UNVERIFIED.** They are `car_control`'s, kept exactly, and
> that code's own comment called `+turn` "left" while producing duties (left wheels
> forward, right wheels back) that rotate a car the other way. Which channel is which wheel
> is bring-up steps 4/5 and is **still open** — the arithmetic is certain, the mapping is
> not, and no amount of firmware can settle it. **Wheels off the ground for the first
> press.**

The `/status` JSON carries the link state under `link_*` and `pico.*` (`seen`, `status`,
`fault`, `age_ms`), so "is the Pico talking, and how long ago?" is answerable from a
browser. The page shows the same thing under the drive pad, refreshed once a second.

### Host-side tests — no hardware needed

```bash
cd firmware/brain
g++ -std=gnu++11 -Wall -Wextra -I include -I ../pico_body/include tools/test_link.cpp -o /tmp/test_link
/tmp/test_link
```

**317 checks.** The CRC against an independently written loop implementation over every
length from 0 to 64; frame and reply round-trips over all five message types; **every
single-bit corruption** in both formats rejected (a corrupt frame that is *obeyed* is the
failure the whole design exists to prevent); the sync hunt fed a frame buried in bytes that
contain the sync pattern; and the mixer, including a case where the 32-bit intermediate
changes the answer.

`-std=gnu++11` is deliberate — that is what this core builds with, so the test doubles as
proof that `protocol.h` has not drifted into C++14. It **had**: the CRC used loops inside
`constexpr` bodies, which GCC rejects at `gnu++11` (`body of 'constexpr' function not a
return-statement`), so the brain could not have compiled it at all. It is recursion now,
and the test is what keeps it that way.

The same test also found a real property of the design worth knowing: feeding a burst of
noise and then a frame produces a **coincidental** second acceptance, because a random
window that begins with the sync pattern passes CRC-8 about 1 time in 256. That is inherent
to an 8-bit CRC, not a bug — the real frame is still recovered intact — and what bounds the
damage is the Pico's slew limit: a false accept can command at most one frame, ±100
permille, until the next real frame arrives 10 ms later.

---

## The IMU (BMI160-family) — and what this particular part actually is

Sampled at **200 Hz in its own task, pinned to core 1 at priority 5** — the slot plan
§10.1 reserves for the control task, so the control law can join it later rather than
move it. The I²C bus runs at **100 kHz, not the 400 kHz the plan asks for**: at 400 kHz
this part ACKs and reads fine but its **config writes are dropped**, and the accelerometer
comes up reporting 0.000 g. See decision 4 at the top of `src/imu.cpp`.

`/imu` returns the latest sample as JSON, and the page polls it at **15 Hz** for a
live readout with accel bars. Nothing in the HTTP path ever touches the bus: the sampler
owns it, and a handler only copies the most recent sample.

Current reading on a still board:

```
[imu] resting |a| = 1.054 g over 200 samples (still board = 1.000)
[imu] gravity check PASSED -- the accelerometer scale is right
[imu] a 0.287 -0.568 0.839 g | w 0.1 -0.4 -0.1 dps | tilt -15.8/-34.1 deg | 31.3 C | seq 801 err 0
```

> ⚠️ **This module is NOT a plain BMI160, and three things follow from that.**
>
> | What | Measured | Consequence |
> | --- | --- | --- |
> | `CHIP_ID` (0x00) | **`0xD1`** — correct for a BMI160 | Corrected 2026-10-11: `0xD1` **is** the BMI160's ID (Bosch's `BMI160_driver` defines `BMI160_CHIP_ID` as `0xD1`); `0x24` is the **BMI270**, which needs an ~8 KB config blob at boot. The **BMX160** reports `0xD1` too, so the ID cannot separate the two — and it does say nothing about a magnetometer. Only the silkscreen can. |
> | `ACC_RANGE` | reads back `0x02` (±8 g) and **lies**: the data is at ±2 g | `kAccelRangeCorrection = 4.0f`. A still board read 4.20 g before this |
> | `GYR_CONF` | reads back `0x1a`, wanted `0x9a` — the **ODR field silently does not take** | Fine for tilt. **Not fine for integrating yaw** — fix before gyro odometry depends on it |
>
> The rule that follows, and the reason the correction is a named constant rather than a
> number buried in an expression: **trust the physics over the register.** A stationary
> board *must* read 1 g, and `report_gravity()` says so out loud one second after every
> boot, so the next board cannot hide the same mistake.

Two other things here are deliberate and load-bearing:

* **The I2C controller is discovered, not assumed.** The camera's SCCB bus takes one of
  the two controllers and which one is an esp32-camera internal; the first version
  assumed and failed at boot. The driver tries `I2C_NUM_0` then `I2C_NUM_1`.
* **Read-only identification happens before any write.** The original code wrote a soft
  reset (`0xB6` → register `0x7E`) *before* checking the ID — and on a part that is not a
  BMI160, `0x7E` means something else entirely.

---

## Why the camera lives here now

`firmware/cam-test/` is kept, and it is still the better place to change anything about the
camera on its own. This merge exists for the interaction, not for tidiness: with both in one
binary, "does the audio stay clean while the camera streams?" is a question you can answer
by ear instead of by argument.

---

## The API-generation trap

This project builds against Arduino core **2.0.17** (PlatformIO `espressif32` 7.1.3 —
verified, not assumed), so the I²S API is the **legacy driver**:

```cpp
#include <driver/i2s.h>
i2s_driver_install(I2S_PORT, &config, 0, NULL);
i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
```

The `ESP_I2S` class with `i2s.begin()` / `i2s.write()` that nearly every current tutorial
and blog post shows **does not exist in this core**. It arrived in core 3.x, along with a
completely different set of types. The two are not interchangeable and the compiler's
complaint is not clear about which world you are in.

If the platform is ever upgraded to a core 3.x release, `src/audio_player.cpp` is the file
that has to be rewritten. Nothing else in the repository touches I²S.

---

## Wiring

Six wires reach the DAC, of which three are the I²S bus. From §6:

| S3 | goes to | Note |
| --- | --- | --- |
| `GPIO47` | DAC pad `2` `BCK` | bit clock |
| `GPIO21` | DAC pad `3` `DIN` | the DAC's data **input**, so the S3's output |
| `GPIO45` | DAC pad `4` `LCK` | word select. `45` is the `VDD_SPI` strap and carries this clock — safe because the DAC's input is high-Z and the strap is sampled only at reset (§2.1) |
| `GPIO48` | strip data, via `R6` (330 Ω) | **also the on-board WS2812's data input** — same net, no buffer, so the on-board LED mirrors the strip's first pixel |
| `+5V` | DAC pad `6` `VIN` | the board has its own 3.3 V regulator — **5 V in, never 3V3** |
| `GND` | DAC pad `5` `GND` | |
| — | DAC pad `15` `LOUT` | → the amp's `IN+`. Line level; the DAC cannot drive a speaker |

Everything else on the DAC is already strapped on the board, and those straps are why
there is **no MCLK wire** (`SCK`→GND makes the DAC derive its clock from `BCK`) and no
configuration to do.

**Hardware confirmed 2026-10-11:** a PCM5102A plus a class-D amp — the `U6`/`U7` pair
exactly as the schematic draws it, so no schematic change was needed.

> **PSRAM, corrected — and now genuinely in use.** `platformio.ini` does not set
> `-DBOARD_HAS_PSRAM` and never needed to: the Freenove board def puts it in its own
> `extra_flags`, alongside `memory_type = qio_opi`. So PSRAM was live all along, and an
> earlier note here claiming the flag was "absent" was simply wrong. It is now **in use**:
> the camera allocates its frame buffers there via `config.fb_location`. §10 item 2's
> "PSRAM not required" applied to the audio-only stage, which is over.

---

## Build, flash, listen

```bash
pio run -d firmware/brain                 # build
pio run -d firmware/brain -t upload       # flash
pio run -d firmware/brain -t monitor      # serial console (UART0)
```

The board boots **silent** — no autoplay, and the audio stays quiet until a button is
pressed. Join the AP (`brain` / `brainpass`, or whatever `secrets.h` says) and open
`http://192.168.4.1/`. The lights come on with the board: head white, tail red, floor
sweeping (see *The layout* above).

Clean build, zero warnings. As built: **RAM 53 220 B / flash 1 207 773 B** — 36.1 % of the
3.3 MB app partition.

---

## When it is silent

In order, because this is the order they actually happen:

1. **The `XSMT` strap on the DAC.** Floated = muted output, and it presents exactly like a
   firmware bug. Meter pad `9` to `3V3`. Do this *first* — it costs 30 seconds and it is
   the single most likely cause of a silent PCM5102A.
2. **DAC power.** `VIN` (pad `6`) on 5 V, `GND` (pad `5`) on ground, and `A3V3` (pad `11`)
   left open. **Never strap `A3V3` to `VIN`** — that is 5 V onto a 3.6 V-max supply.
3. **The amp.** `LOUT` is line level (~2.1 Vrms) and cannot drive a speaker on its own.
   Confirm the amp is powered from 5 V with 470–1000 µF at its supply pins, and that its
   `IN+` sees `LOUT`. A class-D amp with its own `/SD` or enable pin may also need it
   pulled high.
4. **`BCK`/`LRCK` ratio.** With `SCK` grounded the DAC recovers its system clock from
   `BCK`, which makes it fussy about the `BCK`-to-`LRCK` ratio. At 16-bit stereo this
   firmware produces `44100 × 32 = 1.41 MHz`. If the output is silent, hissy, or
   distorted beyond recognition, that ratio is the knob to turn — try
   `I2S_BITS_PER_SAMPLE_32BIT`, or move the DAC's `SCK` strap and feed it a real `MCLK`.
   This one is still an open empirical question, not something to assume either way.
5. **The pin trio.** `BCK` `47`, `DIN` `21`, `LRCK` `45` — permuted on 2026-10-11, so the
   ribbon to the DAC **crosses**. It only runs straight when `47` (the middle header pin)
   carries `DIN` (the middle pad), which is no longer the case. And `48` is the strip
   again, so a strip wired to `45` means you have an older layout. If you wired from
   `docs/esp32-cam.md` §7 instead of the plan, you used `3`/`46`/`14` — that is the
   superseded option analysis; the plan is the authority.

---

## Next, in the order that keeps failures separable

1. ✅ The beeps are audible — done, and now on demand rather than at boot.
2. ✅ The camera stream works — built and verified in `cam-test`, merged here.
3. **Listen while streaming.** Press "sound file" with the video open. If it crackles,
   lower the `framesize` **first** (`?framesize=qvga`) — §6's PSRAM bandwidth contention,
   not an audio bug. I²S buffers stay in internal RAM (the default); that is mitigation 1
   and it is already in place.
4. **The BMI160** — bring-up step 3 of §11. I²C on `41`/`42`, address **`0x69`** (`SA0` is
   pulled up), `WHO_AM_I` must read `0xD1` (`0x24` is a BMI270).
5. **Then the core split** from §10.1: the control task pinned to core 1 at priority 5 with
   `vTaskDelayUntil()`. `loop()` is deliberately empty so that core stays free for it; the
   audio task already owns core 0 alongside the camera and the radio.
6. Replace the tone generator with streamed audio, keeping the same I²S write path — the
   section only changes *who fills the buffer*.
