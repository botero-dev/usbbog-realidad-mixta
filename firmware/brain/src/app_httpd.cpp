// app_httpd.cpp — the HTTP server behind app_httpd.h.
// ---------------------------------------------------------------------------
// A slimmed-down, heavily commented version of the core's own example:
//   libraries/ESP32/examples/Camera/CameraWebServer/app_httpd.cpp
//
// The one thing worth understanding before editing this file: **the stream is
// not a protocol the firmware implements.** `esp_http_server` sends a chunked
// response with a multipart boundary, each part being one JPEG, and the browser's
// plain <img> tag reassembles them. There is no per-frame framing to invent,
// nothing to synchronise, and no WebSocket. That is the whole trick, and it is
// why this file is a few hundred lines instead of a few thousand.
//
// WHY esp_http_server AND NOT THE ARDUINO `WebServer` CLASS: MJPEG needs a
// chunked multipart response on a keep-alive socket. `WebServer` (the class used
// in the network-test project's /bars API) buffers a complete response and
// cannot hold a socket open for an unbounded stream. Different jobs; this one
// gets the IDF server.
//
// THE ONE RULE FOR /play: this handler must not play anything. It sets a request
// and returns. Playing inline would block this task — the same task that is
// mid-`httpd_resp_send_chunk()` on someone's /stream — for the length of the
// sound, so the video would freeze every time a button was pressed.
// ---------------------------------------------------------------------------

#include "app_httpd.h"

#include <Arduino.h>
#include <WiFi.h>

#include <stdlib.h>
#include <string.h>

#include "audio_player.h"
#include "esp_http_server.h"
#include "esp32-hal-rmt.h"  // rmtInit/rmtSetTick/rmtWriteBlocking (the strip)

#include "camera_pins.h"
#include "esp_camera.h"
#include "imu.h"
#include "motor_link.h"

namespace app {
namespace {

// --- ports ----------------------------------------------------------------
// The stream gets its OWN port, and therefore its own server task. See the note
// on start_on_port() for why that is not optional.
constexpr uint16_t kControlPort = 80;
constexpr uint16_t kStreamPort = 81;

// --- stream framing -------------------------------------------------------
// The boundary string is arbitrary but must match what goes in the Content-Type
// header. "--frame" is used here because it is short and easy to spot in a
// packet capture; the example uses a long UUID-ish string, which is also fine.
// What matters is that the digest is distinct from any byte sequence that could
// plausibly appear at a chunk edge in a JPEG.
constexpr char STREAM_CONTENT_TYPE[] =
    "multipart/x-mixed-replace;boundary=frame";
constexpr char STREAM_BOUNDARY[] = "\r\n--frame\r\n";
constexpr char STREAM_PART[] =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// --- state ----------------------------------------------------------------
volatile uint32_t g_stream_clients = 0;
volatile uint32_t g_fps = 0;

// The lights' current state. `LedMode` itself lives in app_httpd.h -- it is the
// strip's public contract -- and the boot default is Run, the always-on scene, not a
// demo effect: the car's lights come on with the car.
LedMode g_led_mode = LedMode::Run;
uint8_t g_led_r = 0, g_led_g = 0, g_led_b = 0;
// Where the floor's sweep is, in BYTE hue: a full turn is 256, so it wraps with a
// plain uint8_t add and there is no 360-vs-0 branch to get wrong.
uint8_t g_floor_hue = 0;

// How many pixels this firmware writes per frame.
//
// GPIO48 drives the on-board WS2812 AND the strip -- ONE net, one RMT stream, so
// both always show the same thing (plan 2.1). The on-board LED is not one of
// these: it is one more chip listening on the same wire, and it shows pixel 0.
//
// 29, not 30: the strip's FIRST pixel is dead -- reverse-polarity damage from
// 2026-10-12, before the wiring was corrected -- and it is bypassed, so the data
// lead now feeds the second chip and these 29 are the pixels that still work.
constexpr int kStripLeds = 29;

// *** THE CEILING: nothing may exceed this, whatever asks for it. ***
// 128, and it is chosen by the BRIGHTEST thing in the catalogue -- the torch's head
// lights. This is a SAFETY limit now, not a brightness setting: the per-mode LEVELS
// further down decide how bright each thing actually is, and this only stops any of
// them -- or a hand-written `/control?led_r=255` -- from going past.
//
// The sequence matters, so record it: this was 20 while the strip was only ever
// ambient lighting, and 20 has NOT gone away -- it moved from being a global cap to
// being the ambient level. A global cap cannot express "head at 128, floor at 20".
//
// Enforced in led_push() because that is the SINGLE gate every frame from every mode
// passes through, so no caller, no future mode and no query string gets around it.
// Max draw, everything on at white: 4 head pixels at 128 (~60 mA) plus 25 at 20
// (~120 mA) -- about 180 mA, still inside a USB 2.0 port's budget.
constexpr uint8_t kLedMaxChannel = 128;

// --- the layout: 29 pixels, named ----------------------------------------
//
// Labelled by the user, 2026-10-12. The indices are PHYSICAL POSITIONS in the
// strip's data order -- and index 0 is NOT the strip's first chip: pixel 1 of the
// strip is dead (reverse-polarity damage) and is bypassed, so firmware index 0
// lands on its second chip. Wire the strip any other way and every label below
// moves.
//
//   index  0- 1   tail light right
//   index  2- 5   tail light centre
//   index  6- 7   tail light left
//   index  8-24   floor
//   index 25-26   head light left
//   index 27-28   head light right
//
// Naming these is not documentation, it is API: led_set_group() takes a LedGroup
// and /control?group=<name> addresses one from a browser. The point of a TABLE
// rather than six #defines is that the tiling can be checked -- see below.
enum class LedGroup { TailRight, TailCentre, TailLeft, Floor, HeadLeft, HeadRight };

struct GroupRange {
  const char *name;  // the /control?group= spelling
  LedGroup group;
  uint8_t first;     // first pixel index, inclusive
  uint8_t count;
};

constexpr GroupRange kGroups[] = {
    {"tail_right", LedGroup::TailRight, 0, 2},
    {"tail_centre", LedGroup::TailCentre, 2, 4},
    {"tail_left", LedGroup::TailLeft, 6, 2},
    {"floor", LedGroup::Floor, 8, 17},
    {"head_left", LedGroup::HeadLeft, 25, 2},
    {"head_right", LedGroup::HeadRight, 27, 2},
};

constexpr size_t kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);

// Every pixel belongs to exactly one group, and the groups cover the strip in
// order with no gap and no overlap. Checked by the COMPILER rather than by eye: a
// gap or an overlap here is a lighting bug that would otherwise only show up on
// the car at night.
//
// *** C++11, NOT C++14: single return statements only. ***
// A loop inside a constexpr body is C++14 ("body of 'constexpr' function not a
// return-statement") and this core builds with -std=gnu++11 -- the same trap that
// stopped protocol.h's CRC from compiling at all. Hence recursion, which is also
// how the CRC there does it.
constexpr bool groups_tile(size_t i, int expected_first) {
  return i == kGroupCount
             ? expected_first == kStripLeds
             : (kGroups[i].first == expected_first &&
                groups_tile(i + 1, expected_first + kGroups[i].count));
}
static_assert(groups_tile(0, 0),
              "LED groups must tile 0..kStripLeds-1: no gap, no overlap, no extras");

// THE frame: what the LEDs are showing. The renderer builds it for Run/Torch/Hazard,
// a group write or a manual colour edits it in Custom, and led_push() sends it.
uint8_t g_scene[kStripLeds * 3];

// The frame as last sent, so an identical one is not pushed again. The write
// BLOCKS the caller for ~1.4 ms (see strip_show), and led_task() pushes the
// status colour every 500 ms whether or not anything changed.
uint8_t g_applied[kStripLeds * 3];
bool g_applied_valid = false;

uint32_t g_led_last_ms = 0;

// --- the always-on scene, and the two overrides ----------------------------
//
// "Head and tail always on, the floor sweeping" is the car's DEFAULT state (user,
// 2026-10-12), so none of this is a demo effect. Every level is a fraction of full
// scale applied through level_channel(), which keeps a colour and a brightness
// independent: "make the tail red" and "make the tail dimmer" are separate one-line
// changes instead of a hunt for a literal.
constexpr uint8_t kAmbientLevel = 20;  // head + tail, always on
constexpr uint8_t kFloorLevel = 20;    // the floor's sweep
constexpr uint8_t kHazardLevel = 64;   // the flashing side lights
constexpr uint8_t kTorchLevel = 128;   // the head lights, torch on

// Head white, tail red -- car-conventional, and stated once so "what colour is the
// tail?" has exactly one answer.
constexpr uint8_t kHeadRGB[3] = {255, 255, 255};
constexpr uint8_t kTailRGB[3] = {255, 0, 0};
constexpr uint8_t kBlackRGB[3] = {0, 0, 0};
// Yellow, as asked. Amber (255,160,0) is what a real indicator looks like -- that is
// a one-line change here if the flash should read as a turn signal.
constexpr uint8_t kHazardRGB[3] = {255, 255, 0};

// The four groups that flash: the head and tail SIDE lights. The tail centre and the
// floor are not side lights and keep doing what they were doing.
constexpr LedGroup kHazardGroups[] = {LedGroup::HeadLeft, LedGroup::HeadRight,
                                      LedGroup::TailLeft, LedGroup::TailRight};
constexpr size_t kHazardGroupCount =
    sizeof(kHazardGroups) / sizeof(kHazardGroups[0]);

// Half a flash period, so the flash runs at ~1.25 Hz -- the rate a hazard flasher
// actually runs at. Derived from the clock, never counted up, so a late tick cannot
// make it drift or stick.
constexpr uint32_t kHazardHalfMs = 400;

// --- frame-size names -----------------------------------------------------
// Names are nicer in a URL than raw enum values, and the numeric value is
// accepted too, so "?framesize=8" and "?framesize=vga" both work.
struct SizeName {
  const char *name;
  framesize_t fs;
};

const SizeName kSizes[] = {
    {"qvga", FRAMESIZE_QVGA}, {"cif", FRAMESIZE_CIF}, {"vga", FRAMESIZE_VGA},
    {"svga", FRAMESIZE_SVGA}, {"xga", FRAMESIZE_XGA}, {"sxga", FRAMESIZE_SXGA},
    {"uxga", FRAMESIZE_UXGA},
};

const char *size_name(framesize_t fs) {
  for (const SizeName &s : kSizes) {
    if (s.fs == fs) return s.name;
  }
  return "?";
}

bool size_parse(const char *text, framesize_t *out) {
  for (const SizeName &s : kSizes) {
    if (strcmp(text, s.name) == 0) {
      *out = s.fs;
      return true;
    }
  }
  // Fall back to a raw number. Range is not validated beyond being positive:
  // set_framesize() rejects what the sensor cannot do and returns non-zero.
  if (text[0] >= '0' && text[0] <= '9') {
    *out = (framesize_t)atoi(text);
    return true;
  }
  return false;
}

// --- request parameters ---------------------------------------------------
//
// Read ONCE per request into a flat "a=1&b=2" buffer, then scan that buffer.
//
// This matters: the body can only be read from the socket once, so a
// per-key "fetch the query string" helper would work for GET and silently
// return nothing for the second key of a POST body. Loading once removes the
// whole class of bug.
bool params_load(httpd_req_t *req, char *dst, size_t dst_len) {
  dst[0] = '\0';

  const size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen > 0 && qlen < dst_len) {
    if (httpd_req_get_url_query_str(req, dst, dst_len) == ESP_OK &&
        dst[0] != '\0') {
      return true;
    }
  }

