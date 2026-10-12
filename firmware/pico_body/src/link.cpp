// link.cpp — the S3 -> Pico link. See link.h for the shape and protocol.h for
// the wire format.
// ---------------------------------------------------------------------------

#include "link.h"

#include <pico/stdlib.h>

#include <hardware/gpio.h>
#include <hardware/uart.h>

#include "log.h"
#include "motors.h"

namespace link {
namespace {

// --- wiring ---------------------------------------------------------------
// UART0's *third* pin pair, and the only free one: GP0/GP1 and GP12/GP13 are
// motor pins now, and GP14/GP15 are UART0's CTS/RTS, not TX/RX at all (plan
// section 3). Do not move these without re-reading that note.
constexpr uint8_t kPinTx = 16;
constexpr uint8_t kPinRx = 17;
constexpr uint32_t kBaud = 921600;

// --- timing ---------------------------------------------------------------
// Derived from kDeadmanMs (link.h) so the banner and this file cannot disagree.
constexpr uint64_t kDeadmanUs = static_cast<uint64_t>(kDeadmanMs) * 1000;

// The reply is 11 bytes at 10 Hz -- 110 B/s on a 921600 link, which is nothing.
// It is deliberately slower than the frame rate so the S3 can parse it whenever
// it likes without a queue.
constexpr uint64_t kReplyPeriodUs = 100000;

// One line every 2 s. Frequent enough to catch a dead-man event in context,
// rare enough that the USB CDC output is not the thing being debugged.
constexpr uint64_t kLogPeriodUs = 2000000;

// --- parser state ---------------------------------------------------------
//
// The hunt itself lives in protocol.h (proto::SyncHunt) so that this end and the
// brain's end cannot implement the same shift-and-compare loop differently. All
// that is here is the buffer, which is static storage and therefore starts
// zeroed -- a zeroed window can never match the sync pattern, so there is no
// "first frame" special case. proto::TX_FRAME_LEN reserves the room.
//
// See protocol.h for why it is a sliding window rather than a state machine.
uint8_t g_win[proto::TX_FRAME_LEN];
proto::SyncHunt g_hunt(g_win, proto::TX_FRAME_LEN, proto::frame_ok);

uint32_t g_frames_ok = 0;
uint32_t g_crc_errors = 0;
uint32_t g_timeouts = 0;
uint32_t g_replies = 0;

uint8_t g_last_seq = 0;
uint8_t g_last_type = 0;

// What is actually being driven, i.e. AFTER the slew limit. Kept separately
// from the command because the next frame's limit is relative to this.
int16_t g_duty[proto::MOTOR_COUNT] = {0, 0, 0, 0};

// Any valid frame at all -- the "is the S3 talking to me" clock.
uint64_t g_last_frame_us = 0;
// Motion commands only -- the dead-man clock. See note_motion() below.
uint64_t g_last_motion_us = 0;

bool g_seen_frame = false;
bool g_motion_armed = false;   // a motion command has been received at least once
bool g_deadman_fired = false;  // reset by the next motion command
bool g_fault_timeout = false;  // latched for the next reply only
bool g_fault_crc = false;      // latched for the next reply only

void zero_duties() {
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) g_duty[i] = 0;
}

// Limit a commanded duty to +-kSlewPerFrame from what the motor is already
// doing. Straight-line clamp: the direction change itself is handled by
// motors.cpp, which zeroes the PWM before moving the IN pins.
int16_t slew(int16_t want, int16_t have) {
  const int16_t delta = static_cast<int16_t>(want - have);
  if (delta > kSlewPerFrame) return static_cast<int16_t>(have + kSlewPerFrame);
  if (delta < -kSlewPerFrame) return static_cast<int16_t>(have - kSlewPerFrame);
  return want;
}

// --- the dead-man ---------------------------------------------------------

// *** MOTION COMMANDS REFRESH THE TIMER; PING DOES NOT. ***
// protocol.h describes PING as "liveness only; does not move anything", and
// that is taken literally here. If PING refreshed the dead-man, a brain that
// kept pinging but stopped sending duties would hold whatever duty was last
// commanded FOREVER -- the timer would be running, but protecting nothing.
// So the timer is fed only by commands that actually command motion.
//
// The reply still counts PING as link-good, because "is the S3 talking to me"
// and "does the S3 still own the motors" are two different questions and
// conflating them is how a safety timer ends up proving the wrong thing.
void note_motion(uint64_t now_us) {
  g_last_motion_us = now_us;
  g_motion_armed = true;
  g_deadman_fired = false;
}

void handle_frame(const proto::Frame& f, uint64_t now_us) {
  g_frames_ok++;
  g_last_seq = f.seq;
  g_last_type = f.type;

  switch (f.type) {
    case proto::MSG_SET_DUTIES: {
      // Raise STBY before driving. Until it is high the TB6612 outputs are
      // high-Z no matter what the twelve logic lines say, so on the very first
      // frame after a timeout this is what actually re-enables the drivers.
      motors::stby(true);
      for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
        g_duty[i] = slew(f.duty[i], g_duty[i]);
        motors::set(i, g_duty[i]);
      }
      note_motion(now_us);
      break;
    }

    case proto::MSG_BRAKE:
      motors::stby(true);  // braking needs the bridge powered
      motors::brake_all();
      zero_duties();  // so a following SET ramps up from zero, not from the
                      // duty that was being driven before the brake
      note_motion(now_us);
      break;

    case proto::MSG_COAST:
      motors::stby(true);
      motors::coast_all();
      zero_duties();
      note_motion(now_us);
      break;

    case proto::MSG_DISABLE:
      // NOT LATCHED, deliberately. DISABLE is a frame, not a mode: the Pico
      // applies it and carries on listening, so the next SET re-enables the
      // drivers. A latch would need a matching re-enable message, and this
      // protocol has none -- it would be a one-way trip requiring a reboot.
      // The brain therefore owns the "stay off" intent (keep sending DISABLE,
      // or simply stop sending and let the dead-man land it), which is the
      // right place for it: the S3 is the one that knows what it wants.
      motors::disable_all();
      zero_duties();
      note_motion(now_us);
      break;

    case proto::MSG_PING:
    default:
      // Liveness only. Deliberately does NOT touch STBY, the duties or the
      // dead-man -- see note_motion().
      break;
  }
}

