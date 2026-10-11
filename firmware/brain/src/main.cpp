// main.cpp — brain: the ESP32-S3 side of the car, camera + audio + AP in one firmware.
// ---------------------------------------------------------------------------
// This is `firmware/cam-test` and `firmware/brain` MERGED (2026-10-11). The two
// were kept apart on purpose while each was being brought up, so that a failure
// had one candidate. Now that both work on their own, they share a binary for a
// reason that neither could test alone:
//
//   DO THE CAMERA AND THE AUDIO CHAIN COEXIST?
//
// Plan section 6 predicts they might not: the camera's DMA reaches into PSRAM and
// so does the I2S DMA, both on the octal PSRAM bus. Merged, that prediction stops
// being a note in a document and becomes something you can hear. The mitigations,
// in the order section 6 gives them:
//   1. keep I2S buffers in INTERNAL RAM -- the default, and what audio_player.cpp
//      relies on;
//   2. drop the camera's `framesize` BEFORE touching anything in the audio driver.
// So if the sound crackles only while the camera streams, try
//   curl 'http://192.168.4.1/control?framesize=qvga'
// before you go looking for a bug in the audio path.
//
// What it is NOT yet: no BMI160, no control loop, no link to the Pico, no motors.
// Bring-up step 3 of section 11 is the IMU, and the core split for the control
// task comes with it.
//
// AUDIO IS SILENT UNTIL ASKED. There is no autoplay: the two buttons on the page
// are the only way to make a sound (see audio_player.h for why).
//
// Wiring: NONE for the camera (it is on the board's own FPC connector). Six wires
// to the DAC, listed in README.md and plan section 6.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <WiFi.h>

#include "app_httpd.h"
#include "audio_player.h"
#include "camera_pins.h"
#include "esp_camera.h"
#include "imu.h"

// Credentials: `secrets.h` if it exists, otherwise the throwaway defaults in
// secrets.example.h. Copy the example to set a real SSID/password; this project
// never needs one to run.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

// --- DIAGNOSTIC BUILD FLAGS ------------------------------------------------
// All default to 0, so the normal build is untouched. They exist to bisect the
// distorted audio ONE VARIABLE AT A TIME, without editing code:
//
//   -DBRAIN_BOOT_BEEP=1   play the beeps ~2 s after boot, with no page needed.
//                         Deliberately NOT a feature -- see audio_player.h on why
//                         nothing autoplays. It exists so the tone can be judged
//                         with no radio in the picture, i.e. with no way to reach
//                         /play.
//   -DBRAIN_NO_CAMERA=1   skip camera init (no PSRAM frame buffers, no camera DMA)
//   -DBRAIN_NO_RADIO=1    skip SoftAP and the web server (radio never transmits)
//
// The `diag` env in platformio.ini turns all three on at once. To bisect, drop
// them one at a time -- see the comment there for the order.
#ifndef BRAIN_BOOT_BEEP
#define BRAIN_BOOT_BEEP 0
#endif
#ifndef BRAIN_NO_CAMERA
#define BRAIN_NO_CAMERA 0
#endif
#ifndef BRAIN_NO_RADIO
#define BRAIN_NO_RADIO 0
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
  WiFi.persistent(false);  // do not write Wi-Fi config to NVS on every boot
  WiFi.mode(WIFI_AP);      // AP only: no STA, so no reconnect loop and no radio
                           // time spent scanning for a router that is not there
  // Power save off. This serves a video stream: latency matters more than the few
  // mA that modem sleep would save, and sleep is a classic cause of "the stream
  // freezes every few seconds".
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

// Why did the LAST boot happen? This matters more than it looks. During physical
// debugging -- wiggling cables -- a brownout is exactly what you are hunting, and
// the ESP32 will reset and come back up looking perfectly healthy. Naming the
// reason turns "it glitched" into evidence, and it costs one line.
const char *reset_reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON (cold start / EN pin)";
    case ESP_RST_BROWNOUT:  return "BROWNOUT  <-- the 5 V rail sagged";
    case ESP_RST_SW:        return "software reset";
    case ESP_RST_PANIC:     return "PANIC (crash)";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_EXT:       return "external pin";
    default:                return "unknown";
  }
}

