// main.cpp — pico_body entry point.
// ---------------------------------------------------------------------------
// Two modes, selected by PICO_MODE_SELFTEST in platformio.ini:
//
//   SELFTEST (default) : no link needed. Drives each motor through a fixed
//                        sweep and prints the state of all twelve logic lines
//                        and four PWM levels, so the Pico can be verified on a
//                        bench supply on its own. This is bring-up step 4 in
//                        docs/circuit-plan.md section 11.
//
//   LINK               : the real job -- parse frames from the S3 and drive the
//                        motors, with the 100 ms dead-man. Stage 2.
//
// Serial is USB CDC on this core, so it never touches UART0 on GP16/GP17,
// which belongs to the link. Debug output and the link cannot collide.
//
// The on-board LED is lit by the first instruction of main(), before stdio and
// before any delay, so "the board has power and the firmware is running" is
// visible with nothing plugged in. See include/status_led.h -- and note there
// that it is a firmware-alive light, not a power light.
// ---------------------------------------------------------------------------

#include <pico/stdlib.h>

#include <hardware/clocks.h>
#include <hardware/gpio.h>

#include "log.h"
#include "motors.h"
#include "protocol.h"
#include "status_led.h"

#ifndef PICO_MODE_SELFTEST
#define PICO_MODE_SELFTEST 0
#endif

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
#if PICO_MODE_SELFTEST

namespace {

constexpr uint16_t kLevelFull = 6250;

const char* const kMotorName[proto::MOTOR_COUNT] = {"FL", "RL", "RR", "FR"};

// 0 -> forward to full -> back -> through zero -> reverse to full -> back.
// Passing through zero exercises the state-change path in both directions,
// which is where the reverse-pulse bug would live.
const int16_t kSweep[] = {0,    250,  500,  750,  1000, 750, 500,  250, 0,
                          -250, -500, -750, -1000, -750, -500, -250, 0};

void print_row(const char* label) {
  logf("%-16s", label);
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
    logf("  %-5s L%4u", motors::state_name(i), motors::level_of(i));
  }
  logf("   STBY=%u\n", motors::stby_enabled() ? 1 : 0);
}

void print_header() {
  logf("%-16s", "state");
  for (uint8_t i = 0; i < proto::MOTOR_COUNT; ++i) {
    logf("  %-5s %-5s", kMotorName[i], "level");
  }
  logf("\n");
}

void sweep_motor(uint8_t m) {
  logf("\n-- %s sweep (driver #%u channel %c) --\n", kMotorName[m],
       (m / 2) + 1, (m % 2 == 0) ? 'A' : 'B');
  for (const int16_t duty : kSweep) {
    motors::set(m, duty);
    const uint16_t level = motors::level_of(m);
    // Percentage computed with INTEGER arithmetic on purpose. The Pico SDK's newlib
    // is built without float support in printf, so a "%.1f" silently prints as
    // nothing at all -- you get "(%)" and no number. Enabling float printf costs
    // flash and stack for no benefit here. See the note in README.md.
    const unsigned tenths = (static_cast<unsigned>(level) * 1000u) / kLevelFull;
    logf("  duty %+5d  ->  %-5s level %4u (%u.%u%%)\n", duty, motors::state_name(m),
         level, tenths / 10u, tenths % 10u);
    sleep_ms(350);
  }
}

}  // namespace

int main() {
  // First instruction in the program, deliberately before stdio_init_all() and
  // before the sleep below: the "powered and running" indication must not depend
  // on a USB host being attached, because on a car it never is.
  status_led::init();
  status_led::on();

  stdio_init_all();

  // Give the USB host a moment to attach, but never block on it: a Pico that
  // refuses to run because no terminal is open is useless on a car.
  sleep_ms(1500);

  logf("\n");
  logf("pico_body  --  SELF TEST (no link, no S3 required)\n");
  logf("=================================================\n");

  logf("system clock   : %u Hz\n", (unsigned)clock_get_hz(clk_sys));
  logf("PWM            : clkdiv 1.0, wrap 6249 -> %u Hz, %u steps\n",
       (unsigned)(clock_get_hz(clk_sys) / (kLevelFull)), (unsigned)kLevelFull);
  logf("status LED     : %s\n",
       STATUS_LED_PRESENT ? "GP25 lit solid (plain Pico)"
                          : "ABSENT (Pico W: the LED belongs to the radio)");

  // Before init(): confirm the pad really does come up pulling STBY low, which
  // is what makes the fail-safe chain work from the instant power is applied.
  logf("GP28 before init: %u  (expect 0 -- pad pull-down)\n",
       (unsigned)gpio_get(28));

  motors::init();

  logf("GP28 after init : %u  (expect 0 -- driven low)\n",
       (unsigned)gpio_get(28));
  print_header();
  print_row("after init");

  logf("\n");
  logf("STBY stays LOW: the drivers are high-Z and nothing can move.\n");
  logf("Raising STBY now -- wheels should be OFF THE GROUND.\n");
  motors::stby(true);
  sleep_ms(1000);
  print_row("stby raised");

  for (uint8_t m = 0; m < proto::MOTOR_COUNT; ++m) {
    sweep_motor(m);
  }

  logf("\n-- all four together, slow ramp --\n");
  motors::stby(true);
  for (int16_t duty = 0; duty <= 1000; duty += 100) {
    int16_t all[proto::MOTOR_COUNT] = {duty, duty, duty, duty};
    motors::set_all(all);
    print_row("ramp up");
    sleep_ms(300);
  }
  for (int16_t duty = 1000; duty >= -1000; duty -= 200) {
    int16_t all[proto::MOTOR_COUNT] = {duty, duty, duty, duty};
    motors::set_all(all);
    print_row("ramp down");
    sleep_ms(300);
  }

  logf("\n-- brake, then coast, then the dead-man landing state --\n");
  motors::brake_all();
  print_row("brake_all");
  sleep_ms(1500);

  motors::coast_all();
  print_row("coast_all");
  sleep_ms(1500);

  motors::disable_all();
  print_row("disable_all");
  logf("STBY low again. Cycle complete; repeating in 3 s.\n");
  sleep_ms(3000);

  // The SDK has no runtime to return to, so the loop is explicit.
  while (true) {
    logf("--- restarting sweep ---\n");
    motors::stby(true);
    for (uint8_t m = 0; m < proto::MOTOR_COUNT; ++m) {
      sweep_motor(m);
    }
    motors::disable_all();
    print_row("disable_all");
    sleep_ms(3000);
  }
}

// ---------------------------------------------------------------------------
// Link mode (stage 2)
// ---------------------------------------------------------------------------
#else

int main() {
  // Same as the self-test: light the LED first, so a successful boot is visible
  // even when nothing is connected to USB.
  status_led::init();
  status_led::on();

  stdio_init_all();
  sleep_ms(1500);
  motors::init();
  logf("pico_body: link mode not implemented yet (stage 2).\n");
  logf("status LED     : %s\n",
       STATUS_LED_PRESENT ? "GP25 lit solid (plain Pico)"
                          : "ABSENT (Pico W: the LED belongs to the radio)");
  logf("Set -DPICO_MODE_SELFTEST=1 in platformio.ini to run the self test.\n");
  while (true) {
    sleep_ms(1000);
  }
}

#endif
