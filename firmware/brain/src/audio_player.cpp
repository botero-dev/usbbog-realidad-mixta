// audio_player.cpp — see audio_player.h.
// ---------------------------------------------------------------------------
// The I2S configuration below is the one VERIFIED ON HARDWARE (three beeps heard
// out of the PCM5102A + amp, 2026-10-11): 44.1 kHz, 16-bit, stereo, on the
// permuted pins 47 BCK / 21 DIN / 45 LRCK. Do not change these numbers without a
// reason you can hear.
//
// ---------------------------------------------------------------------------
// THE API GENERATION TRAP -- READ THIS BEFORE COPYING ANY EXAMPLE OFF THE WEB.
//
// This project builds against Arduino core **2.0.17** (see platformio.ini:
// espressif32 7.1.3). That core has the LEGACY I2S driver: driver/i2s.h,
// i2s_config_t, i2s_driver_install(), i2s_write(). The `ESP_I2S` class with
// `i2s.begin()` / `i2s.write()` that almost every current tutorial shows does
// NOT exist here -- it arrived in core 3.x. The two are not interchangeable and
// the compiler's complaint is opaque. If the platform is ever upgraded, this
// file is what has to change.
// ---------------------------------------------------------------------------

#include "audio_player.h"

#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

#include "audio_clip.h"

