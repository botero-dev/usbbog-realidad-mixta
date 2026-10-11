// brain — ESP32-S3. Stage 1: play a clip out of the audio chain.
// ---------------------------------------------------------------------------
// This is the first S3 firmware in the repository: the project that is meant to
// become the "brain" of docs/circuit-plan.md section 1 (WiFi, camera, IMU, the
// control loop, audio) and to absorb the mixing code in firmware/car_control
// (plan section 10.3).
//
// What it does today is deliberately small, and it is bring-up step 2 of
// section 11: emit a tone from the I2S port so the chain
//
//     S3 --I2S--> PCM5102A --line level--> class-D amp --> speaker
//
// can be heard working on its own, with no motors, no camera and no link in the
// picture. A tone is a chain you can verify; "play a sound file" is five things
// at once that you cannot pin down when it fails.
//
// ---------------------------------------------------------------------------
// THE API GENERATION TRAP -- READ THIS BEFORE COPYING ANY EXAMPLE OFF THE WEB.
//
// This project builds against Arduino core **2.0.17** (see platformio.ini:
// espressif32 7.1.3). That core has the LEGACY I2S driver: driver/i2s.h,
// i2s_config_t, i2s_driver_install(), i2s_write(). The `ESP_I2S` class with
// `i2s.begin()` / `i2s.write()` that almost every current tutorial shows does
// NOT exist here -- it arrived in core 3.x. The two are not interchangeable and
// the compiler's complaint is opaque. If you upgrade the platform later, this
// file is what has to change.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

#include "audio_clip.h"

// --- Pins -------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 6, and hardware/wiring.md. Note the
// signal NAME is from the DAC's point of view: `DIN` is the DAC's *input*, so it
// is the S3's data OUT.
//
// Signals PERMUTED on 2026-10-11 at the user's request: LRCK / BCK / DIN across the
// same three pins (45 / 47 / 21). The pin set is unchanged, so in header order the
// pins now map to DAC pads 4 / 2 / 3 -- which means the ribbon has to CROSS. It only
// runs straight when the middle pin (47) carries DIN, i.e. the middle pad (plan 2.1).
// 48 is the LED strip and the on-board WS2812. 45 is the VDD_SPI strapping pin and
// carries a clock either way, which is safe because the DAC's input is high-Z and the
// strap is sampled only at reset.
static const gpio_num_t PIN_I2S_BCK  = GPIO_NUM_47;  // → DAC pad 2, `BCK`
static const gpio_num_t PIN_I2S_DOUT = GPIO_NUM_21;  // → DAC pad 3, `DIN`
static const gpio_num_t PIN_I2S_LRCK = GPIO_NUM_45;  // → DAC pad 4, `LCK`

// --- I2S format -------------------------------------------------------------
static const i2s_port_t I2S_PORT    = I2S_NUM_0;
static const uint32_t   SAMPLE_RATE = 44100;
static const size_t     BUF_FRAMES  = 256;  // stereo frames per write = 1 KB

// 1024-entry sine table, built once at startup.
//
// A table rather than sinf() per sample: at 44.1 kHz a call per sample would
// still fit (the S3 has an FPU, unlike the classic ESP32), but the audio loop is
// the one place where "it probably keeps up" is not good enough, and indexing a
// table is trivially correct.
static const size_t TABLE_LEN = 1024;
static int16_t sine_table[TABLE_LEN];

// Amplitude, as a fraction of full scale (1.0 = 32767).
//
// This IS the volume control, and it is the only one there is: the PCM5102A is a
// fixed line-level output and the class-D amp has a fixed gain, so making the
// samples smaller is the entire mechanism in software.
//
// 12000 counts (0.366) was the first revision and was far too loud into a small
// speaker. This is a tenth of that.
//
// What it costs: every halving of amplitude throws away one bit of resolution, so
// 0.0366 uses roughly 11 of the 16 bits. Inaudible on a beep. For music you would
// attenuate in the ANALOGUE domain -- a pot, or the amp's gain pins -- and keep the
// digital signal at full scale, which is the general rule for any audio design.
static const float VOLUME = 0.0366f;   // ≈ 1200 counts, about -29 dBFS

static const float FULL_SCALE = 32767.0f;

static void build_sine_table() {
  const float pi = 3.14159265358979f;
  for (size_t i = 0; i < TABLE_LEN; ++i) {
    const float radians = 2.0f * pi * (float)i / (float)TABLE_LEN;
    sine_table[i] = (int16_t)(sinf(radians) * FULL_SCALE * VOLUME);
  }
}

static void i2s_init() {
  // Field order matches the declaration of i2s_driver_config_t in
  // hal/i2s_types.h; anything omitted is zero-initialised.
  const i2s_config_t config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      // Stereo, even though the build is mono: the DAC decodes both slots and we
      // only take LOUT. Feeding both slots the same sample keeps the frame
      // layout a boring, standard I2S one.
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      // I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB is the old spelling and is
      // marked deprecated in this SDK (it compiles with a warning). Use this one.
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = BUF_FRAMES,
      .use_apll = false,
      // If the DSP ever underruns, send silence rather than replaying whatever
      // stale samples are still in the DMA descriptors. Turns a glitch into a
      // gap, which is a much easier thing to recognise by ear.
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0,
  };

  // No MCLK. The PCM5102A board's `SCK` pad is bridged to GND, so the DAC derives
  // its system clock from BCK -- documented in plan section 6, and it means the
  // chain is genuinely three wires.
  const i2s_pin_config_t pins = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = PIN_I2S_BCK,
      .ws_io_num = PIN_I2S_LRCK,
      .data_out_num = PIN_I2S_DOUT,
      .data_in_num = I2S_PIN_NO_CHANGE,
  };

  ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &config, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pins));
  // Start from a known-empty DMA chain, otherwise the first thing you hear can be
  // whatever was in those buffers.
  ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}

