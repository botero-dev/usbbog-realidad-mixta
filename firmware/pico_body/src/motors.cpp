// motors.cpp — see motors.h for the contract.
// ---------------------------------------------------------------------------
// Pin map, from docs/circuit-plan.md section 3. Header position in brackets.
//
//   driver #1 (left wheels)        driver #2 (right wheels)
//     GP0  [1]  PWMA  slice 0A       GP10 [14] PWMA  slice 5A
//     GP1  [2]  AIN1                 GP11 [15] AIN1
//     GP2  [4]  AIN2                 GP12 [16] AIN2
//     GP5  [7]  PWMB  slice 2B       GP15 [20] PWMB  slice 7B
//     GP4  [6]  BIN1                 GP14 [19] BIN1
//     GP3  [5]  BIN2                 GP13 [17] BIN2
//   GP28 [34] STBY, shared by both
//
// Note the B channels have IN2 physically *below* IN1 on the header. That
// reversal is deliberate (it makes the two harnesses mirror images), so the
// table below is not in ascending pin order. Do not "tidy" it.
// ---------------------------------------------------------------------------

#include "motors.h"

#include <pico/stdlib.h>

#include <hardware/gpio.h>
#include <hardware/pwm.h>

namespace {

// ---------------------------------------------------------------------------
// PWM constants — section 3.1
//   f = f_clk_sys / (clkdiv * (TOP + 1)) = 125e6 / (1.0 * 6250) = 20 kHz
//   6250 steps = 12.6 bits of resolution
// ---------------------------------------------------------------------------
constexpr float kClkDiv = 1.0f;
constexpr uint16_t kWrap = 6249;
constexpr uint16_t kLevelFull = kWrap + 1;  // 6250 == output always high

constexpr uint8_t kPinStby = 28;

// Output state. COAST and BRAKE get their own states because they need
// different IN pin patterns and BRAKE cannot be expressed as a signed duty.
enum State : int8_t {
  ST_BRAKE = -2,
  ST_REVERSE = -1,
  ST_COAST = 0,
  ST_FORWARD = 1,
};

struct Chan {
  uint8_t pin_pwm;
  uint8_t pin_in1;  // AIN1 / BIN1
  uint8_t pin_in2;  // AIN2 / BIN2
  uint8_t slice;
  uint8_t channel;
  State state;
  uint16_t level;
};

// Indexed by proto::MotorId: FL, RL, RR, FR.
Chan g_ch[proto::MOTOR_COUNT] = {
    /* FL -> driver #1 A */ {0, 1, 2, 0, 0, ST_COAST, 0},
    /* RL -> driver #1 B */ {5, 4, 3, 0, 0, ST_COAST, 0},
    /* RR -> driver #2 A */ {10, 11, 12, 0, 0, ST_COAST, 0},
    /* FR -> driver #2 B */ {15, 14, 13, 0, 0, ST_COAST, 0},
};

bool g_stby = false;

inline void set_level(Chan& c, uint16_t level) {
  pwm_set_chan_level(c.slice, c.channel, level);
  c.level = level;
}

// permille magnitude (0..1000) -> PWM compare level (0..6250)
inline uint16_t duty_to_level(uint16_t magnitude) {
  return static_cast<uint16_t>((static_cast<uint32_t>(magnitude) * kLevelFull) /
                               proto::DUTY_MAX);
}

// Move a channel to a new output state, in the order that avoids a reverse
// pulse: PWM to zero, then the direction pins, then the new level.
inline void apply_state(Chan& c, State next, uint16_t level_after) {
  if (next != c.state) {
    set_level(c, 0);

    switch (next) {
      case ST_FORWARD:
        gpio_put(c.pin_in1, 1);
        gpio_put(c.pin_in2, 0);
        break;
      case ST_REVERSE:
        gpio_put(c.pin_in1, 0);
        gpio_put(c.pin_in2, 1);
        break;
      case ST_BRAKE:
        gpio_put(c.pin_in1, 1);
        gpio_put(c.pin_in2, 1);
        break;
      case ST_COAST:
      default:
        gpio_put(c.pin_in1, 0);
        gpio_put(c.pin_in2, 0);
        break;
    }
    c.state = next;
  }

  set_level(c, level_after);
}

}  // namespace

// ---------------------------------------------------------------------------