// --- parser ---------------------------------------------------------------

void feed(uint8_t byte, uint64_t now_us) {
  if (!g_hunt.feed(byte)) {
    // Distinguish "a sync pattern that is not a frame" from "just noise". Only
    // the former is worth counting: a wrong-baud banner or a floating line looks
    // like pure noise, and counting that would make crc_errors useless as a
    // signal for actual line trouble.
    if (proto::sync_ok(g_hunt.window())) {
      g_crc_errors++;
      g_fault_crc = true;
    }
    return;
  }

  proto::Frame f;
  proto::frame_decode(g_hunt.window(), &f);
  g_seen_frame = true;
  g_last_frame_us = now_us;
  handle_frame(f, now_us);
}

// --- reply ----------------------------------------------------------------

uint8_t status_bits(uint64_t now_us) {
  uint8_t bits = 0;
  if (motors::stby_enabled()) bits |= proto::ST_STBY_HIGH;
  // LINK_GOOD is about the S3 talking at all, motion or not.
  if (g_seen_frame && (now_us - g_last_frame_us) <= kDeadmanUs) {
    bits |= proto::ST_LINK_GOOD;
  }
  if (g_motion_armed) bits |= proto::ST_WDT_ARMED;
  return bits;
}

void send_reply(uint64_t now_us) {
  uint8_t f[proto::REPLY_LEN];
  const uint8_t fault = g_fault_timeout ? proto::FAULT_TIMEOUT
                        : g_fault_crc   ? proto::FAULT_CRC
                                        : proto::FAULT_OK;
  proto::reply_encode(f, g_last_seq, status_bits(now_us), fault,
                      static_cast<uint16_t>(to_ms_since_boot(get_absolute_time())));

  for (size_t i = 0; i < proto::REPLY_LEN; ++i) {
    uart_putc_raw(uart0, static_cast<char>(f[i]));
  }
  g_replies++;

  // One-shot: a fault is reported once and the next reply says OK, so the S3
  // can count EDGES rather than sampling a flag and guessing.
  g_fault_timeout = false;
  g_fault_crc = false;
}