  // No query string: accept a form-encoded body as well, so that both
  //   curl 'http://192.168.4.1/control?quality=20'
  //   curl -d 'quality=20' http://192.168.4.1/control
  // do the obvious thing. Bounded: anything larger is ignored rather than
  // truncated into garbage.
  const int total = req->content_len;
  if (total > 0 && total < (int)dst_len) {
    const int got = httpd_req_recv(req, dst, total);
    if (got > 0) {
      dst[got] = '\0';
      return true;
    }
  }
  return false;
}

// Minimal "key=value&key=value" scanner. No URL-decoding: every value this
// firmware accepts is a number or a short lowercase name.
bool param_str(const char *params, const char *key, char *out, size_t out_len) {
  const size_t klen = strlen(key);
  const char *p = params;

  while (*p != '\0') {
    const char *amp = strchr(p, '&');
    const size_t seg = amp ? (size_t)(amp - p) : strlen(p);

    if (seg > klen && p[klen] == '=' && strncmp(p, key, klen) == 0) {
      size_t vlen = seg - klen - 1;
      if (vlen > out_len - 1) vlen = out_len - 1;
      memcpy(out, p + klen + 1, vlen);
      out[vlen] = '\0';
      return true;
    }
    if (!amp) break;
    p = amp + 1;
  }
  return false;
}

bool param_int(const char *params, const char *key, int *out) {
  char v[24];
  if (!param_str(params, key, v, sizeof(v))) return false;
  *out = atoi(v);
  return true;
}

// Optional `vol` parameter, in PER-MILLE so it parses as a plain integer:
//   vol=37  -> 0.037  -> 3.7 % of full scale -> about -28.7 dBFS
// Per-mille rather than percent because percent cannot express the quiet end
// (0.4 % is vol=4) and floats in a query string are extra code for no gain.
// Returns true if the parameter was present.
bool apply_volume_param(const char *params) {
  int milli;
  if (!param_int(params, "vol", &milli)) return false;
  if (milli < 0) milli = 0;
  if (milli > 1000) milli = 1000;
  audio_set_volume((float)milli / 1000.0f);
  // Logged because the level is STICKY: a quiet value left over from a previous
  // press makes every later sound quiet, with nothing in the log to explain it
  // unless this line is here.
  Serial.printf("[http] volume -> %u per-mille (%.2f%% FS)\n", (unsigned)milli,
                milli / 10.0f);
  return true;
}

// --- LED ------------------------------------------------------------------
uint8_t clamp8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

// The animation tick. 30 ms is a DISPLAY rate: the floor's sweep moves one step per
// tick, and the hazard flash is sampled from the clock rather than counted, so this
// only decides how smooth the sweep looks. It also fixes the cost -- every tick ends
// in a ~1.4 ms blocking RMT write (see strip_show).
constexpr uint32_t kAnimIntervalMs = 30;
constexpr uint8_t kRainbowStep = 3;  // 256/3 ticks x 30 ms = about 2.6 s per turn

const char *led_mode_name(LedMode m) {
  switch (m) {
    case LedMode::Torch: return "torch";
    case LedMode::Hazard: return "hazard";
    case LedMode::Off: return "off";
    case LedMode::Custom: return "custom";
    default: return "run";
  }
}

// A colour and a brightness, kept independent.
uint8_t level_channel(uint8_t component, uint8_t level) {
  return (uint8_t)(((uint16_t)component * level) / 255);
}

// Hue in BYTE units: 0..255 is one full turn, so there is no wrap at 360 and no
// float anywhere. Saturation and value are 0..255 too. Six sectors of 43 (43 x 6
// = 258, so the last sector is one unit short -- invisible at this brightness).
void hsv_to_rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g,
                uint8_t *b) {
  if (s == 0) {
    *r = *g = *b = v;
    return;
  }
  const uint8_t sector = (uint8_t)(h / 43);
  const uint8_t pos = (uint8_t)((h - sector * 43) * 6);  // 0..255 in the sector
  const uint8_t p = (uint8_t)((v * (255 - s)) / 255);
  const uint8_t q = (uint8_t)((v * (255 - (s * pos) / 255)) / 255);
  const uint8_t t = (uint8_t)((v * (255 - (s * (255 - pos)) / 255)) / 255);
  switch (sector) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
  }
}

// --- the scene: the frame the lights actually show -------------------------
//
// Run, Torch and Hazard are THE SAME always-on scene -- head and tail lit, the floor
// sweeping -- differing only in ONE override. Sharing a single builder rather than
// copying it per mode is what makes "always on" true by construction: there is no
// state in which the tail lights can be forgotten, because there is exactly one place
// they are turned on.
void scene_set_group(LedGroup which, const uint8_t *rgb, uint8_t level) {
  for (size_t i = 0; i < kGroupCount; ++i) {
    if (kGroups[i].group != which) continue;
    for (uint8_t p = 0; p < kGroups[i].count; ++p) {
      const int at = (kGroups[i].first + p) * 3;
      g_scene[at + 0] = level_channel(rgb[0], level);
      g_scene[at + 1] = level_channel(rgb[1], level);
      g_scene[at + 2] = level_channel(rgb[2], level);
    }
    return;
  }
}

