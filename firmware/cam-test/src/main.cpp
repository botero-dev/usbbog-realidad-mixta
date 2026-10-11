// main.cpp — cam-test entry point: camera + SoftAP + MJPEG web stream.
// ---------------------------------------------------------------------------
// THIS IS A STANDALONE BRING-UP PROJECT, not part of firmware/brain.
//
// The reason is separability. The brain project already plays audio and that is
// verified on hardware; if the camera were added there, the first failure could
// be camera, audio, PSRAM bandwidth or the radio, and nothing would tell you
// which. Here there is no audio and no motor link, so a failure has one
// candidate. Merging the two afterwards is what actually tests plan section 6's
// warning that camera DMA and I2S DMA share the octal PSRAM bus.
//
// What it does (plan section 11, bring-up step 1):
//   1. brings the camera up and reports what the sensor says it is;
//   2. raises a SoftAP on 192.168.4.1, same shape as the network-test project,
//      so the Godot app's http://192.168.4.1/... addressing keeps working;
//   3. serves a page whose only job is <img src="/stream">.
//
// Follows the same shape as the network-test project on purpose: SoftAP rather
// than STA, because there is no router in a corridor and nothing to join.
//
// Wiring: NONE. The OV3660 is on the board's own FPC connector (plan section 2:
// the left header row is 100 % camera). Everything here is on-board.
//
// NOT in this project, deliberately: audio, the BMI160, the Pico link, motors.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>

#include "app_httpd.h"
#include "camera_pins.h"
#include "esp_camera.h"

// Credentials: `secrets.h` if it exists, otherwise the throwaway defaults in
// secrets.example.h. Copy the example to set a real SSID/password; this
// project never needs one to run.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

namespace {

// --- camera: the numbers, and why ----------------------------------------
// QVGA/VGA first, UXGA later. Bring-up rule from the Pico side of this repo:
// change one thing at a time. If the first stream is set to UXGA and it stutters,
// you cannot tell a config problem from a bandwidth problem. VGA at quality 12
// is about 20-40 KB per frame and is obviously fine; climb once it is stable.
constexpr framesize_t kFrameSize = FRAMESIZE_VGA;
constexpr int kQuality = 12;  // 0..63, LOWER is better and larger

// 20 MHz is the usual XCLK for this sensor. The header notes that on the S3,
// 16 MHz can enable EDMA mode; leave that alone until there is a reason.
constexpr int kXclkHz = 20000000;

// --- orientation: a MOUNTING fact, not a preference -----------------------
// This carrier's OV3660 sits ROTATED relative to the DVP connector's idea of
// "up", so an unflipped stream is upside down. That is a property of THIS board
// with THIS sensor, which is why it lives here as a boot default rather than as
// a /control call someone has to remember after every reset.
//
// Finding the right pair without a reflash — try in this order:
//     curl 'http://192.168.4.1/control?vflip=1'                  (upside down)
//     curl 'http://192.168.4.1/control?hmirror=1&vflip=1'        (rotated 180°)
//     curl 'http://192.168.4.1/control?hmirror=1'                (mirrored)
//
// The flip is applied IN THE SENSOR, not by a CSS transform on the page, and
// that distinction matters: it also fixes /capture, and it fixes whatever the
// frame looks like to code. A flip in the page would leave the actual pixel data
// mirrored, which is exactly the kind of lie that breaks vision or odometry later.
constexpr int kSensorVFlip = 1;   // was upside down on this board: confirmed 2026-10-11
constexpr int kSensorHMirror = 0;

esp_err_t camera_init() {
  const bool has_psram = ESP.getPsramSize() > 0;

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.pin_xclk = CAM_PIN_XCLK;
  // NOTE the field names: this bundled esp32-camera uses the modern
  // `pin_sccb_sda` / `pin_sccb_scl`. The older `pin_sscb_*` spelling still
  // exists in the header but is marked deprecated and emits a warning.
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_pclk = CAM_PIN_PCLK;

  config.xclk_freq_hz = kXclkHz;
  config.pixel_format = PIXFORMAT_JPEG;

  // JPEG, not RGB565, because the MJPEG stream wants JPEG and doing the
  // conversion in the sensor pipeline is free. RGB565 would mean encoding a
  // JPEG per frame in software, which is the expensive mistake here.
  config.frame_size = kFrameSize;
  config.jpeg_quality = kQuality;

  // Two buffers when PSRAM is there: one fills while the other is being sent,
  // so the stream does not stall behind the network. Without PSRAM, one buffer
  // in internal RAM — the frame size has to come down with it.
  config.fb_count = has_psram ? 2 : 1;
  config.fb_location = has_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  // When EMPTY rather than LATEST: with two buffers this never waits for a
  // frame that is being written, and for a stream a slightly older frame is
  // indistinguishable from a newer one.
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  return esp_camera_init(&config);
}

void print_camera_report() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("[cam] no sensor handle -- esp_camera_init() must have failed");
    return;
  }
  // The PID is the useful line here. It is how you find out that the board
  // shipped an OV3660 and not the OV2640 that half the internet assumes.
  Serial.printf("[cam] PID 0x%04x  (OV3660 = 0x3660, OV2640 = 0x2640)\n", s->id.PID);
  Serial.printf("[cam] sensor version 0x%04x, MID 0x%02x%02x\n", s->id.VER,
                s->id.MIDH, s->id.MIDL);
  // The name for the frame size lives in the HTTP layer (/status); here the raw
  // enum is enough, and printing it means this log does not depend on that table.
  Serial.printf("[cam] framesize enum %d, quality %d\n", (int)s->status.framesize,
                (int)s->status.quality);
  Serial.printf("[cam] xclk %d Hz, pixel format JPEG\n", kXclkHz);
}

