/*
 * network-test — Heltec WiFi LoRa 32 (V3)
 * ---------------------------------------------------------------------------
 * Shows N vertical bars of different brightness on the onboard OLED (any count
 * from 1 to kMaxBars, decided at runtime) and lets them be driven over WiFi
 * through a small JSON REST API.
 *
 *   Target : Heltec WiFi LoRa 32 (V3)  (ESP32-S3, 8 MB flash, SSD1306 OLED)
 *   Stack  : PlatformIO + Arduino framework + U8g2 + ArduinoJson
 *
 * HTTP API (port 80)
 * ------------------
 * The board does not join a network: it CREATES one (SoftAP, see
 * include/secrets.h). Join the SSID it shows on the OLED and browse to the
 * address on the second line - an ESP32 SoftAP is always 192.168.4.1.
 *
 *   GET  /        plain-text help
 *   GET  /bars    -> {"levels":[0.2,0.4,0.6,0.8,1.0]}
 *   POST /bars    <- [0.2,0.4,0.6,0.8,1.0]     (PUT works too)
 *
 * The body may be a bare array or an object like {"levels":[...]}, holding 1 to
 * kMaxBars numbers - the array length decides how many bars are painted. Every
 * value is clamped to 0.0 .. 1.0; a malformed body is answered with 400 and a
 * JSON error.
 *
 * A note on "brightness"
 * ----------------------
 * The onboard panel is a monochrome SSD1306 (128x64, 1 bit per pixel): every
 * pixel is either off or on, there is no grey. renderDitherRect() fakes a grey
 * level by switching on a fixed fraction of the pixels in an 8x8 ordered
 * (Bayer) pattern. The matrix holds values 0..63, so 64 levels are addressable,
 * and a dithered area reads as a uniform shade because the pattern is far finer
 * than the eye resolves at this size.
 *
 * Five bars are painted across the panel, one per level in kBarLevels, so the
 * shades can be compared side by side. Contrast is left at maximum, so every
 * difference on screen comes from the dither pattern alone.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include "secrets.h"  /* SoftAP credentials - see secrets.example.h */

/* --- Heltec WiFi LoRa 32 (V3) onboard OLED wiring ------------------------- */
/* The board variant (variants/heltec_wifi_lora_32_V3/pins_arduino.h) already
   defines these; the fallbacks only keep the sketch buildable if it is ever
   compiled against a plain esp32s3 definition. */
#ifndef SDA_OLED
#define SDA_OLED 17
#endif
#ifndef SCL_OLED
#define SCL_OLED 18
#endif
#ifndef RST_OLED
#define RST_OLED 21
#endif
#ifndef Vext
#define Vext 36
#endif

static constexpr uint8_t kPinOledSda = SDA_OLED;
static constexpr uint8_t kPinOledScl = SCL_OLED;
static constexpr uint8_t kPinOledRst = RST_OLED;
static constexpr uint8_t kPinVext = Vext;  /* active LOW: powers the OLED rail */
static constexpr uint32_t kI2cClockHz = 400000;

/* --- bar demo tuning ------------------------------------------------------ */
/* One brightness level per bar: 0.0 = every pixel off, 1.0 = every pixel on.
   The count is NOT fixed: every POST sets it to the length of the array it
   got, so gBarCount is the live value and these are only the power-on
   defaults. */
static constexpr size_t kMaxBars = 32;   /* 128 px / 32 = 4 px per bar */
static size_t gBarCount = 5;
static float gBarLevels[kMaxBars] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

/* The numeric value of each bar is printed under it, on a strip this tall.
   The strip is dropped automatically when the bars get too narrow for the
   shortest label, and the bars then use the full height again. */
static constexpr uint8_t kValueStripPx = 12;
static const uint8_t *const kValueFont = u8g2_font_6x10_tf;

/* --- network tuning ------------------------------------------------------- */
static constexpr uint16_t kHttpPort = 80;
static constexpr uint8_t kApChannel = 1;      /* 1..13, 2.4 GHz only */
static constexpr uint8_t kApMaxClients = 4;
static constexpr const char *kMdnsHost = "network-test";

/* Reset is pulsed by hand (OLED power has to be up first), so pass U8X8_PIN_NONE. */
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(
    U8G2_R0, U8X8_PIN_NONE, kPinOledScl, kPinOledSda);