// Fill the whole scene with one colour. The renderer never needs this -- it sets the
// groups individually -- but the manual whole-strip override does.
void scene_fill(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < kStripLeds; ++i) {
    g_scene[i * 3 + 0] = r;
    g_scene[i * 3 + 1] = g;
    g_scene[i * 3 + 2] = b;
  }
}

// The floor: one full turn of hue spread across the group. It writes the pixels
// itself because each one wants a DIFFERENT hue -- the one group a single colour
// cannot express.
//
// The step is (p * 256) / count and not p * (256 / count). At 17 pixels the two
// differ by a single hue unit and it would not matter; the reason to use the correct
// form is that at 29 pixels -- the whole strip, as it was before the groups existed --
// the naive one steps by floor(256/29) = 8, spans only 224, and leaves a 32-unit seam
// against a step of 8, which IS visible. Right once, and it cannot be wrong for a
// group of a different length.
void scene_floor_sweep() {
  for (size_t i = 0; i < kGroupCount; ++i) {
    if (kGroups[i].group != LedGroup::Floor) continue;
    const GroupRange &gr = kGroups[i];
    for (uint8_t p = 0; p < gr.count; ++p) {
      uint8_t r = 0, g = 0, b = 0;
      hsv_to_rgb((uint8_t)(g_floor_hue + (p * 256) / gr.count), 255, kFloorLevel,
                 &r, &g, &b);
      const int at = (gr.first + p) * 3;
      g_scene[at + 0] = r;
      g_scene[at + 1] = g;
      g_scene[at + 2] = b;
    }
    return;
  }
}

void scene_render(uint32_t now_ms) {
  // OFF: every pixel to zero. It is rendered on the normal tick rather than sent once
  // and then left, which costs a 1.4 ms write every 30 ms to hold a dark strip -- and
  // buys back the ability to RECOVER. A strip that loses a frame (a marginal data line,
  // a supply dip) is repainted 30 ms later instead of staying wrong until the next
  // command. With the strip's data level still unverified, that matters more than the
  // 5 % of an otherwise empty loop().
  if (g_led_mode == LedMode::Off) {
    scene_fill(0, 0, 0);
    return;
  }

  // Head and tail: always on. These five lines are the whole promise.
  scene_set_group(LedGroup::HeadLeft, kHeadRGB, kAmbientLevel);
  scene_set_group(LedGroup::HeadRight, kHeadRGB, kAmbientLevel);
  scene_set_group(LedGroup::TailLeft, kTailRGB, kAmbientLevel);
  scene_set_group(LedGroup::TailCentre, kTailRGB, kAmbientLevel);
  scene_set_group(LedGroup::TailRight, kTailRGB, kAmbientLevel);

  if (g_led_mode == LedMode::Torch) {
    scene_set_group(LedGroup::HeadLeft, kHeadRGB, kTorchLevel);
    scene_set_group(LedGroup::HeadRight, kHeadRGB, kTorchLevel);
  }

  if (g_led_mode == LedMode::Hazard) {
    // The flash is derived from the CLOCK, not toggled from a counter, so a missed or
    // late tick cannot make it drift or stick on. Off means OFF: a hazard flashes, so
    // the side lights go dark for half the period rather than falling back to their
    // running lights -- falling back would read as a colour change, not a flash.
    const bool on = ((now_ms / kHazardHalfMs) & 1u) == 0;
    for (size_t i = 0; i < kHazardGroupCount; ++i) {
      if (on) {
        scene_set_group(kHazardGroups[i], kHazardRGB, kHazardLevel);
      } else {
        scene_set_group(kHazardGroups[i], kBlackRGB, 255);  // black at any level
      }
    }
  }

  scene_floor_sweep();
}

// --- the strip's data, N pixels at a time ---------------------------------
//
// *** WHY THIS IS NO LONGER neopixelWrite(). ***
// The core's neopixelWrite() sends exactly ONE pixel and core 2.0.17 has no
// public N-pixel API. One pixel was enough while the only learner was the
// on-board WS2812, but it cannot light a strip: every pixel past the first keeps
// whatever it last latched, so the strip shows one LED and then stale colour.
//
// So this drives RMT through the SAME wrapper the core itself uses
// (esp32-hal-rmt.h) with the SAME bit timing it uses -- 100 ns per tick, T0H
// 0.4 us, T1H 0.8 us, taken straight from esp32-hal-rgb-led.c -- so no timing
// here is a new guess. rmt_write_items() takes any length ("send waveform with
// any length", driver/rmt.h) and refills the channel's 48-word memory from this
// buffer in its ISR, which is the only reason 8 pixels (192 items) fit at all.
//
// Three consequences that are not optional:
//   * the item buffer is STATIC -- the driver points at it until the last bit has
//     gone out, so it must not be a stack local;
//   * rmtWriteBlocking() BLOCKS for ~1.4 ms (836 us of bits at 29 pixels + 600 us
//     of latch), so this may never be called from an ISR (plan 10.1 says the same).
//     That block grows with the pixel count and is no longer negligible.
//   * a missing latch is invisible -- a frame with no gap after it is a frame
//     the strips never accept.
constexpr uint16_t kLatchTicks = 3000;  // x 100 ns = 300 us, held twice = 600 us

rmt_data_t g_strip_items[kStripLeds * 24 + 1];  // 697 items = 2788 B of .bss
rmt_obj_t *g_strip = nullptr;

// One WS2812 bit. A 1 is a LONG high, a 0 a short one; both are 1.2 us end to
// end, because the strip measures the HIGH time, not the duty cycle.
void strip_bit(rmt_data_t *it, bool one) {
  if (one) {
    it->level0 = 1; it->duration0 = 8;  // 0.8 us high
    it->level1 = 0; it->duration1 = 4;  // 0.4 us low
  } else {
    it->level0 = 1; it->duration0 = 4;  // 0.4 us high
    it->level1 = 0; it->duration1 = 8;  // 0.8 us low
  }
}

// Send `n` RGB pixels. GRB is the wire order, and that swap is why a straight
// RGB write shows red and green exchanged.
bool strip_show(const uint8_t *rgb, int n) {
  if (n > kStripLeds) n = kStripLeds;
  if (n <= 0) return true;

  if (!g_strip) {
    g_strip = rmtInit(PIN_LED_STRIP, RMT_TX_MODE, RMT_MEM_64);
    if (!g_strip) {
      // Once, not per frame: a silent failure here looks exactly like a dead LED,
      // and a retry every 30 ms would bury the log in it.
      Serial.println("[led] RMT init FAILED -- no LED output this boot");
      return false;
    }
    rmtSetTick(g_strip, 100);  // 100 ns per tick, the core's own value
  }

  rmt_data_t *it = g_strip_items;
  for (int i = 0; i < n; ++i) {
    const uint8_t grb[3] = {rgb[i * 3 + 1], rgb[i * 3 + 0], rgb[i * 3 + 2]};
    for (int ch = 0; ch < 3; ++ch) {
      for (int bit = 7; bit >= 0; --bit) strip_bit(it++, (grb[ch] >> bit) & 1);
    }
  }

  // The latch. An explicit LOW after the frame, because how long the caller
  // waits before the next frame is not something the strip can be told about --
  // and the two variants disagree about what counts as a gap (the classic needs
  // >= 50 us, the ECO/V5 >= 280 us). 600 us satisfies both and costs nothing at
  // a 30 ms update rate.
  //
  // Anything left on a strip LONGER than kStripLeds is simply not addressed:
  // those pixels keep their last value rather than going dark.
  it->level0 = 0; it->duration0 = kLatchTicks;
  it->level1 = 0; it->duration1 = kLatchTicks;
  ++it;

  return rmtWriteBlocking(g_strip, g_strip_items, (size_t)(it - g_strip_items));
}

