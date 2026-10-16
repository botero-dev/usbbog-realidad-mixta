// link.h — the S3 -> Pico link: framing, the dead-man, and the slew limit.
// ---------------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 4 (frame format and the dead-man)
// and section 10.2 (firmware notes).
//
// This is the "body" half of the contract in protocol.h. Everything electrical
// lives in motors.h; this layer only decides WHAT to drive, from bytes.
//
//   GP16  UART0 TX  -> S3 GPIO2   (never the console: under `framework =
//   GP17  UART0 RX  <- S3 GPIO1    picosdk` stdio defaults to USB, so the
//                                  debug log and the link cannot collide)
//
// The three jobs, in the order they matter:
//
//   1. THE DEAD-MAN. If motion commands stop arriving for `kDeadmanMs`, STBY is
//      dropped and the drivers go high-Z. This is the link's whole reason for
//      existing: it is what stops the car when the S3 crashes, reboots, or the
//      TX wire falls off. Everything else here is bookkeeping.
//   2. Framing. A 13-byte frame found by a sliding sync window and accepted
//      only if its CRC matches, so the S3's ROM banner and any line noise are
//      rejected rather than obeyed.
//   3. The slew limit. +-100 permille per frame, the safety net that keeps one
//      bad frame from commanding a full-scale reversal.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#include "protocol.h"

namespace link {

// 200 ms at 100 Hz = 20 missed frames (docs/circuit-plan.md section 4). **Raised
// from 100 ms on 2026-10-12 at the user's request** -- deliberately widened, not a
// fix for an observed cutout, which is what the plan warns about: it says ".Tunable
// to 200 ms if you see spurious cutouts -- but find out *why* first".
//
// THE COST IS REAL, and it is why 100 ms was the number in the first place: 200 ms
// is two whole tenths of a second of the motors still holding their last commanded
// duty after the S3 stops sending. `STBY` low is a COAST, not a brake, so the car
// rolls for that long -- roughly 20 cm at 1 m/s where it used to be 10.
//
// The number to re-derive if the frame rate ever changes; nothing else assumes it.
// Public so the boot banner prints the real value instead of a copy.
constexpr uint32_t kDeadmanMs = 200;

// +-100 permille per frame: full scale in 100 ms at 100 Hz. The safety net that
// stops one frame which happens to pass CRC from commanding a full-scale
// reversal. Public for the same reason as kDeadmanMs.
constexpr int16_t kSlewPerFrame = proto::DUTY_MAX / 10;

// Bring up UART0 on GP16/GP17 at 921600 8N1, arm the dead-man and run forever.
// Never returns. motors::init() must already have been called.
void run();

// Counters for the periodic debug line, and for the bring-up checklist.
// A snapshot of state at the instant this is called, not a delta.
struct Stats {
  uint32_t frames_ok;    // frames accepted (CRC good) since boot
  uint32_t crc_errors;   // sync pattern seen but CRC wrong since boot
  uint32_t timeouts;     // how many times the dead-man has fired since boot
  uint32_t replies_sent;
  uint8_t last_seq;      // seq of the last accepted frame
  uint8_t last_type;     // type of the last accepted frame
  bool deadman_fired;    // true if the most recent transition was the timeout
  bool stby;             // motors::stby_enabled()
  int16_t duty[proto::MOTOR_COUNT];  // what is actually being driven
};

void stats(Stats* out);

}  // namespace link
