// test_link.cpp — host-side tests for the S3 <-> Pico link contract.
// ---------------------------------------------------------------------------
// These run on the PC, not on either MCU. protocol.h and mixer.h are both pure
// C++ with no Arduino or Pico SDK dependency for exactly this reason: the wire
// format and the mixing arithmetic can be tested without a car on the bench.
//
// Build and run (two lines, no continuations -- a `//` comment ending in a
// backslash continues onto the next line and warns under -Wcomment):
//
//   cd firmware/brain
//   g++ -std=gnu++11 -Wall -Wextra -I include -I ../pico_body/include tools/test_link.cpp -o /tmp/test_link
//   /tmp/test_link
//
// -std=gnu++11 is DELIBERATE, not leftover: that is what the ESP32 Arduino core
// builds with, so this doubles as the proof that protocol.h has not quietly
// drifted into C++14. (It did once -- see the CRC comment in protocol.h.)
//
// Why each test is here:
//   * CRC-8 against an independently written loop implementation, over every
//     length from 0 to 64. The standard check value alone would not catch a bug
//     in a framing offset; comparing whole buffers does.
//   * Frame and reply round-trips, over all five message types and the extreme
//     duties, including the full int16 range.
//   * CORRUPTION. Every single-bit flip in a frame must be REJECTED. This is the
//     property the whole fail-safe argument rests on: a corrupt frame that is
//     obeyed is a car that does something nobody asked for.
//   * SyncHunt, fed a frame buried in bytes that themselves contain the sync
//     pattern, and fed a corrupted frame followed by a good one. These are the
//     two cases that justify the sliding window, and it is the piece of link
//     logic that is genuinely easy to get subtly wrong. The first of these also
//     MEASURED a real property of the design: see the note in the test itself.
//   * The mixer, including a case where the 32-bit intermediate changes the
//     ANSWER and not merely the code.
//   * The turn convention and to_wire(), TOGETHER. They are one fix -- the mixer's
//     left_right sign was flipped the same day the crossed left/right channels were
//     corrected -- and either half alone inverts the car's steering. This is where that
//     pairing is pinned down.
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mixer.h"
#include "protocol.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      ++g_failures;                                                     \
      printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                   \
  } while (0)

// An independent CRC-8 (poly 0x07, init 0x00) written as loops. Deliberately a
// different shape from protocol.h's recursion, so a bug in the recursive
// rewriting cannot hide behind itself. The loop version is the textbook form.
static uint8_t ref_crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

// ---------------------------------------------------------------------------

static void test_layout() {
  printf("layout: offsets agree with the declared lengths\n");

  // These two lines are the cheapest possible guard against the kind of edit that
  // moves an offset and forgets a length: everything else here indexes through
  // the enums, so a mismatch would be invisible without them.
  CHECK(proto::TX_FRAME_LEN == proto::OFF_CRC + 1);
  CHECK(proto::REPLY_LEN == proto::ROFF_CRC + 1);
  CHECK(proto::OFF_DUTY + 2 * proto::MOTOR_COUNT == proto::OFF_CRC);
  CHECK(proto::ROFF_RESERVED + 2 == proto::ROFF_CRC);
}

static void test_crc() {
  printf("crc8: matches an independent implementation, every length\n");

  // The standard check value: string "123456789" through CRC-8/SMBUS gives 0xF4.
  CHECK(proto::crc8(proto::kCrcCheckInput, 9) == 0xF4);

  uint8_t buf[65];
  for (size_t i = 0; i < sizeof(buf); ++i) {
    buf[i] = (uint8_t)(i * 37u + 11u);  // something non-repeating
  }
  for (size_t n = 0; n <= sizeof(buf); ++n) {
    CHECK(proto::crc8(buf, n) == ref_crc8(buf, n));
  }
}

static void test_frame_round_trip() {
  printf("frame: round-trip over all types, and rejection of every bit flip\n");

  const int16_t duties[][proto::MOTOR_COUNT] = {
      {0, 0, 0, 0},
      {proto::DUTY_MAX, proto::DUTY_MAX, proto::DUTY_MAX, proto::DUTY_MAX},
      {-proto::DUTY_MAX, -proto::DUTY_MAX, -proto::DUTY_MAX, -proto::DUTY_MAX},
      {1000, -1000, 1000, -1000},
      {1, -1, 32767, -32768},  // the full int16 transport range
  };
  const uint8_t types[] = {proto::MSG_SET_DUTIES, proto::MSG_BRAKE,
                           proto::MSG_COAST, proto::MSG_DISABLE,
                           proto::MSG_PING};

  for (size_t k = 0; k < sizeof(duties) / sizeof(duties[0]); ++k) {
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); ++t) {
      uint8_t f[proto::TX_FRAME_LEN];
      proto::frame_encode(f, types[t], 0x5A, duties[k]);
      CHECK(proto::frame_ok(f));

      proto::Frame got;
      proto::frame_decode(f, &got);
      CHECK(got.type == types[t]);
      CHECK(got.seq == 0x5A);
      for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
        CHECK(got.duty[i] == duties[k][i]);
      }
    }
  }

  // One flipped bit, at EVERY byte position, must be rejected. Positions 0-1 are
  // the sync (checked separately) and 2-12 are covered by the CRC. If any of
  // these were accepted, a corrupt frame would be obeyed -- which is the failure
  // mode the whole design exists to prevent.
  uint8_t good[proto::TX_FRAME_LEN];
  const int16_t d[proto::MOTOR_COUNT] = {10, 20, 30, 40};
  proto::frame_encode(good, proto::MSG_SET_DUTIES, 7, d);
  CHECK(proto::frame_ok(good));

  uint8_t bad[proto::TX_FRAME_LEN];
  for (size_t i = 0; i < proto::TX_FRAME_LEN; ++i) {
    memcpy(bad, good, sizeof(bad));
    bad[i] ^= 0x01;
    CHECK(!proto::frame_ok(bad));
  }
}

