/*
 * network-test — Heltec WiFi LoRa 32 (V3)
 * ---------------------------------------------------------------------------
 * Shows five vertical bars of different brightness on the onboard OLED.
 *
 *   Target : Heltec WiFi LoRa 32 (V3)  (ESP32-S3, 8 MB flash, SSD1306 OLED)
 *   Stack  : PlatformIO + Arduino framework + U8g2
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
#include <U8g2lib.h>
#include <Wire.h>

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
/* One brightness level per bar: 0.0 = every pixel off, 1.0 = every pixel on. */
static constexpr uint8_t kBarCount = 5;
static const float kBarLevels[kBarCount] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};

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
/* Sketch                                                                     */
/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  delay(300); /* give the USB host a moment before the first line */

  Serial.println();
  Serial.println(F("network-test :: Heltec WiFi LoRa 32 (V3) :: five brightness bars"));

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

  const uint8_t width = display.getDisplayWidth();
  const uint8_t height = display.getDisplayHeight();
  Serial.printf("[oled] SSD1306 %ux%u ready\n", width, height);

  /* --- paint the brightness bars ----------------------------------------- */
  display.clearBuffer();
  for (uint8_t i = 0; i < kBarCount; ++i) {
    /* Equal-width bars; integer maths keeps them exactly adjacent. */
    const uint8_t x0 = static_cast<uint16_t>(i) * width / kBarCount;
    const uint8_t x1 = static_cast<uint16_t>(i + 1) * width / kBarCount;
    renderDitherRect(x0, x1, 0, height, kBarLevels[i]);
    Serial.printf("[bars] bar %u  x=%3u..%3u  level=%.2f\n", i, x0, x1,
                  kBarLevels[i]);
  }
  display.sendBuffer();

  Serial.println(F("[bars] painted"));
}

void loop() {
  /* The bars are static and their dither pattern never changes, so there is
     nothing to repaint - the panel keeps showing the last buffer. This loop
     exists only to show that the firmware is still alive. */
  static uint32_t lastLogMs = 0;

  const uint32_t now = millis();
  if (now - lastLogMs >= 5000u) {
    lastLogMs = now;
    Serial.printf("[bars] alive, uptime %lus  levels:", now / 1000u);
    for (uint8_t i = 0; i < kBarCount; ++i) {
      Serial.printf(" %.2f", kBarLevels[i]);
    }
    Serial.println();
  }
}