// Play `freq_hz` for `ms`. A frequency of 0 means silence, which is worth having:
// a gap is how you tell "the code stopped writing samples" from "the DAC is
// hissing".
static void play(uint16_t freq_hz, uint32_t ms) {
  // Phase accumulator: the top 10 bits of a 32-bit counter index the table, so
  // the step size for a frequency is (freq / sample_rate) * 2^32.
  uint32_t phase = 0;
  const uint32_t step =
      (uint32_t)(((uint64_t)freq_hz << 32) / (uint64_t)SAMPLE_RATE);

  int16_t buf[BUF_FRAMES * 2];  // interleaved L, R
  const uint32_t start = millis();

  while ((int32_t)(millis() - start) < (int32_t)ms) {  // wrap-safe comparison
    for (size_t i = 0; i < BUF_FRAMES; ++i) {
      const int16_t sample =
          (freq_hz == 0) ? 0 : sine_table[phase >> 22];
      phase += step;
      buf[2 * i] = sample;      // left  → DAC LOUT (the pad that is wired)
      buf[2 * i + 1] = sample;  // right → DAC ROUT (unused)
    }
    size_t written = 0;
    // portMAX_DELAY: this is the only writer, so blocking until the DMA chain has
    // room is exactly the right behaviour and keeps the phase continuous.
    i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
  }
}

// Play the embedded clip once.
//
// The clip is MONO 16-bit at the same 44.1 kHz the I2S port is already configured
// for, so the only per-frame work is duplicating the sample into both slots -- the
// frame layout stays the standard one the DAC has locked onto, and nothing is
// resampled at runtime. `tools/pcm_to_c.py` explains how to make that true again if
// the audio is ever replaced; a rate or channel mismatch is SILENT, it just plays at
// the wrong pitch.
//
// The samples are full-scale-ish (they came from a 24-bit recording), so the same
// VOLUME that tamed the test tone is applied here -- otherwise this is far louder
// than the beeps ever were.
static void play_clip() {
  int16_t buf[BUF_FRAMES * 2];  // interleaved L, R
  size_t pos = 0;

  while (pos < CLIP_SAMPLES) {
    const size_t room = CLIP_SAMPLES - pos;
    const size_t n = (room < BUF_FRAMES) ? room : BUF_FRAMES;

    for (size_t f = 0; f < n; ++f) {
      const int16_t s = (int16_t)(clip_pcm[pos + f] * VOLUME);
      buf[2 * f] = s;      // left  -> DAC LOUT (the pad that is wired)
      buf[2 * f + 1] = s;  // right -> DAC ROUT (unused in this mono build)
    }

    size_t written = 0;
    i2s_write(I2S_PORT, buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    pos += n;
  }
}

void setup() {
  Serial.begin(115200);
  // USB CDC drops output until a host is listening. Wait briefly, but never block
  // forever: a board that refuses to make a sound because no terminal is open
  // would be a stupid failure mode.
  const uint32_t wait_until = millis() + 2000;
  while (!Serial && (int32_t)(millis() - wait_until) < 0) {
    delay(10);
  }

  Serial.println();
  Serial.println("brain  --  I2S clip player (bring-up step 2)");
  Serial.println("============================================");
  Serial.printf("sample rate : %lu Hz, 16-bit stereo\n", (unsigned long)SAMPLE_RATE);
  Serial.printf("BCK         : GPIO%d  -> DAC pad 2 (BCK)\n", (int)PIN_I2S_BCK);
  Serial.printf("DIN (data)  : GPIO%d  -> DAC pad 3 (DIN)\n", (int)PIN_I2S_DOUT);
  Serial.printf("LRCK        : GPIO%d  -> DAC pad 4 (LCK)\n", (int)PIN_I2S_LRCK);
  Serial.printf("expected BCK: %lu Hz\n", (unsigned long)(SAMPLE_RATE * 32));
  const unsigned clip_s = (unsigned)(CLIP_SAMPLES / SAMPLE_RATE);
  const unsigned clip_ms =
      (unsigned)(((uint64_t)(CLIP_SAMPLES % SAMPLE_RATE) * 1000ULL) / SAMPLE_RATE);
  Serial.printf("clip        : %u samples, %u.%03u s, mono duplicated to both slots\n",
                (unsigned)CLIP_SAMPLES, clip_s, clip_ms);
  Serial.println();

  build_sine_table();
  i2s_init();

  Serial.println("I2S up. The embedded clip plays on repeat, with a 1.5 s gap.");
  Serial.println("Nothing audible? Check, in this order:");
  Serial.println("  1. the `XSMT` bridge on the DAC -- floated = muted, and it looks");
  Serial.println("     exactly like a firmware bug. Meter it.");
  Serial.println("  2. DAC `VIN` on 5 V and `GND` on ground (pads 6 and 5).");
  Serial.println("  3. `LOUT` (pad 15) into the amp's IN+, amp powered from 5 V.");
  Serial.println();
}

void loop() {
  play_clip();
  play(0, 1500);  // audible gap, so a repeat reads as a repeat
}