// --- log ------------------------------------------------------------------

void print_log(uint32_t frames_in_window) {
  Stats s;
  stats(&s);

  logf("[link] %4u frames/2s  ok %lu  crc %lu  timeouts %lu  stby %u  "
       "seq %u type %02x  duty %+5d %+5d %+5d %+5d\n",
       (unsigned)frames_in_window, (unsigned long)s.frames_ok,
       (unsigned long)s.crc_errors, (unsigned long)s.timeouts,
       (unsigned)s.stby ? 1u : 0u, (unsigned)s.last_seq, (unsigned)s.last_type,
       (int)s.duty[0], (int)s.duty[1], (int)s.duty[2], (int)s.duty[3]);
}

void init_uart() {
  uart_init(uart0, kBaud);
  gpio_set_function(kPinTx, GPIO_FUNC_UART);
  gpio_set_function(kPinRx, GPIO_FUNC_UART);
  uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
  uart_set_fifo_enabled(uart0, true);
  // No flow control: the link is two signal wires plus ground (plan 2.3), and
  // the S3 side does not implement RTS/CTS either.
  uart_set_hw_flow(uart0, false, false);
}

}  // namespace

void stats(Stats* out) {
  out->frames_ok = g_frames_ok;
  out->crc_errors = g_crc_errors;
  out->timeouts = g_timeouts;
  out->replies_sent = g_replies;
  out->last_seq = g_last_seq;
  out->last_type = g_last_type;
  out->deadman_fired = g_deadman_fired;
  out->stby = motors::stby_enabled();
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) out->duty[i] = g_duty[i];
}

void run() {
  init_uart();

  const uint64_t start_us = time_us_64();
  uint64_t next_reply_us = start_us + kReplyPeriodUs;
  uint64_t next_log_us = start_us + kLogPeriodUs;
  uint32_t frames_at_last_log = 0;

  while (true) {
    // Drain the RX FIFO completely before doing anything time-based: at
    // 921600 a frame is 13 bytes and arrives every 10 ms, so this inner loop is
    // idle almost always, but draining fully first means the dead-man is always
    // evaluated against the freshest frame.
    while (uart_is_readable(uart0)) {
      feed(static_cast<uint8_t>(uart_getc(uart0)), time_us_64());
    }

    const uint64_t now_us = time_us_64();

    // --- the dead-man ---
    if (g_motion_armed && !g_deadman_fired &&
        (now_us - g_last_motion_us) > kDeadmanUs) {
      motors::disable_all();
      zero_duties();
      g_deadman_fired = true;
      g_fault_timeout = true;
      g_timeouts++;
      // Printed the moment it happens, not summarised in the periodic line:
      // this is the event step 5 of the bring-up checklist exists to provoke
      // (pull the TX wire and watch STBY drop), and it must be unmistakable in
      // the log when it does.
      logf("*** DEAD-MAN: no motion command for %u ms -> STBY LOW ***\n",
           (unsigned)(kDeadmanUs / 1000));
    }

    if (now_us >= next_reply_us) {
      send_reply(now_us);
      next_reply_us += kReplyPeriodUs;
    }

    if (now_us >= next_log_us) {
      const uint32_t delta = g_frames_ok - frames_at_last_log;
      frames_at_last_log = g_frames_ok;
      print_log(delta);
      next_log_us += kLogPeriodUs;
    }
  }
}

}  // namespace link
