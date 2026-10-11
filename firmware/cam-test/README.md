# `cam-test` — ESP32-S3 camera + SoftAP + MJPEG stream

The **camera bring-up** for the S3-CAM, as its own standalone project. It does one
thing: brings the OV3660 up, raises a Wi-Fi access point, and serves a web page whose
only content is a live video stream.

> **Status 2026-10-11:** builds clean (zero warnings) and **has not been flashed yet**.
> As built: **RAM 47 968 B / flash 791 205 B** — 23.7 % of a 3.3 MB app partition.

---

## Why this is not `firmware/brain`

Because a failure needs one candidate. The brain project already plays audio, verified on
hardware, and adding the camera there would make the first failure look identical whether
it was the camera, the audio, the PSRAM bandwidth or the radio.

This is the same reasoning that gave `pico_body/` and `network-test/` their own projects,
and it is what makes the *merge* meaningful: folding this back into `firmware/brain` and
finding that the beeps still behave is the actual test of plan §6's warning that camera
DMA and I²S DMA share the octal PSRAM bus.

**No wiring.** The camera is on the board's own FPC connector (plan §2: the left header
row is 100 % camera). Everything in this project is on-board.

---

## Build, flash, watch

```bash
pio run -d firmware/cam-test               # build
pio run -d firmware/cam-test -t upload     # flash
pio run -d firmware/cam-test -t monitor    # serial
```

`pio device list` does **not** see this board — its WCH bridge (`1a86:55d3`) is unknown to
PlatformIO. If auto-detection fails:

```bash
pio run -d firmware/cam-test -t upload --upload-port /dev/ttyACM0
```

⚠️ Flashing this **replaces the audio firmware** currently on the S3. To put it back:

```bash
pio run -d firmware/brain -t upload
```

---

## What the boot log should say

```
cam-test  --  ESP32-S3 camera + SoftAP + MJPEG stream
=====================================================
chip      : ESP32-S3 rev 2, 2 core(s) @ 240 MHz
flash     : 16 MB
psram     : 8192 KB free of 8192 KB        <-- see the PSRAM note below
free heap : ...

camera is on the board's own FPC connector -- nothing to wire.
LED/torch : GPIO48 (the on-board WS2812, shared with the strip)

[cam] initialising...
[cam] PID 0x3660  (OV3660 = 0x3660, OV2640 = 0x2640)
[cam] sensor version 0x..., MID 0x...
[cam] framesize enum 8, quality 12
[cam] xclk 20000000 Hz, pixel format JPEG
[wifi] SoftAP "cam-test": up   auth WPA2, channel 1
[wifi] ap mac ...
[wifi] open http://192.168.4.1/
```

**The PID line is the one to read.** It is how you find out what is actually on the board
rather than what a tutorial assumed. If it says `0x2640`, the module is not what this
project was written against (everything still works — both are DVP and both stream JPEG —
but the doc set is wrong).

---

## Endpoints

All on port 80, one `esp_http_server` instance. **Not** the Arduino `WebServer` class used
by `network-test`: MJPEG needs a chunked multipart response on a keep-alive socket, which
`WebServer` cannot hold open.

| Endpoint | What it does |
| --- | --- |
| `GET /` | The page. Just `<img src="/stream">` and a link to `/capture`. |
| `GET /stream` | **The video.** `multipart/x-mixed-replace`, one JPEG per part. |
| `GET /capture` | One JPEG snapshot. |
| `GET /status` | JSON: heap, PSRAM, clients, fps, sensor settings, LED state. |
| `GET\|POST /control` | Sensor settings and the LED colour. Answers with the same JSON as `/status`. |

### `/control` parameters

Any subset, in the **query string or a form-encoded body**:

| Parameter | Range | Notes |
| --- | --- | --- |
| `framesize` | `qvga` `cif` `vga` `svga` `xga` `sxga` `uxga`, or a raw number | The reply reports what the sensor *actually* accepted. |
| `quality` | `0`–`63` | **Lower is better** and bigger. |
| `brightness` `contrast` `saturation` | `-2`–`2` | |
| `hmirror` `vflip` | `0` \| `1` | |
| `led_r` `led_g` `led_b` | `0`–`255` | Sets the torch **and turns status mode off** (see below). |
| `led_auto` | `0` \| `1` | Hands the LED back to status indication. |

```bash
curl http://192.168.4.1/status
curl 'http://192.168.4.1/control?framesize=svga&quality=10'
curl -d 'hmirror=1&vflip=1' http://192.168.4.1/control
curl 'http://192.168.4.1/control?led_r=32&led_g=0&led_b=0'
curl 'http://192.168.4.1/control?led_auto=1'
```

---

## The access point

Same shape as `network-test`, deliberately: **SoftAP, fixed `192.168.4.1`**, channel 1.
That keeps the Godot app's `http://192.168.4.1/...` addressing valid as the project grows.

Credentials come from `include/secrets.h` if it exists, otherwise from the throwaway
defaults in `include/secrets.example.h` — so it runs with no setup at all. Copy the example
to set your own. The password is never printed to the serial log.

Your phone or laptop will warn that the network **"has no internet"**. That is expected and
correct: this AP has no uplink. Tell the OS to stay connected anyway.

