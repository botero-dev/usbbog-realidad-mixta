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
#include "esp32-hal-rgb-led.h"  // neopixelWrite()

#include "camera_pins.h"
#include "esp_camera.h"

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

bool g_led_auto = true;  // true = status mode, false = user's torch colour
uint8_t g_led_r = 0, g_led_g = 0, g_led_b = 0;
// Deliberately not equal to any real colour, so the first led_apply() always
// writes. 0,0,0 is a real colour and would be skipped otherwise.
uint8_t g_applied_r = 1, g_applied_g = 1, g_applied_b = 1;
uint32_t g_led_last_ms = 0;

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

void led_apply(uint8_t r, uint8_t g, uint8_t b) {
  if (r == g_applied_r && g == g_applied_g && b == g_applied_b) return;
  g_applied_r = r;
  g_applied_g = g;
  g_applied_b = b;
  // RMT, never PWM. See app_httpd.h.
  neopixelWrite(PIN_LED_STRIP, r, g, b);
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
</style></head>
<body>
<h3>brain &mdash; ESP32-S3</h3>
<img id="cam" alt="stream">
<p><a href="/capture">snapshot</a><a href="/status">status</a></p>
<p>
<button onclick="ask('/play?src=beeps')">beeps</button>
<button onclick="ask('/play?src=clip')">sound file</button>
</p>
<p class="lbl">level <b><span id="lvl">?</span></b> &mdash; used by the
<b>beeps</b> and <b>sound file</b> buttons. Picking a level plays nothing.</p>
<p>
<button class="lv" data-vol="10" onclick="ask('/control?vol=10')">very low</button>
<button class="lv" data-vol="20" onclick="ask('/control?vol=20')">low</button>
<button class="lv" data-vol="50" onclick="ask('/control?vol=50')">medium</button>
<button class="lv" data-vol="100" onclick="ask('/control?vol=100')">high</button>
<button class="lv" data-vol="200" onclick="ask('/control?vol=200')">very high</button>
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
    .then(show)
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
}
fetch('/status', {cache:'no-store'})
  .then(function(r){return r.json();})
  .then(show)
  .catch(function(){});
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

  char json[512];
  const int n = snprintf(
      json, sizeof(json),
      "{\"heap\":%u,\"psram_free\":%u,\"clients\":%u,\"fps\":%u,"
      "\"framesize\":\"%s\",\"quality\":%d,\"brightness\":%d,\"contrast\":%d,"
      "\"saturation\":%d,\"hmirror\":%d,\"vflip\":%d,"
      "\"audio\":\"%s\",\"sample_rate\":%u,\"volume_milli\":%u,"
      "\"led_auto\":%s,\"led\":[%u,%u,%u]}\n",
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
      (unsigned)g_stream_clients, (unsigned)g_fps,
      s ? size_name(s->status.framesize) : "?",
      s ? (int)s->status.quality : -1, s ? (int)s->status.brightness : 0,
      s ? (int)s->status.contrast : 0, s ? (int)s->status.saturation : 0,
      s ? (int)s->status.hmirror : 0, s ? (int)s->status.vflip : 0,
      audio_now_playing(), (unsigned)audio_sample_rate(),
      (unsigned)(audio_volume() * 1000.0f + 0.5f),
      g_led_auto ? "true" : "false", (unsigned)g_led_r, (unsigned)g_led_g,
      (unsigned)g_led_b);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  // SEE THE NOTE IN play_handler. Without this, a browser may satisfy a later
  // /play or /control from its cache and the board never hears about it.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, n);
}

esp_err_t status_handler(httpd_req_t *req) { return send_status_json(req); }

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
//   led_r, led_g, led_b = 0..255   sets the torch and turns led_auto OFF
//   led_auto = 0|1                 hands the LED back to status indication
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

  bool touched_led = false;
  if (param_int(params, "led_r", &n)) { g_led_r = clamp8(n); touched_led = true; }
  if (param_int(params, "led_g", &n)) { g_led_g = clamp8(n); touched_led = true; }
  if (param_int(params, "led_b", &n)) { g_led_b = clamp8(n); touched_led = true; }

  if (touched_led) {
    // Setting a colour IS the request for a torch. Otherwise the 500 ms status
    // update would wipe it out and the endpoint would look broken.
    g_led_auto = false;
    led_apply(g_led_r, g_led_g, g_led_b);
  }

  if (param_int(params, "led_auto", &n)) {
    g_led_auto = (n != 0);
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
  config.max_uri_handlers = 8;
  // Recycle a socket if all of them are in use. Without this, one abandoned
  // browser tab can make the camera look dead to every later client.
  config.lru_purge_enable = true;
  // 4 KB (the default) is tight for a task that snprintfs a header per frame.
  config.stack_size = 8192;
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;

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
      {"/play", HTTP_GET, play_handler},
      {"/play", HTTP_POST, play_handler},
      // Both verbs on purpose: a browser or curl can use GET, while anything
      // that treats this as a REST endpoint will use POST.
      {"/control", HTTP_GET, control_handler},
      {"/control", HTTP_POST, control_handler},
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
  g_led_auto = false;
  led_apply(r, g, b);
}

void led_auto(bool enable) { g_led_auto = enable; }
bool led_auto_enabled() { return g_led_auto; }

void led_get(uint8_t *r, uint8_t *g, uint8_t *b) {
  if (r) *r = g_led_r;
  if (g) *g = g_led_g;
  if (b) *b = g_led_b;
}

void led_task(uint32_t now_ms) {
  if (!g_led_auto) return;
  if (now_ms - g_led_last_ms < 500) return;
  g_led_last_ms = now_ms;

  // Dim on purpose. The on-board WS2812 is bright enough at 30/255 to read
  // across a room, and every millivolt here is shared with the camera, the radio
  // and the amp (plan 7.1's LM2596 brownout argument).
  //
  // Audio wins over streaming: "is it actually playing?" is the question you ask
  // when the amp is silent, and the stream is visible on the screen anyway.
  if (audio_busy()) {
    led_apply(40, 40, 40);  // white: a sound is playing
  } else if (g_stream_clients > 0) {
    led_apply(0, 48, 0);  // green: someone is watching the stream
  } else {
    led_apply(0, 0, 48);  // dim blue: AP up, nobody watching, silent
  }
}

}  // namespace app