// Apply the mounting orientation. Safe to call only after a successful
// esp_camera_init(); set_vflip/set_hmirror write sensor registers directly and
// take effect immediately, including on a stream that is already running.
void apply_default_orientation() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  s->set_hmirror(s, kSensorHMirror);
  s->set_vflip(s, kSensorVFlip);
  Serial.printf("[cam] orientation: hmirror %d, vflip %d (mounted rotated)\n",
                kSensorHMirror, kSensorVFlip);
}

// --- access point: same conventions as the network-test project -----------
constexpr int kApChannel = 1;
constexpr int kApMaxClients = 4;

void start_access_point() {
  WiFi.persistent(false);   // do not write Wi-Fi config to NVS on every boot
  WiFi.mode(WIFI_AP);       // AP only: no STA, so no reconnect loop and no radio
                            // time spent scanning for a router that is not there
  // Power save off. This is a stream: latency matters more than the few mA that
  // modem sleep would save, and sleep is a classic cause of "the stream freezes
  // every few seconds".
  WiFi.setSleep(false);

  IPAddress ip(192, 168, 4, 1);
  IPAddress mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, ip, mask);

  const bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, kApChannel, /*hidden=*/false,
                              kApMaxClients);

  Serial.printf("[wifi] SoftAP \"%s\": %s   auth %s, channel %d\n", AP_SSID,
                ok ? "up" : "FAILED",
                (AP_PASSWORD[0] != '\0') ? "WPA2" : "OPEN", kApChannel);
  // Read the AP MAC here, not in the banner: macAddress() returns a valid value
  // only once the radio is up, and printing six zero bytes is worse than useless
  // when you are trying to find the thing on a network list.
  Serial.printf("[wifi] ap mac %s\n", WiFi.softAPmacAddress().c_str());
  // The password itself is deliberately not printed. If it is worth knowing, it
  // is in secrets.h -- a serial log is the wrong place for it.
  Serial.printf("[wifi] open http://%s/   (gateway %s, mask %s)\n",
                WiFi.softAPIP().toString().c_str(), ip.toString().c_str(),
                mask.toString().c_str());
  Serial.println("[wifi] devices will warn \"no internet\" -- that is expected:");
  Serial.println("       this AP has no uplink, it exists to serve one page.");
}

