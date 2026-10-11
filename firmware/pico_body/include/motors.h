// motors.h — PWM + direction layer for the two ROB-14450 driver boards.
// ---------------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 3 (pin map), 3.1 (PWM maths),
//            3.2 (TB6612 mode table), 10.2 (firmware notes).
//
// Four signed duties in, twelve logic lines and four PWM channels out.
// This layer owns every electrical subtlety; callers only deal in permille.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#include "protocol.h"

namespace motors {

// Bring the hardware up in a safe order:
//   1. STBY driven LOW (motors high-Z) before anything else can twitch
//   2. direction pins low, PWM levels zero
//   3. only then enable the four PWM slices
// Leaves STBY LOW. Call stby(true) when you actually want the drivers live.
void init();

// The shared STBY line into both ROB-14450s (JP2-4). Low = standby = high-Z.
void stby(bool enabled);
bool stby_enabled();

// One motor, permille -1000..+1000.
//   > 0  forward   (IN1=H, IN2=L, PWM = magnitude)
//   < 0  reverse   (IN1=L, IN2=H, PWM = magnitude)
//   == 0 COAST     (IN1=L, IN2=L, PWM = FULL)  <- not PWM 0, see .cpp
// Ordering is handled internally: on a state change the PWM is zeroed before
// the direction pins move, so there is never a reverse pulse.
void set(uint8_t motor, int16_t duty);

void set_all(const int16_t duty[proto::MOTOR_COUNT]);

// Short brake: IN1=IN2=H on all four. Stops fast, unlike coast.
void brake_all();

// High-Z all four. Same as set_all() with zeros.
void coast_all();

// STBY low, levels zeroed, direction pins low. The dead-man's landing state.
void disable_all();

// Diagnostics for the self-test / bring-up: the level currently programmed into
// a channel's PWM register, and its direction state.
uint16_t level_of(uint8_t motor);
const char* state_name(uint8_t motor);

}  // namespace motors