namespace {

// --- Pins -------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 6, and hardware/wiring.md. Note the
// signal NAME is from the DAC's point of view: `DIN` is the DAC's *input*, so it
// is the S3's data OUT.
//
// Signals PERMUTED on 2026-10-11 at the user's request: LRCK / BCK / DIN across
// the same three pins (45 / 47 / 21). The pin set is unchanged, so in header
// order the pins now map to DAC pads 4 / 2 / 3 -- which means the ribbon has to
// CROSS. It only runs straight when the middle pin (47) carries DIN, i.e. the
// middle pad (plan 2.1). 48 is the LED strip and the on-board WS2812. 45 is the
// VDD_SPI strapping pin and carries a clock either way, which is safe because the
// DAC's input is high-Z and the strap is sampled only at reset.
constexpr gpio_num_t PIN_I2S_BCK = GPIO_NUM_47;   // -> DAC pad 2, `BCK`
constexpr gpio_num_t PIN_I2S_DOUT = GPIO_NUM_21;  // -> DAC pad 3, `DIN`
constexpr gpio_num_t PIN_I2S_LRCK = GPIO_NUM_45;  // -> DAC pad 4, `LCK`

// --- I2S format -------------------------------------------------------------
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr size_t BUF_FRAMES = 256;  // stereo frames per write = 1 KB

// --- request state ----------------------------------------------------------
// Written by the HTTP task, read by the audio task. Both run on core 0, but they
// are different tasks, so this is genuinely shared state.
//
// The request flag and the generation counter must be written TOGETHER and read
// TOGETHER, which is why there is a critical section. Without it there is a real
// race: if the audio task read `generation` and then the HTTP task bumped it and
// set `request` in between, the task would pick up the new sound and immediately
// decide it had been superseded by itself -- and silently skip it. That is an
// intermittent "I pressed it and nothing happened", which is a horrible thing to
// chase, so it is closed here rather than left to chance.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

volatile uint8_t s_request = 0;      // 0 none, 1 beeps, 2 clip
volatile uint32_t s_generation = 0;  // bumped per request; aborts playback
volatile uint8_t s_playing = 0;      // 0 idle, 1 beeps, 2 clip

void request(uint8_t which) {
  taskENTER_CRITICAL(&s_mux);
  s_request = which;
  s_generation++;  // immediately invalidates anything currently playing
  taskEXIT_CRITICAL(&s_mux);
}

// A generation value that can never be superseded. Used by the diagnostic sweep,
// which runs with no radio and therefore has no HTTP caller that could supersede it.
constexpr uint32_t kNoSupersede = 0xFFFFFFFFu;

bool superseded(uint32_t gen) {
  return gen != kNoSupersede && s_generation != gen;
}

// --- tone generation --------------------------------------------------------
// 1024-entry sine table, built once at startup.
//
// A table rather than sinf() per sample: at 44.1 kHz a call per sample would
// still fit (the S3 has an FPU, unlike the classic ESP32), but the audio loop is
// the one place where "it probably keeps up" is not good enough, and indexing a
// table is trivially correct.
constexpr size_t TABLE_LEN = 1024;
int16_t sine_table[TABLE_LEN];

// THE DEFAULT amplitude, as a fraction of full scale (1.0 = 32767).
//
// This IS the volume control, and it is the only one there is: the PCM5102A is a
// fixed line-level output and the class-D amp has a fixed gain, so making the
// samples smaller is the entire mechanism in software.
//
// 12000 counts (0.366) was the first revision and was far too loud into a small
// speaker. This is a tenth of that, and it is applied to the clip as well, whose
// samples came from a 24-bit recording and are near full scale.
//
// What it costs: every halving of amplitude throws away one bit of resolution, so
// 0.0366 uses roughly 11 of the 16 bits. Inaudible on a beep. For music you would
// attenuate in the ANALOGUE domain -- a pot, or the amp's gain pins -- and keep
// the digital signal at full scale, which is the general rule for any audio design.
constexpr float kDefaultVolume = 0.0366f;  // ~1200 counts, about -29 dBFS
constexpr float FULL_SCALE = 32767.0f;

// The LIVE amplitude. Mutable because the diagnostic volume sweep steps it (see
// the `sweep` env in platformio.ini); the production build never changes it after
// boot, so this is a constant in all but name.
float g_volume = kDefaultVolume;

// The sine table is pre-scaled, so changing the volume means rebuilding it. That
// costs 1024 sinf() calls -- nothing when it happens once per sweep step -- and it
// keeps the per-sample path free of a multiply, which was the original reason to
// use a table at all.
void build_sine_table() {
  const float pi = 3.14159265358979f;
  for (size_t i = 0; i < TABLE_LEN; ++i) {
    const float radians = 2.0f * pi * (float)i / (float)TABLE_LEN;
    sine_table[i] = (int16_t)(sinf(radians) * FULL_SCALE * g_volume);
  }
}

void set_volume(float fraction) {
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  g_volume = fraction;
  build_sine_table();
}

// The level the NEXT sound should use, and whether it has been applied yet.
//
// Deferred deliberately: applying a volume change rebuilds the 1024-entry sine
// table, and doing that while the audio task is reading it would put a click in the
// middle of a sound. So the HTTP side records the request and the audio task -- the
// only thing that ever plays -- applies it, just before it starts something new.
volatile float g_pending_volume = kDefaultVolume;
volatile bool g_volume_dirty = false;

void i2s_init() {
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

  // No MCLK. The PCM5102A board's `SCK` pad is bridged to GND, so the DAC
  // derives its system clock from BCK -- documented in plan section 6, and it
  // means the chain is genuinely three wires.
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
// hissing". Aborts early (returning) if a newer request has arrived.
void play_tone(uint16_t freq_hz, uint32_t ms, uint32_t gen) {
  // Phase accumulator: the top 10 bits of a 32-bit counter index the table, so
  // the step size for a frequency is (freq / sample_rate) * 2^32.
  uint32_t phase = 0;
  const uint32_t step =
      (uint32_t)(((uint64_t)freq_hz << 32) / (uint64_t)SAMPLE_RATE);

  int16_t buf[BUF_FRAMES * 2];  // interleaved L, R
  const uint32_t start = millis();

  while ((int32_t)(millis() - start) < (int32_t)ms) {  // wrap-safe comparison
    if (superseded(gen)) return;

    for (size_t i = 0; i < BUF_FRAMES; ++i) {
      const int16_t sample = (freq_hz == 0) ? 0 : sine_table[phase >> 22];
      phase += step;
      buf[2 * i] = sample;      // left  -> DAC LOUT (the pad that is wired)
      buf[2 * i + 1] = sample;  // right -> DAC ROUT (unused)
    }

    size_t written = 0;
    // portMAX_DELAY: this task is the only writer, so blocking until the DMA
    // chain has room is exactly the right behaviour and keeps the phase
    // continuous. It also sets the ~6 ms granularity at which a new request can
    // interrupt this one.
    i2s_write(I2S_PORT, buf, sizeof(buf), &written, portMAX_DELAY);
  }
}

// The three test tones, with an audible gap between them.
void play_beeps(uint32_t gen) {
  static const uint16_t notes[] = {440, 660, 880};
  for (const uint16_t hz : notes) {
    if (superseded(gen)) return;
    play_tone(hz, 220, gen);
    if (superseded(gen)) return;
    play_tone(0, 120, gen);
  }
}

// Play the embedded clip once.
//
// The clip is MONO 16-bit at the same 44.1 kHz the I2S port is already configured
// for, so the only per-frame work is duplicating the sample into both slots -- the
// frame layout stays the standard one the DAC has locked onto, and nothing is
// resampled at runtime. `tools/pcm_to_c.py` explains how to make that true again if
// the audio is ever replaced; a rate or channel mismatch is SILENT, it just plays
// at the wrong pitch.
void play_clip_file(uint32_t gen) {
  int16_t buf[BUF_FRAMES * 2];  // interleaved L, R
  size_t pos = 0;

  while (pos < CLIP_SAMPLES) {
    if (superseded(gen)) return;

    const size_t room = CLIP_SAMPLES - pos;
    const size_t n = (room < BUF_FRAMES) ? room : BUF_FRAMES;

    for (size_t f = 0; f < n; ++f) {
      const int16_t s = (int16_t)(clip_pcm[pos + f] * g_volume);
      buf[2 * f] = s;      // left  -> DAC LOUT (the pad that is wired)
      buf[2 * f + 1] = s;  // right -> DAC ROUT (unused in this mono build)
    }

    size_t written = 0;
    i2s_write(I2S_PORT, buf, n * 2 * sizeof(int16_t), &written, portMAX_DELAY);
    pos += n;
  }
}

#ifndef BRAIN_VOLUME_SWEEP
#define BRAIN_VOLUME_SWEEP 0
#endif

#if BRAIN_VOLUME_SWEEP
// --- diagnostic volume sweep (see the `sweep` env in platformio.ini) --------
//
// A CONTINUOUS tone at a series of amplitudes, forever. Nothing in this block is
// a feature; it exists to answer one question about a distorted output that is
// suspected to be a power or headroom problem:
//
//   DOES THE DISTORTION SCALE WITH LEVEL?
//
// A continuous tone is far easier to judge than a 220 ms burst -- you can hear
// whether the rubbish is steady or pumping -- and stepping the level, ascending so
// it never surprises you, turns "it sounds bad" into "the first bad step was 4".
constexpr float kSweepSteps[] = {0.0f, 0.00366f, 0.01f, 0.0366f, 0.10f};
constexpr size_t kSweepCount = sizeof(kSweepSteps) / sizeof(kSweepSteps[0]);
constexpr uint32_t kSweepHoldMs = 6000;
constexpr uint16_t kSweepHz = 440;

void volume_sweep() {
  Serial.println();
  Serial.println("[sweep] continuous 440 Hz, ascending amplitude, 6 s per step, looping");
  Serial.println("[sweep] steps: 0 (silence, the control) 0.00366  0.01  0.0366  0.10");
  Serial.println("[sweep] 0.0366 is today's default; 0.10 is 10% of full scale.");
  Serial.println("[sweep] tell me WHICH STEP NUMBER is the first one that distorts.");
  Serial.println();

  for (;;) {
    for (size_t i = 0; i < kSweepCount; ++i) {
      const float a = kSweepSteps[i];
      set_volume(a);
      // dBFS is the number a meter would read, and it makes the steps comparable
      // to the default without anyone doing arithmetic in their head.
      const float dbfs = (a > 0.0f) ? 20.0f * log10f(a) : -120.0f;
      Serial.printf("[sweep] step %u/%u  amplitude %.5f  (%.2f%% FS, %.1f dBFS)\n",
                    (unsigned)(i + 1), (unsigned)kSweepCount, a, a * 100.0f, dbfs);
      play_tone(kSweepHz, kSweepHoldMs, kNoSupersede);
      play_tone(0, 700, kNoSupersede);  // clear gap so steps do not run together
    }
  }
}
#endif

#ifndef BRAIN_CONT_TONE
#define BRAIN_CONT_TONE 0
#endif
#ifndef BRAIN_CONT_HZ
#define BRAIN_CONT_HZ 440
#endif
// Amplitude in per-mille, so it can be set from a -D flag without a float macro.
// 10 = 0.010 = 1 % of full scale = about -40 dBFS (step 3 of the sweep).
#ifndef BRAIN_CONT_AMP_MILLI
#define BRAIN_CONT_AMP_MILLI 10
#endif

#if BRAIN_CONT_TONE
// --- continuous tone, for PHYSICAL debugging (see the `tone` env) -----------
//
// One steady tone, forever, so a cable or a connector can be flexed while the
// signal is audible. A CONTINUOUS tone is the right tool for that and a beep is
// not: any momentary break in the connection is a discontinuity, and a
// discontinuity in a continuous tone is an instant, unmistakable click. With a
// 220 ms beep you would have to be lucky to flex at the same time as the sound.
//
// It also makes the fault reproducible for someone else, which is the difference
// between "it crackles sometimes" and "flex the USB plug and it clicks".
void continuous_tone() {
  const float amp = (float)BRAIN_CONT_AMP_MILLI / 1000.0f;
  set_volume(amp);
  const float dbfs = (amp > 0.0f) ? 20.0f * log10f(amp) : -120.0f;

  Serial.println();
  Serial.printf("[tone] CONTINUOUS %u Hz at %.3f (%.2f%% FS, %.1f dBFS), forever.\n",
                (unsigned)BRAIN_CONT_HZ, amp, amp * 100.0f, dbfs);
  Serial.println("[tone] Flex things one at a time and listen for a click:");
  Serial.println("[tone]   1. the USB plug at the board, then at the laptop");
  Serial.println("[tone]   2. the three I2S wires to the DAC, and the DAC's ground");
  Serial.println("[tone]   3. the amp's 5 V and ground wires");
  Serial.println("[tone]   4. the speaker leads");
  Serial.println("[tone] A click pins the fault on whichever thing you were touching.");
  Serial.println();

  // 60 s chunks rather than one endless call: it keeps millis() arithmetic
  // trivially safe and restarts the phase at zero, which is a zero crossing, so
  // the seam is inaudible.
  for (;;) {
    play_tone((uint16_t)BRAIN_CONT_HZ, 60000, kNoSupersede);
  }
}
#endif

// --- the player task -------------------------------------------------------
// Core 0, per plan section 10.1: "WiFi, the camera task and audio on core 0; the
// control task on core 1". Arduino's loop() runs on core 1 (the board def sets
// ARDUINO_RUNNING_CORE=1), so this does not compete with the control task that
// will eventually live there.
//
// IF THE AUDIO EVER CRACKLES WHILE THE CAMERA STREAMS, the order to try things is:
//   1. lower the camera's `framesize` (plan section 6 says exactly this);
//   2. then move this task to core 1 -- it is idle otherwise, and it is the one
//      change that addresses every cause of starvation here at once.
// Do the framesize first: it is the cheaper change and the more likely cause.
void player_task(void *) {
#if BRAIN_VOLUME_SWEEP
  // This build has no normal playback path: the sweep runs forever. It is not
  // marked noreturn, so the loop below stays reachable and there is no warning.
  volume_sweep();
#endif
#if BRAIN_CONT_TONE
  // Same idea: diagnostic builds hand the task over to a single endless signal.
  continuous_tone();
#endif
  for (;;) {
    if (s_request == 0) {
      vTaskDelay(pdMS_TO_TICKS(10));  // idle: 10 ms of latency, no CPU
      continue;
    }

    // Read the pair atomically -- see the note in request(). Reading `generation`
    // and then `request` without this would let a request arriving in between be
    // marked superseded by itself, and silently skipped.
    taskENTER_CRITICAL(&s_mux);
    const uint32_t gen = s_generation;
    const uint8_t which = s_request;
    s_request = 0;
    taskEXIT_CRITICAL(&s_mux);
    s_playing = which;

    // Apply any pending volume change now, between sounds, never during one.
    if (g_volume_dirty) {
      g_volume_dirty = false;
      set_volume(g_pending_volume);
    }

    Serial.printf("[audio] playing %s (%u Hz) at %.4f (%.2f%% FS, %.1f dBFS)\n",
                  which == 1 ? "beeps" : "clip", (unsigned)SAMPLE_RATE, g_volume,
                  g_volume * 100.0f,
                  (g_volume > 0.0f) ? 20.0f * log10f(g_volume) : -120.0f);

    const uint32_t started = millis();
    if (which == 1) {
      play_beeps(gen);
    } else {
      play_clip_file(gen);
    }
    const uint32_t took = millis() - started;

    s_playing = 0;
    if (!superseded(gen)) {
      // *** THE DIAGNOSTIC THAT MATTERS FOR "THE AUDIO IS DISTORTED". ***
      // A healthy playback takes about as long as the sound is long. If it takes
      // much longer, then i2s_write() was repeatedly blocking on a DMA chain that
      // had already run dry -- this task was STARVED, most likely by WiFi or the
      // camera, and the DAC was fed gaps where samples should have been. Crackle
      // and "distorted but continuous" is exactly what that sounds like, and it is
      // NOT an audio bug: it is plan section 6's shared-PSRAM-bus prediction, or
      // contention for core 0, showing up in the only place you can hear it.
      //
      // Expected: beeps 3 x (220 + 120) = 1020 ms; clip = 182414 / 44100 = 4136 ms.
      // The "late" thresholds below are ~25 % over, which is already underrun.
      const uint32_t asked = (which == 1) ? 1020u : 4136u;
      const uint32_t late = (asked * 5u) / 4u;
      Serial.printf("[audio] done in %u ms (expected ~%u ms)%s\n",
                    (unsigned)took, (unsigned)asked,
                    (took > late) ? "  <-- LATE: DMA underran, task was starved"
                                  : "");
    }
  }
}

}  // namespace

void audio_init() {
  build_sine_table();
  i2s_init();
  // 8 KB of stack: play_tone() and play_clip_file() each put a 1 KB interleaved
  // buffer on the stack, and a stack overflow on this core is a Silent Reboot
  // with no message. The headroom is cheap.
  xTaskCreatePinnedToCore(player_task, "audio", 8192, nullptr, 4, nullptr, 0);
}

void audio_request_beeps() { request(1); }
void audio_request_clip() { request(2); }

void audio_set_volume(float fraction) {
  if (fraction < 0.0f) fraction = 0.0f;
  if (fraction > 1.0f) fraction = 1.0f;
  g_pending_volume = fraction;
  g_volume_dirty = true;
}

float audio_volume() { return g_pending_volume; }

bool audio_busy() { return s_playing != 0; }

const char *audio_now_playing() {
  switch (s_playing) {
    case 1:
      return "beeps";
    case 2:
      return "clip";
    default:
      return "idle";
  }
}

uint32_t audio_sample_rate() { return SAMPLE_RATE; }
