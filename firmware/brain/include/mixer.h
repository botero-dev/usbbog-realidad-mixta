// mixer.h — the driving mixer: three human-intent values in, four signed
// duties out.
// ---------------------------------------------------------------------------
// PORTED from firmware/car_control/main.cpp, whose `run_task()` did exactly this
// arithmetic inline. Plan section 10.3 moves it here, into the brain, because
// the mixing is a property of the CAR and not of whichever board happens to be
// holding the motor pins.
//
// Kept pure C++ with no Arduino dependency, for the same reason protocol.h is:
// it can be compiled and unit-tested on the host. See tools/test_link.cpp.
//
// The three inputs, in permille:
//
//   throttle    +  forward,        -  reverse
//   front_rear  +  front wheels,   -  rear wheels     (a twist along the chassis)
//   left_right  +  left wheels,    -  right wheels    (the turn)
//
// Motor order is fl, rl, rr, fr -- proto::MotorId, and the frame's order. The
// SIGNS are only meaningful once the channel-to-wheel mapping is confirmed on
// hardware (bring-up steps 4/5 in section 11): until then a swapped pair or a
// reversed motor makes every sign here a coin flip. The arithmetic below is the
// one thing that is certain, because it is the same arithmetic that already ran
// on the original car.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#include "protocol.h"

namespace mixer {

struct Input {
  int16_t throttle;
  int16_t front_rear;
  int16_t left_right;
};

// Clamp on a WIDER type than the inputs. Each input is an int16_t, so their SUM
// can leave the int16_t range even when every input fits in it -- and the sum has
// to exist before it can be clamped. (With the nominal +-1000 permille range the
// sum is at most +-3000 and would fit; the wider type costs nothing on a 32-bit
// MCU and removes the assumption, which is worth more than the register.)
inline int16_t clamp_duty(int32_t v) {
  if (v > proto::DUTY_MAX) return proto::DUTY_MAX;
  if (v < -proto::DUTY_MAX) return -proto::DUTY_MAX;
  return static_cast<int16_t>(v);
}

// The four lines below are car_control's, with ONE sign changed and the clamp added.
//
// *** THE left_right SIGN IS FLIPPED ON PURPOSE. ***
// +left_right now puts LESS duty on the wheels this file calls left, which is what makes
// the car turn LEFT -- brake the left side, drive the right. The page maps its LEFT
// arrow to +turn, so "+ = turn left" is the convention this file implements, and it is
// the opposite of car_control's, whose own comment said "+turn = left" while producing
// duties that rotated the car the other way.
//
// It HAD to flip, because of to_wire() below: the two are ONE fix, and either one alone
// inverts the car's steering. Before them the two errors cancelled -- the arrows happened
// to be right while the wheel names were wrong.
inline void mix(const Input& in, int16_t duty[proto::MOTOR_COUNT]) {
  const int32_t th = in.throttle;
  const int32_t fr = in.front_rear;
  const int32_t lr = in.left_right;

  duty[proto::MOT_FL] = clamp_duty(th + fr - lr);  // front left
  duty[proto::MOT_RL] = clamp_duty(th - fr - lr);  // rear left
  duty[proto::MOT_RR] = clamp_duty(th - fr + lr);  // rear right
  duty[proto::MOT_FR] = clamp_duty(th + fr + lr);  // front right
}

// *** THE LEFT AND RIGHT DRIVER BOARDS ARE THE OTHER WAY ROUND ON THIS CAR. ***
//
// MEASURED 2026-10-12 with the page's wheel-test buttons, which is what they were built
// for: a duty in the frame slot protocol.h calls MOT_FL moves the physical FRONT-RIGHT
// wheel, and MOT_RL moves the rear-right. Both ends of the L/R pair -- a straight
// crossing, not a front/rear mistake.
//
// So the frame's slot names are CHANNEL names, not wheel names, and this is the single
// place that translates one into the other. After to_wire(), a slot carries the duty for
// the wheel that slot physically drives, which is what makes the named interfaces honest
// -- without it the wheel-test buttons go on calling the right wheel "fl".
//
// Deliberately NOT in protocol.h: that file is the WIRE contract and the Pico compiles it
// too, whereas "our left board sits on the right channel" is a fact about THIS CAR's
// wiring, not about the protocol. In here rather than in the frame builder so that the
// host test can cover it -- see test_link.cpp.
inline void to_wire(int16_t duty[proto::MOTOR_COUNT]) {
  const int16_t fl = duty[proto::MOT_FL];
  const int16_t rl = duty[proto::MOT_RL];
  duty[proto::MOT_FL] = duty[proto::MOT_FR];
  duty[proto::MOT_RL] = duty[proto::MOT_RR];
  duty[proto::MOT_FR] = fl;
  duty[proto::MOT_RR] = rl;
}

}  // namespace mixer