void print_banner() {
  Serial.println();
  Serial.println("cam-test  --  ESP32-S3 camera + SoftAP + MJPEG stream");
  Serial.println("=====================================================");
  Serial.printf("chip      : %s rev %u, %u core(s) @ %u MHz\n", ESP.getChipModel(),
                (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(),
                (unsigned)getCpuFrequencyMhz());
  Serial.printf("flash     : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf("psram     : %u KB free of %u KB\n",
                (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));
  Serial.printf("free heap : %u B\n", (unsigned)ESP.getFreeHeap());
  Serial.println();
  Serial.println("camera is on the board's own FPC connector -- nothing to wire.");
  Serial.printf("LED/torch : GPIO%d (the on-board WS2812, shared with the strip)\n",
                (int)PIN_LED_STRIP);
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Bounded wait: a board that will not boot because no terminal is open would
  // be a silly failure, but the banner is much easier to read when it is not
  // racing the host.
  const uint32_t deadline = millis() + 2000;
  while (!Serial && (int32_t)(millis() - deadline) < 0) {
    delay(10);
  }

  print_banner();

  // Camera BEFORE the radio, on purpose. esp_camera_init() is the step most
  // likely to fail (a bad pin map, or a sensor that does not answer on SCCB),
  // and it is much easier to diagnose when it is the only thing that has
  // happened. See /memories/repo/esp32-s3-boot-failure.md for how expensive it
  // is to have two suspects at once.
  Serial.println("[cam] initialising...");
  const esp_err_t cam = camera_init();
  if (cam != ESP_OK) {
    Serial.printf("[cam] FAILED: %s (0x%04x)\n", esp_err_to_name(cam), (unsigned)cam);
    Serial.println("[cam] \"ESP_ERR_NOT_FOUND\" / 0x20001 = the sensor did not");
    Serial.println("      answer on SCCB. Check, in this order:");
    Serial.println("        1. the FPC ribbon is seated and the right way round;");
    Serial.println("        2. none of GPIO 4-18 is claimed by other firmware;");
    Serial.println("        3. the board actually has a sensor fitted.");
    Serial.println("[cam] NOT starting the radio: a half-up firmware that serves");
    Serial.println("      a 404 is harder to diagnose than one that says nothing.");
    return;
  }
  print_camera_report();
  apply_default_orientation();

  start_access_point();

  const int srv = app::server_start();
  if (srv != ESP_OK) {
    Serial.printf("[http] server FAILED to start: %s (0x%04x)\n",
                  esp_err_to_name((esp_err_t)srv), (unsigned)srv);
    return;
  }

  Serial.println();
  Serial.println("[http] endpoints:");
  Serial.println("         /          the page (one <img src=\"/stream\">)");
  Serial.println("         /stream    multipart MJPEG  <-- the video");
  Serial.println("         /capture   one JPEG snapshot");
  Serial.println("         /status    JSON: heap, psram, clients, fps, sensor");
  Serial.println("         /control   GET|POST sensor settings + LED colour");
  Serial.println();
  Serial.println("[http] try, from a device joined to the AP:");
  Serial.printf("         curl http://%s/status\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("         curl 'http://%s/control?framesize=svga&quality=10'\n",
                WiFi.softAPIP().toString().c_str());
  Serial.printf("         curl 'http://%s/control?led_r=32&led_g=0&led_b=0'\n",
                WiFi.softAPIP().toString().c_str());
  Serial.println();
  Serial.println("[led] status mode: dim blue = idle, green = someone streaming.");
  Serial.println("      Setting a colour turns status mode off until ?led_auto=1.");
  Serial.println();
}

void loop() {
  const uint32_t now = millis();

  app::led_task(now);

  static uint32_t last_report = 0;
  if (now - last_report >= 5000) {
    last_report = now;
    Serial.printf("[hb] up %lus  heap %u  psram %u  clients %u  fps %u\n",
                  (unsigned long)(now / 1000), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram(), (unsigned)app::stream_clients(),
                  (unsigned)app::fps());
  }

  // Nothing to do. The camera runs in the stream handler and the radio runs
  // itself; this loop only reports. 20 ms costs nothing and keeps the heartbeat
  // from spinning.
  delay(20);
}