// Push one frame, skipping it if it is byte-identical to the last. That guard is
// load-bearing rather than an optimisation: led_task() pushes the status colour
// every 500 ms regardless, and every push blocks for ~830 us.
bool led_push(const uint8_t *rgb, int n) {
  if (n > kStripLeds) n = kStripLeds;
  if (n <= 0) return true;

  // Cap FIRST, then compare and send, so the dedupe is on what actually goes out
  // and the cap cannot be bypassed by any caller -- see kLedMaxChannel.
  uint8_t frame[kStripLeds * 3];
  const size_t bytes = (size_t)n * 3;
  for (size_t i = 0; i < bytes; ++i) {
    frame[i] = (rgb[i] > kLedMaxChannel) ? kLedMaxChannel : rgb[i];
  }

  if (g_applied_valid && memcmp(g_applied, frame, bytes) == 0) return true;
  const bool ok = strip_show(frame, n);
  memcpy(g_applied, frame, bytes);
  g_applied_valid = true;
  return ok;
}

// One colour across the whole strip. This is the MANUAL override, so it lands in
// Custom -- the state the renderer deliberately leaves alone.
void led_apply(uint8_t r, uint8_t g, uint8_t b) {
  scene_fill(r, g, b);
  g_led_mode = LedMode::Custom;
  led_push(g_scene, kStripLeds);
}

// --- groups ---------------------------------------------------------------
bool led_group_parse(const char *name, LedGroup *out) {
  for (size_t i = 0; i < kGroupCount; ++i) {
    if (strcmp(name, kGroups[i].name) == 0) {
      *out = kGroups[i].group;
      return true;
    }
  }
  return false;
}

// Light ONE group and switch to Custom, where the scene is the display. Only that
// group's pixels are written, so the groups COMPOSE rather than replace: set the
// tail, then the head, then the floor, and all three stay set.
bool led_set_group(LedGroup which, uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t rgb[3] = {r, g, b};
  scene_set_group(which, rgb, 255);  // level 255 writes the components as given
  g_led_mode = LedMode::Custom;
  return led_push(g_scene, kStripLeds);
}

// --- handlers -------------------------------------------------------------

