// app_httpd.h — the camera's HTTP server, and the LED/torch state.
// ---------------------------------------------------------------------------
// Everything the outside world can reach lives behind this header:
//
//   GET  /          the HTML page: the stream, the sound and level buttons,
//                   and one button per embedded clip
//   GET  /stream    multipart MJPEG — the video stream
//   GET  /capture   one JPEG snapshot
//   GET  /status    JSON: heap, PSRAM, clients, fps, audio, volume, sensor,
//                   clips[]
//   GET|POST /control  sensor settings, the LED colour and the audio level
//   GET|POST /play  a sound: ?src=beeps, or ?src=<clip> from clips[], and an
//                   optional ?vol=0..1000 (per-mille) to set the level with it
//
// It is a deliberate, slimmed-down version of the core's own example at
// libraries/ESP32/examples/Camera/CameraWebServer/app_httpd.cpp — same
// multipart/x-mixed-replace mechanism, minus ~100 KB of UI and the sensor
// face-detection code that this project does not need.
//
// NOTE ON /play: it only *requests* a sound. Playback happens in the audio task
// (audio_player.h), because the handler that serves /stream must never block —
// playing inline here would freeze the video for the length of every clip.
#pragma once

#include <stdint.h>

namespace app {

// Start the server on port 80. Returns ESP_OK, or an esp_err_t from
// httpd_start()/httpd_register_uri_handler() on failure.
int server_start();

// How many clients are currently reading /stream. 0 or 1 in practice.
uint32_t stream_clients();

// Most recent measured stream frame rate, in whole frames per second.
uint32_t fps();

// --- the car's lights, on GPIO48 ------------------------------------------
//
// GPIO48 drives the on-board WS2812 and, through one unbuffered net, the 29-pixel
// strip (plan 2.1). The strip is the car's LIGHTING, and its pixels are named groups
// (head/tail left and right, tail centre, floor) -- see `led_print_layout()`.
//
// The state is THREE INDEPENDENT THINGS, not one mutually-exclusive mode, because
// they are things a driver does at the same time:
//
//   * BASE -- a three-state switch, and the only one that decides whether the car
//     has its running lights on at all:
//       OFF    every pixel dark. A STATE, so "/status can say so".
//       LOW    head lights white and tail lights red at 20/255  (the boot default).
//       HIGH   the same scene at 128/255 -- high beams.
//
//   * HAZARD -- a toggle, applied ON TOP of the base. It flashes the head and tail
//     SIDE lights yellow at 64/255, about 1.25 Hz. Between flashes the base shows
//     through: hazard is an overlay, not a replacement, so "hazard while driving"
//     lights the car normally and adds the flash. It also works with the base OFF,
//     because a hazard warning that needs the headlights on is not a hazard warning.
//
//   * RAINBOW -- a toggle for the floor sweep alone. On (the boot default) the floor
//     group turns through the hue wheel; off leaves it dark. It touches ONLY the
//     floor, so it composes with both of the above.
//
// Precedence inside one frame, highest first: hazard flash > rainbow floor > base.
// The three never fight, because they own disjoint groups -- hazard owns the four
// side lights, rainbow owns the floor, base owns the rest.
//
// MANUAL (`?group=<name>&r/g/b`, `?led_r/g/b`) is the one state the firmware does not
// animate: it is the escape hatch for settings groups by hand, and it is entered only
// by an explicit colour write. Hazard and rainbow still apply over it, by the rule
// above.
//
// What is deliberately NOT here any more: the old status indication (dim blue idle /
// green streaming / white audio) and the whole-strip rainbow. The lights are on
// whenever the car is, so the strip can no longer double as a debug indicator -- ask
// the page or `/status` instead.
//
// Colours go out over the **RMT peripheral**, never PWM — PWM on a WS2812 data line
// is not "dim", it is corrupted bit timing. Not with the core's `neopixelWrite()`
// either: that sends exactly ONE pixel and the strip needs `kStripLeds` of them. See
// the note above `strip_show()` in app_httpd.cpp.
enum class LedBase { Off, Low, High, Manual };

// Set the base state. Takes effect on the next animation tick (<= 30 ms), which is
// also what repaints it -- there is one renderer and this is not it.
void led_set_base(LedBase b);

// The two overlays. Independent of each other and of the base.
void led_set_hazard(bool on);
void led_set_rainbow(bool on);
bool led_hazard();
bool led_rainbow();

// Fill the whole strip with one colour, in the Manual state.
void led_set(uint8_t r, uint8_t g, uint8_t b);
void led_get(uint8_t *r, uint8_t *g, uint8_t *b);

// Print the 29-pixel layout -- the named groups -- to Serial, FROM the table that
// led_set_group() uses rather than from a hand-copied string that can drift.
void led_print_layout();

// Render/refresh the lights. Call from loop(); it rate-limits itself, and it is what
// animates the floor sweep and the hazard flash.
void led_task(uint32_t now_ms);

}  // namespace app