/* -------------------------------------------------------------------------- */
/* Ordered dithering                                                          */
/* -------------------------------------------------------------------------- */

/* Standard recursive Bayer 8x8 threshold matrix, values 0..63. */
static const uint8_t kBayer8x8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21},
};

/**
 * Paints `level` (0..1) brightness into the rectangle [x0,x1) x [y0,y1) using
 * 8x8 ordered dithering: 0.0 leaves the area black, 1.0 fills it solid white.
 */
static void renderDitherRect(uint8_t x0, uint8_t x1, uint8_t y0, uint8_t y1,
                             float level) {
  for (uint8_t y = y0; y < y1; ++y) {
    for (uint8_t x = x0; x < x1; ++x) {
      const float threshold = (kBayer8x8[y & 7u][x & 7u] + 0.5f) / 64.0f;
      if (level > threshold) {
        display.drawPixel(x, y);
      }
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * Repaints the bars from gBarLevels and pushes them to the panel.
 *
 * The bars always divide the panel width equally, so a longer array simply
 * means narrower bars - 32 levels is the point where they reach 4 px. While
 * they are wide enough, each bar prints its own value underneath it (two
 * decimals when they fit, one when they do not); otherwise the strip is
 * dropped and the bars go back to the full height.
 */
static void paintBars() {
  const uint8_t width = display.getDisplayWidth();
  const uint8_t height = display.getDisplayHeight();

  /* The narrowest bar decides whether the numbers fit. Bar widths differ by at
     most one pixel, and integer division gives the floor of that range. */
  const uint8_t minBarWidth = static_cast<uint8_t>(width / gBarCount);
  display.setFont(kValueFont);
  const bool withValues = display.getStrWidth("0.0") <= minBarWidth;
  const uint8_t barBottom = withValues ? height - kValueStripPx : height;

  Serial.printf("[bars] %u bars, min %u px wide, values %s\n",
                static_cast<unsigned>(gBarCount),
                static_cast<unsigned>(minBarWidth), withValues ? "on" : "off");

  display.clearBuffer();

  /* --- the bars themselves ------------------------------------------------ */
  for (size_t i = 0; i < gBarCount; ++i) {
    /* Equal-width bars; integer maths keeps them exactly adjacent. */
    const uint8_t x0 = static_cast<uint8_t>(i * width / gBarCount);
    const uint8_t x1 = static_cast<uint8_t>((i + 1) * width / gBarCount);
    renderDitherRect(x0, x1, 0, barBottom, gBarLevels[i]);
  }

  /* --- the value of each bar --------------------------------------------- */
  if (withValues) {
    char label[8];

    for (size_t i = 0; i < gBarCount; ++i) {
      const uint8_t x0 = static_cast<uint8_t>(i * width / gBarCount);
      const uint8_t x1 = static_cast<uint8_t>((i + 1) * width / gBarCount);
      const uint8_t barWidth = static_cast<uint8_t>(x1 - x0);

      snprintf(label, sizeof(label), "%.2f", gBarLevels[i]);
      if (display.getStrWidth(label) > barWidth) {
        snprintf(label, sizeof(label), "%.1f", gBarLevels[i]);
      }

      /* White text on the strip the bars left black, so it is always
         readable - a dark bar would swallow black text. */
      const int x = x0 + (barWidth - display.getStrWidth(label)) / 2;
      display.drawStr(x, height - 2, label);
    }
  }

  display.sendBuffer();
}

/** Two centred lines of text; used to show the IP address right after boot. */
static void showStatusScreen(const char *line1, const String &line2) {
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);

  const char *lines[2] = {line1, line2.c_str()};
  for (uint8_t i = 0; i < 2; ++i) {
    const int x = (display.getDisplayWidth() - display.getStrWidth(lines[i])) / 2;
    display.drawStr(x, 28 + i * 14, lines[i]);
  }
  display.sendBuffer();
}

/* -------------------------------------------------------------------------- */
/* Hardware bring-up                                                          */
/* -------------------------------------------------------------------------- */

static void oledPowerOn() {
  /* Vext is active LOW on this board: pull it down to power the OLED rail. */
  pinMode(kPinVext, OUTPUT);
  digitalWrite(kPinVext, LOW);
  delay(50);

  /* Manual reset pulse required before the first I2C transaction. */
  pinMode(kPinOledRst, OUTPUT);
  digitalWrite(kPinOledRst, HIGH);
  delay(20);
  digitalWrite(kPinOledRst, LOW);
  delay(20);
  digitalWrite(kPinOledRst, HIGH);
  delay(50);
}

static void scanI2cBus() {
  Serial.println(F("[i2c] scanning 0x03..0x77 ..."));
  uint8_t found = 0;
  for (uint8_t addr = 0x03; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[i2c] device at 0x%02X\n", addr);
      ++found;
    }
  }
  if (found == 0) {
    Serial.println(F("[i2c] nothing found (OLED should answer at 0x3C)"));
  }
}

/* -------------------------------------------------------------------------- */
/* WiFi + HTTP API                                                            */
/* -------------------------------------------------------------------------- */

static WebServer server(kHttpPort);

/**
 * Brings up this board as its own access point, using the credentials from
 * include/secrets.h. An ESP32 SoftAP always owns 192.168.4.1 and runs its own
 * DHCP server, so there is nothing to scan for and nothing to configure at
 * boot - just join the SSID from the OLED.
 */
static void startAccessPoint() {
  WiFi.mode(WIFI_AP);

  /* hidden = false, and up to kApMaxClients stations at once. */
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, kApChannel, false, kApMaxClients)) {
    Serial.println(F("[ap] softAP() failed - no network, no HTTP"));
    return;
  }

  Serial.printf("[ap] network \"%s\" up on channel %u (%s)\n", AP_SSID,
                static_cast<unsigned>(WiFi.channel()),
                AP_PASSWORD[0] == '\0' ? "open" : "WPA2");
  Serial.printf("[ap] join it, then browse to http://%s/\n",
                WiFi.softAPIP().toString().c_str());
}

