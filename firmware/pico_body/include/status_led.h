// status_led.h — the on-board LED, used as a "powered and running" indication.
// ---------------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 3 (the Pico pin map) and 10.2.
//
// Why this exists: the Pico has no power LED. The only LED on a plain Pico is
// the one behind GP25, and that is a *user* LED -- it sits dark from power-up
// until firmware drives it. So a board that is powered and happy looks exactly
// like a board with no power at all, which is a bad thing to stare at while
// debugging.
//
// What this gives you, honestly stated:
//
//   lit   = the RP2040 booted and reached main(). That covers "the board has
//           power" in every case except a blank or corrupt flash.
//   dark  = no power, OR powered with no valid firmware, OR the pin is not
//           there at all (see the Pico W note below).
//
// It is therefore a FIRMWARE-ALIVE light, not a power light: with a blank or
// corrupt flash it stays dark on a perfectly healthy board. That distinction is
// why the plan records the hardware alternative -- an LED and a resistor across
// 3V3, lit by the regulator itself and needing no firmware -- and then declines
// it (open item 8): the on-board light was judged enough for this build.
// ---------------------------------------------------------------------------
#pragma once

#include <pico/stdlib.h>

// A plain Pico's board header names the pin for us, and wires the LED through a
// resistor to GND -- so high is lit, with no inversion macro defined.
//
// A Pico W deliberately does NOT define this macro; its own board header says
// "no PICO_DEFAULT_LED_PIN - LED is on Wireless chip", because there the LED
// hangs off the CYW43 and is reached through the wireless driver.
//
// So the guard below is not decoration. On a W, GP25 is not a spare LED at all:
// the SDK's own pico_w.h defines `CYW43_DEFAULT_PIN_WL_CS 25u`, so it is the
// wireless chip's SPI **chip select**. Driving it to announce "I am alive" would
// be poking the radio. Building for `board = pico` (the platformio.ini default)
// selects the LED case; `board = pico_w` compiles this file to nothing.
#if defined(PICO_DEFAULT_LED_PIN)
#define STATUS_LED_PRESENT 1
#else
#define STATUS_LED_PRESENT 0
#endif

namespace status_led {

// Call this as the very first statement in main(), before stdio_init_all() and
// before any sleep: the point of the light is that it does not wait for a USB
// host to attach.
inline void init() {
#if STATUS_LED_PRESENT
  gpio_init(PICO_DEFAULT_LED_PIN);
  gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
  gpio_put(PICO_DEFAULT_LED_PIN, 0);  // start dark, so "lit" always means we set it
#endif
}

inline void set(bool on) {
#if STATUS_LED_PRESENT
  gpio_put(PICO_DEFAULT_LED_PIN, on ? 1 : 0);
#else
  (void)on;  // keeps -Wunused-parameter quiet if this is ever built for a W
#endif
}

inline void on() { set(true); }
inline void off() { set(false); }

}  // namespace status_led
