# `brain` — ESP32-S3 firmware

The **S3 side** of the project. Today it does exactly one thing: puts a tone out of the
I²S port so the audio chain can be heard working on its own.

> ✅ **Heard on hardware, 2026-10-11.** Three beeps out of the PCM5102A + class-D amp +
speaker, first try once the build configuration was fixed — so `S3 → DAC → amp →
speaker` is verified end to end, and `i2s_driver_install()` / `i2s_set_pin()` both succeed
with the permuted pins (`47`/`21`/`45`).
>
> What is **not** verified is §11's other half: *"and still clean while the camera
> streams"*. There is no camera in this firmware yet, and that is where the PSRAM
> bandwidth contention in §6 is expected to show up.
>
> Volume is now `VOLUME` in `src/main.cpp`, set to a tenth of the first revision because
> that one was much too loud into a small speaker.

Authority for every pin and every number here is `docs/circuit-plan.md` — §6 (audio
chain), §10.1 (S3 firmware notes), §11 (bring-up order). It is intended to grow into the
"brain" of §1 — WiFi, camera, IMU and the control loop — and to absorb the mixing code
in `firmware/car_control` (§10.3).

---

## Why a tone, and not a sound file

A tone is a chain you can verify. Playing a `.wav` exercises five things at once — the
file system, the decoder, the I²S format, the DAC and the amp — and when it fails you
learn nothing about which one. Bring-up step 2 in §11 is deliberately just *"test tone is
clean, and still clean while the camera streams"*, with no motors, no camera and no link
in the picture.

Compare it to the Pico: `firmware/pico_body` is at the same stage. Both boards now have a
firmware that proves its own wiring and nothing else.

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

If the platform is ever upgraded to a core 3.x release, `src/main.cpp` is the file that
has to be rewritten. Nothing else in the repository touches I²S yet.

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
exactly as the schematic draws it, so no schematic change was needed. The camera is
deliberately excluded from this stage (audio on its own first), so PSRAM stays off, which
is the decision already recorded in `docs/esp32-cam.md` §10 item 2.

---

## Build, flash, listen

```bash
pio run -d firmware/brain                 # build
pio run -d firmware/brain -t upload       # flash
pio run -d firmware/brain -t monitor      # USB CDC output
```

Expect three beeps (440 / 660 / 880 Hz), a 2 s gap, repeat.

The build is clean under this framework's warning flags. As built: **RAM 20 752 B / flash
280 713 B**.

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

1. Get the three beeps audible on the bench. Nothing else counts until this happens.
2. Replace the tone generator with streamed audio, keeping the same I²S write path — the
   section only changes *who fills the buffer*.
3. Then the core split from §10.1: audio on core 0, the control task pinned to core 1 at
   priority 5 with `vTaskDelayUntil()`. Do not let this loop share a core with the control
   task once the control task exists.
4. **Keep I²S buffers in internal RAM** (the default) — the camera's DMA and the I²S DMA
   both reach for PSRAM, and §6's bandwidth note expects occasional underruns if that is
   ignored. If audio crackles only while the camera is streaming, lower the `framesize`
   before touching the audio driver.