static void test_reply_round_trip() {
  printf("reply: round-trip and rejection of every bit flip\n");

  uint8_t f[proto::REPLY_LEN];
  proto::reply_encode(f, 0x33, proto::ST_STBY_HIGH | proto::ST_LINK_GOOD,
                      proto::FAULT_TIMEOUT, 12345);
  CHECK(proto::reply_ok(f));

  proto::Reply r;
  proto::reply_decode(f, &r);
  CHECK(r.type == proto::MSG_REPLY);
  CHECK(r.seq == 0x33);
  CHECK(r.status == (proto::ST_STBY_HIGH | proto::ST_LINK_GOOD));
  CHECK(r.fault == proto::FAULT_TIMEOUT);
  CHECK(r.uptime_ms == 12345);

  // The reply is a DIFFERENT length with a DIFFERENT CRC range. Re-deriving that
  // range at the call site is exactly the near-miss that gets validated with the
  // wrong constant -- so it is checked here, on the format that has one.
  uint8_t bad[proto::REPLY_LEN];
  for (size_t i = 0; i < proto::REPLY_LEN; ++i) {
    memcpy(bad, f, sizeof(bad));
    bad[i] ^= 0x01;
    CHECK(!proto::reply_ok(bad));
  }
}

static void test_sync_hunt_in_noise() {
  printf("SyncHunt: finds a frame buried in bytes containing the sync pattern\n");

  uint8_t win[proto::TX_FRAME_LEN];
  memset(win, 0, sizeof(win));
  proto::SyncHunt hunt(win, proto::TX_FRAME_LEN, proto::frame_ok);

  const int16_t d[proto::MOTOR_COUNT] = {111, -222, 333, -444};
  uint8_t frame[proto::TX_FRAME_LEN];
  proto::frame_encode(frame, proto::MSG_SET_DUTIES, 0x5A, d);

  // Junk that CONTAINS AA 55 several times without ever being a frame. This is
  // what the S3's ROM banner looked like on the pins the link used to be planned
  // on, and what a floating or noisy line looks like anywhere.
  const uint8_t junk[] = {0xAA, 0x55, 0xAA, 0x55, 0x00, 0xAA, 0x55, 0x12,
                          0x34, 0xAA, 0x55, 0xFF, 0xAA, 0x55};

  // Nothing valid can be reported out of the junk alone.
  for (size_t i = 0; i < sizeof(junk); ++i) {
    CHECK(!hunt.feed(junk[i]));
  }

  // Then the real frame goes in, and must come out INTACT.
  //
  // *** WHY THIS ASSERTS >= 1 AND NOT == 1, WHICH IS NOT SLOPPINESS. ***
  // An earlier version of this test demanded exactly one accepted frame and
  // FAILED -- correctly. Feeding the junk below and then the frame produces a
  // SECOND, coincidental acceptance: the 13 bytes straddling the junk/frame
  // boundary happen to start with AA 55 and happen to have a matching CRC-8.
  //
  // That is inherent to CRC-8, not a bug: any sync pattern matched against a
  // random window is a false accept roughly 1 time in 256. What the design
  // guarantees is that the frame is RECOVERED, and that a false accept is bounded
  // -- it can command at most one frame's worth of duty, and the Pico's slew
  // limit caps that at +-100 permille for 10 ms until the next real frame
  // corrects it. Asserting "exactly one" would be asserting a property the
  // protocol does not have. See the note on SyncHunt in protocol.h.
  int found = 0;
  for (size_t i = 0; i < proto::TX_FRAME_LEN; ++i) {
    if (hunt.feed(frame[i])) found++;
  }
  CHECK(found >= 1);

  // The decisive check: after the frame has been fed, the window IS the frame.
  CHECK(memcmp(hunt.window(), frame, proto::TX_FRAME_LEN) == 0);

  proto::Frame got;
  proto::frame_decode(hunt.window(), &got);
  CHECK(got.type == proto::MSG_SET_DUTIES);
  CHECK(got.seq == 0x5A);
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) CHECK(got.duty[i] == d[i]);
}