void motors::init() {
  // 1. STBY LOW first. Until this is raised the driver outputs are high-Z no
  //    matter what the other twelve lines do, so this is the safe starting
  //    point even before the pins are configured.
  gpio_init(kPinStby);
  gpio_set_dir(kPinStby, GPIO_OUT);
  gpio_put(kPinStby, 0);
  g_stby = false;

  // 2. Direction pins low and PWM function selected with level 0, all before
  //    any slice is enabled.
  for (Chan& c : g_ch) {
    gpio_init(c.pin_in1);
    gpio_set_dir(c.pin_in1, GPIO_OUT);
    gpio_put(c.pin_in1, 0);

    gpio_init(c.pin_in2);
    gpio_set_dir(c.pin_in2, GPIO_OUT);
    gpio_put(c.pin_in2, 0);

    gpio_set_function(c.pin_pwm, GPIO_FUNC_PWM);
    c.slice = pwm_gpio_to_slice_num(c.pin_pwm);
    c.channel = pwm_gpio_to_channel(c.pin_pwm);

    pwm_set_clkdiv(c.slice, kClkDiv);
    pwm_set_wrap(c.slice, kWrap);
    c.level = 0;
    pwm_set_chan_level(c.slice, c.channel, 0);

    c.state = ST_COAST;
  }

  // 3. Enable the slices. The four pins land on four *different* slices
  //    (0, 2, 5 and 7) under this pin order, so each is configured once. All
  //    four get the same clkdiv and wrap, which is what keeps them coherent.
  //    Note slice 5 is shared by GP10/GP11 and slice 7 by GP14/GP15, but the
  //    second pin of each pair is a plain GPIO that never takes GPIO_FUNC_PWM,
  //    so configuring the slice for the PWM pin is all that is needed.
  for (const Chan& c : g_ch) {
    pwm_set_enabled(c.slice, true);
  }
}

void motors::stby(bool enabled) {
  g_stby = enabled;
  gpio_put(kPinStby, enabled ? 1 : 0);
}

bool motors::stby_enabled() { return g_stby; }

void motors::set(uint8_t motor, int16_t duty) {
  if (motor >= proto::MOTOR_COUNT) return;

  if (duty > proto::DUTY_MAX) duty = proto::DUTY_MAX;
  if (duty < -proto::DUTY_MAX) duty = -proto::DUTY_MAX;

  Chan& c = g_ch[motor];

  const State next = (duty > 0) ? ST_FORWARD : ((duty < 0) ? ST_REVERSE : ST_COAST);

  // Zero means COAST, and coasting on a TB6612 needs the PWM line HIGH with
  // both IN pins low (see the mode table, section 3.2). Driving level 0 here
  // would give short brake instead -- a common and silent bug.
  uint16_t level;
  if (next == ST_COAST) {
    level = kLevelFull;
  } else {
    const uint16_t magnitude =
        static_cast<uint16_t>(duty < 0 ? -duty : duty);
    level = duty_to_level(magnitude);
  }

  apply_state(c, next, level);
}

void motors::set_all(const int16_t duty[proto::MOTOR_COUNT]) {
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
    set(i, duty[i]);
  }
}

void motors::brake_all() {
  // Short brake: IN1=IN2=H. The PWM level is irrelevant in this state, but 0
  // keeps the two halves of the bridge from ever being driven against each
  // other during the transition.
  for (Chan& c : g_ch) {
    apply_state(c, ST_BRAKE, 0);
  }
}

void motors::coast_all() {
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
    set(i, 0);
  }
}

void motors::disable_all() {
  // Order matters on the way down too: kill STBY first so the outputs go
  // high-Z, then tidy the logic lines underneath it.
  stby(false);
  for (Chan& c : g_ch) {
    set_level(c, 0);
    gpio_put(c.pin_in1, 0);
    gpio_put(c.pin_in2, 0);
    c.state = ST_COAST;
  }
}

uint16_t motors::level_of(uint8_t motor) {
  if (motor >= proto::MOTOR_COUNT) return 0;
  return g_ch[motor].level;
}

const char* motors::state_name(uint8_t motor) {
  if (motor >= proto::MOTOR_COUNT) return "?";
  switch (g_ch[motor].state) {
    case ST_FORWARD:
      return "fwd";
    case ST_REVERSE:
      return "rev";
    case ST_BRAKE:
      return "brake";
    case ST_COAST:
    default:
      return "coast";
  }
}