// The page: the stream, a snapshot link, and the two sound buttons.
//
// The buttons use fetch() rather than being links, and that is not decoration: a
// plain <a href="/play?..."> would NAVIGATE AWAY from this page and kill the
// stream's socket, so pressing "beeps" would appear to stop the video. fetch()
// leaves the page — and the MJPEG connection — exactly where it is.
constexpr char INDEX_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>brain</title>
<style>
body{margin:0;background:#111;color:#ddd;font:14px/1.4 system-ui,sans-serif;text-align:center}
h3{font-weight:600;margin:16px 0 12px}
img{max-width:100%;height:auto;background:#000}
p{margin:14px 0}
a{color:#8cf;margin:0 .75em}
button{font:inherit;padding:10px 18px;margin:0 6px;border:0;border-radius:6px;
background:#2a4a6a;color:#dff;cursor:pointer}
button:active{background:#1d3a52}
button.lv{padding:8px 12px;background:#24402c}
button.lv:active{background:#18301f}
button.lv.on{background:#3a6a44;color:#fff}
p.lbl{color:#8aa0b0;font-size:13px;margin:18px 0 4px}
p.lbl b{color:#cfe}
.imu{max-width:430px;margin:0 auto 8px}
.ir{display:flex;padding:2px 8px;color:#cde;font-variant-numeric:tabular-nums}
.ir span{color:#8aa0b0;flex:1;text-align:left}
.ir b{flex:1;text-align:right;font-weight:500}
.ibar{position:relative;height:8px;background:#1b2732;margin:3px 8px;
border-radius:4px}
.ibar i{position:absolute;top:0;bottom:0;left:50%;width:0;background:#4a9;
border-radius:4px}
.pad{display:grid;grid-template-columns:repeat(3,68px);gap:6px;justify-content:center;
margin:10px auto}
.pad button{width:68px;height:54px;padding:0;margin:0;font-size:20px;background:#2a3a4a}
.pad button.stop{background:#6a2a2a;font-size:13px}
.pad button.on{background:#3a6a8a}
.pad span{display:block}
input[type=range]{width:240px;vertical-align:middle}
.warn{color:#e0a0b0;font-size:12px;max-width:440px;margin:8px auto}
</style></head>
<body>
<h3>brain &mdash; ESP32-S3</h3>
<img id="cam" alt="stream">
<p><a href="/capture">snapshot</a><a href="/status">status</a></p>
<p class="lbl">drive &mdash; <b>hold</b> a direction; releasing brakes</p>
<div class="pad">
<span></span><button id="bf">&#9650;</button><span></span>
<button id="bl">&#9664;</button><button class="stop" onclick="ask('/stop')">STOP</button><button id="br">&#9654;</button>
<span></span><button id="bb">&#9660;</button><span></span>
</div>
<p class="lbl">speed <b><span id="spdv">40</span>%</b> &mdash; or arrow keys / WASD</p>
<p><input type="range" id="spd" min="0" max="100" value="40"></p>
<p class="lbl" id="drivestat">link: ?</p>
<p class="warn">Direction signs are unverified until the wheel mapping is
confirmed on hardware (bring-up steps 4/5) &mdash; a swapped or reversed motor
will make these buttons lie. Wheels off the ground for the first press.</p>
<p>
<button onclick="ask('/play?src=beeps')">beeps</button>
<button onclick="ask('/play?src=clip')">sound file</button>
</p>
<p class="lbl">level <b><span id="lvl">?</span></b> &mdash; used by the
<b>beeps</b> and <b>sound file</b> buttons. Picking a level plays nothing.</p><p class="lbl" id="imustat">IMU: ?</p>
<div class="imu">
<div class="ir"><span>accel (g)</span><b id="iax">-</b><b id="iay">-</b><b id="iaz">-</b></div>
<div class="ir"><span>gyro (&deg;/s)</span><b id="igx">-</b><b id="igy">-</b><b id="igz">-</b></div>
<div class="ir"><span>tilt (&deg;) / temp</span><b id="ipitch">-</b><b id="iroll">-</b><b id="itemp">-</b></div>
<div class="ibar"><i id="bax"></i></div>
<div class="ibar"><i id="bay"></i></div>
<div class="ibar"><i id="baz"></i></div>
</div>
<p>
<button class="lv" data-vol="10" onclick="ask('/control?vol=10')">very low</button>
<button class="lv" data-vol="20" onclick="ask('/control?vol=20')">low</button>
<button class="lv" data-vol="50" onclick="ask('/control?vol=50')">medium</button>
<button class="lv" data-vol="100" onclick="ask('/control?vol=100')">high</button>
<button class="lv" data-vol="200" onclick="ask('/control?vol=200')">very high</button>
</p>
<p class="lbl">lights &mdash; <b id="ledstat">run</b></p>
<p>
<button class="lv" id="ledrun" onclick="ask('/control?mode=run')">run</button>
<button class="lv" id="ledtor" onclick="ask('/control?mode=torch')">torch</button>
<button class="lv" id="ledhaz" onclick="ask('/control?mode=hazard')">hazard</button>
<button class="lv" id="ledoff" onclick="ask('/control?mode=off')">off</button>
</p>
<script>
// The stream lives on its OWN PORT, because it gets its own server task: its
// handler never returns while a client is watching, and one httpd task can serve
// only one thing at a time. Building the URL from location.hostname keeps this
// correct whatever address the board is reached on.
document.getElementById('cam').src = 'http://' + location.hostname + ':81/stream';

// The five levels the buttons offer, as per-mille. Kept here as well as on the
// server so the page can always name whatever level it is actually at -- including
// one set by curl, which will simply be shown as a bare percentage.
var LEVELS = [[10,'very low'],[20,'low'],[50,'medium'],[100,'high'],[200,'very high']];

// POST, not GET. These requests have SIDE EFFECTS, and a GET that the browser
// decides to cache is never sent to the board at all -- which looks exactly like
// "the button only works the first time". POST is not cached, and cache:'no-store'
// is belt and braces. The server sends no-store too.
function ask(url){
  fetch(url, {method:'POST', cache:'no-store'})
    .then(function(r){return r.json();})
    .then(function(j){ show(j); showLink(j); })
    .catch(function(e){console.log('request failed',e);});
}

function levelName(m){
  for(var i=0;i<LEVELS.length;i++){ if(LEVELS[i][0] === m) return LEVELS[i][1]; }
  return null;
}

// Reflect the level back and mark the active button. Without this a sticky level is
// invisible: pick "very low" once and every later press of beeps is quiet, with
// nothing on screen to say why.
function show(j){
  if(!j || j.volume_milli === undefined) return;
  var m = j.volume_milli;
  var name = levelName(m);
  document.getElementById('lvl').textContent =
      name ? (name + ' (' + (m/10) + '%)') : ((m/10) + '%');
  var bs = document.getElementsByClassName('lv');
  for(var i=0;i<bs.length;i++){
    var mine = parseInt(bs[i].getAttribute('data-vol'),10);
    bs[i].className = (mine === m) ? 'lv on' : 'lv';
  }

  // The lighting state is the board's to decide, so every reply that carries it wins.
  // These buttons are INDICATORS, not toggles: none of them assumes a state, which is
  // also what lets a mode set by curl show up correctly on the page.
  if(j.led_mode !== undefined){
    var lm = j.led_mode;
    document.getElementById('ledrun').className = (lm === 'run') ? 'lv on' : 'lv';
    document.getElementById('ledtor').className = (lm === 'torch') ? 'lv on' : 'lv';
    document.getElementById('ledhaz').className = (lm === 'hazard') ? 'lv on' : 'lv';
    document.getElementById('ledoff').className = (lm === 'off') ? 'lv on' : 'lv';
    document.getElementById('ledstat').textContent = lm;
  }
}

fetch('/status', {cache:'no-store'})
  .then(function(r){return r.json();})
  .then(function(j){ show(j); showLink(j); })
  .catch(function(){});

// --- drive ----------------------------------------------------------------
// HOLD semantics, not click semantics: pointerdown starts, pointerup/cancel/
// leave stops. While anything is held the page re-sends at 10 Hz, because the
// brain's control watchdog commands BRAKE after 250 ms without a command -- so a
// held control has to be continuously re-asserted, and that same property means
// a Wi-Fi link that drops stops the car by itself.
var held = {f:false,b:false,l:false,r:false};
var driving = false;
var driveBusy = false;

function speedPermille(){
  var v = parseInt(document.getElementById('spd').value, 10);
  return (isNaN(v) ? 0 : v) * 10;   // slider 0..100 % -> 0..1000 per-mille
}

document.getElementById('spd').addEventListener('input', function(){
  document.getElementById('spdv').textContent = this.value;
});

function sendDrive(){
  var any = held.f || held.b || held.l || held.r;
  if (!any) {
    // Released: brake EXPLICITLY. Letting the watchdog do it would also work,
    // but a quarter second of coasting on every release is not what "stop" means.
    if (driving) { driving = false; ask('/stop'); }
    return;
  }
  if (driveBusy) return;   // one request in flight, the same guard the IMU poll uses
  driveBusy = true;

  var s = speedPermille();
  var th = (held.f ? s : 0) + (held.b ? -s : 0);
  var turn = (held.l ? s : 0) + (held.r ? -s : 0);
  driving = true;

  fetch('/drive?th=' + th + '&turn=' + turn + '&twist=0',
        {method:'POST', cache:'no-store'})
    .then(function(r){return r.json();})
    .then(function(j){ showLink(j); })
    .catch(function(){})
    .then(function(){ driveBusy = false; });
}
setInterval(sendDrive, 100);

function bindHold(id, key){
  var el = document.getElementById(id);
  function down(e){
    held[key] = true;
    el.className = 'on';
    if (e.cancelable) e.preventDefault();
    // Pointer capture, so pointerup still arrives HERE if the finger slides off
    // the button. Without it the car keeps driving with nothing held down.
    if (el.setPointerCapture && e.pointerId !== undefined) {
      try { el.setPointerCapture(e.pointerId); } catch(_) {}
    }
  }
  function up(){ held[key] = false; el.className = ''; }
  el.addEventListener('pointerdown', down);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
  el.addEventListener('pointerleave', up);
  el.addEventListener('contextmenu', function(e){ e.preventDefault(); });
}
bindHold('bf','f'); bindHold('bb','b'); bindHold('bl','l'); bindHold('br','r');

var KEYS = {ArrowUp:'f',ArrowDown:'b',ArrowLeft:'l',ArrowRight:'r',
            w:'f',s:'b',a:'l',d:'r',W:'f',S:'b',A:'l',D:'r'};
window.addEventListener('keydown', function(e){
  var k = KEYS[e.key]; if (!k) return; held[k] = true; e.preventDefault();
});
window.addEventListener('keyup', function(e){
  var k = KEYS[e.key]; if (!k) return; held[k] = false; e.preventDefault();
});
// A key held down while the tab loses focus never gets its keyup. Clear
// everything, and the next tick brakes.
window.addEventListener('blur', function(){
  held = {f:false,b:false,l:false,r:false};
});

function showLink(j){
  if (!j || j.drive === undefined) return;
  var p = j.pico || {};
  var t = document.getElementById('drivestat');
  if (!p.seen) {
    t.textContent = 'link: no reply from the Pico (tx ' + j.link_tx + ', rx 0) \u2014 is it flashed with -e picolink?';
    return;
  }
  t.textContent = 'link: ' + j.drive
      + (j.control_timeout ? ' (watchdog -> BRAKE)' : '')
      + ' | pico replied ' + (p.age_ms === null ? '?' : p.age_ms + ' ms ago')
      + ', st 0x' + p.status.toString(16) + ', fault ' + p.fault
      + ', rx ' + j.link_rx + ', crc ' + j.link_crc;
}

// A slow /status poll, so the link readout stays live while nothing is being
// driven -- which is exactly when it is being looked at, trying to work out
// whether the Pico is talking at all.
function pollStatus(){
  fetch('/status', {cache:'no-store'})
    .then(function(r){return r.json();})
    .then(function(j){ show(j); showLink(j); })
    .catch(function(){});
}
setInterval(pollStatus, 1000);

// --- IMU, live ------------------------------------------------------------
// Polled at 15 Hz. That is a DISPLAY rate and nothing more: the firmware samples
// the sensor at 200 Hz in its own task, and this only decides how often a human
// sees it. The handler copies the last published sample, so polling faster costs
// the IMU task nothing at all -- it does not read the bus again.
//
// 1000/15 = 66.7 ms, so 66 ms keeps the real rate at or just above 15 Hz.
function bar(id, v, full){
  var el = document.getElementById(id);
  var w = Math.min(Math.abs(v) / full, 1) * 50;
  el.style.width = w + '%';
  el.style.left = (v >= 0 ? 50 : 50 - w) + '%';
}
function num(id, v, dp){
  document.getElementById(id).textContent = (v === null ? '-' : v.toFixed(dp));
}
function showImu(j){
  var st = document.getElementById('imustat');
  if(!j || !j.present){
    st.textContent = 'IMU: not detected -- check the boot log';
    return;
  }
  st.textContent = 'IMU: BMI160 at 0x69, sampling ' + j.rate + ' Hz, seq ' + j.seq
      + (j.errors ? ('  <-- ' + j.errors + ' read errors') : '');
  num('iax', j.ax, 3); num('iay', j.ay, 3); num('iaz', j.az, 3);
  num('igx', j.gx, 1); num('igy', j.gy, 1); num('igz', j.gz, 1);
  num('ipitch', j.pitch, 1); num('iroll', j.roll, 1); num('itemp', j.temp, 1);
  bar('bax', j.ax, 2); bar('bay', j.ay, 2); bar('baz', j.az, 2);
}
// *** ONE REQUEST IN FLIGHT AT A TIME. ***
// At 5 Hz a slow reply merely arrived late. At 15 Hz a reply that takes longer
// than the interval would let setInterval stack up requests, each opening its own
// socket, until the server's 7 are gone -- and lru_purge_enable then tears down
// sockets that are still in use. One boolean removes the failure mode; this guard
// is not there for speed.
var imuBusy = false;
function pollImu(){
  if(imuBusy) return;
  imuBusy = true;
  fetch('/imu', {cache:'no-store'})
    .then(function(r){return r.json();})
    .then(showImu)
    .catch(function(){})
    .then(function(){ imuBusy = false; });
}
setInterval(pollImu, 66);
pollImu();
</script>
</body></html>
)HTML";

esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  // The page is small and it changes with every firmware flash; caching it can
  // only ever mean serving a page that does not match the firmware running.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
    return ESP_FAIL;
  }

  esp_err_t res = ESP_OK;
  if (fb->format != PIXFORMAT_JPEG) {
    // Cannot happen with this project's config, but saying so beats serving
    // raw RGB565 bytes with a JPEG content type and leaving the browser to
    // guess.
    res = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                              "sensor is not in JPEG mode");
  } else {
    res = httpd_resp_set_type(req, "image/jpeg");
    if (res == ESP_OK) {
      res = httpd_resp_set_hdr(req, "Content-Disposition",
                               "inline; filename=capture.jpg");
    }
    if (res == ESP_OK) {
      res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    }
  }

  esp_camera_fb_return(fb);
  return res;
}

esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  // Harmless and occasionally useful: lets a page served from anywhere else
  // embed this stream.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  g_stream_clients++;

  uint32_t last_ms = millis();

  // The loop ends when a send fails, and the ONLY thing that makes a send fail
  // here is the client going away (or the socket timing out). That is the
  // disconnect detection — there is no separate "is the browser still there"
  // check, and there does not need to be.
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[stream] esp_camera_fb_get() returned NULL - stopping");
      res = ESP_FAIL;
      break;
    }

    if (fb->format != PIXFORMAT_JPEG) {
      esp_camera_fb_return(fb);
      Serial.println("[stream] sensor is not in JPEG mode - stopping");
      res = ESP_FAIL;
      break;
    }

    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));

    if (res == ESP_OK) {
      char part[64];
      const size_t hlen =
          snprintf(part, sizeof(part), STREAM_PART, (unsigned)fb->len);
      res = httpd_resp_send_chunk(req, part, hlen);
    }

    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }

    // Frame rate, as a 1/4-weight exponential average: enough to smooth the
    // jitter of a JPEG encode without lagging a real change by seconds.
    const uint32_t now_ms = millis();
    const uint32_t dt = now_ms - last_ms;
    last_ms = now_ms;
    if (dt > 0) {
      g_fps = (g_fps * 3 + (1000u / dt)) / 4u;
    }

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;  // client gone

    // esp_camera_fb_get() above already yields to the scheduler while it waits
    // for a frame, so no delay is needed here. Do not insert one: it would
    // lower the frame rate by exactly the delay.
  }

  g_stream_clients--;
  return res;
}

esp_err_t send_status_json(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();

  motor_link::State ls;
  motor_link::state(&ls);

  // Built in TWO snprintfs on purpose. The link adds a dozen more conversions,
  // and a 40-argument format string is exactly where an argument/format
  // mismatch hides -- splitting it keeps the new ones away from the old ones.
  // The first part deliberately does not close the brace; the link fields are
  // appended and then the object is closed.
  char json[1024];
  int n = snprintf(
      json, sizeof(json),
      "{\"heap\":%u,\"psram_free\":%u,\"ap_stations\":%u,\"clients\":%u,\"fps\":%u,"
      "\"framesize\":\"%s\",\"quality\":%d,\"brightness\":%d,\"contrast\":%d,"
      "\"saturation\":%d,\"hmirror\":%d,\"vflip\":%d,"
      "\"audio\":\"%s\",\"sample_rate\":%u,\"volume_milli\":%u,"
      "\"led_mode\":\"%s\",\"led\":[%u,%u,%u],",
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
      (unsigned)WiFi.softAPgetStationNum(), (unsigned)g_stream_clients,
      (unsigned)g_fps,
      s ? size_name(s->status.framesize) : "?",
      s ? (int)s->status.quality : -1, s ? (int)s->status.brightness : 0,
      s ? (int)s->status.contrast : 0, s ? (int)s->status.saturation : 0,
      s ? (int)s->status.hmirror : 0, s ? (int)s->status.vflip : 0,
      audio_now_playing(), (unsigned)audio_sample_rate(),
      (unsigned)(audio_volume() * 1000.0f + 0.5f),
      led_mode_name(g_led_mode), (unsigned)g_led_r, (unsigned)g_led_g,
      (unsigned)g_led_b);

  // A Pico that has never replied has no age. `null` is honest and parses; the
  // raw 0xFFFFFFFF would be a number a client would believe.
  char age[16];
  if (ls.pico_seen) {
    snprintf(age, sizeof(age), "%lu", (unsigned long)ls.pico_age_ms);
  } else {
    strcpy(age, "null");
  }

  if (n > 0 && n < (int)sizeof(json)) {
    n += snprintf(
        json + n, sizeof(json) - (size_t)n,
        "\"drive\":\"%s\",\"control_timeout\":%s,\"duty\":[%d,%d,%d,%d],"
        "\"link_tx\":%lu,\"link_rx\":%lu,\"link_crc\":%lu,"
        "\"pico\":{\"seen\":%s,\"status\":%u,\"fault\":%u,\"age_ms\":%s,"
        "\"uptime_ms\":%u}}\n",
        motor_link::mode_name(ls.mode), ls.control_timeout ? "true" : "false",
        (int)ls.duty[0], (int)ls.duty[1], (int)ls.duty[2], (int)ls.duty[3],
        (unsigned long)ls.tx_frames, (unsigned long)ls.rx_replies,
        (unsigned long)ls.rx_crc_errors, ls.pico_seen ? "true" : "false",
        (unsigned)ls.pico_status, (unsigned)ls.pico_fault, age,
        (unsigned)ls.pico_uptime_ms);
  }

  if (n < 0 || n >= (int)sizeof(json)) {
    // Truncated JSON is not a partial answer, it is an unparseable one -- and on
    // the page it shows up as the whole status panel silently going blank. Say so
    // in the log rather than letting it look like a client bug.
    Serial.println("[http] WARNING: /status JSON truncated -- raise json[]");
    n = (int)sizeof(json) - 1;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  // SEE THE NOTE IN play_handler. Without this, a browser may satisfy a later
  // /play or /control from its cache and the board never hears about it.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, n);
}

esp_err_t status_handler(httpd_req_t *req) { return send_status_json(req); }

// /imu — the latest sample, never a read of the bus.
//
// The sampler task owns the bus and runs at 200 Hz; this handler only copies the
// most recent sample, so polling it from a page costs nothing and cannot disturb
// the timing of the sampler. `seq` is there so a client can tell "the values are
// stable" from "the sampler stopped" -- those look identical in the numbers.
esp_err_t imu_handler(httpd_req_t *req) {
  ImuSample s;
  imu_latest(&s);

  char json[320];
  const int n = snprintf(
      json, sizeof(json),
      "{\"present\":%s,\"who\":%u,\"seq\":%u,\"errors\":%u,\"rate\":%u,"
      "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
      "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,"
      "\"pitch\":%.2f,\"roll\":%.2f,\"temp\":%.1f}\n",
      imu_present() ? "true" : "false", (unsigned)imu_who_am_i(), (unsigned)s.seq,
      (unsigned)s.errors, (unsigned)imu_rate_hz(), s.ax, s.ay, s.az, s.gx, s.gy,
      s.gz, s.pitch, s.roll, s.temp_c);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, n);
}

// /play?src=beeps | src=clip | vol=0..1000 (per-mille, optional)
//
// Sets a request and returns immediately — see the note at the top of this file.
// The reply is the same JSON as /status, so the caller can see `audio` flip.
//
// `vol` is accepted with or without `src`: `/play?vol=100` on its own just sets
// the level for the next sound, which is what the level buttons use when they are
// only changing the setting.
esp_err_t play_handler(httpd_req_t *req) {
  char params[128];
  params_load(req, params, sizeof(params));

  // Log every arriving request. If a button press produces NO line here, the
  // request never reached the board at all (a cached GET, a dead socket, the
  // wrong IP) -- and that is a completely different bug from one that arrives
  // and plays quietly.
  Serial.printf("[http] /play: '%s'\n", params);

  const bool vol_given = apply_volume_param(params);

  char src[16];
  if (!param_str(params, "src", src, sizeof(src))) {
    if (vol_given) return send_status_json(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        "{\"error\":\"need src=beeps|clip, optional vol=0..1000 (per-mille)\"}");
  }

  if (strcmp(src, "beeps") == 0) {
    audio_request_beeps();
  } else if (strcmp(src, "clip") == 0) {
    audio_request_clip();
  } else {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "{\"error\":\"src must be beeps or clip\"}");
  }

  return send_status_json(req);
}

// /control accepts any subset of these, in the query string or a form body:
//
//   framesize=qvga|cif|vga|svga|xga|sxga|uxga   (or a raw number)
//   quality=0..63              lower = better quality, bigger frames
//   brightness, contrast, saturation = -2..2
//   hmirror, vflip = 0|1
//   mode = run|torch|hazard|off the car's lighting state (see app_httpd.h)
//   group = <name> + r,g,b = 0..255  lights ONE group and opens the Custom mode
//   led_r, led_g, led_b = 0..255   fills the whole strip (also Custom)
//
// A request that carries `group` is ALWAYS a group write and nothing else: it is
// handled and answered on its own, so `group` and `led_r/g/b` can never be confused
// for each other even though they mean similar things. Missing r/g/b in a group write
// are 0.
//
// `mode` is applied FIRST, so a request carrying both it and a colour ends up on the
// explicit lighting state rather than on whatever the colour implied.
//
// It always answers with the same JSON as /status, so a caller can see the
// result of what it just set — including when the sensor silently refused.
esp_err_t control_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no sensor");
    return ESP_FAIL;
  }

  char params[256];
  params_load(req, params, sizeof(params));

  char v[24];
  int n;

  if (param_str(params, "framesize", v, sizeof(v))) {
    framesize_t fs;
    if (size_parse(v, &fs)) {
      // Ignore the return value: the JSON reply reports s->status.framesize,
      // which is the truth about what the sensor actually accepted.
      s->set_framesize(s, fs);
    }
  }
  if (param_int(params, "quality", &n)) s->set_quality(s, n);
  if (param_int(params, "brightness", &n)) s->set_brightness(s, n);
  if (param_int(params, "contrast", &n)) s->set_contrast(s, n);
  if (param_int(params, "saturation", &n)) s->set_saturation(s, n);
  if (param_int(params, "hmirror", &n)) s->set_hmirror(s, n);
  if (param_int(params, "vflip", &n)) s->set_vflip(s, n);

  // Also accepted here, so the level can be set without playing anything.
  // (Accepted on /play too -- see there.)
  apply_volume_param(params);

  // A group write is its own command shape: it addresses part of the strip and
  // opens the Custom mode, so it is handled and answered on its own rather than
  // falling through to the whole-strip colour below.
  char gname[24];
  if (param_str(params, "group", gname, sizeof(gname))) {
    LedGroup which;
    if (!led_group_parse(gname, &which)) {
      return httpd_resp_send_err(
          req, HTTPD_400_BAD_REQUEST,
          "{\"error\":\"unknown group -- tail_right|tail_centre|tail_left|"
          "floor|head_left|head_right\"}");
    }
    int gr = 0, gg = 0, gb = 0;
    param_int(params, "r", &gr);
    param_int(params, "g", &gg);
    param_int(params, "b", &gb);
    // Logged like /play and /drive: "the LED did not change" has to split into
    // never arrived vs arrived with the wrong group name.
    Serial.printf("[http] /control group=%s r=%d g=%d b=%d\n", gname, gr, gg, gb);
    led_set_group(which, clamp8(gr), clamp8(gg), clamp8(gb));
    return send_status_json(req);
  }

  // The lighting state first, so a request carrying both a mode and a colour ends up
  // on the mode.
  if (param_str(params, "mode", v, sizeof(v))) {
    if (strcmp(v, "run") == 0) {
      led_set_mode(LedMode::Run);
    } else if (strcmp(v, "torch") == 0) {
      led_set_mode(LedMode::Torch);
    } else if (strcmp(v, "hazard") == 0) {
      led_set_mode(LedMode::Hazard);
    } else if (strcmp(v, "off") == 0) {
      led_set_mode(LedMode::Off);
    } else {
      return httpd_resp_send_err(
          req, HTTPD_400_BAD_REQUEST,
          "{\"error\":\"mode must be run, torch, hazard or off\"}");
    }
    // Logged like /play and /drive, for the same reason: "the lights did nothing"
    // has to split into never arrived vs arrived with a bad value.
    Serial.printf("[http] /control mode=%s\n", v);
  }

  bool touched_led = false;
  if (param_int(params, "led_r", &n)) { g_led_r = clamp8(n); touched_led = true; }
  if (param_int(params, "led_g", &n)) { g_led_g = clamp8(n); touched_led = true; }
  if (param_int(params, "led_b", &n)) { g_led_b = clamp8(n); touched_led = true; }

  if (touched_led) {
    // An explicit colour IS the manual override, so it lands in Custom and stays
    // there: nothing repaints it until a mode is asked for again.
    led_set(g_led_r, g_led_g, g_led_b);
  }

  return send_status_json(req);
}

// /drive -- the motors. Two shapes, chosen by which parameters are present:
//
//   /drive?th=..&turn=..&twist=..     the mixer  (per-mille, each +-1000)
//   /drive?fl=..&rl=..&rr=..&fr=..    the raw four duties, bypassing the mixer
//
// Mixer inputs, all optional and defaulting to 0:
//   th     throttle     + forward, - reverse
//   turn   left/right   + added to the LEFT wheels, - to the right
//   twist  front/rear   + added to the FRONT wheels, - to the rear
//
// *** THE SIGNS ARE car_control's, AND THEY ARE UNVERIFIED. *** The original car
// used this same arithmetic with a comment calling +turn "left", while the duties
// it produces (left wheels forward, right wheels back) rotate a car the other
// way. Nobody has yet matched FL/RL/RR/FR to the actual wheels -- bring-up steps
// 4/5 -- so treat every direction here as a coin flip until that is closed. The
// ARITHMETIC is certain; which wheel moves is the open question, and the two
// cannot be settled from the same evidence.
//
// EVERY request here refreshes the brain's own control watchdog. If they stop
// arriving for 250 ms the brain commands BRAKE by itself (see motor_link.h), and
// that is what makes a phone that sleeps or leaves Wi-Fi range a stopped car
// rather than one that keeps driving at the last commanded duty.
esp_err_t drive_handler(httpd_req_t *req) {
  char params[128];
  params_load(req, params, sizeof(params));

  // All four are parsed before any is tested. `||` would short-circuit and leave
  // the later ones unread, turning a complete raw request into a misleading
  // "raw mode needs all four".
  int fl = 0, rl = 0, rr = 0, fr = 0;
  const bool has_fl = param_int(params, "fl", &fl);
  const bool has_rl = param_int(params, "rl", &rl);
  const bool has_rr = param_int(params, "rr", &rr);
  const bool has_fr = param_int(params, "fr", &fr);

  if (has_fl || has_rl || has_rr || has_fr) {
    if (!(has_fl && has_rl && has_rr && has_fr)) {
      return httpd_resp_send_err(
          req, HTTPD_400_BAD_REQUEST,
          "{\"error\":\"raw mode needs all four of fl, rl, rr, fr (per-mille)\"}");
    }
    const int16_t duty[proto::MOTOR_COUNT] = {(int16_t)fl, (int16_t)rl,
                                              (int16_t)rr, (int16_t)fr};
    motor_link::drive_raw(duty);
    // Logged for the same reason /play is: it splits "the car did not move" into
    // never arrived vs arrived and commanded nothing.
    Serial.printf("[http] /drive raw fl=%d rl=%d rr=%d fr=%d\n", fl, rl, rr, fr);
    return send_status_json(req);
  }

  int th = 0, turn = 0, twist = 0;
  const bool has_th = param_int(params, "th", &th);
  const bool has_turn = param_int(params, "turn", &turn);
  const bool has_twist = param_int(params, "twist", &twist);
  if (!(has_th || has_turn || has_twist)) {
    return httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        "{\"error\":\"use th/turn/twist, or all four of fl/rl/rr/fr (per-mille)\"}");
  }

  // Note the argument order: motor_link::drive() takes the MIXER's order
  // (throttle, front_rear, left_right), so twist and turn swap places here. That
  // is also why the URL names are not `fr` and `lr`: `fr` already means
  // "front right" in raw mode, and a parameter that means two things depending on
  // which other parameters are present is a bug waiting to be written.
  motor_link::drive((int16_t)th, (int16_t)twist, (int16_t)turn);
  Serial.printf("[http] /drive th=%d turn=%d twist=%d\n", th, turn, twist);
  return send_status_json(req);
}

// /stop[?mode=brake|coast|disable]
//
// BRAKE is the default because a stop that coasts is not a stop -- the car keeps
// rolling, which is the thing the whole fail-safe chain exists to prevent. COAST
// and DISABLE are offered because sometimes that is what is wanted: coast to push
// the car by hand, disable to park it with nothing energised at all.
//
// The three differ in a way worth knowing when debugging: BRAKE holds the motors
// shorted (stops fast, draws nothing unless the wheels turn), COAST leaves the
// outputs high-Z but the drivers enabled, and DISABLE drops the Pico's STBY so
// the drivers themselves are off.
esp_err_t stop_handler(httpd_req_t *req) {
  char params[64];
  params_load(req, params, sizeof(params));

  char mode[16];
  const bool given = param_str(params, "mode", mode, sizeof(mode));

  if (!given || strcmp(mode, "brake") == 0) {
    motor_link::brake();
    Serial.println("[http] /stop -> BRAKE");
  } else if (strcmp(mode, "coast") == 0) {
    motor_link::coast();
    Serial.println("[http] /stop -> COAST");
  } else if (strcmp(mode, "disable") == 0) {
    motor_link::disable();
    Serial.println("[http] /stop -> DISABLE (STBY low at the Pico)");
  } else {
    return httpd_resp_send_err(
        req, HTTPD_400_BAD_REQUEST,
        "{\"error\":\"mode must be brake, coast or disable\"}");
  }

  return send_status_json(req);
}

// --- registration ---------------------------------------------------------
struct Route {
  const char *uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t *);
};

esp_err_t reg(httpd_handle_t server, const char *uri, httpd_method_t method,
              esp_err_t (*handler)(httpd_req_t *)) {
  // Fields assigned one by one rather than with designated initialisers: this
  // is C++, where .field = value in an initialiser list is a compiler
  // extension. It works with this toolchain but warns under -Wpedantic.
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = method;
  u.handler = handler;
  u.user_ctx = nullptr;
  return httpd_register_uri_handler(server, &u);
}

// Bring up ONE httpd instance on `port`, with `n` routes.
//
// *** WHY THIS FUNCTION EXISTS, AND WHY IT IS CALLED TWICE. ***
// esp_http_server runs ALL of a server's handlers in ONE FreeRTOS task --
// httpd_config_t has a single `task_priority`, documented as "the task which runs
// the server". So a handler that does not return blocks that whole instance, and
// its priority is irrelevant because there is nothing to preempt: the task is not
// spinning, it is parked inside someone's handler.
//
// That is exactly what /stream does. It loops for as long as a client stays
// connected -- which is the whole point of MJPEG -- so while anyone is watching
// the video, every other endpoint on that port goes dead. The symptom is
// unmistakable once you know it: button presses do nothing, and then a board
// reset tears the stream down and the queued-up requests are all served at once.
//
// Hence two servers: the stream on its own port, so it can block forever without
// taking /play, /status and /control with it.
esp_err_t start_on_port(uint16_t port, const Route *routes, size_t n) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  // *** THE SECOND PORT THAT CATCHES EVERYONE OUT. ***
  // esp_http_server also opens a UDP control socket, on `ctrl_port`, and
  // HTTPD_DEFAULT_CONFIG() gives EVERY instance the same default (32768). Two
  // servers on one chip therefore fight over it and the second one fails at boot
  // with a thoroughly misleading message:
  //     E httpd: httpd_server_init: error in creating ctrl socket (112)
  // 112 is EADDRINUSE. It is not about the port you were thinking of, and it is
  // not about the socket you asked for. Deriving it from `port` keeps the two
  // instances distinct and needs no second constant to forget.
  config.ctrl_port = 32768 + port;
  // 12 routes live on port 80 as of the link work (/, /capture, /status, /imu,
  // /play x2, /control x2, /drive x2, /stop x2), so the old value of 12 is now
  // exactly the count -- and httpd_start() fails outright if the list outgrows
  // this, at boot, with a message about handlers rather than about this number.
  config.max_uri_handlers = 16;
  // Recycle a socket if all of them are in use. Without this, one abandoned
  // browser tab can make the camera look dead to every later client.
  config.lru_purge_enable = true;
  // 4 KB (the default) is tight for a task that snprintfs a header per frame.
  config.stack_size = 8192;
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;
  // *** PIN THIS SERVER TO CORE 0. ***
  // HTTPD_DEFAULT_CONFIG() leaves core_id = tskNO_AFFINITY, so a server task may
  // land on core 1 -- the core the plan reserves for the control/link work, at
  // the SAME priority 5. It was harmless while core 1 only held the 200 Hz IMU
  // sampler; it stops being harmless now that a 100 Hz frame task also has to be
  // on time, because two equal-priority tasks on one core round-robin and the
  // frame schedule is what the Pico's dead-man is measured against. Pinning the
  // servers to core 0 is the fix the core map in /memories/repo/esp32-brain.md
  // asks for before the control loop arrives. There are TWO instances, so both
  // get it -- this function starts both.
  config.core_id = 0;

  httpd_handle_t server = nullptr;
  esp_err_t res = httpd_start(&server, &config);
  if (res != ESP_OK) return res;

  for (size_t i = 0; i < n; ++i) {
    res = reg(server, routes[i].uri, routes[i].method, routes[i].handler);
    if (res != ESP_OK) return res;
  }
  return ESP_OK;
}

}  // namespace

