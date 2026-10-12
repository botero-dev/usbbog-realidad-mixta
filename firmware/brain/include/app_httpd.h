// app_httpd.h — the camera's HTTP server, and the LED/torch state.
// ---------------------------------------------------------------------------
// Everything the outside world can reach lives behind this header:
//
//   GET  /          the HTML page: the stream, the sound buttons, and the
//                   beep-level buttons
//   GET  /stream    multipart MJPEG — the video stream
//   GET  /capture   one JPEG snapshot
//   GET  /status    JSON: heap, PSRAM, clients, fps, audio, volume, sensor
//   GET|POST /control  sensor settings, the LED colour and the audio level
//   GET|POST /play  a sound: ?src=beeps | ?src=clip, and an optional
//                   ?vol=0..1000 (per-mille) to set the level with it
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
//   * RUN (the BOOT DEFAULT): head lights white and tail lights red, ALWAYS ON at
//     20/255, with the floor sweeping the hue wheel at the same level.
//   * TORCH: that same scene with the head lights at 128/255.
//   * HAZARD: that same scene with the head and tail SIDE lights flashing yellow at
//     64/255, about 1.25 Hz.
//   * OFF: every pixel to zero. All lights dark, and it is a STATE rather than a
//     one-off command so that "the lights are off" is something /status can report
//     and the page can show -- not something you have to remember.
//   * CUSTOM: a per-group scene. `?group=<name>&r/g/b` lights one named group, and
//     the groups COMPOSE rather than replace -- the manual override underneath the
//     automatic states above.
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
enum class LedMode { Run, Torch, Hazard, Off, Custom };

// Set the lighting state. Takes effect on the next animation tick (<= 30 ms), which
// is also what repaints it -- there is one renderer and this is not it.
void led_set_mode(LedMode m);

// Fill the whole strip with one colour, in the Custom state.
void led_set(uint8_t r, uint8_t g, uint8_t b);
void led_get(uint8_t *r, uint8_t *g, uint8_t *b);

// Print the 29-pixel layout -- the named groups -- to Serial, FROM the table that
// led_set_group() uses rather than from a hand-copied string that can drift.
void led_print_layout();

// Render/refresh the lights. Call from loop(); it rate-limits itself, and it is what
// animates the floor sweep and the hazard flash.
void led_task(uint32_t now_ms);

}  // namespace app
