// motor_link.h — the S3 -> Pico link, from the brain's side.
// ---------------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 4 (the frame and the dead-man) and
// section 10.1 (firmware notes).
//
// This is the SENDING end of protocol.h, plus the receive path for the Pico's
// 11-byte reply. It owns three jobs:
//
//   1. THE 100 Hz FRAME. A task pinned to core 1 sends one frame every 10 ms.
//      It runs at a fixed rate regardless of the web traffic, because the Pico's
//      100 ms dead-man is keyed on frames ARRIVING -- a link that only sends when
//      a human moves a finger would drop the dead-man every time they stopped.
//   2. THE CONTROL WATCHDOG. See kControlTimeoutMs below. This is the brain's
//      own safety net and it is NOT the same thing as the Pico's dead-man.
//   3. THE MIXER. throttle / front_rear / left_right -> four signed duties,
//      via mixer.h, which is the port of firmware/car_control.
//
// UART1, not UART0. Plan section 2 puts the link on GPIO43/44 (U0TXD/U0RXD),
// but on THIS carrier those two pins are the USB-UART console -- the board's
// boot log arrives over them -- so using them for the link would mean giving up
// the serial log for the whole of bring-up. The ESP32-S3's GPIO matrix routes a
// UART anywhere, so the link moved to UART1 on the two pins that the I2C move
// freed. Bonus, and not a small one: the S3's ROM bootloader banner goes out on
// UART0, so with the link on UART1 the Pico never sees it at all, and the
// "Pico sees ~30 bytes of garbage on every S3 boot" note in plan section 2.3
// stops applying.
//
//   GPIO1  TX -> Pico GP17   (UART1)
//   GPIO2  RX <- Pico GP16
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

namespace motor_link {

// ---------------------------------------------------------------------------
// THE CONTROL WATCHDOG, and why the Pico's dead-man does not cover it.
//
// The Pico's 100 ms dead-man fires when FRAMES stop arriving, which is the right
// protection for the brain crashing, rebooting, or the TX wire falling off.
//
// It does NOT protect against the far more likely failure: the brain is fine and
// still sending, but the human's phone has gone to sleep, walked out of Wi-Fi
// range, or closed the page. In that case the brain keeps dutifully sending the
// last duty it was told -- so the car keeps driving, at speed, with nobody
// holding a control. The Pico cannot detect this: from its point of view nothing
// has changed.
//
// So the brain stops its own driving when commands stop arriving. 250 ms is
// short enough that a lost client means stopped-and-braking rather than
// runaway, and long enough to survive the jitter of a page sending at 10 Hz.
//
// Deliberately BRAKE, not COAST: a coasting car keeps rolling, and the whole
// point of this watchdog is to stop it. It stays in brake, powered, until a new
// command arrives -- which is nearly free, because a short brake draws no
// current unless the wheels are turned.
// ---------------------------------------------------------------------------
constexpr uint32_t kControlTimeoutMs = 250;

// Plan section 4: frames at 100 Hz.
constexpr uint32_t kFrameHz = 100;

// Plan section 4: 921600 8N1. One frame is 13 bytes = ~141 us on the wire, so
// the link is busy about 1.4 % of the time.
constexpr uint32_t kBaud = 921600;

// Bring up UART1 (GPIO1 TX / GPIO2 RX, 921600 8N1) and start the 100 Hz task.
// Returns false if the task could not be created.
//
// Until a command arrives the link sends DISABLE at 100 Hz, so the car powers up
// with the drivers high-Z and stays that way until something asks for motion.
bool init();

// --- commands. Safe to call from any task; the HTTP handlers do. ---

// The mixer interface: three human-intent values in permille, -1000..+1000.
// See mixer.h for what each one means. Refreshes the control watchdog.
void drive(int16_t throttle, int16_t front_rear, int16_t left_right);

// Raw four duties, bypassing the mixer. For bring-up and for testing the link
// without arguing about signs. Refreshes the control watchdog.
void drive_raw(const int16_t duty[proto::MOTOR_COUNT]);

// Explicit stop modes. These are held until something else is commanded, and
// they do NOT time out: braking is a resting state, not a command that expires.
void brake();
void coast();

// Drop STBY on the Pico: drivers high-Z, nothing energised. The Pico does not
// latch this (see protocol.h), so this must be re-sent to persist -- which is
// exactly what the 100 Hz task does.
void disable();

// Link liveness only. Deliberately does NOT move anything and does NOT feed the
// Pico's dead-man; see protocol.h reading 1.
void ping();

// --- reporting, for /status and the heartbeat ---

struct State {
  bool pico_seen;         // at least one valid reply has been parsed
  uint8_t pico_status;    // ST_* bits from the last reply
  uint8_t pico_fault;     // FAULT_* from the last reply
  uint16_t pico_uptime_ms;
  uint32_t pico_age_ms;   // ms since the last reply (0xFFFFFFFF if never)
  uint32_t tx_frames;     // frames sent since boot
  uint32_t rx_replies;    // reply frames accepted since boot
  uint32_t rx_crc_errors; // reply syncs whose CRC failed (line noise)
  uint8_t mode;           // the MSG_* type currently being commanded
  bool control_timeout;   // true while the watchdog is holding BRAKE
  int16_t duty[proto::MOTOR_COUNT];  // the last four duties the mixer produced
};

// Snapshot. Safe to call from any task.
void state(State* out);

// "SET"/"BRAKE"/"COAST"/"DISABLE"/"PING" for the log and the JSON.
const char* mode_name(uint8_t type);

}  // namespace motor_link