---

## The LED (GPIO48), and why it has two jobs

`GPIO48` is the on-board WS2812 and — through one unbuffered net — the strip's first pixel
(plan §2.1).

* **Status mode (default):** dim blue = AP up and idle, green = someone is watching the
  stream. This is the only "is it alive?" light this board has, which is what the Pico's
  `GP25` does for the body.
* **Torch mode:** setting any colour turns status off until `?led_auto=1`. Without that
  hand-back, the 500 ms status update would silently overwrite whatever you set, and
  `/control` would look broken.

It is driven with `neopixelWrite()` — i.e. **RMT, never PWM**. PWM on a WS2812 data line is
not "dim", it is corrupted bit timing.

⚠️ If a 5 V strip is attached to the `48` net, it needs the level shifter from plan §2.1
(`Vih = 0.7 × VDD = 3.5 V`, and the S3 drives 3.3 V). Driving only the on-board LED, as in
this project, needs nothing.

---

## The settings that matter, and why

* **`FRAMESIZE_VGA`, `quality 12`, `fb_count = 2`.** Start low and climb. If the first
  stream were UXGA and it stuttered, you could not tell a config problem from a bandwidth
  problem.
* **`vflip = 1` (`kSensorVFlip`).** The OV3660 on this carrier is **mounted rotated**, so an
  unflipped stream comes out upside down — found on the first working stream, 2026-10-11.
  It is applied **in the sensor**, not with a CSS transform on the page. That distinction
  matters: it fixes `/capture` too, and it fixes what the pixels look like *to code*. A CSS
  flip would leave the real data mirrored and quietly break vision or odometry later.
* **`PIXFORMAT_JPEG`.** The sensor's own JPEG pipeline, so the stream never needs a
  software encode per frame. RGB565 would be the expensive mistake.
* **`CAMERA_GRAB_WHEN_EMPTY`.** With two buffers this never waits on a frame being written.
  For a stream, a slightly older frame is indistinguishable from a newer one.
* **Console on UART0** (`-DARDUINO_USB_CDC_ON_BOOT=0`). This carrier has exactly one serial
  path and it is the WCH bridge on `43/44`. ⚠️ Plan §11's step 1 says "flash, **USB CDC**,
  camera capture" — that criterion cannot be met as written on this carrier; what is met is
  *a serial console that works*. And `43/44` is the Pico link (plan §2.3), so that flag
  must be flipped back to `1` before the link is ever wired.
* **PSRAM comes from the board def**, not from this project: `freenove_esp32_s3_wroom` sets
  `memory_type = qio_opi`, `psram_type = opi` **and `-DBOARD_HAS_PSRAM`** in its
  `extra_flags`. So the boot log's PSRAM line should be non-zero with nothing added here.
  (This corrects a note in `firmware/brain/platformio.ini` that claimed the flag was
  absent. It is not the flag that is absent — it was never needed.)

---

## Do not re-add these

```ini
board_build.flash_size = 16MB      ; <-- NO
board_build.partitions = ...       ; <-- NO
```

That combination put this board into a ~33 boot/second loop in the bootloader, under two
different board defs, and is documented in `/memories/repo/esp32-s3-boot-failure.md`. The
camera does not need them: `default_8MB.csv` gives a 3.3 MB app partition and this firmware
uses 791 KB of it.

---

## If it does not work

In the order they actually happen:

| Symptom | Most likely cause |
| --- | --- |
| `[cam] FAILED: ESP_ERR_NOT_FOUND` at boot | The sensor did not answer on SCCB. FPC ribbon not seated or reversed; or another bit of firmware is claiming one of `GPIO 4–18`. |
| Boot loop at ~33/second, no app output | The partition/flash overrides above have crept back in. **The ROM banner is not evidence the app started.** |
| AP is up, page loads, image stays black | The FPC ribbon. This is the classic one. |
| Picture is upside down, or rotated 180° | Sensor orientation, **not** a wiring fault: `kSensorVFlip` / `kSensorHMirror` in `main.cpp`. Change without a reflash via `?vflip=1`, `?hmirror=1`, etc. |
| Image is tinted/green, or torn down the middle | Wrong data-bus bit order — `CAM_PIN_D0…D7` in `camera_pins.h`. The `Y2…Y9` silkscreen names map to `D0…D7` in reverse. |
| Stream stutters, device resets | **Power, not software.** AP TX bursts on top of camera load is the worst case for the LM2596; the 1000 µF at the board's 5 V pin is the fix (plan §7.1). |
| Stream works once, then never again | An abandoned client holding the socket — check `lru_purge_enable` is still set. |
| `fps` in `/status` stays 0 | Nobody is reading `/stream`; the counter only advances inside the stream handler. |

---

## Deliberately not in this project

Audio, the BMI160, the Pico link, motors, the control loop. See plan §11 for the order and
§10.1 for the core split that comes next: camera and Wi-Fi on core 0, the eventual 100 Hz
control task pinned to core 1.

The immediate next step after this works is **merging it into `firmware/brain`** and
re-checking the audio — keeping I²S DMA buffers in internal RAM, and lowering `framesize`
before touching the audio driver if it crackles.
