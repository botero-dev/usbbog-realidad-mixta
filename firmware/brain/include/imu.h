// imu.h — the BMI160 6-axis IMU: sampling, and access to the latest reading.
// ---------------------------------------------------------------------------
// Authority: docs/circuit-plan.md section 5 (wiring, mounting, rates) and
// section 5.2 (poll, do not use interrupts).
//
// THE SHAPE OF THIS MODULE, and why it is not just a read function:
//
//   * A TASK samples the sensor at 200 Hz and keeps the most recent sample.
//   * Everything else -- HTTP, the serial log, the future control loop -- reads
//     that latest sample and never touches the bus.
//
// That split matters because the two speeds are different jobs. A human reading
// the web page wants ~15 Hz; the control law will want 200 Hz; and a burst read is
// ~0.6 ms of bus time, so it must not be done from a request handler at a rate the
// network dictates. Polling once, at a fixed rate, in one place, also means the
// numbers on the page and the numbers in the log are the same numbers.
//
// The sampler task is pinned to CORE 1 at priority 5 -- deliberately the slot
// plan section 10.1 reserves for the control task. When the control loop arrives
// it goes in that task, next to the sampling it depends on, and the rest of this
// interface does not change.
// ---------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

struct ImuSample {
  float ax, ay, az;   // acceleration, g
  float gx, gy, gz;   // rotation rate, deg/s
  float pitch, roll;  // derived from the accelerometer, degrees
  float temp_c;       // die temperature, updated at 1 Hz, not per sample
  uint32_t seq;       // increments per sample: a stalled sampler is visible
  uint32_t errors;    // I2C read failures since boot
};

// Install the I2C bus, scan it, probe for the BMI160, configure it and start the
// sampler. Returns true if a BMI160 answered.
//
// It NEVER aborts and never blocks the rest of bring-up: a missing or miswired IMU
// must not take the camera and the audio down with it. If it returns false, the
// boot log has already said what was on the bus and what to check.
bool imu_init();

// True if a BMI160 answered at boot. Once false, always false -- this chip does
// not hot-plug, and pretending otherwise would only hide a bad connection.
bool imu_present();

// WHO_AM_I as read at boot (0xD1 for a BMI160/BMX160, 0 if nothing answered).
// 0xD1 is the BMI160's own ID -- 0x24 is a BMI270, which this driver cannot set up.
uint8_t imu_who_am_i();

// Copy the most recent sample. Safe from any task; never touches the bus.
void imu_latest(ImuSample *out);

// The configured sample rate, for the log and the JSON.
uint32_t imu_rate_hz();

// Scan the bus and write the addresses that ACK into `out` as "0x69 0x68 ".
// Returns the number found. Used by the boot log: a wrong address or an absent
// device is the single most likely IMU problem, and it is worth one line to see.
int imu_scan(char *out, size_t out_len);
