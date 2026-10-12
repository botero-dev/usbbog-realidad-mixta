// protocol.h — the S3 <-> Pico link contract.
// ---------------------------------------------------------------------------
// This file is the single source of truth for the wire format. It is pure C++
// with no Arduino/Pico dependencies, so it can be compiled and unit-tested on
// the host as well as on both MCUs.
//
// Authority: docs/circuit-plan.md section 4.
//
// Link: 921600 8N1, one crossover pair plus a common ground, both ends 3.3 V.
//
//   S3 -> Pico  13 bytes at 100 Hz
//   Pico -> S3  11 bytes, optional for v1
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace proto {

// ---------------------------------------------------------------------------
// Frame constants
// ---------------------------------------------------------------------------

static const uint8_t SYNC0 = 0xAA;
static const uint8_t SYNC1 = 0x55;

static const size_t TX_FRAME_LEN = 13;  // S3 -> Pico
static const size_t REPLY_LEN    = 11;  // Pico -> S3

// S3 -> Pico message types (offset 2)
enum MsgType : uint8_t {
  MSG_SET_DUTIES = 0x01,  // drive at the four duties in this frame
  MSG_BRAKE      = 0x02,  // short brake all four (IN1=IN2=H)
  MSG_COAST      = 0x03,  // high-Z all four (IN1=IN2=L, PWM=H)
  MSG_DISABLE    = 0x04,  // drop STBY and stop listening for duties
  MSG_PING       = 0x05,  // liveness only; does not move anything
};

// ---------------------------------------------------------------------------
// TWO READINGS THE WIRE FORMAT DOES NOT SPELL OUT -- settled here, because both
// ends must agree and neither can infer the answer from the bytes.
// ---------------------------------------------------------------------------
//
// 1. WHICH TYPES FEED THE 100 ms DEAD-MAN.
//    SET/BRAKE/COAST/DISABLE do: they command motion, and the timer exists to
//    stop the car when commands stop arriving.
//    PING does NOT. "Liveness only; does not move anything" is read literally.
//    If PING refreshed the timer, a brain that kept pinging but stopped sending
//    duties would hold the last commanded duty forever -- the timer would be
//    running and protecting nothing. The reply still reports PING as
//    link-good: "is the S3 talking" and "does the S3 still own the motors" are
//    different questions.
//
// 2. DISABLE IS NOT LATCHED.
//    It is a frame, not a mode: the Pico applies it and keeps listening, so the
//    next SET re-arms the drivers. A latch would need a matching re-enable
//    message, which this protocol does not have -- it would make DISABLE a
//    one-way trip whose only exit was a reboot. The "stay off" intent therefore
//    belongs to the brain, which is the only end that knows what it wants:
//    keep sending DISABLE, or stop sending and let the dead-man land it.
// ---------------------------------------------------------------------------

// Pico -> S3 message type (offset 2 of the reply)
static const uint8_t MSG_REPLY = 0x81;

// Status bits (reply offset 4)
enum StatusBit : uint8_t {
  ST_STBY_HIGH  = 1u << 0,
  ST_LINK_GOOD  = 1u << 1,
  ST_WDT_ARMED  = 1u << 2,
};

// Fault codes (reply offset 5)
enum Fault : uint8_t {
  FAULT_OK      = 0,
  FAULT_CRC     = 1,  // at least one frame failed CRC since the last reply
  FAULT_TIMEOUT = 2,  // the dead-man fired since the last reply
  FAULT_HW      = 3,  // reserved for a hardware fault pin
};

// ---------------------------------------------------------------------------
// Duties
// ---------------------------------------------------------------------------
//
// Units are permille: -1000 .. +1000. Sign is direction, magnitude is PWM.
// Zero means COAST, which on a TB6612 requires IN1=IN2=L *and PWM high* -- see
// motors.cpp. Do not "simplify" that to PWM=0; that is short brake instead.

static const int16_t DUTY_MAX = 1000;

// Motor order in the frame. This MUST match duty_motors[] in
// firmware/car_control/main.cpp and the physical wiring of the two drivers:
//   driver #1 (left)  -> FL = channel A, RL = channel B
//   driver #2 (right) -> RR = channel A, FR = channel B
enum MotorId : uint8_t {
  MOT_FL = 0,
  MOT_RL = 1,
  MOT_RR = 2,
  MOT_FR = 3,
  MOTOR_COUNT = 4,
};

// ---------------------------------------------------------------------------
// Frame byte offsets
// ---------------------------------------------------------------------------

enum FrameOffset : size_t {
  OFF_SYNC = 0,   // 2 bytes
  OFF_TYPE = 2,   // 1 byte
  OFF_SEQ  = 3,   // 1 byte
  OFF_DUTY = 4,   // 8 bytes, four int16 little-endian
  OFF_CRC  = 12,  // 1 byte, over bytes 2..11
};

enum ReplyOffset : size_t {
  ROFF_SYNC    = 0,  // 2 bytes
  ROFF_TYPE    = 2,  // 1 byte
  ROFF_SEQ     = 3,  // 1 byte
  ROFF_STATUS  = 4,  // 1 byte
  ROFF_FAULT   = 5,  // 1 byte
  ROFF_UPTIME  = 6,  // 2 bytes, low 16 bits of uptime in ms
  ROFF_RESERVED = 8, // 2 bytes, encoder counts later
  ROFF_CRC     = 10, // 1 byte, over bytes 2..9
};

// ---------------------------------------------------------------------------
// CRC-8, polynomial 0x07, init 0x00, no reflection, no final XOR.
// This is CRC-8/SMBUS; its check value over "123456789" is 0xF4, asserted below.
//
// *** WHY THIS IS RECURSION AND NOT A LOOP -- DO NOT "SIMPLIFY" IT BACK. ***
// A loop (or any local variable) inside a constexpr function body is C++14
// "relaxed constexpr". The ESP32 Arduino core builds with -std=gnu++11, where
// GCC rejects it outright:
//     error: body of 'constexpr' function ... not a return-statement
// This header is the shared contract between TWO toolchains with different
// defaults (Arduino/xtensa at gnu++11, the Pico SDK at C++17), so it stays
// deliberately at the LOWEST common standard: every function here is a single
// return statement, which C++11 accepts. Changing these to loops means the
// brain stops compiling until its build is raised to -std=gnu++14.
// ---------------------------------------------------------------------------

// One shift-and-xor step: a single bit of the CRC.
constexpr uint8_t crc8_bit(uint8_t crc) {
  return (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                      : static_cast<uint8_t>(crc << 1);
}

// `count` bits, recursively. The depth is 8 for a byte.
constexpr uint8_t crc8_bits(uint8_t crc, uint8_t count) {
  return count == 0
             ? crc
             : crc8_bits(crc8_bit(crc), static_cast<uint8_t>(count - 1));
}

// One byte through the CRC.
constexpr uint8_t crc8_byte(uint8_t crc, uint8_t byte) {
  return crc8_bits(static_cast<uint8_t>(crc ^ byte), 8);
}

// A whole buffer, folded left to right so the LAST byte is applied last, exactly
// as the loop version did.
constexpr uint8_t crc8(const uint8_t* data, size_t len) {
  return len == 0 ? 0x00 : crc8_byte(crc8(data, len - 1), data[len - 1]);
}

// A cheap way to catch a typo in the polynomial: if this ever fails to compile,
// the CRC is wrong and the link would silently reject every frame.
//
// The check input is a namespace-scope array, for two separate C++11 reasons:
//   * a constexpr function may not declare variables, so it cannot be local;
//   * a string literal cannot be used either -- casting one to uint8_t* needs a
//     reinterpret_cast, and that is NOT a constant expression (GCC warns
//     "'reinterpret_cast' is not a constant expression" and the assert fails).
// An array of character constants has static storage duration, so its address
// IS a constant expression and the read is legal here. Internal linkage via
// `constexpr`, so each translation unit quietly gets its own copy.
constexpr uint8_t kCrcCheckInput[9] = {'1', '2', '3', '4', '5',
                                       '6', '7', '8', '9'};
constexpr uint8_t crc8_self_test() { return crc8(kCrcCheckInput, 9); }
static_assert(crc8_self_test() == 0xF4,
              "CRC-8 poly 0x07 must give 0xF4 over \"123456789\"");

// ---------------------------------------------------------------------------
// Little-endian helpers (the RP2040 is little-endian, but the host test tool
// and the ESP32 both have to agree explicitly, so this is spelled out)
// ---------------------------------------------------------------------------

inline void put_i16le(uint8_t* p, int16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline int16_t get_i16le(const uint8_t* p) {
  return static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                              (static_cast<uint16_t>(p[1]) << 8));
}

// ---------------------------------------------------------------------------
// Decoded frame
// ---------------------------------------------------------------------------

struct Frame {
  uint8_t type;
  uint8_t seq;
  int16_t duty[MOTOR_COUNT];
};

inline bool sync_ok(const uint8_t* f) {
  return f[OFF_SYNC] == SYNC0 && f[OFF_SYNC + 1] == SYNC1;
}

inline bool crc_ok(const uint8_t* f) {
  return crc8(f + OFF_TYPE, OFF_CRC - OFF_TYPE) == f[OFF_CRC];
}

inline bool frame_ok(const uint8_t* f) { return sync_ok(f) && crc_ok(f); }

inline void frame_decode(const uint8_t* f, Frame* out) {
  out->type = f[OFF_TYPE];
  out->seq = f[OFF_SEQ];
  for (uint8_t i = 0; i < MOTOR_COUNT; ++i) {
    out->duty[i] = get_i16le(f + OFF_DUTY + 2 * i);
  }
}

// Encoder. Used by the Pico only in tests, but kept here so the ESP32 side and
// the host tool encode identically instead of each rolling their own.
inline void frame_encode(uint8_t* f, uint8_t type, uint8_t seq,
                         const int16_t duty[MOTOR_COUNT]) {
  f[OFF_SYNC] = SYNC0;
  f[OFF_SYNC + 1] = SYNC1;
  f[OFF_TYPE] = type;
  f[OFF_SEQ] = seq;
  for (uint8_t i = 0; i < MOTOR_COUNT; ++i) put_i16le(f + OFF_DUTY + 2 * i, duty[i]);
  f[OFF_CRC] = crc8(f + OFF_TYPE, OFF_CRC - OFF_TYPE);
}

inline void reply_encode(uint8_t* f, uint8_t seq, uint8_t status, uint8_t fault,
                         uint16_t uptime_ms) {
  f[ROFF_SYNC] = SYNC0;
  f[ROFF_SYNC + 1] = SYNC1;
  f[ROFF_TYPE] = MSG_REPLY;
  f[ROFF_SEQ] = seq;
  f[ROFF_STATUS] = status;
  f[ROFF_FAULT] = fault;
  f[ROFF_UPTIME] = static_cast<uint8_t>(uptime_ms & 0xFF);
  f[ROFF_UPTIME + 1] = static_cast<uint8_t>((uptime_ms >> 8) & 0xFF);
  f[ROFF_RESERVED] = 0;
  f[ROFF_RESERVED + 1] = 0;
  f[ROFF_CRC] = crc8(f + ROFF_TYPE, ROFF_CRC - ROFF_TYPE);
}

// ---------------------------------------------------------------------------
// Reply (Pico -> S3)
// ---------------------------------------------------------------------------
//
// Same sync pattern, same CRC, same sliding-window hunt as the frame above --
// only the length and the CRC's covered range differ, which is exactly the kind
// of near-miss that gets validated with the wrong constant. So the check lives
// here, next to the offsets it uses, rather than being re-derived by each end.
struct Reply {
  uint8_t type;       // MSG_REPLY
  uint8_t seq;        // echoes the seq of the last frame the Pico accepted
  uint8_t status;     // ST_* bits
  uint8_t fault;      // FAULT_* (one-shot: OK in the reply after the fault)
  uint16_t uptime_ms; // Pico uptime, low 16 bits
};

// NOTE: sync_ok() is shared with the 13-byte frame on purpose -- both formats
// put the sync at offset 0, so one helper covers both and there is no second
// magic offset to get wrong.
inline bool reply_ok(const uint8_t* f) {
  return sync_ok(f) &&
         crc8(f + ROFF_TYPE, ROFF_CRC - ROFF_TYPE) == f[ROFF_CRC];
}

inline void reply_decode(const uint8_t* f, Reply* out) {
  out->type = f[ROFF_TYPE];
  out->seq = f[ROFF_SEQ];
  out->status = f[ROFF_STATUS];
  out->fault = f[ROFF_FAULT];
  out->uptime_ms = static_cast<uint16_t>(
      static_cast<uint16_t>(f[ROFF_UPTIME]) |
      (static_cast<uint16_t>(f[ROFF_UPTIME + 1]) << 8));
}

// ---------------------------------------------------------------------------
// SyncHunt — finding a frame in a byte stream.
// ---------------------------------------------------------------------------
//
// *** WHY A SLIDING WINDOW AND NOT A STATE MACHINE. ***
// Every received byte shifts the window left and lands at the end. Any time the
// first two bytes are the sync pattern, the window is a CANDIDATE, and it counts
// only if the CRC agrees. A bad CRC needs no recovery logic at all: the next byte
// slides it along and the search continues.
//
// What that buys, concretely: the frame can reappear anywhere in a stream of
// junk, and it does -- the S3's ROM bootloader prints its banner on UART0 at
// 115200 on every reset, which a Pico listening on the pins that carried it would
// see as ~30 bytes of wrong-baud noise in the middle of the window. A state
// machine that latched "13 bytes after a sync" would need explicit handling for a
// false sync inside that noise. This has no such case. (With the link moved to
// UART1, plan section 2.3's banner problem no longer applies to THIS project --
// but the robustness is the reason the code is shaped this way, not an accident,
// and it is what tools/test_link.cpp exercises.)
//
// Both ends use this class rather than each rolling its own shift loop, so the
// hunt cannot be implemented twice and differ.
//
// KNOWN AND BOUNDED: a false accept is possible. CRC-8 is 8 bits, so a random
// window that happens to start with the sync pattern passes roughly 1 time in
// 256 -- tools/test_link.cpp measures one, at the boundary between a burst of
// noise and a real frame. This is a property of the plan's chosen CRC, not a bug
// in this code: the window still does its job (the real frame is recovered
// intact, immediately after). What bounds the damage is elsewhere -- a false
// accept can command at most ONE frame, and the Pico's slew limit caps any single
// frame at +-100 permille until the next real frame corrects it 10 ms later.
// Do not "fix" this by lengthening the frame or widening the CRC without
// re-reading plan section 4, which chose both deliberately.
class SyncHunt {
 public:
  typedef bool (*Validator)(const uint8_t*);

  // `win` is caller-owned (static storage on both MCUs, so it starts zeroed) and
  // must hold at least `len` bytes. `valid` is the whole-frame check -- frame_ok()
  // for the S3 -> Pico direction, reply_ok() for the reply.
  SyncHunt(uint8_t* win, size_t len, Validator valid)
      : win_(win), len_(len), valid_(valid) {}

  // Feed one byte. True exactly when the window now holds a complete valid frame.
  bool feed(uint8_t byte) {
    for (size_t i = 0; i + 1 < len_; ++i) win_[i] = win_[i + 1];
    win_[len_ - 1] = byte;
    return sync_ok(win_) && valid_(win_);
  }

  // The window itself, for frame_decode()/reply_decode() once feed() said yes.
  const uint8_t* window() const { return win_; }
  uint8_t* window() { return win_; }

 private:
  uint8_t* win_;
  size_t len_;
  Validator valid_;
};

}  // namespace proto
