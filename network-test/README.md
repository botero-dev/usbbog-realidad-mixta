# network-test — Heltec WiFi LoRa 32 (V3)

Sandbox firmware for the **Heltec WiFi LoRa 32 (V3)**: ESP32-S3, 8 MB flash,
SX1262 LoRa radio and an onboard 0.96" **SSD1306 OLED** (128x64, I²C).

The current sketch splits the panel into five vertical bars, one per brightness
step, and holds them there so the shades can be compared side by side.

## Hardware

| Item | Value |
| --- | --- |
| Board ID (PlatformIO) | `heltec_wifi_lora_32_V3` |
| MCU | ESP32-S3 (8 MB flash, 320 KB RAM) |
| Display | SSD1306, 128x64, monochrome, I²C address `0x3C` |
| OLED SDA / SCL | GPIO **17** / GPIO **18** |
| OLED reset | GPIO **21** |
| OLED power (`Vext`) | GPIO **36** — *active LOW* |
| Serial | UART0 on GPIO 43/44, bridged by the onboard **CP2102** chip |

The V3 exposes this through its single USB-C connector, so plain
`Serial.printf()` shows up in a normal serial monitor. (Only the *V4* removed the
CP2102.)

## Deploy

### 1. Connect the board

Plug the board into a USB-C cable. On Linux the CP2102 is handled by the
in-kernel `cp210x` driver — no extra package needed, and your user must be in
the `dialout` group (it is).

```bash
pio device list            # look for /dev/ttyUSB0
```

### 2. Flash

```bash
pio run -d network-test -t upload
```

`-d network-test` is required because the repository root also holds a
`platformio.ini` (for `firmware/car_control`). If a port is not auto-detected:

```bash
pio run -d network-test -t upload --upload-port /dev/ttyUSB0
```

### 3. Watch it run

```bash
pio device monitor -b 115200
```

Expected output (captured from real hardware):

```
network-test :: Heltec WiFi LoRa 32 (V3) :: five brightness bars
[init] OLED power on
[init] display.begin()
[init] display.begin() returned
[i2c] scanning 0x03..0x77 ...
[i2c] device at 0x3C
[oled] SSD1306 128x64 ready
[bars] bar 0  x=  0.. 25  level=0.20
[bars] bar 1  x= 25.. 51  level=0.40
[bars] bar 2  x= 51.. 76  level=0.60
[bars] bar 3  x= 76..102  level=0.80
[bars] bar 4  x=102..128  level=1.00
[bars] painted
[bars] alive, uptime 5s  levels: 0.20 0.40 0.60 0.80 1.00
```

Five equal-width bars (25–26 px each) run left to right, from a sparse dot field
at 0.20 to solid white at 1.00. The image is static: the firmware paints it once
and then only prints a heartbeat every 5 s.

### From the VS Code PlatformIO toolbar

Open the **`network-test` folder itself** (or `network-test.code-workspace`) —
PlatformIO IDE only picks up the `platformio.ini` of a workspace root, so
working from the repository root would build `firmware/car_control` instead.
Then use the status-bar buttons: `✓ Build`, `→ Upload`, `🔌 Monitor`.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Board not detected at all (`lsusb` shows no `10c4:ea60`) | Charge-only USB cable — swap it for a known data cable. |
| `Failed to connect to ESP32-S3` | Hold **BOOT (GPIO0)**, tap **RST**, release BOOT — then upload. |
| Port busy / permission denied | Close any open serial monitor; confirm `id -nG` contains `dialout`. |
| Monitor silent while the bars stay lit | The monitor is holding the board in reset through DTR/RTS; use `pio device monitor --dtr 0 --rts 0`. |
| Firmware hangs after `[i2c] device at 0x3C` | Do not call `Wire.begin()` yourself — U8g2 initialises the bus in `display.begin()`, and double-initialising the peripheral hangs the ESP32 core. |
| Upload works, screen stays dark | The OLED rail is gated by `Vext`; check that GPIO36 is driven LOW before init (the sketch does this). |

## How "brightness" works on a 1-bit panel

The SSD1306 has exactly two pixel states, black and white — there is no grey.
`renderDitherRect()` fakes a level by switching on a fixed fraction of the
pixels in an 8×8 ordered (Bayer) pattern. The matrix holds values 0..63, so 64
levels are addressable, and a dithered area reads as a uniform shade because the
pattern is far finer than the eye resolves at this size.

Panel contrast is held at maximum, so every difference between the bars comes
from the dither pattern alone. Edit `kBarLevels` in `src/main.cpp` to try other
levels — any value in 0.0 .. 1.0 is valid, e.g.
`{0.1f, 0.3f, 0.5f, 0.7f, 0.9f}`.