int server_start() {
  static const Route kControlRoutes[] = {
      {"/", HTTP_GET, index_handler},
      {"/capture", HTTP_GET, capture_handler},
      {"/status", HTTP_GET, status_handler},
      {"/imu", HTTP_GET, imu_handler},
      {"/play", HTTP_GET, play_handler},
      {"/play", HTTP_POST, play_handler},
      // Both verbs on purpose: a browser or curl can use GET, while anything
      // that treats this as a REST endpoint will use POST.
      {"/control", HTTP_GET, control_handler},
      {"/control", HTTP_POST, control_handler},
      // The motors. Same reasoning as /play and /control: POST is not cached, and
      // a cached GET of a driving command would be a command the board never
      // heard about -- which on the car is indistinguishable from a dead link.
      {"/drive", HTTP_GET, drive_handler},
      {"/drive", HTTP_POST, drive_handler},
      {"/stop", HTTP_GET, stop_handler},
      {"/stop", HTTP_POST, stop_handler},
  };
  // /stream is NOT in the list above, deliberately.
  static const Route kStreamRoutes[] = {
      {"/stream", HTTP_GET, stream_handler},
  };

  esp_err_t res =
      start_on_port(kControlPort, kControlRoutes,
                    sizeof(kControlRoutes) / sizeof(kControlRoutes[0]));
  if (res != ESP_OK) return (int)res;

  res = start_on_port(kStreamPort, kStreamRoutes,
                      sizeof(kStreamRoutes) / sizeof(kStreamRoutes[0]));
  if (res != ESP_OK) return (int)res;

  return (int)ESP_OK;
}