static void test_sync_hunt_resyncs() {
  printf("SyncHunt: a corrupted frame does not swallow the good one after it\n");

  uint8_t win[proto::TX_FRAME_LEN];
  memset(win, 0, sizeof(win));
  proto::SyncHunt hunt(win, proto::TX_FRAME_LEN, proto::frame_ok);

  const int16_t a[proto::MOTOR_COUNT] = {100, 200, 300, 400};
  const int16_t b[proto::MOTOR_COUNT] = {-100, -200, -300, -400};
  uint8_t fa[proto::TX_FRAME_LEN], fb[proto::TX_FRAME_LEN];
  proto::frame_encode(fa, proto::MSG_SET_DUTIES, 1, a);
  proto::frame_encode(fb, proto::MSG_SET_DUTIES, 2, b);
  fa[6] ^= 0x08;  // corrupt one payload byte of the first frame

  int found = 0;
  for (size_t i = 0; i < proto::TX_FRAME_LEN; ++i) {
    if (hunt.feed(fa[i])) found++;
  }
  for (size_t i = 0; i < proto::TX_FRAME_LEN; ++i) {
    if (hunt.feed(fb[i])) found++;
  }

  // Exactly one: the corrupted frame is rejected and the NEXT one is still found,
  // which is the behaviour that keeps a single bad frame from costing a link.
  CHECK(found == 1);

  proto::Frame got;
  proto::frame_decode(hunt.window(), &got);
  CHECK(got.seq == 2);
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) CHECK(got.duty[i] == b[i]);
}

static void test_mixer() {
  printf("mixer: the turn convention, the clamp, the wide intermediate, and to_wire\n");

  int16_t d[proto::MOTOR_COUNT];
  mixer::Input in = {0, 0, 0};

  // Straight drive: all four equal, the case that has to be right before anything else
  // can be -- and to_wire() must leave it alone, because a crossing cannot change four
  // equal values.
  in.throttle = 500;
  mixer::mix(in, d);
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) CHECK(d[i] == 500);
  mixer::to_wire(d);
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) CHECK(d[i] == 500);

  // *** THE TURN CONVENTION, SETTLED 2026-10-12 AND PINNED DOWN HERE. ***
  // +left_right puts LESS duty on the wheels the frame calls left, i.e. the car turns
  // LEFT, which is what the page's LEFT arrow sends. That is the OPPOSITE SIGN to
  // car_control's original, and it had to change because motor_link.cpp now calls
  // to_wire(): the two are one fix, and either alone inverts the steering.
  in.throttle = 0;
  in.front_rear = 0;
  in.left_right = 600;
  mixer::mix(in, d);
  CHECK(d[proto::MOT_FL] == -600);
  CHECK(d[proto::MOT_RL] == -600);
  CHECK(d[proto::MOT_RR] == 600);
  CHECK(d[proto::MOT_FR] == 600);

  // to_wire() then crosses the sides, because the left and right driver boards are
  // crossed on this car: the duties land on the CHANNELS that physically drive the
  // wheels they were named for. Read the result by what each slot drives, not by what
  // protocol.h calls it -- slots FL and RL are the RIGHT wheels here, so after the
  // crossing they carry the +600 and the car turns left.
  mixer::to_wire(d);
  CHECK(d[proto::MOT_FL] == 600);
  CHECK(d[proto::MOT_RL] == 600);
  CHECK(d[proto::MOT_RR] == -600);
  CHECK(d[proto::MOT_FR] == -600);

  // to_wire() must be its own INVERSE: crossing sides is symmetric, and an
  // implementation that forgot one direction would still pass every check above.
  int16_t a[proto::MOTOR_COUNT] = {11, 22, 33, 44};
  int16_t b[proto::MOTOR_COUNT];
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) b[i] = a[i];
  mixer::to_wire(b);
  CHECK(b[proto::MOT_FL] == 44 && b[proto::MOT_RL] == 33 && b[proto::MOT_RR] == 22 &&
        b[proto::MOT_FR] == 11);
  mixer::to_wire(b);
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) CHECK(b[i] == a[i]);

  // The wide intermediate, in a case where it changes the ANSWER rather than just
  // the code: fr (the +left_right term) is now on the FRONT RIGHT, so it is MOT_FR that
  // sums to 3 x 20000 = 60000 and would wrap to -5536 in a 16-bit sum -- a full-scale
  // REVERSAL out of a full-scale FORWARD command. The int32_t sum clamps to +1000.
  in.throttle = 20000;
  in.front_rear = 20000;
  in.left_right = 20000;
  mixer::mix(in, d);
  CHECK(d[proto::MOT_FL] == proto::DUTY_MAX);   // 20000 -> clamped
  CHECK(d[proto::MOT_RL] == -proto::DUTY_MAX);  // -20000
  CHECK(d[proto::MOT_RR] == proto::DUTY_MAX);   // 20000
  CHECK(d[proto::MOT_FR] == proto::DUTY_MAX);   // 60000 -> clamped
}

int main() {
  printf("link contract tests (compiled as C++11, the ESP32's standard)\n");
  printf("=================================================================\n");

  test_layout();
  test_crc();
  test_frame_round_trip();
  test_reply_round_trip();
  test_sync_hunt_in_noise();
  test_sync_hunt_resyncs();
  test_mixer();

  printf("=================================================================\n");
  printf("%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
