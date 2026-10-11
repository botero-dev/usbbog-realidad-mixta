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
// ---------------------------------------------------------------------------

// One byte through the CRC. Kept separate so the self-check below can run
// without needing an array inside a constant expression.
constexpr uint8_t crc8_byte(uint8_t crc, uint8_t byte) {
  crc ^= byte;
  for (uint8_t bit = 0; bit < 8; ++bit) {
    crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07)
                       : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

constexpr uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0x00;
  for (size_t i = 0; i < len; ++i) {
    crc = crc8_byte(crc, data[i]);
  }
  return crc;
}

// A cheap way to catch a typo in the polynomial or the loop: if this ever fails
// to compile, the CRC is wrong and the link would silently reject everything.
constexpr uint8_t crc8_self_test() {
  const char* v = "123456789";
  uint8_t crc = 0x00;
  for (uint8_t i = 0; i < 9; ++i) {
    crc = crc8_byte(crc, static_cast<uint8_t>(v[i]));
  }
  return crc;
}
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

}  // namespace proto
