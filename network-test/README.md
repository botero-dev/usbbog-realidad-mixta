# network-test — Heltec WiFi LoRa 32 (V3)

Sandbox firmware for the **Heltec WiFi LoRa 32 (V3)**: ESP32-S3, 8 MB flash,
SX1262 LoRa radio and an onboard 0.96" **SSD1306 OLED** (128x64, I²C).

The sketch paints vertical bars of different brightness on that display and runs
its own WiFi access point with a small JSON **REST API**, so how many bars there
are and how bright each one is can be changed at runtime without reflashing — and
without needing a router. Each bar prints its own numeric value underneath while
the bars are wide enough for the text.

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

## Network (SoftAP)

The board does not join a network — it **creates** one. Join the SSID it shows
on the OLED (default `network-test`) and the API answers at
`http://192.168.4.1/`, which is the address an ESP32 always takes on its own
SoftAP. The board runs its own DHCP server, so nothing has to be configured on
the client side.

Credentials live in `include/secrets.h`, which is gitignored:

```bash
cp include/secrets.example.h include/secrets.h
$EDITOR include/secrets.h      # AP_SSID, AP_PASSWORD ("" = open network)
```

Only 2.4 GHz is used (channel 1 by default), so any laptop or phone can join.

> While your machine is joined to this access point it has no internet access
> unless it also has a second link (Ethernet, another adapter). Switch back to
> your normal network when you are done.

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
network-test :: Heltec WiFi LoRa 32 (V3) :: bars + REST API
[init] OLED power on
[init] display.begin()
[init] display.begin() returned
[i2c] scanning 0x03..0x77 ...
[i2c] device at 0x3C
[oled] SSD1306 128x64 ready
[ap] network "network-test" up on channel 1 (WPA2)
[ap] join it, then browse to http://192.168.4.1/
[http] listening on port 80
[mdns] http://network-test.local/
[bars] painted
[net] uptime 10s  ssid=network-test  ip=192.168.4.1  clients=0  bars=5: 0.20 0.40 0.60 0.80 1.00
```

The OLED shows the network name and its address for the first 3 s of every boot,
then switches to the bars. The heartbeat repeats every 10 s, and its `clients=`
field tells you how many devices are currently joined to the access point.

### From the VS Code PlatformIO toolbar

Open the **`network-test` folder itself** (or `network-test.code-workspace`) —
PlatformIO IDE only picks up the `platformio.ini` of a workspace root, so
working from the repository root would build `firmware/car_control` instead.
Then use the status-bar buttons: `✓ Build`, `→ Upload`, `🔌 Monitor`.

## REST API

Join the board's own network first, then talk to `http://192.168.4.1/` — the
address it prints on the OLED and in the serial log, on port 80.
`network-test.local` resolves as well, via mDNS, on clients that support it.

| Method | Path | Body | Result |
| --- | --- | --- | --- |
| `GET` | `/` | – | plain-text help |
| `GET` | `/bars` | – | the levels currently on screen |
| `POST` | `/bars` | `[0.2,0.4,0.6,0.8,1.0]` (1..32 values) | sets the levels, repaints, returns them |
| `PUT` | `/bars` | as `POST`, or `{"levels":[...]}` | identical to `POST` |

Between 1 and 32 numbers are required, and **the array length decides how many
bars you get**: send 4 values and you get four wide bars, send 32 and you get 32
narrow ones, always divided evenly across the 128 px. Every value is clamped into
`0.0 .. 1.0` (`0.0` = all pixels off, `1.0` = solid white). A malformed body, a
count outside 1..32 or a non-numeric entry answers `400` with a JSON error and
leaves the display untouched.

Each bar shows its value underneath it on a black strip of its own, with two
decimals when they fit and one when they do not. That works up to 7 bars
(18 px per bar); with 8 or more the strip disappears and the bars use the full
height again, so nothing overlaps. The serial log states the decision on every
repaint:

```
[bars] 5 bars, min 25 px wide, values on
[bars] 12 bars, min 10 px wide, values off
```

```bash
# what is on screen right now
curl http://192.168.4.1/bars

# set all five bars at once
curl -X POST http://192.168.4.1/bars \
     -H 'Content-Type: application/json' \
     -d '[1.0,0.5,0.0,1.0,0.25]'
# {"levels":[1,0.5,0,1,0.25]}

# the object form and PUT do the same thing
curl -X PUT http://192.168.4.1/bars -d '{"levels":[0.2,0.4,0.6,0.8,1.0]}'

# any count in 1..32 is fine - four wide bars this time
curl -X POST http://192.168.4.1/bars \
     -H 'Content-Type: application/json' -d '[0.1,0.3,0.5,0.7]'

# a form field works too - curl -d's own content type, no -H needed
curl -X POST http://192.168.4.1/bars -d 'levels=0.2,0.4,0.6,0.8,1.0'

# validation
curl -i -X POST http://192.168.4.1/bars -d '[1,2,3]'
# HTTP/1.1 400 Bad Request
# {"error":"expected an array of 5 numbers"}
```

`Content-Type: application/json` is the recommended way to send the body. It is
not mandatory: `curl -d` defaults to `application/x-www-form-urlencoded`, which
makes the ESP32's HTTP server parse the payload as form fields rather than a raw
body, so the firmware also recognises that shape (and a plain `levels=...`
field). Any of the three styles above will set the bars.

Float values round-trip without trailing zeros, so `1.0` comes back as `1`.
CORS is enabled, which lets a browser page served from anywhere drive the bars.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| Board not detected at all (`lsusb` shows no `10c4:ea60`) | Charge-only USB cable — swap it for a known data cable. |
| `Failed to connect to ESP32-S3` | Hold **BOOT (GPIO0)**, tap **RST**, release BOOT — then upload. |
| Port busy / permission denied | Close any open serial monitor; confirm `id -nG` contains `dialout`. |
| Monitor silent while the bars stay lit | The monitor is holding the board in reset through DTR/RTS; use `pio device monitor --dtr 0 --rts 0`. |
| Firmware hangs after `[i2c] device at 0x3C` | Do not call `Wire.begin()` yourself — U8g2 initialises the bus in `display.begin()`, and double-initialising the peripheral hangs the ESP32 core. |
| Upload works, screen stays dark | The OLED rail is gated by `Vext`; check that GPIO36 is driven LOW before init (the sketch does this). |
| `[wifi] FAILED (status 1)` | Legacy station mode: no such SSID — check `WIFI_SSID` in `include/secrets.h`. |
| The `network-test` SSID does not appear in a scan | The board is not running or is stuck rebooting — check the OLED. The name comes from `AP_SSID` in `include/secrets.h`. |
| `curl: (7) Failed to connect` to 192.168.4.1 | You are not joined to the board's AP (you may be on your normal network, where 192.168.4.1 belongs to a different router). |
| `{"error":"no json body"}` | The payload was sent as form data with nothing recognisable in it. Add `-H 'Content-Type: application/json'`, or send a `levels=` field. Every request is echoed to the serial log as `[api] POST /bars  args=… ct='…' body='…'`. |

## How "brightness" works on a 1-bit panel

The SSD1306 has exactly two pixel states, black and white — there is no grey.
`renderDitherRect()` fakes a level by switching on a fixed fraction of the
pixels in an 8×8 ordered (Bayer) pattern. The matrix holds values 0..63, so 64
levels are addressable, and a dithered area reads as a uniform shade because the
pattern is far finer than the eye resolves at this size.

Panel contrast is held at maximum, so every difference between the bars comes
from the dither pattern alone. `gBarLevels` in `src/main.cpp` holds the power-on
defaults; either edit them there or POST new ones to `/bars`. Any value in
0.0 .. 1.0 is valid.