uint32_t stream_clients() { return g_stream_clients; }
uint32_t fps() { return g_fps; }

void led_set(uint8_t r, uint8_t g, uint8_t b) {
  g_led_r = r;
  g_led_g = g;
  g_led_b = b;
  led_apply(r, g, b);
}

// Changing state does NOT repaint here: led_task()'s renderer is the only thing that
// knows how to build a frame, and for Run/Torch/Hazard it runs within 30 ms. Doing it
// in two places is how the two would drift apart.
void led_set_mode(LedMode m) { g_led_mode = m; }

// Print the layout from kGroups, so the boot banner cannot drift from the table
// that led_set_group() actually uses.
void led_print_layout() {
  Serial.printf("[led] %d pixels:", kStripLeds);
  for (size_t i = 0; i < kGroupCount; ++i) {
    Serial.printf("  %s %u-%u", kGroups[i].name, (unsigned)kGroups[i].first,
                  (unsigned)(kGroups[i].first + kGroups[i].count - 1));
  }
  Serial.println();
}

void led_get(uint8_t *r, uint8_t *g, uint8_t *b) {
  if (r) *r = g_led_r;
  if (g) *g = g_led_g;
  if (b) *b = g_led_b;
}

void led_task(uint32_t now_ms) {
  // Custom is the one state the firmware does NOT animate: the scene is whatever the
  // last group write made it, and rebuilding it here would throw the user's work away
  // 30 ms after they did it.
  if (g_led_mode == LedMode::Custom) return;

  if (now_ms - g_led_last_ms < kAnimIntervalMs) return;
  g_led_last_ms = now_ms;

  g_floor_hue = (uint8_t)(g_floor_hue + kRainbowStep);
  scene_render(now_ms);
  led_push(g_scene, kStripLeds);
}

}  // namespace app
