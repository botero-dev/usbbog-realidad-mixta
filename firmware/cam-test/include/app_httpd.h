// app_httpd.h — the camera's HTTP server, and the LED/torch state.
// ---------------------------------------------------------------------------
// Everything the outside world can reach lives behind this header:
//
//   GET  /          the HTML page (one <img> and a link to /capture)
//   GET  /stream    multipart MJPEG — the video stream
//   GET  /capture   one JPEG snapshot
//   GET  /status    JSON: heap, PSRAM, clients, fps, sensor settings
//   GET|POST /control  sensor settings and the LED colour
//
// It is a deliberate, slimmed-down version of the core's own example at
// libraries/ESP32/examples/Camera/CameraWebServer/app_httpd.cpp — same
// multipart/x-mixed-replace mechanism, minus ~100 KB of UI and the sensor
// face-detection code that this project does not need.
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

// --- LED / torch on GPIO48 ------------------------------------------------
//
// GPIO48 is the on-board WS2812 and, through one unbuffered net, the strip's
// first pixel as well (plan 2.1). Two things want it:
//
//   * STATUS (the default): dim blue = AP up and idle, green = someone is
//     watching the stream. Cheap, and it answers "is this thing alive?" with
//     nothing plugged in — which is exactly what the Pico's GP25 LED does for
//     the body, and what this board cannot otherwise do (its on-board LED is
//     addressable, not a plain indicator).
//   * TORCH: an explicit colour, which turns the status indication OFF until it
//     is handed back. Without that hand-back, a status update would silently
//     overwrite whatever the user had set.
//
// Colours are written with neopixelWrite(), i.e. the RMT peripheral — never
// PWM. PWM on a WS2812 data line is not "dim", it is corrupted bit timing.
void led_set(uint8_t r, uint8_t g, uint8_t b);
void led_auto(bool enable);          // true = status mode, false = torch
bool led_auto_enabled();
void led_get(uint8_t *r, uint8_t *g, uint8_t *b);

// Non-blocking status update. Call from loop(); it rate-limits itself and only
// touches the LED when the colour actually changes.
void led_task(uint32_t now_ms);

}  // namespace app