/** The current bar levels as {"levels":[...]}. */
static String barsJson() {
  JsonDocument doc;
  JsonArray levels = doc["levels"].to<JsonArray>();
  for (size_t i = 0; i < gBarCount; ++i) {
    levels.add(gBarLevels[i]);
  }

  String body;
  serializeJson(doc, body);
  return body;
}

/** GET /bars -> the levels currently on screen. */
static void handleGetBars() {
  server.send(200, "application/json", barsJson());
}

/**
 * Reads the request body however the client happened to encode it.
 *
 * Any content type other than the two form types is stored verbatim by the
 * ESP32 WebServer under the arg name "plain". But curl's -d defaults to
 * application/x-www-form-urlencoded, and then the payload is parsed as form
 * fields: a JSON array arrives as a single argument *name* with an empty value,
 * and levels=... arrives as a normal form field. Recover all three shapes.
 */
static String requestBody() {
  if (server.hasArg("plain")) {
    return server.arg("plain");
  }

  if (server.args() == 1) {
    const String name = server.argName(0);
    if (name.startsWith("[") || name.startsWith("{")) {
      return name;
    }
  }

  if (server.hasArg("levels")) {
    const String value = server.arg("levels");
    /* levels=[0.2,0.4] and levels=0.2,0.4 both become a JSON array. */
    return value.startsWith("[") ? "{\"levels\":" + value + "}"
                                 : "{\"levels\":[" + value + "]}";
  }

  return String();
}

/**
 * POST /bars (PUT is accepted as well).
 * Body: [0.2,0.4,0.6,0.8,1.0] or {"levels":[0.2,0.4,0.6,0.8,1.0]}
 * Any number of values from 1 to kMaxBars is accepted and the bar count becomes
 * length of the array; each value is clamped to 0.0 .. 1.0 and the panel is
 * repainted before the response is sent.
 */
