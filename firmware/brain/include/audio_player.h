// audio_player.h — the S3's audio output: I2S bring-up, a tone generator, and the
// embedded clip, all driven on request.
// ---------------------------------------------------------------------------
// Two decisions shape this module:
//
// 1. NOTHING PLAYS BY DEFAULT. There is no autoplay, and `loop()` does not touch
//    the audio path. The port comes up silent and stays silent until something
//    asks for a sound (user's request, 2026-10-11). A board that starts beeping
//    the moment it powers up is a board you cannot debug: "is the amp hissing?"
//    is a much harder question when the firmware is also making noise.
//
// 2. PLAYBACK RUNS IN ITS OWN TASK, NEVER IN THE CALLER'S. `i2s_write()` with
//    `portMAX_DELAY` blocks until the DMA chain has room, i.e. for the length of
//    the sound. The caller here is an `esp_http_server` handler, and that task
//    also serves `/stream` — so playing inline would freeze the video for the
//    duration of every clip. The symptom would be "the stream stutters whenever
//    it beeps", which is a confusing thing to debug. So the HTTP side only sets
//    a request; the audio task does the writing.
//
// A new request SUPERSEDES whatever is already playing, taking effect at the
// next buffer boundary (~6 ms). Pressing both buttons quickly gives you the
// second sound — not a queue, and not a stutter.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

// Bring the I2S port up and start the player task. Silent.
// Call once, after Serial is up, before the web server starts.
void audio_init();

// Request a sound. Returns immediately: never blocks, and never fails loudly.
// A request made while something is playing replaces it.
void audio_request_beeps();  // the 440 / 660 / 880 Hz test tones
void audio_request_clip();   // the embedded clip (audio_clip.h), 4.14 s

// Introspection, for /status and the boot log.
bool audio_busy();
const char *audio_now_playing();  // "idle" | "beeps" | "clip"

// Digital amplitude, as a fraction of full scale (0.0 .. 1.0).
//
// This is the master level for every sound, and it is DEFERRED on purpose: the
// change is recorded here and applied by the audio task just before the next
// sound. Changing it takes effect immediately in the sense that the next press
// uses it, but it can never rebuild the sample table underneath a sound that is
// already playing -- which would be audible as a click. Safe to call from any
// task, including an HTTP handler.
void audio_set_volume(float fraction);
float audio_volume();  // the level the next sound will use

// The rate the I2S port is configured for. Exposed because anything that later
// streams audio (a file from flash, a stream from the car) has to match it, and
// a mismatch is silent at the DAC — it just plays at the wrong pitch.
uint32_t audio_sample_rate();