void print_banner() {
  Serial.println();
  Serial.println("brain  --  ESP32-S3 camera + audio + AP (cam-test merged in)");
  Serial.println("=================================================================");
  Serial.printf("chip      : %s rev %u, %u core(s) @ %u MHz\n", ESP.getChipModel(),
                (unsigned)ESP.getChipRevision(), (unsigned)ESP.getChipCores(),
                (unsigned)getCpuFrequencyMhz());
  Serial.printf("reset     : %s\n", reset_reason_name(esp_reset_reason()));
  Serial.printf("flash     : %u MB\n",
                (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf("psram     : %u KB free of %u KB\n",
                (unsigned)(ESP.getFreePsram() / 1024),
                (unsigned)(ESP.getPsramSize() / 1024));
  Serial.printf("free heap : %u B\n", (unsigned)ESP.getFreeHeap());
  Serial.println();
  Serial.println("camera is on the board's own FPC connector -- nothing to wire.");
  Serial.printf("LED/torch : GPIO%d (the on-board WS2812, shared with the strip)\n",
                (int)PIN_LED_STRIP);
  Serial.printf("audio     : I2S %u Hz, SILENT until asked (sound + level buttons)\n",
                (unsigned)audio_sample_rate());
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

  // Camera BEFORE the radio and before I2S, on purpose. esp_camera_init() is the
  // step most likely to fail (a bad pin map, or a sensor that does not answer on
  // SCCB), and it is much easier to diagnose when it is the only thing that has
  // happened. See /memories/repo/esp32-s3-boot-failure.md for how expensive it is
  // to have two suspects at once.
#if BRAIN_NO_CAMERA
  Serial.println("[diag] BRAIN_NO_CAMERA=1: camera init skipped entirely.");
#else
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
#endif  // BRAIN_NO_CAMERA

  // The IMU next, and its failure is NOT fatal. A missing or miswired sensor must
  // not take the camera and the audio down with it -- imu_init() prints what it
  // found on the bus and what to check, and returns false.
  //
  // Order matters here in one specific way: the camera's SCCB bus is installed
  // first, and the IMU deliberately uses the OTHER I2C controller (see imu.cpp),
  // so the two cannot fight over one.
  imu_init();

  // Audio comes up next, and stays silent. Order matters for diagnosis: if the
  // I2S driver failed here it would abort via ESP_ERROR_CHECK with a message,
  // before the radio is up to confuse things.
  audio_init();
  Serial.println("[audio] I2S up. Nothing plays until a button is pressed.");

#if BRAIN_BOOT_BEEP
  // The beep itself is driven from loop() (see there): a single 1 s window right
  // after a reset turned out to be far too easy to miss, and "I didn't catch it"
  // costs a whole flash cycle. It now repeats.
  Serial.println("[diag] BRAIN_BOOT_BEEP=1: beeping every 10 s from now.");
#endif

#if BRAIN_NO_RADIO
  Serial.println("[diag] BRAIN_NO_RADIO=1: no SoftAP, no web server.");
#else
  start_access_point();

  const int srv = app::server_start();
  if (srv != ESP_OK) {
    Serial.printf("[http] server FAILED to start: %s (0x%04x)\n",
                  esp_err_to_name((esp_err_t)srv), (unsigned)srv);
    return;
  }
#endif  // BRAIN_NO_RADIO

#if !BRAIN_NO_RADIO
  Serial.println();
  Serial.println("[http] endpoints -- TWO servers, on two ports:");
  Serial.println("  :80     /          the page: stream, sound buttons, level buttons");
  Serial.println("  :80     /capture   one JPEG snapshot");
  Serial.println("  :80     /status    JSON: heap, psram, clients, fps, audio, volume");
  Serial.println("  :80     /imu       JSON: accel, gyro, tilt, temp, seq (page polls 15 Hz)");
  Serial.println("  :80     /control   GET|POST sensor settings, LED colour, ?vol=0..1000");
  Serial.println("  :80     /play      GET|POST ?src=beeps|clip[&vol=0..1000]  (non-blocking)");
  Serial.println("  :81     /stream    multipart MJPEG  <-- the video");
  Serial.println();
  Serial.println("  The stream is on its own port because its handler never returns while");
  Serial.println("  a client is watching, and one httpd task serves one request at a time.");
  Serial.println("  Sharing a port would freeze /play, /status and /control whenever the");
  Serial.println("  video is being watched.");
  Serial.println();
  Serial.println("[http] try, from a device joined to the AP:");
  Serial.printf("         curl http://%s/status\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("         curl 'http://%s/play?src=beeps&vol=10'\n",
                WiFi.softAPIP().toString().c_str());
  Serial.printf("         curl 'http://%s/control?framesize=qvga'\n",
                WiFi.softAPIP().toString().c_str());
  Serial.println();
  Serial.println("[led] dim blue = idle, green = streaming, white = playing audio.");
  Serial.println("      Setting a colour turns status mode off until ?led_auto=1.");
  Serial.println();
#endif  // !BRAIN_NO_RADIO
}

void loop() {
  const uint32_t now = millis();

#if BRAIN_BOOT_BEEP
  // DIAGNOSTIC ONLY -- compiled out of the real build.
  //
  // This is the only place in the firmware that makes a sound without being asked,
  // and it exists purely so the audio can be judged with no radio in the picture,
  // i.e. when /play cannot be reached. It repeats every 10 s because one shot right
  // after a reset is easy to miss.
  //
  // Watch the LED as well as listening: led_task() turns it white while audio is
  // playing, so a white flash every 10 s means the audio path ran even if you
  // heard nothing -- which separates "no sound is being generated" from
  // "sound is generated but not audible".
  static uint32_t next_beep = 2000;  // ms since boot; first one 2 s in
  if (now >= next_beep) {
    next_beep = now + 10000;
    audio_request_beeps();
    Serial.println("[diag] beeps requested (repeats every 10 s)");
  }
#endif

  app::led_task(now);

  static uint32_t last_report = 0;
  if (now - last_report >= 5000) {
    last_report = now;
    // `ap` is the number of devices ASSOCIATED with the softAP, which is a
    // completely different thing from `clients` (stream viewers). When "nothing
    // loads", this line is the first thing to read: ap 0 means nothing is even on
    // the network, so the problem is Wi-Fi, not HTTP.
    Serial.printf("[hb] up %lus  heap %u  psram %u  ap %u  clients %u  fps %u  audio %s\n",
                  (unsigned long)(now / 1000), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getFreePsram(), (unsigned)WiFi.softAPgetStationNum(),
                  (unsigned)app::stream_clients(), (unsigned)app::fps(),
                  audio_now_playing());

    // The IMU gets its own line so the numbers are not buried in the heartbeat.
    // `seq` climbing steadily is the health check: values that sit still look
    // identical whether the board is motionless or the sampler has died.
    if (imu_present()) {
      ImuSample imu;
      imu_latest(&imu);
      Serial.printf("[imu] a %.3f %.3f %.3f g | w %.1f %.1f %.1f dps | tilt "
                    "%.1f/%.1f deg | %.1f C | seq %u err %u\n",
                    imu.ax, imu.ay, imu.az, imu.gx, imu.gy, imu.gz, imu.pitch,
                    imu.roll, imu.temp_c, (unsigned)imu.seq, (unsigned)imu.errors);
    }
  }

  // Nothing to do here, and that is the design: the camera runs in the stream
  // handler, the radio runs itself, and the audio runs in its own task. This loop
  // only reports. Core 1 stays free for the control task that plan section 10.1
  // pins here at priority 5, so keep this cheap.
  delay(20);
}