static void handleSetBars() {
  const String body = requestBody();
  Serial.printf("[api] %s /bars  args=%d  ct='%s'  body='%s'\n",
                server.method() == HTTP_PUT ? "PUT" : "POST", server.args(),
                server.header("Content-Type").c_str(), body.c_str());

  if (body.isEmpty()) {
    server.send(400, "application/json",
                "{\"error\":\"no json body\",\"hint\":\"send Content-Type: "
                "application/json (curl -H), or a levels=... form field\"}");
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  /* Accept a bare array as well as an object wrapping one under "levels". */
  JsonArray levels =
      doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["levels"].as<JsonArray>();
  if (levels.isNull()) {
    server.send(400, "application/json",
                "{\"error\":\"body must be an array of numbers, or "
                "{\\\"levels\\\":[...]}\"}");
    return;
  }
  if (levels.size() < 1 || levels.size() > kMaxBars) {
    const String error = "{\"error\":\"expected 1 to " + String(kMaxBars) +
                         " numbers, got " + String(levels.size()) + "\"}";
    server.send(400, "application/json", error);
    return;
  }

  size_t count = 0;
  for (JsonVariant entry : levels) {
    if (!entry.is<float>()) {
      const String error =
          "{\"error\":\"entry " + String(count) + " is not a number\"}";
      server.send(400, "application/json", error);
      return;
    }

    const float value = entry.as<float>();
    gBarLevels[count] = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    ++count;
  }
  gBarCount = count;  /* the array length is what decides the bar count */

  paintBars();
  server.send(200, "application/json", barsJson());
}

/** Registers the routes and starts listening. */
static void startHttpApi() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain",
                "network-test bar API\n"
                "  GET  /bars            -> current levels\n"
                "  POST /bars            body: [0.2,0.4,0.6,0.8,1.0]\n"
                "                        Content-Type: application/json\n"
                "  PUT  /bars            body: {\"levels\":[...]}\n"
                "                        1 to 32 numbers; the count sets how "
                "many bars\n"
                "                        (form field levels=0.2,0.4,... also "
                "works)\n");
  });
  server.on("/bars", HTTP_GET, handleGetBars);
  server.on("/bars", HTTP_POST, handleSetBars);
  server.on("/bars", HTTP_PUT, handleSetBars);
  server.onNotFound([]() {
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  });

  server.enableCORS(true);
  server.begin();
  Serial.printf("[http] listening on port %u\n",
                static_cast<unsigned>(kHttpPort));

  if (MDNS.begin(kMdnsHost)) {
    Serial.printf("[mdns] http://%s.local/\n", kMdnsHost);
  } else {
    Serial.println(F("[mdns] responder failed to start"));
  }
}

/* -------------------------------------------------------------------------- */
/* Sketch                                                                     */
/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(300); /* give the USB host a moment before the first line */

  Serial.println();
  Serial.println(F("network-test :: Heltec WiFi LoRa 32 (V3) :: bars + REST API"));

  Serial.println(F("[init] OLED power on"));
  oledPowerOn();

  /* Let U8g2 own the I2C bus: it calls Wire.begin() itself, using the
     clock/data pins handed to the constructor. Initialising the bus here as
     well (i.e. Wire.begin() twice on the same peripheral) hangs the ESP32
     core, so the bus is left alone and only probed afterwards for diagnostics. */
  display.setBusClock(kI2cClockHz);
  Serial.println(F("[init] display.begin()"));
  display.begin();
  Serial.println(F("[init] display.begin() returned"));

  /* Maximum contrast: every difference on screen must come from the dither
     pattern, not from the panel's analogue contrast register. */
  display.setContrast(255);
  scanI2cBus();

  Serial.printf("[oled] SSD1306 %ux%u ready\n", display.getDisplayWidth(),
                display.getDisplayHeight());

  /* --- network ----------------------------------------------------------- */
  startAccessPoint();
  startHttpApi();

  /* Show the network to join and where to reach us, then paint the bars. */
  showStatusScreen(AP_SSID, WiFi.softAPIP().toString());
  delay(3000);

  paintBars();
  Serial.println(F("[bars] painted"));
}

void loop() {
  const uint32_t now = millis();

  /* Serve the REST API. handleClient() must be called as often as possible,
     which is why the bars are only ever repainted from inside a request. */
  server.handleClient();

  /* Heartbeat: the dither pattern is static, the link is what can change. */
  static uint32_t lastLogMs = 0;
  if (now - lastLogMs >= 10000u) {
    lastLogMs = now;
    Serial.printf("[net] uptime %lus  ssid=%s  ip=%s  clients=%u  bars=%u:",
                  now / 1000u, AP_SSID, WiFi.softAPIP().toString().c_str(),
                  static_cast<unsigned>(WiFi.softAPgetStationNum()),
                  static_cast<unsigned>(gBarCount));
    for (size_t i = 0; i < gBarCount; ++i) {
      Serial.printf(" %.2f", gBarLevels[i]);
    }
    Serial.println();
  }
}
